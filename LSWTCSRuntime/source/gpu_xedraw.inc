// ══ Translated-shader replay (LSWTCS_GPUDRAW=1): draws recorded by kernel_stubs gpu_record_draw ══
// Included into gpu_d3d12.cpp (uses its device / command list / barrier helpers).
// Xenia bindful root-signature layout (see include/lsw_gpu_bridge.h). Shared memory = 512 MB
// DEFAULT buffer mirroring guest physical memory; each draw's vertex ranges are copied in (deduped
// by content hash) before the draw. Textures: null SRVs for now (sampling returns 0).
#include "lsw_gpu_frame.h"
#include <map>
#include <unordered_map>
static void barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
static bool g_xd_inited = false, g_xd_ok = false;
// Depth-stencil format of EDRAM depth targets (LSWTCS_STENCIL, default on: D32 + S8). The game uses
// stencil shadow volumes; without stencil the volumes darken everything they cover.
static bool xd_stencil() { static int v = -1; if (v < 0) { const char* e = getenv("LSWTCS_STENCIL"); v = (e && e[0] == '0') ? 0 : 1; } return v == 1; }
static DXGI_FORMAT g_xd_pso_dsv = DXGI_FORMAT_D32_FLOAT;   // DSV format the next PSOs are built for
static ID3D12DescriptorHeap* g_xd_rtv_heap = nullptr;   // CPU-only: RTVs (RTs + resolve targets)
static ID3D12DescriptorHeap* g_xd_dsv_heap = nullptr;   // CPU-only: DSVs
static UINT g_xd_rtv_used = 0, g_xd_dsv_used = 0, g_xd_rtv_inc = 0, g_xd_dsv_inc = 0;
static const UINT kXdMaxRtv = 1024, kXdMaxDsv = 256;
static ID3D12Resource* g_xd_shm = nullptr;                    // shared memory
static D3D12_RESOURCE_STATES g_xd_shm_state = D3D12_RESOURCE_STATE_COPY_DEST;
static ID3D12Resource* g_xd_up[kFrameCount] = {};             // per-frame upload buffers
static uint8_t* g_xd_up_ptr[kFrameCount] = {};
static uint64_t g_xd_up_cap[kFrameCount] = {};
static ID3D12DescriptorHeap* g_xd_srv_heap = nullptr;         // shader-visible CBV/SRV/UAV
static ID3D12DescriptorHeap* g_xd_smp_heap = nullptr;         // shader-visible samplers
static UINT g_xd_srv_inc = 0, g_xd_smp_inc = 0;
static const UINT kXdDescPerFrame = 32768, kXdSmpPerFrame = 1024;
static std::unordered_map<uint32_t, ID3D12RootSignature*> g_xd_rootsigs;
static std::unordered_map<uint64_t, ID3D12PipelineState*> g_xd_psos;
// Shared-memory residency (which vertex ranges need uploading) is decided at record time:
// kernel_stubs.cpp gpu_shm_upload_needed. Ranges marked kLswRangeResident carry no bytes.

static void xd_init() {
    g_xd_inited = true;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 0x20000000ull;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_ID3D12Resource, (void**)&g_xd_shm))) {
        printf("[XEDRAW] shared memory alloc failed\n"); return; }
    D3D12_DESCRIPTOR_HEAP_DESC dh = {}; dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dh.NumDescriptors = kXdDescPerFrame * kFrameCount + 2; dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&dh, IID_ID3D12DescriptorHeap, (void**)&g_xd_srv_heap);
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; dh.NumDescriptors = kXdSmpPerFrame * kFrameCount;
    g_device->CreateDescriptorHeap(&dh, IID_ID3D12DescriptorHeap, (void**)&g_xd_smp_heap);
    if (!g_xd_srv_heap || !g_xd_smp_heap) { printf("[XEDRAW] descriptor heap alloc failed\n"); return; }
    g_xd_srv_inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_xd_smp_inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    // Descriptors 0,1: shared memory raw SRV (t0) + raw UAV (u0), contiguous as the table expects.
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_xd_srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = DXGI_FORMAT_R32_TYPELESS;
    sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Buffer.NumElements = 0x20000000u / 4; sv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    g_device->CreateShaderResourceView(g_xd_shm, &sv, h);
    h.ptr += g_xd_srv_inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {}; uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uv.Buffer.NumElements = 0x20000000u / 4; uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g_device->CreateUnorderedAccessView(g_xd_shm, nullptr, &uv, h);
    D3D12_DESCRIPTOR_HEAP_DESC rh = {}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = kXdMaxRtv;
    g_device->CreateDescriptorHeap(&rh, IID_ID3D12DescriptorHeap, (void**)&g_xd_rtv_heap);
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; rh.NumDescriptors = kXdMaxDsv;
    g_device->CreateDescriptorHeap(&rh, IID_ID3D12DescriptorHeap, (void**)&g_xd_dsv_heap);
    if (!g_xd_rtv_heap || !g_xd_dsv_heap) { printf("[XEDRAW] RTV/DSV heap alloc failed\n"); return; }
    g_xd_rtv_inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_xd_dsv_inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    g_xd_ok = true;
    printf("[XEDRAW] translated-shader replay ready (512MB shared memory)\n"); fflush(stdout);
}

static ID3D12RootSignature* xd_rootsig(uint32_t pt, uint32_t ps, uint32_t vt, uint32_t vs) {
    uint32_t key = pt | (ps << 8) | (vt << 16) | (vs << 24);
    auto it = g_xd_rootsigs.find(key); if (it != g_xd_rootsigs.end()) return it->second;
    D3D12_ROOT_PARAMETER p[10] = {}; UINT n = 0;
    auto cbv = [&](UINT reg, D3D12_SHADER_VISIBILITY vis) {
        p[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[n].Descriptor.ShaderRegister = reg;
        p[n].ShaderVisibility = vis; ++n; };
    cbv(3, D3D12_SHADER_VISIBILITY_ALL);       // 0 fetch constants
    cbv(1, D3D12_SHADER_VISIBILITY_VERTEX);    // 1 float constants VS
    cbv(1, D3D12_SHADER_VISIBILITY_PIXEL);     // 2 float constants PS
    cbv(0, D3D12_SHADER_VISIBILITY_ALL);       // 3 system constants
    cbv(2, D3D12_SHADER_VISIBILITY_ALL);       // 4 bool/loop constants
    D3D12_DESCRIPTOR_RANGE shm[2] = {};
    shm[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; shm[0].NumDescriptors = 1; shm[0].BaseShaderRegister = 0; shm[0].OffsetInDescriptorsFromTableStart = 0;
    shm[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; shm[1].NumDescriptors = 1; shm[1].BaseShaderRegister = 0; shm[1].OffsetInDescriptorsFromTableStart = 1;
    p[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[n].DescriptorTable.NumDescriptorRanges = 2;
    p[n].DescriptorTable.pDescriptorRanges = shm; p[n].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; ++n;   // 5
    D3D12_DESCRIPTOR_RANGE r[4] = {};
    auto table = [&](int i, D3D12_DESCRIPTOR_RANGE_TYPE t, UINT count, UINT base, D3D12_SHADER_VISIBILITY vis) {
        r[i].RangeType = t; r[i].NumDescriptors = count; r[i].BaseShaderRegister = base; r[i].OffsetInDescriptorsFromTableStart = 0;
        p[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[n].DescriptorTable.NumDescriptorRanges = 1;
        p[n].DescriptorTable.pDescriptorRanges = &r[i]; p[n].ShaderVisibility = vis; ++n; };
    if (pt) table(0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, pt, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    if (ps) table(1, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, ps, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    if (vt) table(2, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, vt, 1, D3D12_SHADER_VISIBILITY_VERTEX);
    if (vs) table(3, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, vs, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    D3D12_ROOT_SIGNATURE_DESC d = {}; d.NumParameters = n; d.pParameters = p;
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr; ID3D12RootSignature* rs = nullptr;
    if (SUCCEEDED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
        g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_ID3D12RootSignature, (void**)&rs);
    else printf("[XEDRAW] rootsig fail: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
    safe_rel(blob); safe_rel(err);
    g_xd_rootsigs[key] = rs;
    return rs;
}

static D3D12_BLEND xd_blend(uint32_t f) {
    static const D3D12_BLEND m[17] = { D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_ZERO,
        D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA,
        D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
        D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR,
        D3D12_BLEND_SRC_ALPHA_SAT };
    return f < 17 ? m[f] : D3D12_BLEND_ONE;
}
static D3D12_BLEND xd_blend_a(uint32_t f) {   // alpha-channel factors may not use *_COLOR
    D3D12_BLEND b = xd_blend(f);
    if (b == D3D12_BLEND_SRC_COLOR) return D3D12_BLEND_SRC_ALPHA;
    if (b == D3D12_BLEND_INV_SRC_COLOR) return D3D12_BLEND_INV_SRC_ALPHA;
    if (b == D3D12_BLEND_DEST_COLOR) return D3D12_BLEND_DEST_ALPHA;
    if (b == D3D12_BLEND_INV_DEST_COLOR) return D3D12_BLEND_INV_DEST_ALPHA;
    return b;
}
static D3D12_BLEND_OP xd_blend_op(uint32_t o) {
    static const D3D12_BLEND_OP m[5] = { D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT, D3D12_BLEND_OP_MIN,
                                         D3D12_BLEND_OP_MAX, D3D12_BLEND_OP_REV_SUBTRACT };
    return o < 5 ? m[o] : D3D12_BLEND_OP_ADD;
}

static ID3D12PipelineState* xd_pso(const LswGpuDrawRec& r, ID3D12RootSignature* rs, const DXGI_FORMAT* fmts, uint32_t nrt) {
    DXGI_FORMAT rtv_fmt = nrt ? fmts[0] : DXGI_FORMAT_UNKNOWN;
    // D3D12_PRIMITIVE_TOPOLOGY_TYPE: 1 point, 2 line (incl. LINELIST_ADJ = 10), 3 triangle.
    uint32_t topo_type = (r.topology == 1) ? 1u : ((r.topology <= 3 || r.topology == 10) ? 2u : 3u);
    uint64_t st = uint64_t(r.blendcontrol) ^ (uint64_t(r.color_mask & 0xF) << 32) ^ (uint64_t(r.depthcontrol) * 0x9E3779B97F4A7C15ull)
                ^ (uint64_t(r.stencil_ref & 0xFFFF00u) * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t(r.stencil_ref_bf & 0xFFFF00u) * 0x165667B19E3779F9ull)
                ^ (uint64_t(g_xd_pso_dsv) << 24)
                ^ (uint64_t(r.sc_mode_cntl & 0x7) << 44) ^ (uint64_t(topo_type) << 48) ^ (uint64_t(rtv_fmt) << 52);
    for (uint32_t i = 1; i < nrt; ++i) st = st * 0x100000001B3ull ^ (uint64_t(fmts[i]) << 8) ^ r.blend_mrt[i] ^ (uint64_t((r.color_mask >> (4 * i)) & 0xF) << 40);
    st ^= uint64_t(nrt) << 60;
    uint64_t key = r.vs_key * 0x9E3779B97F4A7C15ull ^ r.ps_key * 0xC2B2AE3D27D4EB4Full ^ st * 0x165667B19E3779F9ull
                 ^ uint64_t(reinterpret_cast<uintptr_t>(r.gs_dxbc)) * 0x94D049BB133111EBull
                 ^ uint64_t(reinterpret_cast<uintptr_t>(rs)) * 0x27D4EB2F165667C5ull;
    auto it = g_xd_psos.find(key); if (it != g_xd_psos.end()) return it->second;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { r.vs_dxbc, r.vs_size };
    if (r.ps_dxbc) pd.PS = { r.ps_dxbc, r.ps_size };
    if (r.gs_dxbc) pd.GS = { r.gs_dxbc, r.gs_size };
    pd.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)topo_type;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    uint32_t cull = r.sc_mode_cntl & 3;   // PA_SU_SC_MODE_CNTL: bit0 cull front, bit1 cull back, bit2 face (1 = CW is front)
    pd.RasterizerState.CullMode = (cull == 1) ? D3D12_CULL_MODE_FRONT : (cull == 2) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = (r.sc_mode_cntl & 4) ? FALSE : TRUE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    uint32_t bc = r.blendcontrol;
    auto& bt = pd.BlendState.RenderTarget[0];
    uint32_t cs = bc & 0x1F, cop = (bc >> 5) & 7, cd = (bc >> 8) & 0x1F, as = (bc >> 16) & 0x1F, aop = (bc >> 21) & 7, ad = (bc >> 24) & 0x1F;
    bt.BlendEnable = !(cs == 1 && cd == 0 && cop == 0 && as == 1 && ad == 0 && aop == 0);
    bt.SrcBlend = xd_blend(cs); bt.DestBlend = xd_blend(cd); bt.BlendOp = xd_blend_op(cop);
    bt.SrcBlendAlpha = xd_blend_a(as); bt.DestBlendAlpha = xd_blend_a(ad); bt.BlendOpAlpha = xd_blend_op(aop);
    bt.RenderTargetWriteMask = UINT8(r.color_mask & 0xF);
    if (!r.ps_dxbc) bt.RenderTargetWriteMask = 0;
    bool zen = (r.depthcontrol >> 1) & 1, zwr = (r.depthcontrol >> 2) & 1; uint32_t zf = (r.depthcontrol >> 4) & 7;
    pd.DepthStencilState.DepthEnable = (zen || zwr) ? TRUE : FALSE;
    pd.DepthStencilState.DepthWriteMask = zwr ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = zen ? (D3D12_COMPARISON_FUNC)(zf + 1) : D3D12_COMPARISON_FUNC_ALWAYS;
    pd.SampleMask = UINT_MAX; pd.NumRenderTargets = nrt; pd.RTVFormats[0] = rtv_fmt;
    if (rtv_fmt == DXGI_FORMAT_UNKNOWN) bt.RenderTargetWriteMask = 0;
    pd.BlendState.IndependentBlendEnable = nrt > 1 ? TRUE : FALSE;
    for (uint32_t i = 1; i < nrt && i < 4; ++i) {
        auto& b = pd.BlendState.RenderTarget[i];
        uint32_t c2 = r.blend_mrt[i];
        uint32_t s2 = c2 & 0x1F, o2 = (c2 >> 5) & 7, d2 = (c2 >> 8) & 0x1F, sa2 = (c2 >> 16) & 0x1F, oa2 = (c2 >> 21) & 7, da2 = (c2 >> 24) & 0x1F;
        b.BlendEnable = !(s2 == 1 && d2 == 0 && o2 == 0 && sa2 == 1 && da2 == 0 && oa2 == 0);
        b.SrcBlend = xd_blend(s2); b.DestBlend = xd_blend(d2); b.BlendOp = xd_blend_op(o2);
        b.SrcBlendAlpha = xd_blend_a(sa2); b.DestBlendAlpha = xd_blend_a(da2); b.BlendOpAlpha = xd_blend_op(oa2);
        b.RenderTargetWriteMask = UINT8((r.color_mask >> (4 * i)) & 0xF);
        pd.RTVFormats[i] = fmts[i];
        if (!(r.ps_rt_mask & (1u << i))) b.RenderTargetWriteMask = 0;
    }
    pd.DSVFormat = g_xd_pso_dsv; pd.SampleDesc.Count = 1;
    // Stencil (RB_DEPTHCONTROL bit0 enable, bit7 backface, front 8-19, back 20-31; ops/funcs map +1).
    if (g_xd_pso_dsv != DXGI_FORMAT_D32_FLOAT && (r.depthcontrol & 1)) {
        uint32_t dc = r.depthcontrol;
        auto& ds = pd.DepthStencilState;
        ds.StencilEnable = TRUE;
        ds.StencilReadMask = UINT8((r.stencil_ref >> 8) & 0xFF);
        ds.StencilWriteMask = UINT8((r.stencil_ref >> 16) & 0xFF);
        ds.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC(((dc >> 8) & 7) + 1);
        ds.FrontFace.StencilFailOp = D3D12_STENCIL_OP(((dc >> 11) & 7) + 1);
        ds.FrontFace.StencilPassOp = D3D12_STENCIL_OP(((dc >> 14) & 7) + 1);
        ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP(((dc >> 17) & 7) + 1);
        if (dc & 0x80) {
            ds.BackFace.StencilFunc = D3D12_COMPARISON_FUNC(((dc >> 20) & 7) + 1);
            ds.BackFace.StencilFailOp = D3D12_STENCIL_OP(((dc >> 23) & 7) + 1);
            ds.BackFace.StencilPassOp = D3D12_STENCIL_OP(((dc >> 26) & 7) + 1);
            ds.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP(((dc >> 29) & 7) + 1);
        } else ds.BackFace = ds.FrontFace;
    }
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = g_device->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState, (void**)&pso);
    if (FAILED(hr)) { static int nf = 0; if (++nf <= 10) { printf("[XEDRAW] PSO fail 0x%08X (vs %u B, ps %u B)\n", (unsigned)hr, r.vs_size, r.ps_size); fflush(stdout); } }
    g_xd_psos[key] = pso;
    return pso;
}



// ── Texture cache: guest fetch constant -> decoded D3D12 texture (base level only for now) ──
extern uint8_t* g_base;
struct XdTex { ID3D12Resource* res; uint32_t dxgi, dim, layers, mapping, mips; };
static std::unordered_map<uint64_t, XdTex> g_xd_tex;
static std::vector<ID3D12Resource*> g_xd_tex_garbage[kFrameCount];   // upload buffers to free
static uint32_t g_xd_tex_created = 0;
// Per-replay memo: fetch constant -> texture. lsw_gpu_texture_key probes ~64 scattered cache lines
// of the texture, so each distinct fetch constant is resolved once per replay instead of once per draw
// that binds it. (On the render thread guest threads keep running, so a texture written during a
// replay is seen as of its first bind in that replay, like a real GPU reading memory asynchronously.)
struct XdFetchKey {
    uint32_t w[6];
    bool operator==(const XdFetchKey& o) const { return memcmp(w, o.w, sizeof w) == 0; }
};
struct XdFetchKeyHash {
    size_t operator()(const XdFetchKey& k) const {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t v : k.w) h = (h ^ v) * 1099511628211ull;
        return size_t(h);
    }
};
static std::unordered_map<XdFetchKey, const struct XdTex*, XdFetchKeyHash> g_xd_tex_memo;
static uint64_t g_xd_replay_seq = 0, g_xd_tex_memo_seq = ~0ull;
static const XdTex* xd_get_texture_uncached(const uint32_t* fetch6);
static const XdTex* xd_get_texture(const uint32_t* fetch6) {
    if (g_xd_tex_memo_seq != g_xd_replay_seq) { g_xd_tex_memo.clear(); g_xd_tex_memo_seq = g_xd_replay_seq; }
    XdFetchKey k; memcpy(k.w, fetch6, sizeof k.w);
    auto it = g_xd_tex_memo.find(k);
    if (it != g_xd_tex_memo.end()) return it->second;
    const XdTex* t = xd_get_texture_uncached(fetch6);
    g_xd_tex_memo.emplace(k, t);
    return t;
}
static const XdTex* xd_get_texture_uncached(const uint32_t* fetch6) {
    uint8_t* phys = g_base + 0x80000000ull;
    uint64_t key = lsw_gpu_texture_key(fetch6, phys);
    auto it = g_xd_tex.find(key);
    if (it != g_xd_tex.end()) return it->second.res ? &it->second : nullptr;
    XdTex t{};
    LswGpuTexDesc d;
    if (!lsw_gpu_decode_texture(fetch6, phys, &d) || !d.ok) { g_xd_tex[key] = t; return nullptr; }
    // DIAG (LSWTCS_TEXDUMP=N): decode the first N BC1 textures (base level, layer 0) to shots/tex_*.png,
    // reading the uploaded bytes exactly as the GPU will, plus the raw fetch constant.
    { static long nd = -2; if (nd == -2) { const char* e = getenv("LSWTCS_TEXDUMP"); nd = e ? atol(e) : 0; }
      if (nd > 0 && d.dxgi == 71 && d.width >= 32 && d.height >= 32) {
          --nd;
          uint32_t w = d.width, h = d.height, bw = (w + 3) / 4;
          std::vector<uint8_t> px(size_t(w) * h * 4, 0);
          auto c565 = [](uint16_t c, uint8_t* o) { o[0] = uint8_t(((c >> 11) & 31) * 255 / 31); o[1] = uint8_t(((c >> 5) & 63) * 255 / 63); o[2] = uint8_t((c & 31) * 255 / 31); };
          for (uint32_t by = 0; by < (h + 3) / 4; ++by) for (uint32_t bx = 0; bx < bw; ++bx) {
              const uint8_t* b = d.data + d.mip_offset[0] + size_t(by) * d.mip_row_bytes[0] + bx * 8;
              uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
              uint32_t idx = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
              uint8_t pal[4][3]; c565(c0, pal[0]); c565(c1, pal[1]);
              for (int k = 0; k < 3; ++k) {
                  if (c0 > c1) { pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3); pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3); }
                  else { pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
              }
              for (int j = 0; j < 16; ++j) {
                  uint32_t x = bx * 4 + (j & 3), y = by * 4 + (j >> 2); if (x >= w || y >= h) continue;
                  uint32_t sidx = (idx >> (2 * j)) & 3; uint8_t* o = &px[(size_t(y) * w + x) * 4];
                  o[0] = pal[sidx][2]; o[1] = pal[sidx][1]; o[2] = pal[sidx][0]; o[3] = 255;   // BGRA for the PNG writer
              }
          }
          _mkdir("shots");
          char path[96]; snprintf(path, sizeof path, "shots/tex_%llu_%ux%u.png", (unsigned long long)key & 0xFFFFFF, w, h);
          lswtcs_write_png(path, px.data(), int(w), int(h), int(w * 4));
          printf("[TEXDUMP] %s fetch=%08X %08X %08X %08X %08X %08X map=0x%X\n", path, fetch6[0], fetch6[1], fetch6[2], fetch6[3], fetch6[4], fetch6[5], d.component_mapping); fflush(stdout);
      } }
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (d.width + d.block - 1) / d.block * d.block; rd.Height = (d.height + d.block - 1) / d.block * d.block;
    rd.DepthOrArraySize = UINT16(d.layers); rd.MipLevels = UINT16(d.mips ? d.mips : 1); rd.Format = (DXGI_FORMAT)d.dxgi; rd.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_ID3D12Resource, (void**)&t.res))) { g_xd_tex[key] = XdTex{}; return nullptr; }
    UINT nm = d.mips ? d.mips : 1;
    UINT nl = d.layers; if (nl * nm > 256) nl = 256 / nm;
    rd.DepthOrArraySize = UINT16(nl);
    static D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[256]; static UINT nrows[256]; static UINT64 rowsz[256]; UINT64 total = 0;
    g_device->GetCopyableFootprints(&rd, 0, nl * nm, 0, fp, nrows, rowsz, &total);
    ID3D12Resource* upb = nullptr;
    D3D12_HEAP_PROPERTIES uhp = {}; uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ub = {}; ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; ub.Width = total;
    ub.Height = 1; ub.DepthOrArraySize = 1; ub.MipLevels = 1; ub.SampleDesc.Count = 1; ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &ub, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_ID3D12Resource, (void**)&upb))) { safe_rel(t.res); g_xd_tex[key] = XdTex{}; return nullptr; }
    uint8_t* map = nullptr; D3D12_RANGE none = {}; upb->Map(0, &none, (void**)&map);
    memset(map, 0, (size_t)total);
    for (UINT l = 0; l < nl; ++l)
        for (UINT m = 0; m < nm; ++m) {
            UINT sub = m + l * nm;
            const uint8_t* srcl = d.data + size_t(l) * d.layer_bytes + d.mip_offset[m];
            uint32_t rb = d.mip_row_bytes[m];
            for (UINT row = 0; row < nrows[sub] && row < d.mip_rows[m]; ++row)
                memcpy(map + fp[sub].Offset + size_t(row) * fp[sub].Footprint.RowPitch, srcl + size_t(row) * rb,
                       rb < rowsz[sub] ? rb : (size_t)rowsz[sub]);
        }
    upb->Unmap(0, nullptr);
    for (UINT sub = 0; sub < nl * nm; ++sub) {
        D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = t.res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = sub;
        D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = upb; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp[sub];
        g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    barrier(t.res, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_xd_tex_garbage[g_frame_idx].push_back(upb);
    t.dxgi = d.dxgi; t.dim = d.dim; t.layers = nl; t.mapping = d.component_mapping; t.mips = nm;
    g_xd_tex[key] = t;
    if (++g_xd_tex_created <= 40) { printf("[XEDRAW] texture #%u %ux%u x%u fmt=%u dim=%u map=0x%X\n", g_xd_tex_created, d.width, d.height, d.layers, d.dxgi, d.dim, d.component_mapping); fflush(stdout); }
    return &g_xd_tex[key];
}

// ── EDRAM render targets (LSWTCS_GPURT, default on) ──────────────────────────────────────────
// Host RT per (EDRAM base, effective pitch, format). MSAA surfaces live in sample space: 4x =
// pitch*2 by rows*2, 2x = rows*2 (Xenos stores samples as extra EDRAM pixels), so a fast clear
// drawn on a 4x surface of pitch P lands on the 1x surface of pitch 2P covering the same EDRAM.
// Every RT is kXdRtH rows tall so any color/depth pair of the same pitch can be bound together.
static const UINT kXdRtH = 2048;
struct XdRt { ID3D12Resource* res; D3D12_RESOURCE_STATES state; UINT w; DXGI_FORMAT fmt; UINT view; bool depth; };
static std::unordered_map<uint64_t, XdRt> g_xd_rts;
// Shader-visible descriptor cursor for the frame being recorded (draw textures + blit sources).
static UINT g_xd_desc_base = 0, g_xd_desc_used = 0;
// SRV table dedup (LSWTCS_BINDCACHE): view spec per written descriptor of the current replay, and
// content hash -> table. Both are reset per replay; resources stay alive for the frame (garbage lists).
struct XdSrvSpec { ID3D12Resource* res; uint32_t fmt, map, dim, mips, layers, pad; };   // no padding: hashed/compared as bytes
struct XdSrvTable { UINT first, count; };
static std::vector<XdSrvSpec> g_xd_srv_specs(kXdDescPerFrame);
static std::unordered_map<uint64_t, XdSrvTable> g_xd_srv_tables;
static void xd_srv_create(const XdSrvSpec& s, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = (DXGI_FORMAT)s.fmt; sv.Shader4ComponentMapping = s.map; sv.ViewDimension = (D3D12_SRV_DIMENSION)s.dim;
    switch (s.dim) {
        case D3D12_SRV_DIMENSION_TEXTURE3D: sv.Texture3D.MipLevels = s.mips; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE: sv.TextureCube.MipLevels = s.mips; break;
        default: sv.Texture2DArray.MipLevels = s.mips; sv.Texture2DArray.ArraySize = s.layers; break;
    }
    g_device->CreateShaderResourceView(s.res, &sv, h);
}
// Sampler descriptor tables, deduplicated by content and kept for the whole run: a hub frame binds
// ~2000 samplers but only a handful of distinct tables, and the shader-visible sampler heap is capped
// at 2048. Tables are never rewritten, so frames in flight can share them safely.
static std::unordered_map<std::string, UINT> g_xd_smp_cache;
static UINT g_xd_smp_next = 0;
// LSWTCS_BINDCACHE: same tables keyed by content hash, verified against the descs kept per heap slot.
static std::unordered_map<uint64_t, struct XdSrvTable> g_xd_smp_hcache;
static D3D12_SAMPLER_DESC g_xd_smp_descs[kXdSmpPerFrame * kFrameCount];

static bool xd_gpurt() { static int v = -1; if (v < 0) { const char* e = getenv("LSWTCS_GPURT"); v = (e && e[0] == '0') ? 0 : 1; } return v == 1; }
// RB_COLOR_INFO color_format (xenos::ColorRenderTargetFormat) -> host format.
static DXGI_FORMAT xd_color_format(uint32_t f) {
    switch (f) {
        case 2: case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case 3: case 12: case 7: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case 4: return DXGI_FORMAT_R16G16_SNORM;
        case 5: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case 6: return DXGI_FORMAT_R16G16_FLOAT;
        case 14: return DXGI_FORMAT_R32_FLOAT;
        case 15: return DXGI_FORMAT_R32G32_FLOAT;
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;   // k_8_8_8_8, k_8_8_8_8_GAMMA
    }
}
static void xd_msaa_scale(uint32_t surf_info, UINT& sx, UINT& sy) {
    uint32_t msaa = (surf_info >> 16) & 3;   // 0 1x, 1 2x, 2 4x
    sx = msaa == 2 ? 2 : 1; sy = msaa >= 1 ? 2 : 1;
}
static void xd_rt_to(XdRt& rt, D3D12_RESOURCE_STATES s) {
    if (rt.state != s) { barrier(rt.res, rt.state, s); rt.state = s; }
}
static XdRt* xd_rt_get(bool depth, uint32_t base, UINT w, DXGI_FORMAT fmt) {
    if (w == 0 || w > 8192) return nullptr;
    uint64_t key = (uint64_t(depth) << 63) | (uint64_t(base & 0xFFF) << 40) | (uint64_t(w) << 16) | uint64_t(fmt);
    auto it = g_xd_rts.find(key);
    if (it != g_xd_rts.end()) return it->second.res ? &it->second : nullptr;
    XdRt rt{}; rt.w = w; rt.fmt = fmt; rt.depth = depth;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = kXdRtH; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Format = depth ? (xd_stencil() ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_TYPELESS) : fmt;
    rd.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {}; cv.Format = depth ? (xd_stencil() ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT) : fmt; cv.DepthStencil.Depth = 1.0f;
    rt.state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    bool room = depth ? g_xd_dsv_used < kXdMaxDsv : g_xd_rtv_used < kXdMaxRtv;
    if (!room || FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, rt.state, &cv,
                                                         IID_ID3D12Resource, (void**)&rt.res))) {
        printf("[XERT] RT alloc failed (%s base %u w %u fmt %u)\n", depth ? "depth" : "color", base, w, fmt); fflush(stdout);
        g_xd_rts[key] = XdRt{}; return nullptr;
    }
    if (depth) {
        rt.view = g_xd_dsv_used++;
        D3D12_DEPTH_STENCIL_VIEW_DESC dv = {}; dv.Format = xd_stencil() ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_xd_dsv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(rt.view) * g_xd_dsv_inc;
        g_device->CreateDepthStencilView(rt.res, &dv, h);
        g_cmd->ClearDepthStencilView(h, xd_stencil() ? (D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL) : D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    } else {
        rt.view = g_xd_rtv_used++;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_xd_rtv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(rt.view) * g_xd_rtv_inc;
        g_device->CreateRenderTargetView(rt.res, nullptr, h);
        const float z[4] = {0, 0, 0, 0};
        g_cmd->ClearRenderTargetView(h, z, 0, nullptr);
    }
    printf("[XERT] new %s RT base=%u %ux%u fmt=%u (total %zu)\n", depth ? "depth" : "color", base, w, kXdRtH, fmt, g_xd_rts.size() + 1); fflush(stdout);
    g_xd_rts[key] = rt;
    return &g_xd_rts[key];
}
static UINT g_xd_null_rtv = ~0u;   // null descriptor for unused MRT slots below the highest bound one
static D3D12_CPU_DESCRIPTOR_HANDLE xd_rtv(UINT i);
static D3D12_CPU_DESCRIPTOR_HANDLE xd_null_rtv() {
    if (g_xd_null_rtv == ~0u) {
        g_xd_null_rtv = g_xd_rtv_used++;
        D3D12_RENDER_TARGET_VIEW_DESC nd = {}; nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; nd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_device->CreateRenderTargetView(nullptr, &nd, xd_rtv(g_xd_null_rtv));
    }
    return xd_rtv(g_xd_null_rtv);
}
static D3D12_CPU_DESCRIPTOR_HANDLE xd_rtv(UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = g_xd_rtv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(i) * g_xd_rtv_inc; return h; }
static D3D12_CPU_DESCRIPTOR_HANDLE xd_dsv(UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = g_xd_dsv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(i) * g_xd_dsv_inc; return h; }
// RT of a draw / resolve: color (RB_COLOR_INFO) or depth (RB_DEPTH_INFO) in sample space.
static XdRt* xd_rt_color(uint32_t surf_info, uint32_t color_info) {
    UINT sx, sy; xd_msaa_scale(surf_info, sx, sy);
    return xd_rt_get(false, color_info & 0xFFF, (surf_info & 0x3FFF) * sx, xd_color_format((color_info >> 16) & 0xF));
}
static XdRt* xd_rt_depth(uint32_t surf_info, uint32_t depth_info) {
    UINT sx, sy; xd_msaa_scale(surf_info, sx, sy);
    return xd_rt_get(true, depth_info & 0xFFF, (surf_info & 0x3FFF) * sx, DXGI_FORMAT_D32_FLOAT);
}

// Resolve destinations: host texture per guest physical base, sampled instead of guest memory.
struct XdResolved { ID3D12Resource* res; D3D12_RESOURCE_STATES state; UINT w, h; DXGI_FORMAT fmt; UINT rtv; uint32_t tfmt; uint64_t frame;
                    uint64_t probe; uint32_t probe_bytes; bool composite; uint64_t composed_frame;
                    uint32_t vy0 = 0xFFFFFFFFu, vy1 = 0; };   // rows ever written by resolves
static std::unordered_map<uint32_t, XdResolved> g_xd_resolved;
// We never write resolve output back to guest memory, so the guest bytes under a resolve target
// only change when the CPU reuses that memory (e.g. a new level texture loaded there). Fingerprint
// them at resolve time; a changed fingerprint means the resolve target is stale.
extern uint8_t* g_base;
static uint64_t xd_guest_probe(uint32_t phys, uint32_t bytes) {
    if (!bytes) return 0;
    const uint8_t* p = g_base + 0x80000000ull;
    uint64_t h = 1469598103934665603ull;
    uint32_t head = std::min<uint32_t>(bytes, 4096) & ~7u;
    for (uint32_t o = 0; o < head; o += 8) {
        uint64_t v; memcpy(&v, p + ((uint64_t(phys) + o) & 0x1FFFFFF8ull), 8); h = (h ^ v) * 1099511628211ull;
    }
    for (uint32_t i = 0; i < 32; ++i) {
        uint64_t off = (uint64_t(bytes - 8) * i) / 31;
        uint64_t a = (uint64_t(phys) + off) & 0x1FFFFFF8ull;
        uint64_t v; memcpy(&v, p + a, 8); h = (h ^ v) * 1099511628211ull;
    }
    return h;
}
static bool xd_resolved_stale(const XdResolved& r, uint32_t dest) {
    return r.probe_bytes && xd_guest_probe(dest, r.probe_bytes) != r.probe;
}
static std::vector<ID3D12Resource*> g_xd_rt_garbage[kFrameCount];
// DIAG: compact per-frame replay state (resolves, composites, stale evictions, present source),
// printed by the blue-pixel catch (LSWTCS_BLUECATCH) for the bad frame and the one before it.
static std::string g_xd_fstate, g_xd_fstate_prev;
static void xd_fstate(const char* fmt, ...) {
    if (g_xd_fstate.size() > 6000) return;
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); g_xd_fstate += b;
}
static uint64_t g_xd_frame_no = 0;
static void xd_res_to(XdResolved& r, D3D12_RESOURCE_STATES s) {
    if (r.state != s) { barrier(r.res, r.state, s); r.state = s; }
}
// Destination host format: depth -> R32_FLOAT; formats the shader samples by channel count keep
// their layout; everything else keeps the source RT format (no precision loss).
static DXGI_FORMAT xd_resolve_format(bool depth_src, uint32_t tfmt, DXGI_FORMAT src) {
    if (depth_src) return DXGI_FORMAT_R32_FLOAT;
    switch (tfmt) {
        case 2: { static int k8 = -1; if (k8 < 0) { const char* e = getenv("LSWTCS_K8RGBA"); k8 = (e && e[0] == '0') ? 0 : 1; }
                  if (k8) return src; return DXGI_FORMAT_R8_UNORM; }   // k_8 (LSWTCS_K8RGBA=1: keep RGBA, experiment)
        case 10: return DXGI_FORMAT_R8G8_UNORM;      // k_8_8
        case 6: return DXGI_FORMAT_R8G8B8A8_UNORM;   // k_8_8_8_8
        default: return src;
    }
}
static XdResolved* xd_resolved_get(uint32_t dest, UINT w, UINT h, DXGI_FORMAT fmt, uint32_t tfmt) {
    auto it = g_xd_resolved.find(dest);
    if (it != g_xd_resolved.end()) {
        XdResolved& r = it->second;
        if (r.res && r.w >= w && r.h >= h && r.fmt == fmt) { r.tfmt = tfmt; return &r; }
        if (r.res) g_xd_rt_garbage[g_frame_idx].push_back(r.res);
        UINT rtv = r.rtv; g_xd_resolved.erase(it);
        (void)rtv;
    }
    if (g_xd_rtv_used >= kXdMaxRtv) return nullptr;
    XdResolved r{}; r.w = w; r.h = h; r.fmt = fmt; r.tfmt = tfmt;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Format = fmt; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    r.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, r.state, nullptr,
                                                 IID_ID3D12Resource, (void**)&r.res))) return nullptr;
    r.rtv = g_xd_rtv_used++;
    g_device->CreateRenderTargetView(r.res, nullptr, xd_rtv(r.rtv));
    const float z[4] = {0, 0, 0, 0};
    g_cmd->ClearRenderTargetView(xd_rtv(r.rtv), z, 0, nullptr);
    static int n = 0; if (++n <= 40) { printf("[XERT] resolve target 0x%08X %ux%u dxgi=%u tfmt=%u\n", dest, w, h, fmt, tfmt); fflush(stdout); }
    g_xd_resolved[dest] = r;
    return &g_xd_resolved[dest];
}

// Blit: copies src texels (with MSAA sample handling) into the bound RTV over a rectangle.
static ID3D12RootSignature* g_xd_blit_rs = nullptr;
static ID3DBlob* g_xd_blit_vs = nullptr; static ID3DBlob* g_xd_blit_ps = nullptr;
static std::unordered_map<uint32_t, ID3D12PipelineState*> g_xd_blit_psos;
static const char* kXdBlitHlsl =
    "cbuffer C : register(b0) { int4 g_off; int4 g_mode; };\n"
    "Texture2D<float4> g_src : register(t0);\n"
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 t = float2((id << 1) & 2, id & 2);\n"
    "  return float4(t * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    "  int2 s = int2(p.xy) + g_off.xy;\n"
    "  if (g_mode.x == 1) { int2 b = s * 2; return 0.25 * (g_src.Load(int3(b, 0)) + g_src.Load(int3(b + int2(1, 0), 0)) +\n"
    "                                       g_src.Load(int3(b + int2(0, 1), 0)) + g_src.Load(int3(b + int2(1, 1), 0))); }\n"
    "  if (g_mode.x == 2) { int2 b = int2(s.x, s.y * 2); return 0.5 * (g_src.Load(int3(b, 0)) + g_src.Load(int3(b + int2(0, 1), 0))); }\n"
    "  if (g_mode.x == 3) return g_src.Load(int3(s * g_mode.yz, 0));\n"
    "  return g_src.Load(int3(s, 0)); }\n";
static bool xd_blit_init() {
    if (g_xd_blit_rs) return true;
    ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kXdBlitHlsl, strlen(kXdBlitHlsl), "xd_blit", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &g_xd_blit_vs, &err)) ||
        FAILED(D3DCompile(kXdBlitHlsl, strlen(kXdBlitHlsl), "xd_blit", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &g_xd_blit_ps, &err))) {
        printf("[XERT] blit compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "?"); fflush(stdout); safe_rel(err); return false; }
    D3D12_ROOT_PARAMETER p[2] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.ShaderRegister = 0; p[0].Constants.Num32BitValues = 8;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE r = {}; r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r.NumDescriptors = 1;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &r; p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC d = {}; d.NumParameters = 2; d.pParameters = p;
    ID3DBlob* blob = nullptr;
    if (SUCCEEDED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
        g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_ID3D12RootSignature, (void**)&g_xd_blit_rs);
    safe_rel(blob); safe_rel(err);
    return g_xd_blit_rs != nullptr;
}
static ID3D12PipelineState* xd_blit_pso(DXGI_FORMAT fmt) {
    auto it = g_xd_blit_psos.find(fmt); if (it != g_xd_blit_psos.end()) return it->second;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_xd_blit_rs;
    pd.VS = { g_xd_blit_vs->GetBufferPointer(), g_xd_blit_vs->GetBufferSize() };
    pd.PS = { g_xd_blit_ps->GetBufferPointer(), g_xd_blit_ps->GetBufferSize() };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX; pd.NumRenderTargets = 1; pd.RTVFormats[0] = fmt; pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(g_device->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState, (void**)&pso))) {
        printf("[XERT] blit PSO failed for format %u\n", fmt); fflush(stdout); }
    g_xd_blit_psos[fmt] = pso;
    return pso;
}
// Draws src (SRV format srv_fmt) into dst_rtv over dst rect [x,y,w,h]; texel = dst pixel + off,
// mode 0 = 1:1, 1 = average 2x2 samples (4x MSAA), 2 = average 1x2 (2x MSAA), 3 = one sample (scale sx,sy).
static uint64_t g_xd_blit_count = 0;   // bumps whenever a blit rebinds the output merger
static void xd_blit(ID3D12Resource* src, DXGI_FORMAT srv_fmt, D3D12_CPU_DESCRIPTOR_HANDLE dst_rtv, DXGI_FORMAT dst_fmt,
                    int32_t x, int32_t y, int32_t w, int32_t h, int32_t offx, int32_t offy, int32_t mode, int32_t sx, int32_t sy) {
    if (w <= 0 || h <= 0 || !xd_blit_init()) return;
    ID3D12PipelineState* pso = xd_blit_pso(dst_fmt); if (!pso) return;
    if (g_xd_desc_used + 1 > kXdDescPerFrame) return;
    UINT slot = g_xd_desc_base + g_xd_desc_used++;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = srv_fmt; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE hc = g_xd_srv_heap->GetCPUDescriptorHandleForHeapStart(); hc.ptr += SIZE_T(slot) * g_xd_srv_inc;
    g_device->CreateShaderResourceView(src, &sv, hc);
    D3D12_GPU_DESCRIPTOR_HANDLE hg = g_xd_srv_heap->GetGPUDescriptorHandleForHeapStart(); hg.ptr += UINT64(slot) * g_xd_srv_inc;
    ++g_xd_blit_count;
    g_cmd->OMSetRenderTargets(1, &dst_rtv, FALSE, nullptr);
    g_cmd->SetGraphicsRootSignature(g_xd_blit_rs);
    g_cmd->SetPipelineState(pso);
    int32_t c[8] = { offx, offy, 0, 0, mode, sx, sy, 0 };
    g_cmd->SetGraphicsRoot32BitConstants(0, 8, c, 0);
    g_cmd->SetGraphicsRootDescriptorTable(1, hg);
    D3D12_VIEWPORT vp = { float(x), float(y), float(w), float(h), 0, 1 };
    D3D12_RECT sc = { x, y, x + w, y + h };
    g_cmd->RSSetViewports(1, &vp); g_cmd->RSSetScissorRects(1, &sc);
    g_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cmd->DrawInstanced(3, 1, 0, 0);
}
static float xd_unorm(uint32_t v, uint32_t bits) { return float(v & ((1u << bits) - 1)) / float((1u << bits) - 1); }
// One recorded resolve: copy the source rectangle into the destination texture, then clears.
static uint32_t g_xd_resolves_done = 0;
static uint64_t g_xd_resolve_seq = 0;   // bumps on every resolve write (views refresh on change)
static void xd_do_resolve(const LswGpuDrawRec& r) {
    UINT sx, sy; xd_msaa_scale(r.surf_info, sx, sy);
    bool depth_src = r.rs_src >= 4;
    int32_t x0 = r.rs_rect[0], y0 = r.rs_rect[1], x1 = r.rs_rect[2], y1 = r.rs_rect[3];
    if (r.rs_copy_cmd <= 1 && x1 > x0 && y1 > y0 && r.rs_dest) {
        XdRt* src = depth_src ? xd_rt_depth(r.surf_info, r.depth_info) : xd_rt_color(r.surf_info, r.color_info);
        if (src) {
            int32_t w = x1 - x0, h = y1 - y0;
            UINT dw = std::max<UINT>(r.rs_dest_pitch, UINT(r.rs_dest_x + w)), dh = std::max<UINT>(r.rs_dest_height, UINT(r.rs_dest_y + h));
            dw = std::min<UINT>(dw, 8192); dh = std::min<UINT>(dh, 8192);
            DXGI_FORMAT dfmt = xd_resolve_format(depth_src, r.rs_dest_fmt, src->fmt);
            XdResolved* dst = xd_resolved_get(r.rs_dest, dw, dh, dfmt, depth_src ? 22u : r.rs_dest_fmt);
            if (dst) {
                xd_rt_to(*src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                xd_res_to(*dst, D3D12_RESOURCE_STATE_RENDER_TARGET);
                int32_t mode = 0;
                if (sx > 1 || sy > 1) {
                    uint32_t ss = r.rs_sample;
                    if (!depth_src && sx == 2 && ss == 6) mode = 1;          // k0123 average
                    else if (!depth_src && sx == 1 && ss == 4) mode = 2;     // k01 average (2x)
                    else mode = 3;                                            // single sample
                }
                xd_blit(src->res, depth_src ? (xd_stencil() ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT) : src->fmt, xd_rtv(dst->rtv), dst->fmt,
                        r.rs_dest_x, r.rs_dest_y, w, h, x0 - r.rs_dest_x, y0 - r.rs_dest_y, mode, int32_t(sx), int32_t(sy));
                dst->frame = ++g_xd_resolve_seq;
                dst->vy0 = std::min<uint32_t>(dst->vy0, uint32_t(r.rs_dest_y));
                dst->vy1 = std::max<uint32_t>(dst->vy1, uint32_t(r.rs_dest_y + h));
                dst->probe_bytes = std::max<uint32_t>(64u, std::min<uint32_t>(dw * dh * 4u, 0x1000000u));
                dst->probe = xd_guest_probe(r.rs_dest, dst->probe_bytes);
                ++g_xd_resolves_done;
            }
        }
    }
    D3D12_RECT cr = { LONG(x0 * sx), LONG(y0 * sy), LONG(x1 * sx), LONG(y1 * sy) };
    if (cr.right <= cr.left || cr.bottom <= cr.top) return;
    if (r.rs_clear_depth) {
        if (XdRt* d = xd_rt_depth(r.surf_info, r.depth_info)) {
            xd_rt_to(*d, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            uint32_t v = (r.rs_depth_clear >> 8) & 0xFFFFFF;
            float z = xd_unorm(v, 24);
            if ((r.depth_info >> 16) & 1) {   // kD24FS8: 20e4 float in [0, 2), host stores half
                uint32_t e = v >> 20, m = v & 0xFFFFF; float f;
                if (e == 0) f = float(m) * (1.0f / 1048576.0f) * (1.0f / 16384.0f);
                else f = std::ldexp(1.0f + float(m) / 1048576.0f, int(e) - 15);
                z = f * 0.5f;
            }
            g_cmd->ClearDepthStencilView(xd_dsv(d->view), xd_stencil() ? (D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL) : D3D12_CLEAR_FLAG_DEPTH,
                                         z, UINT8(r.rs_depth_clear & 0xFF), 1, &cr);
        }
    }
    if (r.rs_clear_color) {
        if (XdRt* c = xd_rt_color(r.surf_info, r.color_info)) {
            xd_rt_to(*c, D3D12_RESOURCE_STATE_RENDER_TARGET);
            uint32_t v = r.rs_color_clear; float cc[4];
            switch ((r.color_info >> 16) & 0xF) {
                case 2: case 10: case 3: case 12:
                    for (int j = 0; j < 3; ++j) cc[j] = xd_unorm(v >> (j * 10), 10);
                    cc[3] = xd_unorm(v >> 30, 2); break;
                default:
                    for (int j = 0; j < 4; ++j) cc[j] = xd_unorm(v >> (j * 8), 8); break;
            }
            g_cmd->ClearRenderTargetView(xd_rtv(c->view), cc, 1, &cr);
        }
    }
}

// ── LSWTCS_RESDUMP=<replayed frame #> (diagnostic): every resolve target -> shots/res_<addr>.png ──
struct XdDump { ID3D12Resource* rb; UINT w, h, pitch; DXGI_FORMAT fmt; uint32_t dest; };
static std::vector<XdDump> g_xd_dumps;
static UINT xd_dump_bpp(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8_UNORM: return 1;
        case DXGI_FORMAT_R8G8_UNORM: return 2;
        case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
        default: return 0;
    }
}
static bool xd_trigger(const char* name) {   // file in the working dir requests a one-shot capture
    if ((g_xd_frame_no % 30) != 0) return false;
    if (GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES) return false;
    DeleteFileA(name);
    return true;
}
static void xd_dump_queue() {
    static long at = -2; if (at == -2) { const char* e = getenv("LSWTCS_RESDUMP"); at = e ? atol(e) : -1; }
    bool trig = xd_trigger("resdump_now");
    if (!trig && (at < 0 || g_xd_frame_no != uint64_t(at))) return;
    for (auto& kv : g_xd_resolved) {
        XdResolved& r = kv.second; UINT bpp = xd_dump_bpp(r.fmt);
        if (!r.res || !bpp) continue;
        XdDump d{}; d.w = r.w; d.h = r.h; d.fmt = r.fmt; d.dest = kv.first; d.pitch = (r.w * bpp + 255) & ~255u;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = UINT64(d.pitch) * d.h;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_ID3D12Resource, (void**)&d.rb))) continue;
        xd_res_to(r, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = d.rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { r.fmt, r.w, r.h, 1, d.pitch };
        src.pResource = r.res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        g_xd_dumps.push_back(d);
    }
    for (auto& kv : g_xd_rts) {
        XdRt& t = kv.second; UINT bpp = xd_dump_bpp(t.fmt);
        if (!t.res || t.depth || !bpp) continue;
        XdDump d{}; d.w = t.w; d.h = 720; d.fmt = t.fmt; d.dest = 0xEE000000u | uint32_t((kv.first >> 40) & 0xFFF) << 12 | t.w; d.pitch = (t.w * bpp + 255) & ~255u;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = UINT64(d.pitch) * d.h;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_ID3D12Resource, (void**)&d.rb))) continue;
        xd_rt_to(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = d.rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { t.fmt, t.w, 720, 1, d.pitch };
        src.pResource = t.res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box = { 0, 0, 0, t.w, 720, 1 };
        g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        g_xd_dumps.push_back(d);
    }
    printf("[RESDUMP] queued %zu resolve targets at replayed frame %llu\n", g_xd_dumps.size(), (unsigned long long)g_xd_frame_no); fflush(stdout);
}
static bool gpu_xedraw_dump_pending() { return !g_xd_dumps.empty(); }
// Call after the frame's fence completed.
static void gpu_xedraw_dump_write() {
    _mkdir("shots");
    for (XdDump& d : g_xd_dumps) {
        uint8_t* m = nullptr; D3D12_RANGE rr = { 0, SIZE_T(d.pitch) * d.h };
        if (SUCCEEDED(d.rb->Map(0, &rr, (void**)&m)) && m) {
            std::vector<uint8_t> bgra(size_t(d.w) * d.h * 4);
            float zmin = 1e30f, zmax = -1e30f;
            if (d.fmt == DXGI_FORMAT_R32_FLOAT)
                for (UINT y = 0; y < d.h; ++y) for (UINT x = 0; x < d.w; ++x) {
                    float v; memcpy(&v, m + size_t(y) * d.pitch + x * 4, 4); zmin = std::min(zmin, v); zmax = std::max(zmax, v); }
            for (UINT y = 0; y < d.h; ++y) for (UINT x = 0; x < d.w; ++x) {
                const uint8_t* p = m + size_t(y) * d.pitch; uint8_t* o = &bgra[(size_t(y) * d.w + x) * 4];
                uint8_t r = 0, g = 0, b = 0;
                switch (d.fmt) {
                    case DXGI_FORMAT_R8_UNORM: r = g = b = p[x]; break;
                    case DXGI_FORMAT_R8G8_UNORM: r = p[x * 2]; g = p[x * 2 + 1]; break;
                    case DXGI_FORMAT_R8G8B8A8_UNORM: r = p[x * 4]; g = p[x * 4 + 1]; b = p[x * 4 + 2]; break;
                    case DXGI_FORMAT_R10G10B10A2_UNORM: { uint32_t v; memcpy(&v, p + x * 4, 4); r = uint8_t((v & 0x3FF) >> 2); g = uint8_t(((v >> 10) & 0x3FF) >> 2); b = uint8_t(((v >> 20) & 0x3FF) >> 2); } break;
                    case DXGI_FORMAT_R32_FLOAT: { float v; memcpy(&v, p + x * 4, 4); float t = zmax > zmin ? (v - zmin) / (zmax - zmin) : 0; r = g = b = uint8_t(std::clamp(t, 0.0f, 1.0f) * 255); } break;
                    default: break;
                }
                o[0] = b; o[1] = g; o[2] = r; o[3] = 255;
            }
            char path[96]; snprintf(path, sizeof path, "shots/res_%08X_%ux%u.png", d.dest, d.w, d.h);
            lswtcs_write_png(path, bgra.data(), int(d.w), int(d.h), int(d.w * 4));
            printf("[RESDUMP] %s fmt=%u%s\n", path, d.fmt, d.fmt == DXGI_FORMAT_R32_FLOAT ? " (depth, normalized)" : ""); fflush(stdout);
            if (d.fmt == DXGI_FORMAT_R32_FLOAT) { printf("[RESDUMP]   depth range %.6f .. %.6f\n", zmin, zmax); fflush(stdout); }
            D3D12_RANGE wn = {}; d.rb->Unmap(0, &wn);
        }
        d.rb->Release();
    }
    g_xd_dumps.clear();
}

// ── Sub-region resolves (predicated tiling) ──────────────────────────────────────────────────
// Each tile is resolved to (texture base + tile_row * row_bytes), i.e. into a sub-range of the
// texture the composite later samples at its base. When a texture is bound at base P with no exact
// resolve target, compose every resolve target whose base lies inside P's (tiled 2D) memory range
// into a target for P at row offset (base - P) / row_bytes. Refreshed once per replayed frame.
static uint32_t xd_tfmt_bpp(uint32_t tfmt) {
    switch (tfmt) { case 2: return 1; case 3: case 4: case 5: case 10: case 24: case 26: return 2; default: return 4; }
}
static XdResolved* xd_resolved_compose(uint32_t base, uint32_t tfmt, uint32_t w, uint32_t h) {
    static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_RESCOMPOSE"); on = (e && e[0] == '0') ? 0 : 1; }
    if (!on || !w || !h) return nullptr;
    uint32_t bpp = xd_tfmt_bpp(tfmt), pitch = (w + 31) & ~31u, rows = (h + 31) & ~31u;
    uint64_t row_bytes = uint64_t(pitch) * bpp, end = uint64_t(base) + row_bytes * rows;
    std::vector<std::pair<uint32_t, XdResolved*>> subs;
    for (auto& kv : g_xd_resolved)
        if (kv.first > base && kv.first < end && kv.second.res && !kv.second.composite && ((kv.first - base) % row_bytes) == 0)
            subs.push_back({kv.first, &kv.second});
    if (subs.empty()) return nullptr;
    DXGI_FORMAT fmt = subs[0].second->fmt;
    XdResolved* c = xd_resolved_get(base, w, h, fmt, tfmt);
    if (!c) return nullptr;
    c->composite = true; c->probe_bytes = 0;
    if (c->composed_frame == g_xd_frame_no) return c;
    c->composed_frame = g_xd_frame_no;
    for (auto& sp : subs) {
        XdResolved& s = *sp.second;
        if (s.fmt != c->fmt) continue;
        uint32_t yoff = uint32_t((sp.first - base) / row_bytes);
        if (yoff >= c->h) continue;
        xd_res_to(s, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        xd_res_to(*c, D3D12_RESOURCE_STATE_RENDER_TARGET);
        UINT cw = std::min(s.w, c->w), ch = std::min(s.h, c->h - yoff);
        xd_blit(s.res, s.fmt, xd_rtv(c->rtv), c->fmt, 0, int32_t(yoff), int32_t(cw), int32_t(ch), 0, -int32_t(yoff), 0, 1, 1);
        static int nl = 0; if (++nl <= 12) { printf("[XERT] compose 0x%08X <- 0x%08X at row %u (%ux%u)\n", base, sp.first, yoff, cw, ch); fflush(stdout); }
    }
    return c;
}

// ── Sized views (LSWTCS_RESVIEW, default on) ─────────────────────────────────────────────────
// Resolve targets are sized by the destination's memory pitch/height (e.g. 768x704 for a 640x704
// mask), but shaders sample with normalized coordinates against the size in the fetch constant.
// Bind a copy of exactly the fetch size (top-left region), refreshed when the source changes.
struct XdView { ID3D12Resource* res; D3D12_RESOURCE_STATES state; UINT w, h; DXGI_FORMAT fmt; UINT rtv; uint64_t src_frame; uint64_t made_frame; };
static std::unordered_map<uint64_t, XdView> g_xd_views;
static XdView* xd_resolved_view(uint32_t base, XdResolved& src, uint32_t w, uint32_t h) {
    static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_RESVIEW"); on = (e && e[0] == '0') ? 0 : 1; }
    if (!on || !w || !h || (w == src.w && h == src.h) || w > 8192 || h > 8192) return nullptr;
    uint64_t key = (uint64_t(base) << 32) ^ (uint64_t(w) << 16) ^ uint64_t(h) ^ (uint64_t(src.fmt) << 56);
    XdView& v = g_xd_views[key];
    if (!v.res) {
        if (g_xd_rtv_used >= kXdMaxRtv) return nullptr;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Format = src.fmt; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        v.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, v.state, nullptr,
                                                     IID_ID3D12Resource, (void**)&v.res))) { v = XdView{}; return nullptr; }
        v.w = w; v.h = h; v.fmt = src.fmt; v.rtv = g_xd_rtv_used++;
        g_device->CreateRenderTargetView(v.res, nullptr, xd_rtv(v.rtv));
        const float z[4] = {0, 0, 0, 0};
        g_cmd->ClearRenderTargetView(xd_rtv(v.rtv), z, 0, nullptr);
        static int nl = 0; if (++nl <= 20) { printf("[XERT] view 0x%08X %ux%u of %ux%u\n", base, w, h, src.w, src.h); fflush(stdout); }
    }
    if (v.src_frame != src.frame || (src.composite && v.made_frame != g_xd_frame_no)) {   // source rewritten since last copy
        v.made_frame = g_xd_frame_no; v.src_frame = src.frame;
        xd_res_to(src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (v.state != D3D12_RESOURCE_STATE_RENDER_TARGET) { barrier(v.res, v.state, D3D12_RESOURCE_STATE_RENDER_TARGET); v.state = D3D12_RESOURCE_STATE_RENDER_TARGET; }
        xd_blit(src.res, src.fmt, xd_rtv(v.rtv), v.fmt, 0, 0, int32_t(std::min(w, src.w)), int32_t(std::min(h, src.h)), 0, 0, 0, 1, 1);
    }
    return &v;
}

// ── Texture source for a fetch constant that points at resolved data ──────────────────────────
// Predicated tiling resolves each tile into a sub-range of the texture the composite samples:
// tile k goes to (base + tile_row_k * row_bytes) with its rows at their EDRAM y. Build a texture of
// exactly the fetch size from the base-address target (row 0) plus every sub-range target (row
// (sub - base) / row_bytes), rebuilt whenever any source is re-resolved. No sub-ranges: the base
// target itself, or a sized view when the fetch size differs.
struct XdTexSrc { ID3D12Resource* res; DXGI_FORMAT fmt; D3D12_RESOURCE_STATES* state; };
static std::unordered_map<uint32_t, XdResolved> g_xd_composed;
static uint64_t g_xd_src_epoch = 0;   // bumps when xd_texture_source drops or replaces a target (memo below)
static bool xd_texture_source_uncached(uint32_t base, uint32_t tfmt, uint32_t w, uint32_t h, XdTexSrc& out);
// LSWTCS_BINDCACHE (default on): per-replay memo of texture sources and per-frame dedup of SRV and
// sampler tables. 0 = old per-draw path (A/B).
static bool xd_bindcache() { static int v = -1; if (v < 0) { const char* e = getenv("LSWTCS_BINDCACHE"); v = (e && e[0] == '0') ? 0 : 1; } return v == 1; }
// The source for (base, format, size) only changes when a resolve writes a target or a target is
// dropped/replaced, so within one replay it is looked up once per resolve epoch instead of twice per
// draw (prepass + bind), each of which re-probed ~4 KB of guest memory and scanned every target.
struct XdSrcMemo { bool ok; XdTexSrc src; };
static std::unordered_map<uint64_t, XdSrcMemo> g_xd_src_memo;
static uint64_t g_xd_src_memo_key[3] = {~0ull, ~0ull, ~0ull};   // replay seq, resolve seq, src epoch
static bool xd_texture_source(uint32_t base, uint32_t tfmt, uint32_t w, uint32_t h, XdTexSrc& out) {
    if (!xd_bindcache()) { GpScope gp(GP_RP_TEXSRC); return xd_texture_source_uncached(base, tfmt, w, h, out); }
    if (g_xd_src_memo_key[0] != g_xd_replay_seq || g_xd_src_memo_key[1] != g_xd_resolve_seq || g_xd_src_memo_key[2] != g_xd_src_epoch) {
        g_xd_src_memo.clear();
        g_xd_src_memo_key[0] = g_xd_replay_seq; g_xd_src_memo_key[1] = g_xd_resolve_seq; g_xd_src_memo_key[2] = g_xd_src_epoch;
    }
    const uint64_t key = uint64_t(base & 0x1FFFFFFFu) | (uint64_t(tfmt & 63) << 29) | (uint64_t((w - 1) & 0x3FFF) << 35) | (uint64_t((h - 1) & 0x3FFF) << 49);
    auto it = g_xd_src_memo.find(key);
    if (it != g_xd_src_memo.end()) { if (it->second.ok) out = it->second.src; return it->second.ok; }
    const uint64_t epoch = g_xd_src_epoch;
    XdTexSrc s{};
    bool ok; { GpScope gp(GP_RP_TEXSRC); ok = xd_texture_source_uncached(base, tfmt, w, h, s); }
    if (g_xd_src_epoch != epoch) {   // this call dropped/replaced a target: other entries may dangle
        g_xd_src_memo.clear(); g_xd_src_memo_key[2] = g_xd_src_epoch;
    }
    g_xd_src_memo.emplace(key, XdSrcMemo{ok, s});
    if (ok) out = s;
    return ok;
}
static bool xd_texture_source_uncached(uint32_t base, uint32_t tfmt, uint32_t w, uint32_t h, XdTexSrc& out) {
    auto ex = g_xd_resolved.find(base);
    if (ex != g_xd_resolved.end() && (!ex->second.res || ex->second.composite)) ex = g_xd_resolved.end();
    if (ex != g_xd_resolved.end() && xd_resolved_stale(ex->second, base)) {
        static int ns = 0; if (++ns <= 200) { printf("[STALE] frame %llu: resolved 0x%08X %ux%u dropped (guest memory changed since resolve #%llu)\n",
            (unsigned long long)g_xd_frame_no, base, ex->second.w, ex->second.h, (unsigned long long)ex->second.frame); fflush(stdout); }
        xd_fstate(" STALE:%X", base);
        g_xd_rt_garbage[g_frame_idx].push_back(ex->second.res); g_xd_resolved.erase(ex); ex = g_xd_resolved.end(); ++g_xd_src_epoch;
    }
    static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_RESCOMPOSE"); on = (e && e[0] == '0') ? 0 : 1; }
    std::vector<std::pair<uint32_t, XdResolved*>> subs;
    uint64_t row_bytes = 0;
    if (on && w && h) {
        uint32_t bpp = xd_tfmt_bpp(tfmt), pitch = (w + 31) & ~31u, rows = (h + 31) & ~31u;
        row_bytes = uint64_t(pitch) * bpp;
        uint64_t end = uint64_t(base) + row_bytes * rows;
        for (auto& kv : g_xd_resolved)
            if (kv.first > base && kv.first < end && kv.second.res && !kv.second.composite && ((kv.first - base) % row_bytes) == 0)
                subs.push_back({kv.first, &kv.second});
    }
    if (subs.empty()) {
        if (ex == g_xd_resolved.end()) { if (w >= 256) xd_fstate(" MISS:%X(%ux%u)", base, w, h); return false; }
        XdResolved& rr = ex->second;
        if (XdView* vw = xd_resolved_view(base, rr, w, h)) { out = {vw->res, vw->fmt, &vw->state}; return true; }
        out = {rr.res, rr.fmt, &rr.state}; return true;
    }
    DXGI_FORMAT fmt = ex != g_xd_resolved.end() ? ex->second.fmt : subs[0].second->fmt;
    uint64_t seq = ex != g_xd_resolved.end() ? ex->second.frame : 0;
    for (auto& sp : subs) seq = std::max<uint64_t>(seq, sp.second->frame);
    XdResolved& c = g_xd_composed[base];
    if (c.res && (c.w != w || c.h != h || c.fmt != fmt)) { g_xd_rt_garbage[g_frame_idx].push_back(c.res); c = XdResolved{}; ++g_xd_src_epoch; }
    if (!c.res) {
        if (g_xd_rtv_used >= kXdMaxRtv) return false;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Format = fmt; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        c.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, c.state, nullptr,
                                                     IID_ID3D12Resource, (void**)&c.res))) { c = XdResolved{}; return false; }
        c.w = w; c.h = h; c.fmt = fmt; c.rtv = g_xd_rtv_used++; c.frame = ~0ull;
        g_device->CreateRenderTargetView(c.res, nullptr, xd_rtv(c.rtv));
        const float z[4] = {0, 0, 0, 0};
        g_cmd->ClearRenderTargetView(xd_rtv(c.rtv), z, 0, nullptr);
        static int nl = 0; if (++nl <= 20) { printf("[XERT] composite 0x%08X %ux%u from %zu sub-ranges%s\n", base, w, h, subs.size(), ex != g_xd_resolved.end() ? " + base" : ""); fflush(stdout); }
    }
    if (c.frame != seq) {
        c.frame = seq;
        xd_fstate(" C:%X(%ux%u,%zu subs%s)", base, w, h, subs.size(), ex != g_xd_resolved.end() ? "+base" : "");
        xd_res_to(c, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (ex != g_xd_resolved.end() && ex->second.fmt == c.fmt) {
            XdResolved& s = ex->second;
            xd_res_to(s, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            uint32_t y0 = s.vy0 == 0xFFFFFFFFu ? 0 : s.vy0, y1 = std::min(s.vy1 ? s.vy1 : s.h, std::min(s.h, c.h));
            if (y1 > y0) xd_blit(s.res, s.fmt, xd_rtv(c.rtv), c.fmt, 0, int32_t(y0), int32_t(std::min(s.w, c.w)), int32_t(y1 - y0), 0, 0, 0, 1, 1);
        }
        for (auto& sp : subs) {
            XdResolved& s = *sp.second;
            if (s.fmt != c.fmt) continue;
            uint32_t yoff = uint32_t((sp.first - base) / row_bytes);
            if (yoff >= c.h) continue;
            xd_res_to(s, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Sub-range rows live at their EDRAM y inside the sub target: copy only the rows its resolves
            // wrote ([vy0, vy1)) to yoff + row, so unwritten rows don't cover the previous tile's seam rows.
            uint32_t y0 = s.vy0 == 0xFFFFFFFFu ? 0 : s.vy0, y1 = s.vy1 ? s.vy1 : s.h;
            y1 = std::min<uint32_t>(y1, std::min<uint32_t>(s.h, c.h - yoff));
            if (y1 <= y0) continue;
            UINT cw = std::min(s.w, c.w);
            xd_blit(s.res, s.fmt, xd_rtv(c.rtv), c.fmt, 0, int32_t(yoff + y0), int32_t(cw), int32_t(y1 - y0), 0, -int32_t(yoff), 0, 1, 1);
        }
    }
    out = {c.res, c.fmt, &c.state};
    return true;
}

// LSWTCS_BINDCACHE: last texture / sampler table bound per stage (0 = PS, 1 = VS) in this replay.
// A draw whose table inputs equal the previous draw's (same fetch words and dims, and for textures no
// resolve, blit or target change since, so no source or resource state moved) rebinds that table.
struct XdTexLast { bool valid; uint32_t count; uint64_t resolve_seq, src_epoch, blits; D3D12_GPU_DESCRIPTOR_HANDLE g;
                   uint8_t dims[32]; uint32_t fw[32 * 6]; };
struct XdSmpLast { bool valid; uint32_t count; D3D12_GPU_DESCRIPTOR_HANDLE g; uint32_t fw[32 * 6]; LswGpuSamplerBinding b[32]; };
static bool xd_tex_same(const XdTexLast& L, const uint32_t* fa, uint32_t count, const uint8_t* dims, const uint8_t* fetches) {
    if (count > 32) count = 32;
    if (!L.valid || L.count != count || L.resolve_seq != g_xd_resolve_seq || L.src_epoch != g_xd_src_epoch || L.blits != g_xd_blit_count) return false;
    for (uint32_t t = 0; t < count; ++t)
        if (L.dims[t] != dims[t] || memcmp(&L.fw[t * 6], fa + (fetches[t] & 31) * 6, 24) != 0) return false;
    return true;
}
static void xd_tex_remember(XdTexLast& L, const uint32_t* fa, uint32_t count, const uint8_t* dims, const uint8_t* fetches, D3D12_GPU_DESCRIPTOR_HANDLE g) {
    if (count > 32) count = 32;
    L.valid = true; L.count = count; L.resolve_seq = g_xd_resolve_seq; L.src_epoch = g_xd_src_epoch; L.blits = g_xd_blit_count; L.g = g;
    for (uint32_t t = 0; t < count; ++t) { L.dims[t] = dims[t]; memcpy(&L.fw[t * 6], fa + (fetches[t] & 31) * 6, 24); }
}
static bool xd_smp_same(const XdSmpLast& L, const uint32_t* fa, uint32_t count, const LswGpuSamplerBinding* b) {
    if (!L.valid || L.count != count) return false;
    for (uint32_t t = 0; t < count; ++t)
        if (memcmp(&L.b[t], &b[t], sizeof b[t]) != 0 || memcmp(&L.fw[t * 6], fa + (b[t].fetch_constant & 31) * 6, 24) != 0) return false;
    return true;
}
static void xd_smp_remember(XdSmpLast& L, const uint32_t* fa, uint32_t count, const LswGpuSamplerBinding* b, D3D12_GPU_DESCRIPTOR_HANDLE g) {
    L.valid = true; L.count = count; L.g = g;
    for (uint32_t t = 0; t < count; ++t) { L.b[t] = b[t]; memcpy(&L.fw[t * 6], fa + (b[t].fetch_constant & 31) * 6, 24); }
}

// Records the published guest frame's draws into the open command list (RT + DSV bound).
static void gpu_xedraw_replay_impl(D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv, uint32_t fb_phys);
static void gpu_xedraw_replay(D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv, uint32_t fb_phys) { gpu_xedraw_replay_impl(bb_rtv, fb_phys); }
static void gpu_xedraw_replay_impl(D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv, uint32_t fb_phys) {
    if (!g_xd_inited) xd_init();
    if (!g_xd_ok) return;
    LswGpuFrame& f = g_gpu_frame_render;   // taken by gpu_frame_take(); replayed again if no newer frame
    if (f.draws.empty()) return;
    uint64_t need = f.arena.size() + 256;
    UINT fi = g_frame_idx;
    if (g_xd_up_cap[fi] < need) {
        safe_rel(g_xd_up[fi]);
        uint64_t cap = 64ull << 20; while (cap < need) cap <<= 1;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = cap;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                     nullptr, IID_ID3D12Resource, (void**)&g_xd_up[fi]))) { g_xd_up_cap[fi] = 0; return; }
        D3D12_RANGE none = {}; g_xd_up[fi]->Map(0, &none, (void**)&g_xd_up_ptr[fi]);
        g_xd_up_cap[fi] = cap;
    }
    { GpScope gp(GP_RP_ARENA); memcpy(g_xd_up_ptr[fi], f.arena.data(), f.arena.size()); }
    for (ID3D12Resource* g : g_xd_tex_garbage[fi]) g->Release();   // slot fi's previous GPU work is complete
    g_xd_tex_garbage[fi].clear();
    for (ID3D12Resource* g : g_xd_rt_garbage[fi]) g->Release();
    g_xd_rt_garbage[fi].clear();
    ++g_xd_frame_no; ++g_xd_replay_seq;
    const bool gpurt = xd_gpurt();
    g_xd_pso_dsv = (gpurt && xd_stencil()) ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    D3D12_GPU_VIRTUAL_ADDRESS up = g_xd_up[fi]->GetGPUVirtualAddress();

    ID3D12DescriptorHeap* heaps[2] = { g_xd_srv_heap, g_xd_smp_heap };
    g_cmd->SetDescriptorHeaps(2, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE shm_table = g_xd_srv_heap->GetGPUDescriptorHandleForHeapStart();
    g_xd_desc_base = 2 + fi * kXdDescPerFrame; g_xd_desc_used = 0;
    g_xd_srv_tables.clear();
    const bool bindcache = xd_bindcache();
    // Command-list state last set by a draw (skip redundant sets); invalid after any blit.
    ID3D12RootSignature* st_rs = nullptr; ID3D12PipelineState* st_pso = nullptr; int st_topo = -1;
    D3D12_VIEWPORT st_vp = {}; D3D12_RECT st_sc = {}; bool st_vp_valid = false; uint64_t st_blits = g_xd_blit_count;
    XdTexLast tl[2] = {}; XdSmpLast sl[2] = {};   // last texture / sampler table per stage (0 = PS, 1 = VS)
    uint32_t smp_overflow = 0;
    uint64_t bound_key = ~0ull;   // (color RT, depth RT) currently bound
    uint32_t resolves = 0, rt_draws = 0, res_tex_binds = 0;
    static long xdlog_at = -2; if (xdlog_at == -2) { const char* e = getenv("LSWTCS_XDLOG"); xdlog_at = e ? atol(e) : -1; }
    static uint64_t xdlog_until = 0;
    if (xd_trigger("xdlog_now")) xdlog_until = g_xd_frame_no + 1;
    // DIAG toggle: file "skipdt" in the working dir cycles the deferred shadow-mask pass (half-res
    // 640-pitch target, samples the scene depth + shadow-map depth) through
    // drawn -> forced WHITE -> forced BLACK -> skipped (stale). WHITE/BLACK clear the mask RT inside
    // the draw's viewport instead of drawing it. Initial mode from LSWTCS_DTMODE (0..3). Logged when cycled.
    static int dt_mode = -1;
    if (dt_mode < 0) { const char* e = getenv("LSWTCS_DTMODE"); dt_mode = e ? (atoi(e) & 3) : 0; }
    if (xd_trigger("skipdt")) {
        dt_mode = (dt_mode + 1) & 3;
        static const char* const kDtNames[4] = {"drawn", "forced WHITE", "forced BLACK", "SKIPPED (stale)"};
        printf("[SKIPDT] shadow-mask pass %s\n", kDtNames[dt_mode]); fflush(stdout);
    }
    const bool xdlog = (xdlog_at >= 0 && g_xd_frame_no >= uint64_t(xdlog_at) && g_xd_frame_no < uint64_t(xdlog_at) + 2) || g_xd_frame_no < xdlog_until;
    uint32_t draw_i = 0;
    if (xdlog) printf("[XDLOG] ==== replayed frame %llu: %zu records, swap fb 0x%08X ====\n", (unsigned long long)g_xd_frame_no, f.draws.size(), f.swap_fb);
    auto shm_to = [&](D3D12_RESOURCE_STATES s) {
        if (g_xd_shm_state != s) { barrier(g_xd_shm, g_xd_shm_state, s); g_xd_shm_state = s; } };
    const D3D12_RESOURCE_STATES kShmRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    uint32_t drawn = 0, skipped = 0, copies = 0;
    auto upload = [&](const LswGpuRange& rg) {
        if (rg.arena_off == kLswRangeResident) return;
        shm_to(D3D12_RESOURCE_STATE_COPY_DEST);
        g_cmd->CopyBufferRegion(g_xd_shm, rg.phys, g_xd_up[fi], rg.arena_off, rg.size);
        ++copies;
    };
    for (const LswGpuRange& rg : f.prelude) upload(rg);   // uploads of frames replaced before replay
    g_xd_fstate_prev.swap(g_xd_fstate); g_xd_fstate.clear();
    xd_fstate("frame %llu:", (unsigned long long)g_xd_frame_no);
    GpScope* gp_diag = new GpScope(GP_RP_DIAG);
    // DIAG (always on, rate-limited): frame-structure fingerprint = draw count + every resolve
    // (dest, rect, size) + distinct viewports; logged to stdout whenever it differs from the previous
    // frame, to catch the periodic broken frames.
    {
        static uint64_t prev_sig = 0; static std::string prev_desc; static uint32_t nfs = 0;
        uint64_t sig = 1469598103934665603ull; std::string desc; uint32_t nd = 0;
        auto mix = [&](uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
        float lvp[4] = {-1, -1, -1, -1};
        for (const LswGpuDrawRec& q : f.draws) {
            if (q.kind == 1) {
                mix(q.rs_dest == (f.swap_fb & 0x1FFFFFFFu) ? 0xFBu : q.rs_dest); mix(q.rs_src); for (int k = 0; k < 4; ++k) mix(uint32_t(q.rs_rect[k]));
                mix(q.rs_dest_pitch); mix(q.rs_dest_height); mix(q.surf_info);
                char b[96]; snprintf(b, sizeof b, " R%u:%X[%d,%d..%d,%d]%ux%u", q.rs_src, q.rs_dest == (f.swap_fb & 0x1FFFFFFFu) ? 0xFBu : q.rs_dest, q.rs_rect[0], q.rs_rect[1], q.rs_rect[2], q.rs_rect[3], q.rs_dest_pitch, q.rs_dest_height);
                desc += b;
            } else {
                ++nd;
                if (q.viewport[0] != lvp[0] || q.viewport[1] != lvp[1] || q.viewport[2] != lvp[2] || q.viewport[3] != lvp[3]) {
                    memcpy(lvp, q.viewport, sizeof lvp);
                    uint32_t u[4]; memcpy(u, q.viewport, sizeof u); for (int k = 0; k < 4; ++k) mix(u[k]);
                    char b[80]; snprintf(b, sizeof b, " V%.0f,%.0f,%.0fx%.0f", q.viewport[0], q.viewport[1], q.viewport[2], q.viewport[3]);
                    desc += b;
                }
            }
        }
        if (sig != prev_sig && nfs < 300) {
            ++nfs;
            printf("[FSIG] frame %llu: %u draws, %zu records, fb 0x%08X:%s\n", (unsigned long long)g_xd_frame_no, nd, f.draws.size(), f.swap_fb,
                   desc.size() > 3000 ? (desc.substr(0, 3000) + "...").c_str() : desc.c_str());
            fflush(stdout);
        }
        prev_sig = sig; prev_desc.swap(desc);
    }
    delete gp_diag;
    for (const LswGpuDrawRec& r : f.draws) {
        if (r.kind == 1) {   // EDRAM resolve
            if (xdlog) printf("[XDLOG] #%u RESOLVE src=%u rect=%d,%d..%d,%d pitch=%u -> 0x%08X %ux%u tfmt=%u clr c%u d%u\n", draw_i, r.rs_src,
                              r.rs_rect[0], r.rs_rect[1], r.rs_rect[2], r.rs_rect[3], r.surf_info & 0x3FFF, r.rs_dest, r.rs_dest_pitch, r.rs_dest_height, r.rs_dest_fmt, r.rs_clear_color, r.rs_clear_depth);
            ++draw_i;
            if (gpurt) { xd_do_resolve(r); ++resolves; bound_key = ~0ull; }
            xd_fstate(" R%u:%X[%d..%d]", r.rs_src, r.rs_dest, r.rs_rect[1], r.rs_rect[3]);
            continue;
        }
        // 1. Vertex data into shared memory: the ranges record time found not resident. Issued for
        // EVERY recorded draw, before any skip below, since the residency tracking assumed them.
        for (uint32_t k = 0; k < r.range_count; ++k) upload(f.ranges[r.first_range + k]);
        // Texture pre-pass: build composed / sized resolve-target copies now (they are blits). Not
        // needed when both tables repeat the previous draw's (their sources are already built).
        const uint32_t* fa_draw = reinterpret_cast<const uint32_t*>(f.arena.data() + r.cb_fetch_off);
        const bool tables_same = bindcache && xd_tex_same(tl[0], fa_draw, r.ps_tex_count, r.ps_tex_dim, r.ps_tex_fetch)
                                           && xd_tex_same(tl[1], fa_draw, r.vs_tex_count, r.vs_tex_dim, r.vs_tex_fetch);
        if (gpurt && !tables_same) {
            GpScope gp(GP_RP_PREPASS);
            const uint64_t blits_before = g_xd_blit_count;
            const uint32_t* fa = reinterpret_cast<const uint32_t*>(f.arena.data() + r.cb_fetch_off);
            auto prep = [&](uint32_t count, const uint8_t* dims, const uint8_t* fetches) {
                for (uint32_t t = 0; t < count && t < 32; ++t) {
                    if (dims[t] > 1) continue;
                    uint32_t tb, tf, tsw, tw, th, tdim;
                    if (!lsw_gpu_texture_info(fa + (fetches[t] & 31) * 6, &tb, &tf, &tsw, &tw, &th, &tdim)) continue;
                    XdTexSrc src; xd_texture_source(tb, tf, tw, th, src);
                }
            };
            prep(r.ps_tex_count, r.ps_tex_dim, r.ps_tex_fetch);
            prep(r.vs_tex_count, r.vs_tex_dim, r.vs_tex_fetch);
            if (g_xd_blit_count != blits_before) bound_key = ~0ull;   // a blit rebound the output merger: force the RT rebind below
        }
        // Render targets of this draw (sample space for MSAA surfaces).
        UINT msx = 1, msy = 1;
        DXGI_FORMAT rtv_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        DXGI_FORMAT rt_fmts[4] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
        uint32_t nrt = 1;
        UINT rt_w = kSwapW, rt_h = kSwapH;
        XdRt* crts[4] = {};
        if (gpurt) {
            GpScope gprt(GP_RP_RT);
            xd_msaa_scale(r.surf_info, msx, msy);
            // Colour targets the pixel shader writes and RB_COLOR_MASK enables (LSWTCS_MRT=0: RT0 only).
            static int mrt_on = -1; if (mrt_on < 0) { const char* e = getenv("LSWTCS_MRT"); mrt_on = (e && e[0] == '0') ? 0 : 1; }
            bool color = r.edram_mode == 4 && r.ps_dxbc;
            uint32_t want = 0;
            if (color) {
                uint32_t psm = (mrt_on && r.ps_rt_mask) ? r.ps_rt_mask : 1u;
                for (uint32_t i = 0; i < 4; ++i) if ((psm & (1u << i)) && ((r.color_mask >> (4 * i)) & 0xF)) want |= 1u << i;
                if (!mrt_on) want &= 1u;
                if (!want && (r.color_mask & 0xF)) want = 1u;   // keep the old RT0 behaviour as a floor
            }
            nrt = 0;
            bool fail = false;
            for (uint32_t i = 0; i < 4; ++i) if (want & (1u << i)) {
                crts[i] = xd_rt_color(r.surf_info, i == 0 ? r.color_info : r.color_info_mrt[i]);
                if (!crts[i]) fail = true;
                nrt = i + 1;
            }
            XdRt* drt = xd_rt_depth(r.surf_info, r.depth_info);
            if (!drt || fail) { ++skipped; continue; }
            for (uint32_t i = 0; i < 4; ++i) rt_fmts[i] = i < nrt ? (crts[i] ? crts[i]->fmt : DXGI_FORMAT_R8G8B8A8_UNORM) : DXGI_FORMAT_UNKNOWN;
            rtv_fmt = nrt ? rt_fmts[0] : DXGI_FORMAT_UNKNOWN;
            rt_w = drt->w; rt_h = kXdRtH;
            uint64_t key = drt->view;
            for (uint32_t i = 0; i < nrt; ++i) key = key * 1031 + (crts[i] ? crts[i]->view + 1 : 0);
            key ^= uint64_t(nrt) << 56;
            bool states_ok = drt->state == D3D12_RESOURCE_STATE_DEPTH_WRITE;
            for (uint32_t i = 0; i < nrt; ++i) if (crts[i] && crts[i]->state != D3D12_RESOURCE_STATE_RENDER_TARGET) states_ok = false;
            if (key != bound_key || !states_ok) {
                D3D12_CPU_DESCRIPTOR_HANDLE rh[4];
                for (uint32_t i = 0; i < nrt; ++i) {
                    if (crts[i]) { xd_rt_to(*crts[i], D3D12_RESOURCE_STATE_RENDER_TARGET); rh[i] = xd_rtv(crts[i]->view); }
                    else rh[i] = xd_null_rtv();
                }
                xd_rt_to(*drt, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                D3D12_CPU_DESCRIPTOR_HANDLE dh = xd_dsv(drt->view);
                g_cmd->OMSetRenderTargets(nrt, nrt ? rh : nullptr, FALSE, &dh);
                bound_key = key;
            }
            ++rt_draws;
        }
        shm_to(kShmRead);
        if (dt_mode && (r.surf_info & 0x3FFF) == 640 && r.ps_tex_count >= 6) {
            const uint32_t* fa0 = reinterpret_cast<const uint32_t*>(f.arena.data() + r.cb_fetch_off);
            bool uses_depth = false;
            for (uint32_t t = 0; t < r.ps_tex_count && !uses_depth; ++t) {
                uint32_t tb = 0, tf = 0, tsw = 0, tw = 0, th = 0, tdim = 0;
                if (!lsw_gpu_texture_info(fa0 + (r.ps_tex_fetch[t] & 31) * 6, &tb, &tf, &tsw, &tw, &th, &tdim)) continue;
                auto rit = g_xd_resolved.find(tb);
                if (rit != g_xd_resolved.end() && rit->second.tfmt == 22) uses_depth = true;
            }
            if (uses_depth) {
                if (dt_mode != 3 && gpurt) {
                    const float c = dt_mode == 1 ? 1.0f : 0.0f;
                    const float clr[4] = {c, c, c, c};
                    D3D12_RECT cr = { LONG(r.viewport[0] * msx), LONG(r.viewport[1] * msy),
                                      LONG((r.viewport[0] + r.viewport[2]) * msx), LONG((r.viewport[1] + r.viewport[3]) * msy) };
                    if (r.viewport[2] <= 0 || r.viewport[3] <= 0) cr = { 0, 0, LONG(rt_w), LONG(rt_h) };
                    for (uint32_t i = 0; i < nrt; ++i)
                        if (crts[i]) g_cmd->ClearRenderTargetView(xd_rtv(crts[i]->view), clr, 1, &cr);
                }
                ++skipped; continue;
            }
        }
        // 2. Root signature / pipeline / bindings.
        ID3D12RootSignature* rs = xd_rootsig(r.ps_tex_count, r.ps_smp_count, r.vs_tex_count, r.vs_smp_count);
        if (xdlog) {
            printf("[XDLOG] #%u DRAW mode=%u pitch=%u msaa=%u col=%u/%u dep=%u n=%u ib=%u topo=%u blend=0x%08X cmask=0x%X zctl=0x%08X vp=%.0f,%.0f %.0fx%.0f z=%.3f..%.3f ps=%s tex=%u\n",
                   draw_i, r.edram_mode, r.surf_info & 0x3FFF, (r.surf_info >> 16) & 3, r.color_info & 0xFFF, (r.color_info >> 16) & 0xF, r.depth_info & 0xFFF,
                   r.vertex_count, r.ib_count, r.topology, r.blendcontrol, r.color_mask, r.depthcontrol,
                   r.viewport[0], r.viewport[1], r.viewport[2], r.viewport[3], r.viewport[4], r.viewport[5], r.ps_dxbc ? "y" : "n", r.ps_tex_count);
            printf("[XDLOG]      vs=%016llX ps=%016llX\n", (unsigned long long)r.vs_key, (unsigned long long)r.ps_key);
            const uint32_t* fa = reinterpret_cast<const uint32_t*>(f.arena.data() + r.cb_fetch_off);
            for (uint32_t t = 0; t < r.ps_tex_count; ++t) {
                uint32_t tb = 0, tf = 0, tsw = 0, tw = 0, th = 0, tdim = 0;
                const uint32_t* f6 = fa + (r.ps_tex_fetch[t] & 31) * 6;
                int ok = lsw_gpu_texture_info(f6, &tb, &tf, &tsw, &tw, &th, &tdim);
                bool hit = ok && g_xd_resolved.count(tb);
                printf("[XDLOG]      t%u fetch%u dim(bind)=%u ok=%d base=0x%08X fmt=%u %ux%u dim=%u swz=0x%03X %s\n", t, r.ps_tex_fetch[t], r.ps_tex_dim[t], ok, tb, tf, tw, th, tdim, tsw,
                       hit ? "RESOLVED" : "guest");
            }
            fflush(stdout);
        }
        ++draw_i;
        if (!gpurt) { rt_fmts[0] = rtv_fmt; nrt = 1; }
        ID3D12PipelineState* pso;
        { GpScope gp(GP_RP_PSO); pso = rs ? xd_pso(r, rs, rt_fmts, nrt) : nullptr; }
        if (!pso) { ++skipped; continue; }
        uint64_t t_draw = gp_now();   // GP_RP_DRAW: root/state/draw calls, bind excluded
        if (g_xd_blit_count != st_blits) { st_rs = nullptr; st_pso = nullptr; st_topo = -1; st_vp_valid = false; st_blits = g_xd_blit_count; }   // a blit set its own state
        if (rs != st_rs || !bindcache) { g_cmd->SetGraphicsRootSignature(rs); st_rs = rs; }
        if (pso != st_pso || !bindcache) { g_cmd->SetPipelineState(pso); st_pso = pso; }
        g_cmd->SetGraphicsRootConstantBufferView(0, up + r.cb_fetch_off);
        g_cmd->SetGraphicsRootConstantBufferView(1, up + r.cb_vsf_off);
        g_cmd->SetGraphicsRootConstantBufferView(2, up + r.cb_psf_off);
        g_cmd->SetGraphicsRootConstantBufferView(3, up + r.cb_sys_off);
        g_cmd->SetGraphicsRootConstantBufferView(4, up + r.cb_bool_off);
        g_cmd->SetGraphicsRootDescriptorTable(5, shm_table);
        UINT param = 6;
        const uint32_t* fetch_all = reinterpret_cast<const uint32_t*>(f.arena.data() + r.cb_fetch_off);
        static int notex = -1; if (notex < 0) { const char* e = getenv("LSWTCS_GPUDRAW_NOTEX"); notex = (e && e[0] == '1') ? 1 : 0; }
        // Texture table as view specs; a table identical to one already written this replay reuses its
        // descriptors (hub: ~2500 texture binds/frame, far fewer distinct tables) instead of creating SRVs.
        auto tex_table_cached = [&](int stage, uint32_t count, const uint8_t* dims, const uint8_t* fetches) {
            if (xd_tex_same(tl[stage], fetch_all, count, dims, fetches)) { g_cmd->SetGraphicsRootDescriptorTable(param++, tl[stage].g); return; }
            tl[stage].valid = false;
            if (count > 32) count = 32;
            XdSrvSpec sp[32];
            for (uint32_t t = 0; t < count; ++t) {
                const uint32_t* f6 = fetch_all + (fetches[t] & 31) * 6;
                XdSrvSpec& s = sp[t]; s = XdSrvSpec{};
                if (gpurt && !notex && dims[t] <= 1) {
                    uint32_t tb, tf, tsw, tw, th, tdim;
                    bool tinfo = lsw_gpu_texture_info(f6, &tb, &tf, &tsw, &tw, &th, &tdim) != 0;
                    XdTexSrc src{};
                    if (tinfo && xd_texture_source(tb, tf, tw, th, src)) {
                        const D3D12_RESOURCE_STATES srv_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        if (*src.state != srv_state) { barrier(src.res, *src.state, srv_state); *src.state = srv_state; }
                        bool single = src.fmt == DXGI_FORMAT_R8_UNORM || src.fmt == DXGI_FORMAT_R32_FLOAT;
                        { static int k8 = -1; if (k8 < 0) { const char* e = getenv("LSWTCS_K8RGBA"); k8 = (e && e[0] == '0') ? 0 : 1; }
                          if (k8 && tf == 2 && !single) tsw = 0x688; }   // experiment: sample RGBA as-is
                        uint32_t map = 0;
                        for (int c = 0; c < 4; ++c) {
                            uint32_t sw = (tsw >> (3 * c)) & 7;
                            if (sw < 4 && single) sw = 0; else if (sw > 5) sw = 4;
                            map |= sw << (3 * c);
                        }
                        s.res = src.res; s.fmt = src.fmt; s.map = map | (1u << 12);
                        s.dim = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; s.mips = 1; s.layers = 1;
                        ++res_tex_binds;
                        continue;
                    }
                }
                const XdTex* tx = notex ? nullptr : xd_get_texture(f6);
                bool cube = dims[t] == 3;
                if (tx && (cube != (tx->dim == 2 && tx->layers == 6))) tx = nullptr;   // dimension mismatch
                if (dims[t] == 2) tx = nullptr;                                          // 3D: not decoded yet
                s.res = tx ? tx->res : nullptr;
                s.fmt = tx ? tx->dxgi : DXGI_FORMAT_R8G8B8A8_UNORM;
                s.map = tx ? tx->mapping : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                switch (dims[t]) {
                    case 2: s.dim = D3D12_SRV_DIMENSION_TEXTURE3D; s.mips = 1; break;
                    case 3: s.dim = D3D12_SRV_DIMENSION_TEXTURECUBE; s.mips = tx ? tx->mips : 1; break;
                    default: s.dim = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; s.mips = tx ? tx->mips : 1; s.layers = tx ? tx->layers : 1; break;
                }
            }
            const uint64_t h = lsw_hash_bytes(reinterpret_cast<const uint8_t*>(sp), uint32_t(sizeof(XdSrvSpec) * count)) ^ count;
            UINT first = ~0u;
            auto it = g_xd_srv_tables.find(h);
            if (it != g_xd_srv_tables.end() && it->second.count == count &&
                memcmp(&g_xd_srv_specs[it->second.first - g_xd_desc_base], sp, sizeof(XdSrvSpec) * count) == 0)
                first = it->second.first;
            if (first == ~0u) {
                if (g_xd_desc_used + count > kXdDescPerFrame) { ++param; return; }
                GpScope gps(GP_RP_SRV);
                first = g_xd_desc_base + g_xd_desc_used;
                for (uint32_t t = 0; t < count; ++t) {
                    D3D12_CPU_DESCRIPTOR_HANDLE hc = g_xd_srv_heap->GetCPUDescriptorHandleForHeapStart(); hc.ptr += SIZE_T(first + t) * g_xd_srv_inc;
                    xd_srv_create(sp[t], hc);
                    g_xd_srv_specs[g_xd_desc_used + t] = sp[t];
                }
                g_xd_desc_used += count;
                g_xd_srv_tables[h] = XdSrvTable{first, count};
            }
            D3D12_GPU_DESCRIPTOR_HANDLE g = g_xd_srv_heap->GetGPUDescriptorHandleForHeapStart(); g.ptr += UINT64(first) * g_xd_srv_inc;
            g_cmd->SetGraphicsRootDescriptorTable(param++, g);
            xd_tex_remember(tl[stage], fetch_all, count, dims, fetches, g);
        };
        auto tex_table = [&](int stage, uint32_t count, const uint8_t* dims, const uint8_t* fetches) {
            if (!count) return;
            if (bindcache) { tex_table_cached(stage, count, dims, fetches); return; }
            if (g_xd_desc_used + count > kXdDescPerFrame) { ++param; return; }
            UINT first = g_xd_desc_base + g_xd_desc_used;
            for (uint32_t t = 0; t < count; ++t) {
                const uint32_t* f6 = fetch_all + (fetches[t] & 31) * 6;
                if (gpurt && !notex && dims[t] <= 1) {
                    uint32_t tb, tf, tsw, tw, th, tdim;
                    bool tinfo = lsw_gpu_texture_info(f6, &tb, &tf, &tsw, &tw, &th, &tdim) != 0;
                    XdTexSrc src{};
                    if (tinfo && xd_texture_source(tb, tf, tw, th, src)) {
                        const D3D12_RESOURCE_STATES srv_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        if (*src.state != srv_state) { barrier(src.res, *src.state, srv_state); *src.state = srv_state; }
                        bool single = src.fmt == DXGI_FORMAT_R8_UNORM || src.fmt == DXGI_FORMAT_R32_FLOAT;
                        { static int k8 = -1; if (k8 < 0) { const char* e = getenv("LSWTCS_K8RGBA"); k8 = (e && e[0] == '0') ? 0 : 1; }
                          if (k8 && tf == 2 && !single) tsw = 0x688; }   // experiment: sample RGBA as-is
                        uint32_t map = 0;
                        for (int c = 0; c < 4; ++c) {
                            uint32_t sw = (tsw >> (3 * c)) & 7;
                            if (sw < 4 && single) sw = 0; else if (sw > 5) sw = 4;
                            map |= sw << (3 * c);
                        }
                        ID3D12Resource* bind_res = src.res;
                        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = src.fmt;
                        sv.Shader4ComponentMapping = map | (1u << 12);
                        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; sv.Texture2DArray.MipLevels = 1; sv.Texture2DArray.ArraySize = 1;
                        D3D12_CPU_DESCRIPTOR_HANDLE h = g_xd_srv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(first + t) * g_xd_srv_inc;
                        g_device->CreateShaderResourceView(bind_res, &sv, h);
                        ++res_tex_binds;
                        continue;
                    }
                }
                const XdTex* tx = notex ? nullptr : xd_get_texture(f6);
                bool cube = dims[t] == 3;
                if (tx && (cube != (tx->dim == 2 && tx->layers == 6))) tx = nullptr;   // dimension mismatch
                if (dims[t] == 2) tx = nullptr;                                          // 3D: not decoded yet
                D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
                sv.Format = tx ? (DXGI_FORMAT)tx->dxgi : DXGI_FORMAT_R8G8B8A8_UNORM;
                sv.Shader4ComponentMapping = tx ? tx->mapping : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                switch (dims[t]) {
                    case 2: sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sv.Texture3D.MipLevels = 1; break;
                    case 3: sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; sv.TextureCube.MipLevels = tx ? tx->mips : 1; break;
                    default: sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; sv.Texture2DArray.MipLevels = tx ? tx->mips : 1;
                             sv.Texture2DArray.ArraySize = tx ? tx->layers : 1; break;
                }
                D3D12_CPU_DESCRIPTOR_HANDLE h = g_xd_srv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(first + t) * g_xd_srv_inc;
                g_device->CreateShaderResourceView(tx ? tx->res : nullptr, &sv, h);
            }
            D3D12_GPU_DESCRIPTOR_HANDLE g = g_xd_srv_heap->GetGPUDescriptorHandleForHeapStart(); g.ptr += UINT64(first) * g_xd_srv_inc;
            g_cmd->SetGraphicsRootDescriptorTable(param++, g);
            g_xd_desc_used += count;
        };
        auto smp_table = [&](int stage, uint32_t count, const LswGpuSamplerBinding* bind) {
            if (!count) return;
            if (count > LSW_GPU_MAX_TEX) count = LSW_GPU_MAX_TEX;
            if (bindcache) {
                if (xd_smp_same(sl[stage], fetch_all, count, bind)) { g_cmd->SetGraphicsRootDescriptorTable(param++, sl[stage].g); return; }
                sl[stage].valid = false;
            }
            GpScope gps(GP_RP_SMP);
            D3D12_SAMPLER_DESC sds[LSW_GPU_MAX_TEX] = {};
            for (uint32_t t = 0; t < count; ++t) {
                LswGpuSamplerDesc sdsc;
                lsw_gpu_sampler(fetch_all + (bind[t].fetch_constant & 31) * 6, &bind[t], &sdsc);
                D3D12_SAMPLER_DESC& sd = sds[t];
                if (sdsc.max_aniso > 1) { sd.Filter = D3D12_FILTER_ANISOTROPIC; sd.MaxAnisotropy = sdsc.max_aniso; }
                else sd.Filter = (D3D12_FILTER)((sdsc.min_linear ? 0x10 : 0) | (sdsc.mag_linear ? 0x4 : 0) | (sdsc.mip_linear ? 0x1 : 0));
                sd.AddressU = (D3D12_TEXTURE_ADDRESS_MODE)sdsc.address_u;
                sd.AddressV = (D3D12_TEXTURE_ADDRESS_MODE)sdsc.address_v;
                sd.AddressW = (D3D12_TEXTURE_ADDRESS_MODE)sdsc.address_w;
                sd.MaxLOD = D3D12_FLOAT32_MAX;
                { static int bo = -1; if (bo < 0) { const char* e = getenv("LSWTCS_BORDER"); bo = (e && e[0] == '0') ? 0 : 1; }
                  if (bo) for (int c = 0; c < 4; ++c) sd.BorderColor[c] = sdsc.border[c]; }
            }
            if (bindcache) {   // hash + compare against the stored descs: no per-draw string allocation
                const uint64_t h = lsw_hash_bytes(reinterpret_cast<const uint8_t*>(sds), uint32_t(sizeof(D3D12_SAMPLER_DESC) * count)) ^ count;
                UINT first = ~0u;
                auto it = g_xd_smp_hcache.find(h);
                if (it != g_xd_smp_hcache.end() && it->second.count == count &&
                    memcmp(&g_xd_smp_descs[it->second.first], sds, sizeof(D3D12_SAMPLER_DESC) * count) == 0)
                    first = it->second.first;
                if (first == ~0u) {
                    if (g_xd_smp_next + count > kXdSmpPerFrame * kFrameCount) { ++smp_overflow; ++param; return; }
                    first = g_xd_smp_next; g_xd_smp_next += count;
                    for (uint32_t t = 0; t < count; ++t) {
                        D3D12_CPU_DESCRIPTOR_HANDLE hc = g_xd_smp_heap->GetCPUDescriptorHandleForHeapStart(); hc.ptr += SIZE_T(first + t) * g_xd_smp_inc;
                        g_device->CreateSampler(&sds[t], hc);
                        g_xd_smp_descs[first + t] = sds[t];
                    }
                    g_xd_smp_hcache[h] = XdSrvTable{first, count};
                    static uint32_t ntab = 0;
                    if (++ntab <= 8 || (ntab % 64) == 0) { printf("[SMPCACHE] %u tables, %u/%u samplers used\n", ntab, g_xd_smp_next, kXdSmpPerFrame * kFrameCount); fflush(stdout); }
                }
                D3D12_GPU_DESCRIPTOR_HANDLE g = g_xd_smp_heap->GetGPUDescriptorHandleForHeapStart(); g.ptr += UINT64(first) * g_xd_smp_inc;
                g_cmd->SetGraphicsRootDescriptorTable(param++, g);
                xd_smp_remember(sl[stage], fetch_all, count, bind, g);
                return;
            }
            std::string key(reinterpret_cast<const char*>(sds), sizeof(D3D12_SAMPLER_DESC) * count);
            UINT first;
            auto sit = g_xd_smp_cache.find(key);
            if (sit != g_xd_smp_cache.end()) first = sit->second;
            else if (g_xd_smp_next + count <= kXdSmpPerFrame * kFrameCount) {
                first = g_xd_smp_next; g_xd_smp_next += count;
                for (uint32_t t = 0; t < count; ++t) {
                    D3D12_CPU_DESCRIPTOR_HANDLE h = g_xd_smp_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(first + t) * g_xd_smp_inc;
                    g_device->CreateSampler(&sds[t], h);
                }
                g_xd_smp_cache.emplace(std::move(key), first);
                static uint32_t ntab = 0;
                if (++ntab <= 8 || (ntab % 64) == 0) { printf("[SMPCACHE] %u tables, %u/%u samplers used\n", ntab, g_xd_smp_next, kXdSmpPerFrame * kFrameCount); fflush(stdout); }
            } else { ++smp_overflow; ++param; return; }   // heap full: keep the root-parameter order intact
            D3D12_GPU_DESCRIPTOR_HANDLE g = g_xd_smp_heap->GetGPUDescriptorHandleForHeapStart(); g.ptr += UINT64(first) * g_xd_smp_inc;
            g_cmd->SetGraphicsRootDescriptorTable(param++, g);
        };
        g_gpuprof[GP_RP_DRAW] += gp_now() - t_draw;
        { GpScope gp(GP_RP_BIND);
          tex_table(0, r.ps_tex_count, r.ps_tex_dim, r.ps_tex_fetch);
          smp_table(0, r.ps_smp_count, r.ps_smp);
          tex_table(1, r.vs_tex_count, r.vs_tex_dim, r.vs_tex_fetch);
          smp_table(1, r.vs_smp_count, r.vs_smp); }
        t_draw = gp_now();
        // 3. Fixed function + draw.
        D3D12_VIEWPORT vp = { r.viewport[0] * msx, r.viewport[1] * msy, r.viewport[2] * msx, r.viewport[3] * msy, r.viewport[4], r.viewport[5] };
        if (vp.Width <= 0 || vp.Height <= 0) { vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)rt_w; vp.Height = (float)rt_h; }
        D3D12_RECT sc = { LONG(r.scissor[0] * msx), LONG(r.scissor[1] * msy), LONG((r.scissor[0] + r.scissor[2]) * msx), LONG((r.scissor[1] + r.scissor[3]) * msy) };
        if (r.scissor[2] == 0 || r.scissor[3] == 0) sc = { 0, 0, LONG(rt_w), LONG(rt_h) };
        if (!bindcache || !st_vp_valid || memcmp(&vp, &st_vp, sizeof vp) != 0 || memcmp(&sc, &st_sc, sizeof sc) != 0) {
            g_cmd->RSSetViewports(1, &vp); g_cmd->RSSetScissorRects(1, &sc);
            st_vp = vp; st_sc = sc; st_vp_valid = true;
        }
        if (gpurt && xd_stencil() && (r.depthcontrol & 1)) g_cmd->OMSetStencilRef(r.stencil_ref & 0xFF);
        if (int(r.topology) != st_topo || !bindcache) { g_cmd->IASetPrimitiveTopology((D3D_PRIMITIVE_TOPOLOGY)r.topology); st_topo = int(r.topology); }
        if (r.ib_count) {
            D3D12_INDEX_BUFFER_VIEW ibv = { up + r.ib_off, r.ib_count * (r.ib_32bit ? 4u : 2u),
                                            r.ib_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT };
            g_cmd->IASetIndexBuffer(&ibv);
            g_cmd->DrawIndexedInstanced(r.ib_count, 1, 0, 0, 0);
        } else {
            g_cmd->DrawInstanced(r.vertex_count, 1, 0, 0);
        }
        g_gpuprof[GP_RP_DRAW] += gp_now() - t_draw;
        ++drawn;
    }
    if (gpurt) {
        bool shown = false;
        if (f.swap_fb) fb_phys = f.swap_fb;
        auto it = g_xd_resolved.find(fb_phys & 0x1FFFFFFFu);
        if (it != g_xd_resolved.end() && it->second.res) {
            XdResolved& rr = it->second;
            xd_res_to(rr, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            xd_blit(rr.res, rr.fmt, bb_rtv, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 0, std::min<UINT>(rr.w, kSwapW), std::min<UINT>(rr.h, kSwapH), 0, 0, 0, 1, 1);
            shown = true;
            xd_fstate(" PRESENT:%X(%ux%u fmt%d)", fb_phys & 0x1FFFFFFFu, rr.w, rr.h, int(rr.fmt));
        } else {
            auto ct = g_xd_rts.find((uint64_t(0) << 40) | (uint64_t(1280) << 16) | uint64_t(DXGI_FORMAT_R8G8B8A8_UNORM));
            if (ct != g_xd_rts.end() && ct->second.res) {
                xd_rt_to(ct->second, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                xd_blit(ct->second.res, ct->second.fmt, bb_rtv, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 0, kSwapW, kSwapH, 0, 0, 0, 1, 1);
                shown = true;
            }
        }
        xd_dump_queue();
        static int nl2 = 0;
        if (nl2++ < 5 || (nl2 % 600) == 0) { printf("[XERT] frame: %u RT draws, %u resolves, %u resolved-texture binds, fb 0x%08X %s, %zu RTs, %zu resolve targets\n",
            rt_draws, resolves, res_tex_binds, fb_phys, shown ? (it != g_xd_resolved.end() ? "from resolve" : "from RT0") : "none", g_xd_rts.size(), g_xd_resolved.size()); fflush(stdout); }
    }
    if (smp_overflow) { static uint32_t nov = 0; if (nov++ < 20) { printf("[SMPCACHE] OVERFLOW: %u sampler tables not bound this frame\n", smp_overflow); fflush(stdout); } }
    static int nlog = 0;
    if (nlog++ < 5 || (nlog % 600) == 0) { printf("[XEDRAW] frame: %u draws (%u skipped), %u shm copies, arena %zu KB, %zu PSOs\n",
        drawn, skipped, copies, f.arena.size() / 1024, g_xd_psos.size()); fflush(stdout); }
}
