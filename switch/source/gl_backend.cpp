// M3 OpenGL backend (switch-mesa, EGL + GL 4.3 core): replays the frames the PM4 recorder builds
// (include/lsw_gpu_frame.h) with the GLSL shaders from gl_bridge.cc. Stage 2 = the PC's
// LSWTCS_GPURT=0 path: only draws to the 1280-pitch frame target are recorded, and they go straight
// into one 1280x720 scene framebuffer (no EDRAM render targets, no resolves yet).
//
// Threading: a render thread owns the GL context and does all GL calls (as gpu_d3d12.cpp's render
// thread on PC); guest threads only publish frames (kernel_stubs) and signal presents.
//
// Conventions: glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE) gives D3D's clip space, so the bridge's
// viewport/NDC values (same as the PC's) apply unchanged and framebuffer row 0 is the top of the
// image; the final blit to the window flips it.
#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lsw_gl_shader.h"
#include "lsw_gpu_bridge.h"
#include "lsw_gpu_frame.h"
#include "lsw_spirv_glsl.h"
#include <dirent.h>
#include <sys/stat.h>
#define XXH_INLINE_ALL
#include "xxhash.h"

extern uint8_t* g_base;
void host_audio_hold(bool on);   // audio_switch.cpp: pause the device feed while programs build

namespace {

constexpr uint32_t kSceneW = 1280, kSceneH = 720;
constexpr GLsizeiptr kShmSize = GLsizeiptr(512) << 20;
constexpr GLsizeiptr kShmBinding = GLsizeiptr(128) << 20;   // xe_shared_memory[4], one per 128 MB
#ifndef GL_UPPER_LEFT
#define GL_UPPER_LEFT 0x8CA2
#endif
#ifndef GL_ZERO_TO_ONE
#define GL_ZERO_TO_ONE 0x935F
#endif
#ifndef GL_MIRROR_CLAMP_TO_EDGE
#define GL_MIRROR_CLAMP_TO_EDGE 0x8743
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLContext g_context = EGL_NO_CONTEXT;
EGLSurface g_surface = EGL_NO_SURFACE;
EGLConfig g_config = nullptr;
constexpr int kMaxCompileWorkers = 3;
EGLContext g_worker_ctx[kMaxCompileWorkers] = {};
int g_num_workers = 0;
bool g_gl_ok = false;

std::mutex g_mtx;
std::condition_variable g_cv;
uint64_t g_frames_posted = 0;   // guest swaps
uint64_t g_frames_done = 0;     // guest swaps the render thread has replayed and presented (under g_mtx)
extern "C" void lswtcs_gil_blocking(void (*fn)(void*), void* arg);   // kernel_stubs.cpp
extern "C" void lsw_malloc_lock_stats(uint64_t* wait_ns, uint64_t* contended, uint64_t* calls);   // platform_switch.cpp

bool env_on(const char* name, bool def) {
    const char* e = getenv(name);
    return e ? e[0] == '1' : def;
}

// ---- EGL --------------------------------------------------------------------------------------
typedef void (*PFN_ClipControl)(GLenum, GLenum);
PFN_ClipControl p_glClipControl = nullptr;
typedef void (*PFN_BufferStorage)(GLenum, GLsizeiptr, const void*, GLbitfield);
PFN_BufferStorage p_glBufferStorage = nullptr;   // GL 4.4 / ARB_buffer_storage (from EGL, like glClipControl)
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#define GL_MAP_COHERENT_BIT 0x0080
#define GL_DYNAMIC_STORAGE_BIT 0x0100
#endif

bool egl_init() {
    // Mesa's shader disk cache (switch-mesa rebuilt with it: switch/tools/build_mesa.sh): compiled
    // programs persist on the SD card, so a program is compiled once per console, not once per
    // launch. Must be set before the screen is created. Ignored by the stock switch-mesa.
    setenv("MESA_GLSL_CACHE_DIR", "sdmc:/lswtcs/mesacache", 0);
    // The compile benchmark (gl_bench) measures the full pipeline: no cache hits.
    if (env_on("LSWTCS_GLBENCH", false)) setenv("MESA_GLSL_CACHE_DISABLE", "true", 1);
    // Xenos 0 * anything = 0 by Maxwell's FMZ multiply (switch/mesa/mesa_fmz_edits.py) instead of
    // per-product selects in the GLSL; must match the translator setting in gl_bridge.cc.
    if (env_on("LSWTCS_GLFMZ", true)) setenv("NV50_MUL_ZERO_WINS", "1", 1);
    printf("[GL] eglGetDisplay...\n"); fflush(stdout);
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY) { printf("[GL] eglGetDisplay failed\n"); return false; }
    printf("[GL] eglInitialize...\n"); fflush(stdout);
    eglInitialize(g_display, nullptr, nullptr);
    printf("[GL] eglInitialize done (0x%X)\n", eglGetError()); fflush(stdout);
    if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) { printf("[GL] eglBindAPI 0x%X\n", eglGetError()); return false; }
    static const EGLint fb_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig& config = g_config; EGLint n = 0;
    eglChooseConfig(g_display, fb_attrs, &config, 1, &n);
    if (!n) { printf("[GL] no EGL config (0x%X)\n", eglGetError()); return false; }
    printf("[GL] eglCreateWindowSurface...\n"); fflush(stdout);
    g_surface = eglCreateWindowSurface(g_display, config, nwindowGetDefault(), nullptr);
    if (g_surface == EGL_NO_SURFACE) { printf("[GL] window surface failed (0x%X)\n", eglGetError()); return false; }
    printf("[GL] eglCreateContext...\n"); fflush(stdout);
    static const EGLint ctx_attrs[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_NONE};
    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (g_context == EGL_NO_CONTEXT) { printf("[GL] context failed (0x%X)\n", eglGetError()); return false; }
    printf("[GL] eglCreateContext done\n"); fflush(stdout);
    // Shader-compile workers: contexts sharing objects with the render context, made current
    // surfaceless on their own threads (program compile below).
    const char* exts = eglQueryString(g_display, EGL_EXTENSIONS);
    const bool surfaceless = exts && strstr(exts, "EGL_KHR_surfaceless_context");
    const char* nw = getenv("LSWTCS_GLWORKERS");
    // Default 0 (compile on the render thread): background compiling works (no corruption since the
    // workers never submit) but on a cold start it shows stale frames for minutes, worse than the
    // stall (user test 2026-10-07). LSWTCS_GLWORKERS=1 re-enables it.
    // Default 2 (2026-10-08): a new scene's programs build on the render thread and two workers at once
    // (LSWTCS_GLWAIT). Needs switch-mesa's EGL to honour the share context (mesa_egl_share_edits.py;
    // without it worker programs were unknown names: black title) and rpmalloc (newlib's single malloc
    // lock made parallel builds 20x slower). Title, 12 programs: 0.80 s of worker time, no GL errors.
    // LSWTCS_GLWORKERS=0: everything on the render thread.
    const int want = surfaceless ? std::max(0, std::min(nw ? atoi(nw) : 2, int(kMaxCompileWorkers))) : 0;
    for (int i = 0; i < want; ++i) {
        EGLContext c = eglCreateContext(g_display, config, g_context, ctx_attrs);
        if (c == EGL_NO_CONTEXT) { printf("[GL] worker context %d failed (0x%X)\n", i, eglGetError()); break; }
        g_worker_ctx[g_num_workers++] = c;
    }
    printf("[GL] %d shader-compile worker contexts (surfaceless %s)\n", g_num_workers, surfaceless ? "yes" : "NO");
    eglMakeCurrent(g_display, g_surface, g_surface, g_context);
    if (!gladLoadGL()) { printf("[GL] gladLoadGL failed\n"); return false; }
    eglSwapInterval(g_display, 1);
    printf("[GL] %s | %s | GLSL %s\n", (const char*)glGetString(GL_RENDERER),
           (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
    // switch-glad stops at GL 4.3; glClipControl (4.5 / ARB_clip_control) comes from EGL.
    p_glClipControl = (PFN_ClipControl)eglGetProcAddress("glClipControl");
    p_glBufferStorage = (PFN_BufferStorage)eglGetProcAddress("glBufferStorage");
    GLint ssbo_max = 0, ubo_align = 0;
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &ssbo_max);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ubo_align);
    printf("[GL] glClipControl %s, max SSBO block %d MB, UBO offset alignment %d\n",
           p_glClipControl ? "available" : "MISSING (image will be flipped, depth halved)", ssbo_max >> 20, ubo_align);
    return true;
}

// ---- resources --------------------------------------------------------------------------------
GLuint g_vao = 0, g_shm = 0, g_arena = 0;
GLsizeiptr g_arena_cap = 0;
GLuint g_scene_fbo = 0, g_scene_color = 0, g_scene_depth = 0;
GLuint g_dummy[3] = {};   // 2D array, cube, 3D: 1x1 zero texel for unbound / unsupported textures

void resources_init() {
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_shm);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_shm);
    glBufferData(GL_SHADER_STORAGE_BUFFER, kShmSize, nullptr, GL_DYNAMIC_DRAW);
    for (GLuint i = 0; i < 4; ++i) glBindBufferRange(GL_SHADER_STORAGE_BUFFER, i, g_shm, i * kShmBinding, kShmBinding);
    glGenTextures(1, &g_scene_color);
    glBindTexture(GL_TEXTURE_2D, g_scene_color);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kSceneW, kSceneH);
    glGenRenderbuffers(1, &g_scene_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_scene_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, kSceneW, kSceneH);
    glGenFramebuffers(1, &g_scene_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_scene_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_scene_depth);
    const GLenum fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const uint32_t zero[6] = {};
    glGenTextures(3, g_dummy);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_dummy[0]);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 1, 1, 1);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    glBindTexture(GL_TEXTURE_CUBE_MAP, g_dummy[1]);
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_RGBA8, 1, 1);
    for (int f = 0; f < 6; ++f)
        glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    glBindTexture(GL_TEXTURE_3D, g_dummy[2]);
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA8, 1, 1, 1);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    // The shaders read shared memory through an R32UI buffer texture on kLswShmTextureUnit
    // (storage-buffer reads are dropped by Eden; lsw_spirv_glsl.cc).
    GLuint shm_tex;
    glGenTextures(1, &shm_tex);
    glActiveTexture(GL_TEXTURE0 + kLswShmTextureUnit);
    glBindTexture(GL_TEXTURE_BUFFER, shm_tex);
    glTexBufferRange(GL_TEXTURE_BUFFER, GL_R32UI, g_shm, 0, kShmSize);
    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (p_glClipControl) p_glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    glDisable(GL_PRIMITIVE_RESTART);
    glEnable(GL_SCISSOR_TEST);
    printf("[GL] resources: shared memory %lld MB, scene FBO status 0x%X, error 0x%X\n",
           (long long)(kShmSize >> 20), fbs, glGetError());
    fflush(stdout);
}

// ---- shaders / programs -----------------------------------------------------------------------
uint32_t g_compiled = 0, g_compile_failed = 0, g_link_failed = 0;

GLuint shader_object(const LswGlShader* s) {
    LswGlShader* m = const_cast<LswGlShader*>(s);   // `gl` is render-thread owned (lsw_gl_shader.h)
    if (m->gl) return m->gl == kLswGlFailed ? 0 : m->gl;
    static const GLenum kType[3] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, GL_GEOMETRY_SHADER};
    GLuint sh = glCreateShader(kType[s->stage]);
    const char* src = s->glsl.c_str();
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        if (++g_compile_failed <= 10) {
            static const char* const kExt[3] = {"vert", "frag", "geom"};
            char path[64];
            snprintf(path, sizeof path, "glsl_fail_%u.%s", g_compile_failed, kExt[s->stage]);
            if (FILE* f = fopen(path, "w")) { fputs(src, f); fclose(f); }
            printf("[GL] shader %016llX compile FAILED (%s): %s\n", (unsigned long long)s->key, path, log);
            fflush(stdout);
        }
        glDeleteShader(sh);
        m->gl = kLswGlFailed;
        return 0;
    }
    ++g_compiled;
    m->gl = sh;
    return sh;
}

// ---- programs: background compile, program list, binary cache --------------------------------
// Mesa compiles each translated program in ~2 s of Switch CPU time (hub: 29 programs = 57 s, measured
// 2026-10-07), and switch-mesa has no disk shader cache, so it reports 0 program-binary formats.
// Instead of stalling the render thread:
//  - programs are compiled and linked on worker threads (own GL contexts sharing objects with the
//    render context); a draw whose program is not ready yet is skipped for that frame;
//  - the GLSL of every program the game uses is saved to sdmc:/lswtcs/glprogs/<key>.glsl3, and at
//    boot all saved programs are queued, so programs from earlier sessions compile in the background
//    during the boot / title screens; programs the game asks for jump the queue;
//  - if the driver ever offers program binaries (a Mesa with its disk cache), workers also save and
//    load binaries in sdmc:/lswtcs/glcache (LSWTCS_GLCACHE=0 disables).
// Key = hash of the three GLSL sources + driver. LSWTCS_GLWORKERS = worker count (default 2; 0 =
// compile synchronously on the render thread, the old behaviour).
bool g_binary_cache_on = false;
uint64_t g_driver_hash = 0;
constexpr uint32_t kProgMagic = 0x5057534Cu, kProgVersion = 1;   // "LSWP" binary file
constexpr uint32_t kSrcMagic = 0x5357534Cu;                        // "LSWS" source file

struct ProgSources { std::string vs, ps, gs; };   // ps/gs empty = none
enum ProgState : uint8_t { kProgQueued, kProgReady, kProgFailed };
struct ProgEntry { ProgState state = kProgQueued; GLuint prog = 0; bool requested = false; };
std::mutex g_prog_mtx;
std::condition_variable g_prog_cv;
std::unordered_map<uint64_t, ProgEntry> g_prog_entries;            // by content key
std::deque<std::pair<uint64_t, ProgSources>> g_prog_queue;          // front = most urgent
// Stats (under g_prog_mtx).
uint32_t g_prog_compiled = 0, g_prog_from_binary = 0, g_prog_cache_written = 0, g_prog_boot_queued = 0;
uint64_t g_prog_compile_ns = 0;
uint32_t g_prog_pending_draws = 0;   // render thread only: draws skipped this replay (program not ready)
std::condition_variable g_prog_done_cv;   // a worker published a program (render thread waits on it)
std::vector<uint64_t> g_frame_wait;      // render thread: content keys the current replay waits for
// LSWTCS_GLWAIT (default 1): a replay whose programs are not built yet waits for them, the render thread
// building queued programs itself next to the workers (all of a new scene's programs in parallel; the
// guest is held meanwhile by gpu_d3d12_present). 0 = the old behaviour: skip the frame, show the last
// complete one (stale frames for minutes on a cold start).
bool prog_wait_mode() {
    static const bool on = env_on("LSWTCS_GLWAIT", true);
    return on;
}
// LSWTCS_GLPROGLIST (default on since 2026-10-09, user decision; was off under the 2026-10-07 "cheaper,
// not cached" rule): save every program's GLSL to sdmc:/lswtcs/glprogs and queue the saved ones at
// boot, so a scene seen in an earlier session (cutscene cuts: ~70 ms per program on hardware, 27 at
// once) finds its programs already built by the workers in spare CPU time. =0 turns it off.
// Bump when the generated GLSL changes for every program (saved sources from older generators would be compiled
// at every boot for nothing): glprogs2 = pixel shaders without `precise` (2026-10-09). Old folders are left alone.
#define LSW_PROGLIST_DIR "glprogs2"
bool prog_list_on() {
    static const bool on = env_on("LSWTCS_GLPROGLIST", true);
    return on;
}

uint64_t program_cache_key(const ProgSources& s) {
    uint64_t h = XXH3_64bits_withSeed(s.vs.data(), s.vs.size(), g_driver_hash);
    h = XXH3_64bits_withSeed(s.ps.empty() ? "-" : s.ps.data(), s.ps.empty() ? 1 : s.ps.size(), h);
    return XXH3_64bits_withSeed(s.gs.empty() ? "-" : s.gs.data(), s.gs.empty() ? 1 : s.gs.size(), h);
}

// Binary cache (dormant unless the driver reports binary formats).
GLuint program_binary_load(uint64_t ckey) {
    char path[64];
    snprintf(path, sizeof path, "glcache/%016llX.bin", (unsigned long long)ckey);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t hdr[4] = {};
    std::vector<uint8_t> bin;
    bool ok = fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == kProgMagic && hdr[1] == kProgVersion && hdr[3] > 0 &&
              hdr[3] < (64u << 20);
    if (ok) { bin.resize(hdr[3]); ok = fread(bin.data(), 1, bin.size(), f) == bin.size(); }
    fclose(f);
    GLuint prog = 0;
    if (ok) {
        prog = glCreateProgram();
        glProgramBinary(prog, GLenum(hdr[2]), bin.data(), GLsizei(bin.size()));
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) { glDeleteProgram(prog); prog = 0; }
    }
    if (!prog) remove(path);
    return prog;
}
bool program_binary_store(uint64_t ckey, GLuint prog) {
    GLint len = 0;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0) return false;
    std::vector<uint8_t> bin(static_cast<size_t>(len));
    GLenum format = 0;
    GLsizei got = 0;
    glGetProgramBinary(prog, len, &got, &format, bin.data());
    if (got <= 0) return false;
    char path[64], tmp[72];
    snprintf(path, sizeof path, "glcache/%016llX.bin", (unsigned long long)ckey);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return false;
    const uint32_t hdr[4] = {kProgMagic, kProgVersion, uint32_t(format), uint32_t(got)};
    const bool ok = fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(bin.data(), 1, size_t(got), f) == size_t(got);
    fclose(f);
    if (ok && rename(tmp, path) == 0) return true;
    remove(tmp);
    return false;
}

// Program list on the SD card: "LSWS", 3 lengths, then the three sources.
void program_list_save(uint64_t ckey, const ProgSources& s) {
    char path[64], tmp[72];
    snprintf(path, sizeof path, LSW_PROGLIST_DIR "/%016llX.glsl3", (unsigned long long)ckey);
    struct stat st;
    if (stat(path, &st) == 0) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    const uint32_t hdr[4] = {kSrcMagic, uint32_t(s.vs.size()), uint32_t(s.ps.size()), uint32_t(s.gs.size())};
    bool ok = fwrite(hdr, sizeof hdr, 1, f) == 1;
    for (const std::string* p : {&s.vs, &s.ps, &s.gs}) ok = ok && fwrite(p->data(), 1, p->size(), f) == p->size();
    fclose(f);
    if (!(ok && rename(tmp, path) == 0)) remove(tmp);
}
bool program_list_read(const char* path, ProgSources& s) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint32_t hdr[4] = {};
    bool ok = fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == kSrcMagic && hdr[1] && hdr[1] < (8u << 20) &&
              hdr[2] < (8u << 20) && hdr[3] < (8u << 20);
    if (ok) {
        s.vs.resize(hdr[1]); s.ps.resize(hdr[2]); s.gs.resize(hdr[3]);
        for (std::string* p : {&s.vs, &s.ps, &s.gs}) ok = ok && fread(&(*p)[0], 1, p->size(), f) == p->size();
    }
    fclose(f);
    return ok;
}

// Compile + link one program in the calling thread's context. Returns 0 on failure (logged).
// Compiled shader stages shared by programs (LSWTCS_GLSTAGEREUSE, default on), keyed by GLSL + stage:
// programs pair a few vertex / geometry shaders with many pixel shaders, and each stage used to be
// compiled again for every program (hub: 52 programs over 77 unique shaders). In-memory for the session
// only; objects are shared across the compile contexts (EGL share context). 0 = failed to compile.
std::mutex g_stage_mtx;
std::unordered_map<uint64_t, GLuint> g_stages;
uint32_t g_stage_reused = 0;
bool stage_reuse_on() {
    static const bool on = env_on("LSWTCS_GLSTAGEREUSE", true);
    return on;
}

GLuint program_build(uint64_t ckey, const ProgSources& s) {
    auto compile_new = [&](GLenum type, const std::string& src) -> GLuint {
        GLuint sh = glCreateShader(type);
        const char* c = src.c_str();
        glShaderSource(sh, 1, &c, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetShaderInfoLog(sh, sizeof log, nullptr, log);
            printf("[GLPROG] %016llX: %s compile FAILED: %s\n", (unsigned long long)ckey,
                   type == GL_VERTEX_SHADER ? "VS" : type == GL_FRAGMENT_SHADER ? "PS" : "GS", log);
            fflush(stdout);
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    };
    auto compile = [&](GLenum type, const std::string& src) -> GLuint {
        if (!stage_reuse_on()) return compile_new(type, src);
        const uint64_t h = XXH3_64bits_withSeed(src.data(), src.size(), type);
        {
            std::lock_guard<std::mutex> lk(g_stage_mtx);
            auto it = g_stages.find(h);
            if (it != g_stages.end()) { ++g_stage_reused; return it->second; }
        }
        const GLuint sh = compile_new(type, src);   // outside the lock: other threads keep compiling
        std::lock_guard<std::mutex> lk(g_stage_mtx);
        auto ins = g_stages.emplace(h, sh);
        if (!ins.second) {   // another thread compiled the same stage meanwhile: use theirs
            if (sh) glDeleteShader(sh);
            ++g_stage_reused;
        }
        return ins.first->second;
    };
    // Draws without a pixel shader (depth-only; colour writes masked) get an empty fragment shader:
    // a VS+GS program with no fragment stage hung Eden's shader recompiler.
    static const std::string kEmptyFs = "#version 430\nvoid main() {}\n";
    const uint64_t t_compile = gp_now();
    const GLuint v = compile(GL_VERTEX_SHADER, s.vs);
    const GLuint p = compile(GL_FRAGMENT_SHADER, s.ps.empty() ? kEmptyFs : s.ps);
    const GLuint g = s.gs.empty() ? 0 : compile(GL_GEOMETRY_SHADER, s.gs);
    const uint64_t t_link = gp_now();
    GLuint prog = 0;
    if (v && p && (s.gs.empty() || g)) {
        prog = glCreateProgram();
        glAttachShader(prog, v);
        glAttachShader(prog, p);
        if (g) glAttachShader(prog, g);
        if (g_binary_cache_on) glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetProgramInfoLog(prog, sizeof log, nullptr, log);
            printf("[GLPROG] %016llX: link FAILED: %s\n", (unsigned long long)ckey, log);
            fflush(stdout);
            glDeleteProgram(prog);
            prog = 0;
        } else {
            glDetachShader(prog, v);
            glDetachShader(prog, p);
            if (g) glDetachShader(prog, g);
        }
    }
    if (!stage_reuse_on())   // shared stages stay alive for the next program
        for (GLuint sh : {v, p, g}) if (sh) glDeleteShader(sh);
    static const bool timing = env_on("LSWTCS_GLTIMING", false);
    if (timing) {
        printf("[GLPROGT] %016llX: compile %llu ms, link %llu ms (VS %zu KB, PS %zu KB, GS %zu KB)\n", (unsigned long long)ckey,
               (unsigned long long)((t_link - t_compile) / 1000000), (unsigned long long)((gp_now() - t_link) / 1000000),
               s.vs.size() / 1024, s.ps.size() / 1024, s.gs.size() / 1024);
        fflush(stdout);
    }
    return prog;
}

// Worker thread: takes queued programs (front first), builds them, publishes the result.
// Threads otherwise all start on the process's default core (libnx), next to the guest threads.
// Layout: guest threads core 0, render thread core 1, shader-compile workers core 2.
void pin_to_core(int core, const char* what) {
    if (env_on("LSWTCS_NOPIN", false)) return;   // DIAG: keep the default core
    const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    printf("[NX] %s pinned to core %d (rc 0x%X)\n", what, core, rc);
    fflush(stdout);
}

void compile_worker_main(int index) {
    // Worker 0 on core 2 (idle). Worker 1 on core 0 at the lowest priority: the guest threads live there
    // but are held while a new scene's programs build, and any guest thread that runs takes precedence.
    if (index == 1) {
        pin_to_core(0, "shader-compile worker 1");
        const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);   // lowest application priority
        printf("[NX] shader-compile worker 1 priority 0x3B (rc 0x%X)\n", rc);
    } else {
        // Core 2 at 0x2D: below Mesa's glthread worker (0x2C) and the audio output thread (0x2B) there.
        pin_to_core(2, "shader-compile worker");
        const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2D);
        printf("[NX] shader-compile worker 0 priority 0x2D (rc 0x%X)\n", rc);
    }
    if (!eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, g_worker_ctx[index])) {
        printf("[GLPROG] worker %d: eglMakeCurrent failed (0x%X)\n", index, eglGetError());
        fflush(stdout);
        return;
    }
    for (;;) {
        std::pair<uint64_t, ProgSources> job;
        {
            std::unique_lock<std::mutex> lk(g_prog_mtx);
            g_prog_cv.wait(lk, [] { return !g_prog_queue.empty(); });
            job = std::move(g_prog_queue.front());
            g_prog_queue.pop_front();
            auto it = g_prog_entries.find(job.first);
            if (it != g_prog_entries.end() && it->second.state != kProgQueued) continue;   // duplicate
        }
        const uint64_t t0 = gp_now();
        static const bool steps = env_on("LSWTCS_GLSTEPS", false);   // DIAG: log each worker step
        if (steps) { printf("[GLSTEP] w%d %016llX build (VS %zu PS %zu GS %zu B)\n", index, (unsigned long long)job.first,
                            job.second.vs.size(), job.second.ps.size(), job.second.gs.size()); fflush(stdout); }
        bool from_binary = false, written = false;
        GLuint prog = g_binary_cache_on ? program_binary_load(job.first) : 0;
        if (prog) from_binary = true;
        else {
            prog = program_build(job.first, job.second);
            if (prog && g_binary_cache_on) written = program_binary_store(job.first, prog);
        }
        if (steps) { printf("[GLSTEP] w%d %016llX built (prog %u) in %llu ms\n", index, (unsigned long long)job.first,
                            prog, (unsigned long long)((gp_now() - t0) / 1000000)); fflush(stdout); }
        // NO glFinish / glFlush here: switch-mesa's nouveau contexts share one GPU channel without
        // locking, so a submission from this context interleaves with the render thread's command
        // stream (Eden: "maxwell_3d ... executing_macro" asserts, then the render thread hangs).
        // Compile and link are CPU-only; the program reaches the render context through the shared
        // namespace once published under g_prog_mtx below.
        const uint64_t dt = gp_now() - t0;
        std::lock_guard<std::mutex> lk(g_prog_mtx);
        ProgEntry& e = g_prog_entries[job.first];
        e.state = prog ? kProgReady : kProgFailed;
        e.prog = prog;
        if (from_binary) ++g_prog_from_binary;
        else { ++g_prog_compiled; g_prog_compile_ns += dt; }
        if (written) ++g_prog_cache_written;
        g_prog_done_cv.notify_all();
    }
}

// Render thread, LSWTCS_GLWAIT: build queued programs here too until every program in g_frame_wait is
// settled (the workers take from the same queue; whichever is free takes the next one).
void wait_frame_programs() {
    auto pending = [] {
        for (uint64_t k : g_frame_wait) {
            auto it = g_prog_entries.find(k);
            if (it != g_prog_entries.end() && it->second.state == kProgQueued) return true;
        }
        return false;
    };
    std::unique_lock<std::mutex> lk(g_prog_mtx);
    if (pending()) host_audio_hold(true);   // the game's audio waits with the picture (audio_switch.cpp)
    while (pending()) {
        if (!g_prog_queue.empty()) {
            auto job = std::move(g_prog_queue.front());
            g_prog_queue.pop_front();
            auto it = g_prog_entries.find(job.first);
            if (it != g_prog_entries.end() && it->second.state != kProgQueued) continue;   // duplicate
            lk.unlock();
            const uint64_t t0 = gp_now();
            const GLuint prog = program_build(job.first, job.second);
            const uint64_t dt = gp_now() - t0;
            lk.lock();
            ProgEntry& e = g_prog_entries[job.first];
            e.state = prog ? kProgReady : kProgFailed;
            e.prog = prog;
            ++g_prog_compiled;
            g_prog_compile_ns += dt;
        } else {
            g_prog_done_cv.wait(lk);   // the rest is being built by workers
        }
    }
    host_audio_hold(false);
    g_frame_wait.clear();
}

// Boot: binary-cache detection, then queue every program saved by earlier sessions and start workers.
void program_cache_init() {
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    const char* e = getenv("LSWTCS_GLCACHE");
    // Opt-in (LSWTCS_GLCACHE=1). Off by default: shader work is made cheaper, not cached (user rule), and
    // with the rebuilt Mesa reporting a binary format the workers' glGetProgramBinary path went live on
    // 2026-10-08: programs built on workers took ~2 s each (vs ~0.2 s) and the title rendered as garbage.
    g_binary_cache_on = formats > 0 && e && e[0] == '1';
    const std::string drv = std::string((const char*)glGetString(GL_RENDERER)) + "|" + (const char*)glGetString(GL_VERSION);
    g_driver_hash = XXH3_64bits(drv.data(), drv.size());
    if (g_binary_cache_on) mkdir("glcache", 0777);
    if (prog_list_on()) mkdir(LSW_PROGLIST_DIR, 0777);
    uint32_t queued = 0;
    if (g_num_workers > 0 && prog_list_on())
        if (DIR* d = opendir(LSW_PROGLIST_DIR)) {
            while (dirent* de = readdir(d)) {
                const char* n = de->d_name;
                if (strlen(n) != 22 || strcmp(n + 16, ".glsl3") != 0) continue;
                char path[64];
                snprintf(path, sizeof path, LSW_PROGLIST_DIR "/%s", n);
                ProgSources s;
                if (!program_list_read(path, s)) { remove(path); continue; }
                const uint64_t ckey = program_cache_key(s);   // driver changed -> different key: still compiled
                std::lock_guard<std::mutex> lk(g_prog_mtx);
                if (g_prog_entries.count(ckey)) continue;
                g_prog_entries[ckey] = ProgEntry{};
                g_prog_queue.emplace_back(ckey, std::move(s));
                ++queued;
            }
            closedir(d);
        }
    g_prog_boot_queued = queued;
    for (int i = 0; i < g_num_workers; ++i) std::thread(compile_worker_main, i).detach();
    g_prog_cv.notify_all();
    printf("[GLPROG] %d compile workers; %u saved programs queued for background compile; binary cache %s (%d formats)\n",
           g_num_workers, queued, g_binary_cache_on ? "ON" : "off", formats);
    fflush(stdout);
}

// DIAG (LSWTCS_GLDBG): 1 = depth/stencil/blend/cull/scissor off and all colour writes on; 2 = also
// every pixel shader replaced by solid magenta. Splits "geometry never reaches the screen" from
// "state or pixel shader rejects it".
int gl_dbg() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("LSWTCS_GLDBG"); v = e ? atoi(e) : 0; }
    return v;
}

struct ProgKey {
    const void *vs, *ps, *gs;
    bool operator==(const ProgKey& o) const { return vs == o.vs && ps == o.ps && gs == o.gs; }
};
struct ProgKeyHash {
    size_t operator()(const ProgKey& k) const {
        return std::hash<const void*>()(k.vs) ^ (std::hash<const void*>()(k.ps) * 31) ^ (std::hash<const void*>()(k.gs) * 131);
    }
};
std::unordered_map<ProgKey, GLuint, ProgKeyHash> g_programs;   // render thread: settled programs
std::unordered_map<ProgKey, uint64_t, ProgKeyHash> g_prog_ckeys;   // render thread: content key per shader set

// Program for a draw, or 0 if it is not ready (still compiling: the draw is skipped this frame) or
// failed to build.
GLuint program(const LswGlShader* vs, const LswGlShader* ps, const LswGlShader* gs) {
    // DIAG modes 3-5 (state unchanged, alpha forced to 1): 3 = texture unit 0 in screen space,
    // 4 = interpolator 0 rgb, 5 = interpolator 0 alpha as grey. Only draws that have a pixel shader.
    if (gl_dbg() >= 3 && ps) {
        static LswGlShader dbg[3] = {};
        static const char* const kSrc[3] = {
            "#version 430\nlayout(location = 0) out vec4 o;\nlayout(binding = 0) uniform sampler2DArray t;\n"
            "void main() { o = vec4(texture(t, vec3(gl_FragCoord.xy / 256.0, 0.0)).rgb, 1.0); }\n",
            "#version 430\nlayout(location = 0) out vec4 o;\nlayout(location = 0) in vec4 i0;\n"
            "void main() { o = vec4(i0.rgb, 1.0); }\n",
            "#version 430\nlayout(location = 0) out vec4 o;\nlayout(location = 0) in vec4 i0;\n"
            "void main() { o = vec4(vec3(i0.a), 1.0); }\n"};
        const int m = std::min(gl_dbg(), 5) - 3;
        if (dbg[m].glsl.empty()) { dbg[m].stage = kLswGlPixel; dbg[m].key = 0xDB0 + m; dbg[m].glsl = kSrc[m]; }
        ps = &dbg[m];
    }
    if (gl_dbg() == 2) {   // magenta pixel shader instead of the game's
        static LswGlShader magenta = [] {
            LswGlShader s{};
            s.stage = kLswGlPixel;
            s.key = 0xDB9;
            s.glsl = "#version 430\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(1.0, 0.0, 1.0, 1.0); }\n";
            return s;
        }();
        ps = &magenta;
    }
    const ProgKey key{vs, ps, gs};
    auto it = g_programs.find(key);
    if (it != g_programs.end()) return it->second;
    // Content key (hashing ~100 KB of GLSL once per shader set, not per draw).
    uint64_t ckey;
    auto ck = g_prog_ckeys.find(key);
    if (ck != g_prog_ckeys.end()) {
        ckey = ck->second;
    } else {
        ProgSources s{vs->glsl, ps ? ps->glsl : std::string(), gs ? gs->glsl : std::string()};
        ckey = program_cache_key(s);
        g_prog_ckeys.emplace(key, ckey);
        if (g_num_workers == 0) {   // synchronous: build right here (old behaviour)
            const uint64_t t0 = gp_now();
            const GLuint prog = program_build(ckey, s);
            std::lock_guard<std::mutex> lk(g_prog_mtx);
            ++g_prog_compiled;
            g_prog_compile_ns += gp_now() - t0;
            g_programs.emplace(key, prog);
            return prog;
        }
        if (prog_list_on()) program_list_save(ckey, s);
        std::lock_guard<std::mutex> lk(g_prog_mtx);
        ProgEntry& e = g_prog_entries[ckey];
        if (e.state == kProgQueued && !e.requested) {
            // Wanted now: queue it at the front (a boot-queued copy further back becomes a duplicate).
            e.requested = true;
            g_prog_queue.emplace_front(ckey, std::move(s));
            g_prog_cv.notify_one();
        }
    }
    std::lock_guard<std::mutex> lk(g_prog_mtx);
    ProgEntry& e = g_prog_entries[ckey];
    if (e.state == kProgQueued) {
        if (!e.requested) {   // boot-queued but not yet asked for: move it to the front
            e.requested = true;
            for (auto q = g_prog_queue.begin(); q != g_prog_queue.end(); ++q)
                if (q->first == ckey) {
                    auto job = std::move(*q);
                    g_prog_queue.erase(q);
                    g_prog_queue.emplace_front(std::move(job));
                    break;
                }
            g_prog_cv.notify_one();
        }
        ++g_prog_pending_draws;
        g_frame_wait.push_back(ckey);
        return 0;
    }
    g_programs.emplace(key, e.prog);   // ready or failed: settled
    return e.prog;
}

// ---- DIAG: render-thread profile ----------------------------------------------------------------
// Leaf stages of a replay (ns, render thread only). [GLSTALL] prints the split of one stalled replay,
// [GLPROF] the average per replay every 60 replays. Draw calls include Mesa's validation, so a new
// program's backend compile (nouveau compiles at first draw) lands in RP_DRAW.
enum { RP_UPLOAD, RP_SHM, RP_FWAIT, RP_PROGRAM, RP_TEXSRC, RP_TEXKEY, RP_DECODE, RP_TEXUP, RP_SAMPLER, RP_RT, RP_BIND, RP_STATE,
       RP_DRAW, RP_RESOLVE, RP_PRESENT, RP_SWAP, RP_EVICT, RP_GPUWAIT, RP_PIPE, RP_COUNT };
const char* const kRpName[RP_COUNT] = {"upload", "shm", "fwait", "program", "texsrc", "texkey", "decode", "texup", "sampler", "rt",
                                       "bind", "state", "draw", "resolve", "present", "swap", "evict", "gpuwait", "pipewait"};
uint64_t g_rp[RP_COUNT];

// DIAG (LSWTCS_GLGPUPROF=N): every Nth replay, a GPU timestamp after every draw and resolve (and after the
// present blit); the GPU time between two timestamps is charged to the later item. Printed as [GLGPU]:
// totals by kind and the programs that cost the most GPU time. Approximate (the GPU overlaps
// neighbouring draws) and texture-source blits count toward the next draw. Ends with a glFinish, so
// only the sampled frame is slowed. For the Switch, where the GPU took 60-80 ms per hub frame.
extern uint64_t g_replays;   // defined with the texture cache below
enum { GK_START, GK_PLAIN, GK_POINTS, GK_RECTS, GK_QUADS, GK_RESOLVE, GK_PRESENT, GK_N };
struct GpuStamp { uint8_t kind; GLuint prog; uint64_t ps_key; };
bool g_gpuprof_frame = false;
// DIAG (LSWTCS_GLGPUSPLIT=1, with LSWTCS_GLGPUPROF): profiled replays cycle normal / "PIXELS OFF" (every draw with a
// 1x1 scissor: almost no pixels shaded) / "PIXELS+VERTS OFF" (also at most 6 vertices per draw). Normal minus pixels-off
// = pixel cost; pixels-off minus pixels+verts-off = vertex cost; the rest = fixed per-draw cost (state, driver).
// Hardware 2026-10-09: pixels-off cost ~7 us per plain draw for every program. Those replays' pictures (and resolves,
// which the scissor also clips) are wrong for one frame. [GLGPU] also reports the vertices (or indices) submitted.
bool g_gpuprof_nopix = false, g_gpuprof_novtx = false;
uint64_t g_gpuprof_verts = 0;
std::vector<GLuint> g_gpq_pool;
std::vector<GpuStamp> g_gpq_labels;
void gpu_stamp(uint8_t kind, GLuint prog = 0, uint64_t ps_key = 0) {
    if (!g_gpuprof_frame) return;
    const size_t i = g_gpq_labels.size();
    if (i >= g_gpq_pool.size()) {
        const size_t add = std::max<size_t>(256, g_gpq_pool.size());
        g_gpq_pool.resize(g_gpq_pool.size() + add);
        glGenQueries(GLsizei(add), g_gpq_pool.data() + g_gpq_pool.size() - add);
    }
    glQueryCounter(g_gpq_pool[i], GL_TIMESTAMP);
    g_gpq_labels.push_back({kind, prog, ps_key});
}
void gpu_prof_finish() {
    if (!g_gpuprof_frame) return;
    gpu_stamp(GK_PRESENT);
    g_gpuprof_frame = false;
    glFinish();
    const size_t n = g_gpq_labels.size();
    std::vector<GLuint64> t(n);
    for (size_t i = 0; i < n; ++i) glGetQueryObjectui64v(g_gpq_pool[i], GL_QUERY_RESULT, &t[i]);
    uint64_t by_kind[GK_N] = {}, cnt[GK_N] = {};
    struct ProgCost { uint64_t ns = 0; uint32_t draws = 0; uint64_t ps_key = 0; };
    std::unordered_map<GLuint, ProgCost> by_prog;
    for (size_t i = 1; i < n; ++i) {
        const uint64_t d = t[i] > t[i - 1] ? t[i] - t[i - 1] : 0;
        const GpuStamp& s = g_gpq_labels[i];
        by_kind[s.kind] += d;
        ++cnt[s.kind];
        if (s.prog) { ProgCost& c = by_prog[s.prog]; c.ns += d; ++c.draws; c.ps_key = s.ps_key; }
    }
    const uint64_t total = n > 1 && t[n - 1] > t[0] ? t[n - 1] - t[0] : 0;
    printf("[GLGPU] replay %llu%s: GPU %.2f ms, %llu verts | plain %.2f (%llu) points %.2f (%llu) rects %.2f (%llu) quads %.2f (%llu) "
           "resolves %.2f (%llu) present %.2f\n", (unsigned long long)g_replays, g_gpuprof_novtx ? " PIXELS+VERTS OFF" : g_gpuprof_nopix ? " PIXELS OFF" : "",
           total / 1e6, (unsigned long long)g_gpuprof_verts,
           by_kind[GK_PLAIN] / 1e6, (unsigned long long)cnt[GK_PLAIN], by_kind[GK_POINTS] / 1e6, (unsigned long long)cnt[GK_POINTS],
           by_kind[GK_RECTS] / 1e6, (unsigned long long)cnt[GK_RECTS], by_kind[GK_QUADS] / 1e6, (unsigned long long)cnt[GK_QUADS],
           by_kind[GK_RESOLVE] / 1e6, (unsigned long long)cnt[GK_RESOLVE], by_kind[GK_PRESENT] / 1e6);
    std::vector<std::pair<GLuint, ProgCost>> top(by_prog.begin(), by_prog.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    for (size_t i = 0; i < top.size() && i < 10; ++i)
        printf("[GLGPU]   program %u (PS %016llX): %.2f ms over %u draws\n", top[i].first,
               (unsigned long long)top[i].second.ps_key, top[i].second.ns / 1e6, top[i].second.draws);
    fflush(stdout);
    g_gpq_labels.clear();
}
struct RpScope {
    int k; uint64_t t0;
    explicit RpScope(int stage) : k(stage), t0(gp_now()) {}
    ~RpScope() { g_rp[k] += gp_now() - t0; }
};

// Bumped by everything outside the draw path that changes GL state or deletes objects (blits, clears,
// texture / render-target / framebuffer creation and deletion, arena reallocation, each new replay):
// the draw loop's redundant-state cache (GlStateCache) starts over when it changes. Deleted names can be
// reused by GL, so deletion must bump it too.
uint64_t g_gl_epoch = 1;

// ---- textures / samplers ----------------------------------------------------------------------
struct GlTex { GLuint tex = 0; GLenum target = 0; uint32_t bytes = 0; uint64_t last_used = 0; };
std::unordered_map<uint64_t, GlTex> g_textures;
uint32_t g_tex_created = 0, g_tex_evicted = 0;
uint64_t g_tex_bytes = 0;   // GPU memory held by g_textures
uint64_t g_replays = 0;

bool gl_format(uint32_t dxgi, GLenum& internal, GLenum& format, GLenum& type, bool& compressed) {
    compressed = false;
    switch (dxgi) {
        case 28: internal = GL_RGBA8; format = GL_RGBA; type = GL_UNSIGNED_BYTE; return true;
        case 49: internal = GL_RG8; format = GL_RG; type = GL_UNSIGNED_BYTE; return true;
        case 61: internal = GL_R8; format = GL_RED; type = GL_UNSIGNED_BYTE; return true;
        case 71: internal = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; compressed = true; return true;
        case 74: internal = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; compressed = true; return true;
        case 77: internal = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; compressed = true; return true;
        case 83: internal = GL_COMPRESSED_RG_RGTC2; compressed = true; return true;
        // B-first DXGI formats keep blue in the low bits; GL's packed types below do the same.
        case 85: internal = GL_RGB565; format = GL_RGB; type = GL_UNSIGNED_SHORT_5_6_5; return true;
        case 86: internal = GL_RGB5_A1; format = GL_BGRA; type = GL_UNSIGNED_SHORT_1_5_5_5_REV; return true;
        case 115: internal = GL_RGBA4; format = GL_BGRA; type = GL_UNSIGNED_SHORT_4_4_4_4_REV; return true;
        default: return false;
    }
}

GlTex create_texture(const LswGpuTexDesc& d) {
    GlTex t;
    GLenum internal, format = 0, type = 0; bool compressed;
    if (!gl_format(d.dxgi, internal, format, type, compressed)) return t;
    const bool cube = d.dim == 2 && d.layers == 6;
    t.target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D_ARRAY;
    const GLsizei mips = GLsizei(d.mips ? d.mips : 1);
    ++g_gl_epoch;   // binds on the active unit
    glGenTextures(1, &t.tex);
    glBindTexture(t.target, t.tex);
    if (cube) glTexStorage2D(t.target, mips, internal, d.width, d.height);
    else glTexStorage3D(t.target, mips, internal, d.width, d.height, d.layers);
    for (uint32_t l = 0; l < d.layers; ++l)
        for (GLsizei m = 0; m < mips; ++m) {
            const uint8_t* src = d.data + size_t(l) * d.layer_bytes + d.mip_offset[m];
            const GLsizei w = GLsizei(std::max(d.width >> m, 1u)), h = GLsizei(std::max(d.height >> m, 1u));
            const GLsizei bytes = GLsizei(d.mip_row_bytes[m] * d.mip_rows[m]);
            if (cube) {
                const GLenum face = GL_TEXTURE_CUBE_MAP_POSITIVE_X + l;
                if (compressed) glCompressedTexSubImage2D(face, m, 0, 0, w, h, internal, bytes, src);
                else glTexSubImage2D(face, m, 0, 0, w, h, format, type, src);
            } else {
                if (compressed) glCompressedTexSubImage3D(t.target, m, 0, 0, GLint(l), w, h, 1, internal, bytes, src);
                else glTexSubImage3D(t.target, m, 0, 0, GLint(l), w, h, 1, format, type, src);
            }
        }
    t.bytes = d.layer_bytes * d.layers;
    if (++g_tex_created <= 40) {
        printf("[GL] texture #%u %ux%u x%u mips %u dxgi %u %s (err 0x%X)\n", g_tex_created, d.width, d.height, d.layers,
               d.mips, d.dxgi, cube ? "cube" : "2d-array", glGetError());
        fflush(stdout);
    }
    return t;
}

// Fetch constant -> texture; memoised per replay (lsw_gpu_texture_key probes texture memory).
struct FetchKey { uint32_t w[6]; bool operator==(const FetchKey& o) const { return !memcmp(w, o.w, sizeof w); } };
struct FetchKeyHash {
    size_t operator()(const FetchKey& k) const {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t v : k.w) h = (h ^ v) * 1099511628211ull;
        return size_t(h);
    }
};
std::unordered_map<FetchKey, GlTex, FetchKeyHash> g_tex_memo;

GlTex get_texture(const uint32_t* fetch6) {
    FetchKey fk; memcpy(fk.w, fetch6, sizeof fk.w);
    auto mi = g_tex_memo.find(fk);
    if (mi != g_tex_memo.end()) return mi->second;
    const uint8_t* phys = g_base + 0x80000000ull;
    uint64_t key;
    { RpScope rp(RP_TEXKEY); key = lsw_gpu_texture_key(fetch6, phys); }
    GlTex t;
    auto it = g_textures.find(key);
    if (it != g_textures.end()) {
        it->second.last_used = g_replays;
        t = it->second;
    } else {
        LswGpuTexDesc d;
        bool ok;
        { RpScope rp(RP_DECODE); ok = lsw_gpu_decode_texture(fetch6, phys, &d) && d.ok; }
        if (ok) { RpScope rp(RP_TEXUP); t = create_texture(d); }
        t.last_used = g_replays;
        g_tex_bytes += t.bytes;
        g_textures.emplace(key, t);
    }
    g_tex_memo.emplace(fk, t);
    return t;
}

// Texture memory budget (LSWTCS_GLTEXMB, default 192 MB): past it, textures not bound in the last
// two replays are deleted, least recently used first, down to 3/4 of the budget. Unbounded caching ran
// Eden out of GPU address space when the hub loaded ("Failed to allocate free space in the GPU AS").
void evict_textures() {
    static const uint64_t budget = [] {
        const char* e = getenv("LSWTCS_GLTEXMB");
        return uint64_t(e ? atoi(e) : 192) << 20;
    }();
    if (g_tex_bytes <= budget) return;
    std::vector<std::pair<uint64_t, uint64_t>> old;   // (last_used, key)
    for (const auto& kv : g_textures)
        if (kv.second.last_used + 2 < g_replays) old.emplace_back(kv.second.last_used, kv.first);
    std::sort(old.begin(), old.end());
    for (const auto& o : old) {
        if (g_tex_bytes <= budget * 3 / 4) break;
        auto it = g_textures.find(o.second);
        if (it->second.tex) { ++g_gl_epoch; glDeleteTextures(1, &it->second.tex); }
        g_tex_bytes -= it->second.bytes;
        g_textures.erase(it);
        ++g_tex_evicted;
    }
}

std::unordered_map<std::string, GLuint> g_samplers;

GLuint get_sampler(const uint32_t* fetch6, const LswGpuSamplerBinding& b) {
    RpScope rp(RP_SAMPLER);
    LswGpuSamplerDesc s;
    lsw_gpu_sampler(fetch6, &b, &s);
    std::string key(reinterpret_cast<const char*>(&s), sizeof s);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    GLuint smp;
    glGenSamplers(1, &smp);
    glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, s.mag_linear ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER,
                        s.min_linear ? (s.mip_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                                     : (s.mip_linear ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST));
    auto addr = [](uint32_t d3d) -> GLint {   // D3D12_TEXTURE_ADDRESS_MODE
        switch (d3d) {
            case 1: return GL_REPEAT; case 2: return GL_MIRRORED_REPEAT; case 3: return GL_CLAMP_TO_EDGE;
            case 4: return GL_CLAMP_TO_BORDER; default: return GL_MIRROR_CLAMP_TO_EDGE;
        }
    };
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_S, addr(s.address_u));
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_T, addr(s.address_v));
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_R, addr(s.address_w));
    glSamplerParameterfv(smp, GL_TEXTURE_BORDER_COLOR, s.border);
    if (s.max_aniso > 1) glSamplerParameterf(smp, GL_TEXTURE_MAX_ANISOTROPY_EXT, float(s.max_aniso));
    g_samplers.emplace(std::move(key), smp);
    return smp;
}

// ---- EDRAM render targets / resolves (port of gpu_xedraw.inc) -----------------------------------
#include "gl_rt.inc"

// DIAG: the frame being captured (shot_now / LSWTCS_GLTEST replays 300 and 1500) logs every draw.
bool g_capture_frame = false;
std::string g_unit_log;   // texture units of the current draw (capture frame only)

// One texture unit of a draw, resolved before any state is bound (texture sources may blit).
// swz: Xenia's 12-bit host swizzle (3 bits per component: 0-3 = R/G/B/A, 4 = 0, 5 = 1), applied as GL
// texture swizzle (the translator's image_view_format_swizzle, gl_bridge.cc) instead of in the shader.
struct UnitBind { uint32_t unit; GLenum target; GLuint tex; GLuint sampler; uint32_t swz; };

// LSWTCS_GLVIEWSWZ=0: swizzles in the shader again (must match gl_bridge.cc).
bool view_swizzle_on() {
    static const bool on = env_on("LSWTCS_GLVIEWSWZ", true);
    return on;
}

void apply_swizzle(GLenum target, uint32_t swz) {
    GLint s[4];
    for (int c = 0; c < 4; ++c) {
        const uint32_t v = (swz >> (3 * c)) & 7;
        s[c] = v < 4 ? GLint(GL_RED + v) : (v & 1) ? GL_ONE : GL_ZERO;
    }
    // Mesa ignores unchanged values, so setting it on every bind costs only the call.
    glTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, s);
}
uint32_t g_res_tex_binds = 0;

// Texture units of a shader: resolved EDRAM data (gl_rt.inc texture_source) when the fetch points at
// it, else the decoded guest texture. `swz` collects swizzle overrides for resolved sources
// (fetch -> 12-bit mapping, written into a per-draw copy of the system constants).
void collect_units(const LswGlShader* s, const uint32_t* fetch_all, bool gpurt, std::vector<UnitBind>& out,
                   std::vector<std::pair<uint32_t, uint32_t>>& swz) {
    if (!s) return;
    static const bool notex = env_on("LSWTCS_GPUDRAW_NOTEX", false);
    for (const LswGlShaderUnit& u : s->units) {
        const uint32_t* f6 = fetch_all + (u.fetch_constant & 31) * 6;
        const GLenum want = u.dimension == 3 ? GL_TEXTURE_CUBE_MAP : u.dimension == 2 ? GL_TEXTURE_3D : GL_TEXTURE_2D_ARRAY;
        uint32_t base = 0, fmt = 0, gswz = 0, w = 0, h = 0, dim = 0;
        const int info = lsw_gpu_texture_info(f6, &base, &fmt, &gswz, &w, &h, &dim);
        const char* how = "";
        GLuint tex = 0;
        uint32_t map = lsw_gpu_texture_mapping(f6);
        if (gpurt && !notex && info && u.dimension <= 1) {
            const HostTex* src = nullptr;
            bool found, rswap = false;
            { RpScope rp(RP_TEXSRC); found = texture_source(base & 0x1FFFFFFFu, fmt, w, h, src, &rswap); }
            if (found && src && src->tex) {
                tex = src->tex;
                map = resolved_swizzle(gswz, fmt, src->fmt, rswap);
                swz.emplace_back(u.fetch_constant & 31, map);
                ++g_res_tex_binds;
                how = " RESOLVED";
            }
        }
        if (!tex) {
            GlTex t = notex ? GlTex{} : get_texture(f6);
            if (t.target == want) tex = t.tex;
            else how = !info ? " NOT-TEXTURE" : t.tex ? " DIM-MISMATCH" : " DUMMY";
        }
        if (!tex) tex = g_dummy[want == GL_TEXTURE_CUBE_MAP ? 1 : want == GL_TEXTURE_3D ? 2 : 0];
        if (g_capture_frame) {
            char b[160];
            snprintf(b, sizeof b, " u%u:f%u%s fmt%u %ux%u dim%u/%u swz%03o base%08X%s", u.unit, u.fetch_constant,
                     u.is_signed ? "s" : "", fmt, w, h, dim, u.dimension, gswz, base, how);
            g_unit_log += b;
        }
        out.push_back({u.unit, want, tex, get_sampler(f6, u.sampler), map & 0xFFFu});
    }
}

// ---- fixed function ---------------------------------------------------------------------------
GLenum blend_factor(uint32_t f) {
    static const GLenum m[17] = {GL_ZERO, GL_ONE, GL_ZERO, GL_ZERO, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                                 GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                                 GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_CONSTANT_COLOR,
                                 GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA,
                                 GL_SRC_ALPHA_SATURATE};
    return f < 17 ? m[f] : GL_ONE;
}
GLenum blend_op(uint32_t o) {
    static const GLenum m[5] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_REVERSE_SUBTRACT};
    return o < 5 ? m[o] : GL_FUNC_ADD;
}
const GLenum kStencilOp[8] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};

// Render-target geometry of a draw: msx/msy = MSAA sample-space scale, rt_w/rt_h = target size.
struct DrawTarget { uint32_t nrt = 1, msx = 1, msy = 1, rt_w = kSceneW, rt_h = kSceneH; };

void apply_state(const LswGpuDrawRec& r, const DrawTarget& dt) {
    // Blend and colour mask per render target (RB_BLENDCONTROL0..3, RB_COLOR_MASK nibbles).
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t bc = i == 0 ? r.blendcontrol : r.blend_mrt[i];
        const uint32_t cs = bc & 0x1F, cop = (bc >> 5) & 7, cd = (bc >> 8) & 0x1F;
        const uint32_t as = (bc >> 16) & 0x1F, aop = (bc >> 21) & 7, ad = (bc >> 24) & 0x1F;
        if (i >= dt.nrt || (cs == 1 && cd == 0 && cop == 0 && as == 1 && ad == 0 && aop == 0)) {
            glDisablei(GL_BLEND, i);
        } else {
            glEnablei(GL_BLEND, i);
            glBlendFuncSeparatei(i, blend_factor(cs), blend_factor(cd), blend_factor(as), blend_factor(ad));
            glBlendEquationSeparatei(i, blend_op(cop), blend_op(aop));
        }
        uint32_t cm = r.ps_dxbc ? ((r.color_mask >> (4 * i)) & 0xF) : 0;
        if (i > 0 && !(r.ps_rt_mask & (1u << i))) cm = 0;
        if (i >= dt.nrt) cm = 0;
        glColorMaski(i, cm & 1, (cm >> 1) & 1, (cm >> 2) & 1, (cm >> 3) & 1);
    }
    // Depth (RB_DEPTHCONTROL: bit1 enable, bit2 write, 4-6 func; Xenos and GL compare orders match).
    const bool zen = (r.depthcontrol >> 1) & 1, zwr = (r.depthcontrol >> 2) & 1;
    if (zen || zwr) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(zen ? GL_NEVER + ((r.depthcontrol >> 4) & 7) : GL_ALWAYS);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(zwr ? GL_TRUE : GL_FALSE);
    // Stencil (bit0 enable, bit7 separate back face; front 8-19, back 20-31).
    if (r.depthcontrol & 1) {
        const uint32_t dc = r.depthcontrol;
        const GLint ref = GLint(r.stencil_ref & 0xFF);
        const GLuint rmask = (r.stencil_ref >> 8) & 0xFF, wmask = (r.stencil_ref >> 16) & 0xFF;
        glEnable(GL_STENCIL_TEST);
        auto face = [&](GLenum f, uint32_t sh) {
            glStencilFuncSeparate(f, GL_NEVER + ((dc >> sh) & 7), ref, rmask);
            glStencilOpSeparate(f, kStencilOp[(dc >> (sh + 3)) & 7], kStencilOp[(dc >> (sh + 9)) & 7],
                                kStencilOp[(dc >> (sh + 6)) & 7]);
            glStencilMaskSeparate(f, wmask);
        };
        face(GL_FRONT, 8);
        face(GL_BACK, (dc & 0x80) ? 20 : 8);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    // Cull (PA_SU_SC_MODE_CNTL: bit0 cull front, bit1 cull back, bit2 1 = clockwise is front). With
    // glClipControl(GL_UPPER_LEFT) GL evaluates winding as D3D does, so the guest's orientation maps
    // directly (verified in Eden 2026-10-07: the mirrored mapping culled every single-sided 2D element of
    // the title). LSWTCS_GLFACE=1 swaps it back (diagnostic).
    const uint32_t cull = r.sc_mode_cntl & 3;
    if (cull) {
        glEnable(GL_CULL_FACE);
        glCullFace(cull == 1 ? GL_FRONT : cull == 2 ? GL_BACK : GL_FRONT_AND_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }
    static const bool swap_face = env_on("LSWTCS_GLFACE", false);
    const bool cw_front = (r.sc_mode_cntl & 4) != 0;
    glFrontFace((cw_front != swap_face) ? GL_CW : GL_CCW);
    // Viewport / scissor / depth range in sample space (rows top-down).
    float vx = r.viewport[0] * dt.msx, vy = r.viewport[1] * dt.msy, vw = r.viewport[2] * dt.msx, vh = r.viewport[3] * dt.msy;
    if (vw <= 0 || vh <= 0) { vx = 0; vy = 0; vw = float(dt.rt_w); vh = float(dt.rt_h); }
    glViewport(GLint(vx), GLint(vy), GLsizei(vw), GLsizei(vh));
    glDepthRangef(r.viewport[4], r.viewport[5]);
    glEnable(GL_SCISSOR_TEST);
    if (r.scissor[2] && r.scissor[3])
        glScissor(GLint(r.scissor[0] * dt.msx), GLint(r.scissor[1] * dt.msy), GLsizei(r.scissor[2] * dt.msx), GLsizei(r.scissor[3] * dt.msy));
    else
        glScissor(0, 0, GLsizei(dt.rt_w), GLsizei(dt.rt_h));
    if (gl_dbg() == 1 || gl_dbg() == 2) {
        glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
        for (uint32_t i = 0; i < 4; ++i) { glDisablei(GL_BLEND, i); glColorMaski(i, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); }
        glScissor(0, 0, GLsizei(dt.rt_w), GLsizei(dt.rt_h));
    }
}

// Redundant-state cache of the draw loop (the PC's skip of unchanged root bindings, f7032ea / 8c092ce):
// calls whose value is the same as at the previous draw are skipped. Mesa marks state dirty on most of
// them even when nothing changes, and each dirty bit costs revalidation at the next draw. Everything is
// forgotten when g_gl_epoch moves (blits, clears, object creation / deletion). LSWTCS_GLSTATECACHE=0: off.
struct StateKey {   // every input of apply_state
    uint32_t blend[4], color_mask, has_ps, ps_rt_mask, depthcontrol, stencil_ref, sc_mode_cntl, dbg;
    float viewport[6];
    uint32_t scissor[4];
    DrawTarget dt;
};
// DIAG: GL state changes actually issued per replay ([GLPROF] "changes"): program, framebuffer, UBO
// range, texture, swizzle, sampler, fixed-function state block, state-cache resets.
enum { SC_PROG, SC_FBO, SC_UBO, SC_TEX, SC_SWZ, SC_SMP, SC_STATE, SC_RESET, SC_N };
uint64_t g_sc_changes[SC_N];
struct GlStateCache {
    static constexpr int kUnits = 32;
    uint64_t epoch = 0;
    GLuint prog = 0, fbo = 0;
    struct Ubo { GLuint buf; GLintptr off; GLsizeiptr size; } ubo[6];
    GLuint tex[kUnits][3], smp[kUnits];
    GLint active = -1;
    std::unordered_map<GLuint, uint32_t> swz;   // texture -> swizzle last set on it
    bool key_valid = false;
    StateKey key;
    void sync() {
        static const bool on = env_on("LSWTCS_GLSTATECACHE", true);
        if (on && epoch == g_gl_epoch) return;
        ++g_sc_changes[SC_RESET];
        epoch = g_gl_epoch;
        prog = fbo = ~0u;
        for (Ubo& u : ubo) u = {~0u, -1, -1};
        memset(tex, 0xFF, sizeof tex);
        memset(smp, 0xFF, sizeof smp);
        active = -1;
        swz.clear();
        key_valid = false;
    }
    void use_program(GLuint p) { if (p != prog) { glUseProgram(p); prog = p; ++g_sc_changes[SC_PROG]; } }
    void bind_fbo(GLuint f) { if (f != fbo) { glBindFramebuffer(GL_FRAMEBUFFER, f); fbo = f; ++g_sc_changes[SC_FBO]; } }
    void bind_ubo(GLuint i, GLuint b, GLintptr off, GLsizeiptr size) {
        Ubo& u = ubo[i];
        if (u.buf == b && u.off == off && u.size == size) return;
        glBindBufferRange(GL_UNIFORM_BUFFER, i, b, off, size);
        u = {b, off, size};
        ++g_sc_changes[SC_UBO];
    }
    void bind_unit(GLuint unit, GLenum target, GLuint t, GLuint sampler, bool set_swz, uint32_t s) {
        const int ti = target == GL_TEXTURE_CUBE_MAP ? 1 : target == GL_TEXTURE_3D ? 2 : 0;
        const bool tracked = unit < GLuint(kUnits);
        auto activate = [&] { if (active != GLint(unit)) { glActiveTexture(GL_TEXTURE0 + unit); active = GLint(unit); } };
        if (!tracked || tex[unit][ti] != t) { activate(); glBindTexture(target, t); if (tracked) tex[unit][ti] = t; ++g_sc_changes[SC_TEX]; }
        if (set_swz) {
            auto it = swz.find(t);
            if (it == swz.end() || it->second != s) { activate(); apply_swizzle(target, s); swz[t] = s; ++g_sc_changes[SC_SWZ]; }
        }
        if (!tracked || smp[unit] != sampler) { glBindSampler(unit, sampler); if (tracked) smp[unit] = sampler; ++g_sc_changes[SC_SMP]; }
    }
};
GlStateCache g_sc;

GLenum topology(uint32_t d3d) {
    switch (d3d) {
        case 1: return GL_POINTS; case 2: return GL_LINES; case 3: return GL_LINE_STRIP;
        case 5: return GL_TRIANGLE_STRIP; case 10: return GL_LINES_ADJACENCY; default: return GL_TRIANGLES;
    }
}

// ---- replay -----------------------------------------------------------------------------------
void dump_first_program(const LswGlShader* vs, const LswGlShader* ps, const LswGlShader* gs);

// DIAG: GLSL of every shader a captured frame uses, once per shader: sdmc:/lswtcs/capglsl/<key>.vert|frag|geom
// (to compare a hardware capture's shaders with Eden's or the PC's).
void dump_capture_glsl(const LswGlShader* s) {
    if (!s || s->glsl.empty()) return;
    static bool dir = false;
    if (!dir) { mkdir("capglsl", 0777); dir = true; }
    static const char* const kExt[3] = {"vert", "frag", "geom"};
    char path[64];
    snprintf(path, sizeof path, "capglsl/%016llX.%s", (unsigned long long)s->key, kExt[s->stage < 3 ? s->stage : 0]);
    struct stat st;
    if (stat(path, &st) == 0) return;
    if (FILE* f = fopen(path, "wb")) { fwrite(s->glsl.data(), 1, s->glsl.size(), f); fclose(f); }
}

// What present_frame shows: a resolve target / render target (top-down rows) or the scene FBO.
struct PresentSrc { GLuint fbo = 0; uint32_t w = kSceneW, h = kSceneH; };
HostTex g_last_good;          // last complete frame (shown while a frame is held for compiling programs)
uint64_t g_frames_held = 0;
uint64_t g_stall_replay = 0;  // DIAG (LSWTCS_STALLSHOTS): last replay that took 1 s or more

// Arena (constants + indices) -> one buffer, bound as UBO ranges and as the index buffer. A ring of
// three fixed buffers, reallocated only when a frame needs more: orphaning one buffer every frame
// (glBufferData) ran Eden out of GPU address space in the hub.
// Writes never let nouveau synchronise: switch-libdrm's nouveau_bo_wait kicks the pushbuffer if the
// buffer is in it and then blocks on its fence, so any synchronised write (glBufferSubData, a plain
// map) into a buffer the GPU touched recently costs a submit plus a wait for the GPU. That hidden
// per-frame glFinish cost 12-13 ms of a 30 ms hub replay in Eden (2026-10-07, [GLPROF] upload/shm).
// Instead the slot is written through an unsynchronised map (bo_map with no access flags: no kick, no
// wait) and the render thread itself waits on the fence placed after that slot's replay, three replays
// back ([GLPROF] fwait = time the GPU really is behind). LSWTCS_GLUPMODE: 0 = glBufferSubData (old),
// 1 = unsynchronised map (default), 2 = persistent coherent map (glBufferStorage; in Eden its CPU
// mapping is unmapped memory: 20k "Unmapped Write8" in eden_log and the render thread crawls).
struct ArenaSlot { GLuint buf = 0; GLsizeiptr cap = 0; uint8_t* map = nullptr; GLsync fence = nullptr; };
ArenaSlot g_ring[3];
uint32_t g_ring_i = 0;
bool g_ring_written = false;   // the current slot was written by this replay: fence it afterwards
bool g_ring_piped = false;     // ... by the glthread worker (LSWTCS_GLPIPE): fence it there too

// LSWTCS_GLPIPE (default OFF; =1 with glthread on). Hardware 2026-10-09: GPU-bound, so it gained little (frame median
// 39.7 -> 35.8 ms, p90 unchanged), added a frame of input latency the user noticed, and the render thread then just
// waited on a full glthread queue. Worth retrying once the GPU cost is down. glFenceSync, glClientWaitSync, glMapBufferRange and
// eglSwapBuffers each make the render thread wait until glthread's worker has executed everything recorded so far
// (Eden 2026-10-09: ~3.2 ms per replay, all in the two glFenceSync calls; hardware ~6.7 ms "other"). With the
// pipeline those run on the worker instead, queued in order with the GL commands (lsw_glthread_call, switch-mesa
// mesa_glthread_edits.py): the render thread records replay N+1 while the worker executes replay N. Only the worker
// touches g_ring[].fence then, and the render thread waits (RP_PIPE) before the next replay until the worker has
// done the previous upload (f.arena is about to be handed back to the guest) and at most one swap is outstanding.
extern "C" int lsw_glthread_call(void (*fn)(void*), void* arg);
extern "C" EGLBoolean lsw_egl_swap_buffers_worker(EGLDisplay dpy, EGLSurface surface);
bool g_glpipe = false;
std::mutex g_pipe_mtx;
std::condition_variable g_pipe_cv;
uint64_t g_pipe_up_done = 0, g_pipe_swap_done = 0;   // worker progress (under g_pipe_mtx)
uint64_t g_pipe_up_queued = 0, g_pipe_swap_queued = 0;   // render thread only
struct ArenaJob { uint32_t slot; GLsizeiptr size, new_cap; const uint8_t* data; uint64_t seq; };
ArenaJob g_arena_job[3];

// glthread worker: the arena upload of one replay (fence wait for the slot, realloc, unsynchronised map + copy).
void arena_job_run(void* p) {
    const ArenaJob& j = *static_cast<const ArenaJob*>(p);
    ArenaSlot& s = g_ring[j.slot];
    if (s.fence) {
        RpScope rp(RP_FWAIT);
        glClientWaitSync(s.fence, GL_SYNC_FLUSH_COMMANDS_BIT, GLuint64(2000000000));
        glDeleteSync(s.fence);
        s.fence = nullptr;
    }
    glBindBuffer(GL_UNIFORM_BUFFER, s.buf);
    if (j.new_cap) glBufferData(GL_UNIFORM_BUFFER, j.new_cap, nullptr, GL_DYNAMIC_DRAW);
    if (j.size) {
        if (void* m = glMapBufferRange(GL_UNIFORM_BUFFER, 0, j.size,
                                       GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT)) {
            memcpy(m, j.data, size_t(j.size));
            glUnmapBuffer(GL_UNIFORM_BUFFER);
        } else {
            glBufferSubData(GL_UNIFORM_BUFFER, 0, j.size, j.data);
        }
    }
    { std::lock_guard<std::mutex> lk(g_pipe_mtx); g_pipe_up_done = j.seq; }
    g_pipe_cv.notify_all();
}

// glthread worker: fence after the replay that wrote ring slot *p.
void arena_fence_job(void* p) {
    g_ring[*static_cast<const uint32_t*>(p)].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void arena_upload(const LswGpuFrame& f) {
    static const int mode = [] {
        const char* e = getenv("LSWTCS_GLUPMODE");
        int m = e ? atoi(e) : 1;
        if (m == 2 && !p_glBufferStorage) m = 1;
        static const char* const kName[3] = {"glBufferSubData", "unsynchronised map", "persistent coherent map"};
        printf("[GL] arena ring: %s\n", kName[m < 0 || m > 2 ? 0 : m]);
        return m;
    }();
    const GLsizeiptr need = GLsizeiptr(f.arena.size()) + 256;
    g_ring_i = (g_ring_i + 1) % 3;
    ArenaSlot& s = g_ring[g_ring_i];
    if (g_glpipe && mode == 1) {   // the upload itself runs on the glthread worker (arena_job_run)
        RpScope rp(RP_UPLOAD);
        if (!s.buf) glGenBuffers(1, &s.buf);   // returns names: a worker sync, once per slot
        glBindBuffer(GL_UNIFORM_BUFFER, s.buf);
        GLsizeiptr new_cap = 0;
        if (need > s.cap) {
            ++g_gl_epoch;
            new_cap = std::max<GLsizeiptr>(need + need / 2, GLsizeiptr(4) << 20);
            s.cap = new_cap;
        }
        g_arena_job[g_ring_i] = {g_ring_i, GLsizeiptr(f.arena.size()), new_cap, f.arena.data(), ++g_pipe_up_queued};
        lsw_glthread_call(arena_job_run, &g_arena_job[g_ring_i]);
        g_ring_written = g_ring_piped = true;
        g_arena = s.buf;
        g_arena_cap = g_ring[0].cap + g_ring[1].cap + g_ring[2].cap;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_arena);   // indices live in the arena too (VAO state)
        return;
    }
    if (s.fence) {
        RpScope rp(RP_FWAIT);
        glClientWaitSync(s.fence, GL_SYNC_FLUSH_COMMANDS_BIT, GLuint64(2000000000));
        glDeleteSync(s.fence);
        s.fence = nullptr;
    }
    RpScope rp(RP_UPLOAD);
    if (!s.buf) glGenBuffers(1, &s.buf);
    glBindBuffer(GL_UNIFORM_BUFFER, s.buf);
    if (need > s.cap) {
        ++g_gl_epoch;
        const GLsizeiptr cap = std::max<GLsizeiptr>(need + need / 2, GLsizeiptr(4) << 20);
        if (mode == 2) {   // immutable storage: replace the buffer
            glDeleteBuffers(1, &s.buf);
            glGenBuffers(1, &s.buf);
            glBindBuffer(GL_UNIFORM_BUFFER, s.buf);
            const GLbitfield map_flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            p_glBufferStorage(GL_UNIFORM_BUFFER, cap, nullptr, map_flags | GL_DYNAMIC_STORAGE_BIT);
            s.map = static_cast<uint8_t*>(glMapBufferRange(GL_UNIFORM_BUFFER, 0, cap, map_flags));
            if (!s.map) printf("[GL] arena slot %u: persistent map failed (0x%X), using glBufferSubData\n", g_ring_i, glGetError());
        } else {
            glBufferData(GL_UNIFORM_BUFFER, cap, nullptr, GL_DYNAMIC_DRAW);
        }
        s.cap = cap;
    }
    const GLsizeiptr size = GLsizeiptr(f.arena.size());
    bool written = false;
    if (s.map) {
        memcpy(s.map, f.arena.data(), f.arena.size());
        written = true;
    } else if (mode == 1 && size) {
        static uint64_t t_map = 0, t_copy = 0, t_unmap = 0, n = 0;   // DIAG: split of the upload
        const uint64_t t0 = gp_now();
        if (void* p = glMapBufferRange(GL_UNIFORM_BUFFER, 0, size,
                                       GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT)) {
            const uint64_t t1 = gp_now();
            memcpy(p, f.arena.data(), f.arena.size());
            const uint64_t t2 = gp_now();
            glUnmapBuffer(GL_UNIFORM_BUFFER);
            t_map += t1 - t0; t_copy += t2 - t1; t_unmap += gp_now() - t2;
            written = true;
            if (++n == 300) {
                // DIAG: arena bytes by block kind (unique offsets) for this frame.
                std::unordered_map<uint32_t, uint32_t> seen[7];
                for (const LswGpuDrawRec& r : f.draws) {
                    if (r.kind != 0) continue;
                    seen[0].emplace(r.cb_sys_off, r.cb_sys_size);
                    seen[1].emplace(r.cb_vsf_off, r.cb_vsf_size);
                    seen[2].emplace(r.cb_psf_off, r.cb_psf_size);
                    seen[3].emplace(r.cb_bool_off, 160u);
                    seen[4].emplace(r.cb_fetch_off, 768u);
                    if (r.ib_count && !r.ib_shm) seen[5].emplace(r.ib_off, r.ib_count * (r.ib_32bit ? 4u : 2u));
                    for (uint32_t k = 0; k < r.range_count; ++k) {
                        const LswGpuRange& rg = f.ranges[r.first_range + k];
                        if (rg.arena_off != kLswRangeResident) seen[6].emplace(rg.arena_off, rg.size);
                    }
                }
                static const char* const kKind[7] = {"sys", "vsf", "psf", "bool", "fetch", "index", "vtx"};
                char line[256]; int len = 0;
                for (int k = 0; k < 7; ++k) {
                    uint64_t bytes = 0;
                    for (const auto& kv : seen[k]) bytes += kv.second;
                    len += snprintf(line + len, sizeof line - len, " %s=%zux/%lluKB", kKind[k], seen[k].size(),
                                    (unsigned long long)(bytes >> 10));
                }
                printf("[GLARENA] %zu draws:%s\n", f.draws.size(), line);
                printf("[GLUPLOAD] per replay: map=%.2fms memcpy=%.2fms unmap=%.2fms (%zu KB, map %p)\n", t_map / 1e6 / n,
                       t_copy / 1e6 / n, t_unmap / 1e6 / n, f.arena.size() / 1024, p);
                fflush(stdout);
                t_map = t_copy = t_unmap = n = 0;
            }
        }
    }
    if (!written) glBufferSubData(GL_UNIFORM_BUFFER, 0, size, f.arena.data());
    g_ring_written = mode != 0;
    g_ring_piped = false;
    g_arena = s.buf;
    g_arena_cap = g_ring[0].cap + g_ring[1].cap + g_ring[2].cap;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_arena);   // indices live in the arena too (VAO state)
}

// After a replay: fence the arena slot it wrote (mapped modes).
void arena_fence() {
    if (!g_ring_written) return;
    g_ring_written = false;
    if (g_ring_piped) {
        static const uint32_t kSlot[3] = {0, 1, 2};
        lsw_glthread_call(arena_fence_job, const_cast<uint32_t*>(&kSlot[g_ring_i]));
        return;
    }
    g_ring[g_ring_i].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

// Host render-target key of a guest colour / depth surface (as rt_get), without creating the target.
uint64_t rt_key(bool depth, uint32_t surf_info, uint32_t info) {
    uint32_t sx, sy;
    msaa_scale(surf_info, sx, sy);
    const uint32_t w = (surf_info & 0x3FFF) * sx;
    const uint32_t fmt = depth ? uint32_t(kDxgiD32S8) : rt_color_format((info >> 16) & 0xF);
    return (uint64_t(depth) << 63) | (uint64_t(info & 0xFFF) << 40) | (uint64_t(w) << 16) | uint64_t(fmt);
}
// Colour targets a draw writes (bit i = RT i), as the draw loop decides.
uint32_t draw_color_targets(const LswGpuDrawRec& r) {
    if (!(r.edram_mode == 4 && r.ps_dxbc)) return 0;
    const uint32_t psm = r.ps_rt_mask ? r.ps_rt_mask : 1u;
    uint32_t want = 0;
    for (uint32_t i = 0; i < 4; ++i) if ((psm & (1u << i)) && ((r.color_mask >> (4 * i)) & 0xF)) want |= 1u << i;
    if (!want && (r.color_mask & 0xF)) want = 1u;
    return want;
}
// Marks dead work in a recorded frame (see dest_dead): resolves whose destination is dead (2), and draws
// that write a single colour target whose next use is such a resolve that also clears it, with a depth
// target nothing in the frame reads (1).
// Content key of a draw: shaders, constant / index CONTENTS (each draw gets its own arena copy, so offsets differ),
// geometry, render target and blend / depth state. Excludes the system constants, viewport and scissor, which are
// what differ between the tiles of a predicated-tiling pass.
uint64_t draw_content_key(const LswGpuFrame& f, const LswGpuDrawRec& r) {
    uint64_t k = XXH3_64bits(&r.vs_key, sizeof r.vs_key) ^ (r.ps_key * 0x9E3779B97F4A7C15ull);
    const uint32_t parts[] = {r.ib_count, r.ib_shm, r.ib_shm ? r.ib_off : 0u, r.vertex_count, r.surf_info,
                              r.color_info, r.depth_info, r.range_count, r.blendcontrol, r.depthcontrol};
    k = XXH3_64bits_withSeed(parts, sizeof parts, k);
    auto arena_hash = [&](uint32_t off, uint32_t size) {
        if (size && size_t(off) + size <= f.arena.size()) k = XXH3_64bits_withSeed(f.arena.data() + off, size, k);
    };
    arena_hash(r.cb_vsf_off, r.cb_vsf_size);
    arena_hash(r.cb_psf_off, r.cb_psf_size);
    arena_hash(r.cb_bool_off, 160);
    arena_hash(r.cb_fetch_off, 768);
    if (!r.ib_shm) arena_hash(r.ib_off, r.ib_count * (r.ib_32bit ? 4u : 2u));
    for (uint32_t q = 0; q < r.range_count && q < 4; ++q) {
        const LswGpuRange& rg = f.ranges[r.first_range + q];
        const uint32_t rp[2] = {rg.phys, rg.size};
        k = XXH3_64bits_withSeed(rp, sizeof rp, k);
    }
    return k;
}

// ---- untiling (LSWTCS_GLUNTILE, default on) -----------------------------------------------------
// The game renders its world with predicated tiling: the whole draw list once per EDRAM tile (1280x704 frame:
// tiles of 320 / 320 / 128 rows), each tile with the viewport moved up by the tile's window offset (Xenia's
// GetHostViewportInfo clamps the viewport and moves the rest into the NDC offset of the system constants), then
// that tile's resolves copy its rows out. The host render targets are kRtH rows tall, so the tile split is not
// needed: tile 0 (offset 0, the full-frame transform) is drawn once with its scissor extended to the frame bottom,
// the later tiles' draws (exact repeats of tile 0's: same shaders, constants, geometry) are skipped, and their
// resolves read their rows from the full render target (rectangle shifted by the tile's offset; their clears
// likewise). Switch 2026-10-09: 64 % of hub draws and ~3,200 of ~5,000 draws per cutscene frame were tile repeats.
// A group is untiled only if every later tile repeats tile 0's draw sequence exactly and the offsets agree.
enum : uint8_t { UT_NONE = 0, UT_EXTEND = 1, UT_SKIP = 2, UT_SHIFT_RESOLVE = 3, UT_SHIFT_CLEAR = 4 };
// clear: resolves of an untiled group, 1 = copy now but apply its clears later, 2 = the group's last resolve: after
// it, apply the held clears. Every tile's resolve reads the one full render target, and the tiles overlap (Switch
// 2026-10-09 hub: rows 0-320, 288-608, 576-704), so tile 0's clear would wipe rows that tile 1 copies next.
// order: the records in replay order. A later tile's EDRAM clear quad (UT_SHIFT_CLEAR, arg = host rows) moves up to
// right after tile 0's and is drawn moved down by its tile's offset: a clear quad's rectangle covers its own tile only,
// so untiled, tile 0's cleared rows 0-320 and the rows below kept every earlier frame (Switch / Eden 2026-10-09: the
// "melting" bottom band, a staircase of old frames in rows 608-704). Drawn there (before any world draw of the
// group) the tiles' clears cover the frame exactly as the tiled clears did.
struct UntilePlan { std::vector<uint8_t> act, clear; std::vector<int32_t> arg, arg2; std::vector<uint32_t> order;
                    uint32_t groups = 0, skipped = 0; };
bool untile_on() {
    static const bool on = [] { const char* e = getenv("LSWTCS_GLUNTILE"); return !(e && e[0] == '0'); }();
    return on;
}
bool is_clear_quad(const LswGpuDrawRec& r) { return r.viewport[2] >= 4096.0f || r.viewport[3] >= 4096.0f; }
void plan_untile(const LswGpuFrame& f, UntilePlan& p) {
    const size_t n = f.draws.size();
    p.act.assign(n, UT_NONE);
    p.clear.assign(n, 0);
    p.order.resize(n);
    for (size_t i = 0; i < n; ++i) p.order[i] = uint32_t(i);
    p.arg.assign(n, 0);
    p.arg2.assign(n, 0);
    p.groups = p.skipped = 0;
    if (!untile_on()) return;
    // Segments: a run of draws followed by a run of resolves.
    struct Seg { size_t d0, d1, r1; };
    std::vector<Seg> segs;
    for (size_t i = 0; i < n;) {
        const size_t d0 = i;
        while (i < n && f.draws[i].kind == 0) ++i;
        const size_t d1 = i;
        while (i < n && f.draws[i].kind == 1) ++i;
        if (d1 > d0 && i > d1) segs.push_back({d0, d1, i});
        else if (d1 == d0 && i == d1) ++i;
    }
    std::vector<uint64_t> key(n, 0);
    for (size_t i = 0; i < n; ++i) if (f.draws[i].kind == 0) key[i] = draw_content_key(f, f.draws[i]);
    auto bottom_of = [&](const Seg& s, float& out) {   // the shared viewport bottom of a tile's world draws
        bool have = false;
        for (size_t i = s.d0; i < s.d1; ++i) {
            const LswGpuDrawRec& r = f.draws[i];
            if (is_clear_quad(r)) continue;
            const float b = r.viewport[1] + r.viewport[3];
            if (!have) { out = b; have = true; }
            else if (b != out) return false;
        }
        return have;
    };
    static const bool dbg = env_on("LSWTCS_GLUNTILEDBG", false);
    if (dbg && (g_replays % 300) == 1) {   // DIAG: the segment structure and why groups are rejected
        char line[1024]; int len = 0;
        for (size_t j = 0; j < segs.size() && len < 900; ++j) {
            float b = -1;
            const bool ok = bottom_of(segs[j], b);
            len += snprintf(line + len, sizeof line - len, " [%zu:%zu d, %zu r, bottom %s%.0f]", j, segs[j].d1 - segs[j].d0,
                            segs[j].r1 - segs[j].d1, ok ? "" : "MIXED ", b);
        }
        printf("[GLUNTILE] replay %llu: %zu segments:%s\n", (unsigned long long)g_replays, segs.size(), line);
        for (size_t j = 0; j + 1 < segs.size(); ++j) {
            const size_t l0 = segs[j].d1 - segs[j].d0, l1 = segs[j + 1].d1 - segs[j + 1].d0;
            if (l0 != l1 || l0 < 8) continue;
            size_t first_diff = l0;
            for (size_t q = 0; q < l0; ++q) if (!is_clear_quad(f.draws[segs[j].d0 + q]) && key[segs[j].d0 + q] != key[segs[j + 1].d0 + q]) { first_diff = q; break; }
            printf("[GLUNTILE]   segments %zu/%zu: %zu draws, first key difference at %zu\n", j, j + 1, l0, first_diff);
        }
    }
    std::vector<std::pair<uint32_t, uint32_t>> moved;   // (tile-0 clear quad, a later tile's clear quad) in tile order
    for (size_t j = 0; j + 1 < segs.size();) {
        const Seg& t0 = segs[j];
        const size_t len = t0.d1 - t0.d0;
        float b0;
        if (len < 8 || !bottom_of(t0, b0)) { ++j; continue; }
        size_t m = j + 1;
        std::vector<int32_t> offs;
        while (m < segs.size()) {
            const Seg& tk = segs[m];
            if (tk.d1 - tk.d0 != len) break;
            bool same = true;
            // The per-tile EDRAM clear quads (8192-wide viewport) differ per tile; untiled, the later tiles' ones are
            // skipped anyway (they would wipe the full render target), so they only have to line up.
            for (size_t q = 0; q < len && same; ++q) {
                const bool c0 = is_clear_quad(f.draws[t0.d0 + q]), ck = is_clear_quad(f.draws[tk.d0 + q]);
                same = (c0 && ck) || (!c0 && !ck && key[t0.d0 + q] == key[tk.d0 + q]);
            }
            float bk;
            if (!same || !bottom_of(tk, bk) || !(bk < b0)) break;
            const int32_t off = int32_t(b0 - bk);
            if (!offs.empty() && off <= offs.back()) break;
            offs.push_back(off);
            ++m;
        }
        if (offs.empty()) { ++j; continue; }
        // Tile 0's draws get their real screen scissor: the union over all tiles of the draw's scissor (each tile's
        // is the game's screen scissor clipped to that tile's window, in tile rows) shifted by the tile's offset.
        // A draw whose screen scissor lies below tile 0 (the floor of a cutscene, Switch 2026-10-09: "melting"
        // bottom band) has an empty scissor in tile 0 and only draws in the later tiles. EDRAM clear quads keep
        // their own scissor: the later tiles' ones are drawn moved instead (UT_SHIFT_CLEAR below).
        for (size_t q = 0; q < len; ++q) {
            const size_t i = t0.d0 + q;
            if (is_clear_quad(f.draws[i])) continue;
            int32_t top = INT32_MAX, bot = INT32_MIN, left = INT32_MAX, right = INT32_MIN;
            for (size_t k = j; k < m; ++k) {
                const LswGpuDrawRec& rk = f.draws[segs[k].d0 + q];
                if (rk.scissor[2] == 0 || rk.scissor[3] == 0) continue;
                const int32_t off = k == j ? 0 : offs[k - j - 1];
                top = std::min(top, int32_t(rk.scissor[1]) + off);
                bot = std::max(bot, int32_t(rk.scissor[1] + rk.scissor[3]) + off);
                left = std::min(left, int32_t(rk.scissor[0]));
                right = std::max(right, int32_t(rk.scissor[0] + rk.scissor[2]));
            }
            if (top >= bot) continue;
            p.act[i] = UT_EXTEND;
            p.arg[i] = (top << 16) | (bot & 0xFFFF);
            p.arg2[i] = (left << 16) | (right & 0xFFFF);
        }
        for (size_t k = j + 1; k < m; ++k) {
            const Seg& tk = segs[k];
            for (size_t i = tk.d0; i < tk.d1; ++i) { p.act[i] = UT_SKIP; ++p.skipped; }
            for (size_t i = tk.d1; i < tk.r1; ++i) { p.act[i] = UT_SHIFT_RESOLVE; p.arg[i] = offs[k - j - 1]; }
        }
        // Host rows of the offsets (they are in the world draws' surface rows).
        uint32_t wsx = 1, wsy = 1;
        for (size_t q = 0; q < len; ++q)
            if (!is_clear_quad(f.draws[t0.d0 + q])) { msaa_scale(f.draws[t0.d0 + q].surf_info, wsx, wsy); break; }
        for (size_t q = 0; q < len; ++q)
            if (is_clear_quad(f.draws[t0.d0 + q]))
                for (size_t k = j + 1; k < m; ++k) {
                    const size_t i = segs[k].d0 + q;
                    p.act[i] = UT_SHIFT_CLEAR;
                    p.arg[i] = offs[k - j - 1] * int32_t(wsy);
                    --p.skipped;
                    moved.emplace_back(uint32_t(t0.d0 + q), uint32_t(i));
                }
        for (size_t k = j; k < m; ++k)
            for (size_t i = segs[k].d1; i < segs[k].r1; ++i) p.clear[i] = 1;
        p.clear[segs[m - 1].r1 - 1] = 2;
        ++p.groups;
        j = m;
    }
    if (moved.empty()) return;
    // Replay order: each moved clear right after the tile-0 clear quad it repeats (moved is sorted by tile-0 index).
    std::stable_sort(moved.begin(), moved.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    p.order.clear();
    size_t mi = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p.act[i] == UT_SHIFT_CLEAR) continue;
        p.order.push_back(uint32_t(i));
        while (mi < moved.size() && moved[mi].first == i) p.order.push_back(moved[mi++].second);
    }
}

void plan_dead_work(const LswGpuFrame& f, std::vector<uint8_t>& dead) {
    const size_t n = f.draws.size();
    dead.assign(n, 0);
    if (!deadskip_on()) return;
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        const LswGpuDrawRec& r = f.draws[i];
        if (r.kind == 1 && r.rs_copy_cmd <= 1 && r.rs_dest && dest_dead(r.rs_dest)) { dead[i] = 2; any = true; }
    }
    // After the decisions: every resolve of this replay counts as "recent" for the next ones (dest_dead), so all
    // tile resolves of a one-shot copy in this replay get the same answer.
    for (size_t i = 0; i < n; ++i)
        if (f.draws[i].kind == 1 && f.draws[i].rs_dest) g_dest_last_resolve[f.draws[i].rs_dest] = g_replays;
    if (!any) return;
    std::unordered_set<uint64_t> depth_read;   // depth targets tested against or resolved this frame
    for (const LswGpuDrawRec& r : f.draws) {
        if (r.kind == 1) { if (r.rs_src >= 4) depth_read.insert(rt_key(true, r.surf_info, r.depth_info)); continue; }
        if (r.depthcontrol & 3) depth_read.insert(rt_key(true, r.surf_info, r.depth_info));   // stencil or depth test
    }
    for (size_t i = 0; i < n; ++i) {
        const LswGpuDrawRec& r = f.draws[i];
        if (r.kind != 0 || draw_color_targets(r) != 1u) continue;
        if (((r.depthcontrol >> 1) & 3) && depth_read.count(rt_key(true, r.surf_info, r.depth_info))) continue;
        const uint64_t x = rt_key(false, r.surf_info, r.color_info);
        for (size_t j = i + 1; j < n; ++j) {
            const LswGpuDrawRec& q = f.draws[j];
            if (q.kind == 1) {
                if (q.rs_src < 4 && rt_key(false, q.surf_info, q.color_info) == x) {
                    if (dead[j] == 2 && q.rs_clear_color) dead[i] = 1;
                    break;
                }
                continue;
            }
            const uint32_t t = draw_color_targets(q);
            bool touches = false;
            for (uint32_t k = 0; k < 4 && !touches; ++k)
                if (t & (1u << k)) touches = rt_key(false, q.surf_info, k == 0 ? q.color_info : q.color_info_mrt[k]) == x;
            if (touches) break;
        }
    }
}

// ---- frame log (LSWTCS_FRAMELOG, default on): sdmc:/lswtcs/frames.log, one line per replay ---------------------
// For the mid-level "show the goal" camera that reaches its target and then swings off in a random direction
// (Switch 2026-10-09): the game's frame time and real frame time, replay / GPU wait / program-build hold, and the
// camera taken from the world draws' vertex constants. The game's shaders read a view matrix (vs_view, NU2
// "projectToEyeSpace") and build worldView on the GPU, so the view matrix is the one rigid 4-register block that
// is identical across most world draws; object matrices differ per draw. Layout (Eden hub dump 2026-10-09,
// LSWTCS_CAMDUMP): c0-c3 viewProj, c4-c6 the view matrix as three rows with the translation in w (c6 = c3), c7-c10
// the world matrix.
// A turn of the view direction by more than 40 degrees in one frame captures the next replay (screenshot and
// per-draw log; at most once per 20 s, 10 per session), so a run needs no button press at the moment it happens.
bool g_cam_auto_capture = false;
uint64_t g_frame_hold_ns = 0;   // this replay's wait for program builds (wait_frame_programs)
bool framelog_on() {
    static const bool on = env_on("LSWTCS_FRAMELOG", true);
    return on;
}
struct CamProbe { bool ok = false; float eye[3] = {}, fwd[3] = {}; uint32_t reg = 0, votes = 0, draws = 0; char layout = '-'; };
CamProbe cam_probe(const LswGpuFrame& f) {
    CamProbe out;
    struct Vote { uint32_t n = 0; const float* m = nullptr; uint32_t reg = 0; char layout = '-'; };
    std::unordered_map<uint64_t, Vote> votes;
    std::vector<uint64_t> here;
    auto unit = [](const float* a, const float* b) { return std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]); };
    for (const LswGpuDrawRec& r : f.draws) {
        if (r.kind != 0 || r.edram_mode != 4 || r.cb_vsf_size < 64 || is_clear_quad(r)) continue;
        if (++out.draws > 96) break;
        if (size_t(r.cb_vsf_off) + r.cb_vsf_size > f.arena.size()) continue;
        const float* c = reinterpret_cast<const float*>(f.arena.data() + r.cb_vsf_off);
        const uint32_t n = std::min<uint32_t>(r.cb_vsf_size / 16, 256);
        here.clear();
        for (uint32_t k = 0; k + 3 <= n; ++k) {
            const float* m = c + k * 4;
            const char layout = 'V';   // three registers = rows of the view matrix, translation in w
            float rows[3][3];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) rows[i][j] = m[i * 4 + j];
            bool rigid = true;
            for (int i = 0; i < 3 && rigid; ++i) {
                rigid = std::fabs(unit(rows[i], rows[i]) - 1.0f) < 2e-3f;
                for (int j = i + 1; j < 3 && rigid; ++j) rigid = unit(rows[i], rows[j]) < 2e-3f;
            }
            if (!rigid) continue;
            // Unrotated blocks are skipped: static geometry shares translation-only world matrices across many draws.
            if (std::fabs(rows[0][0] - 1) < 1e-4f && std::fabs(rows[1][1] - 1) < 1e-4f && std::fabs(rows[2][2] - 1) < 1e-4f) continue;
            const uint64_t h = XXH3_64bits(m, 48);
            if (std::find(here.begin(), here.end(), h) != here.end()) continue;
            here.push_back(h);
            Vote& v = votes[h];
            if (!v.n) { v.m = m; v.reg = k; v.layout = layout; }
            ++v.n;
        }
    }
    {   // DIAG (LSWTCS_CAMDUMP=<replay>): the vertex constants c0-c31 of the first 6 world draws of that replay
        static const uint64_t at = [] { const char* e = getenv("LSWTCS_CAMDUMP"); return e ? strtoull(e, nullptr, 10) : 0ull; }();
        if (at && g_replays == at) {
            uint32_t shown = 0;
            for (const LswGpuDrawRec& r : f.draws) {
                if (r.kind != 0 || r.edram_mode != 4 || r.cb_vsf_size < 64 || is_clear_quad(r)) continue;
                if (++shown > 6) break;
                const float* c = reinterpret_cast<const float*>(f.arena.data() + r.cb_vsf_off);
                printf("[CAMDUMP] draw %u vs %016llX vsf %u regs:\n", shown, (unsigned long long)r.vs_key, r.cb_vsf_size / 16);
                for (uint32_t k = 0; k < 32 && k < r.cb_vsf_size / 16; ++k)
                    printf("[CAMDUMP]   c%-2u %10.4f %10.4f %10.4f %10.4f\n", k, c[k * 4], c[k * 4 + 1], c[k * 4 + 2], c[k * 4 + 3]);
            }
            printf("[CAMDUMP] %zu distinct rigid blocks voted\n", votes.size());
            fflush(stdout);
        }
    }
    const Vote* best = nullptr;
    // Ties go to the higher register: c3 (viewProj's last row) equals c6, so c3-c5 is rigid too and as common.
    for (const auto& kv : votes)
        if (!best || kv.second.n > best->n || (kv.second.n == best->n && kv.second.reg > best->reg)) best = &kv.second;
    if (!best || best->n < std::max<uint32_t>(4, std::min<uint32_t>(out.draws, 96) / 3)) return out;
    const float* m = best->m;
    out.ok = true; out.reg = best->reg; out.votes = best->n; out.layout = best->layout;
    for (int j = 0; j < 3; ++j) {   // eye = -R^T t; forward = the view z row
        out.eye[j] = -(m[0 * 4 + j] * m[0 * 4 + 3] + m[1 * 4 + j] * m[1 * 4 + 3] + m[2 * 4 + j] * m[2 * 4 + 3]);
        out.fwd[j] = m[2 * 4 + j];
    }
    return out;
}
void frame_log(const LswGpuFrame& f, uint64_t replay_ns) {
    if (!framelog_on()) return;
    static FILE* fl = [] {
        FILE* x = fopen("frames.log", "w");
        if (x) fprintf(x, "# replay serial t_s game_ft_ms swap_real_ms replay_ms gpuwait_ms(prev) hold_ms draws | eye xyz | fwd xyz | "
                          "view block (register, layout C/R, draws agreeing / world draws) | turn_deg move\n");
        return x;
    }();
    if (!fl) return;
    static uint64_t t0 = gp_now(), last_serial = 0, gpuwait_last = 0, last_capture_ns = 0;
    static uint32_t auto_shots = 0;
    static CamProbe prev;
    const uint64_t now = gp_now();
    const uint64_t gw = g_rp[RP_GPUWAIT] - gpuwait_last;
    gpuwait_last = g_rp[RP_GPUWAIT];
    const bool repeat = f.serial == last_serial;
    last_serial = f.serial;
    const CamProbe cam = cam_probe(f);
    float turn = 0, move = 0;
    if (cam.ok && prev.ok && !repeat) {
        const float d = std::clamp(cam.fwd[0] * prev.fwd[0] + cam.fwd[1] * prev.fwd[1] + cam.fwd[2] * prev.fwd[2], -1.0f, 1.0f);
        turn = std::acos(d) * 57.29578f;
        move = std::sqrt((cam.eye[0] - prev.eye[0]) * (cam.eye[0] - prev.eye[0]) + (cam.eye[1] - prev.eye[1]) * (cam.eye[1] - prev.eye[1]) +
                         (cam.eye[2] - prev.eye[2]) * (cam.eye[2] - prev.eye[2]));
    }
    fprintf(fl, "%llu %llu%s %.3f %.1f %.1f %.1f %.1f %.1f %zu | %.2f %.2f %.2f | %.3f %.3f %.3f | c%u %c %u/%u | %.1f %.2f%s\n",
            (unsigned long long)g_replays, (unsigned long long)f.serial, repeat ? "R" : "", (now - t0) / 1e9, f.game_ft * 1000.0f,
            f.swap_real * 1000.0f, replay_ns / 1e6, gw / 1e6, g_frame_hold_ns / 1e6, f.draws.size(), cam.eye[0], cam.eye[1], cam.eye[2],
            cam.fwd[0], cam.fwd[1], cam.fwd[2], cam.reg, cam.layout, cam.votes, cam.draws, turn, move, cam.ok ? "" : " NOCAM");
    // Camera memory around the globals that hold the view matrix (LSWTCS_CAMFIND, Eden 2026-10-10), kept for the last
    // 16 replays; a sudden turn writes camdump_<replay>.txt: every word 16 replays earlier and now, changed words
    // marked, with float values. For the level-start camera that parks at the world origin (Kamino, 2026-10-10): the
    // camera's target / position fields, then a data breakpoint on them (PC build) finds the code writing the zero.
    static const uint32_t kCamRanges[][2] = {{0x830B5F00u, 0x830B6400u}, {0x82FC6400u, 0x82FC6700u},
                                              {0x82FE7800u, 0x82FE7A00u}, {0x82FF5400u, 0x82FF5700u}};
    static std::vector<uint32_t> cam_hist[16];
    {

        std::vector<uint32_t>& h = cam_hist[g_replays % 16];
        h.clear();
        for (const auto& rg : kCamRanges)
            for (uint32_t a = rg[0]; a < rg[1]; a += 4) h.push_back(__builtin_bswap32(*reinterpret_cast<const uint32_t*>(g_base + a)));
    }
    // Opt-in (LSWTCS_CAMCAPTURE=1): every hub/level entry is a camera cut, and the capture stalls that load by
    // seconds (Eden hub entry 2026-10-11: 7.8 s replay + 8.2 s swap) and costs the game clock as much.
    static const bool cam_capture = env_on("LSWTCS_CAMCAPTURE", false);
    if (cam_capture && turn > 40.0f && auto_shots < 10 && now - last_capture_ns > 20000000000ull) {
        const std::vector<uint32_t>& was = cam_hist[(g_replays + 1) % 16];   // 15 replays ago (oldest kept)
        const std::vector<uint32_t>& is = cam_hist[g_replays % 16];
        char name[48];
        snprintf(name, sizeof name, "camdump_%llu.txt", (unsigned long long)g_replays);
        if (FILE* cf = fopen(name, "w"); cf && was.size() == is.size()) {
            fprintf(cf, "# replay %llu, camera turned %.1f deg; columns: address, 15 replays earlier, now (hex and float), * = changed\n",
                    (unsigned long long)g_replays, turn);
            size_t i = 0;
            for (const auto& rg : kCamRanges)
                for (uint32_t a = rg[0]; a < rg[1]; a += 4, ++i) {
                    float fw, fi;
                    memcpy(&fw, &was[i], 4); memcpy(&fi, &is[i], 4);
                    fprintf(cf, "%08X %08X %08X %c %14.5g %14.5g\n", a, was[i], is[i], was[i] != is[i] ? '*' : ' ', fw, fi);
                }
            fclose(cf);
        }
        printf("[FRAMELOG] camera memory written to %s\n", name);
        g_cam_auto_capture = true;
        last_capture_ns = now;
        ++auto_shots;
        fprintf(fl, "# camera turned %.1f deg in one frame: capturing replay %llu\n", turn, (unsigned long long)g_replays + 1);
        printf("[FRAMELOG] camera turned %.1f deg in one frame at replay %llu: auto capture\n", turn, (unsigned long long)g_replays);
        fflush(stdout);
    }
    if (cam.ok && !repeat) prev = cam;
    static uint32_t lines = 0;
    if ((++lines % 30) == 0 || g_cam_auto_capture) fflush(fl);
}

PresentSrc replay() {
    gpu_frame_take();
    LswGpuFrame& f = g_gpu_frame_render;   // replayed again if no newer frame was published
    ++g_replays;
    ++g_gl_epoch;   // present / capture / clears changed state since the last replay
    g_tex_memo.clear();
    const bool gpurt = gpurt_on();
    const uint64_t replay_t0 = gp_now();
    uint64_t rp0[RP_COUNT];
    memcpy(rp0, g_rp, sizeof rp0);
    const uint32_t prog0 = g_prog_compiled, tex0 = g_tex_created;
    uint64_t ml_ns0, ml_cont0, ml_calls0;
    lsw_malloc_lock_stats(&ml_ns0, &ml_cont0, &ml_calls0);
    g_prog_pending_draws = 0;
    g_frame_hold_ns = 0;
    {   // capture this replay? (trigger file checked twice a second, or the GLTEST frames)
        static const bool gltest = env_on("LSWTCS_GLTEST", false);
        static uint64_t next_check = 0;
        g_capture_frame = gltest && (g_replays == 300 || g_replays == 1500);
        // LSWTCS_SHOTAT=a,b,c: capture these replays (screenshot + per-draw log) without a trigger file
        // (the Switch has no way to create shot_now mid-run).
        static const std::vector<uint64_t> shot_at = [] {
            std::vector<uint64_t> v;
            if (const char* e = getenv("LSWTCS_SHOTAT"))
                for (const char* q = e; *q;) { v.push_back(strtoull(q, const_cast<char**>(&q), 10)); while (*q == ',' || *q == ' ') ++q; if (*q && (*q < '0' || *q > '9')) break; }
            return v;
        }();
        for (uint64_t a : shot_at) if (a == g_replays) g_capture_frame = true;
        if (g_cam_auto_capture) { g_cam_auto_capture = false; g_capture_frame = true; }   // frame_log: sudden camera turn
        if (g_replays >= next_check) {
            next_check = g_replays + 30;
            if (FILE* t = fopen("shot_now", "rb")) { fclose(t); remove("shot_now"); g_capture_frame = true; }
        }
        // LSWTCS_STALLSHOTS=1: capture a few replays right after a replay of 1 s or more (scene transitions).
        static const bool stall_shots = env_on("LSWTCS_STALLSHOTS", false);
        // LSWTCS_STALLSHOTOFFS=a,b,c: replays after the stall to capture (default 1,4,8,12,16,24,32,48).
        static const std::vector<uint64_t> offs = [] {
            std::vector<uint64_t> v;
            const char* e = getenv("LSWTCS_STALLSHOTOFFS");
            for (const char* q = e ? e : "1,4,8,12,16,24,32,48"; *q;) {
                v.push_back(strtoull(q, const_cast<char**>(&q), 10));
                while (*q == ',' || *q == ' ') ++q;
                if (*q && (*q < '0' || *q > '9')) break;
            }
            return v;
        }();
        if (stall_shots && g_stall_replay) {
            const uint64_t d = g_replays - g_stall_replay;
            uint64_t last = 0;
            for (uint64_t o : offs) { if (o == d) g_capture_frame = true; last = std::max(last, o); }
            if (d > last) g_stall_replay = 0;
        }
    }
    if (g_capture_frame) printf("[GLDRAWCAP] ==== replay %llu: %zu records ====\n", (unsigned long long)g_replays, f.draws.size());
    PresentSrc present{g_scene_fbo, kSceneW, kSceneH};
    if (!gpurt) {   // LSWTCS_GPURT=0: everything into the scene FBO, cleared per frame
        glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);   // the PC replay's backbuffer clear colour
        glClearDepthf(1.0f);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    if (f.draws.empty()) return present;
    // Arena (constants + indices) -> one buffer, bound as UBO ranges and as the index buffer. A ring of
    // three fixed buffers, reallocated only when a frame needs more: orphaning one buffer every frame
    // (glBufferData) ran Eden out of GPU address space in the hub.
    arena_upload(f);
    {   // DIAG (LSWTCS_GLGPUPROF=N): GPU timestamps in every Nth replay (gpu_stamp / gpu_prof_finish)
        static const uint64_t every = [] { const char* e = getenv("LSWTCS_GLGPUPROF"); return uint64_t(e ? atoi(e) : 0); }();
        g_gpuprof_frame = every && (g_replays % every) == 0;
        static const bool split = env_on("LSWTCS_GLGPUSPLIT", false);
        static uint64_t samples = 0;
        const uint64_t kind = g_gpuprof_frame && split ? samples++ % 3 : 0;
        g_gpuprof_nopix = kind != 0;
        g_gpuprof_novtx = kind == 2;
        g_gpuprof_verts = 0;
        gpu_stamp(GK_START);
    }
    const uint64_t t_arena = gp_now();
    // Per-draw system-constant copies with swizzle overrides for resolved textures (2 MB per frame).
    static GLuint patch_ring[3] = {};
    constexpr GLsizeiptr kPatchCap = GLsizeiptr(2) << 20;
    if (!patch_ring[g_ring_i]) {
        glGenBuffers(1, &patch_ring[g_ring_i]);
        glBindBuffer(GL_UNIFORM_BUFFER, patch_ring[g_ring_i]);
        glBufferData(GL_UNIFORM_BUFFER, kPatchCap, nullptr, GL_DYNAMIC_DRAW);
    }
    const GLuint patch_buf = patch_ring[g_ring_i];
    GLintptr patch_off = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_shm);
    g_rp[RP_UPLOAD] += gp_now() - t_arena;
    uint32_t copies = 0;
    // Shared-memory ranges: GPU copies out of the arena buffer, which already holds their bytes
    // (LSWTCS_GLSHMCOPY=0: glBufferSubData). A CPU write into the 512 MB buffer every draw reads makes
    // nouveau wait for the GPU to finish all work submitted so far: 3 copies of ~13 KB cost 11.3 ms per
    // hub replay in Eden and up to 4.8 s per stalled replay on a cold start (2026-10-07, [GLPROF] shm).
    static const bool shm_gpu_copy = env_on("LSWTCS_GLSHMCOPY", true);
    if (shm_gpu_copy) {
        glBindBuffer(GL_COPY_READ_BUFFER, g_arena);
        glBindBuffer(GL_COPY_WRITE_BUFFER, g_shm);
    }
    auto upload = [&](const LswGpuRange& rg) {
        if (rg.arena_off == kLswRangeResident || GLsizeiptr(rg.phys) + rg.size > kShmSize) return;
        RpScope rp(RP_SHM);
        if (shm_gpu_copy) {
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, GLintptr(rg.arena_off), GLintptr(rg.phys),
                                GLsizeiptr(rg.size));
        } else {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_shm);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, GLintptr(rg.phys), GLsizeiptr(rg.size), f.arena.data() + rg.arena_off);
        }
        ++copies;
    };
    for (const LswGpuRange& rg : f.prelude) upload(rg);
    // All-or-nothing frames: if any draw's program is still compiling in the background, run none of
    // the frame (drawing a subset leaves render targets half-built and the composites show garbage:
    // stray texture atlases on black, Eden 2026-10-07) and keep showing the last complete frame. The
    // frame's shared-memory uploads still happen (residency tracking counts on them).
    if (gpurt && g_num_workers > 0) {
        const uint32_t pending0 = g_prog_pending_draws;
        for (const LswGpuDrawRec& r : f.draws) {
            if (r.kind != 0) continue;
            const auto* vs = static_cast<const LswGlShader*>(r.vs_dxbc);
            const auto* gs = static_cast<const LswGlShader*>(r.gs_dxbc);
            if (!vs || (r.gs_type && !gs)) continue;
            RpScope rp(RP_PROGRAM);
            program(vs, static_cast<const LswGlShader*>(r.ps_dxbc), gs);   // requests the compile if needed
        }
        if (g_prog_pending_draws != pending0 && prog_wait_mode()) {
            RpScope rp(RP_PROGRAM);
            const uint64_t th = gp_now();
            wait_frame_programs();
            g_frame_hold_ns += gp_now() - th;
            g_prog_pending_draws = pending0;   // the draw loop below finds them settled
        }
        if (const uint32_t waiting = g_prog_pending_draws - pending0) {
            for (const LswGpuDrawRec& r : f.draws)
                if (r.kind == 0) for (uint32_t k = 0; k < r.range_count; ++k) upload(f.ranges[r.first_range + k]);
            ++g_frames_held;
            if ((g_frames_held % 60) == 1) {
                size_t queued;
                { std::lock_guard<std::mutex> lk(g_prog_mtx); queued = g_prog_queue.size(); }
                printf("[GLHOLD] replay %llu: frame held (%u of %zu records wait on programs, %zu programs queued), %llu frames held so far\n",
                       (unsigned long long)g_replays, waiting, f.draws.size(), queued, (unsigned long long)g_frames_held);
                fflush(stdout);
            }
            if (g_last_good.tex) present = {single_fbo(g_last_good), kSceneW, kSceneH};
            return present;
        }
    }
    uint32_t drawn = 0, no_prog = 0, no_gs = 0, no_rt = 0, resolves = 0, patched = 0;
    uint32_t by_gs[4] = {};   // drawn: plain, point list, rectangle list, quad list
    g_res_tex_binds = 0;
    std::vector<UnitBind> units;
    std::vector<std::pair<uint32_t, uint32_t>> swz;
    uint32_t draw_i = 0;
    uint32_t dead_draws = 0, dead_resolves = 0;
    std::vector<uint8_t> dead;   // per record: 1 = skip the draw, 2 = skip the resolve's copy
    if (gpurt) plan_dead_work(f, dead); else dead.assign(f.draws.size(), 0);
    static UntilePlan ut;
    if (gpurt) plan_untile(f, ut); else { ut.act.assign(f.draws.size(), UT_NONE); ut.clear.assign(f.draws.size(), 0); ut.order.resize(f.draws.size()); for (size_t i = 0; i < f.draws.size(); ++i) ut.order[i] = uint32_t(i); ut.arg.assign(f.draws.size(), 0); ut.arg2.assign(f.draws.size(), 0); }
    GLuint ebo_bound = g_arena;   // arena_upload bound the arena as the index buffer
    LswGpuDrawRec rmod;   // a record with the untiling changes (scissor extended / resolve rows shifted)
    for (const uint32_t rec_order : ut.order) {
        const size_t rec_i = rec_order;
        const LswGpuDrawRec& r_orig = f.draws[rec_i];
        const LswGpuDrawRec* rp_ = &r_orig;
        if (ut.act[rec_i] == UT_EXTEND) {
            rmod = r_orig;
            const uint32_t top = uint32_t(ut.arg[rec_i]) >> 16, bot = uint32_t(ut.arg[rec_i]) & 0xFFFF;
            const uint32_t left = uint32_t(ut.arg2[rec_i]) >> 16, right = uint32_t(ut.arg2[rec_i]) & 0xFFFF;
            rmod.scissor[0] = left;
            rmod.scissor[1] = top;
            rmod.scissor[2] = right - left;
            rmod.scissor[3] = bot - top;
            rp_ = &rmod;
        } else if (ut.act[rec_i] == UT_SHIFT_CLEAR) {   // a later tile's clear quad, moved down to its tile's rows
            rmod = r_orig;
            uint32_t csx, csy; msaa_scale(r_orig.surf_info, csx, csy);
            const int32_t rows = ut.arg[rec_i] / int32_t(csy);
            rmod.viewport[1] += float(rows);
            rmod.scissor[1] += uint32_t(rows);
            rp_ = &rmod;
        } else if (ut.act[rec_i] == UT_SHIFT_RESOLVE) {
            rmod = r_orig;
            rmod.rs_rect[1] += ut.arg[rec_i];
            rmod.rs_rect[3] += ut.arg[rec_i];
            rp_ = &rmod;
        }
        const LswGpuDrawRec& r = *rp_;
        if (ut.act[rec_i] == UT_SKIP) {   // a later tile's repeat of a tile-0 draw: tile 0 covered its rows
            ++draw_i;
            // Residency, as below (a walk-time copy shares tile 0's ranges, uploaded there).
            if (!r.tile_repeat) for (uint32_t k = 0; k < r.range_count; ++k) upload(f.ranges[r.first_range + k]);
            continue;
        }
        if (r.tile_repeat) {
            // A walk-time copy (kernel_stubs.cpp tile skipping) outside an untiled group: drawn like any record (it has
            // its own tile's viewport, scissor and system constants, lsw_gpu_tile_state). Until 2026-10-11 these were
            // dropped, and everything below tile 0 that only those tiles drew went missing (characters cut off at row
            // 320 in an Episode I level). Counted: it means the walk's repeat check and plan_untile disagree.
            static uint64_t n = 0;
            if ((++n % 1000) == 1) printf("[GLUNTILE] %llu walk-time tile copies outside an untiled group drawn\n", (unsigned long long)n);
        }
        if (r.kind == 1) {   // EDRAM resolve
            if (g_capture_frame)
                printf("[GLDRAWCAP] #%u RESOLVE src=%u rect=%d,%d..%d,%d pitch=%u -> 0x%08X %ux%u at %d,%d tfmt=%u clr c%u d%u\n", draw_i, r.rs_src,
                       r.rs_rect[0], r.rs_rect[1], r.rs_rect[2], r.rs_rect[3], r.surf_info & 0x3FFF, r.rs_dest, r.rs_dest_pitch,
                       r.rs_dest_height, r.rs_dest_x, r.rs_dest_y, r.rs_dest_fmt, r.rs_clear_color, r.rs_clear_depth);
            ++draw_i;
            if (gpurt) {
                RpScope rp(RP_RESOLVE);
                const bool copy = dead[rec_i] != 2;
                if (!copy) ++dead_resolves;
                static std::vector<LswGpuDrawRec> held;   // resolves of an untiled group whose clears wait
                if (ut.clear[rec_i]) {
                    do_resolve(r, copy, false);
                    held.push_back(r);
                    if (ut.clear[rec_i] == 2) {
                        for (const LswGpuDrawRec& h : held) do_resolve(h, false, true);
                        held.clear();
                    }
                } else {
                    do_resolve(r, copy);
                }
                ++resolves;
                gpu_stamp(GK_RESOLVE);
            }
            continue;
        }
        ++draw_i;
        // 1. Vertex data into shared memory (every recorded draw: residency tracking assumed it). A walk-time tile copy
        //    shares tile 0's ranges, uploaded with tile 0's record.
        if (!r.tile_repeat) for (uint32_t k = 0; k < r.range_count; ++k) upload(f.ranges[r.first_range + k]);
        if (dead[rec_i] == 1) { ++dead_draws; continue; }   // output only feeds a skipped resolve
        const auto* vs = static_cast<const LswGlShader*>(r.vs_dxbc);
        const auto* ps = static_cast<const LswGlShader*>(r.ps_dxbc);
        const auto* gs = static_cast<const LswGlShader*>(r.gs_dxbc);
        if (r.gs_type && !gs) { ++no_gs; continue; }
        GLuint prog = 0;
        if (vs) { RpScope rp(RP_PROGRAM); prog = program(vs, ps, gs); }
        if (!prog) { ++no_prog; continue; }
        if (env_on("LSWTCS_GLTEST", false)) dump_first_program(vs, ps, gs);
        // 2. Texture sources (may blit composites / views: before any binding).
        const uint32_t* fetch_all = reinterpret_cast<const uint32_t*>(f.arena.data() + r.cb_fetch_off);
        units.clear(); swz.clear(); g_unit_log.clear();
        collect_units(ps, fetch_all, gpurt, units, swz);
        collect_units(vs, fetch_all, gpurt, units, swz);
        // 3. Render targets (sample space for MSAA surfaces).
        DrawTarget dt;
        const uint64_t t_rt = gp_now();
        if (gpurt) {
            msaa_scale(r.surf_info, dt.msx, dt.msy);
            const bool color = r.edram_mode == 4 && r.ps_dxbc;
            uint32_t want = 0;
            if (color) {
                const uint32_t psm = r.ps_rt_mask ? r.ps_rt_mask : 1u;
                for (uint32_t i = 0; i < 4; ++i) if ((psm & (1u << i)) && ((r.color_mask >> (4 * i)) & 0xF)) want |= 1u << i;
                if (!want && (r.color_mask & 0xF)) want = 1u;
            }
            Rt* crts[4] = {};
            uint32_t nrt = 0;
            bool fail = false;
            for (uint32_t i = 0; i < 4; ++i)
                if (want & (1u << i)) {
                    crts[i] = rt_color(r.surf_info, i == 0 ? r.color_info : r.color_info_mrt[i]);
                    if (!crts[i]) fail = true;
                    nrt = i + 1;
                }
            Rt* drt = rt_depth(r.surf_info, r.depth_info);
            if (!drt || fail) { g_rp[RP_RT] += gp_now() - t_rt; ++no_rt; continue; }
            dt.nrt = nrt; dt.rt_w = drt->t.w; dt.rt_h = kRtH;
            const GLuint fbo = draw_fbo(crts, nrt, drt);
            g_sc.sync();
            g_sc.bind_fbo(fbo);
        } else {
            g_sc.sync();
            g_sc.bind_fbo(g_scene_fbo);
        }
        g_rp[RP_RT] += gp_now() - t_rt;
        // 4. Program, constants, textures, state, draw.
        const uint64_t t_bind = gp_now();
        g_sc.use_program(prog);
        if (!swz.empty() && !view_swizzle_on()) {   // system constants with the resolved textures' swizzles
            uint8_t sys[kLswGlClipPlaneOffset];
            memcpy(sys, f.arena.data() + r.cb_sys_off, sizeof sys);
            uint32_t* swizzles = reinterpret_cast<uint32_t*>(sys + 96);   // SystemConstants::texture_swizzles
            for (const auto& s : swz) {
                const uint32_t sh = 12 * (s.first & 1);
                swizzles[s.first >> 1] = (swizzles[s.first >> 1] & ~(0xFFFu << sh)) | ((s.second & 0xFFFu) << sh);
            }
            if (patch_off + GLintptr(sizeof sys) > kPatchCap) patch_off = 0;
            glBindBuffer(GL_UNIFORM_BUFFER, patch_buf);
            glBufferSubData(GL_UNIFORM_BUFFER, patch_off, sizeof sys, sys);
            g_sc.bind_ubo(0, patch_buf, patch_off, sizeof sys);
            patch_off += sizeof sys;
            ++patched;
        } else {
            g_sc.bind_ubo(0, g_arena, r.cb_sys_off, kLswGlClipPlaneOffset);
        }
        g_sc.bind_ubo(5, g_arena, r.cb_sys_off + kLswGlClipPlaneOffset, kLswGlClipPlaneSize);
        g_sc.bind_ubo(1, g_arena, r.cb_vsf_off, std::max<uint32_t>(r.cb_vsf_size, 16));
        g_sc.bind_ubo(2, g_arena, r.cb_psf_off, std::max<uint32_t>(r.cb_psf_size, 16));
        g_sc.bind_ubo(3, g_arena, r.cb_bool_off, 40 * 4);
        g_sc.bind_ubo(4, g_arena, r.cb_fetch_off, 192 * 4);
        for (const UnitBind& u : units) g_sc.bind_unit(u.unit, u.target, u.tex, u.sampler, view_swizzle_on(), u.swz);
        g_rp[RP_BIND] += gp_now() - t_bind;
        {
            RpScope rp(RP_STATE);
            StateKey k;
            memset(&k, 0, sizeof k);   // padding too: compared with memcmp
            k.blend[0] = r.blendcontrol;
            for (int i = 1; i < 4; ++i) k.blend[i] = r.blend_mrt[i];
            k.color_mask = r.color_mask; k.has_ps = r.ps_dxbc != nullptr; k.ps_rt_mask = r.ps_rt_mask;
            k.depthcontrol = r.depthcontrol; k.stencil_ref = r.stencil_ref; k.sc_mode_cntl = r.sc_mode_cntl;
            k.dbg = uint32_t(gl_dbg());
            memcpy(k.viewport, r.viewport, sizeof k.viewport);
            memcpy(k.scissor, r.scissor, sizeof k.scissor);
            k.dt = dt;
            if (!g_sc.key_valid || memcmp(&k, &g_sc.key, sizeof k) != 0) {
                apply_state(r, dt);
                ++g_sc_changes[SC_STATE];
                g_sc.key = k;
                g_sc.key_valid = true;
            }
        }
        if (g_capture_frame) {
            printf("[GLDRAWCAP] #%u prim %u gs %u n=%u ib=%u mode=%u pitch=%u msaa=%u col=%u/%u dep=%u nrt=%u blend=%08X cmask=%X "
                   "zctl=%08X cull=%u vp=%.0f,%.0f,%.0fx%.0f sc=%u,%u..%u,%u ps=%s vs=%016llX ps=%016llX gs=%016llX prog=%u |%s\n", draw_i - 1,
                   r.topology, r.gs_type, r.vertex_count,
                   r.ib_count, r.edram_mode, r.surf_info & 0x3FFF, (r.surf_info >> 16) & 3, r.color_info & 0xFFF,
                   (r.color_info >> 16) & 0xF, r.depth_info & 0xFFF, dt.nrt, r.blendcontrol, r.color_mask, r.depthcontrol,
                   r.sc_mode_cntl & 7, r.viewport[0], r.viewport[1], r.viewport[2], r.viewport[3],
                   r.scissor[0], r.scissor[1], r.scissor[2], r.scissor[3], ps ? "y" : "n",
                   (unsigned long long)vs->key, (unsigned long long)(ps ? ps->key : 0), (unsigned long long)(gs ? gs->key : 0), prog,
                   g_unit_log.c_str());
            dump_capture_glsl(vs);
            dump_capture_glsl(ps);
            dump_capture_glsl(gs);
        }
        const GLenum mode = topology(r.topology);
        // DIAG (LSWTCS_GLTIMING=1): the first draw of each program, finished, includes mesa's lazy backend
        // compile (TGSI -> GPU code happens at validation, not at link).
        static const bool timing = env_on("LSWTCS_GLTIMING", false);
        static std::unordered_map<GLuint, bool> drawn_once;
        const bool first_use = timing && !drawn_once[prog];
        uint64_t t_first = 0;
        if (first_use) { glFinish(); t_first = gp_now(); drawn_once[prog] = true; }
        {
            RpScope rp(RP_DRAW);
            // Index source: the arena, or guest memory in shared memory (r.ib_shm, kernel_stubs LSWTCS_IBSHM).
            const GLuint ebo = r.ib_shm ? g_shm : g_arena;
            if (r.ib_count && ebo != ebo_bound) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo); ebo_bound = ebo; }
            if (g_gpuprof_nopix) { glScissor(0, 0, 1, 1); ++g_gl_epoch; }   // DIAG: the next draw re-sets its scissor
            GLsizei count = GLsizei(r.ib_count ? r.ib_count : r.vertex_count);
            if (g_gpuprof_frame) g_gpuprof_verts += uint64_t(count);
            if (g_gpuprof_novtx) count = std::min<GLsizei>(count, 6);   // DIAG
            if (r.ib_count)
                glDrawElements(mode, count, r.ib_32bit ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                               reinterpret_cast<const void*>(uintptr_t(r.ib_off)));
            else
                glDrawArrays(mode, 0, count);
        }
        if (first_use) {
            glFinish();
            printf("[GLPROGT] program %u first draw: %llu ms (VS %zu KB, PS %zu KB)\n", prog,
                   (unsigned long long)((gp_now() - t_first) / 1000000), vs->glsl.size() / 1024, ps ? ps->glsl.size() / 1024 : 0);
            fflush(stdout);
        }
        ++by_gs[r.gs_type & 3];
        gpu_stamp(uint8_t(GK_PLAIN + (r.gs_type & 3)), prog, ps ? ps->key : 0);
        static const bool gltest = env_on("LSWTCS_GLTEST", false);
        static bool first_done = false;
        if (gltest && !first_done) {
            first_done = true;
            glFinish();
            printf("[GLTEST] first game draw: ok (err 0x%X)\n", glGetError());
            fflush(stdout);
        }
        ++drawn;
    }
    // Present source: the resolve target at the frontbuffer address, else colour RT 0 of pitch 1280.
    const char* shown = "scene";
    if (gpurt) {
        note_dest_used(f.swap_fb & 0x1FFFFFFFu);   // the presented frame is a use
        auto it = g_resolved.find(f.swap_fb & 0x1FFFFFFFu);
        if (f.swap_fb && it != g_resolved.end() && it->second.t.tex) {
            present = {single_fbo(it->second.t), std::min<uint32_t>(it->second.t.w, kSceneW), std::min<uint32_t>(it->second.t.h, kSceneH)};
            shown = "resolve";
        } else {
            auto ct = g_rts.find((uint64_t(0) << 40) | (uint64_t(1280) << 16) | uint64_t(kDxgiRGBA8));
            if (ct != g_rts.end() && ct->second.t.tex) { present = {single_fbo(ct->second.t), kSceneW, kSceneH}; shown = "RT0"; }
        }
        if (g_num_workers > 0 && !prog_wait_mode()) {   // keep this complete frame for frames held while programs compile
            if (!g_last_good.tex) g_last_good = host_tex_create(kSceneW, kSceneH, kDxgiRGBA8);
            glDisable(GL_SCISSOR_TEST);
            glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, present.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, single_fbo(g_last_good));
            glBlitFramebuffer(0, 0, GLint(present.w), GLint(present.h), 0, 0, GLint(present.w), GLint(present.h),
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glEnable(GL_SCISSOR_TEST);
        }
    }
    {   // DIAG: replays that stall, and draws skipped because their program is still compiling
        const uint64_t ms = (gp_now() - replay_t0) / 1000000;
        // A captured replay is slow because of the capture (per-draw log): it must not re-arm the
        // post-stall captures (that looped: one 5 s capture every 60 replays on the Switch, 2026-10-08).
        if (ms >= 1000 && !g_capture_frame) g_stall_replay = g_replays;
        static const uint64_t stall_ms = [] { const char* e = getenv("LSWTCS_GLSTALLMS"); return uint64_t(e ? atoi(e) : 250); }();
        if (ms >= stall_ms || (g_prog_pending_draws && (g_replays % 60) == 0)) {
            size_t queued;
            { std::lock_guard<std::mutex> lk(g_prog_mtx); queued = g_prog_queue.size(); }
            // Split of this replay: leaf stages in ms, "other" = the rest of replay().
            char split[384];
            int n = 0;
            uint64_t staged = 0;
            for (int k = 0; k < RP_COUNT; ++k) {
                const uint64_t d = g_rp[k] - rp0[k];
                staged += d;
                if (d >= 1000000) n += snprintf(split + n, sizeof split - n, " %s=%llu", kRpName[k], (unsigned long long)(d / 1000000));
            }
            const uint64_t total = gp_now() - replay_t0;
            snprintf(split + n, sizeof split - n, " other=%llu", (unsigned long long)((total > staged ? total - staged : 0) / 1000000));
            uint64_t ml_ns, ml_cont, ml_calls;
            lsw_malloc_lock_stats(&ml_ns, &ml_cont, &ml_calls);
            printf("[GLSTALL] replay %llu took %llu ms, %u draws, %u skipped (program compiling), %zu programs queued | "
                   "built %u programs, %u textures |%s | malloc lock: %llu ms waited, %llu of %llu contended\n",
                   (unsigned long long)g_replays, (unsigned long long)ms, drawn, g_prog_pending_draws, queued,
                   g_prog_compiled - prog0, g_tex_created - tex0, split, (unsigned long long)((ml_ns - ml_ns0) / 1000000),
                   (unsigned long long)(ml_cont - ml_cont0), (unsigned long long)(ml_calls - ml_calls0));
            fflush(stdout);
        }
    }
    if (g_replays <= 5 || (g_replays % 120) == 0) {
        {
            std::lock_guard<std::mutex> lk(g_prog_mtx);
            printf("[GLPROG] %u compiled (%llu ms of worker time), %u from binary cache, %u binaries written, %u boot-queued, "
                   "%zu still queued, %zu stages compiled, %u stage compiles saved by reuse\n", g_prog_compiled,
                   (unsigned long long)(g_prog_compile_ns / 1000000), g_prog_from_binary, g_prog_cache_written,
                   g_prog_boot_queued, g_prog_queue.size(), g_stages.size(), g_stage_reused);
        }
        printf("[GLDRAW] replay %llu: %u drawn (plain %u / points %u / rects %u / quads %u), %u no program, %u no GS, %u no RT, "
               "%u resolves (%u copies skipped as dead), %u dead draws skipped, %u resolved-texture binds, %u swizzle patches, "
               "%u shm copies, arena %zu KB, shown %s (fb 0x%08X)\n",
               (unsigned long long)g_replays, drawn, by_gs[0], by_gs[1], by_gs[2], by_gs[3], no_prog, no_gs, no_rt, resolves,
               dead_resolves, dead_draws,
               g_res_tex_binds, patched, copies, f.arena.size() / 1024, shown, f.swap_fb);
        {   // DIAG: draws that repeat an earlier draw of this frame exactly (shaders, constants, geometry, render
            // target) apart from scissor/viewport: the signature of predicated tiling replayed once per tile
            // (cutscenes: ~7000 draws vs ~800 in gameplay, Switch 2026-10-09).
            std::unordered_map<uint64_t, uint32_t> seen;   // key -> first draw index
            uint32_t dups = 0, dups_other_scissor = 0;
            std::unordered_map<uint64_t, uint32_t> scissors;   // scissor rect -> draws
            for (uint32_t i = 0; i < f.draws.size(); ++i) {
                const LswGpuDrawRec& r = f.draws[i];
                if (r.kind != 0) continue;
                const uint64_t k = draw_content_key(f, r);
                const uint64_t sk = (uint64_t(r.scissor[0]) << 48) ^ (uint64_t(r.scissor[1]) << 32) ^ (uint64_t(r.scissor[2]) << 16) ^ r.scissor[3];
                ++scissors[sk];
                auto it = seen.find(k);
                if (it == seen.end()) { seen.emplace(k, i); continue; }
                ++dups;
                const LswGpuDrawRec& o = f.draws[it->second];
                if (memcmp(o.scissor, r.scissor, sizeof r.scissor) != 0) ++dups_other_scissor;
            }
            std::vector<std::pair<uint32_t, uint64_t>> top;
            for (const auto& kv : scissors) top.push_back({kv.second, kv.first});
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            char sc[256]; int len = 0;
            for (size_t q = 0; q < top.size() && q < 4; ++q)
                len += snprintf(sc + len, sizeof sc - len, " [%u,%u %ux%u]x%u", unsigned(top[q].second >> 48),
                                unsigned((top[q].second >> 32) & 0xFFFF), unsigned((top[q].second >> 16) & 0xFFFF),
                                unsigned(top[q].second & 0xFFFF), top[q].first);
            printf("[GLDRAW]   repeats: %u of %zu draws repeat an earlier draw (%u with a different scissor); "
                   "%zu scissor rects, top:%s | untiled: %u tile groups, %u tile repeats skipped\n", dups, f.draws.size(),
                   dups_other_scissor, scissors.size(), sc, ut.groups, ut.skipped);
        }
        printf("[GLDRAW]   %zu programs, %u shaders compiled (%u failed, %u link failed), %zu textures (%llu MB, %u evicted), "
               "%zu RTs + %zu resolve targets + %zu views + %zu composites = %llu MB, arena buffers %lld MB, err 0x%X\n",
               g_programs.size(), g_compiled, g_compile_failed, g_link_failed, g_textures.size(),
               (unsigned long long)(g_tex_bytes >> 20), g_tex_evicted, g_rts.size(), g_resolved.size(), g_views.size(),
               g_composed.size(), (unsigned long long)(g_rt_bytes >> 20), (long long)(g_arena_cap >> 20), glGetError());
        fflush(stdout);
    }
    frame_log(f, gp_now() - replay_t0);
    return present;
}

// ---- DIAG: scene capture ----------------------------------------------------------------------
// File "shot_now" in sdmc:/lswtcs (or automatically at replays 300 and 1500 with LSWTCS_GLTEST=1):
// the scene FBO -> shot_<replay>.png (rows top-down = the guest image) plus a count of pixels that
// differ from the clear colour, so the GL output can be inspected without Eden's window.
uint32_t png_crc(const uint8_t* p, size_t n, uint32_t crc) {
    static uint32_t T[256]; static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1); T[i] = c; }
        init = true;
    }
    for (size_t i = 0; i < n; i++) crc = T[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
void png_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t len) {
    const uint8_t lb[4] = {uint8_t(len >> 24), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len)};
    fwrite(lb, 1, 4, f);
    uint32_t crc = png_crc(reinterpret_cast<const uint8_t*>(type), 4, 0xFFFFFFFFu);
    fwrite(type, 1, 4, f);
    if (len) { crc = png_crc(data, len, crc); fwrite(data, 1, len, f); }
    crc ^= 0xFFFFFFFFu;
    const uint8_t cb[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
    fwrite(cb, 1, 4, f);
}
void write_png_rgba(const char* path, const uint8_t* rgba, int w, int h) {   // zlib "stored" blocks
    FILE* f = fopen(path, "wb");
    if (!f) return;
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    const uint8_t ihdr[13] = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                              uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 6, 0, 0, 0};
    png_chunk(f, "IHDR", ihdr, 13);
    // IDAT streamed in stored deflate blocks through a 64 KB buffer: no full-size copies of the image (captures used
    // to build two, on a heap that may not have such blocks left; Switch 2026-10-09).
    const size_t stride = 1 + size_t(w) * 4, raw_len = size_t(h) * stride;
    const size_t nblk = raw_len ? (raw_len + 65534) / 65535 : 1;
    const uint32_t idat_len = uint32_t(2 + nblk * 5 + raw_len + 4);
    const uint8_t lb[4] = {uint8_t(idat_len >> 24), uint8_t(idat_len >> 16), uint8_t(idat_len >> 8), uint8_t(idat_len)};
    fwrite(lb, 1, 4, f);
    uint32_t crc = png_crc(reinterpret_cast<const uint8_t*>("IDAT"), 4, 0xFFFFFFFFu);
    auto put = [&](const uint8_t* d, size_t n) { crc = png_crc(d, n, crc); fwrite(d, 1, n, f); };
    fwrite("IDAT", 1, 4, f);
    const uint8_t zh[2] = {0x78, 0x01};
    put(zh, 2);
    static uint8_t buf[65535];
    uint32_t a = 1, b = 0;
    size_t pos = 0;
    do {
        const size_t blk = std::min<size_t>(raw_len - pos, 65535u);
        const uint8_t bh[5] = {uint8_t(pos + blk >= raw_len ? 1 : 0), uint8_t(blk), uint8_t(blk >> 8), uint8_t(~blk), uint8_t(~blk >> 8)};
        put(bh, 5);
        for (size_t i = 0; i < blk; ++i) {   // raw stream byte pos+i: filter byte 0 at each row start, then the row
            const size_t r = (pos + i) / stride, c = (pos + i) % stride;
            buf[i] = c ? rgba[r * size_t(w) * 4 + c - 1] : 0;
            a = (a + buf[i]) % 65521; b = (b + a) % 65521;
        }
        put(buf, blk);
        pos += blk;
    } while (pos < raw_len);
    const uint32_t adler = (b << 16) | a;
    const uint8_t ad[4] = {uint8_t(adler >> 24), uint8_t(adler >> 16), uint8_t(adler >> 8), uint8_t(adler)};
    put(ad, 4);
    crc ^= 0xFFFFFFFFu;
    const uint8_t cb[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
    fwrite(cb, 1, 4, f);
    png_chunk(f, "IEND", nullptr, 0);
    fclose(f);
}
void maybe_capture(const PresentSrc& src) {
    if (!g_capture_frame) return;
    const uint32_t w = src.w, h = src.h;
    std::vector<uint8_t> px;
    try { px.resize(size_t(w) * h * 4); }
    catch (const std::bad_alloc&) { printf("[GLSHOT] skipped: out of memory\n"); fflush(stdout); return; }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src.fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    uint32_t differ = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i + 3] = 255;   // alpha as the window shows it (opaque)
        if (px[i] != 13 || px[i + 1] != 13 || px[i + 2] != 20) ++differ;
    }
    char path[64];
    snprintf(path, sizeof path, "shot_%llu.png", (unsigned long long)g_replays);
    write_png_rgba(path, px.data(), int(w), int(h));
    printf("[GLSHOT] %s (%ux%u): %u pixels differ from the clear colour (err 0x%X)\n", path, w, h, differ, glGetError());
    fflush(stdout);
}

// DIAG: file "resdump_now" in sdmc:/lswtcs -> every resolve target and colour render target of this
// replay as res_<dest>_<w>x<h>.png, named and normalised as the PC's resdump_now (gpu_xedraw.inc
// xd_dump_queue): render targets as 0xEE000000 | base << 12 | width, first 720 rows; R32F (depth
// resolves) min..max normalised.
void maybe_resdump() {
    // LSWTCS_RESDUMPAT=N: dump at replay N without the trigger file (Switch hardware).
    static const uint64_t dump_at = [] { const char* e = getenv("LSWTCS_RESDUMPAT"); return uint64_t(e ? atoll(e) : 0); }();
    if (!(dump_at && g_replays == dump_at)) {
        if (g_replays % 30) return;
        FILE* t = fopen("resdump_now", "rb");
        if (!t) return;
        fclose(t);
        remove("resdump_now");
    }
    // Depth images share one scale (LSWTCS_RESDUMPZ=lo,hi, default 0.95..1.0) so values compare across images;
    // the per-image min..max normalisation made tile seams look like depth jumps.
    static const std::pair<float, float> zr = [] {
        float lo = 0.95f, hi = 1.0f;
        if (const char* e = getenv("LSWTCS_RESDUMPZ")) sscanf(e, "%f,%f", &lo, &hi);
        return std::make_pair(lo, hi);
    }();
    // Read back in strips of 64 rows: a whole target as floats was 15-21 MB in one block, and after a long session the
    // console's heap had no such block (Switch 2026-10-09: std::bad_alloc in a capture aborted the game). A target that
    // still does not fit is skipped and logged.
    auto dump = [](const HostTex& tex, uint32_t name, uint32_t rows, const char* prefix = "res") {
        const uint32_t w = tex.w, h = std::min(tex.h, rows);
        constexpr uint32_t kStrip = 64;
        const bool depth = tex.fmt == kDxgiR32F;
        const float zlo = zr.first, zhi = zr.second;
        float zmin = 1e30f, zmax = -1e30f;
        std::vector<float> seam;   // depth composites: centre column rows 308..332
        std::vector<float> f;
        std::vector<uint8_t> px;
        try {
            f.resize(size_t(w) * kStrip * 4);
            px.resize(size_t(w) * h * 4);
        } catch (const std::bad_alloc&) {
            printf("[RESDUMP] %s_%08X_%ux%u skipped: out of memory\n", prefix, name, w, h);
            return;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, single_fbo(tex));
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        for (uint32_t y0 = 0; y0 < h; y0 += kStrip) {
            const uint32_t sh = std::min(kStrip, h - y0);
            glReadPixels(0, GLint(y0), GLsizei(w), GLsizei(sh), GL_RGBA, GL_FLOAT, f.data());
            const size_t n = size_t(w) * sh * 4;
            if (depth) {
                for (size_t i = 0; i < n; i += 4) { zmin = std::min(zmin, f[i]); zmax = std::max(zmax, f[i]); }
                if (prefix[0] == 'c')
                    for (uint32_t y = std::max(y0, 308u); y <= 332 && y < y0 + sh; ++y) seam.push_back(f[(size_t(y - y0) * w + w / 2) * 4]);
            }
            uint8_t* out = px.data() + size_t(y0) * w * 4;
            for (size_t i = 0; i < n; i += 4) {
                for (int c = 0; c < 3; ++c) {
                    float v = depth ? (zhi > zlo ? (f[i] - zlo) / (zhi - zlo) : 0.0f) : f[i + c];
                    if ((tex.fmt == kDxgiR8) && c) v = f[i];
                    if (tex.fmt == kDxgiRG8 && c == 2) v = 0.0f;
                    out[i + c] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                out[i + 3] = 255;
            }
        }
        char path[64];
        snprintf(path, sizeof path, "%s_%08X_%ux%u.png", prefix, name, w, h);
        write_png_rgba(path, px.data(), int(w), int(h));
        printf("[RESDUMP] %s fmt=%u%s", path, tex.fmt, depth ? " (depth, normalized)" : "");
        if (depth) printf(" range %.6f .. %.6f (png scale %.3f..%.3f)", zmin, zmax, zlo, zhi);
        printf("\n");
        if (seam.size() == 25) {   // raw values of the centre column around the first tile seam, for depth composites
            printf("[RESDUMP]   %s centre column rows 308..332:", path);
            for (uint32_t y = 0; y < 25; ++y) printf(" %u=%.6f", 308 + y, seam[y]);
            printf("\n");
        }
    };
    for (const auto& kv : g_resolved)
        if (kv.second.t.tex) dump(kv.second.t, kv.first, kv.second.t.h);
    for (const auto& kv : g_composed)   // what the post passes actually sample (tiles stitched)
        if (kv.second.t.tex) dump(kv.second.t, kv.first, kv.second.t.h, "cmp");
    for (const auto& kv : g_rts)
        if (kv.second.t.tex && !kv.second.depth)
            dump(kv.second.t, 0xEE000000u | uint32_t((kv.first >> 40) & 0xFFF) << 12 | kv.second.t.w, 720);
    printf("[RESDUMP] done at replay %llu (err 0x%X)\n", (unsigned long long)g_replays, glGetError());
    fflush(stdout);
}

void present_frame(const PresentSrc& src) {
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (src.w < kSceneW || src.h < kSceneH) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    // Source rows are top-down; the window's are bottom-up.
    glBlitFramebuffer(0, 0, GLint(src.w), GLint(src.h), 0, kSceneH, GLint(src.w), GLint(kSceneH - src.h),
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
    glEnable(GL_SCISSOR_TEST);
}

// DIAG (LSWTCS_GLTEST=1): before the game draws, run a ladder of small programs, one shader feature of
// the translated shaders at a time. Each step clears a 64x64 corner of the scene, draws a triangle
// covering it, and counts the pixels that changed: Eden runs some programs but silently drops draws
// whose mesa-compiled shader its recompiler cannot decode ("Invalid insn" in Eden's log), and an
// early `return` in a geometry shader hangs it outright (do/while(false) form used instead).
// Expected per step: 4096 pixels.
void gl_self_test() {
    static const char* const kVs =
        "#version 430\n"
        "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.5, 1.0); }\n";
    static const char* const kFs =
        "#version 430\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(1.0, 0.0, 1.0, 1.0); }\n";
    static const char* const kUbo = "layout(binding = 0, std140) uniform U { vec4 c[4]; uint k; } u;\n";
    static const char* const kSsbo = "layout(binding = 0, std430) restrict readonly buffer S { uint m[]; } s[4];\n";
    struct Step { const char* name; std::string vs, gs, fs; };
    const std::string vhead = "#version 430\n", fhead = "#version 430\nlayout(location = 0) out vec4 o;\n";
    const std::string vpos = "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n";
    const std::string vend = "  gl_Position = vec4(p * 2.0 - 1.0, 0.5, 1.0);\n}\n";
    const Step steps[] = {
        {"baseline", kVs, "", kFs},
        // Pixel shader features.
        {"PS uniform block", kVs, "", fhead + kUbo + "void main() { o = u.c[u.k & 3u] + vec4(1.0, 0.0, 1.0, 1.0); }\n"},
        {"PS storage buffer array", kVs, "", fhead + kSsbo +
            "void main() { uint a = uint(gl_FragCoord.x); o = unpackUnorm4x8(s[(a >> 5) & 3u].m[a & 31u]) + vec4(1, 0, 1, 1); }\n"},
        {"PS storage buffer const index", kVs, "", fhead + kSsbo +
            "void main() { uint a = uint(gl_FragCoord.x); o = unpackUnorm4x8(s[1].m[a & 31u]) + vec4(1, 0, 1, 1); }\n"},
        {"PS storage buffer via switch", kVs, "", fhead + kSsbo +
            "uint ld(uint i, uint j) { switch (i) { case 0u: return s[0].m[j]; case 1u: return s[1].m[j];\n"
            "  case 2u: return s[2].m[j]; default: return s[3].m[j]; } }\n"
            "void main() { uint a = uint(gl_FragCoord.x); o = unpackUnorm4x8(ld((a >> 5) & 3u, a & 31u)) + vec4(1, 0, 1, 1); }\n"},
        {"VS storage buffer dyn, used", vhead + kSsbo + "void main() {\n" + vpos +
            "  uint x = s[uint(gl_VertexID) & 3u].m[uint(gl_VertexID) * 7u]; if (x == 0xDEADBEEFu) p = vec2(-5.0);\n" + vend, "", kFs},
        {"VS storage buffer switch, used", vhead + kSsbo +
            "uint ld(uint i, uint j) { switch (i) { case 0u: return s[0].m[j]; case 1u: return s[1].m[j];\n"
            "  case 2u: return s[2].m[j]; default: return s[3].m[j]; } }\n"
            "void main() {\n" + vpos +
            "  uint x = ld(uint(gl_VertexID) & 3u, uint(gl_VertexID) * 7u); if (x == 0xDEADBEEFu) p = vec2(-5.0);\n" + vend, "", kFs},
        {"PS small storage buffer", kVs, "", fhead +
            "layout(binding = 7, std430) restrict readonly buffer T { uint m[]; } t7;\n"
            "void main() { uint a = uint(gl_FragCoord.x); o = unpackUnorm4x8(t7.m[a & 31u]) + vec4(1, 0, 1, 1); }\n"},
        {"PS texel buffer (shared mem)", kVs, "", fhead + "layout(binding = 9) uniform usamplerBuffer tb;\n"
            "void main() { uint a = uint(gl_FragCoord.x) * 4099u; o = unpackUnorm4x8(texelFetch(tb, int(a)).x) + vec4(1, 0, 1, 1); }\n"},
        {"VS texel buffer (shared mem)", vhead + "layout(binding = 9) uniform usamplerBuffer tb;\n" + "void main() {\n" + vpos +
            "  uint x = texelFetch(tb, gl_VertexID * 4099 + 100000000).x; if (x == 0xDEADBEEFu) p = vec2(-5.0);\n" + vend, "", kFs},
        {"PS loop + switch", kVs, "", fhead +
            "void main() { vec4 r = vec4(0.0); uint pc = 0u;\n"
            "  while (true) { switch (pc) { case 0u: r.x += 0.25; pc = (gl_FragCoord.x > 9.0) ? 1u : 2u; break;\n"
            "    case 1u: r.y += 0.5; pc = 2u; break; default: pc = 99u; break; }\n"
            "    if (pc == 99u) break; }\n  o = r + vec4(0.5, 0.0, 0.5, 1.0); }\n"},
        {"PS texture array", kVs, "", fhead + "layout(binding = 0) uniform sampler2DArray t;\n"
            "void main() { o = texture(t, vec3(gl_FragCoord.xy / 64.0, 0.0)) + vec4(1, 0, 1, 1); }\n"},
        {"PS precise", kVs, "", fhead +
            "void main() { precise float a = gl_FragCoord.x * 0.5 + gl_FragCoord.y; o = vec4(fract(a), 0.0, 1.0, 1.0); }\n"},
        // Vertex shader features (the game's VS draws were dropped).
        {"VS uniform dynamic index", vhead + kUbo + "void main() {\n" + vpos +
            "  p += u.c[uint(gl_VertexID) & 3u].xy;\n" + vend, "", kFs},
        {"VS storage buffer array", vhead + kSsbo + "void main() {\n" + vpos +
            "  p += vec2(float(s[uint(gl_VertexID) & 3u].m[uint(gl_VertexID) * 7u]) * 0.0);\n" + vend, "", kFs},
        {"VS bitfieldInsert + shifts", vhead + "void main() {\n" + vpos +
            "  uint x = uint(gl_VertexID); uint y = ((x >> 8u) & 16711935u) | ((x & 16711935u) << 8u);\n"
            "  y = bitfieldInsert(y >> 16u, y, 16, 16); p += vec2(float(y == 0xFFFFFFFFu));\n" + vend, "", kFs},
        {"VS for(;;) break", vhead + "void main() {\n" + vpos +
            "  for (;;) { p *= 1.0; break; }\n" + vend, "", kFs},
        {"VS precise + reciprocal", vhead + "void main() {\n" + vpos +
            "  precise float w = 1.0 / (1.0 + float(gl_VertexID) * 0.0); precise vec2 q = p * w;\n"
            "  gl_Position = vec4(q * 2.0 - 1.0, 0.5, w);\n}\n", "", kFs},
        {"VS local array dynamic index", vhead + "void main() {\n" + vpos +
            "  vec4 r[4]; r[0] = vec4(0.0); r[1] = vec4(0.0); r[2] = vec4(0.0); r[3] = vec4(0.0);\n"
            "  r[gl_VertexID & 3].x = 0.0; p += r[(gl_VertexID + 1) & 3].xy;\n" + vend, "", kFs},
        // Geometry shaders.
        {"GS triangles -> strip", kVs,
            "#version 430\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 4) out;\n"
            "void main() { for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position; EmitVertex(); }\n"
            "  gl_Position = gl_in[0].gl_Position + gl_in[2].gl_Position - gl_in[1].gl_Position; EmitVertex();\n"
            "  EndPrimitive(); }\n", kFs},
        {"GS do/break exit", kVs,
            "#version 430\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
            "void main() { do { if (any(isnan(gl_in[0].gl_Position))) { break; }\n"
            "  for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position; EmitVertex(); }\n"
            "  EndPrimitive(); } while (false); }\n", kFs},
    };
    GLuint ubo;
    glGenBuffers(1, &ubo);
    const uint32_t ubo_data[20] = {};
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof ubo_data, ubo_data, GL_STATIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
    GLuint small_ssbo;   // binding 7: a 4 KB storage buffer
    glGenBuffers(1, &small_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, small_ssbo);
    std::vector<uint32_t> small(1024, 0x11223344u);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(small.size() * 4), small.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, small_ssbo);
    GLint tbo_max = 0;   // unit 9: R32UI buffer texture over the shared-memory buffer
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &tbo_max);
    GLuint tbo;
    glGenTextures(1, &tbo);
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_BUFFER, tbo);
    const GLsizeiptr tbo_bytes = std::min<GLsizeiptr>(kShmSize, GLsizeiptr(tbo_max) * 4);
    glTexBufferRange(GL_TEXTURE_BUFFER, GL_R32UI, g_shm, 0, tbo_bytes);
    printf("[GLTEST] max texture buffer %d texels (%lld MB as R32UI), bound %lld MB (err 0x%X)\n", tbo_max,
           (long long)(GLsizeiptr(tbo_max) * 4 >> 20), (long long)(tbo_bytes >> 20), glGetError());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_dummy[0]);
    glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
    glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    glViewport(0, 0, 64, 64);
    glScissor(0, 0, 64, 64);
    glEnable(GL_SCISSOR_TEST);
    std::vector<uint8_t> px(64 * 64 * 4);
    auto compile = [](GLenum type, const std::string& src) {
        GLuint s = glCreateShader(type);
        const char* c = src.c_str();
        glShaderSource(s, 1, &c, nullptr);
        glCompileShader(s);
        return s;
    };
    for (const Step& s : steps) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        GLuint p = glCreateProgram();
        GLuint v = compile(GL_VERTEX_SHADER, s.vs), f = compile(GL_FRAGMENT_SHADER, s.fs);
        GLuint g = s.gs.empty() ? 0 : compile(GL_GEOMETRY_SHADER, s.gs);
        glAttachShader(p, v); glAttachShader(p, f);
        if (g) glAttachShader(p, g);
        glLinkProgram(p);
        GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        uint32_t changed = 0;
        if (ok) {
            glUseProgram(p);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glFinish();
            glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (size_t i = 0; i < px.size(); i += 4)
                if (px[i] != 13 || px[i + 1] != 13 || px[i + 2] != 20) ++changed;
        }
        printf("[GLTEST] %-28s %s %4u/4096 pixels (err 0x%X)\n", s.name, ok ? "drawn" : "LINK FAILED", changed, glGetError());
        fflush(stdout);
        glDeleteProgram(p); glDeleteShader(v); glDeleteShader(f);
        if (g) glDeleteShader(g);
    }
    glUseProgram(0);
    glDeleteBuffers(1, &ubo);
    // The shared-memory bindings 0-3 were not touched (the S block above reads the real buffer).
}

// DIAG (LSWTCS_GLTEST=1 too): the GLSL of the first linked game program -> sdmc:/lswtcs/gl_first.*
void dump_first_program(const LswGlShader* vs, const LswGlShader* ps, const LswGlShader* gs) {
    static bool done = false;
    if (done) return;
    done = true;
    const LswGlShader* all[3] = {vs, ps, gs};
    static const char* const kNames[3] = {"gl_first.vert", "gl_first.frag", "gl_first.geom"};
    for (int i = 0; i < 3; ++i)
        if (all[i])
            if (FILE* f = fopen(kNames[i], "w")) { fputs(all[i]->glsl.c_str(), f); fclose(f); }
    printf("[GLTEST] first game program written to gl_first.*\n");
    fflush(stdout);
    // Split it: game VS + trivial FS, then trivial VS + game GS + trivial FS.
    static const char* const kVs =
        "#version 430\nvoid main() { gl_Position = vec4(float(gl_VertexID) * 0.1, 0.0, 0.0, 1.0); }\n";
    static const char* const kFs =
        "#version 430\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(1.0); }\n";
    for (int variant = 0; variant < 2; ++variant) {
        if (variant == 1 && !gs) break;
        const char* name = variant == 0 ? "game VS alone" : "game GS alone";
        printf("[GLTEST] %s: drawing...\n", name);
        fflush(stdout);
        GLuint p = glCreateProgram();
        GLuint tv = 0, tf = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(tf, 1, &kFs, nullptr); glCompileShader(tf); glAttachShader(p, tf);
        if (variant == 0) {
            glAttachShader(p, shader_object(vs));
        } else {
            tv = glCreateShader(GL_VERTEX_SHADER);
            glShaderSource(tv, 1, &kVs, nullptr); glCompileShader(tv); glAttachShader(p, tv);
            glAttachShader(p, shader_object(gs));
        }
        glLinkProgram(p);
        GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (ok) { glUseProgram(p); glDrawArrays(variant == 0 ? GL_TRIANGLES : GL_POINTS, 0, 3); glFinish(); }
        printf("[GLTEST] %s: %s (err 0x%X)\n", name, ok ? "ok" : "LINK FAILED", glGetError());
        fflush(stdout);
        glDeleteProgram(p); glDeleteShader(tf);
        if (tv) glDeleteShader(tv);
    }
    printf("[GLTEST] drawing the whole first game program...\n");
    fflush(stdout);
}

// DIAG (LSWTCS_GLBENCH=1): shader-pipeline benchmark on the real switch-mesa, before the game draws.
// Every sdmc:/lswtcs/bench/<variant>/<name>.{vert,frag}.glsl (switch/tools/spirv_glsl_test.sh output,
// copied by switch/tools/glbench.sh) is compiled, linked with a stub for the other stage and drawn
// once (1 point, 1x1 scissor, glFinish), which includes nouveau's lazy backend. Mesa's disk cache is
// off in this mode (egl_init). Prints [GLBENCH] per shader and per variant, then "[GLBENCH] done".
void gl_bench() {
    DIR* top = opendir("sdmc:/lswtcs/bench");
    if (!top) { printf("[GLBENCH] no sdmc:/lswtcs/bench\n"); return; }
    std::vector<std::string> variants;
    while (dirent* e = readdir(top)) if (e->d_name[0] != '.') variants.push_back(e->d_name);
    closedir(top);
    std::sort(variants.begin(), variants.end());
    // Zero uniform blocks on the translator's bindings (system, VS floats, PS floats, bool/loop, fetch,
    // clip planes): the shaders run once on zero constants.
    GLuint ubo;
    glGenBuffers(1, &ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    std::vector<uint8_t> zeros(size_t(64) << 10);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(zeros.size()), zeros.data(), GL_STATIC_DRAW);
    for (GLuint i = 0; i < 6; ++i) glBindBufferBase(GL_UNIFORM_BUFFER, i, ubo);
    glBindVertexArray(g_vao);
    glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
    glViewport(0, 0, 1, 1);
    glScissor(0, 0, 1, 1);
    auto read_file = [](const std::string& path, std::string& out) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        char buf[16384];
        size_t n;
        out.clear();
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        fclose(f);
        return true;
    };
    auto compile = [](GLenum type, const std::string& src, bool& ok) {
        GLuint sh = glCreateShader(type);
        const char* c = src.c_str();
        glShaderSource(sh, 1, &c, nullptr);
        glCompileShader(sh);
        GLint st = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &st);
        if (!st) {
            char log[512] = {};
            glGetShaderInfoLog(sh, sizeof log, nullptr, log);
            printf("[GLBENCH]   compile FAILED: %s\n", log);
            ok = false;
        }
        return sh;
    };
    static const std::string kEmptyFs = "#version 430\nvoid main() {}\n";
    for (const std::string& v : variants) {
        const std::string dir = "sdmc:/lswtcs/bench/" + v;
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        std::vector<std::string> files;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 10 && (n.compare(n.size() - 10, 10, ".vert.glsl") == 0 || n.compare(n.size() - 10, 10, ".frag.glsl") == 0))
                files.push_back(n);
        }
        closedir(d);
        std::sort(files.begin(), files.end());
        uint64_t sum_c = 0, sum_l = 0, sum_d = 0, bytes = 0;
        uint32_t failed = 0;
        for (const std::string& n : files) {
            std::string src;
            if (!read_file(dir + "/" + n, src)) continue;
            const bool is_ps = n.compare(n.size() - 10, 10, ".frag.glsl") == 0;
            // The other stage: an empty fragment shader for a VS; for a PS, a VS declaring the PS's
            // `layout(location = N) ... in` variables as outputs.
            std::string partner = kEmptyFs;
            if (is_ps) {
                partner = "#version 430\n";
                for (size_t p = 0; p < src.size();) {
                    size_t e = src.find('\n', p);
                    if (e == std::string::npos) e = src.size();
                    std::string line = src.substr(p, e - p);
                    const size_t in = line.find(" in ");
                    if (line.compare(0, 16, "layout(location ") == 0 && in != std::string::npos) {
                        line.replace(in, 4, " out ");
                        partner += line + "\n";
                    }
                    p = e + 1;
                }
                partner += "void main() { gl_Position = vec4(0.0, 0.0, 0.5, 1.0); }\n";
            }
            bool ok = true;
            const uint64_t t0 = gp_now();
            const GLuint a = compile(GL_VERTEX_SHADER, is_ps ? partner : src, ok);
            const GLuint b = compile(GL_FRAGMENT_SHADER, is_ps ? src : partner, ok);
            const uint64_t t1 = gp_now();
            GLuint prog = 0;
            if (ok) {
                prog = glCreateProgram();
                glAttachShader(prog, a);
                glAttachShader(prog, b);
                glLinkProgram(prog);
                GLint st = 0;
                glGetProgramiv(prog, GL_LINK_STATUS, &st);
                if (!st) {
                    char log[512] = {};
                    glGetProgramInfoLog(prog, sizeof log, nullptr, log);
                    printf("[GLBENCH]   link FAILED: %s\n", log);
                    ok = false;
                }
            }
            const uint64_t t2 = gp_now();
            if (ok) {
                glUseProgram(prog);
                glDrawArrays(GL_POINTS, 0, 1);
                glFinish();
            }
            const uint64_t t3 = gp_now();
            glUseProgram(0);
            if (prog) glDeleteProgram(prog);
            glDeleteShader(a);
            glDeleteShader(b);
            if (!ok) ++failed;
            sum_c += t1 - t0; sum_l += t2 - t1; sum_d += t3 - t2; bytes += src.size();
            printf("[GLBENCH] %s %s %zu B: compile %.1f link %.1f draw %.1f ms%s\n", v.c_str(), n.c_str(), src.size(),
                   (t1 - t0) / 1e6, (t2 - t1) / 1e6, (t3 - t2) / 1e6, ok ? "" : " FAILED");
            fflush(stdout);
        }
        printf("[GLBENCH] TOTAL %s: %zu shaders, %llu KB, compile %.0f link %.0f draw %.0f = %.0f ms, %u failed\n", v.c_str(),
               files.size(), (unsigned long long)(bytes >> 10), sum_c / 1e6, sum_l / 1e6, sum_d / 1e6,
               (sum_c + sum_l + sum_d) / 1e6, failed);
        fflush(stdout);
    }
    glDeleteBuffers(1, &ubo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    printf("[GLBENCH] done\n");
    fflush(stdout);
}

// DIAG (LSWTCS_GLDEPTHPROBE=1): does a depth render target survive the resolve blit (depth texture
// sampled into R32F)? Clears depth to 0.5 (and draws z = 0.25 into the left half), blits, reads back.
void gl_depth_probe() {
    const GLenum fmts[3] = {GL_DEPTH32F_STENCIL8, GL_DEPTH_COMPONENT32F, GL_DEPTH24_STENCIL8};
    const char* const names[3] = {"D32F_S8", "D32F", "D24_S8"};
    HostTex dst = host_tex_create(64, 64, kDxgiR32F);
    for (int i = 0; i < 3; ++i) {
        HostTex src{0, 64, 64, kDxgiD32S8};
        glGenTextures(1, &src.tex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, src.tex);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, fmts[i], 64, 64, 1);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (fmts[i] != GL_DEPTH_COMPONENT32F)
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, fmts[i] == GL_DEPTH_COMPONENT32F ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT,
                                  src.tex, 0, 0);
        glDrawBuffer(GL_NONE);
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearDepthf(0.5f);
        glClear(GL_DEPTH_BUFFER_BIT);
        const float z[4] = {};
        clear_color_tex(dst, z, nullptr);
        blit(src, dst, 0, 0, 64, 64, 0, 0, 0, 1, 1);
        float px[4] = {-1, -1, -1, -1};
        glBindFramebuffer(GL_READ_FRAMEBUFFER, single_fbo(dst));
        glReadPixels(10, 10, 1, 1, GL_RGBA, GL_FLOAT, px);
        printf("[GLDEPTHPROBE] %s: fbo 0x%X, cleared 0.5 -> resolve reads %.4f %.4f %.4f %.4f (err 0x%X)\n", names[i], st,
               px[0], px[1], px[2], px[3], glGetError());
        fflush(stdout);
        glDeleteFramebuffers(1, &fbo);
        forget_fbos_of(src.tex);
        glDeleteTextures(1, &src.tex);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_SCISSOR_TEST);
}

// Render thread: guest swap `n` is replayed and presented (releases a guest waiting in gpu_d3d12_present).
void frame_done(uint64_t n) {
    { std::lock_guard<std::mutex> lk(g_mtx); g_frames_done = n; }
    g_cv.notify_all();
}

// LSWTCS_GLTHREAD (default ON; =0 turns it off): Mesa's glthread on the render context. GL calls on the render thread are
// only recorded; a Mesa worker thread (pinned to core 2) runs the driver work: on hardware the render thread spent
// ~13 ms of its ~32 ms per heavy frame inside Mesa's draw calls (3200 draws, max clocks, 2026-10-09). switch-mesa
// contains glthread but never enables it (no driconf on this EGL platform), so it is switched on directly. Success =
// glapi's dispatch table changed to Mesa's marshal table.
extern "C" void* _glapi_get_context(void);
extern "C" void* _glapi_get_dispatch(void);
extern "C" void _mesa_glthread_init(void* ctx);
extern "C" void* _mesa_get_dispatch(void* ctx);
extern "C" void _glapi_set_dispatch(void* table);
extern "C" void lsw_set_new_thread_core(int core);   // platform_switch.cpp
// DIAG (LSWTCS_GLSYNCLOG=1, glthread on): GL calls glthread cannot queue make the render thread wait for the worker.
// switch-mesa's _mesa_glthread_finish_before reports each one (mesa_glthread_edits.py); [GLSYNC] lists the top
// waits per replay next to [GLPROF]. Only the render context runs glthread, so only the render thread calls this.
// asm label: inside this anonymous namespace an extern "C" *variable* would still get namespaced linkage.
extern "C" void (*lsw_glthread_sync_hook)(const char* func, uint64_t wait_ns) __asm__("lsw_glthread_sync_hook");
struct GlSyncStat { const char* func; uint64_t n, ns; };
GlSyncStat g_glsync[64];
uint32_t g_glsync_used = 0;
void glsync_hook(const char* func, uint64_t wait_ns) {
    for (uint32_t i = 0; i < g_glsync_used; ++i)
        if (g_glsync[i].func == func) { ++g_glsync[i].n; g_glsync[i].ns += wait_ns; return; }
    if (g_glsync_used < 64) g_glsync[g_glsync_used++] = {func, 1, wait_ns};
}
void glsync_report(uint32_t replays) {
    if (!g_glsync_used) return;
    std::sort(g_glsync, g_glsync + g_glsync_used, [](const GlSyncStat& a, const GlSyncStat& b) { return a.ns > b.ns; });
    char line[512];
    int len = 0;
    for (uint32_t i = 0; i < g_glsync_used && i < 8; ++i)
        len += snprintf(line + len, sizeof line - len, " %s %.1fx %.2fms", g_glsync[i].func,
                        double(g_glsync[i].n) / replays, g_glsync[i].ns / 1e6 / replays);
    printf("[GLSYNC] per replay:%s\n", line);
    g_glsync_used = 0;
}
void enable_glthread() {
    // Default ON since the 2026-10-09 hardware run (render thread median 22.8 -> 15.1 ms, p90 43.9 -> 22.0 ms per
    // in-game frame; less stutter). LSWTCS_GLTHREAD=0 turns it off. Needs switch-mesa built with
    // switch/mesa/mesa_glthread_edits.py: stock switch-mesa has no st_manager::set_background_context, so the
    // worker's start-up job called address 0 and _mesa_glthread_init never returned (black screen, Eden 2026-10-09).
    if (!env_on("LSWTCS_GLTHREAD", true)) { printf("[GL] glthread off (LSWTCS_GLTHREAD=0)\n"); return; }
    void* ctx = _glapi_get_context();
    void* before = _glapi_get_dispatch();
    if (!ctx) { printf("[GL] glthread: no current context\n"); return; }
    void* ctx_disp0 = _mesa_get_dispatch(ctx);
    lsw_set_new_thread_core(2);
    _mesa_glthread_init(ctx);
    lsw_set_new_thread_core(-1);
    printf("[GL] glthread: ctx %p, context dispatch %p -> %p, glapi dispatch %p\n", ctx, ctx_disp0, _mesa_get_dispatch(ctx), before);
    // _mesa_glthread_init points the context's CurrentClientDispatch at the marshal table; glapi only picks that up
    // in _mesa_make_current, which re-binding through this EGL platform does not reach. Apply it directly
    // (Mesa 20.1 context.c: _mesa_get_dispatch returns CurrentClientDispatch).
    _glapi_set_dispatch(_mesa_get_dispatch(ctx));
    void* after = _glapi_get_dispatch();
    printf("[GL] glthread %s (dispatch %p -> %p)\n", after != before ? "ON" : "did not engage", before, after);
    if (after != before && env_on("LSWTCS_GLSYNCLOG", false)) lsw_glthread_sync_hook = glsync_hook;
    g_glpipe = after != before && env_on("LSWTCS_GLPIPE", false);
    if (g_glpipe) printf("[GL] glthread pipeline ON: fences, arena upload and swap run on the worker (LSWTCS_GLPIPE=1)\n");
}

// Frames queued on the GPU (LSWTCS_GLMAXQUEUE, default 1; 0 = no limit): wait until the GPU has finished the
// previous frame before presenting this one. On the Switch the GPU took 60-80 ms per hub frame and up to three window
// buffers queued behind it: 250-400 ms from a button press to the screen (2026-10-08). [GLPROF] gpuwait = time the
// GPU is behind (with LSWTCS_GLPIPE: measured on the glthread worker, as is swap).
bool cap_queue() {
    static const bool on = [] { const char* e = getenv("LSWTCS_GLMAXQUEUE"); return !(e && e[0] == '0'); }();
    return on;
}
std::atomic<uint64_t> g_swap_fail{0};

// glthread worker (LSWTCS_GLPIPE): GPU-queue cap, swap, fence for the next cap, then report the swap done.
void swap_job(void*) {
    static GLsync prev_frame = nullptr;
    if (cap_queue() && prev_frame) {
        RpScope rp(RP_GPUWAIT);
        glClientWaitSync(prev_frame, GL_SYNC_FLUSH_COMMANDS_BIT, GLuint64(2000000000));
        glDeleteSync(prev_frame);
        prev_frame = nullptr;
    }
    bool swapped;
    { RpScope rp(RP_SWAP); swapped = lsw_egl_swap_buffers_worker(g_display, g_surface); }   // blocks for vsync
    if (cap_queue()) prev_frame = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!swapped) ++g_swap_fail;
    { std::lock_guard<std::mutex> lk(g_pipe_mtx); ++g_pipe_swap_done; }
    g_pipe_cv.notify_all();
}

void render_thread_main() {
    pin_to_core(1, "render thread");
    g_gl_ok = egl_init();
    if (g_gl_ok) resources_init();
    if (g_gl_ok) program_cache_init();
    if (g_gl_ok && env_on("LSWTCS_GLTEST", false)) gl_self_test();
    if (g_gl_ok && env_on("LSWTCS_GLBENCH", false)) gl_bench();
    if (g_gl_ok && env_on("LSWTCS_GLDEPTHPROBE", false)) gl_depth_probe();
    if (g_gl_ok) enable_glthread();
    fflush(stdout);
    uint64_t presented = 0, swaps = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [&] { return g_frames_posted != presented; });
            presented = g_frames_posted;
        }
        if (!g_gl_ok) { frame_done(presented); continue; }
        if (g_glpipe) {   // the worker must be done with the previous upload (f.arena goes back to the guest in
                          // replay's gpu_frame_take) and may have at most one swap outstanding
            RpScope rp(RP_PIPE);
            std::unique_lock<std::mutex> lk(g_pipe_mtx);
            g_pipe_cv.wait(lk, [] { return g_pipe_up_done == g_pipe_up_queued && g_pipe_swap_done + 1 >= g_pipe_swap_queued; });
        }
        const uint64_t t0 = gp_now();
        const PresentSrc shown = replay();
        arena_fence();
        const uint64_t t_replay = gp_now() - t0;
        g_gpuprof[GP_REPLAY] += t_replay;
        maybe_capture(shown);
        maybe_resdump();
        { RpScope rp(RP_EVICT); evict_textures(); }
        if (g_glpipe) {   // present blit recorded here; GPU-queue cap, swap and fence run on the worker (swap_job)
            { RpScope rp(RP_PRESENT); present_frame(shown); }
            gpu_prof_finish();
            ++g_pipe_swap_queued;
            lsw_glthread_call(swap_job, nullptr);
        } else {
            static GLsync prev_frame = nullptr;
            if (cap_queue() && prev_frame) {
                RpScope rp(RP_GPUWAIT);
                glClientWaitSync(prev_frame, GL_SYNC_FLUSH_COMMANDS_BIT, GLuint64(2000000000));
                glDeleteSync(prev_frame);
                prev_frame = nullptr;
            }
            { RpScope rp(RP_PRESENT); present_frame(shown); }
            gpu_prof_finish();
            bool swapped;
            { RpScope rp(RP_SWAP); swapped = eglSwapBuffers(g_display, g_surface); }   // blocks for vsync
            if (cap_queue()) prev_frame = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (!swapped) ++g_swap_fail;
        }
        frame_done(presented);
        {   // DIAG: [GLPROF] render-thread stages, average per replay over the last 60
            static uint64_t last[RP_COUNT], replay_ns = 0, max_ns = 0, t_last = 0;
            static uint32_t n = 0;
            replay_ns += t_replay;
            max_ns = std::max(max_ns, t_replay);
            if (++n == 60) {
                const uint64_t now = gp_now();
                char line[512];
                int len = 0;
                uint64_t staged = 0;
                for (int k = 0; k < RP_COUNT; ++k) {
                    const uint64_t d = g_rp[k] - last[k];
                    last[k] = g_rp[k];
                    if (k < RP_PRESENT) staged += d;
                    len += snprintf(line + len, sizeof line - len, " %s=%.2f", kRpName[k], d / 1e6 / n);
                }
                printf("[GLPROF] per replay (%u): wall=%.2fms replay=%.2f (max %.1f, other=%.2f) |%s\n", n,
                       t_last ? (now - t_last) / 1e6 / n : 0.0, replay_ns / 1e6 / n, max_ns / 1e6,
                       replay_ns > staged ? (replay_ns - staged) / 1e6 / n : 0.0, line);
                static uint64_t sc_last[SC_N];
                double ch[SC_N];
                for (int k = 0; k < SC_N; ++k) { ch[k] = double(g_sc_changes[k] - sc_last[k]) / n; sc_last[k] = g_sc_changes[k]; }
                printf("[GLPROF]   changes per replay: program %.0f, fbo %.0f, ubo %.0f, texture %.0f, swizzle %.0f, sampler %.0f, "
                       "state %.0f, cache resets %.0f\n", ch[SC_PROG], ch[SC_FBO], ch[SC_UBO], ch[SC_TEX], ch[SC_SWZ], ch[SC_SMP],
                       ch[SC_STATE], ch[SC_RESET]);
                glsync_report(n);
                fflush(stdout);
                t_last = now; replay_ns = 0; max_ns = 0; n = 0;
            }
        }
        if ((++swaps % 600) == 0) {
            printf("[GL] render thread: %llu swaps (%llu failed), guest swaps %llu\n", (unsigned long long)swaps,
                   (unsigned long long)g_swap_fail.load(), (unsigned long long)presented);
            fflush(stdout);
        }
    }
}
}  // namespace

// ---- presentation (gpu_d3d12.h) ---------------------------------------------------------------
void gpu_d3d12_init() {
    printf("[NX] GPU: OpenGL backend stage 2 (draw replay to one 1280x720 target, no EDRAM RTs)\n");
    std::thread(render_thread_main).detach();
}
void gpu_d3d12_present(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t) {
    static uint64_t frames = 0;
    if ((++frames % 300) == 0) printf("[NX] present #%llu\n", (unsigned long long)frames);
    // One frame in flight, as on the PC (gpu_d3d12.cpp): the guest waits, off the GIL scheduler, until the
    // previous swap is replayed and presented. Without it the game ran on through a multi-second first
    // replay of a new scene (shader builds) and its transitions (the wipe into the Cantina) played out
    // unseen; with it the game is held while the renderer catches up, like a console waiting on its GPU.
    // LSWTCS_RTSYNC=0: no wait (the renderer drops frames instead).
    static const bool sync = env_on("LSWTCS_RTSYNC", true);
    const uint64_t t0 = gp_now();
    if (sync) {
        bool behind;
        { std::lock_guard<std::mutex> lk(g_mtx); behind = g_frames_done != g_frames_posted; }
        if (behind)
            lswtcs_gil_blocking([](void*) {
                std::unique_lock<std::mutex> lk(g_mtx);
                g_cv.wait(lk, [] { return g_frames_done == g_frames_posted; });
            }, nullptr);
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ++g_frames_posted;
    }
    g_cv.notify_all();
    g_gpuprof[GP_RTWAIT] += gp_now() - t0;
    appletMainLoop();   // HOME / exit requests
}
bool gpu_d3d12_poll() { return appletMainLoop(); }
extern "C" void lswtcs_set_pending_shad(const char*) {}
extern "C" int lswtcs_pathb_compile(const char*, uint32_t) { return 0; }   // Path-B HLSL compile (D3D12 only)
