// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence.
//
// OpenXR on the game's own D3D12 device and direct queue. A worker thread owns xrWaitFrame, so the game never blocks
// in OpenXR. The game thread reads the newest head pose once a frame. The Present thread copies the two finished eye
// images to the swapchains and ends the frame. Errors are logged and leave the flat game running.
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xr.h"
#include "capture.h"

extern void Log(const char* fmt, ...);

static PFN_xrGetInstanceProcAddr g_gipa;
static XrInstance g_inst = XR_NULL_HANDLE;
static XrSystemId g_sys = 0;
static XrSession g_sess = XR_NULL_HANDLE;
static XrSpace g_space = XR_NULL_HANDLE;
static XrSwapchain g_chain[2];
static std::vector<XrSwapchainImageD3D12KHR> g_images[2];
static int g_chainW, g_chainH;
static std::atomic<int> g_recW{0}, g_recH{0};
static std::atomic<XrTime> g_predicted{0};
static std::atomic<int> g_state{XR_SESSION_STATE_UNKNOWN};
static std::atomic<int64_t> g_periodNs{0};

#define XRFN(name) static PFN_##name p_##name;
XRFN(xrEnumerateInstanceExtensionProperties)
XRFN(xrCreateInstance)
XRFN(xrGetSystem)
XRFN(xrGetSystemProperties)
XRFN(xrEnumerateViewConfigurationViews)
XRFN(xrCreateSession)
XRFN(xrCreateReferenceSpace)
XRFN(xrPollEvent)
XRFN(xrBeginSession)
XRFN(xrEndSession)
XRFN(xrWaitFrame)
XRFN(xrBeginFrame)
XRFN(xrEndFrame)
XRFN(xrLocateViews)
XRFN(xrEnumerateSwapchainFormats)
XRFN(xrCreateSwapchain)
XRFN(xrDestroySwapchain)
XRFN(xrEnumerateSwapchainImages)
XRFN(xrAcquireSwapchainImage)
XRFN(xrWaitSwapchainImage)
XRFN(xrReleaseSwapchainImage)
XRFN(xrGetD3D12GraphicsRequirementsKHR)

template <class T>
static bool Fn(const char* name, T& out) {
    PFN_xrVoidFunction f = nullptr;
    const XrResult r = g_gipa(g_inst, name, &f);
    out = reinterpret_cast<T>(f);
    if (XR_FAILED(r) || !f) {
        Log("XR no function %s: %d", name, static_cast<int>(r));
        return false;
    }
    return true;
}

bool XrInit(const wchar_t* dir) {
    if (g_inst) return true;
    {
        // OFXR frame generation, off unless chosen. An OpenXR layer in its own folder, turned on for this process only, the
        // way a launcher would, with no tray program. GPL-3.0, so it stays a separate program in its own folder, with its licence and source link.
        const std::wstring ini = std::wstring(dir) + L"RiftApartVR.ini";
        const std::wstring folder = std::wstring(dir) + L"ofxr";
        const std::wstring manifest = folder + L"\\XR_APILAYER_OFXRBridge.json";
        const int fgMode = static_cast<int>(GetPrivateProfileIntW(L"VR", L"FrameGen", 0, ini.c_str()));
        // FrameGenAlwaysLoad: load OFXR at every launch, idle (enabled=0) while frame generation is Off, so turning it on later
        // in the menu works at once instead of at the next launch. OFXR can only join at the start.
        const bool alwaysLoad = GetPrivateProfileIntW(L"VR", L"FrameGenAlwaysLoad", 1, ini.c_str()) != 0;
        if (fgMode != 0 || alwaysLoad) {
            if (GetFileAttributesW(manifest.c_str()) != INVALID_FILE_ATTRIBUTES) {
                // The menu's choices, as OFXR reads them at its start. pacing_mode 0: its director hangs at this game's title on
                // RigB. lower_vram 1: the HUD layer keeps its real image, with no optical flow run over it.
                const int preset = static_cast<int>(GetPrivateProfileIntW(L"VR", L"FrameGenQuality", 1, ini.c_str()));
                const int scale = static_cast<int>(GetPrivateProfileIntW(L"VR", L"FrameGenDetail", 2, ini.c_str()));
                const int diagnostics = static_cast<int>(GetPrivateProfileIntW(L"VR", L"FrameGenLog", 0, ini.c_str()));
                const int fgType = static_cast<int>(GetPrivateProfileIntW(L"VR", L"FrameGenType", 0, ini.c_str()));
                const bool triple = fgMode == 2 && (fgType == 0 || fgType == 2);
                // FSR and DLSS-G run on the game's own copies of AMD's and NVIDIA's files, so the mod ships neither.
                CopyFileW((std::wstring(dir) + L"amd_fidelityfx_dx12.dll").c_str(), (folder + L"\\ofxr_amd_fidelityfx_framegeneration_dx12.dll").c_str(), TRUE);
                CopyFileW((std::wstring(dir) + L"nvngx_dlssg.dll").c_str(), (folder + L"\\nvngx_dlssg.dll").c_str(), TRUE);
                char profile[400];
                sprintf_s(profile, "[ofxr]\r\nenabled=%d\r\nframe_generation=%d\r\nbackend=%d\r\nmotion_vectors=0\r\noverlay_position=0\r\nlower_vram=1\r\npacing_mode=0\r\npreset=%d\r\nscale=%d\r\ntriple_frame_gen=%d\r\ndiagnostics=%d\r\n", fgMode != 0 ? 1 : 0, fgType, triple ? 3 : 2, preset, scale, triple ? 1 : 0, diagnostics);
                const std::wstring profilePath = folder + L"\\settings\\Profiles\\RiftApart.exe.ini";
                if (FILE* f = _wfopen(profilePath.c_str(), L"wb")) {
                    fputs(profile, f);
                    fclose(f);
                }
                SetEnvironmentVariableW(L"XR_API_LAYER_PATH", folder.c_str());
                SetEnvironmentVariableW(L"XR_ENABLE_API_LAYERS", L"XR_APILAYER_OFXRBridge");
                // Its settings stay in its own folder, apart from a tray install's.
                SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR", (folder + L"\\settings").c_str());
                Log("XR frame generation layer loaded, mode %d type %d (%s)", fgMode, fgType, fgMode != 0 ? "on" : "idle until turned on");
            } else {
                Log("XR frame generation chosen, but the ofxr folder has no layer: running without it");
            }
        } else {
            Log("XR frame generation off");
        }
    }
    const std::wstring path = std::wstring(dir) + L"openxr_loader.dll";
    HMODULE lib = LoadLibraryW(path.c_str());
    if (!lib) {
        Log("XR loader not loaded, error %lu", GetLastError());
        return false;
    }
    g_gipa = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(lib, "xrGetInstanceProcAddr"));
    if (!g_gipa || !Fn("xrCreateInstance", p_xrCreateInstance)) return false;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "RiftApartVR");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    const char* ext[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = ext;
    XrResult r = p_xrCreateInstance(&ci, &g_inst);
    if (XR_FAILED(r)) {
        Log("XR xrCreateInstance failed: %d", static_cast<int>(r));
        return false;
    }
    if (!Fn("xrGetSystem", p_xrGetSystem) || !Fn("xrGetSystemProperties", p_xrGetSystemProperties) || !Fn("xrEnumerateViewConfigurationViews", p_xrEnumerateViewConfigurationViews) ||
        !Fn("xrCreateSession", p_xrCreateSession) || !Fn("xrCreateReferenceSpace", p_xrCreateReferenceSpace) || !Fn("xrPollEvent", p_xrPollEvent) ||
        !Fn("xrBeginSession", p_xrBeginSession) || !Fn("xrEndSession", p_xrEndSession) || !Fn("xrWaitFrame", p_xrWaitFrame) || !Fn("xrBeginFrame", p_xrBeginFrame) ||
        !Fn("xrEndFrame", p_xrEndFrame) || !Fn("xrLocateViews", p_xrLocateViews) || !Fn("xrEnumerateSwapchainFormats", p_xrEnumerateSwapchainFormats) ||
        !Fn("xrCreateSwapchain", p_xrCreateSwapchain) || !Fn("xrDestroySwapchain", p_xrDestroySwapchain) || !Fn("xrEnumerateSwapchainImages", p_xrEnumerateSwapchainImages) || !Fn("xrAcquireSwapchainImage", p_xrAcquireSwapchainImage) ||
        !Fn("xrWaitSwapchainImage", p_xrWaitSwapchainImage) || !Fn("xrReleaseSwapchainImage", p_xrReleaseSwapchainImage) || !Fn("xrGetD3D12GraphicsRequirementsKHR", p_xrGetD3D12GraphicsRequirementsKHR))
        return false;
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = p_xrGetSystem(g_inst, &si, &g_sys);
    if (XR_FAILED(r)) {
        Log("XR xrGetSystem failed: %d (is a headset on?)", static_cast<int>(r));
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    p_xrGetSystemProperties(g_inst, g_sys, &sp);
    uint32_t n = 0;
    p_xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    std::vector<XrViewConfigurationView> v(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    p_xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, v.data());
    if (n < 2) return false;
    g_recW = v[0].recommendedImageRectWidth;
    g_recH = v[0].recommendedImageRectHeight;
    Log("XR system '%s', recommended %ux%u per eye, max %ux%u", sp.systemName, v[0].recommendedImageRectWidth, v[0].recommendedImageRectHeight, v[0].maxImageRectWidth, v[0].maxImageRectHeight);
    return true;
}

void XrRecommended(int* w, int* h) {
    *w = g_recW;
    *h = g_recH;
}

// ---- GPU copies on the game's queue ----
static ID3D12Device* g_dev;
static ID3D12CommandQueue* g_queue;
static const int kUiSlots = 3;
struct UiChain {
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    int w = 0, h = 0;
};
static UiChain g_ui[kUiSlots];
static int64_t g_format;
static std::atomic<float> g_hudWidth{2.2f}, g_hudDistance{2.0f}, g_hudDrop{0.1f};
static float g_hudYaw;
static float g_hudPos[3];
static bool g_hudPlaced;

void XrSetHud(float width, float distance, float drop) {
    g_hudWidth = width;
    g_hudDistance = distance;
    g_hudDrop = drop;
}

static const int kRing = 4;
static ID3D12CommandAllocator* g_alloc[kRing];
static ID3D12GraphicsCommandList* g_list[kRing];
static ID3D12Fence* g_fence;
static uint64_t g_fenceValue, g_ringValue[kRing];
static int g_ringIndex;
static HANDLE g_fenceEvent;

// ---- frame hand-over between threads ----
static std::mutex g_frameLock;
static bool g_open;
static bool g_should;
static XrTime g_openTime;
static std::atomic<uint64_t> g_endedWithImages{0}, g_endedEmpty{0}, g_notReady{0}, g_endedScreen{0};
static std::atomic<uint64_t> g_usWaitImage{0}, g_usEnd{0}, g_usPresent{0}, g_usWaitFrame{0};
static uint64_t NowUs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart * 1000000 / f.QuadPart);
}

// Each game frame's head pose, kept until the image drawn from it reaches Present. The layer must carry the pose the
// image was drawn with, or the runtime reprojects by the difference and the world swims.
struct RebuildEye {
    float right[3], up[3], back[3], pos[3];
    float tan[4];  // left, right, up, down
    bool valid;
};
struct PendingPose {
    XrView views[2];
    RebuildEye cameras[2];
    float nearZ;
    uint64_t serial;
    float worldPos[2][3];
};
static const int kPoses = 16;
static PendingPose g_poses[kPoses];
static uint64_t g_poseSerial;
static uint64_t g_renderedSerial, g_usedSerial;
static std::mutex g_poseLock;
static std::atomic<uint64_t> g_unmatched{0}, g_ageSum{0};

static std::atomic<bool> g_alternate{false};
static int g_renderedEye = 0;   // g_poseLock
static bool g_held[2];          // g_poseLock: the eye has an image in its swapchain, drawn at g_heldView
static XrView g_heldView[2];
void XrSetAlternate(bool on) {
    std::lock_guard<std::mutex> l(g_poseLock);
    if (g_alternate.exchange(on) != on) g_held[0] = g_held[1] = false;
}

void XrTagEyeCamera(int eye, const float right[3], const float up[3], const float back[3], const float pos[3], float tanLeft, float tanRight, float tanUp, float tanDown, float nearZ) {
    std::lock_guard<std::mutex> l(g_poseLock);
    PendingPose& p = g_poses[g_poseSerial % kPoses];
    RebuildEye& c = p.cameras[eye];
    memcpy(c.right, right, 12);
    memcpy(c.up, up, 12);
    memcpy(c.back, back, 12);
    memcpy(c.pos, pos, 12);
    c.tan[0] = tanLeft;
    c.tan[1] = tanRight;
    c.tan[2] = tanUp;
    c.tan[3] = tanDown;
    c.valid = true;
    p.nearZ = nearZ;
}

void XrTagPose(int eye, const float worldPos[3]) {
    std::lock_guard<std::mutex> l(g_poseLock);
    PendingPose& p = g_poses[g_poseSerial % kPoses];
    memcpy(p.worldPos[eye], worldPos, 12);
}

void XrMarkRendered(int eye, const float worldPos[3]) {
    std::lock_guard<std::mutex> l(g_poseLock);
    for (uint64_t s = g_poseSerial; s > 0 && s + kPoses > g_poseSerial; --s) {
        const PendingPose& p = g_poses[s % kPoses];
        if (p.serial == s && memcmp(p.worldPos[eye], worldPos, 12) == 0) {
            if (s > g_renderedSerial) {
                g_renderedSerial = s;
                g_renderedEye = eye;
            }
            return;
        }
    }
    ++g_unmatched;
}

static std::mutex g_fakeLock;
static bool g_fakeHeadOn = false, g_fakeLensOn = false;
static XrQuaternionf g_fakeQ{0, 0, 0, 1};
static XrVector3f g_fakeP{0, 0, 0}, g_fakeSide{0.032f, 0, 0};
static float g_fakeLens[4] = {-1, 1, 1, -1};

void XrFakeHead(bool on, float yaw, float pitch, float roll, float x, float y, float z, float ipd) {
    const float k = 3.14159265f / 360.0f;
    const float cy = cosf(yaw * k), sy = sinf(yaw * k), cp = cosf(pitch * k), sp = sinf(pitch * k), cr = cosf(roll * k), sr = sinf(roll * k);
    // Yaw about Y, then pitch about X, then roll about Z.
    const float qx = cy * sp * cr + sy * cp * sr, qy = sy * cp * cr - cy * sp * sr, qz = cy * cp * sr - sy * sp * cr, qw = cy * cp * cr + sy * sp * sr;
    std::lock_guard<std::mutex> l(g_fakeLock);
    g_fakeHeadOn = on;
    g_fakeQ = {qx, qy, qz, qw};
    g_fakeP = {x, y, z};
    const float h = ipd * 0.5f;
    g_fakeSide = {(1 - 2 * (qy * qy + qz * qz)) * h, 2 * (qx * qy + qw * qz) * h, 2 * (qx * qz - qw * qy) * h};
    Log("FAKE head %d yaw %.2f pitch %.2f roll %.2f pos %.3f %.3f %.3f ipd %.3f", on ? 1 : 0, yaw, pitch, roll, x, y, z, ipd);
}

void XrFakeLens(bool on, float left, float right, float up, float down) {
    std::lock_guard<std::mutex> l(g_fakeLock);
    g_fakeLensOn = on;
    g_fakeLens[0] = left;
    g_fakeLens[1] = right;
    g_fakeLens[2] = up;
    g_fakeLens[3] = down;
    Log("FAKE lens %d L %.3f R %.3f U %.3f D %.3f", on ? 1 : 0, left, right, up, down);
}

static void ApplyFake(XrView v[2]) {
    std::lock_guard<std::mutex> l(g_fakeLock);
    if (g_fakeHeadOn) {
        for (int e = 0; e < 2; ++e) {
            const float s = e == 0 ? -1.0f : 1.0f;
            v[e].pose.orientation = g_fakeQ;
            v[e].pose.position = {g_fakeP.x + s * g_fakeSide.x, g_fakeP.y + s * g_fakeSide.y, g_fakeP.z + s * g_fakeSide.z};
        }
    }
    if (g_fakeLensOn) {
        v[0].fov = {atanf(g_fakeLens[0]), atanf(g_fakeLens[1]), atanf(g_fakeLens[2]), atanf(g_fakeLens[3])};
        v[1].fov = {atanf(-g_fakeLens[1]), atanf(-g_fakeLens[0]), atanf(g_fakeLens[2]), atanf(g_fakeLens[3])};
    }
}

bool XrLocate(XrEyePose out[2]) {
    const XrTime t = g_predicted;
    if (!g_sess || !t) return false;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t;
    li.space = g_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    XrView v[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    if (XR_FAILED(p_xrLocateViews(g_sess, &li, &vs, 2, &n, v)) || n != 2) return false;
    if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return false;
    ApplyFake(v);
    for (int e = 0; e < 2; ++e) {
        out[e].qx = v[e].pose.orientation.x;
        out[e].qy = v[e].pose.orientation.y;
        out[e].qz = v[e].pose.orientation.z;
        out[e].qw = v[e].pose.orientation.w;
        out[e].px = v[e].pose.position.x;
        out[e].py = v[e].pose.position.y;
        out[e].pz = v[e].pose.position.z;
        out[e].tanLeft = tanf(v[e].fov.angleLeft);
        out[e].tanRight = tanf(v[e].fov.angleRight);
        out[e].tanUp = tanf(v[e].fov.angleUp);
        out[e].tanDown = tanf(v[e].fov.angleDown);
    }
    std::lock_guard<std::mutex> l(g_poseLock);
    ++g_poseSerial;
    PendingPose& p = g_poses[g_poseSerial % kPoses];
    p.views[0] = v[0];
    p.views[1] = v[1];
    p.cameras[0].valid = p.cameras[1].valid = false;
    p.serial = g_poseSerial;
    return true;
}

static HANDLE FrameEvent() {
    static HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return e;
}

void XrPaceWait() {
    static XrTime seen = 0;
    const int64_t period = g_periodNs;
    if (!period) return;
    const ULONGLONG limitMs = static_cast<ULONGLONG>(period * 3 / 2 / 1000000) + 1;
    const ULONGLONG start = GetTickCount64();
    // Sleeps until the frame thread has a new frame. A polling loop here used about two whole CPU cores.
    while (g_predicted.load() == seen && GetTickCount64() - start < limitMs) WaitForSingleObject(FrameEvent(), 2);
    seen = g_predicted.load();
}

// Set when the runtime moves the seated (LOCAL) space, as SteamVR's recenter does. The game camera recenters from it too.
static std::atomic<bool> g_spaceRecentered{false};
bool XrTakeSpaceRecenter() { return g_spaceRecentered.exchange(false); }
void XrRecenterScreen();

static void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (p_xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const auto& c = *reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
            Log("XR reference space %d recentered by the runtime", static_cast<int>(c.referenceSpaceType));
            if (c.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                XrRecenterScreen();
                g_spaceRecentered = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& s = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            g_state = s.state;
            Log("XR session state %d", static_cast<int>(s.state));
            if (s.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult r = p_xrBeginSession(g_sess, &bi);
                Log("XR xrBeginSession %d", static_cast<int>(r));
            } else if (s.state == XR_SESSION_STATE_STOPPING) {
                p_xrEndSession(g_sess);
            }
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

static void EndEmpty(XrTime t) {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = t;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    p_xrEndFrame(g_sess, &ei);
    ++g_endedEmpty;
}

// Overlap: xrWaitFrame for the next frame comes right after xrBeginFrame of this one, which OpenXR allows, so the game starts its next
// tick while the render thread is still finishing this one. Its xrBeginFrame still waits for this frame's xrEndFrame.
// Off: the next xrWaitFrame waits for xrEndFrame, so the game's work and the render thread's take turns.
static std::atomic<bool> g_overlap{false};
void XrSetOverlap(bool on) {
    g_overlap = on;
    Log("XR overlap %d", on ? 1 : 0);
}
// Option: the render thread records and submits the copies, then leaves xrEndFrame to the worker, so it can go back to the game
// at once. With frame generation xrEndFrame held the render thread 13 to 16 ms. g_endPending: layers are ready in g_endInfo.
static std::atomic<bool> g_endOnWorker{false};
static bool g_endPending = false;
static XrFrameEndInfo g_endInfo{XR_TYPE_FRAME_END_INFO};
static HANDLE g_endEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
void XrSetEndOnWorker(bool on) {
    g_endOnWorker = on;
    Log("XR frame hand-off on %s", on ? "the XR thread" : "the render thread");
}
static void WaitFrameClosed() {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        {
            std::unique_lock<std::mutex> l(g_frameLock);
            if (!g_open) return;
            if (g_endPending) {
                const XrFrameEndInfo ei = g_endInfo;
                l.unlock();
                const uint64_t e0 = NowUs();
                const XrResult r = p_xrEndFrame(g_sess, &ei);
                g_usEnd += NowUs() - e0;
                if (XR_FAILED(r)) Log("XR xrEndFrame failed %d", static_cast<int>(r));
                ++g_endedWithImages;
                l.lock();
                g_endPending = false;
                g_open = false;
                return;
            }
            if (GetTickCount64() - start > 200) {
                EndEmpty(g_openTime);
                g_open = false;
                return;
            }
        }
        WaitForSingleObject(g_endEvent, 1);
    }
}

static DWORD WINAPI XrWorker(void*) {
    ULONGLONG lastStats = GetTickCount64();
    for (;;) {
        PollEvents();
        const int st = g_state;
        if (st != XR_SESSION_STATE_READY && st != XR_SESSION_STATE_SYNCHRONIZED && st != XR_SESSION_STATE_VISIBLE && st != XR_SESSION_STATE_FOCUSED) {
            Sleep(10);
            continue;
        }
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        const uint64_t w0 = NowUs();
        XrResult r = p_xrWaitFrame(g_sess, nullptr, &fs);
        g_usWaitFrame += NowUs() - w0;
        if (XR_FAILED(r)) {
            Log("XR xrWaitFrame failed %d", static_cast<int>(r));
            Sleep(100);
            continue;
        }
        g_predicted = fs.predictedDisplayTime;
        SetEvent(FrameEvent());
        g_periodNs = fs.predictedDisplayPeriod;
        const bool overlap = g_overlap.load();
        if (overlap) WaitFrameClosed();
        r = p_xrBeginFrame(g_sess, nullptr);
        if (XR_FAILED(r)) {
            Log("XR xrBeginFrame failed %d", static_cast<int>(r));
            continue;
        }
        {
            std::lock_guard<std::mutex> l(g_frameLock);
            g_open = true;
            g_should = fs.shouldRender != XR_FALSE;
            g_openTime = fs.predictedDisplayTime;
        }
        if (!overlap) WaitFrameClosed();
        const ULONGLONG now = GetTickCount64();
        if (now - lastStats > 5000) {
            lastStats = now;
            const uint64_t n = g_endedWithImages.load();
            static uint64_t lastN = 0;
            const uint64_t dn = n - lastN ? n - lastN : 1;
            lastN = n;
            Log("XRSTAT state %d period %.2f ms screen %llu with images %llu empty %llu not ready %llu unmatched %llu pose age x100 %llu; per image us: wait frame %llu, image waits %llu, xrEndFrame %llu, whole present step %llu", g_state.load(), g_periodNs.load() / 1e6, g_endedScreen.load(), n, g_endedEmpty.load(), g_notReady.load(), g_unmatched.load(), g_ageSum.exchange(0) * 100 / dn, g_usWaitFrame.exchange(0) / dn, g_usWaitImage.exchange(0) / dn, g_usEnd.exchange(0) / dn, g_usPresent.exchange(0) / dn);
        }
    }
}

bool XrStartSession(void* device, void* queue, int eyeW, int eyeH) {
    g_dev = static_cast<ID3D12Device*>(device);
    g_queue = static_cast<ID3D12CommandQueue*>(queue);
    XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    XrResult r = p_xrGetD3D12GraphicsRequirementsKHR(g_inst, g_sys, &req);
    Log("XR graphics requirements %d, adapter LUID %08lx%08lx, min feature level %x", static_cast<int>(r), req.adapterLuid.HighPart, req.adapterLuid.LowPart, req.minFeatureLevel);
    LUID ours = g_dev->GetAdapterLuid();
    if (req.adapterLuid.LowPart != ours.LowPart || req.adapterLuid.HighPart != ours.HighPart)
        Log("XR adapter LUID differs from the game's %08lx%08lx (expected on the null driver)", ours.HighPart, ours.LowPart);
    XrGraphicsBindingD3D12KHR b{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    b.device = g_dev;
    b.queue = g_queue;
    XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO};
    sc.next = &b;
    sc.systemId = g_sys;
    r = p_xrCreateSession(g_inst, &sc, &g_sess);
    if (XR_FAILED(r)) {
        Log("XR xrCreateSession failed %d", static_cast<int>(r));
        return false;
    }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1;
    p_xrCreateReferenceSpace(g_sess, &rs, &g_space);

    uint32_t nf = 0;
    p_xrEnumerateSwapchainFormats(g_sess, 0, &nf, nullptr);
    std::vector<int64_t> fmts(nf);
    p_xrEnumerateSwapchainFormats(g_sess, nf, &nf, fmts.data());
    int64_t format = 0;
    for (int64_t f : fmts) {
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) format = f;
    }
    if (!format) {
        for (int64_t f : fmts) {
            if (f == DXGI_FORMAT_R8G8B8A8_UNORM) format = f;
        }
    }
    Log("XR swapchain format %lld of %u offered", format, nf);
    g_format = format;
    if (!format) return false;
    g_chainW = eyeW;
    g_chainH = eyeH;
    for (int e = 0; e < 2; ++e) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = eyeW;
        ci.height = eyeH;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        r = p_xrCreateSwapchain(g_sess, &ci, &g_chain[e]);
        if (XR_FAILED(r)) {
            Log("XR xrCreateSwapchain failed %d", static_cast<int>(r));
            return false;
        }
        uint32_t n = 0;
        p_xrEnumerateSwapchainImages(g_chain[e], 0, &n, nullptr);
        g_images[e].assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        p_xrEnumerateSwapchainImages(g_chain[e], n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(g_images[e].data()));
    }
    for (int i = 0; i < kRing; ++i) {
        g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]));
        g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[i], nullptr, IID_PPV_ARGS(&g_list[i]));
        g_list[i]->Close();
    }
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, XrWorker, nullptr, 0, nullptr)) CloseHandle(t);
    Log("XR session created, swapchains %dx%d", eyeW, eyeH);
    return true;
}

// On the Present thread. The two eye images were queued before this Present.
static bool EnsureUiChain(UiChain& c, int w, int h) {
    if (c.chain) return c.w == w && c.h == h;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = g_format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    const XrResult r = p_xrCreateSwapchain(g_sess, &ci, &c.chain);
    if (XR_FAILED(r)) {
        Log("XR UI swapchain failed %d", static_cast<int>(r));
        c.chain = XR_NULL_HANDLE;
        return false;
    }
    uint32_t n = 0;
    p_xrEnumerateSwapchainImages(c.chain, 0, &n, nullptr);
    c.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    p_xrEnumerateSwapchainImages(c.chain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(c.images.data()));
    c.w = w;
    c.h = h;
    Log("XR UI swapchain %dx%d", w, h);
    return true;
}

// Places the HUD ahead of the head, level, and lets it lag: it only starts to follow past 20 degrees.
static void PlaceHud(const PendingPose& pose) {
    const XrPosef& a = pose.views[0].pose;
    const XrPosef& b = pose.views[1].pose;
    const float hx = (a.position.x + b.position.x) * 0.5f, hy = (a.position.y + b.position.y) * 0.5f, hz = (a.position.z + b.position.z) * 0.5f;
    const float fx = -2 * (a.orientation.x * a.orientation.z + a.orientation.w * a.orientation.y);
    const float fz = -(1 - 2 * (a.orientation.x * a.orientation.x + a.orientation.y * a.orientation.y));
    const float yaw = atan2f(-fx, -fz);
    if (!g_hudPlaced) {
        g_hudYaw = yaw;
        g_hudPlaced = true;
    }
    float d = yaw - g_hudYaw;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    const float dead = 0.35f;
    if (d > dead) g_hudYaw += (d - dead) * 0.1f;
    else if (d < -dead) g_hudYaw += (d + dead) * 0.1f;
    const float dist = g_hudDistance;
    g_hudPos[0] = hx - sinf(g_hudYaw) * dist;
    g_hudPos[1] = hy - g_hudDrop.load();
    g_hudPos[2] = hz - cosf(g_hudYaw) * dist;
}

// ---- the flat virtual screen: splash, menus, anything that is not the 3D world ----
static UiChain g_screen;
static int64_t g_screenFormat;
static std::atomic<bool> g_screenPlaced{false};

// ---- mod menu: a picture made on the CPU, shown on top of everything, fixed in the room while open ----
static const float kMenuWidth = 1.3f, kMenuDistance = 1.6f;
static std::mutex g_menuLock;
static std::vector<uint32_t> g_menuPixels;
static int g_menuW, g_menuH;
static std::atomic<bool> g_menuVisible{false}, g_menuDirty{false}, g_menuPlaced{false};
static UiChain g_menu;
static ID3D12Resource* g_menuUpload;
static XrPosef g_menuPose{{0, 0, 0, 1}, {0, 0, 0}};
static bool g_menuAcquired;

void XrMenuShow(const uint32_t* rgba, int width, int height) {
    {
        std::lock_guard<std::mutex> l(g_menuLock);
        g_menuPixels.assign(rgba, rgba + static_cast<size_t>(width) * height);
        g_menuW = width;
        g_menuH = height;
    }
    g_menuDirty = true;
    if (!g_menuVisible.exchange(true)) g_menuPlaced = false;
}

void XrMenuHide() { g_menuVisible = false; }

static bool MenuPlace() {
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = g_openTime;
    li.space = g_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    XrView v[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    if (XR_FAILED(p_xrLocateViews(g_sess, &li, &vs, 2, &n, v)) || n != 2 || !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return false;
    const XrQuaternionf& q = v[0].pose.orientation;
    const float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    const float yaw = atan2f(-fx, -fz);
    g_menuPose.orientation = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    g_menuPose.position = {(v[0].pose.position.x + v[1].pose.position.x) * 0.5f - sinf(yaw) * kMenuDistance, (v[0].pose.position.y + v[1].pose.position.y) * 0.5f, (v[0].pose.position.z + v[1].pose.position.z) * 0.5f - cosf(yaw) * kMenuDistance};
    return true;
}

// Called with the frame's command list open. True if the menu layer is to be sent this frame.
static bool MenuRecord(ID3D12GraphicsCommandList* list, XrCompositionLayerQuad* out) {
    g_menuAcquired = false;
    if (!g_menuVisible) return false;
    int w, h;
    {
        std::lock_guard<std::mutex> l(g_menuLock);
        w = g_menuW;
        h = g_menuH;
    }
    if (!w || !EnsureUiChain(g_menu, w, h)) return false;
    if (!g_menuUpload) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = static_cast<UINT64>(w) * h * 4;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_menuUpload)))) {
            Log("MENU upload buffer failed");
            return false;
        }
        g_menuDirty = true;
    }
    if (!g_menuPlaced && MenuPlace()) g_menuPlaced = true;
    if (g_menuDirty.exchange(false)) {
        void* mapped = nullptr;
        if (SUCCEEDED(g_menuUpload->Map(0, nullptr, &mapped))) {
            {
                std::lock_guard<std::mutex> l(g_menuLock);
                memcpy(mapped, g_menuPixels.data(), g_menuPixels.size() * 4);
            }
            g_menuUpload->Unmap(0, nullptr);
        }
        uint32_t idx = 0;
        p_xrAcquireSwapchainImage(g_menu.chain, nullptr, &idx);
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000;
        p_xrWaitSwapchainImage(g_menu.chain, &wi);
        g_menuAcquired = true;
        ID3D12Resource* dst = g_menu.images[idx].texture;
        D3D12_RESOURCE_BARRIER bar{};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = dst;
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &bar);
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = dst;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.pResource = g_menuUpload;
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(g_format);
        s.PlacedFootprint.Footprint.Width = w;
        s.PlacedFootprint.Footprint.Height = h;
        s.PlacedFootprint.Footprint.Depth = 1;
        s.PlacedFootprint.Footprint.RowPitch = w * 4;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
        list->ResourceBarrier(1, &bar);
    }
    *out = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = g_space;
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = g_menu.chain;
    out->subImage.imageRect.extent = {w, h};
    out->pose = g_menuPose;
    out->size = {kMenuWidth, kMenuWidth * h / static_cast<float>(w)};
    return true;
}

// ---- aim dot: a ring on its own small swapchain, placed along the aim line ----
static const int kDotSize = 64;
static const float kDotDistance = 8.0f, kDotWidth = 0.16f;
static std::atomic<bool> g_dotOn{false};
static float g_dotDir[3], g_dotFrom[3];
static std::mutex g_dotLock;
static UiChain g_dot;
static ID3D12Resource* g_dotUpload;
static bool g_dotAcquired, g_dotReady;

void XrSetAimDot(const float* roomDir, const float* roomFrom) {
    if (!roomDir) {
        g_dotOn = false;
        return;
    }
    {
        std::lock_guard<std::mutex> l(g_dotLock);
        g_dotDir[0] = roomDir[0];
        g_dotDir[1] = roomDir[1];
        g_dotDir[2] = roomDir[2];
        if (roomFrom) {
            g_dotFrom[0] = roomFrom[0];
            g_dotFrom[1] = roomFrom[1];
            g_dotFrom[2] = roomFrom[2];
        }
    }
    g_dotOn = true;
}

static bool DotRecord(ID3D12GraphicsCommandList* list, XrCompositionLayerQuad* out, const PendingPose& pose) {
    g_dotAcquired = false;
    if (!g_dotOn || !EnsureUiChain(g_dot, kDotSize, kDotSize)) return false;
    if (!g_dotUpload) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kDotSize * 256;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_dotUpload)))) return false;
        uint8_t* m = nullptr;
        if (FAILED(g_dotUpload->Map(0, nullptr, reinterpret_cast<void**>(&m)))) return false;
        for (int y = 0; y < kDotSize; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(m + y * 256);
            for (int x = 0; x < kDotSize; ++x) {
                const float dx = x + 0.5f - kDotSize / 2.0f, dy = y + 0.5f - kDotSize / 2.0f;
                const float r = sqrtf(dx * dx + dy * dy);
                // A bright ring with a dark edge, and a centre point. Premultiplied.
                const float ring = std::clamp(1.0f - fabsf(r - 24.0f) / 3.0f, 0.0f, 1.0f);
                const float edge = std::clamp(1.0f - fabsf(r - 24.0f) / 6.0f, 0.0f, 1.0f);
                const float point = std::clamp(4.0f - r, 0.0f, 1.0f);
                const float bright = std::max(ring, point);
                const float alpha = std::max(edge * 0.6f, bright);
                const uint32_t g = static_cast<uint32_t>(bright * 255), rb = static_cast<uint32_t>(bright * 110), a = static_cast<uint32_t>(alpha * 255);
                row[x] = (a << 24) | (rb << 16) | (g << 8) | rb;
            }
        }
        g_dotUpload->Unmap(0, nullptr);
    }
    if (!g_dotReady) {
        uint32_t idx = 0;
        p_xrAcquireSwapchainImage(g_dot.chain, nullptr, &idx);
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000;
        p_xrWaitSwapchainImage(g_dot.chain, &wi);
        g_dotAcquired = true;
        ID3D12Resource* dst = g_dot.images[idx].texture;
        D3D12_RESOURCE_BARRIER bar{};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = dst;
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &bar);
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = dst;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.pResource = g_dotUpload;
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(g_format);
        s.PlacedFootprint.Footprint.Width = kDotSize;
        s.PlacedFootprint.Footprint.Height = kDotSize;
        s.PlacedFootprint.Footprint.Depth = 1;
        s.PlacedFootprint.Footprint.RowPitch = 256;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
        list->ResourceBarrier(1, &bar);
        g_dotReady = true;
    }
    float dir[3], from[3];
    {
        std::lock_guard<std::mutex> l(g_dotLock);
        memcpy(dir, g_dotDir, sizeof(dir));
        memcpy(from, g_dotFrom, sizeof(from));
    }
    *out = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = g_space;
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = g_dot.chain;
    out->subImage.imageRect.extent = {kDotSize, kDotSize};
    // The aim line starts at the game's camera, which is the centred head position in the room, not the head as it leans.
    out->pose.position = {from[0] + dir[0] * kDotDistance, from[1] + dir[1] * kDotDistance, from[2] + dir[2] * kDotDistance};
    // Faces back along the line: yaw first, then pitch.
    const float yaw = atan2f(-dir[0], -dir[2]), pitch = asinf(std::clamp(dir[1], -1.0f, 1.0f));
    const float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f), cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    out->pose.orientation = {cy * sp, sy * cp, -sy * sp, cy * cp};
    out->size = {kDotWidth, kDotWidth};
    return true;
}

static void DotRelease() {
    if (!g_dotAcquired) return;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    p_xrReleaseSwapchainImage(g_dot.chain, &ri);
    g_dotAcquired = false;
}

void XrRecenterScreen() {
    // Everything placed in the room comes round with the head.
    g_screenPlaced = false;
    g_menuPlaced = false;
    g_hudPlaced = false;
}

static void MenuRelease() {
    if (!g_menuAcquired) return;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    p_xrReleaseSwapchainImage(g_menu.chain, &ri);
    g_menuAcquired = false;
}
static XrPosef g_screenPose{{0, 0, 0, 1}, {0, 0, -2.5f}};
static std::atomic<bool> g_cinema{false};
void XrSetCinema(bool on) { g_cinema = on; }
static const float kScreenWidth = 3.2f, kScreenDistance = 2.5f;
void XrRecenterScreen();

static int64_t ScreenFormatFor(int dxgi) {
    // Same family as the back buffer so a plain copy works. The sRGB form because the bytes are already gamma encoded.
    switch (dxgi) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default:
            return 0;
    }
}

void XrOnPresentScreen(void* resource, int width, int height, int dxgiFormat) {
    std::lock_guard<std::mutex> l(g_frameLock);
    if (!g_open || g_endPending) {
        ++g_notReady;
        return;
    }
    const int64_t fmt = ScreenFormatFor(dxgiFormat);
    if (!g_should || !resource || !fmt) {
        static bool told = false;
        if (!fmt && !told) {
            told = true;
            Log("XR screen: back buffer format %d not handled", dxgiFormat);
        }
        EndEmpty(g_openTime);
        g_open = false;
        return;
    }
    if (g_screen.chain && (g_screen.w != width || g_screen.h != height || g_screenFormat != fmt)) {
        p_xrDestroySwapchain(g_screen.chain);
        g_screen = UiChain{};
    }
    if (!g_screen.chain) {
        const int64_t saved = g_format;
        g_format = fmt;
        const bool ok = EnsureUiChain(g_screen, width, height);
        g_format = saved;
        g_screenFormat = fmt;
        if (!ok) {
            EndEmpty(g_openTime);
            g_open = false;
            return;
        }
        Log("XR screen %dx%d format %d", width, height, dxgiFormat);
    }
    if (!g_screenPlaced.exchange(true)) {
        // Straight ahead of the head now, level, fixed in the room.
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = g_openTime;
        li.space = g_space;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t n = 0;
        XrView v[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        if (XR_SUCCEEDED(p_xrLocateViews(g_sess, &li, &vs, 2, &n, v)) && n == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            const XrQuaternionf& q = v[0].pose.orientation;
            const float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
            const float yaw = atan2f(-fx, -fz);
            g_screenPose.orientation = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
            g_screenPose.position = {(v[0].pose.position.x + v[1].pose.position.x) * 0.5f - sinf(yaw) * kScreenDistance, (v[0].pose.position.y + v[1].pose.position.y) * 0.5f, (v[0].pose.position.z + v[1].pose.position.z) * 0.5f - cosf(yaw) * kScreenDistance};
            Log("XR screen placed yaw %.3f at %.2f %.2f %.2f", yaw, g_screenPose.position.x, g_screenPose.position.y, g_screenPose.position.z);
        } else {
            g_screenPlaced = false;
        }
    }
    if (g_fence->GetCompletedValue() < g_ringValue[g_ringIndex]) {
        g_fence->SetEventOnCompletion(g_ringValue[g_ringIndex], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 500);
    }
    ID3D12CommandAllocator* alloc = g_alloc[g_ringIndex];
    ID3D12GraphicsCommandList* list = g_list[g_ringIndex];
    alloc->Reset();
    list->Reset(alloc, nullptr);
    uint32_t idx = 0;
    p_xrAcquireSwapchainImage(g_screen.chain, nullptr, &idx);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;
    p_xrWaitSwapchainImage(g_screen.chain, &wi);
    ID3D12Resource* dst = g_screen.images[idx].texture;
    auto* src = static_cast<ID3D12Resource*>(resource);
    D3D12_RESOURCE_BARRIER bar[2]{};
    bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[0].Transition.pResource = src;
    bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar[1] = bar[0];
    bar[1].Transition.pResource = dst;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(2, bar);
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    d.pResource = dst;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    std::swap(bar[0].Transition.StateBefore, bar[0].Transition.StateAfter);
    std::swap(bar[1].Transition.StateBefore, bar[1].Transition.StateAfter);
    list->ResourceBarrier(2, bar);
    XrCompositionLayerQuad menuLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const bool haveMenu = MenuRecord(list, &menuLayer);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_ringValue[g_ringIndex] = g_fenceValue;
    g_ringIndex = (g_ringIndex + 1) % kRing;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    p_xrReleaseSwapchainImage(g_screen.chain, &ri);
    MenuRelease();
    XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.space = g_space;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g_screen.chain;
    q.subImage.imageRect.extent = {width, height};
    q.pose = g_screenPose;
    q.size = {kScreenWidth, kScreenWidth * height / static_cast<float>(width)};
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&q), reinterpret_cast<XrCompositionLayerBaseHeader*>(&menuLayer)};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = g_openTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = haveMenu ? 2 : 1;
    ei.layers = layers;
    const XrResult r = p_xrEndFrame(g_sess, &ei);
    if (XR_FAILED(r)) Log("XR xrEndFrame failed %d (screen)", static_cast<int>(r));
    ++g_endedScreen;
    g_open = false;
}

static std::atomic<bool> g_hudHole{false};
static std::wstring g_hudAfterPath;
static std::atomic<bool> g_hudAfterWanted{false};
void XrDumpHudAfter(const wchar_t* path) {
    g_hudAfterPath = path;
    g_hudAfterWanted = true;
}
static bool g_uiFilled[kUiSlots];
static void* g_uiLastDst;
void XrSetHudHole(bool on) { g_hudHole = on; }
static ID3D12Resource* g_holeUpload;
static void* g_mirror;
void XrSetMirror(void* backBuffer) { g_mirror = backBuffer; }

// Blank a box in the middle of a HUD image already holding its copy. dst is in the copy-dest state.
static void HudHole(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, int w, int h) {
    const int hw = (w * 14 / 100 + 3) & ~3, hh = h * 24 / 100;
    const UINT pitch = (hw * 4 + 255) & ~255;
    if (!g_holeUpload) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = static_cast<UINT64>(pitch) * hh;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_holeUpload)))) return;
        void* m = nullptr;
        if (FAILED(g_holeUpload->Map(0, nullptr, &m))) return;
        memset(m, 0, static_cast<size_t>(pitch) * hh);
        g_holeUpload->Unmap(0, nullptr);
    }
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    d.pResource = dst;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.pResource = g_holeUpload;
    s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    s.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(g_format);
    s.PlacedFootprint.Footprint.Width = hw;
    s.PlacedFootprint.Footprint.Height = hh;
    s.PlacedFootprint.Footprint.Depth = 1;
    s.PlacedFootprint.Footprint.RowPitch = pitch;
    list->CopyTextureRegion(&d, (w - hw) / 2, (h - hh) / 2, 0, &s, nullptr);
}

// The left eye on the monitor: the middle of the eye at one to one, or centred if the window is larger.
static void MirrorEye(ID3D12GraphicsCommandList* list, ID3D12Resource* eye, D3D12_RESOURCE_STATES eyeState) {
    auto* bb = static_cast<ID3D12Resource*>(g_mirror);
    if (!bb) return;
    const D3D12_RESOURCE_DESC ed = eye->GetDesc(), bd = bb->GetDesc();
    static bool told = false;
    if (ed.Format != bd.Format) {
        if (!told) Log("MIRROR formats differ, eye %d window %d: left as the game draws it", ed.Format, bd.Format);
        told = true;
        return;
    }
    const UINT cw = static_cast<UINT>(std::min<UINT64>(bd.Width, std::min<UINT64>(ed.Width, g_chainW))), ch = std::min<UINT>(bd.Height, std::min<UINT>(ed.Height, g_chainH));
    D3D12_RESOURCE_BARRIER bar[2]{};
    bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[0].Transition.pResource = eye;
    bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[0].Transition.StateBefore = eyeState;
    bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar[1] = bar[0];
    bar[1].Transition.pResource = bb;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(2, bar);
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    d.pResource = bb;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.pResource = eye;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const UINT sx = (std::min<UINT>(static_cast<UINT>(ed.Width), g_chainW) - cw) / 2, sy = (std::min<UINT>(ed.Height, g_chainH) - ch) / 2;
    D3D12_BOX box{sx, sy, 0, sx + cw, sy + ch, 1};
    list->CopyTextureRegion(&d, (static_cast<UINT>(bd.Width) - cw) / 2, (bd.Height - ch) / 2, 0, &s, &box);
    std::swap(bar[0].Transition.StateBefore, bar[0].Transition.StateAfter);
    std::swap(bar[1].Transition.StateBefore, bar[1].Transition.StateAfter);
    list->ResourceBarrier(2, bar);
}

#include "rebuild.inc"
static std::atomic<bool> g_rbTest{false};
void XrRebuildTest(bool on) { g_rbTest = on; }

// ---- GPU time of the mod's own frame work: copies to the headset, HUD, monitor. And the time between two frames' ends. ----
static ID3D12QueryHeap* g_copyHeap;
static ID3D12Resource* g_copyRead;
static UINT64 g_copyFreq;
static uint32_t g_copyFrame;
static std::atomic<uint64_t> g_copyTicks{0}, g_copyCount{0};
static uint64_t g_lastEndTick;
static std::atomic<uint64_t> g_frameTicks{0}, g_frameCount{0};
static const UINT kCopyRing = 16;
static UINT64 g_copyBegins[32];
static uint32_t g_copyBeginCount;
int XrCopyBegins(unsigned long long* out, int max) {
    const int n = static_cast<int>(std::min<uint32_t>(g_copyBeginCount, 32));
    for (int i = 0; i < n && i < max; ++i) out[i] = g_copyBegins[i];
    return std::min(n, max);
}
static bool CopyTimerBegin(ID3D12GraphicsCommandList* list, UINT* index) {
    if (!g_copyHeap) {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = kCopyRing * 2;
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
        if (FAILED(g_queue->GetTimestampFrequency(&g_copyFreq)) || FAILED(g_dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_copyHeap))) || FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_copyRead)))) return false;
    }
    const uint32_t f = g_copyFrame++;
    *index = f % kCopyRing * 2;
    if (f >= kCopyRing) {
        D3D12_RANGE range{*index * 8, (*index + 2) * 8};
        UINT64* m = nullptr;
        if (SUCCEEDED(g_copyRead->Map(0, &range, reinterpret_cast<void**>(&m)))) {
            const UINT64 a = m[*index], b = m[*index + 1];
            D3D12_RANGE none{0, 0};
            g_copyRead->Unmap(0, &none);
            if (b > a) {
                g_copyBegins[g_copyBeginCount++ % 32] = a;
                g_copyTicks += b - a;
                ++g_copyCount;
                if (g_lastEndTick && b > g_lastEndTick) {
                    g_frameTicks += b - g_lastEndTick;
                    ++g_frameCount;
                }
                g_lastEndTick = b;
            }
        }
    }
    list->EndQuery(g_copyHeap, D3D12_QUERY_TYPE_TIMESTAMP, *index);
    return true;
}
void XrCopyTimes(double* copyMs, double* betweenMs) {
    const uint64_t c = g_copyCount.exchange(0), t = g_copyTicks.exchange(0), fc = g_frameCount.exchange(0), ft = g_frameTicks.exchange(0);
    *copyMs = c && g_copyFreq ? static_cast<double>(t) / static_cast<double>(c) * 1000.0 / static_cast<double>(g_copyFreq) : 0;
    *betweenMs = fc && g_copyFreq ? static_cast<double>(ft) / static_cast<double>(fc) * 1000.0 / static_cast<double>(g_copyFreq) : 0;
}

extern void SeqNote(char c);
void XrOnPresent(void* const resources[2], const uint32_t states[2], const XrUiImage* ui, int uiCount) {
    std::lock_guard<std::mutex> l(g_frameLock);
    if (!g_open || g_endPending) {
        ++g_notReady;
        SeqNote('c');
        return;
    }
    const uint64_t present0 = NowUs();
    SeqNote('o');
    PendingPose pose;
    bool havePose = false;
    bool copyEye[2] = {true, true};
    int freshEye = -1;
    {
        std::lock_guard<std::mutex> pl(g_poseLock);
        if (g_renderedSerial > g_usedSerial && g_poses[g_renderedSerial % kPoses].serial == g_renderedSerial) {
            pose = g_poses[g_renderedSerial % kPoses];
            g_ageSum += g_poseSerial - g_renderedSerial;
            g_usedSerial = g_renderedSerial;
            havePose = true;
            if (g_alternate.load()) {
                const int fresh = g_renderedEye;
                freshEye = fresh;
                // Until an eye has been drawn once in this mode, send both as they are.
                if (g_held[1 - fresh]) copyEye[1 - fresh] = false;
                else {
                    g_held[1 - fresh] = true;
                    g_heldView[1 - fresh] = pose.views[1 - fresh];
                }
                g_held[fresh] = true;
                g_heldView[fresh] = pose.views[fresh];
                // With a rebuild the other eye is drawn for this tick's pose. Decided below: keep the real pose until then.
                if (g_rebuildMode.load() == 0) {
                    pose.views[0] = g_heldView[0];
                    pose.views[1] = g_heldView[1];
                } else {
                    pose.views[fresh] = g_heldView[fresh];
                }
            }
        }
    }
    if (!resources[0] || !resources[1]) {
        EndEmpty(g_openTime);
        g_open = false;
        return;
    }
    if (!havePose) {
        // No new image yet. Leave the frame open for the next Present: the worker ends it if nothing comes.
        ++g_notReady;
        SeqNote('n');
        return;
    }
    if (!g_should || !resources[0] || !resources[1]) {
        EndEmpty(g_openTime);
        g_open = false;
        return;
    }
    if (g_fence->GetCompletedValue() < g_ringValue[g_ringIndex]) {
        g_fence->SetEventOnCompletion(g_ringValue[g_ringIndex], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 500);
    }
    ID3D12CommandAllocator* alloc = g_alloc[g_ringIndex];
    ID3D12GraphicsCommandList* list = g_list[g_ringIndex];
    alloc->Reset();
    list->Reset(alloc, nullptr);
    UINT copyIndex = 0;
    const bool copyTimed = CopyTimerBegin(list, &copyIndex);
    // The rebuilt eye: from the drawn eye's image and depth, for this tick's camera.
    bool rebuilt = false;
    if (g_rebuildMode.load() != 0 && freshEye >= 0 && pose.cameras[0].valid && pose.cameras[1].valid) {
        rebuilt = RbRun(list, freshEye, pose.cameras, static_cast<ID3D12Resource*>(resources[freshEye]), static_cast<D3D12_RESOURCE_STATES>(states[freshEye]), pose.nearZ);
    } else if (g_rbTest.load() && g_rebuildMode.load() != 0 && freshEye < 0 && pose.cameras[0].valid && pose.cameras[1].valid) {
        RbRun(list, 0, pose.cameras, static_cast<ID3D12Resource*>(resources[0]), static_cast<D3D12_RESOURCE_STATES>(states[0]), pose.nearZ);
    }
    int rebuiltEye = -1;
    if (rebuilt) rebuiltEye = 1 - freshEye;
    else if (freshEye >= 0 && g_rebuildMode.load() != 0) {
        // No rebuild this time: the held image with the pose it was drawn at.
        std::lock_guard<std::mutex> pl(g_poseLock);
        pose.views[1 - freshEye] = g_heldView[1 - freshEye];
    }
    static XrCompositionLayerProjectionView pv[2];
    pv[0] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    pv[1] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    for (int e = 0; e < 2; ++e) {
        pv[e].pose = pose.views[e].pose;
        pv[e].fov = pose.views[e].fov;
        pv[e].subImage.swapchain = g_chain[e];
        pv[e].subImage.imageRect.extent = {g_chainW, g_chainH};
        const bool fromRebuild = e == rebuiltEye;
        if (fromRebuild) copyEye[e] = true;
        if (!copyEye[e]) continue;  // the runtime keeps showing this swapchain's last released image
        uint32_t idx = 0;
        const uint64_t t0 = NowUs();
        p_xrAcquireSwapchainImage(g_chain[e], nullptr, &idx);
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000;
        p_xrWaitSwapchainImage(g_chain[e], &wi);
        g_usWaitImage += NowUs() - t0;
        ID3D12Resource* dst = g_images[e][idx].texture;
        auto* src = fromRebuild ? g_rbOut : static_cast<ID3D12Resource*>(resources[e]);
        const D3D12_RESOURCE_STATES srcBefore = fromRebuild ? D3D12_RESOURCE_STATE_COPY_SOURCE : static_cast<D3D12_RESOURCE_STATES>(states[e]);
        D3D12_RESOURCE_BARRIER bar[2]{};
        bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar[0].Transition.pResource = src;
        bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar[0].Transition.StateBefore = srcBefore;
        bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bar[1] = bar[0];
        bar[1].Transition.pResource = dst;
        bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        if (fromRebuild) list->ResourceBarrier(1, &bar[1]);
        else list->ResourceBarrier(2, bar);
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = dst;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.pResource = src;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_RESOURCE_DESC sd = src->GetDesc();
        D3D12_BOX box{0, 0, 0, static_cast<UINT>(std::min(static_cast<UINT64>(g_chainW), sd.Width)), std::min(static_cast<UINT>(g_chainH), sd.Height), 1};
        list->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
        std::swap(bar[0].Transition.StateBefore, bar[0].Transition.StateAfter);
        std::swap(bar[1].Transition.StateBefore, bar[1].Transition.StateAfter);
        if (fromRebuild) list->ResourceBarrier(1, &bar[1]);
        else list->ResourceBarrier(2, bar);
    }
    static XrCompositionLayerQuad quads[kUiSlots];
    uint32_t quadCount = 0;
    PlaceHud(pose);
    bool uiAcquired[kUiSlots] = {};
    for (int i = 0; i < uiCount && i < kUiSlots; ++i) {
        if (!ui[i].resource || !EnsureUiChain(g_ui[i], ui[i].width, ui[i].height)) continue;
        const auto addQuad = [&]() {
            XrCompositionLayerQuad& q = quads[quadCount++];
            q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied, as Coherent draws it
            q.space = g_space;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = g_ui[i].chain;
            q.subImage.imageRect.extent = {ui[i].width, ui[i].height};
            q.pose.orientation = {0, sinf(g_hudYaw * 0.5f), 0, cosf(g_hudYaw * 0.5f)};
            q.pose.position = {g_hudPos[0], g_hudPos[1], g_hudPos[2]};
            const float width = g_hudWidth;
            q.size = {width, width * ui[i].height / static_cast<float>(ui[i].width)};
        };
        if (ui[i].keepLast) {
            if (g_uiFilled[i]) addQuad();
            continue;
        }
        g_uiFilled[i] = true;
        uint32_t idx = 0;
        const uint64_t t0 = NowUs();
        p_xrAcquireSwapchainImage(g_ui[i].chain, nullptr, &idx);
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000;
        p_xrWaitSwapchainImage(g_ui[i].chain, &wi);
        g_usWaitImage += NowUs() - t0;
        uiAcquired[i] = true;
        ID3D12Resource* dst = g_ui[i].images[idx].texture;
        if (i == 0) g_uiLastDst = dst;
        auto* src = static_cast<ID3D12Resource*>(ui[i].resource);
        D3D12_RESOURCE_BARRIER bar[2]{};
        bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar[0].Transition.pResource = src;
        bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar[0].Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(ui[i].state);
        bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bar[1] = bar[0];
        bar[1].Transition.pResource = dst;
        bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(2, bar);
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = dst;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.pResource = src;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        if (i == 0 && g_hudHole && !g_cinema) HudHole(list, dst, ui[i].width, ui[i].height);
        std::swap(bar[0].Transition.StateBefore, bar[0].Transition.StateAfter);
        std::swap(bar[1].Transition.StateBefore, bar[1].Transition.StateAfter);
        list->ResourceBarrier(2, bar);
        addQuad();
    }
    MirrorEye(list, static_cast<ID3D12Resource*>(resources[0]), static_cast<D3D12_RESOURCE_STATES>(states[0]));
    static XrCompositionLayerQuad menuLayer, dotLayer;
    menuLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    dotLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    const bool haveMenu = MenuRecord(list, &menuLayer);
    const bool haveDot = !g_cinema && DotRecord(list, &dotLayer, pose);
    if (copyTimed) {
        list->EndQuery(g_copyHeap, D3D12_QUERY_TYPE_TIMESTAMP, copyIndex + 1);
        list->ResolveQueryData(g_copyHeap, D3D12_QUERY_TYPE_TIMESTAMP, copyIndex, 2, g_copyRead, static_cast<UINT64>(copyIndex) * 8);
    }
    list->Close();
    ID3D12CommandList* lists[] = {list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_ringValue[g_ringIndex] = g_fenceValue;
    g_ringIndex = (g_ringIndex + 1) % kRing;
    MenuRelease();
    DotRelease();
    for (int e = 0; e < 2; ++e) {
        if (!copyEye[e]) continue;
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        p_xrReleaseSwapchainImage(g_chain[e], &ri);
    }
    for (int i = 0; i < kUiSlots; ++i) {
        if (uiAcquired[i]) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            p_xrReleaseSwapchainImage(g_ui[i].chain, &ri);
            if (i == 0 && g_hudAfterWanted.exchange(false)) {
                void* r[1] = {g_uiLastDst};
                uint32_t st[1] = {D3D12_RESOURCE_STATE_RENDER_TARGET};
                const wchar_t* paths[1] = {g_hudAfterPath.c_str()};
                CaptureRequest(r, st, paths, 1);
            }
        }
    }
    static XrCompositionLayerProjection layer;
    layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_space;
    layer.viewCount = 2;
    layer.views = pv;
    static const XrCompositionLayerBaseHeader* layers[4 + kUiSlots];
    uint32_t layerCount = 0;
    static XrCompositionLayerQuad cinema[2];
    if (g_cinema) {
        // The middle band of each eye's image, 16 by 9, one quad per eye at the same place in the room. Same tick, real depth.
        const int bandH = std::min(g_chainH, g_chainW * 9 / 16);
        for (int e = 0; e < 2; ++e) {
            XrCompositionLayerQuad& c = cinema[e];
            c = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            c.space = g_space;
            c.eyeVisibility = e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
            c.subImage.swapchain = g_chain[e];
            c.subImage.imageRect.offset = {0, (g_chainH - bandH) / 2};
            c.subImage.imageRect.extent = {g_chainW, bandH};
            c.pose = g_screenPose;
            c.size = {kScreenWidth, kScreenWidth * bandH / static_cast<float>(g_chainW)};
            layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&c);
        }
    } else {
        layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);
    }
    for (uint32_t i = 0; i < quadCount; ++i) layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quads[i]);
    if (haveDot) layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&dotLayer);
    if (haveMenu) layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&menuLayer);
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = g_openTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    if (g_endOnWorker.load()) {
        g_endInfo = ei;
        g_endPending = true;
        SetEvent(g_endEvent);
        g_usPresent += NowUs() - present0;
        return;
    }
    const uint64_t e0 = NowUs();
    const XrResult r = p_xrEndFrame(g_sess, &ei);
    g_usEnd += NowUs() - e0;
    if (XR_FAILED(r)) Log("XR xrEndFrame failed %d", static_cast<int>(r));
    ++g_endedWithImages;
    g_usPresent += NowUs() - present0;
    g_open = false;
}
