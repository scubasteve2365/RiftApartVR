// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence.
// Picture setting: flat things (HUD, mod menu, aim dot, splash and menu screen, cinema shots) go to the headset either as quad
// layers (Panel) or drawn into each eye and sent as a projection layer (Steady). Some runtimes stretch quads when the head turns
// (Virtual Desktop); the projection way is pinned to the frame's own head pose.
// Steady keeps a texture for each panel (the runtime's swapchain images can only be touched while acquired), then a compute pass
// lays them out per eye from the same pose, size and image rectangle the quad would have had, so both ways match.
// Included by xr.cpp after EnsureUiChain. Slots: 0-2 UI images, 3 menu, 4 aim dot, 5 flat screen, 6-7 cinema eyes.

static const int kPanelSlots = 8, kPanelMax = 6;
enum { kPanelMenu = 3, kPanelDot = 4, kPanelScreen = 5, kPanelCinema = 6 };
struct PanelTex {
    ID3D12Resource* tex = nullptr;
    int w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
struct PanelCb {
    float a[4], b[4], c[4], d[4];  // position + slot, rotation, size + uv offset, uv scale + alpha used + eye mask
};
struct PicCb {
    float eyePos[4], eyeQ[4], tan[4], dims[4], misc[4];  // misc: panel count, opaque
    PanelCb panels[kPanelMax];
};

static PanelTex g_panelTex[kPanelSlots];
static ID3D12RootSignature* g_picRoot;
static ID3D12PipelineState* g_picPso;
static ID3D12DescriptorHeap* g_picHeap;
static UINT g_picInc;
static ID3D12Resource* g_picCb, *g_picOut[2];
static uint8_t* g_picCbMapped;
static UiChain g_picChain[2];
static bool g_picAcquired[2];
static PanelCb g_picPanels[kPanelMax];
static int g_picEyeMask[kPanelMax];
static int g_picCount;
static XrCompositionLayerProjectionView g_picViews[2];
static XrCompositionLayerProjection g_picLayer;
static std::wstring g_picDumpPath;
static std::atomic<bool> g_picDumpWanted{false};
static const UINT kPicCbStride = (sizeof(PicCb) + 255) & ~255u;

static const char kPicHlsl[] = R"(
struct Panel { float4 a, b, c, d; };
cbuffer C : register(b0) { float4 eyePos, eyeQ, tanLRUD, dims, misc; Panel panels[6]; };
Texture2D tex[8] : register(t0);
RWTexture2D<float4> outp : register(u0);
SamplerState lin : register(s0);
float3 rot(float4 q, float3 v) { return v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
float3 unrot(float4 q, float3 v) { return rot(float4(-q.xyz, q.w), v); }
[numthreads(32, 32, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    static const float2 sub[4] = {float2(-0.125, -0.375), float2(0.375, -0.125), float2(0.125, 0.375), float2(-0.375, 0.125)};
    float4 acc = misc.y > 0.5 ? float4(0, 0, 0, 1) : float4(0, 0, 0, 0);
    int n = (int)misc.x;
    for (int i = 0; i < n; ++i) {
        Panel p = panels[i];
        int slot = (int)p.a.w;
        float4 s = 0;
        for (int k = 0; k < 4; ++k) {
            float2 f = (id.xy + 0.5 + sub[k]) * dims.zw;
            float3 d = rot(eyeQ, float3(lerp(tanLRUD.x, tanLRUD.y, f.x), lerp(tanLRUD.z, tanLRUD.w, f.y), -1));
            float3 o = unrot(p.b, eyePos.xyz - p.a.xyz);
            float3 ld = unrot(p.b, d);
            if (abs(ld.z) < 1e-6) continue;
            float t = -o.z / ld.z;
            if (t <= 0) continue;
            float3 h = o + t * ld;
            float2 uv = float2(h.x / p.c.x + 0.5, 0.5 - h.y / p.c.y);
            if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) continue;
            float4 c = tex[NonUniformResourceIndex(slot)].SampleLevel(lin, p.c.zw + uv * p.d.xy, 0);
            if (p.d.z < 0.5) c.a = 1;
            s += c;
        }
        s *= 0.25;
        acc = s + acc * (1 - s.a);
    }
    outp[id.xy] = acc;
}
)";

static bool PicPipelines() {
    if (g_picRoot) return true;
    if (g_picFailed) return false;
    g_picFailed = true;  // cleared at the end; any early return leaves the Panel way in use
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    const HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = dc ? reinterpret_cast<CompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    const HMODULE d12 = GetModuleHandleW(L"d3d12.dll");
    const auto serialize = d12 ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!compile || !serialize) {
        Log("PICTURE no shader compiler, Panel in use");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE srv{}, uav{};
    srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv.NumDescriptors = kPanelSlots;
    uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uav;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;
    ID3DBlob* blob = nullptr, *err = nullptr;
    if (FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) || FAILED(g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_picRoot)))) {
        Log("PICTURE root signature failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        g_picRoot = nullptr;
        return false;
    }
    ID3DBlob* cs = nullptr, *e = nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    if (FAILED(compile(kPicHlsl, sizeof(kPicHlsl) - 1, "picture", nullptr, nullptr, "Main", "cs_5_1", 0, 0, &cs, &e))) {
        Log("PICTURE shader: %s", e ? static_cast<const char*>(e->GetBufferPointer()) : "");
        g_picRoot = nullptr;
        return false;
    }
    pd.pRootSignature = g_picRoot;
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_picPso)))) {
        Log("PICTURE pipeline failed");
        g_picRoot = nullptr;
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kPanelSlots + 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_picHeap)))) {
        Log("PICTURE descriptor heap failed");
        g_picRoot = nullptr;
        return false;
    }
    g_picInc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC nul{};
    nul.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nul.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nul.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nul.Texture2D.MipLevels = 1;
    for (int i = 0; i < kPanelSlots; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_picHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * g_picInc;
        g_dev->CreateShaderResourceView(nullptr, &nul, h);
    }
    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>(kRing) * 2 * kPicCbStride;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_picCb))) ||
        FAILED(g_picCb->Map(0, nullptr, reinterpret_cast<void**>(&g_picCbMapped)))) {
        Log("PICTURE constants failed");
        g_picRoot = nullptr;
        return false;
    }
    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = g_chainW;
    td.Height = g_chainH;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (int i = 0; i < 2; ++i) {
        if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_picOut[i])))) {
            Log("PICTURE output texture failed");
            g_picRoot = nullptr;
            return false;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_picHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(kPanelSlots + i) * g_picInc;
        g_dev->CreateUnorderedAccessView(g_picOut[i], nullptr, &uv, h);
    }
    g_picFailed = false;
    Log("PICTURE pass ready, %dx%d per eye", g_chainW, g_chainH);
    return true;
}

static bool PanelHave(int slot) { return g_panelTex[slot].tex != nullptr; }

// image is a swapchain image in the render target state, already holding this frame's picture.
static void PanelMirror(ID3D12GraphicsCommandList* list, int slot, ID3D12Resource* image, int w, int h, int64_t format) {
    if (!PictureSteady() || !PicPipelines()) return;
    const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const DXGI_FORMAT f = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    PanelTex& m = g_panelTex[slot];
    if (!m.tex || m.w != w || m.h != h || m.format != f) {
        if (m.tex) {
            // A resize: the descriptor below may be in use by frames still on the GPU.
            const UINT64 v = ++g_fenceValue;
            g_queue->Signal(g_fence, v);
            if (g_fence->GetCompletedValue() < v) {
                g_fence->SetEventOnCompletion(v, g_fenceEvent);
                WaitForSingleObject(g_fenceEvent, 500);
            }
            m.tex->Release();
            m.tex = nullptr;
        }
        D3D12_HEAP_PROPERTIES def{};
        def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = f;
        td.SampleDesc.Count = 1;
        if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m.tex)))) {
            Log("PICTURE mirror %d (%dx%d) failed", slot, w, h);
            m.tex = nullptr;
            return;
        }
        m.w = w;
        m.h = h;
        m.format = f;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = f;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE ch = g_picHeap->GetCPUDescriptorHandleForHeapStart();
        ch.ptr += static_cast<SIZE_T>(slot) * g_picInc;
        g_dev->CreateShaderResourceView(m.tex, &sv, ch);
    }
    D3D12_RESOURCE_BARRIER bar[2]{};
    bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[0].Transition.pResource = image;
    bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar[1] = bar[0];
    bar[1].Transition.pResource = m.tex;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(2, bar);
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    d.pResource = m.tex;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.pResource = image;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    std::swap(bar[0].Transition.StateBefore, bar[0].Transition.StateAfter);
    std::swap(bar[1].Transition.StateBefore, bar[1].Transition.StateAfter);
    list->ResourceBarrier(2, bar);
}

static void PanelsBegin() { g_picCount = 0; }

// eyes: bit 0 left, bit 1 right.
static void PanelAddRaw(int slot, const XrPosef& pose, float width, float height, int rx, int ry, int rw, int rh, bool alpha, int eyes) {
    if (g_picCount >= kPanelMax || !g_panelTex[slot].tex) return;
    const PanelTex& m = g_panelTex[slot];
    PanelCb& p = g_picPanels[g_picCount];
    p = {{pose.position.x, pose.position.y, pose.position.z, static_cast<float>(slot)},
         {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
         {width, height, static_cast<float>(rx) / m.w, static_cast<float>(ry) / m.h},
         {static_cast<float>(rw) / m.w, static_cast<float>(rh) / m.h, alpha ? 1.0f : 0.0f, 0.0f}};
    g_picEyeMask[g_picCount] = eyes;
    ++g_picCount;
}

// The same place, size and image as this quad would have had.
static void PanelAdd(int slot, const XrCompositionLayerQuad& q) {
    PanelAddRaw(slot, q.pose, q.size.width, q.size.height, q.subImage.imageRect.offset.x, q.subImage.imageRect.offset.y, q.subImage.imageRect.extent.width,
                q.subImage.imageRect.extent.height, (q.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0, 3);
}

// Draws each eye's panels and copies them to the overlay swapchains. Views are the frame's head poses. False if nothing to send.
static bool PanelsRender(ID3D12GraphicsCommandList* list, const XrPosef poses[2], const XrFovf fovs[2], bool opaque) {
    if (!g_picCount || !PicPipelines()) return false;
    if (!EnsureUiChain(g_picChain[0], g_chainW, g_chainH) || !EnsureUiChain(g_picChain[1], g_chainW, g_chainH)) return false;
    list->SetDescriptorHeaps(1, &g_picHeap);
    list->SetComputeRootSignature(g_picRoot);
    list->SetPipelineState(g_picPso);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_picHeap->GetGPUDescriptorHandleForHeapStart();
    list->SetComputeRootDescriptorTable(1, gpu);
    for (int e = 0; e < 2; ++e) {
        PicCb cb{};
        cb.eyePos[0] = poses[e].position.x;
        cb.eyePos[1] = poses[e].position.y;
        cb.eyePos[2] = poses[e].position.z;
        cb.eyeQ[0] = poses[e].orientation.x;
        cb.eyeQ[1] = poses[e].orientation.y;
        cb.eyeQ[2] = poses[e].orientation.z;
        cb.eyeQ[3] = poses[e].orientation.w;
        cb.tan[0] = tanf(fovs[e].angleLeft);
        cb.tan[1] = tanf(fovs[e].angleRight);
        cb.tan[2] = tanf(fovs[e].angleUp);
        cb.tan[3] = tanf(fovs[e].angleDown);
        cb.dims[0] = static_cast<float>(g_chainW);
        cb.dims[1] = static_cast<float>(g_chainH);
        cb.dims[2] = 1.0f / g_chainW;
        cb.dims[3] = 1.0f / g_chainH;
        int count = 0;
        for (int i = 0; i < g_picCount; ++i)
            if (g_picEyeMask[i] & (1 << e)) cb.panels[count++] = g_picPanels[i];
        cb.misc[0] = static_cast<float>(count);
        cb.misc[1] = opaque ? 1.0f : 0.0f;
        const UINT offset = (static_cast<UINT>(g_ringIndex) * 2 + e) * kPicCbStride;
        memcpy(g_picCbMapped + offset, &cb, sizeof(cb));
        list->SetComputeRootConstantBufferView(0, g_picCb->GetGPUVirtualAddress() + offset);
        D3D12_GPU_DESCRIPTOR_HANDLE uav = gpu;
        uav.ptr += static_cast<UINT64>(kPanelSlots + e) * g_picInc;
        list->SetComputeRootDescriptorTable(2, uav);
        list->Dispatch((g_chainW + 31) / 32, (g_chainH + 31) / 32, 1);

        uint32_t idx = 0;
        p_xrAcquireSwapchainImage(g_picChain[e].chain, nullptr, &idx);
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000;
        p_xrWaitSwapchainImage(g_picChain[e].chain, &wi);
        g_picAcquired[e] = true;
        ID3D12Resource* dst = g_picChain[e].images[idx].texture;
        D3D12_RESOURCE_BARRIER bar[2]{};
        bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar[0].Transition.pResource = g_picOut[e];
        bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bar[1] = bar[0];
        bar[1].Transition.pResource = dst;
        bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(2, bar);
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = dst;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.pResource = g_picOut[e];
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        std::swap(bar[0].Transition.StateBefore, bar[0].Transition.StateAfter);
        std::swap(bar[1].Transition.StateBefore, bar[1].Transition.StateAfter);
        list->ResourceBarrier(2, bar);

        g_picViews[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        g_picViews[e].pose = poses[e];
        g_picViews[e].fov = fovs[e];
        g_picViews[e].subImage.swapchain = g_picChain[e].chain;
        g_picViews[e].subImage.imageRect.extent = {g_chainW, g_chainH};
    }
    g_picLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    g_picLayer.layerFlags = opaque ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    g_picLayer.space = g_space;
    g_picLayer.viewCount = 2;
    g_picLayer.views = g_picViews;
    return true;
}

static void PanelsRelease() {
    for (int e = 0; e < 2; ++e) {
        if (!g_picAcquired[e]) continue;
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        p_xrReleaseSwapchainImage(g_picChain[e].chain, &ri);
        g_picAcquired[e] = false;
    }
    if (g_picDumpWanted.exchange(false) && g_picOut[0]) {
        static std::wstring pa, pb;
        pa = g_picDumpPath + L"_L.bmp";
        pb = g_picDumpPath + L"_R.bmp";
        void* r[2] = {g_picOut[0], g_picOut[1]};
        uint32_t st[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        const wchar_t* paths[2] = {pa.c_str(), pb.c_str()};
        CaptureRequest(r, st, paths, 2);
    }
}

void XrPictureDump(const wchar_t* base) {
    g_picDumpPath = base;
    g_picDumpWanted = true;
}
