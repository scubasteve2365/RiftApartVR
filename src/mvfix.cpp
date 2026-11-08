// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence. Written for this mod, no outside shader code.
//
// Motion vectors for DLSS in alternate eye (1.06). Each eye is drawn every other tick, so DLSS's history for an eye is two
// ticks old. The game's motion vectors take the camera part from that eye's own previous camera (two ticks), but things that
// move by themselves carry only one tick of their movement (their previous position is kept per object, per game tick, not
// per view): measured on RigB with the camera still, moving texels' vectors averaged 2.2 to 3.1 pixels in alternate eye
// against 2.6 to 3.2 in native, where twice native was expected. DLSS is then told moving things moved half as far as they
// did since its history, and leaves a trail on them (Ratchet, enemies, traffic).
//
// The fix, right before each eye's DLSS call on the game's own list: the camera part is what the game's clipToPrevClip
// matrix (from its DLSS constants) gives for the pixel's depth; the rest is the object's own movement, which is scaled by
// the ratio of the eye's history age to the object vectors' span (2 in alternate eye). DLSS gets a texture of the corrected
// vectors in place of the game's. DLSS sets its own command list state when it runs, so this pass setting its own first is
// covered by the game restoring its state after DLSS, as it must anyway.
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include "mvfix.h"

extern void Log(const char* fmt, ...);

namespace {

const char kHlsl[] = R"(
cbuffer C : register(b0) {
    float4x4 c2p;    // clip to previous clip, as the game gave DLSS
    float4 p;        // width, height, motion vector scale x, y (to image units)
    float4 q;        // object factor, matrix convention (0 row vector, 1 column vector), stats slot, stats on
};
Texture2D<float> depthTex : register(t0);
Texture2D<float2> mvTex : register(t1);
RWTexture2D<float2> outMv : register(u0);
RWByteAddressBuffer stats : register(u1);
groupshared uint gsN, gsObj, gsCam;

[numthreads(16, 16, 1)] void Fix(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) { gsN = 0; gsObj = 0; gsCam = 0; }
    GroupMemoryBarrierWithGroupSync();
    if (id.x < (uint)p.x && id.y < (uint)p.y) {
        float2 uv = (id.xy + 0.5) / p.xy;
        float d = depthTex[id.xy];
        float4 clip = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
        float4 pc = q.y >= 0.5 ? mul(c2p, clip) : mul(clip, c2p);
        pc /= pc.w;
        float2 puv = float2(pc.x * 0.5 + 0.5, 0.5 - pc.y * 0.5);
        float2 cam = (puv - uv) / p.zw;           // the camera's part, in the game's vector units
        float2 m = mvTex[id.xy];
        float2 obj = m - cam;
        outMv[id.xy] = cam + obj * q.x;
        if (q.w >= 0.5) {
            float lo = length(obj);
            if (lo > 0.5) { InterlockedAdd(gsN, 1); InterlockedAdd(gsObj, (uint)(min(lo, 4000.0) * 16)); }
            InterlockedAdd(gsCam, (uint)(min(length(cam), 4000.0) * 4));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (q.w >= 0.5 && gi == 0) {
        uint o;
        stats.InterlockedAdd((uint)q.z * 16, gsN, o);
        stats.InterlockedAdd((uint)q.z * 16 + 4, gsObj, o);
        stats.InterlockedAdd((uint)q.z * 16 + 8, gsCam, o);
    }
}
)";

struct Consts {
    float c2p[16];
    float p[4];
    float q[4];
};

ID3D12Device* g_dev = nullptr;
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso = nullptr;
ID3D12DescriptorHeap* g_heap = nullptr;
UINT g_inc = 0;
bool g_failed = false;
ID3D12Resource* g_out[3] = {};
UINT g_outW[3] = {}, g_outH[3] = {};
ID3D12Resource* g_stats = nullptr, *g_statsRead = nullptr, *g_zero = nullptr;
const UINT kSlots = 32;
UINT g_slotNext = 0;
bool g_slotPending[kSlots] = {};
std::mutex g_lock;
float g_c2p[3][16];
float g_scale[3][2];
bool g_haveMatrix[3] = {};
std::atomic<int> g_mode{2};       // 0 off, 1 statistics only, 2 fix alternate eye (default from 1.06)
std::atomic<int> g_convention{1};  // column vector: verified on RigB, 0.5 percent of pixels left moving against 85
std::atomic<float> g_factor{2.f};
uint64_t g_n = 0, g_obj = 0, g_cam = 0, g_calls = 0;

bool Setup(ID3D12Device* dev) {
    if (g_pso) return true;
    if (g_failed) return false;
    g_failed = true;
    g_dev = dev;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    const HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = dc ? reinterpret_cast<CompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    const HMODULE d12 = GetModuleHandleW(L"d3d12.dll");
    const auto serialize = d12 ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!compile || !serialize) return false;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = sizeof(Consts) / 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    ID3DBlob* blob = nullptr, *err = nullptr;
    if (FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) || FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_root)))) {
        Log("MVFIX root signature failed");
        return false;
    }
    ID3DBlob* cs = nullptr, *e = nullptr;
    if (FAILED(compile(kHlsl, sizeof(kHlsl) - 1, "mvfix", nullptr, nullptr, "Fix", "cs_5_1", 0, 0, &cs, &e))) {
        Log("MVFIX shader: %s", e ? static_cast<const char*>(e->GetBufferPointer()) : "");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_root;
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 3 * 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kSlots * 16;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES dh{D3D12_HEAP_TYPE_DEFAULT}, rh{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC ud = bd;
    ud.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_stats));
    dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_statsRead));
    D3D12_HEAP_PROPERTIES uh{D3D12_HEAP_TYPE_UPLOAD};
    dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_zero));
    void* z = nullptr;
    if (g_zero && SUCCEEDED(g_zero->Map(0, nullptr, &z))) {
        memset(z, 0, kSlots * 16);
        g_zero->Unmap(0, nullptr);
    }
    if (!g_stats || !g_statsRead || !g_zero) return false;
    g_failed = false;
    Log("MVFIX ready");
    return true;
}

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    l->ResourceBarrier(1, &x);
}

}  // namespace

void MvFixSetMatrix(int vp, const float* clipToPrevClip, float scaleX, float scaleY) {
    if (vp < 1 || vp > 2 || !clipToPrevClip) return;
    std::lock_guard<std::mutex> l(g_lock);
    memcpy(g_c2p[vp], clipToPrevClip, sizeof(g_c2p[vp]));
    g_scale[vp][0] = scaleX;
    g_scale[vp][1] = scaleY;
    g_haveMatrix[vp] = true;
}

void MvFixSetMode(int mode, float factor, int convention) {
    g_mode = mode;
    g_factor = factor;
    g_convention = convention;
    Log("MVFIX mode %d (0 off, 1 statistics, 2 fix in alternate eye), object factor %.2f, convention %d", mode, factor, convention);
}

int MvFixMode() { return g_mode.load(); }

void* MvFixRun(int vp, void* cmd, void* depthRes, void* mvRes, uint32_t mvState, bool alternate) {
    const int mode = g_mode.load();
    if (mode == 0 || vp < 1 || vp > 2 || !cmd || !depthRes || !mvRes) return nullptr;
    if (mode == 2 && !alternate) return nullptr;  // native: the game's vectors are right, nothing to do
    auto* list = static_cast<ID3D12GraphicsCommandList*>(cmd);
    auto* depth = static_cast<ID3D12Resource*>(depthRes);
    auto* mv = static_cast<ID3D12Resource*>(mvRes);
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_haveMatrix[vp]) return nullptr;
    if (!g_dev) {
        ID3D12Device* dev = nullptr;
        if (FAILED(depth->GetDevice(IID_PPV_ARGS(&dev)))) return nullptr;
        g_dev = dev;
    }
    if (!Setup(g_dev)) return nullptr;
    const D3D12_RESOURCE_DESC md = mv->GetDesc(), dd = depth->GetDesc();
    if (md.Width != dd.Width || md.Height != dd.Height || md.Width < 16) return nullptr;
    if (!g_out[vp] || g_outW[vp] != md.Width || g_outH[vp] != md.Height) {
        D3D12_HEAP_PROPERTIES dh{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC od{};
        od.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        od.Width = md.Width;
        od.Height = md.Height;
        od.DepthOrArraySize = 1;
        od.MipLevels = 1;
        od.Format = DXGI_FORMAT_R16G16_FLOAT;
        od.SampleDesc.Count = 1;
        od.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* o = nullptr;
        if (FAILED(g_dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &od, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&o)))) return nullptr;
        g_out[vp] = o;  // an old one may still be in flight: left to the process
        g_outW[vp] = static_cast<UINT>(md.Width);
        g_outH[vp] = md.Height;
        Log("MVFIX output %llux%u for viewport %d", md.Width, md.Height, vp);
    }
    // Statistics: read the slot written kSlots calls ago, then reuse it.
    const UINT slot = g_slotNext++ % kSlots;
    if (g_slotPending[slot]) {
        uint32_t* rp = nullptr;
        D3D12_RANGE rr{slot * 16, slot * 16 + 12};
        if (SUCCEEDED(g_statsRead->Map(0, &rr, reinterpret_cast<void**>(&rp)))) {
            g_n += rp[slot * 4];
            g_obj += rp[slot * 4 + 1];
            g_cam += rp[slot * 4 + 2];
            D3D12_RANGE none{0, 0};
            g_statsRead->Unmap(0, &none);
        }
        if (++g_calls % 400 == 0) {
            const double px = static_cast<double>(g_outW[vp]) * g_outH[vp] * 400;
            Log("MVFIX %llu calls: object movement over 0.5 px on %.2f%% of pixels, mean %.2f px there; camera part mean %.2f px",
                g_calls, g_n / px * 100, g_n ? g_obj / 16.0 / g_n : 0.0, g_cam / 4.0 / px);
            g_n = g_obj = g_cam = 0;
        }
    }
    g_slotPending[slot] = true;
    // Descriptors: depth, game vectors, output, stats.
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(vp) * 4 * g_inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = dd.Format == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : dd.Format == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_R32_FLOAT : dd.Format;
    g_dev->CreateShaderResourceView(depth, &sd, h);
    h.ptr += g_inc;
    sd.Format = DXGI_FORMAT_R16G16_FLOAT;
    g_dev->CreateShaderResourceView(mv, &sd, h);
    h.ptr += g_inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format = DXGI_FORMAT_R16G16_FLOAT;
    g_dev->CreateUnorderedAccessView(g_out[vp], nullptr, &ud, h);
    h.ptr += g_inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC bu{};
    bu.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    bu.Format = DXGI_FORMAT_R32_TYPELESS;
    bu.Buffer.NumElements = kSlots * 4;
    bu.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g_dev->CreateUnorderedAccessView(g_stats, nullptr, &bu, h);
    Consts c{};
    memcpy(c.c2p, g_c2p[vp], sizeof(c.c2p));
    c.p[0] = static_cast<float>(md.Width);
    c.p[1] = static_cast<float>(md.Height);
    c.p[2] = g_scale[vp][0];
    c.p[3] = g_scale[vp][1];
    const bool fix = mode >= 2 && alternate;
    c.q[0] = fix ? g_factor.load() : 1.f;
    c.q[1] = static_cast<float>(g_convention.load());
    c.q[2] = static_cast<float>(slot);
    c.q[3] = 1.f;
    // The game's inputs are readable at its DLSS call (depth in GENERIC_READ). Vectors: as tagged.
    const auto mvSt = static_cast<D3D12_RESOURCE_STATES>(mvState);
    const bool mvMove = (mvSt & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0;
    if (mvMove) Barrier(list, mv, mvSt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, g_out[vp], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The shader adds into this slot, so zero it first from a zeroed upload buffer.
    Barrier(list, g_stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(g_stats, slot * 16, g_zero, 0, 16);
    Barrier(list, g_stats, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[1] = {g_heap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_root);
    list->SetPipelineState(g_pso);
    list->SetComputeRoot32BitConstants(0, sizeof(Consts) / 4, &c, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE t = g_heap->GetGPUDescriptorHandleForHeapStart();
    t.ptr += static_cast<UINT64>(vp) * 4 * g_inc;
    list->SetComputeRootDescriptorTable(1, t);
    list->Dispatch((static_cast<UINT>(md.Width) + 15) / 16, (md.Height + 15) / 16, 1);
    Barrier(list, g_out[vp], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (mvMove) Barrier(list, mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mvSt);
    Barrier(list, g_stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(g_statsRead, slot * 16, g_stats, slot * 16, 12);
    Barrier(list, g_stats, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return fix ? g_out[vp] : nullptr;
}
