// ── Host audio output (XAudio2 2.9, loaded dynamically) ──────────────────────────────────────
// The game's XAudio render-driver client hands one frame per call to XAudioSubmitRenderDriverFrame:
// 256 samples x 6 channels (5.1), PLANAR (channel-major) big-endian float at 48 kHz (Xenia
// AudioDriver kFrame*Default). Each client gets one 6-channel float source voice; frames are
// byte-swapped + interleaved into a per-client ring and queued. XAudio2 downmixes 5.1 to the host
// speaker layout. OnBufferEnd reports each played frame back to kernel_stubs so the game's mixer is
// paced by the real device clock (Xenia does the same with a semaphore).
// LSWTCS_AUDIO_OUT=0 disables output (frames are then paced by the old 5.33 ms timer).
#include <windows.h>
#include <xaudio2.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>

extern "C" void dbg_ram(const char* fmt, ...);

namespace {
constexpr int kMaxClients = 8;
constexpr int kChannels = 6, kSamples = 256, kRate = 48000;
constexpr int kRing = 32;                       // > max queued frames (8) with margin
constexpr uint32_t kFrameFloats = kChannels * kSamples;

typedef void (*EndFn)(int idx);

struct VoiceCb : public IXAudio2VoiceCallback {
    int idx = -1; EndFn on_end = nullptr;
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { if (on_end) on_end(idx); }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT hr) override { dbg_ram("[AUDIO] voice %d error 0x%08X\n", idx, (unsigned)hr); }
};

struct Client {
    IXAudio2SourceVoice* voice = nullptr;
    VoiceCb cb;
    float ring[kRing][kFrameFloats];
    uint32_t pos = 0;
};

std::mutex g_mx;
IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
int g_state = -1;                                // -1 untried, 0 failed/disabled, 1 ready
Client* g_clients[kMaxClients] = {};

bool engine_up() {
    if (g_state >= 0) return g_state == 1;
    g_state = 0;
    const char* e = getenv("LSWTCS_AUDIO_OUT");
    if (e && e[0] == '0') { dbg_ram("[AUDIO] host output disabled (LSWTCS_AUDIO_OUT=0)\n"); return false; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE m = LoadLibraryA("XAudio2_9.dll");
    if (!m) m = LoadLibraryA("XAudio2_9redist.dll");
    if (!m) { dbg_ram("[AUDIO] XAudio2_9.dll not found -> no host output\n"); printf("[AUDIO] XAudio2_9.dll not found\n"); return false; }
    typedef HRESULT (WINAPI *PFN)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
    PFN create = (PFN)GetProcAddress(m, "XAudio2Create");
    HRESULT hr = create ? create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR) : E_FAIL;
    if (FAILED(hr) || !g_xa) { printf("[AUDIO] XAudio2Create failed 0x%08X\n", (unsigned)hr); return false; }
    hr = g_xa->CreateMasteringVoice(&g_master);
    if (FAILED(hr)) { printf("[AUDIO] CreateMasteringVoice failed 0x%08X\n", (unsigned)hr); g_xa->Release(); g_xa = nullptr; return false; }
    g_xa->StartEngine();
    g_state = 1;
    printf("[AUDIO] XAudio2 host output ready (5.1 float 48 kHz -> device mix)\n"); fflush(stdout);
    dbg_ram("[AUDIO] XAudio2 host output ready\n");
    return true;
}
}  // namespace

// Returns true if a host voice was created; on_end(idx) is then called once per played frame.
bool host_audio_open(int idx, EndFn on_end) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (idx < 0 || idx >= kMaxClients || !engine_up()) return false;
    if (g_clients[idx]) return true;
    Client* c = new Client();
    c->cb.idx = idx; c->cb.on_end = on_end;
    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = kChannels;
    fmt.Format.nSamplesPerSec = kRate;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = kChannels * 4;
    fmt.Format.nAvgBytesPerSec = kRate * fmt.Format.nBlockAlign;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                        SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    static const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    fmt.SubFormat = kFloat;   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    HRESULT hr = g_xa->CreateSourceVoice(&c->voice, &fmt.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &c->cb);
    if (FAILED(hr)) { printf("[AUDIO] CreateSourceVoice failed 0x%08X\n", (unsigned)hr); delete c; return false; }
    c->voice->Start(0);
    g_clients[idx] = c;
    dbg_ram("[AUDIO] client %d voice created\n", idx);
    return true;
}

// frame_be = guest pointer (host-mapped) to 256x6 planar big-endian floats.
void host_audio_submit(int idx, const uint8_t* frame_be) {
    Client* c = (idx >= 0 && idx < kMaxClients) ? g_clients[idx] : nullptr;
    if (!c || !c->voice) return;
    float* out = c->ring[c->pos];
    c->pos = (c->pos + 1) % kRing;
    const uint32_t* in = reinterpret_cast<const uint32_t*>(frame_be);
    uint32_t* o = reinterpret_cast<uint32_t*>(out);
    for (int s = 0; s < kSamples; ++s)
        for (int ch = 0; ch < kChannels; ++ch)
            o[s * kChannels + ch] = __builtin_bswap32(in[ch * kSamples + s]);
    {   // level meter: peak + count of non-silent frames, logged every 375 frames (~2 s)
        static float peak = 0; static uint32_t frames = 0, loud = 0, logs = 0;
        float fp = 0; for (uint32_t i = 0; i < kFrameFloats; ++i) { float a = out[i] < 0 ? -out[i] : out[i]; if (a > fp) fp = a; }
        if (fp > peak) peak = fp; if (fp > 1e-4f) ++loud;
        if (++frames % 375 == 0) {
            if (logs < 40 || (logs % 15) == 0) { dbg_ram("[AUDIO] level: peak=%.4f non-silent=%u/375\n", peak, loud);
                                                 printf("[AUDIO] level: peak=%.4f non-silent=%u/375\n", peak, loud); fflush(stdout); }
            ++logs; peak = 0; loud = 0;
        }
    }
    XAUDIO2_BUFFER b{};
    b.AudioBytes = kFrameFloats * 4;
    b.pAudioData = reinterpret_cast<const BYTE*>(out);
    b.PlayLength = kSamples;
    HRESULT hr = c->voice->SubmitSourceBuffer(&b);
    if (FAILED(hr)) { static int n = 0; if (n++ < 8) dbg_ram("[AUDIO] SubmitSourceBuffer %d failed 0x%08X\n", idx, (unsigned)hr); if (c->cb.on_end) c->cb.on_end(idx); }
}

void host_audio_close(int idx) {
    std::lock_guard<std::mutex> lk(g_mx);
    Client* c = (idx >= 0 && idx < kMaxClients) ? g_clients[idx] : nullptr;
    if (!c) return;
    g_clients[idx] = nullptr;
    if (c->voice) { c->voice->Stop(0); c->voice->FlushSourceBuffers(); c->voice->DestroyVoice(); }
    delete c;
}
