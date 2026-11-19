#define NOMINMAX
#include <algorithm>
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
using namespace Gdiplus;

struct Accent { int state; int flags; unsigned color; int anim; };
struct WcaData { int attr; void* data; SIZE_T size; };
using SetWcaFn = BOOL(WINAPI*)(HWND, WcaData*);
using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

constexpr wchar_t kName[] = L"OpalBar";
constexpr wchar_t kVersion[] = L"0.3.0";
constexpr wchar_t kTrayClass[] = L"OpalBar10Tray";
constexpr wchar_t kSettingsClass[] = L"OpalBar10Settings";
constexpr wchar_t kMutex[] = L"OpalBar10.single";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunName[] = L"OpalBar10";
constexpr UINT kTrayMsg = WM_APP + 1, kOpenMsg = WM_APP + 2;
constexpr UINT_PTR kReapply = 1, kSave = 1, kPaint = 2;
constexpr UINT kCmdSettings = 1, kCmdEnabled = 2, kCmdHide = 3, kCmdExit = 4;
constexpr float kWinW = 560, kWinH = 444;
constexpr int kDefMode = 1;
constexpr unsigned kDefTint = 0x202020;
constexpr double kDefDensity = 0.5;

enum Kind { kSlider, kToggle, kColor, kButton, kTile };
struct Item {
    Kind kind; const wchar_t* label;
    double min, max, step, def;
    double v = 0; RectF box{}; bool enabled = true;
};
enum { iEnabled, iClear, iBlur, iAcrylic, iColorMode, iSolid, iTint, iDensity, iAutostart, iTray, iMaxSolid, iAllMon, iLang, iReset, iCount };

static std::vector<Item> g_items = {
    { kToggle, L"Enabled", 0, 1, 1, 1 },
    { kTile, L"Clear", 0, 0, 0, 0 },
    { kTile, L"Blur", 0, 0, 0, 0 },
    { kTile, L"Acrylic", 0, 0, 0, 0 },
    { kTile, L"Color", 0, 0, 0, 0 },
    { kTile, L"Solid", 0, 0, 0, 0 },
    { kColor, L"Tint color", 0, 0, 0, kDefTint },
    { kSlider, L"Tint density", 0, 1, 0.01, kDefDensity },
    { kToggle, L"Start with Windows", 0, 1, 1, 0 },
    { kToggle, L"Tray icon", 0, 1, 1, 1 },
    { kToggle, L"Solid when a window is maximized", 0, 1, 1, 0 },
    { kToggle, L"All monitors", 0, 1, 1, 1 },
    { kButton, L"Русский", 0, 0, 0, 0 },
    { kButton, L"Reset", 0, 0, 0, 0 },
};

static const wchar_t* kRu[][2] = {
    { L"Enabled", L"Включён" }, { L"Disabled", L"Выключен" }, { L"Settings", L"Настройки" },
    { L"Hide", L"Скрыть" }, { L"Exit", L"Выход" }, { L"Effect", L"Эффект" },
    { L"Clear", L"Прозрачный" }, { L"Blur", L"Размытие" }, { L"Acrylic", L"Акрил" }, { L"Color", L"Цвет" },
    { L"Tint color", L"Цвет тонировки" }, { L"Tint density", L"Плотность тонировки" },
    { L"Start with Windows", L"Запускать с Windows" }, { L"Tray icon", L"Значок в трее" },
    { L"Reset", L"Сброс" }, { L"OpalBar Settings", L"Настройки OpalBar" },
    { L"Solid", L"Сплошной" }, { L"Solid when a window is maximized", L"Сплошной при развёрнутом окне" },
    { L"All monitors", L"Все мониторы" },
};

FILE* g_log;
SetWcaFn g_set;
HWND g_tray, g_win;
UINT g_taskbarCreated;
bool g_ru;
int g_mode = kDefMode;
int g_state;
unsigned g_color, g_solid;
float g_s = 1;
int g_hover = -1, g_drag = -1;
std::wstring g_ini;
std::unique_ptr<Bitmap> g_logo;

const wchar_t* Tr(const wchar_t* en) {
    if (g_ru) for (auto& p : kRu) if (!wcscmp(p[0], en)) return p[1];
    return en;
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

double& V(int i) { return g_items[i].v; }
float S(float dip) { return dip * g_s; }

BOOL CALLBACK CollectTaskbars(HWND h, LPARAM lp) {
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
    return TRUE;
}

bool Maximized(HWND bar) {
    struct Ctx { HMONITOR mon; bool found; } c{ MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST), false };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(h) || !IsZoomed(h) || IsIconic(h) || (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
        DWORD cloaked = 0;
        DwmGetWindowAttribute(h, 14, &cloaked, sizeof cloaked);
        if (!cloaked && MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) == c->mon) c->found = true;
        return !c->found;
    }, reinterpret_cast<LPARAM>(&c));
    return c.found;
}

void Push() {
    static size_t lastCount = SIZE_MAX;
    static bool lastFailed = false;
    std::vector<HWND> bars;
    EnumWindows(CollectTaskbars, reinterpret_cast<LPARAM>(&bars));
    if (bars.size() != lastCount) { Log("taskbar windows: %zu", bars.size()); lastCount = bars.size(); }
    bool failed = false;
    for (HWND h : bars) {
        int state = g_state;
        unsigned color = g_color;
        if (V(iEnabled)) {
            wchar_t cls[32];
            GetClassNameW(h, cls, 32);
            if (!wcscmp(cls, L"Shell_SecondaryTrayWnd") && !V(iAllMon)) { state = 0; color = 0; }
            else if (V(iMaxSolid) && Maximized(h)) { state = 1; color = g_solid; }
        }
        Accent acc{state, 2, color, 0};
        WcaData d{19, &acc, sizeof acc};
        SetLastError(0);
        if (!g_set(h, &d)) { failed = true; if (!lastFailed) Log("apply failed hwnd=%p err=%lu", (void*)h, GetLastError()); }
    }
    lastFailed = failed;
}

void Reapply() {
    unsigned c = unsigned(V(iTint));
    unsigned abgr = (unsigned(std::clamp(V(iDensity), 0.0, 1.0) * 255) << 24) | ((c & 255) << 16) | (c & 0xFF00) | (c >> 16 & 255);
    g_solid = abgr | 0xFF000000;
    if (!V(iEnabled)) { g_state = 0; g_color = 0; }
    else if (g_mode == 0 || (g_mode <= 2 && !(abgr >> 24))) { g_state = 2; g_color = 0; }
    else if (g_mode == 1) { g_state = 3; g_color = abgr; }
    else if (g_mode == 2) { g_state = 4; g_color = abgr; }
    else if (g_mode == 3) { g_state = 2; g_color = abgr; }
    else { g_state = 1; g_solid = g_color = abgr | 0xFF000000; }
    Push();
}

bool AutostartOn() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k)) return false;
    bool on = !RegQueryValueExW(k, kRunName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    return on;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k)) return;
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, kRunName, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunName);
    }
    RegCloseKey(k);
}

void Put(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(sec, key, v.c_str(), g_ini.c_str());
}

void Persist() {
    wchar_t b[32];
    swprintf_s(b, L"#%06X", unsigned(V(iTint)));
    Put(L"Taskbar", L"TintColor", b);
    swprintf_s(b, L"%.2f", V(iDensity));
    Put(L"Taskbar", L"TintOpacity", b);
    Put(L"Taskbar", L"Mode", std::to_wstring(g_mode));
    Put(L"App", L"Enabled", V(iEnabled) ? L"1" : L"0");
    Put(L"App", L"TrayIcon", V(iTray) ? L"1" : L"0");
    Put(L"Taskbar", L"SolidWhenMaximized", V(iMaxSolid) ? L"1" : L"0");
    Put(L"Taskbar", L"AllMonitors", V(iAllMon) ? L"1" : L"0");
    Put(L"App", L"Language", g_ru ? L"ru" : L"en");
}

void LoadSettings() {
    wchar_t b[32];
    GetPrivateProfileStringW(L"App", L"Language", L"en", b, 8, g_ini.c_str());
    g_ru = !wcscmp(b, L"ru");
    V(iEnabled) = GetPrivateProfileIntW(L"App", L"Enabled", 1, g_ini.c_str()) != 0;
    V(iTray) = GetPrivateProfileIntW(L"App", L"TrayIcon", 1, g_ini.c_str()) != 0;
    g_mode = GetPrivateProfileIntW(L"Taskbar", L"Mode", kDefMode, g_ini.c_str());
    if (g_mode < 0 || g_mode > 4) g_mode = kDefMode;
    V(iMaxSolid) = GetPrivateProfileIntW(L"Taskbar", L"SolidWhenMaximized", 0, g_ini.c_str()) != 0;
    V(iAllMon) = GetPrivateProfileIntW(L"Taskbar", L"AllMonitors", 1, g_ini.c_str()) != 0;
    GetPrivateProfileStringW(L"Taskbar", L"TintColor", L"", b, 32, g_ini.c_str());
    V(iTint) = b[0] == L'#' ? double(wcstoul(b + 1, nullptr, 16) & 0xFFFFFF) : kDefTint;
    GetPrivateProfileStringW(L"Taskbar", L"TintOpacity", L"", b, 32, g_ini.c_str());
    V(iDensity) = b[0] ? std::clamp(_wtof(b), 0.0, 1.0) : kDefDensity;
    V(iAutostart) = AutostartOn();
}

void AddTray() {
    NOTIFYICONDATAW n{sizeof n};
    n.hWnd = g_tray;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    n.uCallbackMessage = kTrayMsg;
    n.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(n.szTip, kName);
    if (!Shell_NotifyIconW(NIM_ADD, &n)) Log("tray add failed err=%lu", GetLastError());
}

void RemoveTray() {
    NOTIFYICONDATAW n{sizeof n};
    n.hWnd = g_tray;
    n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
}

void ShowTray() { if (V(iTray)) AddTray(); else RemoveTray(); }

void RoundRect(GraphicsPath& p, RectF r, float rad) {
    rad = std::min(rad, std::min(r.Width, r.Height) / 2);
    float d = rad * 2;
    if (d < 1) { p.AddRectangle(r); return; }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

void FillRound(Graphics& g, RectF r, float rad, Color c) {
    GraphicsPath p; RoundRect(p, r, rad);
    SolidBrush b(c); g.FillPath(&b, &p);
}

void StrokeRound(Graphics& g, RectF r, float rad, Color c, float width) {
    GraphicsPath p; RoundRect(p, r, rad);
    Pen pen(c, width); g.DrawPath(&pen, &p);
}

enum Face { kRegular, kSemibold };

void Text(Graphics& g, const std::wstring& s, RectF r, float size, Color c, Face face = kRegular,
          StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter) {
    FontFamily fam(face == kSemibold ? L"Segoe UI Semibold" : L"Segoe UI");
    Font f(&fam, S(size), FontStyleRegular, UnitPixel);
    SolidBrush b(c);
    StringFormat fmt; fmt.SetAlignment(h); fmt.SetLineAlignment(v); fmt.SetFormatFlags(StringFormatFlagsNoWrap); fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(s.c_str(), -1, &f, r, &fmt, &b);
}

float TextW(const wchar_t* s, float size) {
    Bitmap bmp(1, 1);
    Graphics g(&bmp);
    FontFamily fam(L"Segoe UI");
    Font f(&fam, S(size), FontStyleRegular, UnitPixel);
    RectF r;
    g.MeasureString(s, -1, &f, PointF(0, 0), &r);
    return r.Width;
}

Color White(BYTE a) { return Color(a, 255, 255, 255); }
Color Rgb(double v, BYTE a) { auto c = (unsigned)v; return Color(a, BYTE(c >> 16), BYTE(c >> 8), BYTE(c)); }
const Color kText = White(0xF2), kSub = White(0xB0), kMuted = White(0x60), kInk(255, 24, 24, 26);
constexpr BYTE kTile_ = 0x20, kTileHot = 0x38, kEdge = 0x30, kTrackA = 0x48, kFillA = 0xE6, kHair = 0x1C;

std::unique_ptr<Bitmap> LoadPng(int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL h = r ? LoadResource(nullptr, r) : nullptr;
    IStream* st = h ? SHCreateMemStream((const BYTE*)LockResource(h), SizeofResource(nullptr, r)) : nullptr;
    if (!st) return nullptr;
    std::unique_ptr<Bitmap> out;
    {
        Bitmap src(st);
        if (src.GetLastStatus() == Ok) {
            out = std::make_unique<Bitmap>(src.GetWidth(), src.GetHeight(), PixelFormat32bppPARGB);
            Graphics(out.get()).DrawImage(&src, 0, 0, src.GetWidth(), src.GetHeight());
        }
    }
    st->Release();
    return out;
}

RectF g_pane;

void Layout() {
    float m = 24, x = m + 20, w = kWinW - 2 * m - 40, top = 20;
    g_pane = RectF(S(m), S(top), S(kWinW - 2 * m), S(kWinH - top - 20));
    auto box = [&](int i, float X, float Y, float W, float H) { g_items[i].box = RectF(S(X), S(Y), S(W), S(H)); };
    box(iEnabled, kWinW - m - 20 - 130, top + 8, 130, 40);
    float tw = (w - 4 * 8) / 5;
    for (int k = 0; k < 5; k++) box(iClear + k, x + k * (tw + 8), 120, tw, 32);
    box(iTint, x, 164, w, 40);
    box(iDensity, x, 212, w, 44);
    float t1 = std::ceil(TextW(Tr(g_items[iAutostart].label), 13)) + 16 + 40;
    float t2 = std::ceil(TextW(Tr(g_items[iTray].label), 13)) + 16 + 40;
    box(iAutostart, x, 284, t1, 40);
    box(iTray, x + t1 + 32, 284, t2, 40);
    float t3 = std::ceil(TextW(Tr(g_items[iMaxSolid].label), 13)) + 16 + 40;
    float t4 = std::ceil(TextW(Tr(g_items[iAllMon].label), 13)) + 16 + 40;
    box(iMaxSolid, x, 328, t3, 40);
    box(iAllMon, x + t3 + 32, 328, t4, 40);
    box(iLang, x, 376, 120, 32);
    box(iReset, x + 128, 376, 120, 32);
    g_items[iTint].enabled = g_mode != 0;
    g_items[iDensity].enabled = g_mode != 0 && g_mode != 4;
}

RectF Track(const Item& it) { return RectF(it.box.X, it.box.Y + S(30), it.box.Width, S(4)); }

void DrawItem(Graphics& g, int i) {
    auto& it = g_items[i];
    Color text = it.enabled ? kText : kMuted;
    bool hot = it.enabled && (i == g_hover || i == g_drag);
    if (it.kind == kSlider) {
        Text(g, Tr(it.label), RectF(it.box.X, it.box.Y, it.box.Width, S(20)), 13, text);
        wchar_t v[32]; swprintf_s(v, L"%.0f %%", it.v * 100);
        Text(g, v, RectF(it.box.X, it.box.Y, it.box.Width, S(20)), 12, it.enabled ? kSub : kMuted, kRegular, StringAlignmentFar);
        RectF tr = Track(it);
        float f = (float)((it.v - it.min) / (it.max - it.min));
        FillRound(g, tr, S(2), White(it.enabled ? kTrackA : 0x20));
        FillRound(g, RectF(tr.X, tr.Y, tr.Width * f, tr.Height), S(2), White(it.enabled ? kFillA : 0x50));
        float cx = tr.X + tr.Width * f, cy = tr.Y + tr.Height / 2, kr = S(hot ? 8.f : 6.f);
        SolidBrush knob(it.enabled ? White(255) : Color(255, 128, 128, 132));
        g.FillEllipse(&knob, cx - kr, cy - kr, 2 * kr, 2 * kr);
    } else if (it.kind == kToggle) {
        Text(g, Tr(i == iEnabled ? (it.v ? L"Enabled" : L"Disabled") : it.label), it.box, 13, text);
        RectF sw(it.box.GetRight() - S(40), it.box.Y + (it.box.Height - S(20)) / 2, S(40), S(20));
        bool on = it.v != 0;
        if (on) FillRound(g, sw, S(10), White(kFillA));
        else { FillRound(g, sw, S(10), White(hot ? kTileHot : 0x10)); StrokeRound(g, sw, S(10), White(0x80), S(1)); }
        float kd = S(hot ? 14.f : 12.f), kx = on ? sw.GetRight() - S(4) - kd : sw.X + S(4);
        SolidBrush k(on ? kInk : White(0xD0));
        g.FillEllipse(&k, kx, sw.Y + (sw.Height - kd) / 2, kd, kd);
    } else if (it.kind == kColor) {
        Text(g, Tr(it.label), it.box, 13, text);
        RectF sw(it.box.GetRight() - S(40), it.box.Y + (it.box.Height - S(24)) / 2, S(40), S(24));
        FillRound(g, sw, S(6), Rgb(it.v, it.enabled ? 255 : 0x60));
        StrokeRound(g, sw, S(6), White(hot ? 0x90 : 0x40), S(1));
        wchar_t hex[16]; swprintf_s(hex, L"#%06X", unsigned(it.v));
        Text(g, hex, RectF(it.box.X, it.box.Y, it.box.Width - S(52), it.box.Height), 12, it.enabled ? kSub : kMuted, kRegular, StringAlignmentFar);
    } else {
        bool sel = it.kind == kTile && i - iClear == g_mode;
        FillRound(g, it.box, S(8), White(sel ? 0x50 : hot ? kTileHot : kTile_));
        StrokeRound(g, it.box, S(8), White(sel ? 0xB0 : kEdge), S(1));
        Text(g, i == iLang ? (g_ru ? L"English" : L"Русский") : Tr(it.label), it.box, 13, kText, sel ? kSemibold : kRegular, StringAlignmentCenter);
    }
}

void Paint(HWND w, HDC dc) {
    RECT rc; GetClientRect(w, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    Layout();
    Bitmap buf(rc.right, rc.bottom, PixelFormat32bppPARGB);
    Graphics g(&buf);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.Clear(Color(255, 24, 24, 26));
    FillRound(g, g_pane, S(16), Color(255, 38, 38, 42));
    StrokeRound(g, g_pane, S(16), White(kEdge), S(1));
    if (g_logo) {
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(g_logo.get(), RectF(g_pane.X + S(20), g_pane.Y + S(14), S(28), S(28)));
    }
    Text(g, kName, RectF(g_pane.X + S(56), g_pane.Y, S(120), S(56)), 20, kText, kSemibold);
    Text(g, kVersion, RectF(g_pane.X + S(144), g_pane.Y + S(3), S(60), S(56)), 12, kSub);
    SolidBrush hair(White(kHair));
    g.FillRectangle(&hair, g_pane.X, S(76), g_pane.Width, S(1));
    g.FillRectangle(&hair, g_pane.X, S(272), g_pane.Width, S(1));
    Text(g, Tr(L"Effect"), RectF(g_pane.X + S(20), S(88), S(300), S(24)), 14, kText, kSemibold);
    for (int i = 0; i < iCount; i++) DrawItem(g, i);
    Graphics(dc).DrawImage(&buf, 0, 0);
}

int HitTest(float x, float y) {
    for (int i = 0; i < iCount; i++) {
        auto& it = g_items[i];
        RectF r = it.box;
        if (it.kind == kSlider) r = RectF(r.X - S(10), r.Y + S(20), r.Width + S(20), S(28));
        if (it.enabled && r.Contains(x, y)) return i;
    }
    return -1;
}

void Change(int i, double v) {
    auto& it = g_items[i];
    if (it.kind == kSlider) v = std::clamp(v, it.min, it.max);
    it.v = v;
    if (i == iAutostart) SetAutostart(v != 0);
    if (i == iTray) ShowTray();
    Reapply();
    if (g_win) { SetTimer(g_win, kSave, 200, nullptr); InvalidateRect(g_win, nullptr, FALSE); }
    else Persist();
}

void SetMode(int m) {
    g_mode = m;
    Reapply();
    if (g_win) { SetTimer(g_win, kSave, 200, nullptr); InvalidateRect(g_win, nullptr, FALSE); }
    else Persist();
}

void Activate(HWND w, int i) {
    auto& it = g_items[i];
    if (it.kind == kToggle) Change(i, it.v ? 0 : 1);
    else if (it.kind == kTile) SetMode(i - iClear);
    else if (it.kind == kColor) {
        static COLORREF custom[16];
        unsigned c = unsigned(it.v);
        CHOOSECOLORW cc{ sizeof(cc), w };
        cc.rgbResult = RGB(c >> 16 & 255, c >> 8 & 255, c & 255);
        cc.lpCustColors = custom;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        if (ChooseColorW(&cc)) Change(i, double((GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult)));
    } else if (i == iLang) {
        g_ru = !g_ru;
        SetWindowTextW(w, Tr(L"OpalBar Settings"));
        SetTimer(w, kSave, 200, nullptr);
        InvalidateRect(w, nullptr, FALSE);
    } else if (i == iReset) {
        g_mode = kDefMode;
        V(iTint) = kDefTint;
        V(iDensity) = kDefDensity;
        V(iMaxSolid) = 0;
        V(iAllMon) = 1;
        Reapply();
        SetTimer(w, kSave, 200, nullptr);
        InvalidateRect(w, nullptr, FALSE);
    }
}

void SlideTo(int i, float x) {
    auto& it = g_items[i];
    RectF tr = Track(it);
    Change(i, it.min + std::clamp((x - tr.X) / tr.Width, 0.f, 1.f) * (it.max - it.min));
}

LRESULT CALLBACK SettingsProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        Paint(w, dc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int i = HitTest((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
        if (i < 0) return 0;
        if (g_items[i].kind == kSlider) { g_drag = i; SetCapture(w); SlideTo(i, (float)GET_X_LPARAM(lp)); }
        else Activate(w, i);
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        int i = HitTest((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
        if (i >= 0 && g_items[i].kind == kColor) Change(i, g_items[i].def);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_drag >= 0) { SlideTo(g_drag, (float)GET_X_LPARAM(lp)); return 0; }
        int h = HitTest((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
        if (h != g_hover) {
            g_hover = h;
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, w };
            TrackMouseEvent(&t);
            InvalidateRect(w, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = -1;
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (g_drag >= 0) { g_drag = -1; ReleaseCapture(); InvalidateRect(w, nullptr, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(w, &p);
            SetCursor(LoadCursorW(nullptr, HitTest((float)p.x, (float)p.y) >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(w);
        return 0;
    case WM_TIMER:
        KillTimer(w, kSave);
        Persist();
        return 0;
    case WM_DPICHANGED: {
        g_s = HIWORD(wp) / 96.f;
        auto r = (RECT*)lp;
        SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(w, kSave);
        Persist();
        g_win = nullptr;
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void OpenSettings() {
    if (g_win) { ShowWindow(g_win, SW_RESTORE); SetForegroundWindow(g_win); return; }
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kSettingsClass;
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    UINT dpi = GetDpiForSystem();
    g_s = dpi / 96.f;
    RECT r{ 0, 0, (LONG)S(kWinW), (LONG)S(kWinH) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_win = CreateWindowExW(0, kSettingsClass, Tr(L"OpalBar Settings"), style, wa.left + (wa.right - wa.left - (r.right - r.left)) / 2,
                            wa.top + (wa.bottom - wa.top - (r.bottom - r.top)) / 2, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!g_win) { Log("settings window failed err=%lu", GetLastError()); return; }
    g_s = GetDpiForWindow(g_win) / 96.f;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_win, 20, &dark, sizeof(dark));
    ShowWindow(g_win, SW_SHOW);
    SetForegroundWindow(g_win);
}

void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdSettings, Tr(L"Settings"));
    AppendMenuW(menu, MF_STRING | (V(iEnabled) ? MF_CHECKED : 0), kCmdEnabled, Tr(L"Enabled"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdHide, Tr(L"Hide"));
    AppendMenuW(menu, MF_STRING, kCmdExit, Tr(L"Exit"));
    SetMenuDefaultItem(menu, kCmdSettings, FALSE);
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_tray);
    UINT id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_tray, nullptr);
    PostMessageW(g_tray, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (id == kCmdSettings) OpenSettings();
    else if (id == kCmdEnabled) Change(iEnabled, V(iEnabled) ? 0 : 1);
    else if (id == kCmdHide) Change(iTray, 0);
    else if (id == kCmdExit) DestroyWindow(g_tray);
}

LRESULT CALLBACK TrayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_taskbarCreated) { ShowTray(); Reapply(); return 0; }
    switch (m) {
    case WM_TIMER:
        if (g_state) Push();
        return 0;
    case kTrayMsg:
        if (LOWORD(l) == WM_LBUTTONUP) OpenSettings();
        else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) ShowMenu();
        return 0;
    case kOpenMsg:
        OpenSettings();
        return 0;
    case WM_DESTROY:
        if (g_win) DestroyWindow(g_win);
        KillTimer(h, kReapply);
        V(iEnabled) = 0;
        g_state = 0; g_color = 0;
        Push();
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE once = CreateMutexW(nullptr, TRUE, kMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND old = FindWindowW(kTrayClass, nullptr)) PostMessageW(old, kOpenMsg, 0, 0);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    dir.resize(dir.find_last_of(L'\\') + 1);
    g_ini = dir + L"OpalBar10.ini";
    g_log = _wfopen((dir + L"OpalBar10.log").c_str(), L"wb");

    OSVERSIONINFOW v{sizeof v};
    if (auto f = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll"), "RtlGetVersion")) f(&v);
    Log("OpalBar10 %ls windows %lu.%lu build %lu", kVersion, v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);

    g_set = (SetWcaFn)GetProcAddress(GetModuleHandleW(L"user32"), "SetWindowCompositionAttribute");
    if (!g_set) { Log("SetWindowCompositionAttribute missing"); return 2; }

    ULONG_PTR gdip;
    GdiplusStartupInput gsi;
    GdiplusStartup(&gdip, &gsi, nullptr);
    g_logo = LoadPng(2);
    LoadSettings();

    WNDCLASSW wc{};
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = inst;
    wc.lpszClassName = kTrayClass;
    RegisterClassW(&wc);
    g_tray = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayClass, kName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    ShowTray();
    Reapply();
    SetTimer(g_tray, kReapply, 250, nullptr);
    Log("started, mode %d enabled %d", g_mode, int(V(iEnabled)));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Log("exit");
    g_logo.reset();
    GdiplusShutdown(gdip);
    CloseHandle(once);
    return 0;
}
