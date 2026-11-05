// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence.
//
// A WINHTTP.dll proxy (the game imports it directly, so this loads before the game's own code runs).
// Build 2: logging hooks that change nothing, plus live test commands that do. No command runs by itself.
#include <windows.h>
#include <MinHook.h>
#include "capture.h"
#include "xr.h"
#include <d3d12.h>
#include <wincodec.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <dxgi1_4.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#undef small
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

#define BUILD_NUMBER 91

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

// ---- A launch overwrites the mod's log and the game's. Keep the last ones, and list them with the game's crash dumps. ----
// Runs before the mod's log opens. Windows file calls only, since the loader may still be busy.
static void KeepLogHistory() {
    const std::wstring dir = g_dir + L"vrlab_logs\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring modLog = g_dir + L"RiftApartVR.log";
    wchar_t profile[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    const std::wstring docs = std::wstring(profile) + L"\\Documents\\Ratchet & Clank - Rift Apart\\";
    const std::wstring gameLog = docs + L"Ratchet & Clank - Rift Apart.log";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExW(modLog.c_str(), GetFileExInfoStandard, &a)) {
        SYSTEMTIME t{};
        FileTimeToSystemTime(&a.ftLastWriteTime, &t);
        wchar_t stamp[32];
        swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        MoveFileExW(modLog.c_str(), (dir + L"mod-" + stamp + L".log").c_str(), MOVEFILE_REPLACE_EXISTING);
        CopyFileW(gameLog.c_str(), (dir + L"game-" + stamp + L".log").c_str(), FALSE);
    }
    // Names sort by time. Keep the newest 40 logs, 20 launches.
    std::vector<std::wstring> logs;
    WIN32_FIND_DATAW f{};
    HANDLE h = FindFirstFileW((dir + L"*.log").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
        do logs.push_back(f.cFileName);
        while (FindNextFileW(h, &f));
        FindClose(h);
    }
    std::sort(logs.begin(), logs.end(), [](const std::wstring& x, const std::wstring& y) { return x.substr(x.find(L'-')) > y.substr(y.find(L'-')); });
    for (size_t i = 40; i < logs.size(); ++i) DeleteFileW((dir + logs[i]).c_str());
    if (logs.size() > 40) logs.resize(40);
    FILE* index = nullptr;
    if (_wfopen_s(&index, (dir + L"index.txt").c_str(), L"w") != 0 || !index) return;
    for (const std::wstring& l : logs) fwprintf(index, L"log %ls\n", l.c_str());
    h = FindFirstFileW((docs + L"*.mdmp").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
        do fwprintf(index, L"dump %ls\n", f.cFileName);
        while (FindNextFileW(h, &f));
        FindClose(h);
    }
    fclose(index);
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
// -1: the size the game's own DLSS choice gives its own view.
static std::atomic<int> g_eyeRenderPercent{-1};
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
        int percent = g_eyeRenderPercent.load();
        if (percent < 0) {
            // The game's quality setting, live: settings object +0xc0, 0 Ultra Performance, 1 Performance, 2 Balanced, 3 Quality
            // (read from the game's log per registry value). Other values log as Quality too.
            static const int kRatios[] = {33, 50, 58, 67};
            percent = 67;
            uint8_t* app = *reinterpret_cast<uint8_t**>(g_base + 0x684bda0);
            uint8_t* obj = app ? *reinterpret_cast<uint8_t**>(app) : nullptr;
            if (obj) {
                using GetSettingsFn = uint8_t*(__fastcall*)(uint8_t*);
                uint8_t* settings = (*reinterpret_cast<GetSettingsFn**>(obj))[0x130 / 8](obj);
                const int q = settings ? *reinterpret_cast<int*>(settings + 0xc0) : 3;
                if (q >= 0 && q < 4) percent = kRatios[q];
                Log("EYE game DLSS quality %d", q);
            }
        }
        if (aaType != 0) percent = 100;
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
// The tone map blends the game's whole UI into the view from the overlay target (0x67334c0 + 0x230, its resource word at
// +0x98) when the view is not "generic". Eyes need that path for colour, not the UI: the UI goes on its own quads.
// 0x678e710 is the game's own neutral stand-in, used when it runs without the overlay (0x11dc080).
static std::atomic<bool> g_eyeNoUi{true};
// The overlay is cleared every frame and drawn only in the game's own view's work. Set when that view reaches the tone map.
static std::atomic<bool> g_overlayFresh{false};
static std::atomic<int> g_wantHudShot{0};
static std::string g_rbShotName;
static const size_t kOverlayResource = 0x67334c0 + 0x230 + 0x98, kNeutralResource = 0x678e710;
static std::atomic<uint8_t*> g_eyeA{nullptr}, g_eyeB{nullptr};
// Alternate eye, a menu option on top of native stereo: one eye drawn per tick. Off on the vendor screen and flat screens.
static std::atomic<bool> g_alternateSetting{false}, g_alternateNow{false}, g_rebuildSetting{false};
static void* __fastcall HookTone(void* a, void* b, void* c, uint8_t* view, void* e, uint64_t generic) {
    if (g_toneFix && view) {
        uint8_t* owner = *reinterpret_cast<uint8_t**>(view + 0x1ff8);
        if (owner && (owner == g_eyeA.load() || owner == g_eyeB.load())) {
            generic = 0;
            if (g_eyeNoUi) {
                if (owner == g_eyeA.load()) XrMarkRendered(0, reinterpret_cast<const float*>(view + 0x30));
                else if (g_alternateNow.load()) XrMarkRendered(1, reinterpret_cast<const float*>(view + 0x30));
                uint64_t* overlay = reinterpret_cast<uint64_t*>(g_base + kOverlayResource);
                const uint64_t saved = *overlay;
                *overlay = *reinterpret_cast<uint64_t*>(g_base + kNeutralResource);
                void* r = g_origTone(a, b, c, view, e, generic);
                *overlay = saved;
                return r;
            }
        }
    }
    if (view) {
        // The render thread's copy of the view holds the camera this image was drawn with.
        uint8_t* owner = *reinterpret_cast<uint8_t**>(view + 0x1ff8);
        if (owner && owner == g_eyeA.load()) XrMarkRendered(0, reinterpret_cast<const float*>(view + 0x30));
        else if (owner && owner == g_eyeB.load() && g_alternateNow.load()) XrMarkRendered(1, reinterpret_cast<const float*>(view + 0x30));
        // Offscreen views, eyes among them, have a negative id. The game's own view does not.
        if (*reinterpret_cast<int*>(view + 0xf90) >= 0) g_overlayFresh = true;
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
// 0 off: the camera tilts the world. 1 on: level always. 2 auto: level, except while the camera itself leans over, as on gravity walls.
static std::atomic<int> g_levelCamera{2};
static float g_levelBlend = 0;
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
static void OfxrPacingUpdate();
static void OfxrApply();
static void OfxrPacingTick();
static void CreateEyeNow();
// A vendor: the game zooms onto the vendor's screen and holds. Lens 10.87 in Steve's log. A boss fight's zoom also went to
// 9 to 10, but moving, so it takes a very narrow lens and a held camera. Off in the menu means never.
static std::atomic<bool> g_vendorOn{true};
// Steve's GPU hang: a scripted ride opens with the game zoomed in, lens 10 to 20, and at the full headset view his RTX 5090 hangs on
// a draw, on two drivers. On the game's own camera it rides through. So zoomed cutscene shots use that camera on the room's screen.
// 0 zoomed shots, 1 all cutscenes, 2 never. Cause of the hang not known.
static std::atomic<int> g_cutsceneCamera{0};
static const size_t kCinematicTracks = 0x61d19d4;
static std::atomic<bool> g_vendorNow{false}, g_forceVendor{false};
// 0 off, 1 while aiming, 2 always.
static std::atomic<int> g_aimDot{1};
static float g_aimRoom[3];
static std::atomic<ULONGLONG> g_lastTrigger{0};
// 1 while the pause menu is up. Found by comparing .data, tools\\flag_watch.py. Beside the frame counter 0x6794fe8.
static const size_t kPausedFlag = 0x6794fd0;
// Logging and counters that only help development. LabProbes=0 in the ini, or `probes 0`, turns them off.
static std::atomic<bool> g_labProbes{true};
static void UpdateMode() {
    uint8_t* main = ActiveView();
    bool world = false;
    if (main) {
        const float* cam = reinterpret_cast<const float*>(main);
        const float* proj = reinterpret_cast<const float*>(main + 0x1c0);
        const bool atOrigin = fabsf(cam[12]) + fabsf(cam[13]) + fabsf(cam[14]) < 1.0f;
        const double vfov = proj[5] != 0 ? 2.0 * atan(1.0 / fabs(proj[5])) * 57.29577951 : 0;
        // The title's fly-by holds one spot with a 33.3 degree lens.
        const float titleDist = fabsf(cam[12] + 17.55f) + fabsf(cam[13] - 39.476f) + fabsf(cam[14] + 223.651f);
        const bool titleSpot = titleDist < 1.0f || (titleDist < 12.0f && fabs(vfov - 33.30) < 0.02);
        // Frame to frame change of the game's camera, position and direction.
        static float lastCam[16];
        float moved = 0;
        for (int i = 0; i < 3; ++i) moved = (std::max)(moved, fabsf(cam[12 + i] - lastCam[12 + i]));
        for (int i = 8; i < 11; ++i) moved = (std::max)(moved, fabsf(cam[i] - lastCam[i]));
        memcpy(lastCam, cam, sizeof(lastCam));
        static int zoomHeld = 0, vendorOff = 0;
        const bool zoomed = vfov > 0 && vfov < 13.0;
        zoomHeld = zoomed && moved < 0.003f ? zoomHeld + 1 : 0;
        // The numbers the rule rests on, for checking it against a real vendor in Steve's next log.
        if (zoomed && g_labProbes) {
            static int zoomLog = 0;
            if (++zoomLog % 30 == 1) Log("VENDOR zoom lens %.2f moved %.5f held %d", vfov, moved, zoomHeld);
        }
        static bool vendor = false;
        static int cineOn = 0, cineOff = 0;
        static bool cine = false;
        const int cutsceneRule = g_cutsceneCamera.load();
        const bool inCutscene = *reinterpret_cast<int*>(g_base + kCinematicTracks) > 0;
        // Zoomed: starts under lens 25, holds until 30, so a shot does not flip back and forth.
        const bool wanted = inCutscene && (cutsceneRule == 1 || (cutsceneRule == 0 && vfov > 0 && vfov < (cine ? 30.0 : 25.0)));
        if (wanted) {
            ++cineOn;
            cineOff = 0;
        } else {
            ++cineOff;
            cineOn = 0;
        }
        // On at once: the hang can come within seconds of the zoom.
        if (cineOn >= 1) cine = true;
        if (cineOff >= 15) cine = false;
        if (g_forceVendor.load() || zoomHeld >= 8 || cine) {
            vendor = true;
            vendorOff = 0;
        } else if (!zoomed) {
            // The zoom going away ends it fast. A small wobble while still zoomed does not.
            if (++vendorOff >= 3) vendor = false;
        } else if (moved >= 0.05f) {
            if (++vendorOff >= 8) vendor = false;
        } else {
            vendorOff = 0;
        }
        vendor = vendor && g_vendorOn.load();
        if (vendor != g_vendorNow.load()) {
            Log("MODE vendor screen %s, lens %.2f", vendor ? "starts" : "ends", vfov);
            g_vendorNow = vendor;
            XrSetCinema(vendor);
            OfxrPacingUpdate();
        }
        // Only the title's own spots are flat. A rule on its 39.43 lens used to be here, and gameplay's aim zoom can pass through
        // that lens: Steve's log has it at 39.41, the flat screen came on, and no menu option could stop it.
        world = !atOrigin && !titleSpot;
        static bool seenWorld = false;
        if (world != seenWorld) {
            Log("MODE %s spot, camera %.2f %.2f %.2f lens %.2f", world ? "world" : "title", cam[12], cam[13], cam[14], vfov);
            seenWorld = world;
        }
    }
    OfxrPacingTick();
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
        OfxrPacingUpdate();
    }
}

static void CreateEyeNow() { CreateEyeImpl(); }

// ---- What fills memory: the whole system's commit, this process, and the card. Goes with the PERF lines. ----
static void LogMemory() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    GlobalMemoryStatusEx(&m);
    PROCESS_MEMORY_COUNTERS_EX pc{};
    pc.cb = sizeof(pc);
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pc), sizeof(pc));
    static IDXGIAdapter3* adapter = nullptr;
    static bool tried = false;
    if (!tried && CaptureDevice()) {
        tried = true;
        const HMODULE d = LoadLibraryW(L"dxgi.dll");
        using CreateFn = HRESULT(WINAPI*)(REFIID, void**);
        const auto create = d ? reinterpret_cast<CreateFn>(GetProcAddress(d, "CreateDXGIFactory1")) : nullptr;
        IDXGIFactory4* factory = nullptr;
        if (create && SUCCEEDED(create(IID_PPV_ARGS(&factory))) && factory) {
            const LUID luid = static_cast<ID3D12Device*>(CaptureDevice())->GetAdapterLuid();
            factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
            factory->Release();
        }
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    if (adapter) adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    const auto mb = [](uint64_t b) { return static_cast<unsigned long long>(b / (1024 * 1024)); };
    Log("MEM system commit %llu of %llu MB, RAM free %llu of %llu MB; game private %llu, working set %llu MB; card %llu of %llu MB", mb(m.ullTotalPageFile - m.ullAvailPageFile), mb(m.ullTotalPageFile), mb(m.ullAvailPhys), mb(m.ullTotalPhys), mb(pc.PrivateUsage), mb(pc.WorkingSetSize), mb(local.CurrentUsage), mb(local.Budget));
}

// ---- Where the CPU time goes: every thread of the game, whole machine, at each PERF report. Lab probe. ----
static std::atomic<DWORD> g_gameThreadId{0}, g_presentThreadId{0}, g_topThreadId{0};
static void LogThreads() {
    static std::map<DWORD, uint64_t> last;
    static uint64_t lastWall = 0, lastBusy = 0, lastIdle = 0;
    FILETIME wallFt{};
    GetSystemTimeAsFileTime(&wallFt);
    const uint64_t wall = (static_cast<uint64_t>(wallFt.dwHighDateTime) << 32) | wallFt.dwLowDateTime;
    FILETIME idleFt{}, kernelFt{}, userFt{};
    GetSystemTimes(&idleFt, &kernelFt, &userFt);
    const auto u64 = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    const uint64_t idle = u64(idleFt), busy = u64(kernelFt) + u64(userFt) - idle;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    std::vector<std::pair<uint64_t, DWORD>> delta;
    uint64_t total = 0;
    std::map<DWORD, uint64_t> now;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME a, b, k, us;
        if (GetThreadTimes(h, &a, &b, &k, &us)) {
            const uint64_t t = u64(k) + u64(us);
            now[te.th32ThreadID] = t;
            auto it = last.find(te.th32ThreadID);
            if (it != last.end() && t >= it->second) {
                delta.push_back({t - it->second, te.th32ThreadID});
                total += t - it->second;
            }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    last.swap(now);
    const uint64_t span = wall - lastWall;
    const uint64_t sysBusy = busy - lastBusy, sysIdle = idle - lastIdle;
    const bool have = lastWall != 0 && span > 0;
    lastWall = wall;
    lastBusy = busy;
    lastIdle = idle;
    if (!have) return;
    std::sort(delta.rbegin(), delta.rend());
    for (const auto& d : delta) {
        if (d.second != g_gameThreadId.load()) {
            g_topThreadId = d.second;
            break;
        }
    }
    char line[300];
    int w = 0;
    const DWORD game = g_gameThreadId.load(), present = g_presentThreadId.load();
    for (size_t i = 0; i < delta.size() && i < 8 && w < 240; ++i) {
        const double pct = 100.0 * static_cast<double>(delta[i].first) / static_cast<double>(span);
        w += sprintf_s(line + w, sizeof(line) - w, "%s%.0f ", delta[i].second == game ? "game=" : delta[i].second == present ? "render=" : "", pct);
    }
    double gamePct = 0, presentPct = 0;
    for (const auto& d : delta) {
        const double pct = 100.0 * static_cast<double>(d.first) / static_cast<double>(span);
        if (d.second == game) gamePct = pct;
        if (d.second == present) presentPct = pct;
    }
    Log("CPU game thread %.0f%%, render thread %.0f%%, process %.2f cores, whole PC %.0f%% busy; top threads %s", gamePct, presentPct, static_cast<double>(total) / static_cast<double>(span), 100.0 * static_cast<double>(sysBusy) / static_cast<double>(sysBusy + sysIdle), line);
}

// ---- What the busiest thread is doing. Stops it briefly 40 times every 20 s and copies its instruction pointer and top of stack. ----
// Nothing is logged, allocated or locked while it is stopped: it could be holding the lock. Lab probe.
static std::wstring ModuleOf(uint64_t a, uint64_t* offset) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a), &m) || !m) return L"";
    GetModuleFileNameW(m, path, MAX_PATH);
    *offset = a - reinterpret_cast<uint64_t>(m);
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}
static DWORD WINAPI ThreadSampler(void*) {
    static uint8_t stackCopy[0x3000];
    using NtQueryFn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    const auto query = reinterpret_cast<NtQueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    using DescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto description = reinterpret_cast<DescFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    for (;;) {
        Sleep(20000);
        const DWORD tid = g_topThreadId.load();
        if (!g_labProbes.load() || !tid || tid == GetCurrentThreadId() || !g_xrEyes.load()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!h) continue;
        uint64_t start = 0;
        if (query) query(h, 9, &start, sizeof(start), nullptr);
        std::wstring name;
        PWSTR raw = nullptr;
        if (description && SUCCEEDED(description(h, &raw)) && raw) {
            name = raw;
            LocalFree(raw);
        }
        std::map<std::wstring, int> where;
        std::map<uint64_t, int> frames;
        int got = 0;
        for (int i = 0; i < 40; ++i) {
            Sleep(25);
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(h) == static_cast<DWORD>(-1)) break;
            const BOOL ok = GetThreadContext(h, &c);
            bool copied = false;
            if (ok) copied = SafeCopy(stackCopy, reinterpret_cast<void*>(c.Rsp), sizeof(stackCopy));
            ResumeThread(h);
            if (!ok) continue;
            ++got;
            uint64_t off = 0;
            std::wstring mod = ModuleOf(c.Rip, &off);
            ++where[mod.empty() ? L"?" : mod];
            // The nearest address in the game's own code says what it was doing for the game.
            const uint64_t lo = reinterpret_cast<uint64_t>(g_base) + g_textStart, hi = lo + g_textSize;
            uint64_t hit = c.Rip >= lo && c.Rip < hi ? c.Rip : 0;
            if (!hit && copied) {
                const uint64_t* words = reinterpret_cast<const uint64_t*>(stackCopy);
                for (size_t k = 0; k < sizeof(stackCopy) / 8 && !hit; ++k) {
                    if (words[k] >= lo && words[k] < hi) hit = words[k];
                }
            }
            if (hit) ++frames[(hit - reinterpret_cast<uint64_t>(g_base)) & ~0xffull];
        }
        CloseHandle(h);
        uint64_t startOff = 0;
        const std::wstring startMod = start ? ModuleOf(start, &startOff) : L"";
        char line[400];
        int w = 0;
        std::vector<std::pair<int, std::wstring>> wl;
        for (const auto& e : where) wl.push_back({e.second, e.first});
        std::sort(wl.rbegin(), wl.rend());
        for (size_t i = 0; i < wl.size() && i < 6; ++i) w += sprintf_s(line + w, sizeof(line) - w, "%ls %d, ", wl[i].second.c_str(), wl[i].first);
        Log("TOPTHREAD %lu name '%ls' start %ls+%llx, %d samples in: %s", tid, name.c_str(), startMod.c_str(), static_cast<unsigned long long>(startOff), got, line);
        w = 0;
        std::vector<std::pair<int, uint64_t>> fl;
        for (const auto& e : frames) fl.push_back({e.second, e.first});
        std::sort(fl.rbegin(), fl.rend());
        for (size_t i = 0; i < fl.size() && i < 10 && w < 340; ++i) w += sprintf_s(line + w, sizeof(line) - w, "%llx x%d, ", static_cast<unsigned long long>(fl[i].second), fl[i].first);
        Log("TOPTHREAD game code near: %s", line);
    }
}

// ---- When the GPU hangs: D3D12's own recorder says which command list and which operation it stopped in. ----
// Must be turned on before the game makes its device. DeviceDiagnostics=0 in the ini turns it off.
static void DredEnable() {
    const HMODULE d = LoadLibraryW(L"d3d12.dll");
    using GetFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto get = d ? reinterpret_cast<GetFn>(GetProcAddress(d, "D3D12GetDebugInterface")) : nullptr;
    ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
    if (get && SUCCEEDED(get(IID_PPV_ARGS(&settings))) && settings) {
        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->Release();
        Log("DRED on");
        char cpu[128] = {};
        DWORD size = sizeof(cpu);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, nullptr, cpu, &size) == ERROR_SUCCESS) Log("CPU %s, %u logical", cpu, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    } else {
        Log("DRED not available");
    }
}
static void DredReport(ID3D12Device* device, HRESULT why) {
    Log("DRED device removed %08lx, camera below", static_cast<unsigned long>(why));
    LogView();
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
        Log("DRED no data");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT out{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&out))) {
        int shown = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = out.pHeadAutoBreadcrumbNode; n && shown < 12; n = n->pNext) {
            const UINT32 last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (last >= n->BreadcrumbCount) continue;  // this list finished
            ++shown;
            Log("DRED unfinished list %p queue %p, %ls / %ls, done %u of %u", n->pCommandList, n->pCommandQueue, n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"", n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"", last, n->BreadcrumbCount);
            char ops[300];
            int w = 0;
            for (UINT32 i = last > 6 ? last - 6 : 0; i < n->BreadcrumbCount && i < last + 8 && w < 280; ++i) w += sprintf_s(ops + w, sizeof(ops) - w, i == last ? "[%d] " : "%d ", static_cast<int>(n->pCommandHistory[i]));
            Log("DRED  operations, [next]: %s", ops);
        }
        Log("DRED %d lists unfinished", shown);
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))) {
        Log("DRED page fault at %llx", static_cast<unsigned long long>(fault.PageFaultVA));
        int shown = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* n = fault.pHeadExistingAllocationNode; n && shown < 6; n = n->pNext, ++shown) Log("DRED  existing %s type %d", n->ObjectNameA ? n->ObjectNameA : "", static_cast<int>(n->AllocationType));
        shown = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* n = fault.pHeadRecentFreedAllocationNode; n && shown < 6; n = n->pNext, ++shown) Log("DRED  freed %s type %d", n->ObjectNameA ? n->ObjectNameA : "", static_cast<int>(n->AllocationType));
    }
    dred->Release();
}
static DWORD WINAPI DredWatch(void*) {
    for (;;) {
        Sleep(500);
        auto* device = static_cast<ID3D12Device*>(CaptureDevice());
        if (!device) continue;
        const HRESULT why = device->GetDeviceRemovedReason();
        if (FAILED(why)) {
            DredReport(device, why);
            return 0;
        }
    }
}

// ---- timing probe, game thread, reset every report ----
static uint64_t PerfNow() {
    static const double toUs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e6 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(static_cast<double>(c.QuadPart) * toUs);
}
static std::atomic<uint64_t> g_usFrame{0}, g_usPace{0}, g_usOffscreen{0}, g_usEyeSubmit{0}, g_usMainSubmit{0}, g_usGamePart{0}, g_perfFrames{0};
static void __fastcall HookRenderAll() {
    ++g_frames;
    g_gameThreadId = GetCurrentThreadId();
    {
        // Whole frame: from one arrival here to the next.
        static uint64_t last = 0;
        const uint64_t nowUs = PerfNow();
        if (last) {
            g_usFrame += nowUs - last;
            ++g_perfFrames;
        }
        last = nowUs;
    }
    {
        const uint64_t t = PerfNow();
        if (g_xrSession && g_pace) XrPaceWait();
        g_usPace += PerfNow() - t;
    }
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
                const int levelMode = g_levelCamera.load();
                if (levelMode != 0) {
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
                    // Auto: the camera's right row leaving level means gravity is sideways. Follow the camera there, smoothly.
                    const float lean = fabsf(cam[1]);
                    static bool leaning = false;
                    if (levelMode == 2) {
                        if (lean > 0.35f) leaning = true;
                        else if (lean < 0.15f) leaning = false;
                    } else {
                        leaning = false;
                    }
                    g_levelBlend = std::clamp(g_levelBlend + (leaning ? 1.0f : -1.0f) / 45.0f, 0.0f, 1.0f);
                    float rows[12];
                    for (int i = 0; i < 12; ++i) rows[i] = level[i] + (cam[i] - level[i]) * g_levelBlend;
                    // Square the rows up again: forward, then right against it, then down from both.
                    float* F = rows + 8;
                    float n = sqrtf(F[0] * F[0] + F[1] * F[1] + F[2] * F[2]);
                    for (int i = 0; i < 3; ++i) F[i] /= n;
                    float* R = rows;
                    const float dot = R[0] * F[0] + R[1] * F[1] + R[2] * F[2];
                    for (int i = 0; i < 3; ++i) R[i] -= F[i] * dot;
                    n = sqrtf(R[0] * R[0] + R[1] * R[1] + R[2] * R[2]);
                    for (int i = 0; i < 3; ++i) R[i] /= n;
                    float* D = rows + 4;
                    D[0] = F[1] * R[2] - F[2] * R[1];
                    D[1] = F[2] * R[0] - F[0] * R[2];
                    D[2] = F[0] * R[1] - F[1] * R[0];
                    memcpy(cam, rows, sizeof(rows));
                }
                const float nearZ = *reinterpret_cast<float*>(main + 0x3f8), farZ = *reinterpret_cast<float*>(main + 0x3fc);
                {
                    // The game aims along its camera's forward row. In the room: undo the level frame, then the centring.
                    const float* real = reinterpret_cast<const float*>(main);
                    const float d0 = real[8], d1 = real[9], d2 = real[10];
                    const float vx = d0 * cam[0] + d1 * cam[1] + d2 * cam[2];
                    const float vy = -(d0 * cam[4] + d1 * cam[5] + d2 * cam[6]);
                    const float vz = -(d0 * cam[8] + d1 * cam[9] + d2 * cam[10]);
                    const float yc = cosf(-g_centreYaw), ys = sinf(-g_centreYaw);
                    const float room[3] = {yc * vx - ys * vz, vy, ys * vx + yc * vz};
                    const float* lensNow = reinterpret_cast<const float*>(main + 0x1c0);
                    const double lens = lensNow[5] != 0 ? 2.0 * atan(1.0 / fabs(lensNow[5])) * 57.29577951 : 0;
                    // Aiming lens, or a trigger touched in the last two seconds: the game's reticle is up.
                    const bool aiming = fabs(lens - 32.64) < 0.5 || GetTickCount64() - g_lastTrigger.load() < 2000;
                    const int dot = g_aimDot.load();
                    const bool dotOn = !g_vendorNow && (dot == 2 || (dot == 1 && aiming));
                    XrSetAimDot(dotOn ? room : nullptr, g_centre);
                    // Paused or in a cutscene the HUD image holds menus: otherwise the game's reticle never shows, aiming or not.
                    XrSetHudHole(!g_vendorNow && dot != 0 && *reinterpret_cast<int*>(g_base + kPausedFlag) == 0);
                    g_aimRoom[0] = room[0];
                    g_aimRoom[1] = room[1];
                    g_aimRoom[2] = room[2];
                }
                float jx = 0, jy = 0;
                reinterpret_cast<JitterFn>(g_base + kJitter.rva)(main, &jx, &jy);
                // Flag bit 0 makes the engine skip an offscreen view. The skipped eye also gets no new camera, so its
                // motion vectors still run from the tick it was last drawn.
                const bool alternate = g_alternateSetting.load() && !g_vendorNow.load();
                static int freshEye = 0;
                if (alternate) freshEye ^= 1;
                if (alternate != g_alternateNow.load()) {
                    g_alternateNow = alternate;
                    XrSetAlternate(alternate);
                    Log("MODE alternate eye %s", alternate ? "on" : "off");
                    OfxrApply();
                }
                for (int fe = 0; fe < 2; ++fe) {
                    uint32_t& flags = *reinterpret_cast<uint32_t*>(g_eyes[fe] + 0x16ccc);
                    flags = alternate && fe != freshEye ? (flags | 1u) : (flags & ~1u);
                }
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
                    XrTagEyeCamera(e, right, up, back, eyePose + 12, p.tanLeft, p.tanRight, p.tanUp, p.tanDown, nearZ);
                    if (alternate && e != freshEye) continue;
                    XrTagPose(e, eyePose + 12);
                    const bool fix = g_lensFix.load();
                    if (g_vendorNow) {
                        // The game's own camera and framing, both eyes the same: that screen is close. The head does not move it.
                        memcpy(eyePose, main, sizeof(eyePose));
                        XrTagPose(e, eyePose + 12);
                        // Width as the game has it; height fills the eye image, of which the middle 16 by 9 band is shown.
                        const float* noJitter = reinterpret_cast<const float*>(main + 0x1c0);
                        const float tanX = 1.0f / fabsf(noJitter[0]);
                        const float tanY = tanX * static_cast<float>(g_forceH) / static_cast<float>(g_forceW);
                        reinterpret_cast<LensPerspFn>(g_base + kLensPersp.rva)(desc, nearZ, farZ, -tanX, tanX, -tanY, tanY, 0, 0, jx, jy, 0, 0);
                    } else {
                    reinterpret_cast<LensPerspFn>(g_base + kLensPersp.rva)(desc, nearZ, farZ, p.tanLeft, p.tanRight, fix ? -p.tanUp : p.tanDown, fix ? -p.tanDown : p.tanUp, 0, 0, jx, jy, 0, 0);
                    }
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
    {
        const uint64_t t = PerfNow();
        g_origRenderAll();
        g_usOffscreen += PerfNow() - t;
    }
}

// ---- pass queue: what each view asks the renderer to do. Lab log, and a list of passes an eye leaves out. ----
using PassQueueFn = int64_t(__fastcall*)(void*, int64_t, int64_t, int64_t);
static PassQueueFn g_origPassQueue;
// ---- Each model of each view goes through here. ----
// The HUD is a real model in the world, models/ui/playerhudrtt, placed by the game near its camera. In the game's own view
// it lands in the UI overlay. In an offscreen view it lands in the picture, 20 units out, with bloom: the doubled HUD.
// So the eyes leave out every model under models/ui. The context's +8 is the view it works for.
static std::atomic<bool> g_eyeDropUi{true};
using ModelQueueFn = void(__fastcall*)(uint8_t*, uint8_t*, char);
static ModelQueueFn g_origModelQueue;
static const char kUiModels[] = "models\x5cui\x5c";
static void __fastcall HookModelQueue(uint8_t* ctx, uint8_t* inst, char flag) {
    const uint8_t* const view = *reinterpret_cast<uint8_t* const*>(ctx + 8);
    if ((view == g_eyeA.load() || view == g_eyeB.load()) && g_eyeDropUi.load()) {
        const uint8_t* const model = *reinterpret_cast<uint8_t* const*>(inst + 0x88);
        const char* const name = model ? *reinterpret_cast<const char* const*>(model + 0x10) : nullptr;
        if (name && memcmp(name, kUiModels, sizeof(kUiModels) - 1) == 0) return;
    }
    g_origModelQueue(ctx, inst, flag);
}

// Resting the flat view: the scene submit returns at once when view +0x11d8 is clear. Set again when not resting.
static std::atomic<bool> g_restFlat{false};
static std::atomic<int> g_skipMain{4};
static std::atomic<bool> g_mainEveryOverride{false};  // lab
static std::atomic<uint64_t> g_mainSkipped{0};
static std::atomic<bool> g_eyeDefer{false}, g_eyeShare{false};
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
        if (g_labProbes) {
            std::lock_guard<std::mutex> l(g_statsLock);
            ++g_sceneSubmits[view];
            g_sceneMask[view] = mask;
        }
    }
    // Eyes read the main view's Adapted Lum, so they have the exposure the game chose. The handle is only lent while
    // the scene submit copies the view into its job: if it stayed, rebuilding an eye would free the main view's buffer.
    // The game's own view only feeds the HUD in VR: the eyes are the picture and the monitor shows an eye. It runs one
    // frame in this many, because the UI overlay is drawn in its work. 0 every frame. 1 never, and the HUD freezes.
    if (view == ActiveView() && g_xrEyes.load()) {
        // A pause menu only draws if this view runs every frame. So does the vendor screen's menu. In VR a still game camera says
        // nothing about that: standing still and looking around is normal, so it is not a reason to run this view.
        const bool menuUp = *reinterpret_cast<int*>(g_base + kPausedFlag) != 0 || g_vendorNow.load();
        const int every = menuUp || g_mainEveryOverride.load() ? 0 : g_skipMain.load();
        static uint32_t count = 0;
        ++count;
        if (every == 1 || (every > 1 && count % static_cast<uint32_t>(every) != 0)) {
            ++g_mainSkipped;
            return nullptr;
        }
    }
    uint8_t* main = g_shareLum ? ActiveView() : nullptr;
    if (main && view != main && (view == g_eyeA.load() || view == g_eyeB.load())) {
        uint8_t saved[0x38];
        memcpy(saved, view + 0x1740, sizeof(saved));
        memcpy(view + 0x1740, main + 0x1740, sizeof(saved));
        const uint64_t t = PerfNow();
        // Lab, Fable's leads. Defer: the game thread does not wait for this eye's model jobs, as with the main view.
        // Share: the right eye takes the left's lights and key light shadows, by the submit's own share struct.
        static void* leftResult = nullptr;
        const bool isLeft = view == g_eyeA.load();
        void* const share = g_eyeShare.load() && !isLeft && leftResult ? leftResult : a;
        void* r = g_origSceneSubmit(view, share, mask, g_eyeDefer.load() ? 1 : b);
        if (isLeft) leftResult = r;
        g_usEyeSubmit += PerfNow() - t;
        memcpy(view + 0x1740, saved, sizeof(saved));
        return r;
    }
    const uint64_t t = PerfNow();
    void* r = g_origSceneSubmit(view, a, mask, b);
    g_usMainSubmit += PerfNow() - t;
    return r;
}

static void __fastcall HookCameraSubmit(uint8_t* view, const uint8_t* desc) {
    if (g_labProbes) {
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
    MH_CreateHook(g_base + 0x11212d0, reinterpret_cast<void*>(&HookModelQueue), reinterpret_cast<void**>(&g_origModelQueue));
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
static std::atomic<int> g_uiCount{1};
static std::atomic<bool> g_mirrorEye{true};

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
    // 5 is the game's composite of its whole UI: HUD pieces from a parts sheet, reticle and menus. One image, so nothing doubles.
    const int ids[3] = {5, 3, 4};
    for (int i = 0; i < 3 && !rebuilding; ++i) {
        uint8_t* tex = UiTexture(ids[i], &ui[i].width, &ui[i].height);
        if (tex) ui[i].resource = TextureResource(tex, &ui[i].state);
    }
    ui[0].keepLast = g_skipMain.load() != 0 && !g_overlayFresh.exchange(false);
    {
        // Lab: dump the HUD image exactly when a fresh one is about to be copied.
        static std::atomic<int>& want = g_wantHudShot;
        if (!ui[0].keepLast && ui[0].resource && want.exchange(0)) {
            void* r[1] = {ui[0].resource};
            uint32_t st0[1] = {ui[0].state};
            static std::wstring path;
            path = g_dir + L"vrlab_shots\\hudshot.bmp";
            const wchar_t* paths[1] = {path.c_str()};
            Log("HUD shot queued: %s", CaptureRequest(r, st0, paths, 1) ? "yes" : "no");
        }
    }
    XrSetMirror(g_mirrorEye.load() ? backBuffer : nullptr);
    g_presentThreadId = GetCurrentThreadId();
    XrOnPresent(res, st, ui, g_uiCount.load());
    if (!g_rbShotName.empty() && XrRebuildResult() && res[0] && res[1]) {
        // Lab: the real eyes and the rebuilt one from the same frame, to compare.
        static std::wstring paths[3];
        const std::wstring base(g_rbShotName.begin(), g_rbShotName.end());
        paths[0] = g_dir + L"vrlab_shots\\" + base + L"_L.bmp";
        paths[1] = g_dir + L"vrlab_shots\\" + base + L"_R.bmp";
        paths[2] = g_dir + L"vrlab_shots\\" + base + L"_B.bmp";
        void* r[3] = {res[0], res[1], XrRebuildResult()};
        uint32_t s3[3] = {st[0], st[1], D3D12_RESOURCE_STATE_COPY_SOURCE};
        const wchar_t* pp[3] = {paths[0].c_str(), paths[1].c_str(), paths[2].c_str()};
        Log("REBUILD shot: %s", CaptureRequest(r, s3, pp, 3) ? "queued" : "not ready");
        g_rbShotName.clear();
    }
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
// ---- OFXR frame generation. A separate program in the ofxr folder, loaded at launch if chosen. Once loaded it takes changes
// live through the control functions it exports for other modules in the process, and its INI beside the DLL for 3X. ----
static int g_fgMode = 0;      // 0 off, 1 2X, 2 3X
static int g_fgPreset = 1;    // OFXR's numbers: 0 slow, 1 medium, 2 fast
static int g_fgScale = 2;     // OFXR's numbers: 0 full, 1 three quarters, 2 half, 3 quarter
static int g_fgType = 0;      // 0 optical flow, 1 FSR 3.1, 2 DLSS-G, 3 Hybrid
static int g_fgTypeAtLaunch = 0;
static bool g_overlapSetting = false;
static bool g_fgDirector = false;
struct OfxrSettingsV2 {
    uint32_t structSize;
    int32_t enabled, backend, preset, scale, backward, motionVectors, frameGeneration;
};
struct OfxrSnapshotV2 {
    uint32_t structSize, apiVersion;
    OfxrSettingsV2 desired;
    uint64_t revision;
    uint32_t sessions, pending, bypass, ready, errors;
    int32_t lastError;
    uint32_t providers;
};
struct OfxrGuideStats {
    uint32_t structSize, apiVersion;
    uint64_t published, depthPublications, invalidPublications, snapshotCopies, snapshotFailures, resolveCalls, resolveMissingStreams, resolveStalePairs, resolveQueueMismatches, matched, temporalRejections, invalidRejections, used, lastUsedPublication;
    uint32_t status;
    uint64_t fsrAttempts, fsrUsed, fsrFailures;
    uint32_t fsrStatus;
};
static HMODULE OfxrModule() { return GetModuleHandleW(L"XR_APILAYER_XRFrameBridge_diagnostic.dll"); }
static bool OfxrSnapshot(OfxrSnapshotV2* out) {
    const HMODULE m = OfxrModule();
    const auto get = m ? reinterpret_cast<int(*)(OfxrSnapshotV2*)>(GetProcAddress(m, "OFXR_GetControlSnapshotV2")) : nullptr;
    if (!get) return false;
    *out = {};
    out->structSize = sizeof(OfxrSnapshotV2);
    out->apiVersion = 2;
    out->desired.structSize = sizeof(OfxrSettingsV2);
    return get(out) != 0;
}
static void OfxrLogState(const char* when) {
    OfxrSnapshotV2 v;
    if (!OfxrSnapshot(&v)) {
        Log("FRAMEGEN %s: not loaded", when);
        return;
    }
    Log("FRAMEGEN %s: enabled %d type %d backend %d preset %d scale %d, revision %llu sessions %u pending %u bypass %u ready %u errors %u last error %d", when, v.desired.enabled, v.desired.frameGeneration, v.desired.backend, v.desired.preset, v.desired.scale, static_cast<unsigned long long>(v.revision), v.sessions, v.pending, v.bypass, v.ready, v.errors, v.lastError);
    if (const auto stats = reinterpret_cast<int(*)(OfxrGuideStats*)>(GetProcAddress(OfxrModule(), "OFXR_GetDlssGuideStatisticsV2"))) {
        OfxrGuideStats g{};
        g.structSize = sizeof(g);
        g.apiVersion = 2;
        if (stats(&g)) Log("FRAMEGEN guides: published %llu with depth %llu invalid %llu, resolve calls %llu missing %llu stale %llu, matched %llu used %llu, status %u, fsr attempts %llu used %llu failures %llu status %u", g.published, g.depthPublications, g.invalidPublications, g.resolveCalls, g.resolveMissingStreams, g.resolveStalePairs, g.matched, g.used, g.status, g.fsrAttempts, g.fsrUsed, g.fsrFailures, g.fsrStatus);
    }
}
// Director pacing hung the render thread for good on RigB: at the title if on from launch, and seconds after a switch in the world.
// RigB's null driver may be why: not known. So every launch starts on Framework, Director is only asked for in the world, and the
// menu's choice is saved only after it has run for 30 s, so a freeze cannot bring itself back at the next launch.
static void OfxrPacingUpdate() {
    const HMODULE m = OfxrModule();
    const auto request = m ? reinterpret_cast<int(*)(int)>(GetProcAddress(m, "OFXR_RequestPacingV1")) : nullptr;
    if (!request) return;
    const int want = g_fgDirector && g_xrEyes.load() && !g_vendorNow.load() ? 1 : 0;
    static int current = -1;
    if (want == current) return;
    current = want;
    Log("FRAMEGEN pacing %s, accepted %d", want ? "Director" : "Framework", request(want));
}
static void SaveSetting(const wchar_t* name, int value);
static void OfxrPacingTick() {
    static ULONGLONG since = 0;
    static bool saved = false;
    const bool on = g_fgDirector && OfxrModule() && g_xrEyes.load() && !g_vendorNow.load();
    if (!on) {
        since = 0;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!since) since = now;
    if (!saved && now - since > 30000) {
        saved = true;
        SaveSetting(L"FrameGenDirector", 1);
        Log("FRAMEGEN Director ran 30 s, saved");
    }
}
// Live when OFXR is loaded. 3X only exists on its 0.2.14 route, backend 3, so the mode picks the route too.
static void OfxrApply() {
    const HMODULE m = OfxrModule();
    const auto request = m ? reinterpret_cast<int(*)(const OfxrSettingsV2*)>(GetProcAddress(m, "OFXR_RequestControlV2")) : nullptr;
    if (!request) {
        Log("FRAMEGEN mode %d saved for next launch: OFXR is not loaded", g_fgMode);
        return;
    }
    // Hybrid can only be chosen or left at a launch: OFXR's control functions do not carry it.
    if (g_fgType == 3 || g_fgTypeAtLaunch == 3) {
        Log("FRAMEGEN Hybrid involved, saved for next launch");
        if (g_fgTypeAtLaunch != 3 || g_fgType != 3) return;
    }
    const bool triple = g_fgMode == 2 && (g_fgType == 0 || g_fgType == 2);
    const std::wstring ini = g_dir + L"ofxr\\ofxr_bridge.ini";
    WritePrivateProfileStringW(L"ofxr", L"deep_pipeline", L"1", ini.c_str());
    WritePrivateProfileStringW(L"ofxr", L"triple_frame_gen", triple ? L"1" : L"0", ini.c_str());
    OfxrSettingsV2 c{};
    c.structSize = sizeof(c);
    c.enabled = g_fgMode != 0 && !g_alternateNow.load();
    c.backend = g_fgType == 3 ? 0 : triple ? 3 : 2;
    c.frameGeneration = g_fgType == 1 ? 1 : g_fgType == 2 ? 2 : 0;
    c.preset = g_fgPreset;
    c.scale = g_fgScale;
    const int ok = request(&c);
    Log("FRAMEGEN asked mode %d type %d preset %d scale %d, accepted %d", g_fgMode, g_fgType, g_fgPreset, g_fgScale, ok);
}
static float g_hudDistanceSetting = 2.0f;
static std::wstring g_iniPath;
static std::vector<uint32_t> g_menuImage;
static const int kMenuW = 1024, kMenuH = 1488, kMenuRows = 18;

static void SaveSetting(const wchar_t* name, int value) {
    WritePrivateProfileStringW(L"VR", name, std::to_wstring(value).c_str(), g_iniPath.c_str());
}

static void MenuRowText(int row, wchar_t (&name)[64], wchar_t (&value)[64]) {
    value[0] = 0;
    switch (row) {
    case 0: wcscpy_s(name, L"Recenter view"); break;
    case 1:
        wcscpy_s(name, L"World scale");
        swprintf_s(value, L"%d%%", static_cast<int>(g_worldScale.load() * 100 + 0.5f));
        break;
    case 2:
        wcscpy_s(name, L"Decoupled pitch");
        wcscpy_s(value, g_levelCamera == 0 ? L"Off" : g_levelCamera == 1 ? L"On" : L"Auto");
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
        wcscpy_s(name, L"Skip the launcher window (next launch)");
        wcscpy_s(value, g_skipLauncherSetting ? L"On" : L"Off");
        break;
    case 6:
        wcscpy_s(name, L"Vendor screen up close");
        wcscpy_s(value, g_vendorOn ? L"On" : L"Off");
        break;
    case 7:
        wcscpy_s(name, L"Aim dot");
        wcscpy_s(value, g_aimDot == 0 ? L"Off" : g_aimDot == 1 ? L"While aiming" : L"Always");
        break;
    case 8:
        wcscpy_s(name, L"Snapshot in 2 seconds");
        break;
    case 9:
    {
        wcscpy_s(name, L"Frame generation");
        OfxrSnapshotV2 v;
        const bool loaded = OfxrSnapshot(&v);
        const bool threeX = g_fgMode == 2 && g_fgType != 1 && g_fgType != 3;
        swprintf_s(value, L"%ls%ls", g_fgMode == 0 ? L"Off" : threeX ? L"3X" : L"2X", g_fgMode == 0 ? L"" : g_alternateSetting ? L", off in alternate" : !loaded ? L" next launch" : v.errors ? L" failed" : L"");
        break;
    }
    case 10: {
        static const wchar_t* const types[4] = {L"Optical flow", L"FSR 3.1 (no frames)", L"DLSS-G", L"Hybrid"};
        wcscpy_s(name, L"Frame generation type");
        OfxrSnapshotV2 v;
        const bool notLive = !OfxrSnapshot(&v) || g_fgType != g_fgTypeAtLaunch && (g_fgType == 3 || g_fgTypeAtLaunch == 3);
        swprintf_s(value, L"%ls%ls", types[g_fgType], notLive && g_fgMode != 0 ? L" next launch" : L"");
        break;
    }
    case 11:
        wcscpy_s(name, L"Optical flow quality");
        wcscpy_s(value, g_fgPreset == 2 ? L"Fast" : g_fgPreset == 1 ? L"Medium" : L"Slow");
        break;
    case 12:
        wcscpy_s(name, L"Optical flow detail");
        wcscpy_s(value, g_fgScale == 0 ? L"100%" : g_fgScale == 1 ? L"75%" : g_fgScale == 2 ? L"50%" : L"25%");
        break;
    case 13:
        wcscpy_s(name, L"Frame generation pacing");
        wcscpy_s(value, g_fgDirector ? L"Director" : L"Framework");
        break;
    case 16:
        wcscpy_s(name, L"Frame overlap");
        wcscpy_s(value, g_overlapSetting ? L"On" : L"Off");
        break;
    case 15: {
        static const wchar_t* const rules[3] = {L"Zoomed shots", L"All cutscenes", L"Never (can hang)"};
        wcscpy_s(name, L"Cutscenes on a screen");
        wcscpy_s(value, rules[g_cutsceneCamera.load() % 3]);
        break;
    }
    case 14:
        wcscpy_s(name, L"Stereo");
        wcscpy_s(value, !g_alternateSetting ? L"Native" : g_rebuildSetting ? L"Alternate, depth rebuild" : L"Alternate, plain");
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
    // After the game's Options screen: deep blue, a slanted title tab, cyan highlight, a thin line under each row.
    for (int y = 0; y < kMenuH; ++y) {
        const int t = y * 255 / kMenuH;
        fill(0, y, kMenuW, y + 1, RGB(8 + t / 40, 14 + t / 18, 52 + t / 5));
    }
    {
        const POINT tab[] = {{0, 26}, {kMenuW - 330, 26}, {kMenuW - 290, 66}, {kMenuW - 290, 96}, {0, 96}};
        HBRUSH br = CreateSolidBrush(RGB(14, 72, 142));
        HGDIOBJ oldBrush = SelectObject(dc, br);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, tab, 5);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(br);
    }
    fill(0, 96, kMenuW, 99, RGB(40, 216, 232));
    HFONT big = CreateFontW(50, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Bahnschrift");
    HFONT mid = CreateFontW(36, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Bahnschrift");
    HFONT small = CreateFontW(28, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Bahnschrift");
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, big);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT title{44, 30, kMenuW - 300, 92};
    DrawTextW(dc, L"VR OPTIONS", -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, mid);
    for (int i = 0; i < kMenuRows; ++i) {
        const int top = 116 + i * 72;
        if (i == g_menuRow) fill(28, top, kMenuW - 28, top + 66, RGB(40, 216, 232));
        else fill(48, top + 68, kMenuW - 48, top + 69, RGB(40, 78, 130));
        wchar_t name[64], value[64];
        MenuRowText(i, name, value);
        SetTextColor(dc, i == g_menuRow ? RGB(8, 22, 52) : RGB(214, 228, 245));
        RECT a{48, top, kMenuW - 48, top + 66};
        DrawTextW(dc, name, -1, &a, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        DrawTextW(dc, value, -1, &a, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
    SelectObject(dc, small);
    SetTextColor(dc, RGB(150, 182, 214));
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

static DWORD WINAPI SnapshotThread(void*);
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
        g_levelCamera = (g_levelCamera.load() + (dir < 0 ? 2 : 1)) % 3;
        SaveSetting(L"LevelCamera", g_levelCamera.load());
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
        g_skipLauncherSetting = !g_skipLauncherSetting;
        SaveSetting(L"SkipLauncher", g_skipLauncherSetting ? 1 : 0);
        break;
    case 6:
        g_vendorOn = !g_vendorOn;
        SaveSetting(L"VendorScreen", g_vendorOn ? 1 : 0);
        break;
    case 7:
        g_aimDot = (g_aimDot.load() + (dir < 0 ? 2 : 1)) % 3;
        SaveSetting(L"AimDot", g_aimDot.load());
        break;
    case 8:
        if (dir == 0) {
            CreateThread(nullptr, 0, SnapshotThread, nullptr, 0, nullptr);
            MenuClose();
            return;
        }
        break;
    case 9:
        g_fgMode = (g_fgMode + (dir < 0 ? 2 : 1)) % 3;
        SaveSetting(L"FrameGen", g_fgMode);
        OfxrApply();
        break;
    case 10:
        g_fgType = (g_fgType + (dir < 0 ? 3 : 1)) % 4;
        SaveSetting(L"FrameGenType", g_fgType);
        if (g_fgMode) OfxrApply();
        break;
    case 11:
        g_fgPreset = (g_fgPreset + (dir < 0 ? 1 : 2)) % 3;
        SaveSetting(L"FrameGenQuality", g_fgPreset);
        if (g_fgMode) OfxrApply();
        break;
    case 12:
        g_fgScale = (g_fgScale + (dir < 0 ? 1 : 3)) % 4;
        SaveSetting(L"FrameGenDetail", g_fgScale);
        if (g_fgMode) OfxrApply();
        break;
    case 13:
        g_fgDirector = !g_fgDirector;
        if (!g_fgDirector) SaveSetting(L"FrameGenDirector", 0);
        OfxrPacingUpdate();
        break;
    case 14:
    {
        // Native, then alternate with the rebuild, then plain alternate.
        const int now = !g_alternateSetting ? 0 : g_rebuildSetting ? 1 : 2;
        const int next = (now + (dir < 0 ? 2 : 1)) % 3;
        g_alternateSetting = next != 0;
        g_rebuildSetting = next == 1;
        SaveSetting(L"AlternateEye", next == 0 ? 0 : next == 1 ? 2 : 1);
        XrRebuildSetMode(next == 1 ? 1 : 0);
        break;
    }
    case 15:
        g_cutsceneCamera = (g_cutsceneCamera.load() + (dir < 0 ? 2 : 1)) % 3;
        SaveSetting(L"CutsceneCamera", g_cutsceneCamera.load());
        break;
    case 16:
        g_overlapSetting = !g_overlapSetting;
        SaveSetting(L"FrameOverlap", g_overlapSetting ? 1 : 0);
        XrSetOverlap(g_overlapSetting);
        break;
    default:
        if (dir == 0) {
            MenuClose();
            return;
        }
        break;
    }
    Log("MENU row %d dir %d: scale %.2f pitch decoupled %d hud %.2f resolution %d", g_menuRow, dir, g_worldScale.load(), g_levelCamera.load(), g_hudDistanceSetting, g_resolutionSetting);
    MenuDraw();
}

// ---- snapshot: what the mod has in hand, for showing a problem from the headset. Two seconds to close the menu. ----
static std::atomic<int> g_snapNumber{0};
static bool BmpToJpeg(const std::wstring& bmp, const std::wstring& jpg) {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* out = nullptr;
    IPropertyBag2* props = nullptr;
    if (SUCCEEDED(f->CreateDecoderFromFilename(bmp.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(jpg.c_str(), GENERIC_WRITE)) && SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&out, &props)) && SUCCEEDED(out->Initialize(props))) {
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        out->SetSize(w, h);
        out->SetPixelFormat(&fmt);
        ok = SUCCEEDED(out->WriteSource(frame, nullptr)) && SUCCEEDED(out->Commit()) && SUCCEEDED(enc->Commit());
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{props, out, enc, stream, frame, dec, f}) {
        if (u) u->Release();
    }
    return ok;
}
static uint8_t* UiTexture(int i, int* w, int* h);
static void* TextureResource(uint8_t* tex, uint32_t* state);
static DWORD WINAPI SnapshotThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Sleep(2000);
    const int n = g_snapNumber.fetch_add(1) + 1;
    const std::wstring dir = g_dir + L"vrlab_snaps\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%02d%02d%02d_%d", t.wHour, t.wMinute, t.wSecond, n);
    std::vector<void*> res;
    std::vector<uint32_t> states;
    std::vector<std::wstring> bases;
    {
        std::lock_guard<std::mutex> l(g_eyesLock);
        int e = 0;
        for (uint8_t* eye : g_eyes) {
            if (void* r = EyeTexture(eye)) {
                res.push_back(r);
                states.push_back(*reinterpret_cast<uint32_t*>(EyeRecord(eye) + 0x28));
                bases.push_back(std::wstring(stamp) + (e == 0 ? L"_left" : L"_right"));
            }
            ++e;
        }
    }
    for (int i : {5, 0, 1, 2, 3, 4}) {
        int w = 0, h = 0;
        uint8_t* tex = UiTexture(i, &w, &h);
        uint32_t st = 0;
        void* r = tex ? TextureResource(tex, &st) : nullptr;
        if (r) {
            res.push_back(r);
            states.push_back(st);
            bases.push_back(std::wstring(stamp) + (i == 5 ? L"_hud" : L"_ui" + std::to_wstring(i)));
        }
    }
    std::vector<std::wstring> paths;
    std::vector<const wchar_t*> ptrs;
    for (auto& b : bases) paths.push_back(dir + b + L".bmp");
    for (auto& p : paths) ptrs.push_back(p.c_str());
    const bool queued = !res.empty() && CaptureRequest(res.data(), states.data(), ptrs.data(), static_cast<int>(res.size()));
    char state[256] = "";
    if (uint8_t* v = ActiveView()) {
        const float* c = reinterpret_cast<const float*>(v);
        const float* proj = reinterpret_cast<const float*>(v + 0x1c0);
        sprintf_s(state, "cam %.2f %.2f %.2f fwd %.3f %.3f %.3f lens %.2f vendor %d paused %d pitch %d", c[12], c[13], c[14], c[8], c[9], c[10], proj[5] != 0 ? 2.0 * atan(1.0 / fabs(proj[5])) * 57.29577951 : 0.0, g_vendorNow.load() ? 1 : 0, *reinterpret_cast<int*>(g_base + kPausedFlag), g_levelCamera.load());
    }
    Log("SNAP %d queued %d: %s %s", n, static_cast<int>(res.size()), queued ? "yes" : "no", state);
    // Written by the capture code as files appear. Each becomes a JPEG, listed in the index the lab reads.
    FILE* index = nullptr;
    for (size_t i = 0; i < paths.size(); ++i) {
        for (int w = 0; w < 200 && GetFileAttributesW(paths[i].c_str()) == INVALID_FILE_ATTRIBUTES; ++w) Sleep(100);
        Sleep(400);
        const std::wstring jpg = dir + bases[i] + L".jpg";
        if (BmpToJpeg(paths[i], jpg)) {
            DeleteFileW(paths[i].c_str());
            if (_wfopen_s(&index, (dir + L"index.txt").c_str(), L"a") == 0 && index) {
                fwprintf(index, L"%ls.jpg\n", bases[i].c_str());
                fclose(index);
            }
        }
    }
    Log("SNAP %d done", n);
    CoUninitialize();
    return 0;
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
            if (st.lt > 40 || st.rt > 40) g_lastTrigger = GetTickCount64();
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
            if (g_menuRow == 0 || g_menuRow == 2 || g_menuRow >= 5) MenuChange(0);
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
// ---- GPU time of each DLSS call, by timestamps either side of it on the game's own command list. Lab probe. ----
static const UINT kDlssSlots = 3, kDlssRing = 16;
static ID3D12QueryHeap* g_dlssHeap;
static ID3D12Resource* g_dlssRead;
static UINT64 g_dlssFreq;
static std::atomic<uint32_t> g_dlssFrame[kDlssSlots];
static std::atomic<uint64_t> g_dlssTicks[kDlssSlots], g_dlssCount[kDlssSlots];
static std::atomic<uint64_t> g_eyeSpanTicks{0}, g_eyeSpanCount{0};
static std::atomic<uint64_t> g_splitBetweenTicks{0}, g_splitBetweenCount{0}, g_rightEndCount{0};
static UINT64 g_rightEnds[32];
static bool DlssTimerBegin(ID3D12GraphicsCommandList* list, int vp, UINT* index) {
    if (!g_labProbes.load() || vp < 0 || vp >= static_cast<int>(kDlssSlots) || !list) return false;
    if (!g_dlssHeap) {
        ID3D12Device* dev = nullptr;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev)))) return false;
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = kDlssSlots * kDlssRing * 2;
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
        auto* q = static_cast<ID3D12CommandQueue*>(CaptureQueue());
        const bool ok = q && SUCCEEDED(q->GetTimestampFrequency(&g_dlssFreq)) && SUCCEEDED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_dlssHeap))) && SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_dlssRead)));
        dev->Release();
        if (!ok) return false;
    }
    const uint32_t f = g_dlssFrame[vp].fetch_add(1);
    *index = (vp * kDlssRing + f % kDlssRing) * 2;
    // What an earlier use of this slot measured has finished by now: the ring is many frames long.
    if (f >= kDlssRing) {
        const UINT old = *index;
        D3D12_RANGE range{old * 8, (old + 2) * 8};
        UINT64* m = nullptr;
        if (SUCCEEDED(g_dlssRead->Map(0, &range, reinterpret_cast<void**>(&m)))) {
            const UINT64 a = m[old], b = m[old + 1];
            D3D12_RANGE none{0, 0};
            g_dlssRead->Unmap(0, &none);
            if (b > a) {
                g_dlssTicks[vp] += b - a;
                ++g_dlssCount[vp];
                static UINT64 leftEnd = 0;
                // Split of an eye. Left's DLSS end to right's DLSS start: left's work after DLSS plus right's scene.
                // Right's DLSS end to the mod's copies: right's work after DLSS, at output size.
                if (vp == 2) {
                    if (leftEnd && a > leftEnd) {
                        g_splitBetweenTicks += a - leftEnd;
                        ++g_splitBetweenCount;
                    }
                    g_rightEnds[g_rightEndCount.fetch_add(1) % 32] = b;
                }
                if (vp == 1) leftEnd = b;
                if (vp == 2 && leftEnd && b > leftEnd) {
                    g_eyeSpanTicks += b - leftEnd;
                    ++g_eyeSpanCount;
                    leftEnd = 0;
                }
            }
        }
    }
    list->EndQuery(g_dlssHeap, D3D12_QUERY_TYPE_TIMESTAMP, *index);
    return true;
}
static void DlssTimerEnd(ID3D12GraphicsCommandList* list, UINT index) {
    list->EndQuery(g_dlssHeap, D3D12_QUERY_TYPE_TIMESTAMP, index + 1);
    list->ResolveQueryData(g_dlssHeap, D3D12_QUERY_TYPE_TIMESTAMP, index, 2, g_dlssRead, static_cast<UINT64>(index) * 8);
}
// ---- Streamline's resource tags: where each eye's depth and motion vectors are. Layout from sl_core_types.h. ----
struct SlBase {
    void* next;
    uint8_t type[16];
    size_t version;
};
struct SlResource : SlBase {
    uint32_t kind;
    void* native;
    void* memory;
    void* view;
    uint32_t state, width, height, nativeFormat;
};
struct SlExtent {
    uint32_t top, left, width, height;
};
struct SlTag : SlBase {
    SlResource* resource;
    uint32_t bufferType;
    uint32_t lifecycle;
    SlExtent extent;
};
static std::atomic<int> g_slEvalLogLeft{0};
static int __cdecl HookSlEvaluate(uint32_t feature, const void* frame, const void** inputs, uint32_t n, void* cmd) {
    ++g_slCount[0];
    const int vp = g_slViewport.load();
    if (g_slEvalLogLeft.load() > 0 && g_slEvalLogLeft.fetch_sub(1) > 0) {
        for (uint32_t i = 0; i < n; ++i) {
            const SlBase* b = static_cast<const SlBase*>(inputs[i]);
            uint32_t g = 0;
            memcpy(&g, b->type, 4);
            Log("SLEVAL viewport %d feature %u input %u/%u guid %08x version %zu", vp, feature, i, n, g, b->version);
            if (g == 0x4c6a5aad) {
                const SlTag* t = reinterpret_cast<const SlTag*>(b);
                D3D12_RESOURCE_DESC d{};
                if (t->resource && t->resource->native) d = static_cast<ID3D12Resource*>(t->resource->native)->GetDesc();
                Log("SLEVAL  tag type %u native %p state %#x lifecycle %u extent %u %u %ux%u, resource %llux%u format %u flags %#x", t->bufferType, t->resource ? t->resource->native : nullptr, t->resource ? t->resource->state : 0, t->lifecycle, t->extent.left, t->extent.top, t->extent.width, t->extent.height, static_cast<unsigned long long>(d.Width), d.Height, static_cast<unsigned>(d.Format), static_cast<unsigned>(d.Flags));
            }
        }
    }
    auto* timed = static_cast<ID3D12GraphicsCommandList*>(cmd);
    UINT timerIndex = 0;
    const bool timing = feature == 0 && DlssTimerBegin(timed, vp, &timerIndex);
    struct End {
        bool on;
        ID3D12GraphicsCommandList* l;
        UINT i;
        ~End() {
            if (on) DlssTimerEnd(l, i);
        }
    } end{timing, timed, timerIndex};
    if ((vp == 1 || vp == 2) && feature == 0) {
        for (uint32_t i = 0; i < n; ++i) {
            const SlBase* b = static_cast<const SlBase*>(inputs[i]);
            uint32_t g = 0;
            memcpy(&g, b->type, 4);
            if (g == 0x4c6a5aad) {
                const SlTag* t = reinterpret_cast<const SlTag*>(b);
                if (t->bufferType == 0 && t->resource) XrRebuildCopyDepth(vp - 1, cmd, t->resource->native);
            }
        }
    }
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
struct EyeTagged {
    std::atomic<ID3D12Resource*> resource{nullptr};
    std::atomic<uint32_t> state{0};
    SlExtent extent{};
};
static EyeTagged g_eyeDepthTag[3], g_eyeMotionTag[3];
using SlSetTagFn = int(__cdecl*)(const void*, const SlTag*, uint32_t, void*);
static SlSetTagFn g_origSlSetTag;
static std::atomic<int> g_slTagLogLeft{0};
static int __cdecl HookSlSetTag(const void* viewport, const SlTag* tags, uint32_t n, void* cmd) {
    const int vp = g_slViewport.load();
    for (uint32_t i = 0; i < n && vp >= 0 && vp < 3; ++i) {
        const SlTag& t = tags[i];
        if (!t.resource) continue;
        EyeTagged* into = t.bufferType == 0 ? &g_eyeDepthTag[vp] : t.bufferType == 1 ? &g_eyeMotionTag[vp] : nullptr;
        if (into) {
            into->resource = static_cast<ID3D12Resource*>(t.resource->native);
            into->state = t.resource->state;
            into->extent = t.extent;
        }
        if (g_slTagLogLeft.load() > 0 && g_slTagLogLeft.fetch_sub(1) > 0) {
            D3D12_RESOURCE_DESC d{};
            if (t.resource->native) d = static_cast<ID3D12Resource*>(t.resource->native)->GetDesc();
            Log("SLTAG viewport %d type %u native %p state %#x lifecycle %u, sl size %ux%u format %u, extent %u %u %ux%u, resource %llux%u format %u flags %#x", vp, t.bufferType, t.resource->native, t.resource->state, t.lifecycle, t.resource->width, t.resource->height, t.resource->nativeFormat, t.extent.left, t.extent.top, t.extent.width, t.extent.height, static_cast<unsigned long long>(d.Width), d.Height, static_cast<unsigned>(d.Format), static_cast<unsigned>(d.Flags));
        }
    }
    if (g_slViewport.load() == 0 || !viewport) return g_origSlSetTag(viewport, tags, n, cmd);
    ViewportCopy copy(viewport);
    return g_origSlSetTag(copy.bytes, tags, n, cmd);
}
static std::atomic<int> g_slConstLogLeft{0};
static int __cdecl HookSlSetConst(const void* values, const void* frame, const void* viewport) {
    ++g_slCount[2];
    if (g_slConstLogLeft.load() > 0 && g_slConstLogLeft.fetch_sub(1) > 0 && values) {
        const float* f = reinterpret_cast<const float*>(static_cast<const uint8_t*>(values) + sizeof(SlBase));
        const uint32_t* u = reinterpret_cast<const uint32_t*>(f);
        // After five 4x4 matrices: jitter, mvec scale, pinhole, position, up, right, fwd, near, far, fov, aspect, invalid, booleans.
        const float* a = f + 80;
        Log("SLCONST viewport %d viewToClip %.4f %.4f %.4f %.4f / %.4f %.4f / %.4f %.4f %.4f %.4f", g_slViewport.load(), f[0], f[5], f[10], f[11], f[14], f[15], f[8], f[9], f[2], f[3]);
        Log("SLCONST jitter %.3f %.3f mvec scale %.5f %.5f pos %.3f %.3f %.3f near %.3f far %.1f fov %.3f aspect %.3f bools %u %u %u %u", a[0], a[1], a[2], a[3], a[6], a[7], a[8], a[18], a[19], a[20], a[21], u[80 + 23], u[80 + 24], u[80 + 25], u[80 + 26]);
    }
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
                          {"slSetTag", reinterpret_cast<void*>(&HookSlSetTag), reinterpret_cast<void**>(&g_origSlSetTag)},
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
static std::atomic<bool> g_smallFlat{true};
static void OverrideSetting(const wchar_t* name, void* data) {
    if (!name || !data) return;
    if (g_skipLauncher && !_wcsicmp(name, L"ShowLauncher")) {
        Log("REG ShowLauncher was %d, the game is told 0", *static_cast<int*>(data));
        *static_cast<int*>(data) = 0;
        return;
    }
    {
        // [Force] section of the ini: any DWORD game setting, by its registry name, for tests and for the VR effects choice.
        const int forced = static_cast<int>(GetPrivateProfileIntW(L"Force", name, -1, (g_dir + L"RiftApartVR.ini").c_str()));
        if (forced >= 0) {
            if (*static_cast<int*>(data) != forced) Log("REG %ls was %d, forced %d", name, *static_cast<int*>(data), forced);
            *static_cast<int*>(data) = forced;
            return;
        }
    }
    if (g_smallFlat) {
        // The window sets the HUD's layout and the size of the game's own view, which only draws the HUD now.
        // 16 by 9 keeps HUD pieces in reach whatever the monitor. The monitor shows an eye.
        int small = -1;
        if (!_wcsicmp(name, L"Fullscreen") || !_wcsicmp(name, L"ExclusiveFullscreen") || !_wcsicmp(name, L"WindowMaximized")) small = 0;
        else if (!_wcsicmp(name, L"WindowWidth") || !_wcsicmp(name, L"UserWindowWidth")) small = 2560;
        else if (!_wcsicmp(name, L"WindowHeight") || !_wcsicmp(name, L"UserWindowHeight")) small = 1440;
        if (small >= 0) {
            if (*static_cast<int*>(data) != small) Log("REG %ls was %d, the game is told %d", name, *static_cast<int*>(data), small);
            *static_cast<int*>(data) = small;
            return;
        }
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
// Slot 5 is not a Coherent view: the game's own composite of its whole UI, the overlay the tone map blends in.
static uint8_t* OverlayTexture(int* w, int* h) {
    uint8_t* t = g_base + 0x67334c0;
    if (*reinterpret_cast<int*>(t + 0x400) == 0) return nullptr;
    uint8_t* tex = t + 0x230;
    if (w) SafeCopy(w, tex + 0x1d0, 4);
    if (h) SafeCopy(h, tex + 0x1d4, 4);
    return tex;
}
static uint8_t* UiTexture(int i, int* w, int* h) {
    if (i == 5) return OverlayTexture(w, h);
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
    // A render target struct, such as the overlay, keeps its wrapper at +0x88 (0x11df5a0 reads it there).
    const size_t wrapAt = tex == g_base + 0x67334c0 + 0x230 ? 0x88 : 0x40;
    if (!SafeCopy(&wrap, tex + wrapAt, 8) || !wrap || !SafeCopy(&rec, wrap + 0x38, 8) || !rec || !SafeCopy(&res, rec, 8) || !res) return nullptr;
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
    else if (sscanf(c.c_str(), "level %u", &g_w) == 1) g_levelCamera = static_cast<int>(g_w);
    else if (sscanf(c.c_str(), "lensfix %u", &g_w) == 1) g_lensFix = g_w != 0;
    else if (sscanf(c.c_str(), "eyenoui %u", &g_w) == 1) g_eyeNoUi = g_w != 0;
    else if (sscanf(c.c_str(), "eyeui %u", &g_w) == 1) g_eyeDropUi = g_w != 0;
    else if (sscanf(c.c_str(), "fg %u %u %u", &g_w, &g_h, &g_k) == 3) {
        g_fgMode = static_cast<int>(g_w) % 3;
        g_fgPreset = static_cast<int>(g_h) % 3;
        g_fgScale = static_cast<int>(g_k) % 4;
        OfxrApply();
    }
    else if (sscanf(c.c_str(), "fgtype %u", &g_w) == 1) {
        g_fgType = static_cast<int>(g_w) % 4;
        OfxrApply();
    }
    else if (sscanf(c.c_str(), "fgpace %u", &g_w) == 1) {
        g_fgDirector = g_w != 0;
        OfxrPacingUpdate();
    }
    else if (sscanf(c.c_str(), "alternate %u", &g_w) == 1) {
        g_alternateSetting = g_w != 0;
        g_rebuildSetting = g_w == 2;
        XrRebuildSetMode(g_w == 2 ? 1 : 0);
    }
    else if (sscanf(c.c_str(), "rbtest %u", &g_w) == 1) {
        XrRebuildSetMode(g_w ? 1 : (g_rebuildSetting ? 1 : 0));
        XrRebuildTest(g_w != 0);
    }
    else if (sscanf(c.c_str(), "rbstage %u", &g_w) == 1) XrRebuildStage(static_cast<int>(g_w));
    else if (sscanf(c.c_str(), "rbshot %63s", g_text) == 1) g_rbShotName = g_text;
    else if (sscanf(c.c_str(), "sltag %u", &g_w) == 1) { g_slTagLogLeft = static_cast<int>(g_w); g_slEvalLogLeft = static_cast<int>(g_w); }
    else if (sscanf(c.c_str(), "eyedefer %u", &g_w) == 1) g_eyeDefer = g_w != 0;
    else if (sscanf(c.c_str(), "eyeshare %u", &g_w) == 1) g_eyeShare = g_w != 0;
    else if (c == "viewinfo") {
        uint8_t* const vs[3] = {ActiveView(), g_eyeA.load(), g_eyeB.load()};
        for (uint8_t* v : vs) {
            if (!v) continue;
            Log("VIEWINFO %p occlusion %p lens lod %.3f scale %.3f share flag %u flags %#x", v, *reinterpret_cast<void**>(v + 0x1bf0), *reinterpret_cast<float*>(v + 0x1d14), *reinterpret_cast<float*>(v + 0x1d18), *(v + 0x11da), *reinterpret_cast<uint32_t*>(v + 0x16ccc));
        }
    }
    else if (c == "dredtest") {
        // Lab: removes the device on purpose, to prove the report works.
        ID3D12Device5* d5 = nullptr;
        if (CaptureDevice() && SUCCEEDED(static_cast<ID3D12Device*>(CaptureDevice())->QueryInterface(IID_PPV_ARGS(&d5))) && d5) d5->RemoveDevice();
    }
    else if (sscanf(c.c_str(), "maineveryframe %u", &g_w) == 1) g_mainEveryOverride = g_w != 0;
    else if (sscanf(c.c_str(), "overlap %u", &g_w) == 1) XrSetOverlap(g_w != 0);
    else if (c == "fgstate") OfxrLogState("now");
    else if (sscanf(c.c_str(), "mirror %u", &g_w) == 1) g_mirrorEye = g_w != 0;
    else if (sscanf(c.c_str(), "eyeskip %u", &g_w) == 1) {
        // Lab: stop drawing an eye, to see what it costs. 1 right, 2 both, 0 neither.
        std::lock_guard<std::mutex> l(g_eyesLock);
        if (g_eyes.size() >= 2) {
            uint32_t& fa = *reinterpret_cast<uint32_t*>(g_eyes[0] + 0x16ccc);
            uint32_t& fb = *reinterpret_cast<uint32_t*>(g_eyes[1] + 0x16ccc);
            fb = g_w >= 1 ? (fb | 1u) : (fb & ~1u);
            fa = g_w >= 2 ? (fa | 1u) : (fa & ~1u);
        }
    }
    else if (c == "hudshot") g_wantHudShot = 1;
    else if (c == "snapshot") CreateThread(nullptr, 0, SnapshotThread, nullptr, 0, nullptr);
    else if (sscanf(c.c_str(), "probes %u", &g_w) == 1) g_labProbes = g_w != 0;
    else if (sscanf(c.c_str(), "hudafter %63s", g_text) == 1) {
        static std::wstring p;
        p = g_dir + L"vrlab_shots\\" + std::wstring(g_text, g_text + strlen(g_text)) + L".bmp";
        XrDumpHudAfter(p.c_str());
    }
    else if (sscanf(c.c_str(), "skipmain %u", &g_w) == 1) g_skipMain = static_cast<int>(g_w);
    else if (sscanf(c.c_str(), "vendortest %u", &g_w) == 1) g_forceVendor = g_w != 0;
    else if (sscanf(c.c_str(), "aimdot %u", &g_w) == 1) g_aimDot = static_cast<int>(g_w);
    else if (c == "aiminfo") {
        uint8_t* m = ActiveView();
        const float* v = m ? reinterpret_cast<const float*>(m) : nullptr;
        if (v) Log("AIM room %.4f %.4f %.4f camera forward %.4f %.4f %.4f", g_aimRoom[0], g_aimRoom[1], g_aimRoom[2], v[8], v[9], v[10]);
    }
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

// ---- read-only probe: which methods of a class run, and when. Swaps vtable slots for a stub that counts and jumps on. ----
struct VtProbe {
    const char* name;
    size_t rva;
    std::vector<volatile uint64_t*> counters;
    std::vector<uint64_t> last;
};
static std::vector<VtProbe> g_vtProbes;
static void VtProbeAdd(const char* name, size_t rva) {
    VtProbe probe{name, rva, {}, {}};
    void** vt = reinterpret_cast<void**>(g_base + rva);
    uint8_t* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!page) return;
    uint8_t* out = page;
    for (int i = 0; i < 64; ++i) {
        uint8_t* target = static_cast<uint8_t*>(vt[i]);
        if (target < g_base + 0x1000 || target >= g_base + 0x2e36000) break;  // past the end of the vtable: not in .text
        // Data: counter, then original. Code: lock inc qword [rip+counter]; jmp [rip+original].
        auto* counter = reinterpret_cast<volatile uint64_t*>(out);
        *reinterpret_cast<uint64_t*>(out) = 0;
        *reinterpret_cast<void**>(out + 8) = target;
        uint8_t* code = out + 16;
        const uint8_t inc[] = {0xf0, 0x48, 0xff, 0x05};
        memcpy(code, inc, 4);
        *reinterpret_cast<int32_t*>(code + 4) = static_cast<int32_t>(out - (code + 8));
        code[8] = 0xff;
        code[9] = 0x25;
        *reinterpret_cast<int32_t*>(code + 10) = static_cast<int32_t>((out + 8) - (code + 14));
        out += 32;
        DWORD old = 0;
        VirtualProtect(&vt[i], 8, PAGE_READWRITE, &old);
        vt[i] = code;
        VirtualProtect(&vt[i], 8, old, &old);
        probe.counters.push_back(counter);
        probe.last.push_back(0);
    }
    Log("VTPROBE %s rva %zx: %zu methods", name, rva, probe.counters.size());
    g_vtProbes.push_back(std::move(probe));
}
static void VtProbeReport() {
    for (VtProbe& p : g_vtProbes) {
        char line[512];
        int n = 0;
        for (size_t i = 0; i < p.counters.size() && n < 480; ++i) {
            const uint64_t c = *p.counters[i];
            if (c != p.last[i]) {
                n += sprintf_s(line + n, sizeof(line) - n, " %zu:%llu", i, static_cast<unsigned long long>(c - p.last[i]));
                p.last[i] = c;
            }
        }
        if (n) Log("VTC %s%s", p.name, line);
    }
}

static DWORD WINAPI ProbeThread(void*) {
    {
        const std::wstring early = g_dir + L"RiftApartVR.ini";
        // The game's own DLSS menu is the only control. Old EyeDLSS and EyeDLSSScene values in the ini are ignored: a size far
        // from the game's mode stops DLSS running in the eyes, with no error.
        g_regForce = !g_eyeDlss && GetPrivateProfileIntW(L"VR", L"ForceGameAA", 1, early.c_str()) != 0;
        g_skipLauncher = GetPrivateProfileIntW(L"VR", L"SkipLauncher", 1, early.c_str()) != 0;
        g_smallFlat = GetPrivateProfileIntW(L"VR", L"SmallFlatView", 1, early.c_str()) != 0;
        g_skipMain = static_cast<int>(GetPrivateProfileIntW(L"VR", L"HudEveryFrames", 4, early.c_str()));
        g_cutsceneCamera = static_cast<int>(GetPrivateProfileIntW(L"VR", L"CutsceneCamera", 0, early.c_str())) % 3;
        CreateThread(nullptr, 0, ThreadSampler, nullptr, 0, nullptr);
        if (GetPrivateProfileIntW(L"VR", L"DeviceDiagnostics", 1, early.c_str()) != 0) {
            DredEnable();
            CreateThread(nullptr, 0, DredWatch, nullptr, 0, nullptr);
        }
        MH_Initialize();
        InstallRegHooks();
    }
    FindSections();
    FindActiveView();
    InstallHooks();
    const std::wstring ini = g_dir + L"RiftApartVR.ini";
    g_iniPath = ini;
    g_skipLauncherSetting = GetPrivateProfileIntW(L"VR", L"SkipLauncher", 1, ini.c_str()) != 0;
    g_vendorOn = GetPrivateProfileIntW(L"VR", L"VendorScreen", 1, ini.c_str()) != 0;
    g_fgMode = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"VR", L"FrameGen", 0, ini.c_str())), 0, 2);
    g_fgPreset = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"VR", L"FrameGenQuality", 1, ini.c_str())), 0, 2);
    g_fgScale = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"VR", L"FrameGenDetail", 2, ini.c_str())), 0, 3);
    g_fgType = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"VR", L"FrameGenType", 0, ini.c_str())), 0, 3);
    g_fgTypeAtLaunch = g_fgType;
    g_fgDirector = GetPrivateProfileIntW(L"VR", L"FrameGenDirector", 0, ini.c_str()) != 0;
    g_overlapSetting = GetPrivateProfileIntW(L"VR", L"FrameOverlap", 0, ini.c_str()) != 0;
    XrSetOverlap(g_overlapSetting);
    {
        const UINT alt = GetPrivateProfileIntW(L"VR", L"AlternateEye", 0, ini.c_str());
        g_alternateSetting = alt != 0;
        g_rebuildSetting = alt == 2;
        XrRebuildSetMode(alt == 2 ? 1 : 0);
    }
    g_aimDot = static_cast<int>(GetPrivateProfileIntW(L"VR", L"AimDot", 1, ini.c_str()));
    g_labProbes = GetPrivateProfileIntW(L"VR", L"LabProbes", 1, ini.c_str()) != 0;
    g_skipLauncher = g_skipLauncherSetting;
    InstallSlProbe();
    if (GetPrivateProfileIntW(L"VR", L"VtProbe", 0, ini.c_str())) {
        VtProbeAdd("CameraCinematic", 0x2ee2ae0);
        VtProbeAdd("GameCamera", 0x2ee3258);
        VtProbeAdd("LookCamera", 0x2ee36b0);
        VtProbeAdd("StrafeCamera", 0x2ee3fa8);
        VtProbeAdd("RelativeAnimCamera", 0x45acda0);
        VtProbeAdd("PlacedCamera", 0x45ac578);
        VtProbeAdd("FixedCamera", 0x45ac600);
        VtProbeAdd("PanCamera", 0x45ac698);
        VtProbeAdd("SplineCameraFly", 0x45ace40);
        VtProbeAdd("DualSplineCameraFly", 0x45abb80);
        VtProbeAdd("CollisionPredictive", 0x2ee60c8);
    }
    InstallPadHooks();
    CreateThread(nullptr, 0, PadThread, nullptr, 0, nullptr);
    const bool autoStart = GetPrivateProfileIntW(L"VR", L"AutoStart", 1, ini.c_str()) != 0;
    const int resolution = GetPrivateProfileIntW(L"VR", L"Resolution", 100, ini.c_str());
    g_worldScale = GetPrivateProfileIntW(L"VR", L"WorldScalePercent", 300, ini.c_str()) / 100.0f;
    g_levelCamera = static_cast<int>(GetPrivateProfileIntW(L"VR", L"LevelCamera", 2, ini.c_str()));
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
            static ULONGLONG lastVt = 0;
            if (!g_vtProbes.empty() && now - lastVt >= 1000) {
                lastVt = now;
                VtProbeReport();
            }
        }
        {
            // Gravity boots: does the game's camera lean? Logged once a second when its right row leaves the level.
            static ULONGLONG lastRow = 0;
            if (g_labProbes && now - lastRow >= 1000) {
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
        {
            static ULONGLONG lastPerf = 0;
            if (g_labProbes && now - lastPerf >= 5000) {
                lastPerf = now;
                const uint64_t n = g_perfFrames.exchange(0);
                if (n) {
                    const double f = static_cast<double>(n) * 1000.0;
                    if (g_dlssFreq) {
                        double ms[kDlssSlots] = {};
                        for (UINT v = 0; v < kDlssSlots; ++v) {
                            const uint64_t c = g_dlssCount[v].exchange(0), t = g_dlssTicks[v].exchange(0);
                            ms[v] = c ? static_cast<double>(t) / static_cast<double>(c) * 1000.0 / static_cast<double>(g_dlssFreq) : 0;
                        }
                        double copyMs = 0, betweenMs = 0;
                        XrCopyTimes(&copyMs, &betweenMs);
                        const uint64_t spanCount = g_eyeSpanCount.exchange(0), spanTicks = g_eyeSpanTicks.exchange(0);
                        const double spanMs = spanCount && g_dlssFreq ? static_cast<double>(spanTicks) / static_cast<double>(spanCount) * 1000.0 / static_cast<double>(g_dlssFreq) : 0;
                        {
                            // Pair each copy's start with the last right eye DLSS end before it.
                            UINT64 begins[32];
                            const int nb = XrCopyBegins(begins, 32);
                            double postSum = 0;
                            int postN = 0;
                            for (int bi = 0; bi < nb; ++bi) {
                                UINT64 best = 0;
                                for (const UINT64 re : g_rightEnds) if (re < begins[bi] && re > best) best = re;
                                if (best && begins[bi] - best < g_dlssFreq / 10) {
                                    postSum += static_cast<double>(begins[bi] - best);
                                    ++postN;
                                }
                            }
                            const double post = postN ? postSum / postN * 1000.0 / static_cast<double>(g_dlssFreq) : 0;
                            const uint64_t bc = g_splitBetweenCount.exchange(0), bt = g_splitBetweenTicks.exchange(0);
                            const double between = bc ? static_cast<double>(bt) / static_cast<double>(bc) * 1000.0 / static_cast<double>(g_dlssFreq) : 0;
                            Log("SPLIT ms: after DLSS, at output size %.2f; before right's DLSS, its scene plus left's after DLSS %.2f; so scene %.2f; DLSS %.2f", post, between, between - post, ms[2]);
                        }
                        Log("PERF GPU ms: DLSS game's view %.2f, left %.2f, right %.2f; right eye whole %.2f; mod's copies %.2f; between frame ends %.2f", ms[0], ms[1], ms[2], spanMs, copyMs, betweenMs);
                    }
                    LogMemory();
                    LogThreads();
                    Log("PERF frames %llu, ms per frame: whole %.2f, pacing wait %.2f, all offscreen %.2f, eye submits %.2f, main submit %.2f", static_cast<unsigned long long>(n), g_usFrame.exchange(0) / f, g_usPace.exchange(0) / f, g_usOffscreen.exchange(0) / f, g_usEyeSubmit.exchange(0) / f, g_usMainSubmit.exchange(0) / f);
                }
            }
        }
        if (g_labProbes && now - lastView >= 5000) {
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
    KeepLogHistory();
    g_log = _wfopen((g_dir + L"RiftApartVR.log").c_str(), L"w");
    g_base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    SYSTEMTIME st;
    GetLocalTime(&st);
    Log("RiftApartVR build %d loaded at %02d:%02d:%02d.%03d, real winhttp %p", BUILD_NUMBER, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, real);

    if (HANDLE t = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr)) CloseHandle(t);
    return TRUE;
}
