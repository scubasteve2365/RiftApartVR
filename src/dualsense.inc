// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence.
// PS5 DualSense for the mod menu (1.2.1). The game reads the DualSense itself through Sony's pad library over HID, not XInput,
// so the mod's XInput and Windows.Gaming.Input hooks never see it.
// Reading: the mod opens the pad's HID device itself (shared, read only) on its own thread; Windows gives every open handle a
// copy of each input report, so this does not take anything from the game. Report layouts (public, as in Linux hid-playstation):
//   USB report 0x01, 64 bytes: [1] LX [2] LY [3] RX [4] RY [5] L2 [6] R2 [8] hat (low 4 bits) and Square 0x10 Cross 0x20
//     Circle 0x40 Triangle 0x80, [9] L1 0x01 R1 0x02 L2 0x04 R2 0x08 Create 0x10 Options 0x20 L3 0x40 R3 0x80, [10] PS 0x01
//     Touchpad 0x02 Mute 0x04.
//   Bluetooth report 0x31: the same fields one byte later. Bluetooth report 0x01 before the full mode is switched on (10 bytes):
//     [1] LX [2] LY [3] RX [4] RY [5] hat and face buttons [6] shoulders and sticks [7] PS [8] L2 [9] R2.
// Blocking while the menu is open: the game's own reads of the pad (CreateFileW on a DualSense path, then ReadFile, and for
// overlapped reads GetOverlappedResult) get a neutral report: sticks centred, triggers and buttons released. Key-ups are not an
// issue here: every report carries the whole state, so the game sees everything released and picks up again after.

#include <setupapi.h>
#include <hidsdi.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

struct DsState {
    WORD buttons;  // XInput bits: up 1 down 2 left 4 right 8, Options 0x10 (Start), Create 0x20 (Back), L3 0x40, R3 0x80,
                   // L1 0x100, R1 0x200, Cross 0x1000 (A), Circle 0x2000 (B), Square 0x4000 (X), Triangle 0x8000 (Y)
    uint8_t lt, rt;
    short lx, ly;
    ULONGLONG at;
};
static std::atomic<uint64_t> g_dsPacked{0};  // buttons, triggers and sticks packed so one load is consistent
static std::atomic<ULONGLONG> g_dsAt{0};
static std::atomic<bool> g_dsConnected{false}, g_menuDs{false};
static std::atomic<int> g_dsBus{0};  // 1 USB, 2 Bluetooth
static std::atomic<uint32_t> g_dsGameReads{0}, g_dsGamePending{0}, g_dsGameBlanked{0}, g_dsGameHandles{0};

static uint64_t DsPack(WORD b, uint8_t lt, uint8_t rt, int8_t lx, int8_t ly) {
    return static_cast<uint64_t>(b) | static_cast<uint64_t>(lt) << 16 | static_cast<uint64_t>(rt) << 24 | static_cast<uint64_t>(static_cast<uint8_t>(lx)) << 32 |
           static_cast<uint64_t>(static_cast<uint8_t>(ly)) << 40;
}
static bool DsRead(DsState* out) {
    const ULONGLONG at = g_dsAt.load();
    if (!g_dsConnected.load() || !at || GetTickCount64() - at > 500) return false;
    const uint64_t p = g_dsPacked.load();
    out->buttons = static_cast<WORD>(p & 0xffff);
    out->lt = static_cast<uint8_t>(p >> 16);
    out->rt = static_cast<uint8_t>(p >> 24);
    out->lx = static_cast<short>(static_cast<int8_t>(p >> 32) * 256);
    out->ly = static_cast<short>(static_cast<int8_t>(p >> 40) * 256);
    out->at = at;
    return true;
}

// Decodes one input report. Returns false for reports it does not know. off is where LX sits.
static bool DsDecode(const uint8_t* r, DWORD n, WORD* buttons, uint8_t* lt, uint8_t* rt, int8_t* lx, int8_t* ly) {
    int off = 0;
    bool simple = false;
    if (r[0] == 0x01 && n >= 64) off = 1;
    else if (r[0] == 0x31 && n >= 64) off = 2;
    else if (r[0] == 0x01 && n >= 10) {
        off = 1;
        simple = true;
    } else {
        return false;
    }
    const uint8_t b0 = simple ? r[5] : r[off + 7], b1 = simple ? r[6] : r[off + 8];
    *lt = simple ? r[8] : r[off + 4];
    *rt = simple ? r[9] : r[off + 5];
    *lx = static_cast<int8_t>(r[off] - 128);
    *ly = static_cast<int8_t>(127 - r[off + 1]);  // up is positive, as XInput
    WORD b = 0;
    switch (b0 & 0x0f) {
        case 0: b |= 1; break;
        case 1: b |= 1 | 8; break;
        case 2: b |= 8; break;
        case 3: b |= 2 | 8; break;
        case 4: b |= 2; break;
        case 5: b |= 2 | 4; break;
        case 6: b |= 4; break;
        case 7: b |= 1 | 4; break;
        default: break;
    }
    if (b0 & 0x10) b |= 0x4000;
    if (b0 & 0x20) b |= 0x1000;
    if (b0 & 0x40) b |= 0x2000;
    if (b0 & 0x80) b |= 0x8000;
    if (b1 & 0x01) b |= 0x100;
    if (b1 & 0x02) b |= 0x200;
    if (b1 & 0x10) b |= 0x20;
    if (b1 & 0x20) b |= 0x10;
    if (b1 & 0x40) b |= 0x40;
    if (b1 & 0x80) b |= 0x80;
    *buttons = b;
    return true;
}

// A neutral report in place: sticks centred, hat released, buttons and triggers zero. Motion, touch and battery left alone.
static void DsBlank(uint8_t* r, DWORD n) {
    int off = 0;
    bool simple = false;
    if (r[0] == 0x01 && n >= 64) off = 1;
    else if (r[0] == 0x31 && n >= 64) off = 2;
    else if (r[0] == 0x01 && n >= 10) {
        off = 1;
        simple = true;
    } else {
        return;
    }
    r[off] = r[off + 1] = r[off + 2] = r[off + 3] = 0x80;
    if (simple) {
        r[5] = 0x08;
        r[6] = 0;
        r[7] &= 0xfc;
        r[8] = r[9] = 0;
    } else {
        r[off + 4] = r[off + 5] = 0;
        r[off + 7] = 0x08;
        r[off + 8] = 0;
        r[off + 9] &= 0xf8;
    }
    ++g_dsGameBlanked;
}

static bool DsPath(const wchar_t* p) {
    if (!p) return false;
    std::wstring s(p);
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s.find(L"vid_054c") != std::wstring::npos && (s.find(L"pid_0ce6") != std::wstring::npos || s.find(L"pid_0df2") != std::wstring::npos);
}

// ---- the game's reads ----
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using GetOverlappedResultFn = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
static CreateFileWFn g_origCreateFileW;
static ReadFileFn g_origReadFile;
static GetOverlappedResultFn g_origGetOverlapped;
static std::mutex g_dsLock;
static std::set<HANDLE> g_dsHandles;
static std::map<LPOVERLAPPED, uint8_t*> g_dsPending;

static HANDLE WINAPI HookCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    const HANDLE h = g_origCreateFileW(path, access, share, sa, disp, flags, tmpl);
    if (h != INVALID_HANDLE_VALUE && DsPath(path)) {
        {
            std::lock_guard<std::mutex> l(g_dsLock);
            g_dsHandles.insert(h);
        }
        ++g_dsGameHandles;
        Log("DUALSENSE game opened %ls, access %#lx share %#lx flags %#lx, handle %p", path, access, share, flags, h);
    }
    return h;
}
static bool DsGameHandle(HANDLE h) {
    if (!g_dsGameHandles.load()) return false;
    std::lock_guard<std::mutex> l(g_dsLock);
    return g_dsHandles.count(h) != 0;
}
static BOOL WINAPI HookReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    const BOOL ok = g_origReadFile(h, buf, n, got, ov);
    if (!buf || !DsGameHandle(h)) return ok;
    ++g_dsGameReads;
    if (ok) {
        const DWORD m = got ? *got : n;
        if (PadBlocked() && m) DsBlank(static_cast<uint8_t*>(buf), m);
    } else if (ov && GetLastError() == ERROR_IO_PENDING) {
        ++g_dsGamePending;
        std::lock_guard<std::mutex> l(g_dsLock);
        g_dsPending[ov] = static_cast<uint8_t*>(buf);
        SetLastError(ERROR_IO_PENDING);
    }
    return ok;
}
static BOOL WINAPI HookGetOverlapped(HANDLE h, LPOVERLAPPED ov, LPDWORD got, BOOL wait) {
    const BOOL ok = g_origGetOverlapped(h, ov, got, wait);
    if (ok && ov && g_dsGameHandles.load()) {
        uint8_t* buf = nullptr;
        {
            std::lock_guard<std::mutex> l(g_dsLock);
            auto it = g_dsPending.find(ov);
            if (it != g_dsPending.end()) {
                buf = it->second;
                g_dsPending.erase(it);
            }
        }
        if (buf && PadBlocked() && got && *got) DsBlank(buf, *got);
    }
    return ok;
}

// ---- the mod's own read ----
static std::wstring DsFind() {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    SP_DEVICE_INTERFACE_DATA d{sizeof(d)};
    for (DWORD i = 0; found.empty() && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &d); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &d, nullptr, 0, &need, nullptr);
        std::vector<uint8_t> mem(need + 8);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(mem.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &d, det, need, nullptr, nullptr) && DsPath(det->DevicePath)) found = det->DevicePath;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static DWORD WINAPI DsThread(void*) {
    for (;;) {
        const std::wstring path = DsFind();
        if (path.empty()) {
            g_dsConnected = false;
            Sleep(2000);
            continue;
        }
        CreateFileWFn create = g_origCreateFileW ? g_origCreateFileW : &CreateFileW;
        ReadFileFn read = g_origReadFile ? g_origReadFile : &ReadFile;
        HANDLE h = create(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            static int told = 0;
            if (told++ < 3) Log("DUALSENSE found %ls but could not open it: %lu", path.c_str(), GetLastError());
            Sleep(3000);
            continue;
        }
        HIDP_CAPS caps{};
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetPreparsedData(h, &pp)) {
            HidP_GetCaps(pp, &caps);
            HidD_FreePreparsedData(pp);
        }
        g_dsBus = caps.InputReportByteLength >= 78 ? 2 : 1;
        Log("DUALSENSE mod reads %ls, input report %u bytes (%s)", path.c_str(), caps.InputReportByteLength, g_dsBus.load() == 2 ? "Bluetooth" : "USB");
        g_dsConnected = true;
        std::vector<uint8_t> r(std::max<USHORT>(caps.InputReportByteLength, 128));
        WORD lastButtons = 0xffff;
        int changes = 0, unknown = 0;
        uint8_t lastId = 0;
        DWORD lastLen = 0;
        for (;;) {
            DWORD n = 0;
            if (!read(h, r.data(), static_cast<DWORD>(r.size()), &n, nullptr)) {
                Log("DUALSENSE read ended: %lu", GetLastError());
                break;
            }
            if (r[0] != lastId || n != lastLen) {
                lastId = r[0];
                lastLen = n;
                Log("DUALSENSE report id %#x, %lu bytes", r[0], n);
            }
            WORD b = 0;
            uint8_t lt = 0, rt = 0;
            int8_t lx = 0, ly = 0;
            if (!DsDecode(r.data(), n, &b, &lt, &rt, &lx, &ly)) {
                if (unknown++ < 5) Log("DUALSENSE unknown report id %#x, %lu bytes", r[0], n);
                continue;
            }
            g_dsPacked = DsPack(b, lt, rt, lx, ly);
            g_dsAt = GetTickCount64();
            if (b != lastButtons && changes < 400) {
                ++changes;
                Log("PADDS buttons %#06x L2 %u R2 %u left stick %d %d", b, lt, rt, lx, ly);
                lastButtons = b;
            }
        }
        g_dsConnected = false;
        CloseHandle(h);
        Sleep(1000);
    }
}

static void InstallDualSense() {
    HMODULE k = GetModuleHandleW(L"kernelbase.dll");
    if (!k) k = GetModuleHandleW(L"kernel32.dll");
    void* c = reinterpret_cast<void*>(GetProcAddress(k, "CreateFileW"));
    void* r = reinterpret_cast<void*>(GetProcAddress(k, "ReadFile"));
    void* g = reinterpret_cast<void*>(GetProcAddress(k, "GetOverlappedResult"));
    const MH_STATUS a = c ? MH_CreateHook(c, reinterpret_cast<void*>(&HookCreateFileW), reinterpret_cast<void**>(&g_origCreateFileW)) : MH_ERROR_FUNCTION_NOT_FOUND;
    const MH_STATUS b = r ? MH_CreateHook(r, reinterpret_cast<void*>(&HookReadFile), reinterpret_cast<void**>(&g_origReadFile)) : MH_ERROR_FUNCTION_NOT_FOUND;
    const MH_STATUS d = g ? MH_CreateHook(g, reinterpret_cast<void*>(&HookGetOverlapped), reinterpret_cast<void**>(&g_origGetOverlapped)) : MH_ERROR_FUNCTION_NOT_FOUND;
    if (a == MH_OK) MH_EnableHook(c);
    if (b == MH_OK) MH_EnableHook(r);
    if (d == MH_OK) MH_EnableHook(g);
    Log("DUALSENSE hooks CreateFileW %s ReadFile %s GetOverlappedResult %s", MH_StatusToString(a), MH_StatusToString(b), MH_StatusToString(d));
    CreateThread(nullptr, 0, DsThread, nullptr, 0, nullptr);
}
