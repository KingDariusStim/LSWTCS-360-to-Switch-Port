// EDRAM render targets, resolves and resolved-texture sources for the OpenGL replay (included by
// gl_backend.cpp). A port of the D3D12 machinery in LSWTCSRuntime/source/gpu_xedraw.inc (same keys,
// sizes and rules; see the comments there for the why of each piece), with GL objects:
//  - every host texture here is a 1-layer GL_TEXTURE_2D_ARRAY, because the translated shaders sample
//    2D textures as sampler2DArray, so render targets and resolve targets bind to them directly;
//  - rows are top-down (upper-left clip control), so EDRAM/guest rows map to texture rows 1:1.

// ---- formats -----------------------------------------------------------------------------------
// Host formats are kept as DXGI numbers (shared with the PC logic and lsw_gpu_texture.cc).
enum : uint32_t {
    kDxgiRGBA16F = 10, kDxgiRGBA16Snorm = 13, kDxgiRG32F = 16, kDxgiRGB10A2 = 24, kDxgiRGBA8 = 28,
    kDxgiRG16F = 34, kDxgiRG16Snorm = 37, kDxgiR32F = 41, kDxgiRG8 = 49, kDxgiR8 = 61, kDxgiD32S8 = 20,
};
// Depth render targets (tagged kDxgiD32S8 as on the PC) are GL_DEPTH24_STENCIL8: sampling a
// GL_DEPTH32F_STENCIL8 texture returns 0 on switch-mesa (LSWTCS_GLDEPTHPROBE, 2026-10-07), which zeroed
// every depth resolve -> no shadows (shadow-mask pass), wrong composite lighting, faded-out soft
// particles. D24S8 is also the guest's own format. LSWTCS_GLD32F=1 restores D32F_S8.
bool depth_d32f() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("LSWTCS_GLD32F"); v = (e && e[0] == '1') ? 1 : 0; }
    return v == 1;
}
GLenum rt_internal(uint32_t dxgi) {
    switch (dxgi) {
        case kDxgiRGBA16F: return GL_RGBA16F;
        case kDxgiRGBA16Snorm: return GL_RGBA16_SNORM;
        case kDxgiRG32F: return GL_RG32F;
        case kDxgiRGB10A2: return GL_RGB10_A2;
        case kDxgiRG16F: return GL_RG16F;
        case kDxgiRG16Snorm: return GL_RG16_SNORM;
        case kDxgiR32F: return GL_R32F;
        case kDxgiRG8: return GL_RG8;
        case kDxgiR8: return GL_R8;
        case kDxgiD32S8: return depth_d32f() ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
        default: return GL_RGBA8;
    }
}
// RB_COLOR_INFO color_format (xenos::ColorRenderTargetFormat) -> host format.
uint32_t rt_color_format(uint32_t f) {
    switch (f) {
        case 2: case 10: return kDxgiRGB10A2;
        case 3: case 12: case 7: return kDxgiRGBA16F;
        case 4: return kDxgiRG16Snorm;
        case 5: return kDxgiRGBA16Snorm;
        case 6: return kDxgiRG16F;
        case 14: return kDxgiR32F;
        case 15: return kDxgiRG32F;
        default: return kDxgiRGBA8;   // k_8_8_8_8, k_8_8_8_8_GAMMA
    }
}
uint32_t rt_bytes_per_pixel(uint32_t dxgi) {
    switch (dxgi) {
        case kDxgiRGBA16F: case kDxgiRGBA16Snorm: case kDxgiRG32F: return 8;
        case kDxgiD32S8: return depth_d32f() ? 8 : 4;
        case kDxgiRG8: return 2;
        case kDxgiR8: return 1;
        default: return 4;
    }
}
bool gpurt_on() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("LSWTCS_GPURT"); v = (e && e[0] == '0') ? 0 : 1; }
    return v == 1;
}

// ---- host 2D-array textures ---------------------------------------------------------------------
uint64_t g_rt_bytes = 0;   // GPU memory held by render targets, resolve targets, views and composites
struct HostTex { GLuint tex = 0; uint32_t w = 0, h = 0, fmt = 0; };
HostTex host_tex_create(uint32_t w, uint32_t h, uint32_t fmt) {
    ++g_gl_epoch;
    HostTex t{0, w, h, fmt};
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, t.tex);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, rt_internal(fmt), GLsizei(w), GLsizei(h), 1);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (fmt == kDxgiD32S8) glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
    g_rt_bytes += uint64_t(w) * h * rt_bytes_per_pixel(fmt);
    return t;
}
void host_tex_free(HostTex& t) {
    if (t.tex) { ++g_gl_epoch; glDeleteTextures(1, &t.tex); g_rt_bytes -= uint64_t(t.w) * t.h * rt_bytes_per_pixel(t.fmt); }
    t = HostTex{};
}
// Framebuffer with `tex` (colour or depth-stencil) alone, for clears and as a blit destination.
std::unordered_map<GLuint, GLuint> g_single_fbos;
GLuint single_fbo(const HostTex& t) {
    GLuint& fbo = g_single_fbos[t.tex];
    if (!fbo) {
        ++g_gl_epoch;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (t.fmt == kDxgiD32S8) {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, t.tex, 0, 0);
            glDrawBuffer(GL_NONE);
        } else {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t.tex, 0, 0);
        }
    }
    return fbo;
}
void forget_fbos_of(GLuint tex);   // drops cached draw framebuffers using tex (below)

// ---- EDRAM render targets ---------------------------------------------------------------------
// Host RT per (EDRAM base, effective pitch in sample space, format), kRtH rows tall so any colour /
// depth pair of the same pitch can be bound together (gpu_xedraw.inc "EDRAM render targets").
constexpr uint32_t kRtH = 2048;
struct Rt { HostTex t; bool depth = false; };
std::unordered_map<uint64_t, Rt> g_rts;
// LSWTCS_MSAA1X (default 1): 2x MSAA surfaces render at one sample per pixel (msaa_scale). 0 = 2x in
// sample space as before (kept for a later optimised 2x path).
bool msaa_one_x() {
    static const bool on = [] { const char* e = getenv("LSWTCS_MSAA1X"); return !(e && e[0] == '0'); }();
    return on;
}
void msaa_scale(uint32_t surf_info, uint32_t& sx, uint32_t& sy) {
    // LSWTCS_MSAA1X (default on): 2x MSAA surfaces render at one sample per pixel (no anti-aliasing). Sample-space
    // rendering shades every sample: the hub's world draws are 2x MSAA, i.e. 1280x1440 shaded, on a GPU
    // that needed ~48 ms per hub frame on the Switch (2026-10-08). Every sample-space size, viewport,
    // scissor, clear and resolve goes through here, and the SPIR-V shaders do not read the sample count
    // outside the interlock path the port does not use.
    // Only 2x surfaces (the world) drop to one sample per pixel; 4x surfaces stay in full sample space.
    // The game clears through 4x 640-pitch surfaces: the 2x world (1280 samples wide; making the 4x
    // surface 640 wide made it a separate target, the world depth was never cleared, black world) and
    // 1x screens such as the title (a 4x row covers two 1x rows; squeezing 4x vertically left the bottom
    // half uncleared: streaks). With 4x untouched both still alias; the 4x clear of the squeezed 2x
    // world simply covers extra rows. (Eden, 2026-10-08)
    const uint32_t msaa = (surf_info >> 16) & 3;   // 0 1x, 1 2x, 2 4x
    sx = msaa == 2 ? 2 : 1;
    sy = msaa == 2 ? 2 : (msaa == 1 && !msaa_one_x()) ? 2 : 1;
}
void clear_color_tex(const HostTex& t, const float* c, const int* rect);
void clear_depth_tex(const HostTex& t, float z, int stencil, const int* rect);
Rt* rt_get(bool depth, uint32_t base, uint32_t w, uint32_t fmt) {
    if (w == 0 || w > 8192) return nullptr;
    const uint64_t key = (uint64_t(depth) << 63) | (uint64_t(base & 0xFFF) << 40) | (uint64_t(w) << 16) | uint64_t(fmt);
    auto it = g_rts.find(key);
    if (it != g_rts.end()) return it->second.t.tex ? &it->second : nullptr;
    Rt rt;
    rt.depth = depth;
    rt.t = host_tex_create(w, kRtH, depth ? uint32_t(kDxgiD32S8) : fmt);
    if (depth) clear_depth_tex(rt.t, 1.0f, 0, nullptr);
    else { const float z[4] = {}; clear_color_tex(rt.t, z, nullptr); }
    printf("[GLRT] new %s RT base=%u %ux%u fmt=%u (total %zu, %llu MB)\n", depth ? "depth" : "color", base, w, kRtH, fmt,
           g_rts.size() + 1, (unsigned long long)(g_rt_bytes >> 20));
    fflush(stdout);
    return &(g_rts[key] = rt);
}
Rt* rt_color(uint32_t surf_info, uint32_t color_info) {
    uint32_t sx, sy; msaa_scale(surf_info, sx, sy);
    return rt_get(false, color_info & 0xFFF, (surf_info & 0x3FFF) * sx, rt_color_format((color_info >> 16) & 0xF));
}
Rt* rt_depth(uint32_t surf_info, uint32_t depth_info) {
    uint32_t sx, sy; msaa_scale(surf_info, sx, sy);
    return rt_get(true, depth_info & 0xFFF, (surf_info & 0x3FFF) * sx, kDxgiD32S8);
}

// Draw framebuffers: one per (colour RTs, depth RT) combination.
struct FboKey {
    GLuint c[4]; GLuint d;
    bool operator==(const FboKey& o) const { return !memcmp(this, &o, sizeof *this); }
};
struct FboKeyHash {
    size_t operator()(const FboKey& k) const {
        uint64_t h = 1469598103934665603ull;
        for (GLuint v : k.c) h = (h ^ v) * 1099511628211ull;
        return size_t((h ^ k.d) * 1099511628211ull);
    }
};
std::unordered_map<FboKey, GLuint, FboKeyHash> g_draw_fbos;
GLuint draw_fbo(Rt* const* colors, uint32_t ncolor, Rt* depth) {
    FboKey k{};
    for (uint32_t i = 0; i < ncolor; ++i) k.c[i] = colors[i] ? colors[i]->t.tex : 0;
    k.d = depth ? depth->t.tex : 0;
    GLuint& fbo = g_draw_fbos[k];
    if (!fbo) {
        ++g_gl_epoch;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        GLenum bufs[4] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE};
        for (uint32_t i = 0; i < ncolor; ++i)
            if (k.c[i]) {
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, k.c[i], 0, 0);
                bufs[i] = GL_COLOR_ATTACHMENT0 + i;
            }
        if (k.d) glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, k.d, 0, 0);
        glDrawBuffers(4, bufs);
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) { printf("[GLRT] draw framebuffer incomplete 0x%X\n", st); fflush(stdout); }
    }
    return fbo;
}
void forget_fbos_of(GLuint tex) {
    for (auto it = g_draw_fbos.begin(); it != g_draw_fbos.end();) {
        bool uses = it->first.d == tex;
        for (GLuint c : it->first.c) uses |= c == tex;
        if (uses) { glDeleteFramebuffers(1, &it->second); it = g_draw_fbos.erase(it); } else ++it;
    }
    auto s = g_single_fbos.find(tex);
    if (s != g_single_fbos.end()) { glDeleteFramebuffers(1, &s->second); g_single_fbos.erase(s); }
}

// ---- clears -----------------------------------------------------------------------------------
// rect: x0, y0, x1, y1 in texture pixels (nullptr = whole texture).
void clear_color_tex(const HostTex& t, const float* c, const int* rect) {
    ++g_gl_epoch;
    glBindFramebuffer(GL_FRAMEBUFFER, single_fbo(t));
    if (rect) { glEnable(GL_SCISSOR_TEST); glScissor(rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]); }
    else glDisable(GL_SCISSOR_TEST);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearBufferfv(GL_COLOR, 0, c);
}
void clear_depth_tex(const HostTex& t, float z, int stencil, const int* rect) {
    ++g_gl_epoch;
    glBindFramebuffer(GL_FRAMEBUFFER, single_fbo(t));
    if (rect) { glEnable(GL_SCISSOR_TEST); glScissor(rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]); }
    else glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClearBufferfi(GL_DEPTH_STENCIL, 0, z, stencil);
}

// ---- blit -------------------------------------------------------------------------------------
// Copies src texels into dst over the rectangle [x, y, w, h]: texel = dst pixel + off; mode 0 = 1:1,
// 1 = average 2x2 samples (4x MSAA), 2 = average 1x2 (2x MSAA), 3 = one sample (scale sx, sy).
// Same shader as gpu_xedraw.inc xd_blit. Leaves program / framebuffer / state changed: callers
// rebind before drawing (g_blit_count).
uint64_t g_blit_count = 0;
// swap_rb: exchange red and blue on the way (a resolve with RB_COPY_DEST_INFO.copy_dest_swap).
void blit(const HostTex& src, const HostTex& dst, int x, int y, int w, int h, int offx, int offy, int mode, int sx, int sy,
          bool swap_rb = false) {
    if (w <= 0 || h <= 0 || !src.tex || !dst.tex) return;
    static GLuint prog = 0;
    static GLint u_off = -1, u_mode = -1, u_swap = -1;
    if (!prog) {
        static const char* const kVs =
            "#version 430\nvoid main() { vec2 t = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "  gl_Position = vec4(t * 2.0 - 1.0, 0.0, 1.0); }\n";
        static const char* const kFs =
            "#version 430\nlayout(binding = 0) uniform sampler2DArray src;\nuniform ivec2 off;\nuniform ivec3 mode;\nuniform int swp;\n"
            "layout(location = 0) out vec4 o;\n"
            "vec4 ld(ivec2 p) { return texelFetch(src, ivec3(p, 0), 0); }\n"
            "void main() { ivec2 s = ivec2(gl_FragCoord.xy) + off;\n"
            "  if (mode.x == 1) { ivec2 b = s * 2; o = 0.25 * (ld(b) + ld(b + ivec2(1, 0)) + ld(b + ivec2(0, 1)) + ld(b + ivec2(1, 1))); }\n"
            "  else if (mode.x == 2) { ivec2 b = ivec2(s.x, s.y * 2); o = 0.5 * (ld(b) + ld(b + ivec2(0, 1))); }\n"
            "  else if (mode.x == 3) o = ld(s * mode.yz);\n"
            "  else o = ld(s);\n"
            "  if (swp != 0) o = o.bgra; }\n";
        GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(v, 1, &kVs, nullptr); glCompileShader(v);
        glShaderSource(f, 1, &kFs, nullptr); glCompileShader(f);
        prog = glCreateProgram();
        glAttachShader(prog, v); glAttachShader(prog, f); glLinkProgram(prog);
        GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) { char log[512] = {}; glGetProgramInfoLog(prog, sizeof log, nullptr, log); printf("[GLRT] blit link failed: %s\n", log); fflush(stdout); }
        u_off = glGetUniformLocation(prog, "off");
        u_mode = glGetUniformLocation(prog, "mode");
        u_swap = glGetUniformLocation(prog, "swp");
    }
    ++g_blit_count;
    ++g_gl_epoch;
    glBindFramebuffer(GL_FRAMEBUFFER, single_fbo(dst));
    glUseProgram(prog);
    glUniform2i(u_off, offx, offy);
    glUniform3i(u_mode, mode, sx, sy);
    glUniform1i(u_swap, swap_rb ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, src.tex);
    glBindSampler(0, 0);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_SCISSOR_TEST);
    glViewport(x, y, w, h);
    glScissor(x, y, w, h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// ---- resolve targets --------------------------------------------------------------------------
// Host texture per guest physical destination, sampled instead of guest memory
// (gpu_xedraw.inc "Resolve destinations"). Guest memory under a target is fingerprinted at resolve
// time; a changed fingerprint means the CPU reused the memory and the target is stale.
struct Resolved {
    HostTex t; uint32_t tfmt = 0; uint64_t frame = 0; uint64_t probe = 0; uint32_t probe_bytes = 0;
    bool composite = false; uint32_t vy0 = 0xFFFFFFFFu, vy1 = 0;
    bool swap = false;   // last resolve into it had copy_dest_swap (red/blue exchanged in guest memory)
};
std::unordered_map<uint32_t, Resolved> g_resolved;

// ---- dead work (LSWTCS_GLDEADSKIP, default on) ------------------------------------------------
// A resolve whose destination nothing has sampled for kDeadReplays replays copies for nobody: the hub
// renders a blur chain (a downsample + 8 passes into 0x0B499000 / 0x0B4D9000 / 0x0B4E9000, 9 resolves)
// whose results no draw samples (Eden capture 2026-10-08). The copy of such a resolve is skipped (its
// clears still run), and so is a draw whose only output feeds it (plan_dead_work). If a skipped
// destination is sampled again its resolves resume from the next replay (one stale frame at worst).
constexpr uint64_t kDeadReplays = 120;
std::unordered_map<uint32_t, uint64_t> g_dest_first, g_dest_used;   // replay first resolved / last sampled
void note_dest_used(uint32_t dest) { g_dest_used[dest] = g_replays; }
bool deadskip_on() {
    static const bool on = [] { const char* e = getenv("LSWTCS_GLDEADSKIP"); return !(e && e[0] == '0'); }();
    return on;
}
bool dest_dead(uint32_t dest) {
    if (!deadskip_on()) return false;
    auto fi = g_dest_first.find(dest);
    if (fi == g_dest_first.end() || g_replays < fi->second + kDeadReplays) return false;
    auto ui = g_dest_used.find(dest);
    const uint64_t last = ui == g_dest_used.end() ? fi->second : ui->second;
    return g_replays > last + kDeadReplays;
}
uint64_t g_resolve_seq = 0;   // bumps on every resolve write
uint64_t g_src_epoch = 0;     // bumps when a target is dropped or replaced
uint64_t guest_probe(uint32_t phys, uint32_t bytes) {
    if (!bytes) return 0;
    const uint8_t* p = g_base + 0x80000000ull;
    uint64_t h = 1469598103934665603ull;
    const uint32_t head = std::min<uint32_t>(bytes, 4096) & ~7u;
    for (uint32_t o = 0; o < head; o += 8) { uint64_t v; memcpy(&v, p + ((uint64_t(phys) + o) & 0x1FFFFFF8ull), 8); h = (h ^ v) * 1099511628211ull; }
    for (uint32_t i = 0; i < 32; ++i) {
        const uint64_t off = (uint64_t(bytes - 8) * i) / 31;
        uint64_t v; memcpy(&v, p + ((uint64_t(phys) + off) & 0x1FFFFFF8ull), 8); h = (h ^ v) * 1099511628211ull;
    }
    return h;
}
uint32_t resolve_format(bool depth_src, uint32_t tfmt, uint32_t src) {
    if (depth_src) return kDxgiR32F;
    switch (tfmt) {
        case 2: return src;            // k_8: sampled as RGBA (the PC's LSWTCS_K8RGBA default)
        case 10: return kDxgiRG8;      // k_8_8
        case 6: return kDxgiRGBA8;     // k_8_8_8_8
        default: return src;
    }
}
Resolved* resolved_get(uint32_t dest, uint32_t w, uint32_t h, uint32_t fmt, uint32_t tfmt) {
    auto it = g_resolved.find(dest);
    if (it != g_resolved.end()) {
        Resolved& r = it->second;
        if (r.t.tex && r.t.w >= w && r.t.h >= h && r.t.fmt == fmt) { r.tfmt = tfmt; return &r; }
        forget_fbos_of(r.t.tex);
        host_tex_free(r.t);
        g_resolved.erase(it);
        ++g_src_epoch;
    }
    Resolved r;
    r.t = host_tex_create(w, h, fmt);
    r.tfmt = tfmt;
    const float z[4] = {};
    clear_color_tex(r.t, z, nullptr);
    static int n = 0;
    if (++n <= 40) { printf("[GLRT] resolve target 0x%08X %ux%u fmt=%u tfmt=%u\n", dest, w, h, fmt, tfmt); fflush(stdout); }
    return &(g_resolved[dest] = r);
}
float unorm(uint32_t v, uint32_t bits) { return float(v & ((1u << bits) - 1)) / float((1u << bits) - 1); }

// One recorded resolve: copy the source rectangle into the destination texture, then the clears.
uint32_t g_resolves_done = 0;
void do_resolve(const LswGpuDrawRec& r, bool copy = true, bool clears = true) {
    uint32_t sx, sy; msaa_scale(r.surf_info, sx, sy);
    const bool depth_src = r.rs_src >= 4;
    const int32_t x0 = r.rs_rect[0], y0 = r.rs_rect[1], x1 = r.rs_rect[2], y1 = r.rs_rect[3];
    if (copy && r.rs_copy_cmd <= 1 && x1 > x0 && y1 > y0 && r.rs_dest) {
        g_dest_first.emplace(r.rs_dest, g_replays);
        Rt* src = depth_src ? rt_depth(r.surf_info, r.depth_info) : rt_color(r.surf_info, r.color_info);
        if (src) {
            const int32_t w = x1 - x0, h = y1 - y0;
            uint32_t dw = std::max<uint32_t>(r.rs_dest_pitch, uint32_t(r.rs_dest_x + w));
            uint32_t dh = std::max<uint32_t>(r.rs_dest_height, uint32_t(r.rs_dest_y + h));
            dw = std::min<uint32_t>(dw, 8192); dh = std::min<uint32_t>(dh, 8192);
            const uint32_t dfmt = resolve_format(depth_src, r.rs_dest_fmt, src->t.fmt);
            if (Resolved* dst = resolved_get(r.rs_dest, dw, dh, dfmt, depth_src ? 22u : r.rs_dest_fmt)) {
                int32_t mode = 0;
                if (sx > 1 || sy > 1) {
                    const uint32_t ss = r.rs_sample;
                    if (!depth_src && sx == 2 && sy == 2 && ss == 6) mode = 1;        // k0123 average
                    else if (!depth_src && sx == 1 && sy == 2 && ss == 4) mode = 2;   // k01 average (2x)
                    else mode = 3;   // single sample (also 4x under LSWTCS_MSAA1X: sx 2, sy 1)
                }
                // Copied unswapped: the host texture keeps the render target's channel order, and copy_dest_swap is
                // applied where the copy is sampled (resolved_swizzle), as the guest's fetch swizzle undoes it there.
                // Swapping here turned the HUD (presented straight from the resolved front buffer) blue (2026-10-11).
                blit(src->t, dst->t, r.rs_dest_x, r.rs_dest_y, w, h, x0 - r.rs_dest_x, y0 - r.rs_dest_y, mode, int32_t(sx), int32_t(sy));
                dst->swap = r.rs_dest_swap != 0;
                dst->frame = ++g_resolve_seq;
                dst->vy0 = std::min<uint32_t>(dst->vy0, uint32_t(r.rs_dest_y));
                dst->vy1 = std::max<uint32_t>(dst->vy1, uint32_t(r.rs_dest_y + h));
                dst->probe_bytes = std::max<uint32_t>(64u, std::min<uint32_t>(dw * dh * 4u, 0x1000000u));
                dst->probe = guest_probe(r.rs_dest, dst->probe_bytes);
                ++g_resolves_done;
            }
        }
    }
    const int cr[4] = {int(x0 * sx), int(y0 * sy), int(x1 * sx), int(y1 * sy)};
    if (!clears || cr[2] <= cr[0] || cr[3] <= cr[1]) return;
    if (r.rs_clear_depth) {
        if (Rt* d = rt_depth(r.surf_info, r.depth_info)) {
            const uint32_t v = (r.rs_depth_clear >> 8) & 0xFFFFFF;
            float z = unorm(v, 24);
            if ((r.depth_info >> 16) & 1) {   // kD24FS8: 20e4 float in [0, 2), host stores half
                const uint32_t e = v >> 20, m = v & 0xFFFFF;
                const float fz = e == 0 ? float(m) * (1.0f / 1048576.0f) * (1.0f / 16384.0f)
                                        : std::ldexp(1.0f + float(m) / 1048576.0f, int(e) - 15);
                z = fz * 0.5f;
            }
            clear_depth_tex(d->t, z, int(r.rs_depth_clear & 0xFF), cr);
        }
    }
    if (r.rs_clear_color) {
        if (Rt* c = rt_color(r.surf_info, r.color_info)) {
            const uint32_t v = r.rs_color_clear;
            float cc[4];
            switch ((r.color_info >> 16) & 0xF) {
                case 2: case 10: case 3: case 12:
                    for (int j = 0; j < 3; ++j) cc[j] = unorm(v >> (j * 10), 10);
                    cc[3] = unorm(v >> 30, 2);
                    break;
                default:
                    for (int j = 0; j < 4; ++j) cc[j] = unorm(v >> (j * 8), 8);
                    break;
            }
            clear_color_tex(c->t, cc, cr);
        }
    }
}

// ---- texture sources for resolved data ---------------------------------------------------------
// gpu_xedraw.inc "Sized views" + "Texture source for a fetch constant that points at resolved data":
// a fetch whose base has a resolve target (and/or sub-range targets from predicated tiling) samples
// a host texture of exactly the fetch size instead of guest memory.
uint32_t tfmt_bpp(uint32_t tfmt) {
    switch (tfmt) { case 2: return 1; case 3: case 4: case 5: case 10: case 24: case 26: return 2; default: return 4; }
}
struct View { HostTex t; uint64_t src_frame = 0, made_frame = 0; };
std::unordered_map<uint64_t, View> g_views;
const HostTex* resolved_view(uint32_t base, Resolved& src, uint32_t w, uint32_t h) {
    if (!w || !h || (w == src.t.w && h == src.t.h) || w > 8192 || h > 8192) return nullptr;
    const uint64_t key = (uint64_t(base) << 32) ^ (uint64_t(w) << 16) ^ uint64_t(h) ^ (uint64_t(src.t.fmt) << 56);
    View& v = g_views[key];
    if (!v.t.tex) {
        v.t = host_tex_create(w, h, src.t.fmt);
        const float z[4] = {};
        clear_color_tex(v.t, z, nullptr);
    }
    if (v.src_frame != src.frame || (src.composite && v.made_frame != g_replays)) {
        v.made_frame = g_replays; v.src_frame = src.frame;
        blit(src.t, v.t, 0, 0, int(std::min(w, src.t.w)), int(std::min(h, src.t.h)), 0, 0, 0, 1, 1);
    }
    return &v.t;
}
std::unordered_map<uint32_t, Resolved> g_composed;
// *swap (optional): the source's resolve had copy_dest_swap (for a stitched source: its first piece).
bool texture_source(uint32_t base, uint32_t tfmt, uint32_t w, uint32_t h, const HostTex*& out, bool* swap = nullptr) {
    auto ex = g_resolved.find(base);
    if (ex != g_resolved.end()) note_dest_used(base);
    if (ex != g_resolved.end() && (!ex->second.t.tex || ex->second.composite)) ex = g_resolved.end();
    if (ex != g_resolved.end() && ex->second.probe_bytes && guest_probe(base, ex->second.probe_bytes) != ex->second.probe) {
        forget_fbos_of(ex->second.t.tex);   // stale: the CPU reused the memory
        host_tex_free(ex->second.t);
        g_resolved.erase(ex);
        ex = g_resolved.end();
        ++g_src_epoch;
    }
    std::vector<std::pair<uint32_t, Resolved*>> subs;
    uint64_t row_bytes = 0;
    if (w && h) {
        const uint32_t bpp = tfmt_bpp(tfmt), pitch = (w + 31) & ~31u, rows = (h + 31) & ~31u;
        row_bytes = uint64_t(pitch) * bpp;
        const uint64_t end = uint64_t(base) + row_bytes * rows;
        for (auto& kv : g_resolved)
            if (kv.first > base && kv.first < end && kv.second.t.tex && !kv.second.composite && ((kv.first - base) % row_bytes) == 0)
                subs.push_back({kv.first, &kv.second});
    }
    if (swap) *swap = ex != g_resolved.end() ? ex->second.swap : !subs.empty() && subs[0].second->swap;
    if (subs.empty()) {
        if (ex == g_resolved.end()) return false;
        Resolved& rr = ex->second;
        if (const HostTex* vw = resolved_view(base, rr, w, h)) { out = vw; return true; }
        out = &rr.t;
        return true;
    }
    const uint32_t fmt = ex != g_resolved.end() ? ex->second.t.fmt : subs[0].second->t.fmt;
    uint64_t seq = ex != g_resolved.end() ? ex->second.frame : 0;
    for (auto& sp : subs) seq = std::max<uint64_t>(seq, sp.second->frame);
    Resolved& c = g_composed[base];
    if (c.t.tex && (c.t.w != w || c.t.h != h || c.t.fmt != fmt)) { forget_fbos_of(c.t.tex); host_tex_free(c.t); c = Resolved{}; ++g_src_epoch; }
    if (!c.t.tex) {
        c.t = host_tex_create(w, h, fmt);
        const float z[4] = {};
        clear_color_tex(c.t, z, nullptr);
        c.frame = ~0ull;
    }
    if (c.frame != seq) {
        c.frame = seq;
        // Blit in resolve order, newest last: on the 360 all of these share one piece of memory, so whichever resolve
        // wrote a row most recently is what the texture holds. The old order (base first, then every sub-range) let
        // the per-tile colour resolves of tiles 2/3 (written before the shadow composite) paste their unshadowed rows
        // over the later full-screen resolve of the shadowed image: hardware "band" below the first tile seam
        // (row 320 of 704), shadows only in the top part of the screen (2026-10-09).
        std::vector<std::pair<uint64_t, std::pair<Resolved*, uint32_t>>> order;   // (resolve seq, (source, row offset))
        if (ex != g_resolved.end() && ex->second.t.fmt == c.t.fmt) order.push_back({ex->second.frame, {&ex->second, 0u}});
        for (auto& sp : subs) {
            note_dest_used(sp.first);
            if (sp.second->t.fmt != c.t.fmt) continue;
            const uint32_t yoff = uint32_t((sp.first - base) / row_bytes);
            if (yoff < c.t.h) order.push_back({sp.second->frame, {sp.second, yoff}});
        }
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& o : order) {
            Resolved& s = *o.second.first;
            const uint32_t yoff = o.second.second;
            uint32_t y0 = s.vy0 == 0xFFFFFFFFu ? 0 : s.vy0, y1 = s.vy1 ? s.vy1 : s.t.h;
            y1 = std::min<uint32_t>(y1, std::min<uint32_t>(s.t.h, c.t.h - yoff));
            if (y1 <= y0) continue;
            blit(s.t, c.t, 0, int(yoff + y0), int(std::min(s.t.w, c.t.w)), int(y1 - y0), 0, -int(yoff), 0, 1, 1);
        }
    }
    out = &c.t;
    return true;
}

// Swizzle for a fetch bound to a resolved source (gpu_xedraw.inc tex_table): single-channel host
// formats replicate channel 0, k_8 resolves keep RGBA (sampled as-is), constants 0/1 pass through.
// swap: the resolve that wrote the source had copy_dest_swap. On the console guest memory then holds red and blue
// exchanged and the fetch swizzle (e.g. A8R8G8B8's) maps them back; the host texture holds the render target's own
// order, so the swizzle's red/blue sources are exchanged instead. Until 2026-10-11 the game's render-target format
// switch (sub_8231B770) was broken and its surfaces arrived as format 2 with no swizzle, sampled as identity.
uint32_t resolved_swizzle(uint32_t guest_swizzle, uint32_t tfmt, uint32_t host_fmt, bool swap = false) {
    const bool single = host_fmt == kDxgiR8 || host_fmt == kDxgiR32F;
    if (tfmt == 2 && !single) return 0x688;   // RGBA identity
    uint32_t map = 0;
    for (int c = 0; c < 4; ++c) {
        uint32_t s = (guest_swizzle >> (3 * c)) & 7;
        if (swap && !single && (s == 0 || s == 2)) s ^= 2;
        if (s < 4 && single) s = 0; else if (s > 5) s = 4;
        map |= s << (3 * c);
    }
    return map;
}
