// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence.
// Depth rebuild for alternate eye. The eye not drawn this tick is rebuilt from the eye that was: its depth is projected into the
// missing eye's view, then each pixel looks up the drawn eye's colour. What the drawn eye could not see comes from the missing
// eye's own last image, turned to the current view. Included by xr.cpp. Written for this mod, no outside shader code.

static const char kRebuildHlsl[] = R"(
cbuffer C : register(b0) {
    float4 dRight, dUp, dBack, dPos;
    float4 sRight, sUp, sBack, sPos;
    float4 hRight, hUp, hBack;
    float4 dTan, sTan, hTan;   // left, right, up, down
    float4 misc;               // near, map width, map height, unused
};
Texture2D<float4> srcColor : register(t0);
Texture2D<float4> histColor : register(t1);
Texture2D<float>  srcDepth : register(t2);
RWTexture2D<uint>   map : register(u0);
RWTexture2D<float4> outColor : register(u1);
SamplerState lin : register(s0);

float3 EyeRay(float2 uv, float4 t) { return float3(lerp(t.x, t.y, uv.x), lerp(t.z, t.w, uv.y), -1); }
float3 ToWorld(float3 p, float4 r, float4 u, float4 b, float4 o) { return o.xyz + r.xyz * p.x + u.xyz * p.y + b.xyz * p.z; }
float3 ToEye(float3 w, float4 r, float4 u, float4 b, float4 o) { float3 v = w - o.xyz; return float3(dot(v, r.xyz), dot(v, u.xyz), dot(v, b.xyz)); }
float2 ToUv(float3 e, float4 t) { float2 q = e.xy / -e.z; return float2((q.x - t.x) / (t.y - t.x), (q.y - t.z) / (t.w - t.z)); }

[numthreads(16, 16, 1)] void Clear(uint3 id : SV_DispatchThreadID) { map[id.xy] = 0; }

[numthreads(16, 16, 1)] void Splat(uint3 id : SV_DispatchThreadID) {
    uint2 size = uint2(misc.y, misc.z);
    if (id.x >= size.x || id.y >= size.y) return;
    float d = max(srcDepth[id.xy], 0.000001);
    float z = misc.x / d;
    float2 uv = (id.xy + 0.5) / size;
    float3 w = ToWorld(EyeRay(uv, sTan) * z, sRight, sUp, sBack, sPos);
    float3 e = ToEye(w, dRight, dUp, dBack, dPos);
    if (e.z >= -0.001) return;
    float2 t = ToUv(e, dTan);
    if (any(t < 0) || any(t >= 1)) return;
    uint2 at = uint2(t * size);
    uint old;
    InterlockedMax(map[at], asuint(misc.x / -e.z), old);
}

[numthreads(16, 16, 1)] void Resolve(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    outColor.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (id.xy + 0.5) / float2(w, h);
    int2 size = int2(misc.y, misc.z);
    int2 at = clamp(int2(uv * size), int2(0, 0), size - 1);
    uint c = map[at];
    if (c == 0) {
        // Cracks between projected texels. Wider gaps stay holes.
        for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)
            c = max(c, map[clamp(at + int2(x, y), int2(0, 0), size - 1)]);
    }
    float3 ray = EyeRay(uv, dTan);
    float3 worldDir = dRight.xyz * ray.x + dUp.xyz * ray.y + dBack.xyz * ray.z;
    bool ok = false;
    float4 color = 0;
    if (c != 0) {
        float z = misc.x / asfloat(c);
        float3 p = ToWorld(ray * z, dRight, dUp, dBack, dPos);
        float3 e = ToEye(p, sRight, sUp, sBack, sPos);
        float2 s = ToUv(e, sTan);
        if (e.z < 0 && all(s >= 0) && all(s <= 1)) {
            // What the drawn eye holds there must be this surface, not something in front of it.
            int2 ds = clamp(int2(s * size), int2(0, 0), size - 1);
            float seen = misc.x / max(srcDepth[ds], 0.000001);
            if (seen > -e.z * 0.93) {
                color = srcColor.SampleLevel(lin, s, 0);
                ok = true;
            }
        }
    }
    if (!ok) {
        float3 v = float3(dot(worldDir, hRight.xyz), dot(worldDir, hUp.xyz), dot(worldDir, hBack.xyz));
        float2 s = v.z < 0 ? ToUv(v, hTan) : uv;
        color = histColor.SampleLevel(lin, saturate(s), 0);
    }
    outColor[id.xy] = float4(color.rgb, 1);
}
)";

struct RebuildConstants {
    float dRight[4], dUp[4], dBack[4], dPos[4];
    float sRight[4], sUp[4], sBack[4], sPos[4];
    float hRight[4], hUp[4], hBack[4];
    float dTan[4], sTan[4], hTan[4];
    float misc[4];
};
static_assert(sizeof(RebuildConstants) == 60 * 4, "root constants");

static ID3D12RootSignature* g_rbRoot;
static ID3D12PipelineState* g_rbClear, *g_rbSplat, *g_rbResolve;
static ID3D12DescriptorHeap* g_rbHeap;
static UINT g_rbInc;
static ID3D12Resource* g_rbHist[2], *g_rbOut, *g_rbMap, *g_rbDepth[2];
static UINT g_rbDepthW, g_rbDepthH;
static bool g_rbDepthValid[2];
static std::mutex g_rbLock;
static bool g_rbFailed;
static std::atomic<int> g_rebuildMode{0};  // 0 off, 1 on
static RebuildEye g_rbHeld[2];             // the camera of each eye's image in g_rbHist
static std::atomic<uint64_t> g_rbCount{0};
static std::atomic<int> g_rbStage{3};      // lab: 0 nothing, 1 depth copy, 2 and history, 3 everything
void XrRebuildStage(int stage) { g_rbStage = stage; }

static ID3D12Resource* RbTexture(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ID3D12Resource* r = nullptr;
    const HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) Log("REBUILD texture %ux%u format %d failed %08lx", w, h, static_cast<int>(f), static_cast<unsigned long>(hr));
    return r;
}

static bool RbPipelines() {
    if (g_rbRoot) return true;
    if (g_rbFailed) return false;
    g_rbFailed = true;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    const HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = dc ? reinterpret_cast<CompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    const HMODULE d12 = GetModuleHandleW(L"d3d12.dll");
    const auto serialize = d12 ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!compile || !serialize) {
        Log("REBUILD no shader compiler");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].OffsetInDescriptorsFromTableStart = 3;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = sizeof(RebuildConstants) / 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;
    ID3DBlob* blob = nullptr, *err = nullptr;
    if (FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) || FAILED(g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rbRoot)))) {
        Log("REBUILD root signature failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        return false;
    }
    const struct { const char* entry; ID3D12PipelineState** out; } stages[3] = {{"Clear", &g_rbClear}, {"Splat", &g_rbSplat}, {"Resolve", &g_rbResolve}};
    for (const auto& st : stages) {
        ID3DBlob* cs = nullptr, *e = nullptr;
        if (FAILED(compile(kRebuildHlsl, sizeof(kRebuildHlsl) - 1, "rebuild", nullptr, nullptr, st.entry, "cs_5_1", 0, 0, &cs, &e))) {
            Log("REBUILD shader %s: %s", st.entry, e ? static_cast<const char*>(e->GetBufferPointer()) : "");
            g_rbRoot = nullptr;
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rbRoot;
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(st.out)))) {
            Log("REBUILD pipeline %s failed", st.entry);
            g_rbRoot = nullptr;
            return false;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 10;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rbHeap)))) {
        g_rbRoot = nullptr;
        return false;
    }
    g_rbInc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_rbFailed = false;
    Log("REBUILD pipelines ready");
    return true;
}

static void RbBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = a;
    bar.Transition.StateAfter = b;
    list->ResourceBarrier(1, &bar);
}

static void RbWriteDescriptors(int source) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rbHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(source) * 5 * g_rbInc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_dev->CreateShaderResourceView(g_rbHist[source], &sd, h);
    h.ptr += g_rbInc;
    g_dev->CreateShaderResourceView(g_rbHist[1 - source], &sd, h);
    h.ptr += g_rbInc;
    sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    g_dev->CreateShaderResourceView(g_rbDepth[source], &sd, h);
    h.ptr += g_rbInc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format = DXGI_FORMAT_R32_UINT;
    g_dev->CreateUnorderedAccessView(g_rbMap, nullptr, &ud, h);
    h.ptr += g_rbInc;
    ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_dev->CreateUnorderedAccessView(g_rbOut, nullptr, &ud, h);
}

// Called on the game's list right at its DLSS call, while the eye's depth is there to be read. In GENERIC_READ, which allows a copy.
void XrRebuildCopyDepth(int eye, void* cmd, void* depth) {
    if (g_rebuildMode.load() == 0 || !g_dev || !cmd || !depth || eye < 0 || eye > 1 || g_rbStage.load() < 1) return;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(cmd);
    auto* res = static_cast<ID3D12Resource*>(depth);
    std::lock_guard<std::mutex> l(g_rbLock);
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Format != DXGI_FORMAT_R32G8X24_TYPELESS) return;
    if (!g_rbDepth[eye] || g_rbDepthW != d.Width || g_rbDepthH != d.Height) {
        // Size change: start over. The old ones may still be in flight, so they are left to the process.
        g_rbDepthW = static_cast<UINT>(d.Width);
        g_rbDepthH = d.Height;
        g_rbDepthValid[0] = g_rbDepthValid[1] = false;
        g_rbDepth[0] = RbTexture(g_rbDepthW, g_rbDepthH, d.Format, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_rbDepth[1] = RbTexture(g_rbDepthW, g_rbDepthH, d.Format, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_rbMap = RbTexture(g_rbDepthW, g_rbDepthH, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Log("REBUILD depth %ux%u", g_rbDepthW, g_rbDepthH);
    }
    if (!g_rbDepth[eye]) return;
    RbBarrier(list, g_rbDepth[eye], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(g_rbDepth[eye], res);
    RbBarrier(list, g_rbDepth[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_rbDepthValid[eye] = true;
}

void XrRebuildSetMode(int mode) {
    g_rebuildMode = mode;
    Log("REBUILD mode %d", mode);
}

void* XrRebuildResult() { return g_rbOut; }

static void Rb3(float* out, const float* in) {
    out[0] = in[0];
    out[1] = in[1];
    out[2] = in[2];
    out[3] = 0;
}
static void RbEye(float* right, float* up, float* back, float* pos, float* tan, const RebuildEye& e) {
    Rb3(right, e.right);
    Rb3(up, e.up);
    Rb3(back, e.back);
    if (pos) Rb3(pos, e.pos);
    memcpy(tan, e.tan, 16);
}

// Copies the drawn eye into its history, then rebuilds the other eye into g_rbOut, left in COPY_SOURCE. False: nothing rebuilt.
static bool RbRun(ID3D12GraphicsCommandList* list, int source, const RebuildEye eyes[2], ID3D12Resource* srcColor, D3D12_RESOURCE_STATES srcState, float nearZ) {
    if (g_rbStage.load() < 2 || !RbPipelines()) return false;
    std::lock_guard<std::mutex> l(g_rbLock);
    if (!g_rbDepthValid[source] || !g_rbDepth[0] || !g_rbDepth[1] || !g_rbMap) return false;
    static bool made = false;
    if (!made) {
        made = true;
        for (int e = 0; e < 2; ++e) g_rbHist[e] = RbTexture(g_chainW, g_chainH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_rbOut = RbTexture(g_chainW, g_chainH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    if (!g_rbHist[0] || !g_rbHist[1] || !g_rbOut) return false;
    // History of the drawn eye: the source of this rebuild, and a later hole fill for itself.
    RbBarrier(list, srcColor, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    RbBarrier(list, g_rbHist[source], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    d.pResource = g_rbHist[source];
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.pResource = srcColor;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_RESOURCE_DESC sd = srcColor->GetDesc();
    D3D12_BOX box{0, 0, 0, static_cast<UINT>(std::min(static_cast<UINT64>(g_chainW), sd.Width)), std::min(static_cast<UINT>(g_chainH), sd.Height), 1};
    list->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
    RbBarrier(list, g_rbHist[source], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    RbBarrier(list, srcColor, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
    if (g_rbStage.load() < 3) return false;
    const int dest = 1 - source;
    g_rbHeld[source] = eyes[source];
    RebuildConstants c{};
    RbEye(c.dRight, c.dUp, c.dBack, c.dPos, c.dTan, eyes[dest]);
    RbEye(c.sRight, c.sUp, c.sBack, c.sPos, c.sTan, eyes[source]);
    // Until this eye has an image of its own, fill from the drawn eye's.
    const bool haveHeld = g_rbHeld[dest].valid;
    RbEye(c.hRight, c.hUp, c.hBack, nullptr, c.hTan, haveHeld ? g_rbHeld[dest] : eyes[source]);
    c.misc[0] = nearZ;
    c.misc[1] = static_cast<float>(g_rbDepthW);
    c.misc[2] = static_cast<float>(g_rbDepthH);
    RbWriteDescriptors(source);
    if (!haveHeld) {
        // The other history holds nothing yet: rebuild it with this eye's image in both places.
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_rbHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (static_cast<SIZE_T>(source) * 5 + 1) * g_rbInc;
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        g_dev->CreateShaderResourceView(g_rbHist[source], &v, h);
    }
    ID3D12DescriptorHeap* heaps[1] = {g_rbHeap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_rbRoot);
    list->SetComputeRoot32BitConstants(0, sizeof(RebuildConstants) / 4, &c, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE table = g_rbHeap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<UINT64>(source) * 5 * g_rbInc;
    list->SetComputeRootDescriptorTable(1, table);
    const UINT mx = (g_rbDepthW + 15) / 16, my = (g_rbDepthH + 15) / 16;
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = g_rbMap;
    list->SetPipelineState(g_rbClear);
    list->Dispatch(mx, my, 1);
    list->ResourceBarrier(1, &uav);
    list->SetPipelineState(g_rbSplat);
    list->Dispatch(mx, my, 1);
    list->ResourceBarrier(1, &uav);
    RbBarrier(list, g_rbOut, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(g_rbResolve);
    list->Dispatch((g_chainW + 15) / 16, (g_chainH + 15) / 16, 1);
    RbBarrier(list, g_rbOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ++g_rbCount;
    return true;
}
