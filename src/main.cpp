// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence.
//
// A WINHTTP.dll proxy (the game imports it directly, so this loads before the game's own code runs).
// Build 2: logging hooks that change nothing, plus live test commands that do. No command runs by itself.
#include <windows.h>
#include <MinHook.h>
#include "capture.h"
#include "xr.h"
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <map>
#include <mutex>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#define BUILD_NUMBER 15

// The stubs in winhttp_stubs.asm jump through this table.
static const char* const kNames[] = {
#include "winhttp_names.inc"
};
extern "C" void* g_realWinhttp[sizeof(kNames) / sizeof(kNames[0])] = {};

static FILE* g_log = nullptr;
static ULONGLONG g_start = 0;
static std::wstring g_dir;
static uint8_t* g_base = nullptr;
static size_t g_textStart = 0, g_textSize = 0;
static void** g_activeViewPtr = nullptr;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    char line[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof(line), fmt, a);
    va_end(a);
    fprintf(g_log, "%8.3f %s\n", (GetTickCount64() - g_start) / 1000.0, line);
    fflush(g_log);
}

// Reads a block of game memory, surviving an address that has gone away.
static bool SafeCopy(void* out, const void* from, size_t n) {
    __try {
        memcpy(out, from, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void FindSections() {
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    Log("EXE base %p image size %x link time %x", g_base, nt->OptionalHeader.SizeOfImage, nt->FileHeader.TimeDateStamp);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!memcmp(sec[i].Name, ".text", 5)) {
            g_textStart = sec[i].VirtualAddress;
            g_textSize = sec[i].Misc.VirtualSize;
        }
    }
}

// Two one-line functions compare a pointer to the active view globals: cmp rcx,[rip+x]; sete al; ret
static void FindActiveView() {
    const uint8_t* t = g_base + g_textStart;
    std::vector<size_t> hits;
    for (size_t i = 0; i + 11 < g_textSize; ++i) {
        if (t[i] == 0x48 && t[i + 1] == 0x3B && t[i + 2] == 0x0D && t[i + 7] == 0x0F && t[i + 8] == 0x94 && t[i + 9] == 0xC0 && t[i + 10] == 0xC3)
            hits.push_back(i);
    }
    Log("SIG active view tests: %zu hits", hits.size());
    for (size_t h : hits) Log("SIG   hit rva %zx", g_textStart + h);
    if (hits.size() == 2 && hits[1] - hits[0] == 16) {
        int32_t d = *reinterpret_cast<const int32_t*>(t + hits[1] + 3);
        g_activeViewPtr = reinterpret_cast<void**>(const_cast<uint8_t*>(t) + hits[1] + 7 + d);
        Log("SIG active view global rva %zx", reinterpret_cast<uint8_t*>(g_activeViewPtr) - g_base);
    } else {
        Log("SIG active view NOT FOUND, this build of the game is not known");
    }
}

static void LogView() {
    if (!g_activeViewPtr) return;
    void* view = nullptr;
    if (!SafeCopy(&view, g_activeViewPtr, sizeof(view)) || !view) {
        Log("VIEW none");
        return;
    }
    float cam[16], proj[16];
    if (!SafeCopy(cam, view, sizeof(cam)) || !SafeCopy(proj, static_cast<uint8_t*>(view) + 0x1c0, sizeof(proj))) {
        Log("VIEW unreadable %p", view);
        return;
    }
    const double vfov = proj[5] != 0 ? 2.0 * atan(1.0 / fabs(proj[5])) * 57.29577951 : 0;
    const double aspect = proj[0] != 0 ? fabs(proj[5] / proj[0]) : 0;
    Log("VIEW %p pos %.3f %.3f %.3f forward %.3f %.3f %.3f vfov %.2f aspect %.3f near %.3f", view, cam[12], cam[13], cam[14], cam[8], cam[9], cam[10], vfov, aspect, proj[14]);
}

// ---- engine addresses, RVAs for build v3.630.1.0, each checked against its first bytes before use ----
struct Target {
    const char* name;
    size_t rva;
    const char* bytes;
};
static const Target kOffscreenRenderAll{"OffscreenRenderAll", 0x12bb1c0, "\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x20\x33\xdb"};
static const Target kCameraSubmit{"CameraSubmit", 0x12324e0, "\x48\x3b\xd1\x74\x7c\x48\x8b\xc1\x41\xb8\x08"};
static const Target kOffscreenCreate{"OffscreenCreate", 0x1239af0, "\x48\x89\x5c\x24\x10\x48\x89\x6c\x24\x18\x57\x48\x83\xec\x60"};
static const Target kSceneSubmit{"SceneSubmit", 0x12bb640, "\x44\x88\x4c\x24\x20\x48\x89\x54\x24\x10\x55\x53\x56\x57"};
static const Target kViewCreate{"ViewCreate", 0x1238fb0, "\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18\x48\x89\x7c\x24\x20\x41\x56"};
static const Target kLensPersp{"LensPersp", 0x1213240, "\x48\x8b\xc4\x48\x89\x58\x08\x55\x48\x8d\x6c\x24\x80"};
static const Target kLensPose{"LensPose", 0x1214150, "\x40\x53\x48\x83\xec\x60\x0f\x10\x02\x48\x8b\xd9\x0f\x11"};
static const Target kJitter{"Jitter", 0x1234030, "\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10\x48\x89\x7c\x24\x18\x41\x56"};
static const Target kBufferSize{"BufferSize", 0x120c2e0, "\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10\x48\x89\x7c\x24\x18\x55\x41\x54"};
static const Target kTone{"Tone", 0x11dc080, "\x48\x8b\xc4\x48\x89\x50\x10\x55\x53\x56\x57\x41"};
static const size_t kManager = 0x6739a10;
    // ViewContextManager
static const size_t kShowSlot = 0x5339598;   // the game draws this offscreen slot over the active view

static bool Check(const Target& t) {
    const bool ok = memcmp(g_base + t.rva, t.bytes, strlen(t.bytes)) == 0;
    Log("TARGET %s rva %zx %s", t.name, t.rva, ok ? "ok" : "BYTES DIFFER, refusing");
    return ok;
}

struct OffscreenDesc {
    const char* name;
    float nearZ, farZ, fovDegrees;
    uint32_t width, height, format;
    void* sizeSource;
    uint8_t useSource;
    uint32_t kind;
    int32_t unknown;
};
static_assert(offsetof(OffscreenDesc, width) == 0x14 && offsetof(OffscreenDesc, sizeSource) == 0x20 && offsetof(OffscreenDesc, useSource) == 0x28 && offsetof(OffscreenDesc, kind) == 0x2c && offsetof(OffscreenDesc, unknown) == 0x30, "layout");

using SceneSubmitFn = void*(__fastcall*)(uint8_t*, void*, uint32_t, char);
using RenderAllFn = void(__fastcall*)();
using CameraSubmitFn = void(__fastcall*)(uint8_t*, const uint8_t*);
using OffscreenCreateFn = uint8_t*(__fastcall*)(uint8_t*, OffscreenDesc*);
static const char kEyeName[] = "RiftApartVR eye test";
static std::atomic<uint32_t> g_createViewKind{0};  // 3 makes the engine treat the view as a child of the main view
using ViewCreateFn = void(__fastcall*)(uint8_t*, const char*, void*, void*, void*, uint64_t, void*, uint32_t);
static ViewCreateFn g_origViewCreate;
static uint8_t* ActiveView();
static void __fastcall HookViewCreate(uint8_t* view, const char* name, void* buffers, void* a4, void* a5, uint64_t kind, void* parent, uint32_t a8) {
    if (name == kEyeName && g_createViewKind != 0) {
        kind = g_createViewKind == 9 ? 0 : g_createViewKind.load();
        if (kind == 3) parent = ActiveView();
        Log("EYE view %p ctor kind %llu parent %p", view, kind, parent);
    }
    g_origViewCreate(view, name, buffers, a4, a5, kind, parent, a8);
}

// Eyes get their own full buffer set at their own size, whatever the game's window or resolution setting is.
using BufferSizeFn = void(__fastcall*)(uint8_t*, const char*, uint32_t, uint32_t, int, char);
static BufferSizeFn g_origBufferSize;
static std::atomic<uint32_t> g_forceW{0}, g_forceH{0};
static void __fastcall HookBufferSize(uint8_t* buffers, const char* name, uint32_t w, uint32_t h, int kind, char align) {
    const bool ours = name == kEyeName && g_forceW != 0;
    if (ours) {
        w = g_forceW;
        h = g_forceH;
        *reinterpret_cast<void**>(buffers + 0x14990) = nullptr;  // no sharing with, or clamping to, the main view's set
    }
    g_origBufferSize(buffers, name, w, h, kind, align);
    if (ours) {
        for (size_t off : {size_t{0x14910}, size_t{0x14918}, size_t{0x14920}}) {
            reinterpret_cast<uint32_t*>(buffers + off)[0] = w;
            reinterpret_cast<uint32_t*>(buffers + off)[1] = h;
        }
        Log("EYE buffers %p %ux%u kind %d", buffers, w, h, kind);
    }
}

// The tone mapper gives views with a negative id (+0xf90, all offscreen views) a plainer treatment. Eyes want the main view's.
using ToneFn = void*(__fastcall*)(void*, void*, void*, uint8_t*, void*, uint64_t);
static ToneFn g_origTone;
static std::atomic<bool> g_toneFix{true};
static std::atomic<uint8_t*> g_eyeA{nullptr}, g_eyeB{nullptr};
static void* __fastcall HookTone(void* a, void* b, void* c, uint8_t* view, void* e, uint64_t generic) {
    if (g_toneFix && view) {
        uint8_t* owner = *reinterpret_cast<uint8_t**>(view + 0x1ff8);
        if (owner && (owner == g_eyeA.load() || owner == g_eyeB.load())) generic = 0;
    }
    return g_origTone(a, b, c, view, e, generic);
}

static SceneSubmitFn g_origSceneSubmit;
static RenderAllFn g_origRenderAll;
static CameraSubmitFn g_origCameraSubmit;

static std::atomic<uint64_t> g_frames{0};
static std::mutex g_statsLock;
static std::map<std::pair<size_t, void*>, uint64_t> g_cameraSubmits;
static std::map<void*, uint64_t> g_sceneSubmits;
static std::map<void*, uint32_t> g_sceneMask;

// Test commands set these, and the game's own thread acts on them at the top of the offscreen pass.
static std::atomic<int> g_wantCreate{0};
static std::atomic<uint32_t> g_createW{1920}, g_createH{1080}, g_createKind{2};
static std::vector<uint8_t*> g_eyes;
static std::mutex g_eyesLock;
static std::atomic<bool> g_copyCamera{false};
static std::atomic<bool> g_stereo{false};
static std::atomic<float> g_ipd{0.064f};
static std::atomic<bool> g_xrEyes{false}, g_wantRecenter{false}, g_pace{true}, g_shareLum{false}, g_shareFog{false};
static std::atomic<float> g_worldScale{1.0f};
static bool g_haveCentre = false;
static float g_centre[3], g_centreYaw;
// Off-centre perspective from edges, then the pose: the same two steps the game's own camera builders end with.
// Edges are tangents: left, right, bottom, top, as OpenXR gives them.
using LensPerspFn = void(__fastcall*)(void*, float, float, float, float, float, float, float, float, float, float, char, char);
using LensPoseFn = void(__fastcall*)(void*, const float*);
using JitterFn = void(__fastcall*)(uint8_t*, float*, float*);

static uint8_t* ActiveView() {
    return g_activeViewPtr ? static_cast<uint8_t*>(*g_activeViewPtr) : nullptr;
}

static void __fastcall HookRenderAll() {
    ++g_frames;
    if (g_xrEyes && g_pace) XrPaceWait();
    if (g_wantCreate.exchange(0)) {
        OffscreenDesc d{};
        d.name = kEyeName;
        d.nearZ = 0.1f;
        d.farZ = 100000.f;
        d.fovDegrees = 47.45f;
        d.width = g_createW;
        d.height = g_createH;
        d.format = 28;
        d.kind = g_createKind;
        d.unknown = -1;
        uint8_t* eye = reinterpret_cast<OffscreenCreateFn>(g_base + kOffscreenCreate.rva)(g_base + kManager, &d);
        Log("EYE created %p %ux%u kind %u", eye, d.width, d.height, d.kind);
        if (eye) {
            std::lock_guard<std::mutex> l(g_eyesLock);
            if (g_eyes.empty()) g_eyeA = eye;
            else if (g_eyes.size() == 1) g_eyeB = eye;
            g_eyes.push_back(eye);
        }
    }
    if (g_copyCamera) {
        if (uint8_t* main = ActiveView()) {
            std::lock_guard<std::mutex> l(g_eyesLock);
            for (uint8_t* eye : g_eyes) g_origCameraSubmit(eye, main);
        }
    }
    if (g_shareLum) {
        if (uint8_t* main = ActiveView()) {
            // The eyes read the main view's Adapted Lum, so both have the exposure the game chose for its own view.
            std::lock_guard<std::mutex> l(g_eyesLock);
            for (uint8_t* eye : g_eyes) memcpy(eye + 0x1740, main + 0x1740, 0x38);
        }
    }
    if (g_shareFog) {
        if (uint8_t* main = ActiveView()) {
            std::lock_guard<std::mutex> l(g_eyesLock);
            for (uint8_t* eye : g_eyes) memcpy(eye + 0x1030, main + 0x1030, 0x30);
        }
    }
    if (g_xrEyes) {
        if (uint8_t* main = ActiveView()) {
            XrEyePose eye[2];
            std::lock_guard<std::mutex> l(g_eyesLock);
            if (g_eyes.size() >= 2 && XrLocate(eye)) {
                if (!g_haveCentre || g_wantRecenter.exchange(false)) {
                    // Centre: the head's position and yaw now become the game camera's.
                    const float cx = (eye[0].px + eye[1].px) * 0.5f, cy = (eye[0].py + eye[1].py) * 0.5f, cz = (eye[0].pz + eye[1].pz) * 0.5f;
                    g_centre[0] = cx;
                    g_centre[1] = cy;
                    g_centre[2] = cz;
                    const float fx = -2 * (eye[0].qx * eye[0].qz + eye[0].qw * eye[0].qy), fz = -(1 - 2 * (eye[0].qx * eye[0].qx + eye[0].qy * eye[0].qy));
                    g_centreYaw = atan2f(-fx, -fz);
                    g_haveCentre = true;
                    Log("XR eye0 q %.3f %.3f %.3f %.3f p %.3f %.3f %.3f tan L %.3f R %.3f U %.3f D %.3f", eye[0].qx, eye[0].qy, eye[0].qz, eye[0].qw, eye[0].px, eye[0].py, eye[0].pz, eye[0].tanLeft, eye[0].tanRight, eye[0].tanUp, eye[0].tanDown);
                    Log("XR eye1 q %.3f %.3f %.3f %.3f p %.3f %.3f %.3f tan L %.3f R %.3f U %.3f D %.3f", eye[1].qx, eye[1].qy, eye[1].qz, eye[1].qw, eye[1].px, eye[1].py, eye[1].pz, eye[1].tanLeft, eye[1].tanRight, eye[1].tanUp, eye[1].tanDown);
                    Log("XR centred at %.3f %.3f %.3f yaw %.3f", cx, cy, cz, g_centreYaw);
                }
                float cam[16];
                memcpy(cam, main, sizeof(cam));
                const float nearZ = *reinterpret_cast<float*>(main + 0x3f8), farZ = *reinterpret_cast<float*>(main + 0x3fc);
                float jx = 0, jy = 0;
                reinterpret_cast<JitterFn>(g_base + kJitter.rva)(main, &jx, &jy);
                const float cy = cosf(-g_centreYaw), sy = sinf(-g_centreYaw);
                const float scale = g_worldScale.load();
                alignas(16) static uint8_t desc[0x600];
                for (int e = 0; e < 2; ++e) {
                    const XrEyePose& p = eye[e];
                    // Head axes in LOCAL space from the quaternion.
                    const float x = p.qx, y = p.qy, z = p.qz, w = p.qw;
                    const float ax[3][3] = {
                        {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)},
                        {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)},
                        {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)}};
                    const float dp[3] = {p.px - g_centre[0], p.py - g_centre[1], p.pz - g_centre[2]};
                    // Recentre around up, then: ref x = camera right, ref y = camera up (minus row 1), ref z = camera back (minus row 2).
                    auto toWorld = [&](const float v[3], float out[3]) {
                        const float vx = cy * v[0] + sy * v[2], vz = -sy * v[0] + cy * v[2];
                        for (int i = 0; i < 3; ++i) out[i] = vx * cam[i] - v[1] * cam[4 + i] - vz * cam[8 + i];
                    };
                    float eyePose[16] = {};
                    float right[3], up[3], back[3], offset[3];
                    toWorld(ax[0], right);
                    toWorld(ax[1], up);
                    toWorld(ax[2], back);
                    toWorld(dp, offset);
                    for (int i = 0; i < 3; ++i) {
                        eyePose[i] = right[i];
                        eyePose[4 + i] = -up[i];
                        eyePose[8 + i] = -back[i];
                        eyePose[12 + i] = cam[12 + i] + offset[i] * scale;
                    }
                    eyePose[15] = 1;
                    reinterpret_cast<LensPerspFn>(g_base + kLensPersp.rva)(desc, nearZ, farZ, p.tanLeft, p.tanRight, p.tanDown, p.tanUp, 0, 0, jx, jy, 0, 0);
                    reinterpret_cast<LensPoseFn>(g_base + kLensPose.rva)(desc, eyePose);
                    g_origCameraSubmit(g_eyes[e], desc);
                }
            }
        }
    }
    if (g_stereo) {
        if (uint8_t* main = ActiveView()) {
            std::lock_guard<std::mutex> l(g_eyesLock);
            if (g_eyes.size() >= 2) {
                float pose[16];
                memcpy(pose, main, sizeof(pose));
                const float* noJitter = reinterpret_cast<const float*>(main + 0x1c0);
                const float tanX = 1.0f / fabsf(noJitter[0]), tanY = 1.0f / fabsf(noJitter[5]);
                const float nearZ = *reinterpret_cast<float*>(main + 0x3f8), farZ = *reinterpret_cast<float*>(main + 0x3fc);
                float jx = 0, jy = 0;
                reinterpret_cast<JitterFn>(g_base + kJitter.rva)(main, &jx, &jy);  // one phase for the pair
                const float half = g_ipd.load() * 0.5f;
                alignas(16) static uint8_t desc[0x600];
                for (int e = 0; e < 2; ++e) {
                    float eyePose[16];
                    memcpy(eyePose, pose, sizeof(pose));
                    const float side = e == 0 ? -half : half;
                    for (int i = 0; i < 3; ++i) eyePose[12 + i] += pose[i] * side;
                    reinterpret_cast<LensPerspFn>(g_base + kLensPersp.rva)(desc, nearZ, farZ, -tanX, tanX, -tanY, tanY, 0, 0, jx, jy, 0, 0);
                    reinterpret_cast<LensPoseFn>(g_base + kLensPose.rva)(desc, eyePose);
                    g_origCameraSubmit(g_eyes[e], desc);
                }
            }
        }
    }
    g_origRenderAll();
}

static void* __fastcall HookSceneSubmit(uint8_t* view, void* a, uint32_t mask, char b) {
    {
        std::lock_guard<std::mutex> l(g_statsLock);
        ++g_sceneSubmits[view];
        g_sceneMask[view] = mask;
    }
    return g_origSceneSubmit(view, a, mask, b);
}

static void __fastcall HookCameraSubmit(uint8_t* view, const uint8_t* desc) {
    {
        std::lock_guard<std::mutex> l(g_statsLock);
        ++g_cameraSubmits[{static_cast<size_t>(reinterpret_cast<uint8_t*>(_ReturnAddress()) - g_base), view}];
    }
    g_origCameraSubmit(view, desc);
}

static void InstallHooks() {
    if (!Check(kOffscreenRenderAll) || !Check(kCameraSubmit) || !Check(kOffscreenCreate) || !Check(kSceneSubmit) || !Check(kViewCreate) || !Check(kLensPersp) || !Check(kLensPose) || !Check(kJitter) || !Check(kBufferSize) || !Check(kTone)) return;
    MH_Initialize();
    MH_CreateHook(g_base + kOffscreenRenderAll.rva, reinterpret_cast<void*>(&HookRenderAll), reinterpret_cast<void**>(&g_origRenderAll));
    MH_CreateHook(g_base + kSceneSubmit.rva, reinterpret_cast<void*>(&HookSceneSubmit), reinterpret_cast<void**>(&g_origSceneSubmit));
    MH_CreateHook(g_base + kCameraSubmit.rva, reinterpret_cast<void*>(&HookCameraSubmit), reinterpret_cast<void**>(&g_origCameraSubmit));
    MH_CreateHook(g_base + kViewCreate.rva, reinterpret_cast<void*>(&HookViewCreate), reinterpret_cast<void**>(&g_origViewCreate));
    MH_CreateHook(g_base + kTone.rva, reinterpret_cast<void*>(&HookTone), reinterpret_cast<void**>(&g_origTone));
    MH_CreateHook(g_base + kBufferSize.rva, reinterpret_cast<void*>(&HookBufferSize), reinterpret_cast<void**>(&g_origBufferSize));
    const MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
    Log("HOOKS %s", MH_StatusToString(st));
}

static void LogManager() {
    uint8_t m[0x110];
    if (!SafeCopy(m, g_base + kManager, sizeof(m))) return;
    const uint8_t* pool = *reinterpret_cast<uint8_t**>(m);
    const uint64_t used = *reinterpret_cast<uint64_t*>(m + 8);
    Log("MANAGER pool %p used mask %llx count %d capacity %d active %p second %p frames %llu", pool, used, *reinterpret_cast<int*>(m + 0x10), *reinterpret_cast<int*>(m + 0x14), *reinterpret_cast<void**>(m + 0xf8), *reinterpret_cast<void**>(m + 0x100), g_frames.load());
    for (int i = 0; i < 8; ++i) {
        uint8_t* s = reinterpret_cast<uint8_t**>(m + 0x30)[i];
        if (!s) continue;
        char name[48] = "?";
        const char* np = nullptr;
        if (SafeCopy(&np, s + 0x16c88, 8) && np) SafeCopy(name, np, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        Log("SLOT %d %p '%s' %dx%d kind %d flags %x mask %x format %d texture %p buffers %p", i, s, name, *reinterpret_cast<int*>(s + 0x16c70), *reinterpret_cast<int*>(s + 0x16c74), *reinterpret_cast<int*>(s + 0x16c7c), *reinterpret_cast<uint32_t*>(s + 0x16ccc), *reinterpret_cast<uint32_t*>(s + 0x16cd4), *reinterpret_cast<uint32_t*>(s + 0x16cd0), *reinterpret_cast<void**>(s + 0x2088), *reinterpret_cast<void**>(s + 0x2098));
    }
    Log("SHOW slot value %d", *reinterpret_cast<int*>(g_base + kShowSlot));
}

static void LogStats() {
    std::lock_guard<std::mutex> l(g_statsLock);
    for (auto& e : g_cameraSubmits) Log("CAMERA submit from rva %zx to view %p: %llu", e.first.first, e.first.second, e.second);
    for (auto& e : g_sceneSubmits) Log("SCENE submit view %p: %llu mask %x", e.first, e.second, g_sceneMask[e.first]);
    Log("FRAMES %llu", g_frames.load());
    g_cameraSubmits.clear();
    g_sceneSubmits.clear();
}

// The engine's texture wraps the resource: view +0x2088 texture, +0x40 wrapper, +0x38 record, +0 the ID3D12Resource,
// record +0x28 the state the engine tracks. Only trusted if its vtable is in D3D12Core.dll.
static uint8_t* EyeRecord(uint8_t* eye) {
    uint8_t* tex = *reinterpret_cast<uint8_t**>(eye + 0x2088);
    uint8_t* wrap = tex ? *reinterpret_cast<uint8_t**>(tex + 0x40) : nullptr;
    return wrap ? *reinterpret_cast<uint8_t**>(wrap + 0x38) : nullptr;
}

static void* EyeTexture(uint8_t* eye) {
    uint8_t* rec = EyeRecord(eye);
    void* res = nullptr;
    if (!rec || !SafeCopy(&res, rec, 8) || !res) return nullptr;
    void* vt = nullptr;
    HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
    HMODULE owner = nullptr;
    if (!core || !SafeCopy(&vt, res, 8) || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(vt), &owner) || owner != core) {
        Log("EYE resource not in D3D12Core: %p vtable %p", res, vt);
        return nullptr;
    }
    return res;
}

static void DumpEyes(const std::string& name) {
    std::lock_guard<std::mutex> l(g_eyesLock);
    CreateDirectoryW((g_dir + L"vrlab_shots").c_str(), nullptr);
    std::vector<void*> res;
    std::vector<uint32_t> states;
    std::vector<std::wstring> paths;
    int i = 0;
    for (uint8_t* eye : g_eyes) {
        void* r = EyeTexture(eye);
        if (!r) continue;
        res.push_back(r);
        states.push_back(*reinterpret_cast<uint32_t*>(EyeRecord(eye) + 0x28));
        paths.push_back(g_dir + L"vrlab_shots\\" + std::wstring(name.begin(), name.end()) + (i == 0 ? L"_L.bmp" : i == 1 ? L"_R.bmp" : L"_X.bmp"));
        ++i;
    }
    std::vector<const wchar_t*> ptrs;
    for (auto& p : paths) ptrs.push_back(p.c_str());
    const bool ok = !res.empty() && CaptureRequest(res.data(), states.data(), ptrs.data(), static_cast<int>(res.size()));
    Log("DUMP queued %d images: %s", static_cast<int>(res.size()), ok ? "yes" : "no");
}

// Reads only: lists pointers in an engine texture object and which module each one's first word (a vtable) lies in.
static void WalkObject(const char* label, const uint8_t* obj, size_t size) {
    Log("WALK %s %p", label, obj);
    for (size_t off = 0; off < size; off += 8) {
        void* v = nullptr;
        if (!SafeCopy(&v, obj + off, 8) || reinterpret_cast<uint64_t>(v) < 0x10000) continue;
        void* vt = nullptr;
        if (!SafeCopy(&vt, v, 8)) continue;
        HMODULE own = nullptr, of = nullptr;
        wchar_t a[64] = L"-", b[64] = L"-";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(v), &own)) {
            wchar_t full[MAX_PATH];
            GetModuleFileNameW(own, full, MAX_PATH);
            wcsncpy(a, wcsrchr(full, L'\\') + 1, 63);
        }
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(vt), &of)) {
            wchar_t full[MAX_PATH];
            GetModuleFileNameW(of, full, MAX_PATH);
            wcsncpy(b, wcsrchr(full, L'\\') + 1, 63);
        }
        Log("  +%#05zx %p in %ls first word %p in %ls", off, v, a, vt, b);
    }
}

static void TexInfo() {
    std::lock_guard<std::mutex> l(g_eyesLock);
    if (g_eyes.empty()) return;
    uint8_t* tex = *reinterpret_cast<uint8_t**>(g_eyes[0] + 0x2088);
    WalkObject("texture", tex, 0x100);
    uint8_t* inner = *reinterpret_cast<uint8_t**>(tex + 0x40);
    WalkObject("inner", inner, 0x200);
}

static void OnPresentForXr() {
    std::lock_guard<std::mutex> l(g_eyesLock);
    if (g_eyes.size() < 2) return;
    void* res[2] = {EyeTexture(g_eyes[0]), EyeTexture(g_eyes[1])};
    uint32_t st[2] = {};
    if (res[0] && res[1]) {
        st[0] = *reinterpret_cast<uint32_t*>(EyeRecord(g_eyes[0]) + 0x28);
        st[1] = *reinterpret_cast<uint32_t*>(EyeRecord(g_eyes[1]) + 0x28);
    }
    XrOnPresent(res, st);
}

static bool g_xrStarted = false;
static DWORD WINAPI XrStartThread(void* arg);
static void StartXr(int percent) {
    if (g_xrStarted) return;
    g_xrStarted = true;
    if (HANDLE t = CreateThread(nullptr, 0, XrStartThread, reinterpret_cast<void*>(static_cast<intptr_t>(percent)), 0, nullptr)) CloseHandle(t);
}

// Both sticks clicked and held half a second: recenter. Reads the pad through XInput, loaded only here.
static void PollPad() {
    using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
    static GetStateFn get = [] {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        return m ? reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
    }();
    if (!get || !g_xrEyes) return;
    static ULONGLONG since = 0;
    static bool fired = false;
    bool held = false;
    for (DWORD pad = 0; pad < 4 && !held; ++pad) {
        struct {
            DWORD packet;
            WORD buttons;
            uint8_t rest[10];
        } st{};
        if (get(pad, &st) == 0 && (st.buttons & 0x00C0) == 0x00C0) held = true;  // left and right thumb
    }
    const ULONGLONG now = GetTickCount64();
    if (!held) {
        since = 0;
        fired = false;
    } else if (!since) {
        since = now;
    } else if (!fired && now - since >= 500) {
        fired = true;
        g_wantRecenter = true;
        Log("PAD both sticks: recenter");
    }
}

// Runs on a thread of its own: `xr <scale percent>`.
static DWORD WINAPI XrStartThread(void* arg) {
    const int percent = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    if (!XrInit(g_dir.c_str())) return 0;
    int w = 0, h = 0;
    XrRecommended(&w, &h);
    w = (w * percent / 100 + 7) & ~7;
    h = (h * percent / 100 + 7) & ~7;
    Log("XR eye size %dx%d (%d percent of recommended)", w, h, percent);
    g_forceW = w;
    g_forceH = h;
    g_createViewKind = 1;  // kinds 3 and up turn particles black. Kinds below 2 get occlusion culling.
    g_shareLum = true;
    g_createW = w;
    g_createH = h;
    g_createKind = 1;
    for (int i = 0; i < 2; ++i) {
        g_wantCreate = 1;
        while (g_wantCreate.load()) Sleep(20);
        Sleep(1500);
    }
    {
        std::lock_guard<std::mutex> l(g_eyesLock);
        if (g_eyes.size() < 2) {
            Log("XR eyes not created");
            return 0;
        }
        for (uint8_t* eye : g_eyes) *reinterpret_cast<uint32_t*>(eye + 0x16cd4) = 0x3fff;  // the same passes as the main view
        void* r = EyeTexture(g_eyes[0]);
        if (!r) return 0;
        CaptureSetup(r);
    }
    for (int i = 0; i < 500 && !CaptureReady(); ++i) Sleep(20);
    if (!CaptureReady()) {
        Log("XR capture not ready");
        return 0;
    }
    if (!XrStartSession(CaptureDevice(), CaptureQueue(), w, h)) return 0;
    g_xrEyes = true;
    CaptureSetPresentCallback(&OnPresentForXr);
    Log("XR running");
    return 0;
}

// Coherent UI views: a table of 0x150 byte records. +0x20 render target data, whose first word is the colour texture.
static const size_t kUiViews = 0x687d310;
static uint8_t* UiTexture(int i, int* w, int* h) {
    uint8_t* v = g_base + kUiViews + static_cast<size_t>(i) * 0x150;
    uint8_t* rt = nullptr;
    if (!SafeCopy(&rt, v + 0x20, 8) || !rt) return nullptr;
    uint8_t* tex = nullptr;
    if (!SafeCopy(&tex, rt, 8) || !tex) return nullptr;
    SafeCopy(w, v + 300, 4);
    SafeCopy(h, v + 0x130, 4);
    return tex;
}

static void* TextureResource(uint8_t* tex, uint32_t* state) {
    uint8_t* wrap = nullptr;
    uint8_t* rec = nullptr;
    void* res = nullptr;
    if (!SafeCopy(&wrap, tex + 0x40, 8) || !wrap || !SafeCopy(&rec, wrap + 0x38, 8) || !rec || !SafeCopy(&res, rec, 8) || !res) return nullptr;
    SafeCopy(state, rec + 0x28, 4);
    return res;
}

static void UiList(bool dump, const std::string& name) {
    std::vector<void*> res;
    std::vector<uint32_t> states;
    std::vector<std::wstring> paths;
    for (int i = 0; i < 64; ++i) {
        int w = 0, h = 0;
        uint8_t* tex = UiTexture(i, &w, &h);
        if (!tex) continue;
        uint32_t st = 0;
        void* r = TextureResource(tex, &st);
        Log("UI view %d %dx%d texture %p resource %p state %x", i, w, h, tex, r, st);
        if (dump && r) {
            res.push_back(r);
            states.push_back(st);
            paths.push_back(g_dir + L"vrlab_shots\\" + std::wstring(name.begin(), name.end()) + L"_ui" + std::to_wstring(i) + L".bmp");
        }
    }
    if (dump && !res.empty()) {
        std::vector<const wchar_t*> ptrs;
        for (auto& p : paths) ptrs.push_back(p.c_str());
        CreateDirectoryW((g_dir + L"vrlab_shots").c_str(), nullptr);
        Log("UI dump queued: %s", CaptureRequest(res.data(), states.data(), ptrs.data(), static_cast<int>(res.size())) ? "yes" : "no");
    }
}

static uint32_t g_w, g_h, g_k;
static float g_f;
static char g_text[64];
static int g_show;

static void RunCommand(const std::string& c) {
    Log("CMD %s", c.c_str());
    if (c == "probe") LogView();
    else if (c == "manager") LogManager();
    else if (c == "stats") LogStats();
    else if (sscanf(c.c_str(), "eye %u %u %u", &g_w, &g_h, &g_k) == 3) {
        g_createW = g_w;
        g_createH = g_h;
        g_createKind = g_k;
        g_wantCreate = 1;
    } else if (sscanf(c.c_str(), "viewkind %u", &g_w) == 1) g_createViewKind = g_w;
    else if (sscanf(c.c_str(), "xr %u", &g_w) == 1) StartXr(g_w);
    else if (sscanf(c.c_str(), "pace %u", &g_w) == 1) g_pace = g_w != 0;
    else if (sscanf(c.c_str(), "sharelum %u", &g_w) == 1) g_shareLum = g_w != 0;
    else if (sscanf(c.c_str(), "sharefog %u", &g_w) == 1) g_shareFog = g_w != 0;
    else if (sscanf(c.c_str(), "tonefix %u", &g_w) == 1) g_toneFix = g_w != 0;
    else if (c == "recenter") g_wantRecenter = true;
    else if (sscanf(c.c_str(), "scale %f", &g_f) == 1) g_worldScale = g_f;
    else if (c == "d3d") {
        std::lock_guard<std::mutex> l(g_eyesLock);
        void* r = g_eyes.empty() ? nullptr : EyeTexture(g_eyes[0]);
        Log("D3D start from %p", r);
        if (r) CaptureSetup(r);
    } else if (sscanf(c.c_str(), "stereo %f", &g_f) == 1) {
        g_ipd = g_f;
        g_copyCamera = false;
        g_xrEyes = false;
        g_stereo = true;
    } else if (sscanf(c.c_str(), "dump %63s", g_text) == 1) DumpEyes(g_text);
    else if (c == "texinfo") TexInfo();
    else if (c == "uilist") UiList(false, "");
    else if (sscanf(c.c_str(), "uidump %63s", g_text) == 1) UiList(true, g_text);
    else if (c == "lens") {
        if (uint8_t* m = ActiveView()) {
            const float* v = reinterpret_cast<const float*>(m + 0x3f8);
            Log("LENS near %.4f far %.4f edges %.5f %.5f %.5f %.5f fov %.4f %.4f %.4f aspect %.4f", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
        }
    }
    else if (c == "copy 1") g_copyCamera = true;
    else if (c == "copy 0") g_copyCamera = false;
    else if (sscanf(c.c_str(), "show %d", &g_show) == 1) *reinterpret_cast<int*>(g_base + kShowSlot) = g_show;
    else if (sscanf(c.c_str(), "emask %x", &g_w) == 1) {
        std::lock_guard<std::mutex> l(g_eyesLock);
        for (uint8_t* eye : g_eyes) *reinterpret_cast<uint32_t*>(eye + 0x16cd4) = g_w;
    }
}

static DWORD WINAPI ProbeThread(void*) {
    FindSections();
    FindActiveView();
    InstallHooks();
    const std::wstring ini = g_dir + L"RiftApartVR.ini";
    const bool autoStart = GetPrivateProfileIntW(L"VR", L"AutoStart", 1, ini.c_str()) != 0;
    const int resolution = GetPrivateProfileIntW(L"VR", L"Resolution", 50, ini.c_str());
    g_worldScale = GetPrivateProfileIntW(L"VR", L"WorldScalePercent", 100, ini.c_str()) / 100.0f;
    g_pace = GetPrivateProfileIntW(L"VR", L"Pace", 1, ini.c_str()) != 0;
    Log("INI AutoStart %d Resolution %d WorldScale %.2f Pace %d", autoStart ? 1 : 0, resolution, g_worldScale.load(), g_pace.load() ? 1 : 0);
    ULONGLONG inPlaySince = 0;
    const std::wstring cmdPath = g_dir + L"vrlab_command.txt";
    DeleteFileW(cmdPath.c_str());
    long pos = 0;
    ULONGLONG lastView = 0;
    for (;;) {
        Sleep(100);
        if (FILE* f = _wfopen(cmdPath.c_str(), L"rb")) {
            fseek(f, pos, SEEK_SET);
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                std::string s(line);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                if (!s.empty()) RunCommand(s);
            }
            pos = ftell(f);
            fclose(f);
        }
        const ULONGLONG now = GetTickCount64();
        PollPad();
        if (autoStart && !g_xrStarted && g_activeViewPtr) {
            // Start once the game camera has been away from the origin (title and loading sit at 0) for three seconds.
            void* view = nullptr;
            float pos[3] = {};
            const bool read = SafeCopy(&view, g_activeViewPtr, 8) && view && SafeCopy(pos, static_cast<uint8_t*>(view) + 0x30, sizeof(pos));
            const bool away = read && fabsf(pos[0]) + fabsf(pos[1]) + fabsf(pos[2]) > 1.0f;
            if (!away) {
                inPlaySince = 0;
            } else if (!inPlaySince) {
                inPlaySince = now;
            } else if (now - inPlaySince >= 3000) {
                Log("AUTOSTART camera away from origin for 3 s");
                StartXr(resolution);
            }
        }
        if (now - lastView >= 5000) {
            lastView = now;
            LogView();
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID) {
    if (why != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    g_start = GetTickCount64();

    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring realPath = std::wstring(sys) + L"\\winhttp.dll";
    HMODULE real = LoadLibraryExW(realPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (real) {
        for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) g_realWinhttp[i] = GetProcAddress(real, kNames[i]);
    }

    // crs-handler.exe and crs-video.exe sit in the same folder and load this proxy too. They only get the forwarding.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* exeName = wcsrchr(exe, L'\\');
    if (!exeName || _wcsicmp(exeName + 1, L"RiftApart.exe") != 0) return TRUE;

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(inst, path, MAX_PATH);
    g_dir = path;
    g_dir.resize(g_dir.find_last_of(L'\\') + 1);
    g_log = _wfopen((g_dir + L"RiftApartVR.log").c_str(), L"w");
    g_base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    SYSTEMTIME st;
    GetLocalTime(&st);
    Log("RiftApartVR build %d loaded at %02d:%02d:%02d.%03d, real winhttp %p", BUILD_NUMBER, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, real);

    if (HANDLE t = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr)) CloseHandle(t);
    return TRUE;
}
