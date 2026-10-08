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
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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

bool env_on(const char* name, bool def) {
    const char* e = getenv(name);
    return e ? e[0] == '1' : def;
}

// ---- EGL --------------------------------------------------------------------------------------
typedef void (*PFN_ClipControl)(GLenum, GLenum);
PFN_ClipControl p_glClipControl = nullptr;

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
    const int want = surfaceless ? std::max(0, std::min(nw ? atoi(nw) : 0, int(kMaxCompileWorkers))) : 0;
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
    snprintf(path, sizeof path, "glprogs/%016llX.glsl3", (unsigned long long)ckey);
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
GLuint program_build(uint64_t ckey, const ProgSources& s) {
    auto compile = [&](GLenum type, const std::string& src) -> GLuint {
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
    pin_to_core(2, "shader-compile worker");
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
    }
}

// Boot: binary-cache detection, then queue every program saved by earlier sessions and start workers.
void program_cache_init() {
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    const char* e = getenv("LSWTCS_GLCACHE");
    g_binary_cache_on = formats > 0 && !(e && e[0] == '0');
    const std::string drv = std::string((const char*)glGetString(GL_RENDERER)) + "|" + (const char*)glGetString(GL_VERSION);
    g_driver_hash = XXH3_64bits(drv.data(), drv.size());
    if (g_binary_cache_on) mkdir("glcache", 0777);
    mkdir("glprogs", 0777);
    uint32_t queued = 0;
    if (g_num_workers > 0)
        if (DIR* d = opendir("glprogs")) {
            while (dirent* de = readdir(d)) {
                const char* n = de->d_name;
                if (strlen(n) != 22 || strcmp(n + 16, ".glsl3") != 0) continue;
                char path[64];
                snprintf(path, sizeof path, "glprogs/%s", n);
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
        program_list_save(ckey, s);
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
        return 0;
    }
    g_programs.emplace(key, e.prog);   // ready or failed: settled
    return e.prog;
}

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
    const uint64_t key = lsw_gpu_texture_key(fetch6, phys);
    GlTex t;
    auto it = g_textures.find(key);
    if (it != g_textures.end()) {
        it->second.last_used = g_replays;
        t = it->second;
    } else {
        LswGpuTexDesc d;
        if (lsw_gpu_decode_texture(fetch6, phys, &d) && d.ok) t = create_texture(d);
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
        if (it->second.tex) glDeleteTextures(1, &it->second.tex);
        g_tex_bytes -= it->second.bytes;
        g_textures.erase(it);
        ++g_tex_evicted;
    }
}

std::unordered_map<std::string, GLuint> g_samplers;

GLuint get_sampler(const uint32_t* fetch6, const LswGpuSamplerBinding& b) {
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
            if (texture_source(base & 0x1FFFFFFFu, fmt, w, h, src) && src && src->tex) {
                tex = src->tex;
                map = resolved_swizzle(gswz, fmt, src->fmt);
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

GLenum topology(uint32_t d3d) {
    switch (d3d) {
        case 1: return GL_POINTS; case 2: return GL_LINES; case 3: return GL_LINE_STRIP;
        case 5: return GL_TRIANGLE_STRIP; case 10: return GL_LINES_ADJACENCY; default: return GL_TRIANGLES;
    }
}

// ---- replay -----------------------------------------------------------------------------------
void dump_first_program(const LswGlShader* vs, const LswGlShader* ps, const LswGlShader* gs);

// What present_frame shows: a resolve target / render target (top-down rows) or the scene FBO.
struct PresentSrc { GLuint fbo = 0; uint32_t w = kSceneW, h = kSceneH; };
HostTex g_last_good;          // last complete frame (shown while a frame is held for compiling programs)
uint64_t g_frames_held = 0;

PresentSrc replay() {
    gpu_frame_take();
    LswGpuFrame& f = g_gpu_frame_render;   // replayed again if no newer frame was published
    ++g_replays;
    g_tex_memo.clear();
    const bool gpurt = gpurt_on();
    const uint64_t replay_t0 = gp_now();
    g_prog_pending_draws = 0;
    {   // capture this replay? (trigger file checked twice a second, or the GLTEST frames)
        static const bool gltest = env_on("LSWTCS_GLTEST", false);
        static uint64_t next_check = 0;
        g_capture_frame = gltest && (g_replays == 300 || g_replays == 1500);
        if (g_replays >= next_check) {
            next_check = g_replays + 30;
            if (FILE* t = fopen("shot_now", "rb")) { fclose(t); remove("shot_now"); g_capture_frame = true; }
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
    const GLsizeiptr need = GLsizeiptr(f.arena.size()) + 256;
    static GLuint ring[3] = {}, patch_ring[3] = {};
    static GLsizeiptr ring_cap[3] = {};
    static uint32_t ring_i = 0;
    ring_i = (ring_i + 1) % 3;
    if (!ring[ring_i]) glGenBuffers(1, &ring[ring_i]);
    g_arena = ring[ring_i];
    glBindBuffer(GL_UNIFORM_BUFFER, g_arena);
    if (need > ring_cap[ring_i]) {
        ring_cap[ring_i] = std::max<GLsizeiptr>(need + need / 2, GLsizeiptr(4) << 20);
        glBufferData(GL_UNIFORM_BUFFER, ring_cap[ring_i], nullptr, GL_DYNAMIC_DRAW);
    }
    g_arena_cap = ring_cap[0] + ring_cap[1] + ring_cap[2];
    glBufferSubData(GL_UNIFORM_BUFFER, 0, GLsizeiptr(f.arena.size()), f.arena.data());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_arena);   // indices live in the arena too (VAO state)
    // Per-draw system-constant copies with swizzle overrides for resolved textures (2 MB per frame).
    constexpr GLsizeiptr kPatchCap = GLsizeiptr(2) << 20;
    if (!patch_ring[ring_i]) {
        glGenBuffers(1, &patch_ring[ring_i]);
        glBindBuffer(GL_UNIFORM_BUFFER, patch_ring[ring_i]);
        glBufferData(GL_UNIFORM_BUFFER, kPatchCap, nullptr, GL_DYNAMIC_DRAW);
    }
    const GLuint patch_buf = patch_ring[ring_i];
    GLintptr patch_off = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_shm);
    uint32_t copies = 0;
    auto upload = [&](const LswGpuRange& rg) {
        if (rg.arena_off == kLswRangeResident || GLsizeiptr(rg.phys) + rg.size > kShmSize) return;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_shm);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, GLintptr(rg.phys), GLsizeiptr(rg.size), f.arena.data() + rg.arena_off);
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
            program(vs, static_cast<const LswGlShader*>(r.ps_dxbc), gs);   // requests the compile if needed
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
    for (const LswGpuDrawRec& r : f.draws) {
        if (r.kind == 1) {   // EDRAM resolve
            if (g_capture_frame)
                printf("[GLDRAWCAP] #%u RESOLVE src=%u rect=%d,%d..%d,%d pitch=%u -> 0x%08X %ux%u tfmt=%u clr c%u d%u\n", draw_i, r.rs_src,
                       r.rs_rect[0], r.rs_rect[1], r.rs_rect[2], r.rs_rect[3], r.surf_info & 0x3FFF, r.rs_dest, r.rs_dest_pitch,
                       r.rs_dest_height, r.rs_dest_fmt, r.rs_clear_color, r.rs_clear_depth);
            ++draw_i;
            if (gpurt) { do_resolve(r); ++resolves; }
            continue;
        }
        ++draw_i;
        // 1. Vertex data into shared memory (every recorded draw: residency tracking assumed it).
        for (uint32_t k = 0; k < r.range_count; ++k) upload(f.ranges[r.first_range + k]);
        const auto* vs = static_cast<const LswGlShader*>(r.vs_dxbc);
        const auto* ps = static_cast<const LswGlShader*>(r.ps_dxbc);
        const auto* gs = static_cast<const LswGlShader*>(r.gs_dxbc);
        if (r.gs_type && !gs) { ++no_gs; continue; }
        const GLuint prog = vs ? program(vs, ps, gs) : 0;
        if (!prog) { ++no_prog; continue; }
        if (env_on("LSWTCS_GLTEST", false)) dump_first_program(vs, ps, gs);
        // 2. Texture sources (may blit composites / views: before any binding).
        const uint32_t* fetch_all = reinterpret_cast<const uint32_t*>(f.arena.data() + r.cb_fetch_off);
        units.clear(); swz.clear(); g_unit_log.clear();
        collect_units(ps, fetch_all, gpurt, units, swz);
        collect_units(vs, fetch_all, gpurt, units, swz);
        // 3. Render targets (sample space for MSAA surfaces).
        DrawTarget dt;
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
            if (!drt || fail) { ++no_rt; continue; }
            dt.nrt = nrt; dt.rt_w = drt->t.w; dt.rt_h = kRtH;
            glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo(crts, nrt, drt));
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
        }
        // 4. Program, constants, textures, state, draw.
        glUseProgram(prog);
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
            glBindBufferRange(GL_UNIFORM_BUFFER, 0, patch_buf, patch_off, sizeof sys);
            patch_off += sizeof sys;
            ++patched;
        } else {
            glBindBufferRange(GL_UNIFORM_BUFFER, 0, g_arena, r.cb_sys_off, kLswGlClipPlaneOffset);
        }
        glBindBufferRange(GL_UNIFORM_BUFFER, 5, g_arena, r.cb_sys_off + kLswGlClipPlaneOffset, kLswGlClipPlaneSize);
        glBindBufferRange(GL_UNIFORM_BUFFER, 1, g_arena, r.cb_vsf_off, std::max<uint32_t>(r.cb_vsf_size, 16));
        glBindBufferRange(GL_UNIFORM_BUFFER, 2, g_arena, r.cb_psf_off, std::max<uint32_t>(r.cb_psf_size, 16));
        glBindBufferRange(GL_UNIFORM_BUFFER, 3, g_arena, r.cb_bool_off, 40 * 4);
        glBindBufferRange(GL_UNIFORM_BUFFER, 4, g_arena, r.cb_fetch_off, 192 * 4);
        for (const UnitBind& u : units) {
            glActiveTexture(GL_TEXTURE0 + u.unit);
            glBindTexture(u.target, u.tex);
            if (view_swizzle_on()) apply_swizzle(u.target, u.swz);
            glBindSampler(u.unit, u.sampler);
        }
        apply_state(r, dt);
        if (g_capture_frame)
            printf("[GLDRAWCAP] #%u prim %u gs %u n=%u ib=%u mode=%u pitch=%u msaa=%u col=%u/%u dep=%u nrt=%u blend=%08X cmask=%X "
                   "zctl=%08X cull=%u vp=%.0f,%.0f,%.0fx%.0f ps=%s |%s\n", draw_i - 1, r.topology, r.gs_type, r.vertex_count,
                   r.ib_count, r.edram_mode, r.surf_info & 0x3FFF, (r.surf_info >> 16) & 3, r.color_info & 0xFFF,
                   (r.color_info >> 16) & 0xF, r.depth_info & 0xFFF, dt.nrt, r.blendcontrol, r.color_mask, r.depthcontrol,
                   r.sc_mode_cntl & 7, r.viewport[0], r.viewport[1], r.viewport[2], r.viewport[3], ps ? "y" : "n",
                   g_unit_log.c_str());
        const GLenum mode = topology(r.topology);
        // DIAG (LSWTCS_GLTIMING=1): the first draw of each program, finished, includes mesa's lazy backend
        // compile (TGSI -> GPU code happens at validation, not at link).
        static const bool timing = env_on("LSWTCS_GLTIMING", false);
        static std::unordered_map<GLuint, bool> drawn_once;
        const bool first_use = timing && !drawn_once[prog];
        uint64_t t_first = 0;
        if (first_use) { glFinish(); t_first = gp_now(); drawn_once[prog] = true; }
        if (r.ib_count)
            glDrawElements(mode, GLsizei(r.ib_count), r.ib_32bit ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                           reinterpret_cast<const void*>(uintptr_t(r.ib_off)));
        else
            glDrawArrays(mode, 0, GLsizei(r.vertex_count));
        if (first_use) {
            glFinish();
            printf("[GLPROGT] program %u first draw: %llu ms (VS %zu KB, PS %zu KB)\n", prog,
                   (unsigned long long)((gp_now() - t_first) / 1000000), vs->glsl.size() / 1024, ps ? ps->glsl.size() / 1024 : 0);
            fflush(stdout);
        }
        ++by_gs[r.gs_type & 3];
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
        auto it = g_resolved.find(f.swap_fb & 0x1FFFFFFFu);
        if (f.swap_fb && it != g_resolved.end() && it->second.t.tex) {
            present = {single_fbo(it->second.t), std::min<uint32_t>(it->second.t.w, kSceneW), std::min<uint32_t>(it->second.t.h, kSceneH)};
            shown = "resolve";
        } else {
            auto ct = g_rts.find((uint64_t(0) << 40) | (uint64_t(1280) << 16) | uint64_t(kDxgiRGBA8));
            if (ct != g_rts.end() && ct->second.t.tex) { present = {single_fbo(ct->second.t), kSceneW, kSceneH}; shown = "RT0"; }
        }
        if (g_num_workers > 0) {   // keep this complete frame for frames held while programs compile
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
        if (ms >= 250 || (g_prog_pending_draws && (g_replays % 60) == 0)) {
            size_t queued;
            { std::lock_guard<std::mutex> lk(g_prog_mtx); queued = g_prog_queue.size(); }
            printf("[GLSTALL] replay %llu took %llu ms, %u draws, %u skipped (program compiling), %zu programs queued\n",
                   (unsigned long long)g_replays, (unsigned long long)ms, drawn, g_prog_pending_draws, queued);
            fflush(stdout);
        }
    }
    if (g_replays <= 5 || (g_replays % 120) == 0) {
        {
            std::lock_guard<std::mutex> lk(g_prog_mtx);
            printf("[GLPROG] %u compiled (%llu ms of worker time), %u from binary cache, %u binaries written, %u boot-queued, "
                   "%zu still queued\n", g_prog_compiled, (unsigned long long)(g_prog_compile_ns / 1000000), g_prog_from_binary,
                   g_prog_cache_written, g_prog_boot_queued, g_prog_queue.size());
        }
        printf("[GLDRAW] replay %llu: %u drawn (plain %u / points %u / rects %u / quads %u), %u no program, %u no GS, %u no RT, "
               "%u resolves, %u resolved-texture binds, %u swizzle patches, %u shm copies, arena %zu KB, shown %s (fb 0x%08X)\n",
               (unsigned long long)g_replays, drawn, by_gs[0], by_gs[1], by_gs[2], by_gs[3], no_prog, no_gs, no_rt, resolves,
               g_res_tex_binds, patched, copies, f.arena.size() / 1024, shown, f.swap_fb);
        printf("[GLDRAW]   %zu programs, %u shaders compiled (%u failed, %u link failed), %zu textures (%llu MB, %u evicted), "
               "%zu RTs + %zu resolve targets + %zu views + %zu composites = %llu MB, arena buffers %lld MB, err 0x%X\n",
               g_programs.size(), g_compiled, g_compile_failed, g_link_failed, g_textures.size(),
               (unsigned long long)(g_tex_bytes >> 20), g_tex_evicted, g_rts.size(), g_resolved.size(), g_views.size(),
               g_composed.size(), (unsigned long long)(g_rt_bytes >> 20), (long long)(g_arena_cap >> 20), glGetError());
        fflush(stdout);
    }
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
    const size_t raw_len = size_t(h) * (1 + size_t(w) * 4);
    std::vector<uint8_t> raw(raw_len);
    for (int y = 0; y < h; y++) {
        raw[size_t(y) * (1 + size_t(w) * 4)] = 0;
        memcpy(&raw[size_t(y) * (1 + size_t(w) * 4) + 1], rgba + size_t(y) * w * 4, size_t(w) * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t pos = 0; pos < raw_len;) {
        const size_t blk = std::min<size_t>(raw_len - pos, 65535u);
        z.push_back(pos + blk >= raw_len ? 1 : 0);
        z.push_back(uint8_t(blk)); z.push_back(uint8_t(blk >> 8));
        z.push_back(uint8_t(~blk)); z.push_back(uint8_t(~blk >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + blk);
        pos += blk;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    const uint32_t adler = (b << 16) | a;
    z.push_back(uint8_t(adler >> 24)); z.push_back(uint8_t(adler >> 16)); z.push_back(uint8_t(adler >> 8)); z.push_back(uint8_t(adler));
    png_chunk(f, "IDAT", z.data(), uint32_t(z.size()));
    png_chunk(f, "IEND", nullptr, 0);
    fclose(f);
}
void maybe_capture(const PresentSrc& src) {
    if (!g_capture_frame) return;
    const uint32_t w = src.w, h = src.h;
    std::vector<uint8_t> px(size_t(w) * h * 4);
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
    if (g_replays % 30) return;
    FILE* t = fopen("resdump_now", "rb");
    if (!t) return;
    fclose(t);
    remove("resdump_now");
    auto dump = [](const HostTex& tex, uint32_t name, uint32_t rows) {
        const uint32_t w = tex.w, h = std::min(tex.h, rows);
        std::vector<float> f(size_t(w) * h * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, single_fbo(tex));
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_FLOAT, f.data());
        float zmin = 1e30f, zmax = -1e30f;
        const bool depth = tex.fmt == kDxgiR32F;
        if (depth) for (size_t i = 0; i < f.size(); i += 4) { zmin = std::min(zmin, f[i]); zmax = std::max(zmax, f[i]); }
        std::vector<uint8_t> px(size_t(w) * h * 4);
        for (size_t i = 0; i < f.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                float v = depth ? (zmax > zmin ? (f[i] - zmin) / (zmax - zmin) : 0.0f) : f[i + c];
                if ((tex.fmt == kDxgiR8) && c) v = f[i];
                if (tex.fmt == kDxgiRG8 && c == 2) v = 0.0f;
                px[i + c] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            px[i + 3] = 255;
        }
        char path[64];
        snprintf(path, sizeof path, "res_%08X_%ux%u.png", name, w, h);
        write_png_rgba(path, px.data(), int(w), int(h));
        printf("[RESDUMP] %s fmt=%u%s", path, tex.fmt, depth ? " (depth, normalized)" : "");
        if (depth) printf(" range %.6f .. %.6f", zmin, zmax);
        printf("\n");
    };
    for (const auto& kv : g_resolved)
        if (kv.second.t.tex) dump(kv.second.t, kv.first, kv.second.t.h);
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

void render_thread_main() {
    pin_to_core(1, "render thread");
    g_gl_ok = egl_init();
    if (g_gl_ok) resources_init();
    if (g_gl_ok) program_cache_init();
    if (g_gl_ok && env_on("LSWTCS_GLTEST", false)) gl_self_test();
    if (g_gl_ok && env_on("LSWTCS_GLBENCH", false)) gl_bench();
    if (g_gl_ok && env_on("LSWTCS_GLDEPTHPROBE", false)) gl_depth_probe();
    fflush(stdout);
    uint64_t presented = 0, swaps = 0, swap_fail = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [&] { return g_frames_posted != presented; });
            presented = g_frames_posted;
        }
        if (!g_gl_ok) continue;
        const uint64_t t0 = gp_now();
        const PresentSrc shown = replay();
        g_gpuprof[GP_REPLAY] += gp_now() - t0;
        maybe_capture(shown);
        maybe_resdump();
        evict_textures();
        present_frame(shown);
        if (!eglSwapBuffers(g_display, g_surface)) ++swap_fail;
        if ((++swaps % 600) == 0) {
            printf("[GL] render thread: %llu swaps (%llu failed), guest swaps %llu\n", (unsigned long long)swaps,
                   (unsigned long long)swap_fail, (unsigned long long)presented);
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
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ++g_frames_posted;
    }
    g_cv.notify_one();
    appletMainLoop();   // HOME / exit requests
}
bool gpu_d3d12_poll() { return appletMainLoop(); }
extern "C" void lswtcs_set_pending_shad(const char*) {}
extern "C" int lswtcs_pathb_compile(const char*, uint32_t) { return 0; }   // Path-B HLSL compile (D3D12 only)
