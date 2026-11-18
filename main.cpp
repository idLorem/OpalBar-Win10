#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <string>
#include <vector>

struct Accent { int state; int flags; unsigned color; int anim; };
struct WcaData { int attr; void* data; SIZE_T size; };
using SetWcaFn = BOOL(WINAPI*)(HWND, WcaData*);
using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

struct Mode { const char* name; int state; unsigned color; };
const Mode kModes[] = {
    {"clear", 2, 0x00000000},
    {"blur", 3, 0x80000000},
    {"acrylic", 4, 0x99000000},
    {"color", 2, 0xA0C07030},
};
constexpr int kSecondsPerMode = 6;

FILE* g_log;
volatile LONG g_stop;
SetWcaFn g_set;

void Log(const char* fmt, ...) {
    SYSTEMTIME t; GetLocalTime(&t);
    char line[1024];
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, a);
    va_end(a);
    fprintf(stdout, "%s\n", line);
    if (g_log) { fprintf(g_log, "%s\n", line); fflush(g_log); }
}

std::string Narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

BOOL CALLBACK CollectTaskbars(HWND h, LPARAM lp) {
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
    return TRUE;
}

BOOL CALLBACK LogChild(HWND h, LPARAM) {
    wchar_t cls[128];
    GetClassNameW(h, cls, 128);
    RECT r; GetWindowRect(h, &r);
    Log("    child %p class=%s rect=%ld,%ld,%ld,%ld", (void*)h, Narrow(cls).c_str(), r.left, r.top, r.right, r.bottom);
    return TRUE;
}

std::vector<HWND> Taskbars() {
    std::vector<HWND> v;
    EnumWindows(CollectTaskbars, reinterpret_cast<LPARAM>(&v));
    return v;
}

void Describe(const std::vector<HWND>& bars) {
    Log("taskbar windows found: %zu", bars.size());
    for (HWND h : bars) {
        wchar_t cls[64]; GetClassNameW(h, cls, 64);
        RECT r; GetWindowRect(h, &r);
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        Log("  %p class=%s pid=%lu visible=%d rect=%ld,%ld,%ld,%ld dpi=%u",
            (void*)h, Narrow(cls).c_str(), pid, IsWindowVisible(h) ? 1 : 0, r.left, r.top, r.right, r.bottom, GetDpiForWindow(h));
        EnumChildWindows(h, LogChild, 0);
    }
}

int Apply(const std::vector<HWND>& bars, int state, unsigned color, bool verbose) {
    int ok = 0;
    for (HWND h : bars) {
        Accent acc{state, 2, color, 0};
        WcaData d{19, &acc, sizeof acc};
        SetLastError(0);
        BOOL r = g_set(h, &d);
        DWORD e = GetLastError();
        if (r) ++ok;
        if (verbose || !r) Log("  SetWindowCompositionAttribute hwnd=%p state=%d color=%08X -> %d (err=%lu)", (void*)h, state, color, r, e);
    }
    return ok;
}

BOOL WINAPI OnCtrl(DWORD) { InterlockedExchange(&g_stop, 1); return TRUE; }

void Hold(int state, unsigned color, int seconds, const char* name) {
    Log("mode %s: state=%d color=%08X for %s", name, state, color, seconds ? "a fixed time" : "until Ctrl+C");
    DWORD start = GetTickCount();
    bool first = true;
    int lastOk = -1;
    size_t lastCount = 0;
    while (!g_stop && (seconds == 0 || GetTickCount() - start < DWORD(seconds) * 1000)) {
        auto bars = Taskbars();
        if (bars.size() != lastCount) { Describe(bars); lastCount = bars.size(); }
        int ok = Apply(bars, state, color, first);
        if (ok != lastOk) Log("  applied ok=%d of %zu", ok, bars.size());
        lastOk = ok;
        first = false;
        Sleep(250);
    }
}

void Restore() {
    auto bars = Taskbars();
    int ok = Apply(bars, 0, 0, true);
    Log("restore: ok=%d of %zu", ok, bars.size());
}

int wmain(int argc, wchar_t** argv) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring logPath = path;
    logPath = logPath.substr(0, logPath.find_last_of(L'\\') + 1) + L"OpalBar10.log";
    g_log = _wfopen(logPath.c_str(), L"wb");
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    OSVERSIONINFOW v{sizeof v};
    if (auto f = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll"), "RtlGetVersion")) f(&v);
    Log("OpalBar10 test build, log=%s", Narrow(logPath.c_str()).c_str());
    Log("windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    if (v.dwBuildNumber >= 22000) Log("note: Windows 11, taskbar is XAML, composition effects may be ignored");

    g_set = (SetWcaFn)GetProcAddress(GetModuleHandleW(L"user32"), "SetWindowCompositionAttribute");
    Log("SetWindowCompositionAttribute: %s", g_set ? "found" : "missing");
    if (!g_set) return 2;

    auto bars = Taskbars();
    Describe(bars);
    if (bars.empty()) { Log("no taskbar windows, nothing to do"); return 3; }

    if (argc > 1) {
        for (auto& m : kModes) {
            if (Narrow(argv[1]) == m.name) { Hold(m.state, m.color, 0, m.name); Restore(); Log("done"); return 0; }
        }
        Log("unknown mode, use: clear blur acrylic color (no argument = cycle all)");
        return 1;
    }

    for (auto& m : kModes) {
        if (g_stop) break;
        Hold(m.state, m.color, kSecondsPerMode, m.name);
        Restore();
        Sleep(1000);
    }
    Log("done");
    return 0;
}
