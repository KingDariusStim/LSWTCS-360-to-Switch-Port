// gpu_d3d12.cpp — Minimal D3D12 framebuffer presenter for LSWTCSRuntime.
//
// Xenia pathway:  VdSwap writes PM4_XE_SWAP into the ring buffer.
// When a proper Xenia command processor is attached it will call IssueSwap(),
// which copies the EDRAM-resolved frontbuffer to a D3D12 texture and presents.
//
// Until then: we directly upload the guest frontbuffer from g_base + phys_addr
// and CopyTextureRegion it onto the swapchain backbuffer, then Present().
// Xbox 360 k_8_8_8_8 + k8in32 stores pixels as [B,G,R,A] bytes in guest memory,
// which maps 1:1 to DXGI_FORMAT_B8G8R8A8_UNORM without any per-pixel swap.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include <direct.h>   // _mkdir (harvest dir)
#include <unordered_map>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "renderdoc_app.h"   // RenderDoc in-app capture API (LSWTCS_RDOC)
extern "C" void lswtcs_gil_blocking(void (*fn)(void*), void* arg);   // kernel_stubs.cpp

// ── RenderDoc in-app capture: programmatic full-GPU-state capture (.rdc). ──
// Loads renderdoc.dll (must be BEFORE D3D12 device creation so RenderDoc hooks it),
// then LSWTCS_RDOC=N captures frame N → build/rdoc/cap_*.rdc (inspect via renderdoccmd
// thumb/convert or the RenderDoc UI). Inert if renderdoc.dll isn't present.
static RENDERDOC_API_1_1_2* g_rdoc = nullptr;
static void rdoc_init() {
    HMODULE m = GetModuleHandleA("renderdoc.dll");          // present if launched under RenderDoc
    if (!m) m = LoadLibraryA("renderdoc.dll");              // else load from PATH / install dir
    if (!m) m = LoadLibraryA("C:\\Program Files\\RenderDoc\\renderdoc.dll");
    if (!m) { printf("[RDOC] renderdoc.dll not found — capture disabled\n"); return; }
    pRENDERDOC_GetAPI GetAPI = (pRENDERDOC_GetAPI)GetProcAddress(m, "RENDERDOC_GetAPI");
    if (GetAPI && GetAPI(eRENDERDOC_API_Version_1_1_2, (void**)&g_rdoc) == 1) {
        _mkdir("rdoc");
        g_rdoc->SetCaptureFilePathTemplate("rdoc/cap");
        printf("[RDOC] RenderDoc in-app API ready (LSWTCS_RDOC=<frame> to capture)\n");
    }
    fflush(stdout);
}

// ── Minimal zero-dependency PNG writer (BGRA8 in → RGBA8 PNG out). Used by the
// in-process frame capture (LSWTCS_SHOTS) so I can SEE the real D3D12 output —
// PrintWindow returns black on a flip-model swapchain, so OS-side grabs fail. ──
static uint32_t png_crc(const uint8_t* p, size_t n, uint32_t crc) {
    static uint32_t T[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1); T[i] = c; } init = true; }
    for (size_t i = 0; i < n; i++) crc = T[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static void png_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t len) {
    uint8_t lb[4] = {(uint8_t)(len>>24),(uint8_t)(len>>16),(uint8_t)(len>>8),(uint8_t)len};
    fwrite(lb, 1, 4, f);
    uint32_t crc = png_crc((const uint8_t*)type, 4, 0xFFFFFFFFu);
    fwrite(type, 1, 4, f);
    if (len) { crc = png_crc(data, len, crc); fwrite(data, 1, len, f); }
    crc ^= 0xFFFFFFFFu;
    uint8_t cb[4] = {(uint8_t)(crc>>24),(uint8_t)(crc>>16),(uint8_t)(crc>>8),(uint8_t)crc};
    fwrite(cb, 1, 4, f);
}
static void lswtcs_write_png(const char* path, const uint8_t* bgra, int w, int h, int pitch) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    const uint8_t sig[8] = {137,80,78,71,13,10,26,10}; fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(w>>24),(uint8_t)(w>>16),(uint8_t)(w>>8),(uint8_t)w,
                        (uint8_t)(h>>24),(uint8_t)(h>>16),(uint8_t)(h>>8),(uint8_t)h, 8,6,0,0,0};
    png_chunk(f, "IHDR", ihdr, 13);
    size_t raw_len = (size_t)h * (1 + (size_t)w * 4);
    std::vector<uint8_t> raw(raw_len); size_t o = 0;
    for (int y = 0; y < h; y++) { raw[o++] = 0; const uint8_t* row = bgra + (size_t)y * pitch;
        for (int x = 0; x < w; x++) { raw[o++]=row[x*4+2]; raw[o++]=row[x*4+1]; raw[o++]=row[x*4+0]; raw[o++]=row[x*4+3]; } }
    std::vector<uint8_t> z; z.push_back(0x78); z.push_back(0x01);   // zlib stored
    size_t pos = 0;
    while (pos < raw_len) { size_t blk = (raw_len - pos < 65535u) ? (raw_len - pos) : 65535u;
        bool last = (pos + blk >= raw_len);
        z.push_back(last ? 1 : 0); z.push_back(blk & 0xFF); z.push_back((blk >> 8) & 0xFF);
        uint16_t nlen = ~(uint16_t)blk; z.push_back(nlen & 0xFF); z.push_back((nlen >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + blk); pos += blk; }
    uint32_t a = 1, b = 0; for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    uint32_t adler = (b << 16) | a;
    z.push_back(adler>>24); z.push_back(adler>>16); z.push_back(adler>>8); z.push_back(adler);
    png_chunk(f, "IDAT", z.data(), (uint32_t)z.size());
    png_chunk(f, "IEND", nullptr, 0);
    fclose(f);
}

template<typename T>
static inline void safe_rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ────────────��────────────────────────────────���────────────────────────────────
static const UINT kFrameCount   = 2;
static const UINT kSwapW        = 1280;
static const UINT kSwapH        = 720;
static const UINT64 kUploadSize = 8 * 1024 * 1024;  // 8 MB

static ID3D12Device*             g_device    = nullptr;
static ID3D12CommandQueue*       g_queue     = nullptr;
static IDXGISwapChain3*          g_swapchain = nullptr;
static ID3D12DescriptorHeap*     g_rtv_heap  = nullptr;
static UINT                      g_rtv_sz    = 0;
static ID3D12Resource*           g_rt[kFrameCount]   = {};
static ID3D12CommandAllocator*   g_alloc[kFrameCount] = {};
static ID3D12GraphicsCommandList* g_cmd      = nullptr;
static ID3D12Fence*              g_fence     = nullptr;
static HANDLE                    g_fence_ev  = nullptr;
static UINT64                    g_fence_for[kFrameCount] = {};
static UINT64                    g_fence_val = 0;
static UINT                      g_frame_idx = 0;

static ID3D12Resource*           g_upload    = nullptr;
static void*                     g_upload_ptr= nullptr;

static HWND g_hwnd  = nullptr;
static bool g_ready = false;

// ── Graphics-pipeline scaffolding (the foundation for Xenos→D3D12 translation) ──
// Milestone 1: a hardcoded test triangle through a real PSO, proving the pipeline
// (root sig + HLSL→DXBC + input layout + vertex buffer + draw-to-RTV) works end to
// end. Real translated shaders/geometry plug into this same machinery later.
static ID3D12RootSignature* g_test_rootsig = nullptr;
static ID3D12PipelineState* g_test_pso     = nullptr;
static ID3D12Resource*      g_test_vb      = nullptr;
static D3D12_VERTEX_BUFFER_VIEW g_test_vbv = {};
static bool                 g_gputest      = false;
static ID3D12Resource*      g_readback     = nullptr;   // one-shot pixel verification
static bool                 g_readback_done= false;
static const UINT           kReadPitch     = ((kSwapW * 4 + 255u) & ~255u);

// ── Geometry executor (LSWTCS_GEOM) — passthrough pos+color PSO + dynamic VB ────
// Renders the game's mode-4 UI rect-list quads (collected as clip-space triangles
// in kernel_stubs g_geom_verts) into the swapchain backbuffer.
extern float    g_geom_present_verts[];        // {x,y,z, r,g,b,a} per vertex, clip space
extern uint32_t g_geom_vert_ready;     // verts ready to draw this frame
struct GeomBatch { uint32_t first; uint32_t dctl; };
extern GeomBatch g_geom_present_batches[];
extern uint32_t  g_geom_present_batch_count;
extern uint32_t  g_geom_present_depth_clear;
static ID3D12RootSignature* g_geom_rootsig = nullptr;
static ID3D12PipelineState* g_geom_pso     = nullptr;
static ID3D12Resource*      g_geom_vb      = nullptr;   // upload heap, CPU-written
static void*                g_geom_vb_ptr  = nullptr;
static UINT                 g_geom_vb_cap  = 0;         // bytes
static bool                 g_geom_enabled = false;

static ID3DBlob* compile_hlsl(const char* src, const char* entry, const char* target) {
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        printf("[GPU] D3DCompile %s failed 0x%08X: %s\n", entry, (unsigned)hr,
               err ? (const char*)err->GetBufferPointer() : "?");
        fflush(stdout);
        safe_rel(err);
        return nullptr;
    }
    safe_rel(err);
    return code;
}

// ──────────────────────────────────────────────────────────────────────────────
// PATH B: compile the game's HLSL directly to D3D12, bypassing its broken compiler
// and the Xenos microcode entirely. Proven viable (build/test_shc.cpp).
// ──────────────────────────────────────────────────────────────────────────────
static const char* pathb_sem(const char* nm, int nl, int* tc) {
    static char buf[24];
    auto eq  = [&](const char* k){ int kl=(int)strlen(k); return kl==nl && !memcmp(nm,k,nl); };
    auto pre = [&](const char* k){ int kl=(int)strlen(k); return nl>kl && !memcmp(nm,k,kl); };
    if (eq("position")) return "POSITION";
    if (eq("normal"))   return "NORMAL";
    if (eq("tangent"))  return "TANGENT";
    if (eq("binormal")) return "BINORMAL";
    if (pre("colorSet"))     { snprintf(buf,sizeof(buf),"COLOR%c", nm[8]); return buf; }
    if (pre("uvSet"))        { snprintf(buf,sizeof(buf),"TEXCOORD%c", nm[5]); return buf; }
    if (pre("blendWeight"))  return "BLENDWEIGHT";
    if (pre("blendIndices")) return "BLENDINDICES";
    snprintf(buf,sizeof(buf),"TEXCOORD%d", 8 + ((*tc)++)); return buf;
}
// Raw game HLSL → SM5-compilable HLSL: fill VertexInput semantics, drop empty
// annotations, strip 'shared' and ': register(cN)'.
static std::string pathb_preprocess(const char* src, uint32_t len) {
    std::string out; out.reserve(len + 1024);
    int inVI = 0, tc = 0;
    for (uint32_t i = 0; i < len && src[i]; ) {
        if (!inVI && i + 18 <= len && !memcmp(src + i, "struct VertexInput", 18)) inVI = 1;
        else if (inVI && src[i] == '}') inVI = 0;
        if (src[i] == ':') {
            uint32_t j = i + 1; while (j < len && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < len && src[j] == ';') {
                if (inVI) {
                    int nsx = (int)i;
                    while (nsx > 0) { char c = src[nsx-1]; if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_') nsx--; else break; }
                    out += ": "; out += pathb_sem(src + nsx, (int)i - nsx, &tc);
                }
                i = j; continue;
            } else if (j < len && src[j] == ':') { i = j; continue; }
        }
        out += src[i++];
    }
    size_t p;
    while ((p = out.find("shared ")) != std::string::npos) out.erase(p, 7);
    while ((p = out.find(": register(c")) != std::string::npos) { size_t e = out.find(')', p); if (e == std::string::npos) break; out.erase(p, e - p + 1); }
    return out;
}
static ID3DBlob* pathb_compile_one(const std::string& s, const char* entry, const char* target) {
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(s.data(), s.size(), "game.hlsl", nullptr, nullptr, entry, target,
                            D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        static int e = 0;
        if (e++ < 4) printf("[PATHB] %s/%s FAIL: %.300s\n", entry, target, err ? (const char*)err->GetBufferPointer() : "?");
    }
    safe_rel(err);
    return code;
}
// ──────────────────────────────────────────────────────────────────────────────
// HARVEST: hash ↔ HLSL capture (LSWTCS_HARVEST=1). Pairs the game's SHAD-cache
// hash (the filename it just tried to open, e.g. 0XFDB1F172 — set by NtCreateFile)
// with the HLSL source the game generates for that shader at compile time. This
// builds the foundational table for the shader harvester.
//   harvest/<gamehash>_<seq>.hlsl     raw game HLSL (ground truth)
//   harvest/<gamehash>_<seq>.pp.hlsl  preprocessed (SM5-compilable)
//   harvest/<gamehash>_<seq>.vs.dxbc  / .ps.dxbc   compiled blobs (verification)
//   harvest/index.csv                 seq,gamehash,fnv1a,len,vs_ok,vs_sz,ps_ok,ps_sz
// ──────────────────────────────────────────────────────────────────────────────
static char     g_pending_shad[24] = {0};   // last SHAD hash the game tried to open
static uint64_t g_pending_seq      = 0;      // bumped each SHAD open, to detect freshness
static uint64_t g_pending_seen     = ~0ull;  // last seq a compile consumed

// ── Path-B DXBC store: keep the compiled vs/ps blobs keyed by shader identity, so the
// draw-time path can bind them to a real D3D12 PSO. Keyed by the FNV-1a of the game's
// HLSL (content-addressed — robust to whether the SHAD hash was paired) and also by the
// game hash string when available. The blobs are retained (AddRef'd by D3DCompile output;
// we own one ref and never release while stored).
struct PathBShader { ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; };
static std::unordered_map<uint32_t, PathBShader>   g_pathb_by_fnv;   // fnv1a(hlsl) -> blobs
static std::unordered_map<std::string, uint32_t>   g_pathb_hash2fnv; // "FDB1F172" -> fnv key
extern "C" int  lswtcs_pathb_count() { return (int)g_pathb_by_fnv.size(); }
// Look up retained DXBC by game hash (e.g. "0XFDB1F172" or "FDB1F172"). Returns 1 + fills
// out-ptrs on hit. For the future draw-time PSO build.
extern "C" int lswtcs_pathb_get(const char* hash, const void** vs, uint32_t* vslen,
                                const void** ps, uint32_t* pslen) {
    if (!hash) return 0;
    std::string h = hash; size_t x = h.find_first_of("Xx"); if (x != std::string::npos) h = h.substr(x + 1);
    auto it = g_pathb_hash2fnv.find(h);
    if (it == g_pathb_hash2fnv.end()) return 0;
    auto jt = g_pathb_by_fnv.find(it->second);
    if (jt == g_pathb_by_fnv.end() || !jt->second.vs || !jt->second.ps) return 0;
    if (vs) *vs = jt->second.vs->GetBufferPointer(); if (vslen) *vslen = (uint32_t)jt->second.vs->GetBufferSize();
    if (ps) *ps = jt->second.ps->GetBufferPointer(); if (pslen) *pslen = (uint32_t)jt->second.ps->GetBufferSize();
    return 1;
}

// Called by NtCreateFile when the game opens SHAD\0X<hash>. Stores the bare hash.
extern "C" void lswtcs_set_pending_shad(const char* hash) {
    if (!hash) return;
    size_t i = 0; for (; hash[i] && i < sizeof(g_pending_shad) - 1; i++) g_pending_shad[i] = hash[i];
    g_pending_shad[i] = 0;
    g_pending_seq++;
}

static uint32_t fnv1a32(const char* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 16777619u; }
    return h;
}
static void harvest_write(const char* path, const void* data, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, n, f);
    fclose(f);
}

static int harvest_enabled() {
    static int e = -1;
    if (e < 0) { const char* v = getenv("LSWTCS_HARVEST"); e = (v && v[0] != '0') ? 1 : 0;
                 if (e) { _mkdir("harvest"); } }
    return e;
}

// Called when the game compiles a shader. Returns 1 if we produced a D3D12 VS+PS.
extern "C" int lswtcs_pathb_compile(const char* hlsl, uint32_t len) {
    if (!hlsl || len < 32 || len > 0x80000u) return 0;
    std::string s = pathb_preprocess(hlsl, len);
    ID3DBlob* vs = pathb_compile_one(s, "vertexProgram",   "vs_5_0");
    ID3DBlob* ps = pathb_compile_one(s, "fragmentProgram", "ps_5_0");
    static int n = 0; n++;
    if (n <= 16)
        printf("[PATHB#%d] %u->%zu B  vs_5_0=%s(%zu)  ps_5_0=%s(%zu)\n", n, len, s.size(),
               vs ? "OK" : "FAIL", vs ? vs->GetBufferSize() : 0,
               ps ? "OK" : "FAIL", ps ? ps->GetBufferSize() : 0);
    fflush(stdout);

    if (harvest_enabled()) {
        static uint64_t hseq = 0;
        uint64_t seq = hseq++;
        uint32_t fnv = fnv1a32(hlsl, len);
        // Pair with the game's SHAD hash only if it's fresh (an open since the last
        // consumed compile); otherwise mark NOPAIR so we still capture by content.
        char ghash[24];
        if (g_pending_seq != g_pending_seen && g_pending_shad[0]) {
            snprintf(ghash, sizeof(ghash), "%s", g_pending_shad);
            g_pending_seen = g_pending_seq;
        } else {
            snprintf(ghash, sizeof(ghash), "NOPAIR");
        }
        char p[256];
        snprintf(p, sizeof(p), "harvest/%s_%llu.hlsl", ghash, (unsigned long long)seq);
        harvest_write(p, hlsl, len);
        snprintf(p, sizeof(p), "harvest/%s_%llu.pp.hlsl", ghash, (unsigned long long)seq);
        harvest_write(p, s.data(), s.size());
        if (vs) { snprintf(p, sizeof(p), "harvest/%s_%llu.vs.dxbc", ghash, (unsigned long long)seq);
                  harvest_write(p, vs->GetBufferPointer(), vs->GetBufferSize()); }
        if (ps) { snprintf(p, sizeof(p), "harvest/%s_%llu.ps.dxbc", ghash, (unsigned long long)seq);
                  harvest_write(p, ps->GetBufferPointer(), ps->GetBufferSize()); }
        FILE* idx = fopen("harvest/index.csv", "a");
        if (idx) {
            if (seq == 0) fprintf(idx, "seq,gamehash,fnv1a,hlsl_len,vs_ok,vs_sz,ps_ok,ps_sz\n");
            fprintf(idx, "%llu,%s,0x%08X,%u,%d,%zu,%d,%zu\n",
                    (unsigned long long)seq, ghash, fnv, len,
                    vs ? 1 : 0, vs ? vs->GetBufferSize() : 0,
                    ps ? 1 : 0, ps ? ps->GetBufferSize() : 0);
            fclose(idx);
        }
    }

    int ok = (vs && ps) ? 1 : 0;
    // Keep the blobs keyed by content hash (+ game hash if paired) for draw-time PSO build.
    if (ok) {
        uint32_t key = fnv1a32(hlsl, len);
        auto& slot = g_pathb_by_fnv[key];
        if (slot.vs != vs) { safe_rel(slot.vs); slot.vs = vs; } else safe_rel(vs);
        if (slot.ps != ps) { safe_rel(slot.ps); slot.ps = ps; } else safe_rel(ps);
        if (g_pending_shad[0]) {
            std::string h = g_pending_shad; size_t x = h.find_first_of("Xx");
            if (x != std::string::npos) h = h.substr(x + 1);
            g_pathb_hash2fnv[h] = key;
        }
        static int kept = 0; if (++kept <= 16)
            printf("[PATHB-KEEP] stored fnv=0x%08X hash=%s (total %d)\n", key,
                   g_pending_shad[0] ? g_pending_shad : "<none>", (int)g_pathb_by_fnv.size());
        fflush(stdout);
    } else { safe_rel(vs); safe_rel(ps); }
    return ok;
}

static void gpu_init_test_pipeline() {
    g_gputest = getenv("LSWTCS_GPUTEST") != nullptr;
    if (!g_gputest) return;

    // ── Root signature: empty (no CBV/SRV), allow input-assembler layout ──
    ID3DBlob* sig = nullptr; ID3DBlob* sigerr = nullptr;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigerr);
    if (FAILED(hr)) { printf("[GPU] SerializeRootSig failed 0x%08X: %s\n", (unsigned)hr,
                             sigerr ? (const char*)sigerr->GetBufferPointer() : "?"); safe_rel(sigerr); return; }
    g_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                  IID_ID3D12RootSignature, (void**)&g_test_rootsig);
    safe_rel(sig); safe_rel(sigerr);

    // ── Test shaders (pass-through position + vertex colour) ──
    static const char* kHLSL =
        "struct VIn  { float3 pos:POSITION; float3 col:COLOR; };\n"
        "struct VOut { float4 pos:SV_POSITION; float3 col:COLOR; };\n"
        "VOut VSMain(VIn i){ VOut o; o.pos=float4(i.pos,1); o.col=i.col; return o; }\n"
        "float4 PSMain(VOut i):SV_TARGET { return float4(i.col,1); }\n";
    ID3DBlob* vs = compile_hlsl(kHLSL, "VSMain", "vs_5_0");
    ID3DBlob* ps = compile_hlsl(kHLSL, "PSMain", "ps_5_0");
    if (!vs || !ps) { safe_rel(vs); safe_rel(ps); return; }

    D3D12_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_test_rootsig;
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout = { il, 2 };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pd.SampleDesc.Count = 1;
    hr = g_device->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState, (void**)&g_test_pso);
    safe_rel(vs); safe_rel(ps);
    if (FAILED(hr)) { printf("[GPU] CreateGraphicsPipelineState failed 0x%08X\n", (unsigned)hr); return; }

    // ── Vertex buffer: a colourful triangle in clip space ──
    struct V { float p[3]; float c[3]; };
    static const V verts[3] = {
        { { 0.0f,  0.6f, 0.0f}, {1,0,0} },
        { { 0.6f, -0.6f, 0.0f}, {0,1,0} },
        { {-0.6f, -0.6f, 0.0f}, {0,0,1} },
    };
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = sizeof(verts);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_ID3D12Resource, (void**)&g_test_vb);
    void* p = nullptr; D3D12_RANGE none = {};
    g_test_vb->Map(0, &none, &p); memcpy(p, verts, sizeof(verts)); g_test_vb->Unmap(0, nullptr);
    g_test_vbv.BufferLocation = g_test_vb->GetGPUVirtualAddress();
    g_test_vbv.SizeInBytes    = sizeof(verts);
    g_test_vbv.StrideInBytes  = sizeof(V);

    // Readback buffer (host-readable) for one-shot pixel verification of the draw.
    D3D12_HEAP_PROPERTIES rhp = {}; rhp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rrd = {};
    rrd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rrd.Width = (UINT64)kReadPitch * kSwapH;
    rrd.Height = 1; rrd.DepthOrArraySize = 1; rrd.MipLevels = 1;
    rrd.Format = DXGI_FORMAT_UNKNOWN; rrd.SampleDesc.Count = 1;
    rrd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g_device->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &rrd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_ID3D12Resource, (void**)&g_readback);

    printf("[GPU] test pipeline ready (LSWTCS_GPUTEST) — triangle PSO + VB\n"); fflush(stdout);
}

// ──────────────��─────────────────────────────��─────────────────────────────────
// Depth: per-RB_DEPTHCONTROL PSOs (created on demand) + a D32 depth buffer for the replay.
static D3D12_GRAPHICS_PIPELINE_STATE_DESC g_geom_pd = {};
static D3D12_INPUT_ELEMENT_DESC g_geom_il[2];
static ID3DBlob* g_geom_vs_blob = nullptr; static ID3DBlob* g_geom_ps_blob = nullptr;
static ID3D12PipelineState* g_geom_pso_by_dctl[32] = {};
static ID3D12Resource* g_geom_depth = nullptr;
static ID3D12DescriptorHeap* g_geom_dsv_heap = nullptr;
static ID3D12PipelineState* geom_pso_for(uint32_t dctl) {
    bool en = (dctl >> 1) & 1, wr = (dctl >> 2) & 1; uint32_t fn = (dctl >> 4) & 7;
    uint32_t key = (en ? 1u : 0u) | (wr ? 2u : 0u) | (fn << 2);
    if (g_geom_pso_by_dctl[key]) return g_geom_pso_by_dctl[key];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = g_geom_pd;
    pd.DepthStencilState.DepthEnable = (en || wr) ? TRUE : FALSE;
    pd.DepthStencilState.DepthWriteMask = wr ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = en ? (D3D12_COMPARISON_FUNC)(fn + 1) : D3D12_COMPARISON_FUNC_ALWAYS;  // Xenos 0..7 -> NEVER..ALWAYS
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(g_device->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState, (void**)&pso))) return nullptr;
    g_geom_pso_by_dctl[key] = pso;
    printf("[GEOM] depth PSO en=%d wr=%d func=%u\n", en, wr, fn); fflush(stdout);
    return pso;
}

static void gpu_init_geom_pipeline() {
    if (!getenv("LSWTCS_GEOM")) return;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = nullptr; ID3DBlob* sigerr = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigerr);
    if (FAILED(hr)) { printf("[GEOM] sig fail 0x%08X: %s\n",(unsigned)hr, sigerr?(const char*)sigerr->GetBufferPointer():"?"); safe_rel(sigerr); return; }
    g_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                  IID_ID3D12RootSignature, (void**)&g_geom_rootsig);
    safe_rel(sig); safe_rel(sigerr);

    // Passthrough: positions are already clip-space (transformed in geom_collect_draw).
    static const char* kHLSL =
        "struct VIn  { float3 pos:POSITION; float4 col:COLOR; };\n"
        "struct VOut { float4 pos:SV_POSITION; float4 col:COLOR; };\n"
        "VOut VSMain(VIn i){ VOut o; o.pos=float4(i.pos,1); o.col=i.col; return o; }\n"
        "float4 PSMain(VOut i):SV_TARGET { return i.col; }\n";
    ID3DBlob* vs = compile_hlsl(kHLSL, "VSMain", "vs_5_0");
    ID3DBlob* ps = compile_hlsl(kHLSL, "PSMain", "ps_5_0");
    if (!vs || !ps) { safe_rel(vs); safe_rel(ps); printf("[GEOM] shader compile failed\n"); return; }

    D3D12_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_geom_rootsig;
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout = { il, 2 };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    // Alpha blending: UI quads are translucent overlays (a=0 → no contribution),
    // so transparent/degenerate quads stop painting opaque black over content.
    auto& bt = pd.BlendState.RenderTarget[0];
    bt.BlendEnable = TRUE;
    bt.SrcBlend = D3D12_BLEND_ONE;  bt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;  /* premultiplied (kernel_stubs geom_emit_pm) */ bt.BlendOp = D3D12_BLEND_OP_ADD;
    bt.SrcBlendAlpha = D3D12_BLEND_ONE;   bt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; bt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    bt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pd.SampleDesc.Count = 1;
    hr = g_device->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState, (void**)&g_geom_pso);
    if (FAILED(hr)) { safe_rel(vs); safe_rel(ps); printf("[GEOM] PSO fail 0x%08X\n",(unsigned)hr); return; }
    // Keep the desc/blobs for depth-state PSO variants; the base PSO stays depth-less (no DSV).
    g_geom_vs_blob = vs; g_geom_ps_blob = ps;
    g_geom_il[0] = il[0]; g_geom_il[1] = il[1];
    g_geom_pd = pd; g_geom_pd.InputLayout = { g_geom_il, 2 };
    {   // D32 depth buffer + DSV heap for the replay
        D3D12_DESCRIPTOR_HEAP_DESC dh = {}; dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dh.NumDescriptors = 1;
        g_device->CreateDescriptorHeap(&dh, IID_ID3D12DescriptorHeap, (void**)&g_geom_dsv_heap);
        D3D12_HEAP_PROPERTIES dhp = {}; dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC dd = {}; dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = kSwapW; dd.Height = kSwapH; dd.DepthOrArraySize = 1; dd.MipLevels = 1;
        dd.Format = DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count = 1; dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
        if (g_geom_dsv_heap && SUCCEEDED(g_device->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &dd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_ID3D12Resource, (void**)&g_geom_depth)))
            g_device->CreateDepthStencilView(g_geom_depth, nullptr, g_geom_dsv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    // Dynamic vertex buffer (CPU-written upload heap). 300k verts * 28 B ≈ 8.4 MB.
    g_geom_vb_cap = 300000u * 28u;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = g_geom_vb_cap;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_ID3D12Resource, (void**)&g_geom_vb);
    if (FAILED(hr)) { printf("[GEOM] VB fail 0x%08X\n",(unsigned)hr); return; }
    D3D12_RANGE none = {}; g_geom_vb->Map(0, &none, &g_geom_vb_ptr);

    g_geom_enabled = true;
    printf("[GEOM] geometry executor ready (LSWTCS_GEOM) — passthrough PSO + %u-vert dynamic VB\n",
           300000u); fflush(stdout);
}

#include "gpu_xedraw.inc"

// Record the collected quads into the open command list (RT already bound + cleared).
static void gpu_geom_replay() {
    if (!g_geom_enabled) return;
    extern std::mutex g_geom_present_mtx;
    std::lock_guard<std::mutex> lk(g_geom_present_mtx);
    uint32_t nverts = g_geom_vert_ready;
    if (nverts == 0) return;
    if ((UINT64)nverts * 28u > g_geom_vb_cap) nverts = g_geom_vb_cap / 28u;
    memcpy(g_geom_vb_ptr, g_geom_present_verts, (size_t)nverts * 28u);
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = g_geom_vb->GetGPUVirtualAddress();
    vbv.SizeInBytes    = nverts * 28u;
    vbv.StrideInBytes  = 28u;
    g_cmd->SetGraphicsRootSignature(g_geom_rootsig);
    g_cmd->SetPipelineState(g_geom_pso);
    g_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cmd->IASetVertexBuffers(0, 1, &vbv);
    if (!g_geom_depth || g_geom_present_batch_count == 0 || getenv("LSWTCS_GEOM_NODEPTH")) {
        g_cmd->DrawInstanced(nverts, 1, 0, 0);   // NOTE: with a DSV bound the base PSO (no DSV format) is invalid; see present()
    } else {
        uint32_t first0 = g_geom_present_batches[0].first < nverts ? g_geom_present_batches[0].first : nverts;
        ID3D12PipelineState* p0 = geom_pso_for(0);
        if (first0 && p0) { g_cmd->SetPipelineState(p0); g_cmd->DrawInstanced(first0, 1, 0, 0); }
        for (uint32_t b = 0; b < g_geom_present_batch_count; ++b) {
            uint32_t s0 = g_geom_present_batches[b].first;
            uint32_t e0 = (b + 1 < g_geom_present_batch_count) ? g_geom_present_batches[b + 1].first : nverts;
            if (e0 > nverts) e0 = nverts;
            if (s0 >= e0) continue;
            ID3D12PipelineState* p = geom_pso_for(g_geom_present_batches[b].dctl);
            if (!p) continue;
            g_cmd->SetPipelineState(p);
            g_cmd->DrawInstanced(e0 - s0, 1, s0, 0);
        }
    }
    static int n = 0; if (n++ < 5) { printf("[GEOM] drew %u verts (%u quads)\n", nverts, nverts/6); fflush(stdout); }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE || msg == WM_DESTROY) { PostQuitMessage(0); ExitProcess(0); return 0; }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// Dedicated window thread: creates the HWND and runs GetMessage so the window
// stays responsive even when main/worker threads are busy in guest code.
static HANDLE g_window_ready_ev = nullptr;

static DWORD WINAPI window_thread_proc(LPVOID) {
    // DPI awareness: ensure a 1280x720 client = 1280x720 PHYSICAL pixels (no OS
    // up-scaling on high-DPI monitors). Prefer per-monitor-v2, fall back to system.
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *PFN_SPDAC)(HANDLE);
        if (u32) {
            auto p = (PFN_SPDAC)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
            if (!p || !p((HANDLE)-4)) SetProcessDPIAware();
        } else SetProcessDPIAware();
    }
    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = L"LSWTCSWnd";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassEx(&wc);

    // Fixed-size window (no resize/maximize) so the client stays exactly 1280x720.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, (LONG)kSwapW, (LONG)kSwapH};
    AdjustWindowRect(&r, style, FALSE);
    g_hwnd = CreateWindowEx(0, L"LSWTCSWnd",
                            L"LEGO Star Wars: The Complete Saga [Port]",
                            style,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top,
                            nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!g_hwnd) { printf("[GPU] CreateWindow failed (%lu)\n", GetLastError()); SetEvent(g_window_ready_ev); return 1; }
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    SetEvent(g_window_ready_ev);  // signal main thread that HWND is ready

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

static void barrier(ID3D12Resource* res,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_cmd->ResourceBarrier(1, &b);
}

// ───────────────────────────────────────────────────────��──────────────────────
void gpu_d3d12_init() {
    if (!getenv("LSWTCS_NORDOC")) rdoc_init();   // LSWTCS_NORDOC=1 skips (RenderDoc's crash handler hides our crash logs). BEFORE any D3D12 device creation so RenderDoc can hook the API
    // ── Window on dedicated thread (stays responsive during long guest init) ──
    g_window_ready_ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    CreateThread(nullptr, 0, window_thread_proc, nullptr, 0, nullptr);
    WaitForSingleObject(g_window_ready_ev, 5000);
    CloseHandle(g_window_ready_ev);
    g_window_ready_ev = nullptr;
    if (!g_hwnd) { printf("[GPU] Window thread failed\n"); return; }

    // ── DXGI factory ───────────────────────────────────────────────────────
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr)) { printf("[GPU] CreateDXGIFactory2 failed: 0x%08X\n", (unsigned)hr); return; }

    // ── D3D12 device (hardware first, then WARP) ───────────────────────────
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc; adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { safe_rel(adapter); continue; }
        hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                               IID_ID3D12Device, (void**)&g_device);
        if (SUCCEEDED(hr)) break;
        safe_rel(adapter);
    }
    if (!g_device) {
        IDXGIAdapter* warp = nullptr;
        factory->EnumWarpAdapter(IID_IDXGIAdapter, (void**)&warp);
        hr = D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0,
                               IID_ID3D12Device, (void**)&g_device);
        safe_rel(warp);
    }
    safe_rel(adapter);
    if (!g_device) {
        printf("[GPU] D3D12CreateDevice failed: 0x%08X\n", (unsigned)hr);
        safe_rel(factory); return;
    }

    // ── Command queue ──────────────────────────────────────────────────────
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_device->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, (void**)&g_queue);

    // ── Swapchain ─────────────────────────���────────────────────────────────
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width       = kSwapW;
    scd.Height      = kSwapH;
    scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kFrameCount;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    factory->CreateSwapChainForHwnd(g_queue, g_hwnd, &scd, nullptr, nullptr, &sc1);
    sc1->QueryInterface(IID_IDXGISwapChain3, (void**)&g_swapchain);
    safe_rel(sc1);
    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    safe_rel(factory);

    // ── RTV heap ─────────────────���─────────────────────────────���───────────
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kFrameCount;
    g_device->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, (void**)&g_rtv_heap);
    g_rtv_sz = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; i++) {
        g_swapchain->GetBuffer(i, IID_ID3D12Resource, (void**)&g_rt[i]);
        g_device->CreateRenderTargetView(g_rt[i], nullptr, rtv);
        rtv.ptr += g_rtv_sz;
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_ID3D12CommandAllocator, (void**)&g_alloc[i]);
    }

    // ── Command list + fence ──────────────────────────��────────────────────
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                 g_alloc[0], nullptr,
                                 IID_ID3D12GraphicsCommandList, (void**)&g_cmd);
    g_cmd->Close();
    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&g_fence);
    g_fence_ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // ── Upload buffer (persistently mapped, CPU-write) ─────────────────────
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width              = kUploadSize;
    rd.Height             = 1;
    rd.DepthOrArraySize   = 1;
    rd.MipLevels          = 1;
    rd.Format             = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count   = 1;
    rd.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                       IID_ID3D12Resource, (void**)&g_upload);
    D3D12_RANGE none = {};
    g_upload->Map(0, &none, &g_upload_ptr);

    g_frame_idx = g_swapchain->GetCurrentBackBufferIndex();
    // Readback staging buffer — created UNCONDITIONALLY (not just under GPUTEST) so the
    // in-process frame capture (LSWTCS_SHOTS) works in any render mode.
    if (!g_readback) {
        D3D12_HEAP_PROPERTIES rhp = {}; rhp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rrd = {};
        rrd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rrd.Width = (UINT64)kReadPitch * kSwapH;
        rrd.Height = 1; rrd.DepthOrArraySize = 1; rrd.MipLevels = 1;
        rrd.Format = DXGI_FORMAT_UNKNOWN; rrd.SampleDesc.Count = 1;
        rrd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        g_device->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &rrd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_ID3D12Resource, (void**)&g_readback);
    }
    gpu_init_test_pipeline();
    gpu_init_geom_pipeline();
    g_ready = true;
    printf("[GPU] D3D12 initialized — %ux%u window, BGRA8 swapchain\n", kSwapW, kSwapH);
    fflush(stdout);
}

// ─────────────────────────────────────────��────────────────────────────────────
static void gpu_d3d12_present_impl(uint8_t* base, uint32_t phys_addr,
                                   uint32_t row_pitch, uint32_t width, uint32_t height) {
    if (!g_ready) return;
    gpu_frame_take();
    struct GpPresent { uint64_t t0 = gp_now(); ~GpPresent() { g_gpuprof[GP_PRESENT] += gp_now() - t0; } } gp_present;

    // ── Window message pump ───────────────────────────────────────────────���
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (msg.message == WM_QUIT) ExitProcess(0);
    }
    // DIAG (LSWTCS_NOREPLAY=1): skip the replay + submit entirely (window freezes). Frame time in this
    // mode = the upper bound of what an asynchronous render thread can save.
    { static int noreplay = -1; if (noreplay < 0) { const char* e = getenv("LSWTCS_NOREPLAY"); noreplay = (e && e[0] == '1') ? 1 : 0; }
      if (noreplay) return; }

    // ── Wait for frame N-1 to finish ────────────────────��─────────────────
    if (g_fence->GetCompletedValue() < g_fence_for[g_frame_idx]) {
        uint64_t gp0 = gp_now();
        g_fence->SetEventOnCompletion(g_fence_for[g_frame_idx], g_fence_ev);
        // Block off the guest scheduler: holding the turn here (up to a whole refresh interval
        // under flip-model pacing) starved every other guest thread.
        lswtcs_gil_blocking([](void*) { WaitForSingleObject(g_fence_ev, INFINITE); }, nullptr);
        g_gpuprof[GP_FENCEWAIT] += gp_now() - gp0;
    }

    // ── Reset command recording ─────────────────────────────────��──────────
    g_alloc[g_frame_idx]->Reset();
    g_cmd->Reset(g_alloc[g_frame_idx], nullptr);

    ID3D12Resource* rt = g_rt[g_frame_idx];

    // ── RenderDoc capture trigger (LSWTCS_RDOC=N → capture frame N) ──
    static int rdoc_target = -2; static uint32_t rdoc_ctr = 0; bool rdoc_cap = false;
    if (rdoc_target == -2) { const char* e = getenv("LSWTCS_RDOC"); rdoc_target = (e ? atoi(e) : -1); }
    if (g_rdoc && rdoc_target >= 0 && rdoc_ctr == (uint32_t)rdoc_target) {
        g_rdoc->StartFrameCapture(nullptr, nullptr); rdoc_cap = true;
    }
    rdoc_ctr++;

    if (g_geom_enabled) {
        // ── Geometry executor: draw the game's collected UI quads to the backbuffer ──
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)g_frame_idx * g_rtv_sz;
        barrier(rt, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // Clear to the game's clear color (so UI sits on its real background).
        const float clr[4] = {0.05f, 0.05f, 0.08f, 1.0f};
        static int gpudraw = -1; if (gpudraw < 0) { const char* e = getenv("LSWTCS_GPUDRAW"); gpudraw = (e && e[0] == '0') ? 0 : 1; }   // default ON
        bool use_depth = g_geom_depth && (g_geom_present_batch_count || gpudraw) && !getenv("LSWTCS_GEOM_NODEPTH");
        if (use_depth) {
            D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_geom_dsv_heap->GetCPUDescriptorHandleForHeapStart();
            g_cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            // RB_DEPTH_CLEAR (D24S8: depth in bits 31:8). LSWTCS_GEOM_ZCLEAR overrides (e.g. 1.0).
            // The game clears depth with a draw (an offscreen ALWAYS/write quad), not via
            // RB_DEPTH_CLEAR (reads 0); its ship draws use LESS_EQUAL -> clear to the far plane.
            float zc = 1.0f;
            if (const char* e = getenv("LSWTCS_GEOM_ZCLEAR")) zc = (float)atof(e);
            g_cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, zc, 0, 0, nullptr);
        } else {
            g_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        }
        g_cmd->ClearRenderTargetView(rtv, clr, 0, nullptr);
        D3D12_VIEWPORT vp = {0,0,(float)kSwapW,(float)kSwapH,0,1};
        D3D12_RECT scr = {0,0,(LONG)kSwapW,(LONG)kSwapH};
        g_cmd->RSSetViewports(1,&vp); g_cmd->RSSetScissorRects(1,&scr);
        if (gpudraw) {
            uint64_t gp0 = gp_now();
            gpu_xedraw_replay(rtv, phys_addr);
            g_gpuprof[GP_REPLAY] += gp_now() - gp0;
            // Replay binds EDRAM render targets / blit targets: restore the backbuffer for the rest.
            if (use_depth) { D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_geom_dsv_heap->GetCPUDescriptorHandleForHeapStart();
                             g_cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv); }
            else g_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            g_cmd->RSSetViewports(1,&vp); g_cmd->RSSetScissorRects(1,&scr);
        }
        gpu_geom_replay();
        barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    } else if (g_gputest && g_test_pso) {
        // ── Milestone 1: render the test triangle into the backbuffer RTV ──
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)g_frame_idx * g_rtv_sz;
        barrier(rt, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float clr[4] = {0.10f, 0.10f, 0.25f, 1.0f};
        g_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g_cmd->ClearRenderTargetView(rtv, clr, 0, nullptr);
        D3D12_VIEWPORT vp = {0,0,(float)kSwapW,(float)kSwapH,0,1};
        D3D12_RECT scr = {0,0,(LONG)kSwapW,(LONG)kSwapH};
        g_cmd->RSSetViewports(1,&vp); g_cmd->RSSetScissorRects(1,&scr);
        g_cmd->SetGraphicsRootSignature(g_test_rootsig);
        g_cmd->SetPipelineState(g_test_pso);
        g_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_cmd->IASetVertexBuffers(0,1,&g_test_vbv);
        g_cmd->DrawInstanced(3,1,0,0);
        if (g_readback && !g_readback_done) {
            barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION rd_dst = {}, rd_src = {};
            rd_dst.pResource = g_readback; rd_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rd_dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            rd_dst.PlacedFootprint.Footprint.Width = kSwapW; rd_dst.PlacedFootprint.Footprint.Height = kSwapH;
            rd_dst.PlacedFootprint.Footprint.Depth = 1; rd_dst.PlacedFootprint.Footprint.RowPitch = kReadPitch;
            rd_src.pResource = rt; rd_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; rd_src.SubresourceIndex = 0;
            g_cmd->CopyTextureRegion(&rd_dst, 0, 0, 0, &rd_src, nullptr);
            barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        } else {
            barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }
    } else {

    // ── Copy guest framebuffer into the upload buffer ──────────────────────
    // Xbox 360 k_8_8_8_8 + k8in32 stores pixels as [B,G,R,A] bytes — exact
    // match for DXGI_FORMAT_B8G8R8A8_UNORM; no per-pixel byte-swap needed.

    const UINT copy_w   = (width  < kSwapW) ? width  : kSwapW;
    const UINT copy_h   = (height < kSwapH) ? height : kSwapH;

    // D3D12 upload buffer requires row pitch aligned to 256 bytes.
    const UINT aligned_pitch = (copy_w * 4 + 255u) & ~255u;
    const UINT64 needed      = (UINT64)aligned_pitch * copy_h;

    // Xbox 360 physical framebuffer addresses are small (e.g. 0x0000C000),
    // NOT >= 0x80000000 (which was wrong — that's the virtual address range).
    // Accept any non-zero physical address that fits in our 4 GB guest space.
    if (needed <= kUploadSize && phys_addr != 0 &&
        (uint64_t)phys_addr + (uint64_t)copy_h * aligned_pitch <= 0x100000000ULL) {
        uint8_t* dst = (uint8_t*)g_upload_ptr;
        const uint8_t* src = base + phys_addr;
        const UINT src_row = (row_pitch > 0) ? row_pitch : copy_w * 4;

        // Upload the guest framebuffer directly. Whatever the GPU-resolve stage
        // wrote (the cleared screen today, real geometry later) is shown as-is;
        // an un-rendered framebuffer simply shows black.
        for (UINT y = 0; y < copy_h; y++)
            memcpy(dst + (UINT64)y * aligned_pitch,
                   src + (UINT64)y * src_row,
                   copy_w * 4);

        // Transition render target PRESENT → COPY_DEST
        barrier(rt, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

        // CopyTextureRegion: upload buffer → swapchain backbuffer
        D3D12_TEXTURE_COPY_LOCATION src_loc = {};
        src_loc.pResource                             = g_upload;
        src_loc.Type                                  = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint.Offset                = 0;
        src_loc.PlacedFootprint.Footprint.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        src_loc.PlacedFootprint.Footprint.Width       = copy_w;
        src_loc.PlacedFootprint.Footprint.Height      = copy_h;
        src_loc.PlacedFootprint.Footprint.Depth       = 1;
        src_loc.PlacedFootprint.Footprint.RowPitch    = aligned_pitch;

        D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
        dst_loc.pResource        = rt;
        dst_loc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = 0;

        D3D12_BOX box = {0, 0, 0, copy_w, copy_h, 1};
        g_cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);

        // Transition back to PRESENT
        barrier(rt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    } else {
        // No valid framebuffer address yet — show black.
        uint8_t* dst = (uint8_t*)g_upload_ptr;
        const UINT tw = kSwapW, th = kSwapH;
        const UINT tp = (tw * 4 + 255u) & ~255u;
        for (UINT y = 0; y < th; y++)
            memset(dst + (UINT64)y * tp, 0, tw * 4);

        D3D12_TEXTURE_COPY_LOCATION src_loc{}, dst_loc{};
        dst_loc.pResource        = rt;
        dst_loc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = 0;
        src_loc.pResource        = g_upload;
        src_loc.Type             = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint.Offset                = 0;
        src_loc.PlacedFootprint.Footprint.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        src_loc.PlacedFootprint.Footprint.Width       = tw;
        src_loc.PlacedFootprint.Footprint.Height      = th;
        src_loc.PlacedFootprint.Footprint.Depth       = 1;
        src_loc.PlacedFootprint.Footprint.RowPitch    = tp;

        barrier(rt, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_BOX box = {0, 0, 0, tw, th, 1};
        g_cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);
        barrier(rt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    }
    }  // end of (g_gputest ? triangle : framebuffer-copy)

    // ── CAPTURE: copy the backbuffer → readback staging (LSWTCS_SHOTS=N → every N frames). ──
    // Branch-agnostic: RT is in PRESENT state here. The actual PNG is written after the fence below.
    static int shot_n = -1; static uint32_t pres_ctr = 0;
    if (shot_n < 0) { const char* e = getenv("LSWTCS_SHOTS"); shot_n = (e && atoi(e) > 0) ? atoi(e) : 0;
                      if (shot_n) _mkdir("shots"); }
    static int bluecatch = -1; if (bluecatch < 0) { const char* e = getenv("LSWTCS_BLUECATCH"); bluecatch = (e && e[0] == '1') ? 1 : 0; if (bluecatch) _mkdir("shots"); }
    static bool shot_once = false;   // DIAG: file "shot_now" -> PNG of this frame
    if ((pres_ctr % 30) == 0 && GetFileAttributesA("shot_now") != INVALID_FILE_ATTRIBUTES) { DeleteFileA("shot_now"); _mkdir("shots"); shot_once = true; }
    bool cap = (shot_n > 0 && g_readback && (pres_ctr % (uint32_t)shot_n) == 0) || (bluecatch && g_readback) || (shot_once && g_readback);
    if (cap) {
        barrier(rt, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION rd{}, rs{};
        rd.pResource = g_readback; rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.PlacedFootprint.Footprint.Width = kSwapW; rd.PlacedFootprint.Footprint.Height = kSwapH;
        rd.PlacedFootprint.Footprint.Depth = 1; rd.PlacedFootprint.Footprint.RowPitch = kReadPitch;
        rs.pResource = rt; rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; rs.SubresourceIndex = 0;
        g_cmd->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
        barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    }
    pres_ctr++;

    // ── Execute + present ──────────────────────────────────────────────────
    uint64_t gp_sub0 = gp_now();
    g_cmd->Close();
    ID3D12CommandList* lists[] = {g_cmd};
    g_queue->ExecuteCommandLists(1, lists);

    // Sync interval 0: no host-vsync — the runtime's frame limiter (kernel_stubs
    // VdSwap) is the sole timing authority, locking to a fixed target (Switch
    // Lite 60 Hz) regardless of the host monitor's refresh rate.
    g_swapchain->Present(0, 0);
    g_gpuprof[GP_SUBMIT] += gp_now() - gp_sub0;

    if (rdoc_cap && g_rdoc) {
        g_rdoc->EndFrameCapture(nullptr, nullptr);
        printf("[RDOC] captured frame %d → rdoc/cap_*.rdc\n", rdoc_target); fflush(stdout);
    }

    g_fence_val++;
    g_queue->Signal(g_fence, g_fence_val);
    g_fence_for[g_frame_idx] = g_fence_val;
    if (gpu_xedraw_dump_pending()) {   // LSWTCS_RESDUMP: wait for this frame, write the resolve targets
        g_fence->SetEventOnCompletion(g_fence_val, g_fence_ev);
        WaitForSingleObject(g_fence_ev, INFINITE);
        gpu_xedraw_dump_write();
    }

    // One-shot verification: wait for this frame, read back a few pixels. If the
    // triangle rasterized, the center differs from the (clear-colour) corners.
    if (g_gputest && g_readback && !g_readback_done) {
        g_fence->SetEventOnCompletion(g_fence_val, g_fence_ev);
        WaitForSingleObject(g_fence_ev, INFINITE);
        uint8_t* m = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)kReadPitch * kSwapH };
        if (SUCCEEDED(g_readback->Map(0, &rr, (void**)&m)) && m) {
            auto px = [&](UINT x, UINT y) -> uint32_t { return *(uint32_t*)(m + (UINT64)y * kReadPitch + (UINT64)x * 4); };
            printf("[GPUTEST] readback center=0x%08X tl=0x%08X tr=0x%08X bl=0x%08X br=0x%08X\n",
                   px(kSwapW/2, kSwapH/2), px(4,4), px(kSwapW-5,4), px(4,kSwapH-5), px(kSwapW-5,kSwapH-5));
            fflush(stdout);
            D3D12_RANGE wn = {};
            g_readback->Unmap(0, &wn);
        }
        g_readback_done = true;
    }

    // ── CAPTURE write-out: wait for this frame, map readback, encode PNG ──
    if (cap) {
        g_fence->SetEventOnCompletion(g_fence_val, g_fence_ev);
        WaitForSingleObject(g_fence_ev, INFINITE);
        uint8_t* m = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)kReadPitch * kSwapH };
        if (SUCCEEDED(g_readback->Map(0, &rr, (void**)&m)) && m && bluecatch && !shot_once && !(shot_n > 0 && ((pres_ctr - 1) % (uint32_t)shot_n) == 0)) {
            // Blue-pixel catch: sample a 64x36 grid of the presented image (B8G8R8A8) for pure blue.
            int nb = 0;
            for (UINT gy = 0; gy < 36; ++gy) for (UINT gx = 0; gx < 64; ++gx) {
                const uint8_t* p = m + (UINT64)(gy * 20 + 10) * kReadPitch + (UINT64)(gx * 20 + 10) * 4;
                if (p[0] > 200 && p[1] < 40 && p[2] < 40) ++nb;
            }
            static int ncaught = 0;
            if (nb >= 6 && ncaught < 12) {
                ++ncaught;
                char p[64]; snprintf(p, sizeof(p), "shots/blue_%05u.png", (pres_ctr - 1));
                lswtcs_write_png(p, m, (int)kSwapW, (int)kSwapH, (int)kReadPitch);
                printf("[BLUE] present %u: %d/2304 blue samples -> %s\n[BLUE]   this: %s\n[BLUE]   prev: %s\n", pres_ctr - 1, nb, p,
                       g_xd_fstate.c_str(), g_xd_fstate_prev.c_str());
                fflush(stdout);
            }
            D3D12_RANGE wn = {}; g_readback->Unmap(0, &wn);
        } else if (m) {
            lswtcs_write_png("shots/latest.png", m, (int)kSwapW, (int)kSwapH, (int)kReadPitch);
            char p[64]; snprintf(p, sizeof(p), "shots/f%05u.png", (pres_ctr - 1));
            lswtcs_write_png(p, m, (int)kSwapW, (int)kSwapH, (int)kReadPitch);
            D3D12_RANGE wn = {}; g_readback->Unmap(0, &wn);
            printf("[SHOT] wrote shots/latest.png + %s\n", p); fflush(stdout);
        }
        shot_once = false;
    }

    g_frame_idx = g_swapchain->GetCurrentBackBufferIndex();
}

// ──────────────────────────────────────────���───────────────────────────────────
bool gpu_d3d12_poll() {
    if (!g_hwnd) return true;
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (msg.message == WM_QUIT) return false;
    }
    return true;
}

// ── Render thread (LSWTCS_RENDERTHREAD, default on) ─────────────────────────────────────────────
// gpu_d3d12_present is called by the guest thread that runs VdSwap. The replay of the recorded frame
// (D3D12 command building), submit and present run on this dedicated host thread instead, in parallel
// with the guest's next frame: one frame in flight. The guest waits (off the cooperative scheduler)
// only when the previous frame's replay has not finished yet. Everything D3D12 / xd_* is touched only
// by this thread after init. LSWTCS_RENDERTHREAD=0: replay synchronously on the guest thread (old).
// Switch: this thread takes one of the three application cores.
struct GpuRenderJob { uint8_t* base; uint32_t phys, pitch, w, h; };
static std::mutex g_rt_mtx;
static std::condition_variable g_rt_cv;
static GpuRenderJob g_rt_job;
static bool g_rt_pending = false, g_rt_busy = false;
static void gpu_render_thread_main() {
    for (;;) {
        GpuRenderJob job;
        { std::unique_lock<std::mutex> lk(g_rt_mtx);
          g_rt_cv.wait(lk, [] { return g_rt_pending; });
          job = g_rt_job; g_rt_pending = false; g_rt_busy = true; }
        gpu_d3d12_present_impl(job.base, job.phys, job.pitch, job.w, job.h);
        { std::lock_guard<std::mutex> lk(g_rt_mtx); g_rt_busy = false; }
        g_rt_cv.notify_all();
    }
}
void gpu_d3d12_present(uint8_t* base, uint32_t phys_addr, uint32_t row_pitch, uint32_t width, uint32_t height) {
    static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_RENDERTHREAD"); on = (e && e[0] == '0') ? 0 : 1; }
    if (!on) { gpu_d3d12_present_impl(base, phys_addr, row_pitch, width, height); return; }
    static bool started = false;
    if (!started) {
        started = true;
        std::thread(gpu_render_thread_main).detach();
        printf("[RENDERTHREAD] replay + present run on a dedicated thread (LSWTCS_RENDERTHREAD=0 disables)\n"); fflush(stdout);
    }
    uint64_t t0 = gp_now();
    {   // wait for the previous frame's replay to finish (one frame in flight), off the guest scheduler
        std::unique_lock<std::mutex> lk(g_rt_mtx);
        if (g_rt_pending || g_rt_busy) {
            lk.unlock();
            lswtcs_gil_blocking([](void*) {
                std::unique_lock<std::mutex> l2(g_rt_mtx);
                g_rt_cv.wait(l2, [] { return !g_rt_pending && !g_rt_busy; });
            }, nullptr);
            lk.lock();
        }
        g_rt_job = GpuRenderJob{base, phys_addr, row_pitch, width, height};
        g_rt_pending = true;
    }
    g_rt_cv.notify_all();
    g_gpuprof[GP_RTWAIT] += gp_now() - t0;
}
