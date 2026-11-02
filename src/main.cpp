// RiftApartVR. Copyright (c) 2026 scubasteve2365. MIT licence.
//
// Build 1: read-only probe. A WINHTTP.dll proxy (the game imports it directly, so this loads before the game's own
// code runs). It logs what the engine is doing and changes nothing: no hooks, no writes to game memory.
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#define BUILD_NUMBER 1

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

static void Log(const char* fmt, ...) {
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

static void RunCommand(const std::string& c) {
    Log("CMD %s", c.c_str());
    if (c == "probe") LogView();
}

static DWORD WINAPI ProbeThread(void*) {
    FindSections();
    FindActiveView();
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
