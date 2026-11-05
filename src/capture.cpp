// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence.
//
// D3D12 side: finds the game's direct queue and its Present, without loading d3d12.dll early, and copies eye images
// to files for the scripted stereo check. Everything starts from a real game texture, so it runs late, after boot.
#include <windows.h>
#include <mutex>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "capture.h"

extern void Log(const char* fmt, ...);

static ID3D12Device* g_device;
static std::atomic<ID3D12CommandQueue*> g_queue{nullptr};
static std::atomic<uint64_t> g_presents{0};
static std::atomic<bool> g_ready{false};

using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static ExecuteFn g_origExecute;
static PresentFn g_origPresent;
static Present1Fn g_origPresent1;

struct DumpJob {
    std::vector<ID3D12Resource*> resources;
    std::vector<uint32_t> states;
    std::vector<std::wstring> paths;
};
static std::atomic<DumpJob*> g_job{nullptr};

// ---- All the GPU work of this queue: a timestamp either side of each submission, game's and mod's. Lab probe. ----
// The stamps are small lists recorded once. 256 pairs in a ring, far more than frames in flight use before one comes round again.
static const UINT kStampPairs = 256;
static ID3D12QueryHeap* g_stampHeap;
static ID3D12Resource* g_stampRead;
static const UINT64* g_stampData;
static ID3D12GraphicsCommandList* g_stampLists[kStampPairs * 2];
static std::atomic<uint64_t> g_stampHead{0};
static uint64_t g_stampTail;
static UINT64 g_stampFreq;
static std::atomic<int> g_stampState{0};  // 0 not made, 1 ready, 2 failed
static bool StampSetup(ID3D12CommandQueue* q) {
    ID3D12Device* d = nullptr;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&d))) || !d) return false;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kStampPairs * 2;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = qd.Count * 8;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12CommandAllocator* alloc = nullptr;
    void* mapped = nullptr;
    if (FAILED(q->GetTimestampFrequency(&g_stampFreq)) || FAILED(d->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_stampHeap))) || FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_stampRead))) || FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) || FAILED(g_stampRead->Map(0, nullptr, &mapped))) return false;
    g_stampData = static_cast<const UINT64*>(mapped);
    for (UINT i = 0; i < kStampPairs * 2; ++i) {
        if (FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&g_stampLists[i])))) return false;
        g_stampLists[i]->EndQuery(g_stampHeap, D3D12_QUERY_TYPE_TIMESTAMP, i);
        g_stampLists[i]->ResolveQueryData(g_stampHeap, D3D12_QUERY_TYPE_TIMESTAMP, i, 1, g_stampRead, static_cast<UINT64>(i) * 8);
        g_stampLists[i]->Close();
    }
    return true;
}
std::atomic<bool> g_stampOn{false};  // off: the game's work does not come through this hook, so it only saw the mod's
// Busy GPU time in ms and submissions since the last call, and the time they span.
void CaptureGpuBusy(double* busyMs, double* spanMs, uint64_t* submits) {
    *busyMs = *spanMs = 0;
    *submits = 0;
    if (g_stampState.load() != 1 || !g_stampFreq) return;
    const uint64_t head = g_stampHead.load();
    // Only pairs surely finished, and not about to be overwritten.
    const uint64_t safe = head > 32 ? head - 32 : 0;
    uint64_t from = g_stampTail;
    if (safe > from + (kStampPairs - 40)) from = safe - (kStampPairs - 40);
    UINT64 busy = 0, first = 0, last = 0;
    for (uint64_t k = from; k < safe; ++k) {
        const UINT i = static_cast<UINT>(k % kStampPairs) * 2;
        const UINT64 a = g_stampData[i], b = g_stampData[i + 1];
        if (b < a) continue;
        busy += b - a;
        if (!first) first = a;
        last = b;
        ++*submits;
    }
    g_stampTail = safe;
    const double toMs = 1000.0 / static_cast<double>(g_stampFreq);
    *busyMs = static_cast<double>(busy) * toMs;
    *spanMs = last > first ? static_cast<double>(last - first) * toMs : 0;
}

static void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (!g_queue.load() && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        g_queue = q;
        Log("D3D game direct queue %p", q);
    }
    const bool stamp = q == g_queue.load() && g_stampOn.load();
    if (stamp && g_stampState.load() == 0) {
        static std::mutex setup;
        std::lock_guard<std::mutex> l(setup);
        if (g_stampState.load() == 0) {
            const bool ok = StampSetup(q);
            g_stampState = ok ? 1 : 2;
            Log("D3D GPU stamps %s", ok ? "ready" : "failed");
        }
    }
    if (stamp && g_stampState.load() == 1) {
        const UINT i = static_cast<UINT>(g_stampHead.fetch_add(1) % kStampPairs) * 2;
        ID3D12CommandList* before[1] = {g_stampLists[i]};
        ID3D12CommandList* after[1] = {g_stampLists[i + 1]};
        g_origExecute(q, 1, before);
        g_origExecute(q, n, lists);
        g_origExecute(q, 1, after);
        return;
    }
    g_origExecute(q, n, lists);
}

static void WriteBmp(const std::wstring& path, const uint8_t* data, UINT pitch, UINT w, UINT h) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        Log("DUMP could not open file, errno %d", errno);
        return;
    }
    const uint32_t row = w * 3 + (4 - (w * 3) % 4) % 4;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + row * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -static_cast<LONG>(h);
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> line(row);
    uint32_t alphaMax = 0;
    uint64_t alphaSum = 0;
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* s = data + y * pitch;
        for (UINT x = 0; x < w; ++x) {
            line[x * 3] = s[x * 4 + 2];
            line[x * 3 + 1] = s[x * 4 + 1];
            line[x * 3 + 2] = s[x * 4];
            alphaMax = s[x * 4 + 3] > alphaMax ? s[x * 4 + 3] : alphaMax;
            alphaSum += s[x * 4 + 3];
        }
        fwrite(line.data(), row, 1, f);
    }
    fclose(f);
    Log("DUMP %ux%u alpha max %u mean %.2f", w, h, alphaMax, static_cast<double>(alphaSum) / (static_cast<double>(w) * h));
}

// On the Present thread, after the game has queued this frame's work, so the copies land after both eyes' draws.
static void RunJob(DumpJob* job) {
    ID3D12CommandQueue* q = g_queue.load();
    if (!q) {
        Log("DUMP no queue yet");
        delete job;
        return;
    }
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    std::vector<ID3D12Resource*> buffers;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> prints(job->resources.size());
    for (size_t i = 0; i < job->resources.size(); ++i) {
        ID3D12Resource* r = job->resources[i];
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        UINT64 total = 0;
        g_device->GetCopyableFootprints(&d, 0, 1, 0, &prints[i], nullptr, nullptr, &total);
        Log("DUMP resource %p %llux%u format %d mips %u flags %x state %x", r, d.Width, d.Height, d.Format, d.MipLevels, d.Flags, job->states[i]);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* b = nullptr;
        g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&b));
        buffers.push_back(b);
        D3D12_RESOURCE_BARRIER bar{};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = r;
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(job->states[i]);
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        // A barrier from a state to itself removes the device.
        const bool needBarrier = bar.Transition.StateBefore != bar.Transition.StateAfter;
        if (needBarrier) list->ResourceBarrier(1, &bar);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = b;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = prints[i];
        src.pResource = r;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
        if (needBarrier) list->ResourceBarrier(1, &bar);
    }
    list->Close();
    ID3D12CommandList* lists[] = {list};
    g_origExecute(q, 1, lists);
    q->Signal(fence, 1);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, ev);
    const DWORD wait = WaitForSingleObject(ev, 3000);
    CloseHandle(ev);
    Log("DUMP wait %lu device removed reason %lx", wait, g_device->GetDeviceRemovedReason());
    for (size_t i = 0; i < buffers.size(); ++i) {
        void* p = nullptr;
        const HRESULT mapped = wait == WAIT_OBJECT_0 ? buffers[i]->Map(0, nullptr, &p) : E_FAIL;
        Log("DUMP map %lx pitch %u %ls", mapped, prints[i].Footprint.RowPitch, job->paths[i].c_str());
        if (SUCCEEDED(mapped)) {
            WriteBmp(job->paths[i], static_cast<uint8_t*>(p), prints[i].Footprint.RowPitch, prints[i].Footprint.Width, prints[i].Footprint.Height);
            buffers[i]->Unmap(0, nullptr);
        }
        buffers[i]->Release();
    }
    list->Release();
    alloc->Release();
    fence->Release();
    delete job;
    Log("DUMP done");
}

static std::atomic<void (*)(void*, unsigned, unsigned, int)> g_callback{nullptr};

static thread_local int t_depth = 0;

extern void SeqNote(char c);
static void OnPresent(IDXGISwapChain* chain) {
    ++g_presents;
    SeqNote('G');
    if (DumpJob* job = g_job.exchange(nullptr)) RunJob(job);
    auto fn = g_callback.load();
    if (!fn) return;
    // The picture about to be shown: the flat screen in the headset uses it.
    void* buffer = nullptr;
    unsigned w = 0, h = 0;
    int format = 0;
    IDXGISwapChain3* c3 = nullptr;
    ID3D12Resource* res = nullptr;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&c3)))) {
        if (SUCCEEDED(c3->GetBuffer(c3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&res)))) {
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            buffer = res;
            w = static_cast<unsigned>(d.Width);
            h = d.Height;
            format = d.Format;
        }
    }
    fn(buffer, w, h, format);
    if (res) res->Release();
    if (c3) c3->Release();
}

bool CaptureReady() { return g_ready.load(); }
void* CaptureDevice() { return g_device; }
void* CaptureQueue() { return g_queue.load(); }
void CaptureSetPresentCallback(void (*fn)(void*, unsigned, unsigned, int)) { g_callback = fn; }

static std::atomic<bool> g_noVsync{false};
void CaptureNoVsync(bool on) { g_noVsync = on; }

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* s, UINT a, UINT b) {
    if (g_noVsync) a = 0;
    if (t_depth++ == 0) OnPresent(s);
    const HRESULT r = g_origPresent(s, a, b);
    --t_depth;
    return r;
}

static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* s, UINT a, UINT b, const DXGI_PRESENT_PARAMETERS* p) {
    if (g_noVsync) a = 0;
    if (t_depth++ == 0) OnPresent(s);
    const HRESULT r = g_origPresent1(s, a, b, p);
    --t_depth;
    return r;
}

static DWORD WINAPI SetupThread(void* resource) {
    auto* res = static_cast<ID3D12Resource*>(resource);
    if (FAILED(res->GetDevice(IID_PPV_ARGS(&g_device)))) {
        Log("D3D no device");
        return 0;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* tmp = nullptr;
    if (FAILED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&tmp)))) {
        Log("D3D no queue");
        return 0;
    }
    void** vt = *reinterpret_cast<void***>(tmp);
    MH_CreateHook(vt[10], reinterpret_cast<void*>(&HookExecute), reinterpret_cast<void**>(&g_origExecute));
    Log("D3D ExecuteCommandLists hook %s", MH_StatusToString(MH_EnableHook(vt[10])));
    tmp->Release();
    for (int i = 0; i < 200 && !g_queue.load(); ++i) Sleep(10);
    ID3D12CommandQueue* q = g_queue.load();
    if (!q) {
        Log("D3D game queue not seen");
        return 0;
    }
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    using CreateFactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
    IDXGIFactory4* factory = nullptr;
    if (!create || FAILED(create(0, IID_PPV_ARGS(&factory)))) {
        Log("D3D no factory");
        return 0;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"RiftApartVRDummy";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(wc.lpszClassName, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, nullptr, nullptr);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 16;
    sd.Height = 16;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* chain = nullptr;
    const HRESULT hr = factory->CreateSwapChainForHwnd(q, wnd, &sd, nullptr, nullptr, &chain);
    if (FAILED(hr)) {
        Log("D3D dummy swapchain failed %lx", hr);
        return 0;
    }
    void** sv = *reinterpret_cast<void***>(chain);
    MH_CreateHook(sv[8], reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_origPresent));
    MH_CreateHook(sv[22], reinterpret_cast<void*>(&HookPresent1), reinterpret_cast<void**>(&g_origPresent1));
    Log("D3D Present hooks %s %s", MH_StatusToString(MH_EnableHook(sv[8])), MH_StatusToString(MH_EnableHook(sv[22])));
    chain->Release();
    factory->Release();
    DestroyWindow(wnd);
    Sleep(500);
    Log("D3D presents seen %llu", g_presents.load());
    g_ready = true;
    return 0;
}

void CaptureSetup(void* anyGameResource) {
    if (HANDLE t = CreateThread(nullptr, 0, SetupThread, anyGameResource, 0, nullptr)) CloseHandle(t);
}

bool CaptureRequest(void* const* resources, const uint32_t* states, const wchar_t* const* paths, int count) {
    if (!g_origPresent) return false;
    auto* job = new DumpJob;
    for (int i = 0; i < count; ++i) {
        job->resources.push_back(static_cast<ID3D12Resource*>(resources[i]));
        job->states.push_back(states[i]);
        job->paths.emplace_back(paths[i]);
    }
    if (DumpJob* old = g_job.exchange(job)) delete old;
    return true;
}
