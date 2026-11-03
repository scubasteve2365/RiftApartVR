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

#define BUILD_NUMBER 34

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
static const Target kUpscalePass{"UpscalePass", 0x11df5a0, "\x85\xd2\x0f\x8e\xbf\x0c"};
static const Target kSlConstants{"SlConstants", 0x1238930, "\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10"};
static const Target kUpEval{"UpscalerEvaluate", 0x16e87b0, "\x40\x55\x53\x56\x57\x41\x54\x41\x55"};
static const Target kUpEvalDlss{"DlssEvaluate", 0x16e8100, "\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10"};
static const Target kCameraPush{"CameraPush", 0xfc7110, "\x40\x53\x57\x41\x56\x48\x81\xec\x30\x05"};
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
static std::atomic<ULONGLONG> g_rebuildTick{0};
static std::atomic<uint32_t> g_createViewKind{0};  // 3 makes the engine treat the view as a child of the main view
using ViewCreateFn = void(__fastcall*)(uint8_t*, const char*, void*, void*, void*, uint64_t, void*, uint32_t);
static ViewCreateFn g_origViewCreate;
static uint8_t* ActiveView();
static void __fastcall HookViewCreate(uint8_t* view, const char* name, void* buffers, void* a4, void* a5, uint64_t kind, void* parent, uint32_t a8) {
    if (name == kEyeName) g_rebuildTick = GetTickCount64();
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
// Percent of the eye's size that the scene is drawn at, DLSS making up the rest. 100 is DLAA.
static std::atomic<int> g_eyeRenderPercent{67};
static std::atomic<uint32_t> g_eyeRenderW{0}, g_eyeRenderH{0};
static std::atomic<uint32_t> g_forceW{0}, g_forceH{0};
static void __fastcall HookBufferSize(uint8_t* buffers, const char* name, uint32_t w, uint32_t h, int kind, char align) {
    const bool ours = name == kEyeName && g_forceW != 0;
    uint32_t outW = w, outH = h;
    if (ours) {
        // The size passed in, kept at +0x14908, is what the scene is drawn at. +0x14910 on is the size after upscaling.
        outW = g_forceW;
        outH = g_forceH;
        // Only DLSS can make up the difference. With any other choice in the game's menu the eyes draw at full size.
        const int aaType = *reinterpret_cast<int*>(g_base + 0x533959c);
        const int percent = aaType == 0 ? g_eyeRenderPercent.load() : 100;
        Log("EYE scene percent %d (setting %d, game aa type %d)", percent, g_eyeRenderPercent.load(), aaType);
        w = (outW * percent / 100 + 7) & ~7u;
        h = (outH * percent / 100 + 7) & ~7u;
        g_eyeRenderW = w;
        g_eyeRenderH = h;
        *reinterpret_cast<void**>(buffers + 0x14990) = nullptr;  // no sharing with, or clamping to, the main view's set
    }
    g_origBufferSize(buffers, name, w, h, kind, align);
    if (ours) {
        for (size_t off : {size_t{0x14910}, size_t{0x14918}, size_t{0x14920}}) {
            reinterpret_cast<uint32_t*>(buffers + off)[0] = outW;
            reinterpret_cast<uint32_t*>(buffers + off)[1] = outH;
        }
        Log("EYE buffers %p output %ux%u scene %ux%u kind %d", buffers, outW, outH, w, h, kind);
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
    if (view) {
        // The render thread's copy of the view holds the camera this image was drawn with.
        uint8_t* owner = *reinterpret_cast<uint8_t**>(view + 0x1ff8);
        if (owner && owner == g_eyeA.load()) XrMarkRendered(reinterpret_cast<const float*>(view + 0x30));
    }
    return g_origTone(a, b, c, view, e, generic);
}

// ---- what kind of camera is the game using? MSVC RTTI names the class. ----
static std::string RttiName(void* obj) {
    uint8_t* vt = nullptr;
    if (!SafeCopy(&vt, obj, 8) || !vt) return "?";
    uint8_t* col = nullptr;
    if (!SafeCopy(&col, vt - 8, 8) || !col) return "?";
    int32_t td = 0;
    if (!SafeCopy(&td, col + 0xc, 4)) return "?";
    char name[96] = {};
    if (!SafeCopy(name, g_base + td + 0x10, sizeof(name) - 1)) return "?";
    return name;
}
using CameraPushFn = void(__fastcall*)(int64_t*, int);
static CameraPushFn g_origCameraPush;
static std::mutex g_camLock;
static void* g_lastCamVt[8];
static std::string g_camClass;
static uint8_t* MainCameraObject(int64_t* sys, int slot) {
    int64_t cam = 0;
    int64_t list = 0;
    if (SafeCopy(&list, sys + 0xf, 8) && list) SafeCopy(&cam, reinterpret_cast<int64_t*>(list) + slot, 8);
    if (!cam) {
        SafeCopy(&list, sys, 8);
        if (list) SafeCopy(&cam, reinterpret_cast<int64_t*>(list) + slot, 8);
    }
    return reinterpret_cast<uint8_t*>(cam);
}
static std::atomic<bool> g_wantCamTree{false};
// Lists fields of a camera object that point at objects with RTTI, following those in the Camera2 family.
static void WalkCam(uint8_t* obj, int depth) {
    if (depth > 3) return;
    for (size_t off = 8; off < 0x400; off += 8) {
        uint8_t* p = nullptr;
        if (!SafeCopy(&p, obj + off, 8) || reinterpret_cast<uint64_t>(p) < 0x10000 || p == obj) continue;
        uint8_t* vt = nullptr;
        if (!SafeCopy(&vt, p, 8)) continue;
        if (vt < g_base + 0x2e37000 || vt >= g_base + 0x512c000) continue;  // vtables live in .rdata
        const std::string n = RttiName(p);
        if (n.size() < 4 || n[0] != '.' || n[1] != '?') continue;
        if (n.find("Camera") == std::string::npos && n.find("Cine") == std::string::npos && n.find("Hero") == std::string::npos) continue;
        Log("CAMTREE %*s+%zx %p %s", depth * 2, "", off, p, n.c_str());
        if (n.find("Camera2") != std::string::npos) WalkCam(p, depth + 1);
    }
}
static void __fastcall HookCameraPush(int64_t* sys, int slot) {
    if (slot >= 0 && slot < 8) {
        if (uint8_t* cam = MainCameraObject(sys, slot)) {
            void* vt = nullptr;
            if (SafeCopy(&vt, cam, 8) && vt != g_lastCamVt[slot]) {
                g_lastCamVt[slot] = vt;
                const std::string n = RttiName(cam);
                Log("CAMCLASS slot %d %s", slot, n.c_str());
                if (slot == 0) {
                    std::lock_guard<std::mutex> l(g_camLock);
                    g_camClass = n;
                }
            }
        }
    }
    if (slot == 0 && g_wantCamTree.exchange(false)) {
        Log("CAMTREE system %p %s", sys, RttiName(sys).c_str());
        // Report every RTTI object reachable from the camera system: fields, and pointer arrays in its fields.
        for (size_t off = 0; off < 0x300; off += 8) {
            uint8_t* p = nullptr;
            if (!SafeCopy(&p, reinterpret_cast<uint8_t*>(sys) + off, 8) || reinterpret_cast<uint64_t>(p) < 0x10000) continue;
            const std::string n = RttiName(p);
            if (n.size() > 4 && n[0] == '.') Log("CAMTREE +%zx %p %s", off, p, n.c_str());
            for (size_t j = 0; j < 12; ++j) {
                uint8_t* q = nullptr;
                if (!SafeCopy(&q, p + j * 8, 8) || reinterpret_cast<uint64_t>(q) < 0x10000) continue;
                const std::string m = RttiName(q);
                if (m.size() > 4 && m[0] == '.') {
                    Log("CAMTREE +%zx[%zu] %p %s", off, j, q, m.c_str());
                    if (m.find("Camera2") != std::string::npos) WalkCam(q, 1);
                }
            }
        }
    }
    g_origCameraPush(sys, slot);
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
static bool g_haveCentre = false;
static std::atomic<bool> g_xrEyes{false};
static std::atomic<int> g_wantCreate{0};
static std::atomic<uint32_t> g_createW{1920}, g_createH{1080}, g_createKind{2};
static void CreateEyeImpl();
static std::vector<uint8_t*> g_eyes;
static std::mutex g_eyesLock;
static std::atomic<bool> g_copyCamera{false};
static std::atomic<bool> g_stereo{false};
static std::atomic<float> g_ipd{0.064f};
static std::atomic<bool> g_wantRecenter{false}, g_pace{true}, g_shareLum{false};
static std::atomic<float> g_worldScale{1.0f};
static std::atomic<bool> g_levelCamera{true};
// The engine's lens takes the top of the picture first, measured along its downward Y: minus OpenXR's up, then minus its down.
static std::atomic<bool> g_lensFix{true};
// Eye jitter: 0 none, 1 the game's value resized from the flat view's pixels to the eye's, 2 the flat view's value as it is.
static std::atomic<int> g_eyeJitter{1};
static float g_lastFacing[2] = {0, 1};
static float g_centre[3], g_centreYaw;
// Off-centre perspective from edges, then the pose: the same two steps the game's own camera builders end with.
// Edges are tangents: left, right, bottom, top, as OpenXR gives them.
using LensPerspFn = void(__fastcall*)(void*, float, float, float, float, float, float, float, float, float, float, char, char);
using LensPoseFn = void(__fastcall*)(void*, const float*);
using JitterFn = void(__fastcall*)(uint8_t*, float*, float*);

static uint8_t* ActiveView() {
    return g_activeViewPtr ? static_cast<uint8_t*>(*g_activeViewPtr) : nullptr;
}

static void CreateEyeImpl() {
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
        *reinterpret_cast<uint32_t*>(eye + 0x16cd4) = 0x3fff;  // the same passes as the main view
        if (g_eyes.empty()) g_eyeA = eye;
        else if (g_eyes.size() == 1) g_eyeB = eye;
        g_eyes.push_back(eye);
    }
}

// ---- flat screen or 3D world ----
// The title sits at the origin with a 39.43 degree lens. Anything else that lasts a second is treated as the world.
static std::atomic<bool> g_xrSession{false};
static void CreateEyeNow();
static void UpdateMode() {
    uint8_t* main = ActiveView();
    bool world = false;
    if (main) {
        const float* cam = reinterpret_cast<const float*>(main);
        const float* proj = reinterpret_cast<const float*>(main + 0x1c0);
        const bool atOrigin = fabsf(cam[12]) + fabsf(cam[13]) + fabsf(cam[14]) < 1.0f;
        const double vfov = proj[5] != 0 ? 2.0 * atan(1.0 / fabs(proj[5])) * 57.29577951 : 0;
        // The title's fly-by holds one spot with a 33.3 degree lens.
        const bool titleSpot = fabsf(cam[12] + 17.55f) + fabsf(cam[13] - 39.476f) + fabsf(cam[14] + 223.651f) < 1.0f;
        world = !atOrigin && !titleSpot && fabs(vfov - 39.43) > 0.05;
    }
    static ULONGLONG since = 0;
    const ULONGLONG now = GetTickCount64();
    if (!world) {
        since = 0;
    } else if (!since) {
        since = now;
    }
    const bool wantEyes = world && now - since >= 1000;
    if (wantEyes && g_eyes.size() < 2) {
        CreateEyeNow();
        return;
    }
    if (wantEyes != g_xrEyes.load()) {
        Log("MODE %s", wantEyes ? "3D world" : "flat screen");
        std::lock_guard<std::mutex> l(g_eyesLock);
        for (uint8_t* eye : g_eyes) {
            // Flag bit 0 makes the engine skip an offscreen view: the eyes cost nothing on the flat screen.
            uint32_t& flags = *reinterpret_cast<uint32_t*>(eye + 0x16ccc);
            flags = wantEyes ? (flags & ~1u) : (flags | 1u);
        }
        g_haveCentre = wantEyes && g_haveCentre;
        g_xrEyes = wantEyes;
    }
}

static void CreateEyeNow() { CreateEyeImpl(); }

static void __fastcall HookRenderAll() {
    ++g_frames;
    if (g_xrSession && g_pace) XrPaceWait();
    if (g_xrSession) UpdateMode();
    if (g_wantCreate.exchange(0)) CreateEyeNow();
    {
        // Moved below: placeholder kept empty.
    }
    if (false) {
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
                if (g_levelCamera) {
                    // A solid, level world: keep only the direction the camera faces. Its pitch and roll never reach the head.
                    float fx = cam[8], fz = cam[10];
                    const float len = sqrtf(fx * fx + fz * fz);
                    if (len > 0.05f) {
                        fx /= len;
                        fz /= len;
                        g_lastFacing[0] = fx;
                        g_lastFacing[1] = fz;
                    } else {
                        fx = g_lastFacing[0];
                        fz = g_lastFacing[1];
                    }
                    const float level[12] = {-fz, 0, fx, 0, 0, -1, 0, 0, fx, 0, fz, 0};
                    memcpy(cam, level, sizeof(level));
                }
                const float nearZ = *reinterpret_cast<float*>(main + 0x3f8), farZ = *reinterpret_cast<float*>(main + 0x3fc);
                float jx = 0, jy = 0;
                reinterpret_cast<JitterFn>(g_base + kJitter.rva)(main, &jx, &jy);
                {
                    // The game scales jitter by the size at +0x448, +0x44c: one pixel of the flat view, not of an eye.
                    const int mode = g_eyeJitter.load();
                    const float mw = *reinterpret_cast<float*>(main + 0x448), mh = *reinterpret_cast<float*>(main + 0x44c);
                    if (mode == 0) {
                        jx = 0;
                        jy = 0;
                    } else if (mode == 1 && g_eyeRenderW.load() && g_eyeRenderH.load() && mw > 0 && mh > 0) {
                        jx *= mw / static_cast<float>(g_eyeRenderW.load());
                        jy *= mh / static_cast<float>(g_eyeRenderH.load());
                    }
                }
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
                    if (e == 0) XrTagPose(eyePose + 12);
                    const bool fix = g_lensFix.load();
                    reinterpret_cast<LensPerspFn>(g_base + kLensPersp.rva)(desc, nearZ, farZ, p.tanLeft, p.tanRight, fix ? -p.tanUp : p.tanDown, fix ? -p.tanDown : p.tanUp, 0, 0, jx, jy, 0, 0);
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

// Resting the flat view: the scene submit returns at once when view +0x11d8 is clear. Set again when not resting.
static std::atomic<bool> g_restFlat{false};
static void* __fastcall HookSceneSubmit(uint8_t* view, void* a, uint32_t mask, char b) {
    if (view == ActiveView()) {
        static uint8_t saved = 1;
        static bool resting = false;
        const bool want = g_restFlat && g_xrEyes;
        uint8_t* flag = view + 0x11d8;
        if (want && !resting) {
            saved = *flag;
            resting = true;
            Log("REST flat view, flag was %u", saved);
        }
        if (want) {
            *flag = 0;
        } else if (resting) {
            *flag = saved;
            resting = false;
            Log("REST flat view off");
        }
    }
    {
        std::lock_guard<std::mutex> l(g_statsLock);
        ++g_sceneSubmits[view];
        g_sceneMask[view] = mask;
    }
    // Eyes read the main view's Adapted Lum, so they have the exposure the game chose. The handle is only lent while
    // the scene submit copies the view into its job: if it stayed, rebuilding an eye would free the main view's buffer.
    uint8_t* main = g_shareLum ? ActiveView() : nullptr;
    if (main && view != main && (view == g_eyeA.load() || view == g_eyeB.load())) {
        uint8_t saved[0x38];
        memcpy(saved, view + 0x1740, sizeof(saved));
        memcpy(view + 0x1740, main + 0x1740, sizeof(saved));
        void* r = g_origSceneSubmit(view, a, mask, b);
        memcpy(view + 0x1740, saved, sizeof(saved));
        return r;
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
    if (!Check(kOffscreenRenderAll) || !Check(kCameraSubmit) || !Check(kOffscreenCreate) || !Check(kSceneSubmit) || !Check(kViewCreate) || !Check(kLensPersp) || !Check(kLensPose) || !Check(kJitter) || !Check(kBufferSize) || !Check(kTone) || !Check(kCameraPush)) return;
    MH_Initialize();
    MH_CreateHook(g_base + kOffscreenRenderAll.rva, reinterpret_cast<void*>(&HookRenderAll), reinterpret_cast<void**>(&g_origRenderAll));
    MH_CreateHook(g_base + kSceneSubmit.rva, reinterpret_cast<void*>(&HookSceneSubmit), reinterpret_cast<void**>(&g_origSceneSubmit));
    MH_CreateHook(g_base + kCameraSubmit.rva, reinterpret_cast<void*>(&HookCameraSubmit), reinterpret_cast<void**>(&g_origCameraSubmit));
    MH_CreateHook(g_base + kViewCreate.rva, reinterpret_cast<void*>(&HookViewCreate), reinterpret_cast<void**>(&g_origViewCreate));
    MH_CreateHook(g_base + kCameraPush.rva, reinterpret_cast<void*>(&HookCameraPush), reinterpret_cast<void**>(&g_origCameraPush));
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

// ---- keep the game running without Windows focus, only while VR is on ----
static std::atomic<HWND> g_gameWindow{nullptr};
static std::atomic<bool> g_keepFocus{false};
static WNDPROC g_origWndProc;
static std::atomic<uint64_t> g_focusDropped{0};
using GetWindowFn = HWND(WINAPI*)();
static GetWindowFn g_origGetForeground, g_origGetActive, g_origGetFocus;
static HWND WINAPI HookGetForeground() {
    HWND g = g_gameWindow;
    return g_keepFocus && g ? g : g_origGetForeground();
}
static HWND WINAPI HookGetActive() {
    HWND g = g_gameWindow;
    return g_keepFocus && g ? g : g_origGetActive();
}
static HWND WINAPI HookGetFocus() {
    HWND g = g_gameWindow;
    return g_keepFocus && g ? g : g_origGetFocus();
}
static LRESULT CALLBACK FocusWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_keepFocus) {
        const bool losing = (m == WM_ACTIVATEAPP && w == FALSE) || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) || m == WM_KILLFOCUS || (m == WM_NCACTIVATE && w == FALSE);
        if (losing) {
            ++g_focusDropped;
            return m == WM_NCACTIVATE ? TRUE : 0;
        }
    }
    return CallWindowProcW(g_origWndProc, h, m, w, l);
}
static BOOL CALLBACK FindGameWindow(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && wcscmp(cls, L"GameNxApp") == 0) {
        g_gameWindow = h;
        return FALSE;
    }
    return TRUE;
}
static void InstallFocusFix() {
    EnumWindows(FindGameWindow, 0);
    HWND g = g_gameWindow;
    if (!g) {
        Log("FOCUS game window not found");
        return;
    }
    HMODULE u = GetModuleHandleW(L"user32.dll");
    MH_CreateHook(GetProcAddress(u, "GetForegroundWindow"), reinterpret_cast<void*>(&HookGetForeground), reinterpret_cast<void**>(&g_origGetForeground));
    MH_CreateHook(GetProcAddress(u, "GetActiveWindow"), reinterpret_cast<void*>(&HookGetActive), reinterpret_cast<void**>(&g_origGetActive));
    MH_CreateHook(GetProcAddress(u, "GetFocus"), reinterpret_cast<void*>(&HookGetFocus), reinterpret_cast<void**>(&g_origGetFocus));
    MH_EnableHook(GetProcAddress(u, "GetForegroundWindow"));
    MH_EnableHook(GetProcAddress(u, "GetActiveWindow"));
    MH_EnableHook(GetProcAddress(u, "GetFocus"));
    g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FocusWndProc)));
    g_keepFocus = true;
    Log("FOCUS fix on, window %p", g);
}

static uint8_t* UiTexture(int i, int* w, int* h);
static void* TextureResource(uint8_t* tex, uint32_t* state);
static std::atomic<int> g_uiCount{3};

static void OnPresentForXr(void* backBuffer, unsigned bbW, unsigned bbH, int bbFormat) {
    if (!g_xrEyes) {
        XrOnPresentScreen(backBuffer, static_cast<int>(bbW), static_cast<int>(bbH), bbFormat);
        return;
    }
    std::lock_guard<std::mutex> l(g_eyesLock);
    if (g_eyes.size() < 2) return;
    // The engine rebuilds eye textures when graphics settings change. Send nothing until that has settled.
    const bool rebuilding = GetTickCount64() - g_rebuildTick.load() < 1500;
    void* res[2] = {rebuilding ? nullptr : EyeTexture(g_eyes[0]), rebuilding ? nullptr : EyeTexture(g_eyes[1])};
    uint32_t st[2] = {};
    if (res[0] && res[1]) {
        SafeCopy(&st[0], EyeRecord(g_eyes[0]) + 0x28, 4);
        SafeCopy(&st[1], EyeRecord(g_eyes[1]) + 0x28, 4);
    }
    XrUiImage ui[3] = {};
    const int ids[3] = {0, 3, 4};  // UI views the size of the screen: 0 and 4 not yet identified, 3 the pause menu
    for (int i = 0; i < 3 && !rebuilding; ++i) {
        uint8_t* tex = UiTexture(ids[i], &ui[i].width, &ui[i].height);
        if (tex) ui[i].resource = TextureResource(tex, &ui[i].state);
    }
    XrOnPresent(res, st, ui, g_uiCount.load());
}

static bool g_xrStarted = false;
static DWORD WINAPI XrStartThread(void* arg);
static void StartXr(int percent) {
    if (g_xrStarted) return;
    g_xrStarted = true;
    if (HANDLE t = CreateThread(nullptr, 0, XrStartThread, reinterpret_cast<void*>(static_cast<intptr_t>(percent)), 0, nullptr)) CloseHandle(t);
}

// ---- mod menu ----
// Opens with both sticks clicked and held half a second. Settings save to RiftApartVR.ini as they change.
static std::atomic<bool> g_menuOpen{false};
static int g_menuRow = 0;
static int g_resolutionSetting = 100;
static bool g_skipLauncherSetting = true;
static int g_dlssPercentSetting = 67;  // 0 means no DLSS in the eyes: the game's own anti-aliasing
static float g_hudDistanceSetting = 2.0f;
static std::wstring g_iniPath;
static std::vector<uint32_t> g_menuImage;
static const int kMenuW = 1024, kMenuH = 768, kMenuRows = 8;

static void SaveSetting(const wchar_t* name, int value) {
    WritePrivateProfileStringW(L"VR", name, std::to_wstring(value).c_str(), g_iniPath.c_str());
}

static void MenuRowText(int row, wchar_t (&name)[64], wchar_t (&value)[32]) {
    value[0] = 0;
    switch (row) {
    case 0: wcscpy_s(name, L"Recenter view"); break;
    case 1:
        wcscpy_s(name, L"World scale");
        swprintf_s(value, L"%d%%", static_cast<int>(g_worldScale.load() * 100 + 0.5f));
        break;
    case 2:
        wcscpy_s(name, L"Decoupled pitch");
        wcscpy_s(value, g_levelCamera ? L"On" : L"Off");
        break;
    case 3:
        wcscpy_s(name, L"HUD distance");
        swprintf_s(value, L"%.2f m", g_hudDistanceSetting);
        break;
    case 4:
        wcscpy_s(name, L"Resolution (next launch)");
        swprintf_s(value, L"%d%%", g_resolutionSetting);
        break;
    case 5:
        wcscpy_s(name, L"DLSS scene size (next launch)");
        if (g_dlssPercentSetting == 0) wcscpy_s(value, L"DLSS off");
        else swprintf_s(value, L"%d%%", g_dlssPercentSetting);
        break;
    case 6:
        wcscpy_s(name, L"Skip the launcher window (next launch)");
        wcscpy_s(value, g_skipLauncherSetting ? L"On" : L"Off");
        break;
    default: wcscpy_s(name, L"Close"); break;
    }
}

// Draws the menu with GDI into a top-down buffer, then gives it to the headset as RGBA.
static void MenuDraw() {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kMenuW;
    bi.bmiHeader.biHeight = -kMenuH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(dc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    auto fill = [&](int l, int t, int r, int b, COLORREF c) {
        RECT rc{l, t, r, b};
        HBRUSH br = CreateSolidBrush(c);
        FillRect(dc, &rc, br);
        DeleteObject(br);
    };
    fill(0, 0, kMenuW, kMenuH, RGB(20, 16, 44));
    fill(0, 0, kMenuW, 8, RGB(60, 210, 230));
    HFONT big = CreateFontW(56, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HFONT mid = CreateFontW(38, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HFONT small = CreateFontW(28, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, big);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT title{44, 26, kMenuW - 44, 100};
    DrawTextW(dc, L"Rift Apart VR", -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, mid);
    for (int i = 0; i < kMenuRows; ++i) {
        const int top = 116 + i * 72;
        if (i == g_menuRow) fill(28, top, kMenuW - 28, top + 64, RGB(60, 210, 230));
        wchar_t name[64], value[32];
        MenuRowText(i, name, value);
        SetTextColor(dc, i == g_menuRow ? RGB(12, 10, 30) : RGB(235, 235, 245));
        RECT a{48, top, kMenuW - 48, top + 64};
        DrawTextW(dc, name, -1, &a, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        DrawTextW(dc, value, -1, &a, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
    SelectObject(dc, small);
    SetTextColor(dc, RGB(170, 170, 195));
    RECT help{44, kMenuH - 62, kMenuW - 44, kMenuH - 12};
    DrawTextW(dc, L"D-pad: choose and change     A: select     B: close", -1, &help, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    GdiFlush();
    g_menuImage.resize(static_cast<size_t>(kMenuW) * kMenuH);
    const uint32_t* src = static_cast<const uint32_t*>(bits);
    for (size_t i = 0; i < g_menuImage.size(); ++i) {
        const uint32_t p = src[i];
        g_menuImage[i] = 0xff000000u | ((p & 0xff) << 16) | (p & 0xff00) | ((p >> 16) & 0xff);
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(big);
    DeleteObject(mid);
    DeleteObject(small);
    DeleteObject(bmp);
    DeleteDC(dc);
    XrMenuShow(g_menuImage.data(), kMenuW, kMenuH);
}

static void MenuClose() {
    g_menuOpen = false;
    XrMenuHide();
    Log("MENU closed");
}

static void MenuOpen() {
    g_menuOpen = true;
    MenuDraw();
    Log("MENU open");
}

// dir: -1 or 1 changes the value, 0 is A.
static void MenuChange(int dir) {
    switch (g_menuRow) {
    case 0:
        if (dir == 0) {
            g_wantRecenter = true;
            XrRecenterScreen();
            Log("MENU recenter");
        }
        break;
    case 1: {
        const int v = std::clamp(static_cast<int>(g_worldScale.load() * 100 + 0.5f) + dir * 25, 100, 1000);
        g_worldScale = v / 100.0f;
        SaveSetting(L"WorldScalePercent", v);
        break;
    }
    case 2:
        g_levelCamera = !g_levelCamera;
        SaveSetting(L"LevelCamera", g_levelCamera ? 1 : 0);
        break;
    case 3:
        g_hudDistanceSetting = std::clamp(g_hudDistanceSetting + dir * 0.25f, 1.0f, 5.0f);
        XrSetHud(2.2f * g_hudDistanceSetting / 2.0f, g_hudDistanceSetting, 0.1f);
        SaveSetting(L"HudDistanceCm", static_cast<int>(g_hudDistanceSetting * 100 + 0.5f));
        break;
    case 4:
        g_resolutionSetting = std::clamp(g_resolutionSetting + dir * 10, 50, 200);
        SaveSetting(L"Resolution", g_resolutionSetting);
        break;
    case 5:
    {
        // Off, Performance, Balanced, Quality, Ultra Quality, DLAA.
        static const int steps[] = {0, 50, 58, 67, 77, 100};
        int i = 3;
        for (int k = 0; k < 6; ++k) if (steps[k] == g_dlssPercentSetting) i = k;
        i = std::clamp(i + (dir == 0 ? 1 : dir), 0, 5);
        g_dlssPercentSetting = steps[i];
        SaveSetting(L"EyeDLSS", g_dlssPercentSetting ? 1 : 0);
        if (g_dlssPercentSetting) SaveSetting(L"EyeDLSSPercent", g_dlssPercentSetting);
        break;
    }
    case 6:
        g_skipLauncherSetting = !g_skipLauncherSetting;
        SaveSetting(L"SkipLauncher", g_skipLauncherSetting ? 1 : 0);
        break;
    default:
        if (dir == 0) {
            MenuClose();
            return;
        }
        break;
    }
    Log("MENU row %d dir %d: scale %.2f pitch decoupled %d hud %.2f resolution %d dlss %d", g_menuRow, dir, g_worldScale.load(), g_levelCamera.load() ? 1 : 0, g_hudDistanceSetting, g_resolutionSetting, g_dlssPercentSetting);
    MenuDraw();
}

// The game must not see the pad while the menu is open.
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);
static XInputGetStateFn g_origXInput14, g_origXInput13, g_origXInput910, g_origXInputEx14, g_origXInputEx13;
static std::atomic<uint32_t> g_xinputCalls[5];
template <XInputGetStateFn* Orig, int Slot>
static DWORD WINAPI HookXInput(DWORD pad, void* state) {
    ++g_xinputCalls[Slot];
    const DWORD r = (*Orig)(pad, state);
    if (r == 0 && g_menuOpen) {
        const DWORD packet = *static_cast<DWORD*>(state);
        memset(state, 0, 16);
        *static_cast<DWORD*>(state) = packet;
    }
    return r;
}

static void InstallPadHooks() {
    struct Item {
        const wchar_t* dll;
        const char* name;
        void* hook;
        void** orig;
    };
    // Ordinal 100 is XInputGetStateEx, which many games use for the guide button.
    const Item items[] = {
        {L"xinput1_4.dll", "XInputGetState", reinterpret_cast<void*>(&HookXInput<&g_origXInput14, 0>), reinterpret_cast<void**>(&g_origXInput14)},
        {L"xinput1_3.dll", "XInputGetState", reinterpret_cast<void*>(&HookXInput<&g_origXInput13, 1>), reinterpret_cast<void**>(&g_origXInput13)},
        {L"xinput9_1_0.dll", "XInputGetState", reinterpret_cast<void*>(&HookXInput<&g_origXInput910, 2>), reinterpret_cast<void**>(&g_origXInput910)},
        {L"xinput1_4.dll", MAKEINTRESOURCEA(100), reinterpret_cast<void*>(&HookXInput<&g_origXInputEx14, 3>), reinterpret_cast<void**>(&g_origXInputEx14)},
        {L"xinput1_3.dll", MAKEINTRESOURCEA(100), reinterpret_cast<void*>(&HookXInput<&g_origXInputEx13, 4>), reinterpret_cast<void**>(&g_origXInputEx13)},
    };
    for (const Item& h : items) {
        HMODULE m = LoadLibraryW(h.dll);
        void* f = m ? reinterpret_cast<void*>(GetProcAddress(m, h.name)) : nullptr;
        if (!f) continue;
        const MH_STATUS st = MH_CreateHook(f, h.hook, h.orig);
        const MH_STATUS en = MH_EnableHook(f);
        Log("PAD hook %ls %s %s", h.dll, MH_StatusToString(st), MH_StatusToString(en));
    }
}

// The game's real pad path is Windows.Gaming.Input, found by its log and by 0x16f9a3e. Both interfaces are hooked at
// their code, reached through a controller of our own asking. Vtable slots: IGamepad +0x40, IRawGameController +0x68.
struct GamepadReading {
    uint64_t timestamp;
    uint32_t buttons;
    uint32_t pad;
    double lt, rt, lx, ly, rx, ry;
};
using WgiGamepadReadFn = HRESULT(STDMETHODCALLTYPE*)(void*, GamepadReading*);
using WgiRawReadFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, uint8_t*, UINT32, int*, UINT32, double*, uint64_t*);
static WgiGamepadReadFn g_origWgiGamepad;
static WgiRawReadFn g_origWgiRaw;
static std::atomic<uint32_t> g_wgiGamepadCalls{0}, g_wgiRawCalls{0}, g_wgiBlocked{0};
static HRESULT STDMETHODCALLTYPE HookWgiGamepad(void* self, GamepadReading* out) {
    const HRESULT r = g_origWgiGamepad(self, out);
    ++g_wgiGamepadCalls;
    if (SUCCEEDED(r) && out && g_menuOpen) {
        out->buttons = 0;
        out->lt = out->rt = out->lx = out->ly = out->rx = out->ry = 0;
        ++g_wgiBlocked;
    }
    return r;
}
static HRESULT STDMETHODCALLTYPE HookWgiRaw(void* self, UINT32 nb, uint8_t* b, UINT32 ns, int* sw, UINT32 na, double* a, uint64_t* t) {
    const HRESULT r = g_origWgiRaw(self, nb, b, ns, sw, na, a, t);
    ++g_wgiRawCalls;
    // Rest values differ by controller, so axes hold what they read before the menu opened.
    static double lastAxes[16];
    if (SUCCEEDED(r)) {
        if (g_menuOpen) {
            if (b) memset(b, 0, nb);
            if (sw) memset(sw, 0, ns * sizeof(int));
            if (a) memcpy(a, lastAxes, std::min<UINT32>(na, 16) * sizeof(double));
        } else if (a) {
            memcpy(lastAxes, a, std::min<UINT32>(na, 16) * sizeof(double));
        }
    }
    return r;
}
struct ComLike {
    void** vt;
};
static void InstallWgiHooks() {
    static bool gamepadDone = false, rawDone = false, failed = false;
    if ((gamepadDone && rawDone) || failed) return;
    HMODULE combase = LoadLibraryW(L"combase.dll");
    using RoInitFn = HRESULT(WINAPI*)(int);
    using RoGetFactoryFn = HRESULT(WINAPI*)(void*, const GUID&, void**);
    using MakeStringFn = HRESULT(WINAPI*)(const wchar_t*, UINT32, void**);
    auto roInit = combase ? reinterpret_cast<RoInitFn>(GetProcAddress(combase, "RoInitialize")) : nullptr;
    auto roFactory = combase ? reinterpret_cast<RoGetFactoryFn>(GetProcAddress(combase, "RoGetActivationFactory")) : nullptr;
    auto makeString = combase ? reinterpret_cast<MakeStringFn>(GetProcAddress(combase, "WindowsCreateString")) : nullptr;
    if (!roInit || !roFactory || !makeString) {
        failed = true;
        Log("WGI combase functions missing");
        return;
    }
    static bool inited = false;
    if (!inited) {
        roInit(1);  // multithreaded; a thread of this mod only
        inited = true;
    }
    // get_Gamepads and get_RawGameControllers are both slot 10 of their statics; a vector view's GetAt is 6, Size 7.
    auto firstOf = [&](const wchar_t* cls, const GUID& iid, const GUID* elementIid) -> ComLike* {
        void* name = nullptr;
        if (FAILED(makeString(cls, static_cast<UINT32>(wcslen(cls)), &name))) return nullptr;
        ComLike* statics = nullptr;
        if (FAILED(roFactory(name, iid, reinterpret_cast<void**>(&statics))) || !statics) return nullptr;
        ComLike* vec = nullptr;
        using GetVecFn = HRESULT(STDMETHODCALLTYPE*)(void*, ComLike**);
        if (FAILED(reinterpret_cast<GetVecFn>(statics->vt[10])(statics, &vec)) || !vec) return nullptr;
        UINT32 n = 0;
        using SizeFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32*);
        reinterpret_cast<SizeFn>(vec->vt[7])(vec, &n);
        if (!n) return nullptr;
        ComLike* item = nullptr;
        using GetAtFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, ComLike**);
        if (FAILED(reinterpret_cast<GetAtFn>(vec->vt[6])(vec, 0, &item))) return nullptr;
        (void)elementIid;
        return item;
    };
    static const GUID kGamepadStatics{0x8BBCE529, 0xD49C, 0x39E9, {0x95, 0x60, 0xE4, 0x7D, 0xDE, 0x96, 0xB7, 0xC8}};
    static const GUID kRawStatics{0xEB8D0792, 0xE95A, 0x4B19, {0xAF, 0xC7, 0x0A, 0x59, 0xF8, 0xBF, 0x75, 0x9E}};
    if (!gamepadDone) {
        if (ComLike* pad = firstOf(L"Windows.Gaming.Input.Gamepad", kGamepadStatics, nullptr)) {
            void* f = pad->vt[8];
            const MH_STATUS a = MH_CreateHook(f, reinterpret_cast<void*>(&HookWgiGamepad), reinterpret_cast<void**>(&g_origWgiGamepad));
            const MH_STATUS b = MH_EnableHook(f);
            Log("WGI gamepad hook %s %s", MH_StatusToString(a), MH_StatusToString(b));
            gamepadDone = true;
        }
    }
    if (!rawDone) {
        if (ComLike* raw = firstOf(L"Windows.Gaming.Input.RawGameController", kRawStatics, nullptr)) {
            void* f = raw->vt[13];
            const MH_STATUS a = MH_CreateHook(f, reinterpret_cast<void*>(&HookWgiRaw), reinterpret_cast<void**>(&g_origWgiRaw));
            const MH_STATUS b = MH_EnableHook(f);
            Log("WGI raw hook %s %s", MH_StatusToString(a), MH_StatusToString(b));
            rawDone = true;
        }
    }
}

// Runs every 16 ms: reads the pad underneath the hooks above.
static void PollPad() {
    XInputGetStateFn get = g_origXInput14 ? g_origXInput14 : g_origXInput13 ? g_origXInput13 : g_origXInput910;
    if (!get || !g_xrSession) return;
    struct {
        DWORD packet;
        WORD buttons;
        uint8_t lt, rt;
        short lx, ly, rx, ry;
    } st{};
    WORD buttons = 0;
    short ly = 0, lx = 0;
    for (DWORD pad = 0; pad < 4; ++pad) {
        if (get(pad, &st) == 0) {
            buttons |= st.buttons;
            if (abs(st.ly) > abs(ly)) ly = st.ly;
            if (abs(st.lx) > abs(lx)) lx = st.lx;
        }
    }
    const ULONGLONG now = GetTickCount64();
    // Both sticks clicked and held: open or close.
    static ULONGLONG since = 0;
    static bool fired = false;
    const bool held = (buttons & 0x00C0) == 0x00C0;
    if (!held) {
        since = 0;
        fired = false;
    } else if (!since) {
        since = now;
    } else if (!fired && now - since >= 500) {
        fired = true;
        if (g_menuOpen) MenuClose();
        else MenuOpen();
        return;
    }
    if (!g_menuOpen) return;
    // One step per press, then repeating after a short hold, so key repeat never skips rows.
    enum { kUp, kDown, kLeft, kRight, kA, kB, kCount };
    const bool now_down[kCount] = {(buttons & 1) != 0 || ly > 16000, (buttons & 2) != 0 || ly < -16000, (buttons & 4) != 0 || lx < -16000, (buttons & 8) != 0 || lx > 16000, (buttons & 0x1000) != 0, (buttons & 0x2000) != 0};
    static ULONGLONG next[kCount] = {};
    static bool was[kCount] = {};
    for (int i = 0; i < kCount; ++i) {
        bool press = false;
        if (now_down[i] && !was[i]) {
            press = true;
            next[i] = now + 400;
        } else if (now_down[i] && i <= kRight && now >= next[i]) {
            press = true;
            next[i] = now + 150;
        }
        was[i] = now_down[i];
        if (!press) continue;
        if (i == kUp || i == kDown) {
            g_menuRow = (g_menuRow + (i == kUp ? kMenuRows - 1 : 1)) % kMenuRows;
            MenuDraw();
        } else if (i == kLeft || i == kRight) {
            MenuChange(i == kLeft ? -1 : 1);
        } else if (i == kA) {
            if (g_menuRow == 0 || g_menuRow == 2 || g_menuRow == 6 || g_menuRow == kMenuRows - 1) MenuChange(0);
        } else {
            MenuClose();
        }
        if (!g_menuOpen) return;
    }
}

static DWORD WINAPI PadThread(void*) {
    ULONGLONG lastTry = 0;
    for (;;) {
        Sleep(16);
        PollPad();
        const ULONGLONG now = GetTickCount64();
        if (now - lastTry > 2000) {
            lastTry = now;
            InstallWgiHooks();
        }
    }
}

// ---- DLSS for each eye: one Streamline viewport per view. Main 0, left eye 1, right eye 2. ----
// The game has one viewport and rebuilds DLSS when the output size changes, and it skips offscreen views. So on the
// render thread: the upscale pass sees eyes as ordinary views, and each eye calls the DLSS step directly on its own viewport.
static std::atomic<bool> g_eyeDlss{true};
static std::atomic<int> g_slViewport{0};
static std::atomic<int> g_slLogLeft{12};
static std::atomic<uint32_t> g_slCount[8];
static const size_t kViewportValue = 0x20;  // sl::ViewportHandle: next, GUID, version, then the value. Seen live, always 0 from the game.
struct ViewportCopy {
    uint8_t bytes[0x30];
    explicit ViewportCopy(const void* from) {
        memcpy(bytes, from, sizeof(bytes));
        *reinterpret_cast<uint32_t*>(bytes + kViewportValue) = static_cast<uint32_t>(g_slViewport.load());
    }
};
using SlEvaluateFn = int(__cdecl*)(uint32_t, const void*, const void**, uint32_t, void*);
using SlSetConstFn = int(__cdecl*)(const void*, const void*, const void*);
using SlGetFeatureFn = int(__cdecl*)(uint32_t, const char*, void**);
using SlSetOptionsFn = int(__cdecl*)(const void*, const void*);
static SlEvaluateFn g_origSlEvaluate;
static SlSetConstFn g_origSlSetConst;
static SlGetFeatureFn g_origSlGetFeature;
static SlSetOptionsFn g_origSlSetOptions;
static int __cdecl HookSlEvaluate(uint32_t feature, const void* frame, const void** inputs, uint32_t n, void* cmd) {
    ++g_slCount[0];
    const int vp = g_slViewport.load();
    if (vp == 0 || feature != 0 || n == 0 || n > 8) return g_origSlEvaluate(feature, frame, inputs, n, cmd);
    const void* local[8];
    memcpy(local, inputs, n * sizeof(void*));
    ViewportCopy copy(inputs[0]);
    local[0] = copy.bytes;
    const int r = g_origSlEvaluate(feature, frame, local, n, cmd);
    ++g_slCount[3];
    if (r != 0 && g_slLogLeft.fetch_sub(1) > 0) Log("SL evaluate viewport %d result %d", vp, r);
    return r;
}
static int __cdecl HookSlSetConst(const void* values, const void* frame, const void* viewport) {
    ++g_slCount[2];
    if (g_slViewport.load() == 0) return g_origSlSetConst(values, frame, viewport);
    ViewportCopy copy(viewport);
    const int r = g_origSlSetConst(values, frame, copy.bytes);
    if (r != 0 && g_slLogLeft.fetch_sub(1) > 0) Log("SL constants viewport %d result %d", g_slViewport.load(), r);
    return r;
}
static int __cdecl HookSlSetOptions(const void* viewport, const void* options) {
    ++g_slCount[4];
    if (g_slViewport.load() == 0) return g_origSlSetOptions(viewport, options);
    ViewportCopy copy(viewport);
    const int r = g_origSlSetOptions(copy.bytes, options);
    if (r != 0 && g_slLogLeft.fetch_sub(1) > 0) Log("SL options viewport %d result %d", g_slViewport.load(), r);
    return r;
}
static int __cdecl HookSlGetFeature(uint32_t feature, const char* name, void** fn) {
    const int r = g_origSlGetFeature(feature, name, fn);
    if (r == 0 && fn && *fn && name && !strcmp(name, "slDLSSSetOptions") && !g_origSlSetOptions) {
        g_origSlSetOptions = reinterpret_cast<SlSetOptionsFn>(*fn);
        *fn = reinterpret_cast<void*>(&HookSlSetOptions);
        Log("SL options routed");
    }
    return r;
}

static int EyeIndexOfJob(uint8_t* job) {
    if (!job) return 0;
    uint8_t* owner = nullptr;
    if (!SafeCopy(&owner, job + 0x1ff8, 8) || !owner) return 0;
    if (owner == g_eyeA.load()) return 1;
    if (owner == g_eyeB.load()) return 2;
    return 0;
}
// The constants step takes the job's view and runs just before its DLSS, so it names the viewport for what follows.
using SlConstantsFn = void(__fastcall*)(uint8_t*, int, int, int);
static SlConstantsFn g_origSlConstants;
static void __fastcall HookSlConstants(uint8_t* job, int frame, int w, int h) {
    g_slViewport = g_eyeDlss ? EyeIndexOfJob(job) : 0;
    g_origSlConstants(job, frame, w, h);
}
using UpscalePassFn = void(__fastcall*)(uint8_t*, int);
static UpscalePassFn g_origUpscalePass;
static void __fastcall HookUpscalePass(uint8_t* list, int count) {
    int saved[8] = {};
    uint8_t* jobs[8] = {};
    const int n = std::min(count, 8);
    if (g_eyeDlss && count > 0) {
        for (int i = 0; i < n; ++i) {
            uint8_t** entry = *reinterpret_cast<uint8_t***>(list + 8 + i * 16);
            uint8_t* job = entry ? *entry : nullptr;
            if (EyeIndexOfJob(job)) {
                jobs[i] = job;
                int* id = reinterpret_cast<int*>(job + 0xf90);
                saved[i] = *id;
                *id = 0;  // negative is what makes the pass skip a view
            }
        }
    }
    g_origUpscalePass(list, count);
    for (int i = 0; i < n; ++i) {
        if (jobs[i]) *reinterpret_cast<int*>(jobs[i] + 0xf90) = saved[i];
    }
    g_slViewport = 0;
}
using UpEvalFn = void(__fastcall*)(uint8_t*, uint32_t*);
static UpEvalFn g_origUpEval;
static void __fastcall HookUpEval(uint8_t* self, uint32_t* desc) {
    const int vp = g_slViewport.load();
    if (vp == 0 || !g_eyeDlss) {
        g_origUpEval(self, desc);
        return;
    }
    static std::atomic<int> left{6};
    if (left.fetch_sub(1) > 0) Log("EYEDLSS viewport %d in %ux%u out %ux%u method %d ready %u", vp, desc[0], desc[1], desc[2], desc[3], *reinterpret_cast<int*>(self + 0x10), self[0xd]);
    // Only once the game's own DLSS is set up, and only for the DLSS method.
    if (*reinterpret_cast<int*>(self + 0x10) != 2 || !self[0xd]) return;
    reinterpret_cast<UpEvalFn>(g_base + kUpEvalDlss.rva)(self, desc);
}

static void InstallSlProbe() {
    HMODULE m = GetModuleHandleW(L"sl.interposer.dll");
    if (!m) m = LoadLibraryW((g_dir + L"sl.interposer.dll").c_str());
    if (!m) {
        Log("SL interposer not loaded");
        return;
    }
    struct Item {
        const char* name;
        void* hook;
        void** orig;
    };
    const Item items[] = {{"slEvaluateFeature", reinterpret_cast<void*>(&HookSlEvaluate), reinterpret_cast<void**>(&g_origSlEvaluate)},
                          {"slSetConstants", reinterpret_cast<void*>(&HookSlSetConst), reinterpret_cast<void**>(&g_origSlSetConst)},
                          {"slGetFeatureFunction", reinterpret_cast<void*>(&HookSlGetFeature), reinterpret_cast<void**>(&g_origSlGetFeature)}};
    for (const Item& h : items) {
        void* f = reinterpret_cast<void*>(GetProcAddress(m, h.name));
        const MH_STATUS st = f ? MH_CreateHook(f, h.hook, h.orig) : MH_ERROR_FUNCTION_NOT_FOUND;
        const MH_STATUS en = f ? MH_EnableHook(f) : MH_UNKNOWN;
        Log("SL hook %s %s %s", h.name, MH_StatusToString(st), MH_StatusToString(en));
    }
    if (!Check(kUpscalePass) || !Check(kSlConstants) || !Check(kUpEval) || !Check(kUpEvalDlss)) {
        g_eyeDlss = false;
        return;
    }
    MH_CreateHook(g_base + kUpscalePass.rva, reinterpret_cast<void*>(&HookUpscalePass), reinterpret_cast<void**>(&g_origUpscalePass));
    MH_CreateHook(g_base + kSlConstants.rva, reinterpret_cast<void*>(&HookSlConstants), reinterpret_cast<void**>(&g_origSlConstants));
    MH_CreateHook(g_base + kUpEval.rva, reinterpret_cast<void*>(&HookUpEval), reinterpret_cast<void**>(&g_origUpEval));
    Log("EYEDLSS hooks %s", MH_StatusToString(MH_EnableHook(MH_ALL_HOOKS)));
}

// ---- the eyes need the game's own anti-aliasing, which it turns off when an upscaler is chosen ----
static std::atomic<bool> g_regForce{false};
static std::atomic<bool> g_skipLauncher{true};
static void OverrideSetting(const wchar_t* name, void* data) {
    if (!name || !data) return;
    if (g_skipLauncher && !_wcsicmp(name, L"ShowLauncher")) {
        Log("REG ShowLauncher was %d, the game is told 0", *static_cast<int*>(data));
        *static_cast<int*>(data) = 0;
        return;
    }
    if (!g_regForce) return;
    int value = -1;
    if (!_wcsicmp(name, L"UpscaleMethod")) value = 0;
    else if (!_wcsicmp(name, L"AA")) value = 2;
    // Nobody sees the flat view in VR: a target it cannot meet keeps it small. Eyes are sized on their own (measured, build 29).
    else if (!_wcsicmp(name, L"DynamicResolutionTargetFPS")) value = 120;
    if (value < 0) return;
    int* d = static_cast<int*>(data);
    if (*d != value) {
        Log("REG %ls was %d, the game is told %d", name, *d, value);
        *d = value;
    }
}
using RegQueryFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegGetFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
static RegQueryFn g_origRegQuery;
static RegGetFn g_origRegGet;
static LSTATUS WINAPI HookRegQuery(HKEY k, LPCWSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD size) {
    const LSTATUS r = g_origRegQuery(k, name, res, type, data, size);
    // The caller may not ask for the type. These two settings are the only names touched, and always four bytes.
    if (r == 0 && (!type || *type == REG_DWORD) && (!size || *size == 4)) OverrideSetting(name, data);
    return r;
}
using RegQueryAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
static RegQueryAFn g_origRegQueryA;
static LSTATUS WINAPI HookRegQueryA(HKEY k, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD size) {
    const LSTATUS r = g_origRegQueryA(k, name, res, type, data, size);
    if (r == 0 && name && (!type || *type == REG_DWORD) && (!size || *size == 4)) {
        wchar_t wide[64];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, 64)) OverrideSetting(wide, data);
    }
    return r;
}
static LSTATUS WINAPI HookRegGet(HKEY k, LPCWSTR sub, LPCWSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD size) {
    DWORD localType = 0;
    const LSTATUS r = g_origRegGet(k, sub, name, flags, type ? type : &localType, data, size);
    if (r == 0 && (type ? *type : localType) == REG_DWORD && (!size || *size == 4)) OverrideSetting(name, data);
    return r;
}
static void InstallRegHooks() {
    void* a = nullptr;
    void* b = nullptr;
    const MH_STATUS s1 = MH_CreateHookApiEx(L"kernelbase.dll", "RegQueryValueExW", reinterpret_cast<void*>(&HookRegQuery), reinterpret_cast<void**>(&g_origRegQuery), &a);
    const MH_STATUS s2 = MH_CreateHookApiEx(L"kernelbase.dll", "RegGetValueW", reinterpret_cast<void*>(&HookRegGet), reinterpret_cast<void**>(&g_origRegGet), &b);
    void* c = nullptr;
    const MH_STATUS s3 = MH_CreateHookApiEx(L"kernelbase.dll", "RegQueryValueExA", reinterpret_cast<void*>(&HookRegQueryA), reinterpret_cast<void**>(&g_origRegQueryA), &c);
    if (a) MH_EnableHook(a);
    if (b) MH_EnableHook(b);
    if (c) MH_EnableHook(c);
    Log("REG hooks %s %s %s", MH_StatusToString(s1), MH_StatusToString(s2), MH_StatusToString(s3));
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
    {
        // Any texture the game has made leads to its device.
        void* r = nullptr;
        for (int i = 0; i < 600 && !r; ++i) {
            int tw = 0, th = 0;
            uint32_t st = 0;
            if (uint8_t* tex = UiTexture(0, &tw, &th)) r = TextureResource(tex, &st);
            if (!r) Sleep(50);
        }
        if (!r) {
            Log("XR no game texture found");
            return 0;
        }
        CaptureSetup(r);
    }
    for (int i = 0; i < 500 && !CaptureReady(); ++i) Sleep(20);
    if (!CaptureReady()) {
        Log("XR capture not ready");
        return 0;
    }
    if (!XrStartSession(CaptureDevice(), CaptureQueue(), w, h)) return 0;
    InstallFocusFix();
    CaptureNoVsync(true);
    CaptureSetPresentCallback(&OnPresentForXr);
    g_xrSession = true;
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
static float g_hud[3];
static char g_text[64];
static int g_show;

static float g_fh[7];
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
    else if (sscanf(c.c_str(), "tonefix %u", &g_w) == 1) g_toneFix = g_w != 0;
    else if (sscanf(c.c_str(), "keepfocus %u", &g_w) == 1) g_keepFocus = g_w != 0;
    else if (sscanf(c.c_str(), "level %u", &g_w) == 1) g_levelCamera = g_w != 0;
    else if (sscanf(c.c_str(), "lensfix %u", &g_w) == 1) g_lensFix = g_w != 0;
    else if (c == "padinfo") {
        Log("PADINFO WGI gamepad reads %u, raw reads %u, blocked %u", g_wgiGamepadCalls.load(), g_wgiRawCalls.load(), g_wgiBlocked.load());
        Log("PADINFO XInput calls: 1_4 %u, 1_3 %u, 9_1_0 %u, 1_4 Ex %u, 1_3 Ex %u", g_xinputCalls[0].load(), g_xinputCalls[1].load(), g_xinputCalls[2].load(), g_xinputCalls[3].load(), g_xinputCalls[4].load());
        for (const wchar_t* n : {L"sdl2.dll", L"sdl3.dll", L"libScePad.dll", L"GameInput.dll", L"hid.dll", L"dinput8.dll", L"Windows.Gaming.Input.dll", L"steam_api64.dll", L"nvapi64.dll"}) Log("PADINFO module %ls %p", n, GetModuleHandleW(n));
    } else if (c == "menu") {
        if (g_menuOpen) MenuClose();
        else MenuOpen();
    } else if (sscanf(c.c_str(), "menushot %63s", g_text) == 1) {
        MenuDraw();
        const std::wstring path = g_dir + L"vrlab_shots\\" + std::wstring(g_text, g_text + strlen(g_text)) + L".raw";
        if (FILE* f = _wfopen(path.c_str(), L"wb")) {
            fwrite(g_menuImage.data(), 4, g_menuImage.size(), f);
            fclose(f);
            Log("MENU shot %dx%d", kMenuW, kMenuH);
        }
    }
    else if (sscanf(c.c_str(), "eyejitter %u", &g_w) == 1) g_eyeJitter = static_cast<int>(g_w);
    else if (sscanf(c.c_str(), "eyedlss %u", &g_w) == 1) g_eyeDlss = g_w != 0;
    else if (c == "slinfo") {
        Log("SLINFO evaluate %u, on eye viewports %u, constants %u, options %u", g_slCount[0].load(), g_slCount[3].load(), g_slCount[2].load(), g_slCount[4].load());
        uint8_t* app = *reinterpret_cast<uint8_t**>(g_base + 0x684bda0);
        if (app) {
            uint8_t* up = *reinterpret_cast<uint8_t**>(app + 0xe0);
            if (up) {
                size_t* vt = *reinterpret_cast<size_t**>(up);
                Log("SLINFO upscaler %p vtable rva %zx, +0x28 rva %zx", up, reinterpret_cast<uint8_t*>(vt) - g_base, static_cast<size_t>(vt[5] - reinterpret_cast<size_t>(g_base)));
            }
        }
        g_slLogLeft = 20;
    } else if (c == "aainfo") {
        auto u8at = [](size_t rva) { return static_cast<unsigned>(*(g_base + rva)); };
        auto i32at = [](size_t rva) { return *reinterpret_cast<int*>(g_base + rva); };
        Log("AAINFO pass type %d, aa type %d, flags %u %u %u, global render %d x %d, taa ring %d, frame %d", i32at(0x6732b8c), i32at(0x533959c), u8at(0x53395e9), u8at(0x53395a4), u8at(0x6733634), i32at(0x67334c0), i32at(0x67334c4), i32at(0x533b8ec), i32at(0x6794fe8));
        if (uint8_t* m = ActiveView()) {
            float jx = 0, jy = 0;
            reinterpret_cast<JitterFn>(g_base + kJitter.rva)(m, &jx, &jy);
            Log("AAINFO main size %.1f %.1f jitter %.7f %.7f, in eye pixels %.3f %.3f", *reinterpret_cast<float*>(m + 0x448), *reinterpret_cast<float*>(m + 0x44c), jx, jy, jx * g_forceW * 0.5f, jy * g_forceH * 0.5f);
        }
        std::lock_guard<std::mutex> l(g_eyesLock);
        if (!g_eyes.empty()) {
            uint8_t* e = g_eyes[0];
            float jx = 0, jy = 0;
            reinterpret_cast<JitterFn>(g_base + kJitter.rva)(e, &jx, &jy);
            Log("AAINFO eye size %.1f %.1f own jitter %.7f %.7f buffers %p", *reinterpret_cast<float*>(e + 0x448), *reinterpret_cast<float*>(e + 0x44c), jx, jy, *reinterpret_cast<void**>(e + 0x1690));
        }
    }
    else if (c == "fakehead off") XrFakeHead(false, 0, 0, 0, 0, 0, 0, 0);
    else if (c == "fakelens off") XrFakeLens(false, -1, 1, 1, -1);
    else if (sscanf(c.c_str(), "fakehead %f %f %f %f %f %f %f", &g_fh[0], &g_fh[1], &g_fh[2], &g_fh[3], &g_fh[4], &g_fh[5], &g_fh[6]) == 7) XrFakeHead(true, g_fh[0], g_fh[1], g_fh[2], g_fh[3], g_fh[4], g_fh[5], g_fh[6]);
    else if (sscanf(c.c_str(), "fakelens %f %f %f %f", &g_fh[0], &g_fh[1], &g_fh[2], &g_fh[3]) == 4) XrFakeLens(true, g_fh[0], g_fh[1], g_fh[2], g_fh[3]);
    else if (sscanf(c.c_str(), "uicount %u", &g_w) == 1) g_uiCount = static_cast<int>(g_w);
    else if (sscanf(c.c_str(), "restflat %u", &g_w) == 1) g_restFlat = g_w != 0;
    else if (sscanf(c.c_str(), "novsync %u", &g_w) == 1) CaptureNoVsync(g_w != 0);
    else if (sscanf(c.c_str(), "hud %f %f %f", &g_hud[0], &g_hud[1], &g_hud[2]) == 3) XrSetHud(g_hud[0], g_hud[1], g_hud[2]);
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
    else if (c == "camtree") g_wantCamTree = true;
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
    {
        const std::wstring early = g_dir + L"RiftApartVR.ini";
        g_eyeDlss = GetPrivateProfileIntW(L"VR", L"EyeDLSS", 1, early.c_str()) != 0;
        g_eyeRenderPercent = g_eyeDlss ? std::clamp(static_cast<int>(GetPrivateProfileIntW(L"VR", L"EyeDLSSPercent", 67, early.c_str())), 33, 100) : 100;
        g_regForce = !g_eyeDlss && GetPrivateProfileIntW(L"VR", L"ForceGameAA", 1, early.c_str()) != 0;
        g_skipLauncher = GetPrivateProfileIntW(L"VR", L"SkipLauncher", 1, early.c_str()) != 0;
        MH_Initialize();
        InstallRegHooks();
    }
    FindSections();
    FindActiveView();
    InstallHooks();
    const std::wstring ini = g_dir + L"RiftApartVR.ini";
    g_iniPath = ini;
    g_skipLauncherSetting = GetPrivateProfileIntW(L"VR", L"SkipLauncher", 1, ini.c_str()) != 0;
    g_dlssPercentSetting = g_eyeDlss ? g_eyeRenderPercent.load() : 0;
    g_skipLauncher = g_skipLauncherSetting;
    InstallSlProbe();
    InstallPadHooks();
    CreateThread(nullptr, 0, PadThread, nullptr, 0, nullptr);
    const bool autoStart = GetPrivateProfileIntW(L"VR", L"AutoStart", 1, ini.c_str()) != 0;
    const int resolution = GetPrivateProfileIntW(L"VR", L"Resolution", 100, ini.c_str());
    g_worldScale = GetPrivateProfileIntW(L"VR", L"WorldScalePercent", 300, ini.c_str()) / 100.0f;
    g_levelCamera = GetPrivateProfileIntW(L"VR", L"LevelCamera", 1, ini.c_str()) != 0;
    g_pace = GetPrivateProfileIntW(L"VR", L"Pace", 1, ini.c_str()) != 0;
    g_resolutionSetting = resolution;
    g_hudDistanceSetting = GetPrivateProfileIntW(L"VR", L"HudDistanceCm", 200, ini.c_str()) / 100.0f;
    XrSetHud(2.2f * g_hudDistanceSetting / 2.0f, g_hudDistanceSetting, 0.1f);
    Log("INI AutoStart %d Resolution %d WorldScale %.2f Pace %d", autoStart ? 1 : 0, resolution, g_worldScale.load(), g_pace.load() ? 1 : 0);
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
        {
            // Gravity boots: does the game's camera lean? Logged once a second when its right row leaves the level.
            static ULONGLONG lastRow = 0;
            if (now - lastRow >= 1000) {
                lastRow = now;
                if (uint8_t* v = ActiveView()) {
                    float m[16];
                    if (SafeCopy(m, v, sizeof(m)) && fabsf(m[1]) > 0.03f) Log("CAMROW right %.3f %.3f %.3f down %.3f %.3f %.3f forward %.3f %.3f %.3f pos %.2f %.2f %.2f", m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
                }
            }
        }
        {
            // When do the first game textures exist? Log once.
            static bool seenUi = false;
            if (!seenUi) {
                int w = 0, h = 0;
                if (uint8_t* tex = UiTexture(0, &w, &h)) {
                    uint32_t st = 0;
                    if (void* r = TextureResource(tex, &st)) {
                        seenUi = true;
                        Log("UI first texture %dx%d resource %p", w, h, r);
                        if (autoStart) {
                            Log("AUTOSTART at the first game texture");
                            StartXr(resolution);
                        }
                    }
                }
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
