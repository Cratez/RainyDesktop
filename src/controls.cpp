#include "controls.h"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_MSC_VER)
// Themed (modern-looking) common controls.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace fs = std::filesystem;

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
    kMenuSettings = 100,
    kMenuWipe,
    kMenuClear,
    kMenuPause,
    kMenuHide,
    kMenuScreenshot,
    kMenuQuit,
    kMenuAutoWipeBase = 200,  // + minutes
};

enum ControlId : int {
    kIdSliderBase = 1000,  // + slider index
    kIdValueBase = 1100,   // + slider index
    kIdPause = 1200,
    kIdWipe,
    kIdDefaults,
    kIdClose,
};

struct SliderDef {
    const wchar_t* label;
    float Settings::*field;
    float lo, hi;
    bool logScale;
    // Formats the value for display.
    void (*format)(float v, wchar_t* out, size_t n);
};

void FormatTimes(float v, wchar_t* out, size_t n) { swprintf_s(out, n, L"%.1fx", v); }
// Radius in pixels at 1440p on a 27" panel (about 0.233 mm per pixel) to a diameter in mm.
void FormatMm(float v, wchar_t* out, size_t n) { swprintf_s(out, n, L"%.1f mm", v * 2.0f * 0.233f); }

const SliderDef kSliders[] = {
    {L"Rain amount", &Settings::rain, 0.1f, 6.0f, true, FormatTimes},
    {L"Fine mist", &Settings::mist, 0.0f, 3.0f, false, FormatTimes},
    {L"Glass grip (friction)", &Settings::friction, 0.4f, 2.5f, true, FormatTimes},
    {L"Running speed", &Settings::runSpeed, 0.3f, 3.0f, true, FormatTimes},
    {L"Smallest raindrop", &Settings::minDrop, 1.0f, 10.0f, false, FormatMm},
    {L"Largest raindrop", &Settings::maxDrop, 3.0f, 25.0f, false, FormatMm},
    {L"Streak life", &Settings::streakLife, 0.3f, 4.0f, true, FormatTimes},
};
constexpr int kSliderCount = static_cast<int>(sizeof(kSliders) / sizeof(kSliders[0]));
constexpr int kSliderSteps = 200;

const int kAutoWipeChoices[] = {0, 1, 2, 5, 10, 30};

struct State {
    HINSTANCE inst = nullptr;
    fs::path ini;
    Settings* settings = nullptr;
    ControlCallbacks cb;
    HWND tray = nullptr;      // hidden window that owns the tray icon
    HWND window = nullptr;    // settings window, when open
    HFONT font = nullptr;
    HICON icon = nullptr, iconSmall = nullptr;
    UINT taskbarCreated = 0;
    NOTIFYICONDATAW nid{};
};

State st;

int ToSlider(const SliderDef& d, float v) {
    float t = d.logScale ? std::log(v / d.lo) / std::log(d.hi / d.lo) : (v - d.lo) / (d.hi - d.lo);
    return static_cast<int>(std::lround(std::clamp(t, 0.0f, 1.0f) * kSliderSteps));
}

float FromSlider(const SliderDef& d, int pos) {
    float t = static_cast<float>(pos) / kSliderSteps;
    return d.logScale ? d.lo * std::pow(d.hi / d.lo, t) : d.lo + (d.hi - d.lo) * t;
}

// A blue raindrop, drawn per pixel so it needs no resource file.
HICON MakeDropIcon(int size) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    if (!color || !mask) return LoadIcon(nullptr, IDI_APPLICATION);

    auto* px = static_cast<uint32_t*>(bits);
    const float cx = 0.5f, cy = 0.62f, r = 0.32f, tipY = 0.06f;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            // Signed distance (in icon units) to a circle topped by a cone.
            float dc_ = std::hypot(u - cx, v - cy) - r;
            float halfW = v < cy ? r * std::clamp((v - tipY) / (cy - tipY), 0.0f, 1.0f) : 0.0f;
            float dcone = v < cy && v > tipY ? std::fabs(u - cx) - halfW : 1.0f;
            float dist = std::min(dc_, dcone * 0.85f);
            float a = std::clamp(0.5f - dist * size, 0.0f, 1.0f);
            // Shading: deeper blue at the bottom, a highlight upper left.
            float shade = 0.55f + 0.45f * (1.0f - v);
            float hl = std::exp(-(std::pow(u - 0.4f, 2.0f) + std::pow(v - 0.55f, 2.0f)) * 90.0f);
            float R = std::min(1.0f, 0.20f * shade + hl), G = std::min(1.0f, 0.55f * shade + hl),
                  B = std::min(1.0f, 0.95f * shade + hl);
            auto c = [&](float f) { return static_cast<uint32_t>(f * a * 255.0f + 0.5f); };
            px[y * size + x] = (c(1.0f) << 24) | (c(R) << 16) | (c(G) << 8) | c(B);
        }
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIcon(nullptr, IDI_APPLICATION);
}

// The icon embedded in the exe, falling back to the drawn one if it is missing.
HICON LoadAppIcon(HINSTANCE inst, int size) {
    if (auto icon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, 0)))
        return icon;
    return MakeDropIcon(size);
}

// ---------------------------------------------------------------- persistence

float ReadFloat(const wchar_t* key, float def) {
    wchar_t buf[64];
    GetPrivateProfileStringW(L"raindesk", key, L"", buf, 64, st.ini.c_str());
    if (!buf[0]) return def;
    return static_cast<float>(_wtof(buf));
}

void WriteFloat(const wchar_t* key, float v) {
    wchar_t buf[64];
    swprintf_s(buf, L"%.4f", v);
    WritePrivateProfileStringW(L"raindesk", key, buf, st.ini.c_str());
}

void Load() {
    Settings& s = *st.settings;
    Settings def;
    s.rain = ReadFloat(L"rain", def.rain);
    s.mist = ReadFloat(L"mist", def.mist);
    s.friction = ReadFloat(L"friction", def.friction);
    s.runSpeed = ReadFloat(L"runSpeed", def.runSpeed);
    s.minDrop = ReadFloat(L"minDrop", def.minDrop);
    s.maxDrop = ReadFloat(L"maxDrop", def.maxDrop);
    s.streakLife = ReadFloat(L"streakLife", def.streakLife);
    s.autoWipeMinutes = static_cast<int>(ReadFloat(L"autoWipeMinutes", 0));
    s.paused = ReadFloat(L"paused", 0) != 0;
    for (const SliderDef& d : kSliders) s.*d.field = std::clamp(s.*d.field, d.lo, d.hi);
}

// ---------------------------------------------------------------- settings window

HWND Child(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, st.window,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), st.inst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
    return c;
}

void ShowValue(int i) {
    wchar_t text[32];
    kSliders[i].format(st.settings->*kSliders[i].field, text, 32);
    SetDlgItemTextW(st.window, kIdValueBase + i, text);
}

void SyncWindow() {
    if (!st.window) return;
    for (int i = 0; i < kSliderCount; ++i) {
        SendDlgItemMessageW(st.window, kIdSliderBase + i, TBM_SETPOS, TRUE,
                            ToSlider(kSliders[i], st.settings->*kSliders[i].field));
        ShowValue(i);
    }
    CheckDlgButton(st.window, kIdPause, st.settings->paused ? BST_CHECKED : BST_UNCHECKED);
}

void BuildWindow(HWND hwnd) {
    st.window = hwnd;
    UINT dpi = GetDpiForWindow(hwnd);
    auto S = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };

    if (st.font) DeleteObject(st.font);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    st.font = CreateFontIndirectW(&ncm.lfMessageFont);

    int y = S(14);
    const int labelW = S(150), sliderW = S(220), valueW = S(70), rowH = S(34), x0 = S(14);
    for (int i = 0; i < kSliderCount; ++i) {
        Child(L"STATIC", kSliders[i].label, SS_LEFT, x0, y + S(6), labelW, S(20), -1);
        HWND tb = Child(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x0 + labelW, y, sliderW, S(30),
                        kIdSliderBase + i);
        SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(0, kSliderSteps));
        SendMessageW(tb, TBM_SETPAGESIZE, 0, kSliderSteps / 10);
        Child(L"STATIC", L"", SS_LEFT, x0 + labelW + sliderW + S(8), y + S(6), valueW, S(20), kIdValueBase + i);
        y += rowH;
    }
    y += S(4);
    Child(L"BUTTON", L"Pause rain", BS_AUTOCHECKBOX | WS_TABSTOP, x0, y, S(200), S(24), kIdPause);
    y += S(36);
    const int bw = S(110), bh = S(30), gap = S(10);
    Child(L"BUTTON", L"Wipe now", BS_PUSHBUTTON | WS_TABSTOP, x0, y, bw, bh, kIdWipe);
    Child(L"BUTTON", L"Defaults", BS_PUSHBUTTON | WS_TABSTOP, x0 + bw + gap, y, bw, bh, kIdDefaults);
    Child(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, x0 + 2 * (bw + gap), y, bw, bh, kIdClose);
    y += bh + S(14);

    RECT rc{0, 0, x0 * 2 + labelW + sliderW + S(8) + valueW, y};
    AdjustWindowRectExForDpi(&rc, GetWindowLongW(hwnd, GWL_STYLE), FALSE, GetWindowLongW(hwnd, GWL_EXSTYLE), dpi);
    SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
    SyncWindow();
}

void Changed() {
    controls::Save();
    if (st.cb.changed) st.cb.changed();
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            BuildWindow(hwnd);
            return 0;
        case WM_HSCROLL: {
            int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
            int i = id - kIdSliderBase;
            if (i >= 0 && i < kSliderCount) {
                int pos = static_cast<int>(SendMessageW(reinterpret_cast<HWND>(lp), TBM_GETPOS, 0, 0));
                st.settings->*kSliders[i].field = FromSlider(kSliders[i], pos);
                ShowValue(i);
                Changed();
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdPause:
                    st.settings->paused = IsDlgButtonChecked(hwnd, kIdPause) == BST_CHECKED;
                    Changed();
                    break;
                case kIdWipe:
                    if (st.cb.wipe) st.cb.wipe();
                    break;
                case kIdDefaults: {
                    Settings def;
                    def.autoWipeMinutes = st.settings->autoWipeMinutes;
                    def.hidden = st.settings->hidden;
                    *st.settings = def;
                    SyncWindow();
                    Changed();
                    break;
                }
                case kIdClose:
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;
        case WM_DPICHANGED: {
            // Rebuild at the new size on the monitor the window moved to.
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(hwnd, GW_CHILD)) DestroyWindow(c);
            SetWindowPos(hwnd, nullptr, r->left, r->top, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
            BuildWindow(hwnd);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        case WM_DESTROY:
            st.window = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- tray

void AddTrayIcon() {
    st.nid = {};
    st.nid.cbSize = sizeof(st.nid);
    st.nid.hWnd = st.tray;
    st.nid.uID = kTrayId;
    st.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    st.nid.uCallbackMessage = kTrayMessage;
    st.nid.hIcon = st.iconSmall;
    wcscpy_s(st.nid.szTip, L"raindesk");
    Shell_NotifyIconW(NIM_ADD, &st.nid);
    st.nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &st.nid);
}

void ShowTrayMenu() {
    Settings& s = *st.settings;
    HMENU autoWipe = CreatePopupMenu();
    for (int m : kAutoWipeChoices) {
        wchar_t text[32];
        if (m == 0)
            wcscpy_s(text, L"Off");
        else
            swprintf_s(text, L"Every %d minute%s", m, m == 1 ? L"" : L"s");
        AppendMenuW(autoWipe, MF_STRING | (s.autoWipeMinutes == m ? MF_CHECKED : 0), kMenuAutoWipeBase + m, text);
    }
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuWipe, L"Wipe the glass\tCtrl+Alt+W");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(autoWipe), L"Auto wipe");
    AppendMenuW(menu, MF_STRING, kMenuClear, L"Dry the glass instantly");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (s.paused ? MF_CHECKED : 0), kMenuPause, L"Pause rain");
    AppendMenuW(menu, MF_STRING | (s.hidden ? MF_CHECKED : 0), kMenuHide, L"Hide rain");
    AppendMenuW(menu, MF_STRING, kMenuScreenshot, L"Save screenshot\tCtrl+Alt+S");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit\tCtrl+Alt+Q");
    SetMenuDefaultItem(menu, kMenuSettings, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(st.tray);  // so the menu closes when you click elsewhere
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, st.tray, nullptr);
    PostMessageW(st.tray, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenu

    switch (cmd) {
        case kMenuSettings:
            controls::OpenSettings();
            break;
        case kMenuWipe:
            if (st.cb.wipe) st.cb.wipe();
            break;
        case kMenuClear:
            if (st.cb.clearNow) st.cb.clearNow();
            break;
        case kMenuPause:
            s.paused = !s.paused;
            SyncWindow();
            Changed();
            break;
        case kMenuHide:
            s.hidden = !s.hidden;
            Changed();
            break;
        case kMenuScreenshot:
            if (st.cb.screenshot) st.cb.screenshot();
            break;
        case kMenuQuit:
            if (st.cb.quit) st.cb.quit();
            break;
        default:
            if (cmd >= kMenuAutoWipeBase && cmd <= kMenuAutoWipeBase + 60) {
                s.autoWipeMinutes = static_cast<int>(cmd - kMenuAutoWipeBase);
                Changed();
            }
    }
}

LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kTrayMessage) {
        switch (LOWORD(lp)) {
            case WM_CONTEXTMENU:
            case WM_RBUTTONUP:
                ShowTrayMenu();
                break;
            case NIN_SELECT:
            case NIN_KEYSELECT:
            case WM_LBUTTONDBLCLK:
                controls::OpenSettings();
                break;
        }
        return 0;
    }
    if (msg == st.taskbarCreated && st.taskbarCreated) {  // Explorer restarted
        AddTrayIcon();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

namespace controls {

HICON AppIcon() { return st.icon; }

void Init(HINSTANCE inst, const fs::path& iniPath, Settings& settings, ControlCallbacks callbacks) {
    st.inst = inst;
    st.ini = iniPath;
    st.settings = &settings;
    st.cb = std::move(callbacks);
    Load();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    st.icon = LoadAppIcon(inst, GetSystemMetrics(SM_CXICON));
    st.iconSmall = LoadAppIcon(inst, GetSystemMetrics(SM_CXSMICON));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"RaindeskTray";
    RegisterClassExW(&wc);
    st.tray = CreateWindowExW(0, wc.lpszClassName, L"raindesk", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);

    WNDCLASSEXW sc{};
    sc.cbSize = sizeof(sc);
    sc.lpfnWndProc = SettingsProc;
    sc.hInstance = inst;
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    sc.hIcon = st.icon;
    sc.hIconSm = st.iconSmall;
    sc.lpszClassName = L"RaindeskSettings";
    RegisterClassExW(&sc);

    st.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();
}

void Shutdown() {
    Shell_NotifyIconW(NIM_DELETE, &st.nid);
    if (st.window) DestroyWindow(st.window);
    if (st.tray) DestroyWindow(st.tray);
}

void OpenSettings() {
    if (st.window) {
        ShowWindow(st.window, SW_SHOWNORMAL);
        SetForegroundWindow(st.window);
        return;
    }
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"RaindeskSettings", L"raindesk settings",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                                CW_USEDEFAULT, 400, 300, nullptr, nullptr, st.inst, nullptr);
    // Open near the bottom right, above the tray.
    RECT wr, work;
    GetWindowRect(hwnd, &wr);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    SetWindowPos(hwnd, nullptr, work.right - w - 24, work.bottom - h - 24, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
}

void Save() {
    const Settings& s = *st.settings;
    WriteFloat(L"rain", s.rain);
    WriteFloat(L"mist", s.mist);
    WriteFloat(L"friction", s.friction);
    WriteFloat(L"runSpeed", s.runSpeed);
    WriteFloat(L"minDrop", s.minDrop);
    WriteFloat(L"maxDrop", s.maxDrop);
    WriteFloat(L"streakLife", s.streakLife);
    WriteFloat(L"autoWipeMinutes", static_cast<float>(s.autoWipeMinutes));
    WriteFloat(L"paused", s.paused ? 1.0f : 0.0f);
}

}  // namespace controls
