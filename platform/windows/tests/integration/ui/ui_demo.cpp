// ui_demo: standalone host for the tray icon, quick panel and the themed settings window.
//
//   ui_demo                 interactive: tray icon + settings window, changes are logged
//   ui_demo --selftest      non-interactive checks; writes ui_selftest.txt and PNG previews next
//                           to the exe; exit code != 0 on failure
//   --dpi <n>               lay the settings window out for the given DPI (e.g. 144 = 150 %)
#include <windows.h>

#include <commctrl.h>
#include <objidl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "app/ui/countdown.h"
#include "core/skins.h"
#include "app/ui/d2d_paint.h"
#include "app/ui/hotkey_edit.h"
#include "app/ui/process_picker.h"
#include "app/ui/settings_ids.h"
#include "app/ui/settings_window.h"
#include "app/ui/strings.h"
#include "app/ui/theme.h"
#include "app/ui/tray_icon.h"
#include "app/ui/ui_util.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace snow;
using namespace snow::ui;

namespace {

constexpr UINT kTrayCommandMsg = WM_APP + 1;
const wchar_t* kThemeTag[3] = {L"night", L"globe", L"postcard"};

struct Demo {
    Settings settings;
    TrayIcon tray;
    SettingsWindow window;
    bool paused = false;
    bool hidden = false;
    bool interactionMode = false;
    int changedCount = 0;
    int clearCount = 0;
    int fireworksCount = 0;
    int diagCalls = 0;
    int quickChanges = 0;
    std::vector<std::pair<int, LPARAM>> commands;
    std::wstring exeDir;
};
Demo* g_demo = nullptr;

std::wstring textOf(HWND hwnd) {
    wchar_t buf[2048] = {};
    GetWindowTextW(hwnd, buf, ARRAYSIZE(buf));
    return buf;
}

void logLine(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

TrayState trayState(const Demo& d) {
    TrayState st;
    st.paused = d.paused;
    st.hidden = d.hidden;
    st.interactionMode = d.interactionMode;
    st.autostart = d.settings.startupWithWindows;
    st.preset = d.settings.weather.preset;
    st.tool = d.settings.interaction.tool;
    st.leftClickTogglesPause = d.settings.system.trayLeftClickTogglesPause;
    for (const UserPreset& p : d.settings.userPresets) st.userPresetNames.push_back(widen(p.name));
    st.interactionKeys = L"Ctrl+Alt";
    return st;
}

DiagnosticsInfo fakeDiagnostics() {
    Demo& d = *g_demo;
    ++d.diagCalls;
    DiagnosticsInfo info;
    info.fps = 59.9f;
    info.frameMs = 1.37f;
    info.simStepMs = 0.42f;
    info.particles = 1840;
    info.surfaces = 12;
    info.snowballs = 3;
    info.snowmen = 1;
    info.monitors = 2;
    info.trackedWindows = 17;
    info.paused = d.paused;
    info.statusText = tr(d.hidden ? StrId::StatusHidden : d.paused ? StrId::StatusPaused : StrId::StatusRunning);
    info.logPath = d.exeDir + L"\\ui_demo.log";
    info.interactionKeys = L"Ctrl+Alt";
    return info;
}

SettingsWindowCallbacks makeCallbacks() {
    SettingsWindowCallbacks cb;
    cb.onChanged = [](const Settings& s) {
        Demo& d = *g_demo;
        d.settings = s;
        ++d.changedCount;
        d.tray.setSettings(s);
        d.tray.setState(trayState(d));
        logLine("onChanged #%d", d.changedCount);
    };
    cb.onClearSnow = [] { ++g_demo->clearCount; };
    cb.onFireworks = [] { ++g_demo->fireworksCount; };
    cb.getDiagnostics = fakeDiagnostics;
    cb.validateHotkey = [](const std::string& text) -> std::wstring {
        return text == "Ctrl+Alt+F9" ? std::wstring(tr(StrId::HotkeyInUse)) : std::wstring();
    };
    return cb;
}

LRESULT CALLBACK ownerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Demo* d = g_demo;
    LRESULT result = 0;
    if (d && d->tray.handleMessage(hwnd, msg, wp, lp, &result)) return result;
    if (d && msg == kTrayCommandMsg) {
        const auto cmd = static_cast<TrayCommand>(wp);
        d->commands.push_back({static_cast<int>(wp), lp});
        switch (cmd) {
            case TrayCommand::TogglePause: d->paused = !d->paused; break;
            case TrayCommand::PresetLight: d->settings.weather.preset = WeatherPreset::Light; break;
            case TrayCommand::PresetNormal: d->settings.weather.preset = WeatherPreset::Normal; break;
            case TrayCommand::PresetBlizzard: d->settings.weather.preset = WeatherPreset::Blizzard; break;
            case TrayCommand::PresetThaw: d->settings.weather.preset = WeatherPreset::Thaw; break;
            case TrayCommand::OpenSettings: d->window.show(d->settings); break;
            case TrayCommand::OpenQuickPanel: d->tray.showQuickPanel(); break;
            case TrayCommand::ToggleInteraction: d->interactionMode = !d->interactionMode; break;
            case TrayCommand::ToolMitten: d->settings.interaction.tool = Tool::Mitten; break;
            case TrayCommand::ToolShovel: d->settings.interaction.tool = Tool::Shovel; break;
            case TrayCommand::ToolSnowball: d->settings.interaction.tool = Tool::Snowball; break;
            case TrayCommand::ToggleHideAll: d->hidden = !d->hidden; break;
            case TrayCommand::ToggleAutostart: d->settings.startupWithWindows = !d->settings.startupWithWindows; break;
            case TrayCommand::ApplyUserPreset:
                if (lp >= 0 && static_cast<size_t>(lp) < d->settings.userPresets.size()) {
                    const UserPreset& p = d->settings.userPresets[static_cast<size_t>(lp)];
                    d->settings.weather = p.weather;
                    d->settings.accumulation.maxHeightPxAt96Dpi = p.maxHeightPxAt96Dpi;
                    d->settings.accumulation.driftOpacityPercent = p.driftOpacityPercent;
                }
                break;
            case TrayCommand::Exit: PostQuitMessage(0); break;
            default: break;
        }
        d->tray.setSettings(d->settings);
        d->tray.setState(trayState(*d));
        d->window.update(d->settings);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND createOwner(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ownerProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SnowUiDemoOwner";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"ui_demo", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
}

void pump(SettingsWindow& window, DWORD ms = 0) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (!window.preTranslateMessage(&msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (GetTickCount64() - start >= ms) break;
        Sleep(10);
    }
}

// ---- image helpers ---------------------------------------------------------------------------
bool savePng(HBITMAP bitmap, const std::wstring& path) {
    static const CLSID kPngEncoder = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    Gdiplus::Bitmap image(bitmap, nullptr);
    return image.Save(path.c_str(), &kPngEncoder, nullptr) == Gdiplus::Ok;
}

struct MemBitmap {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    int w = 0, h = 0;
    MemBitmap(int width, int height) : w(width), h(height) {
        HDC screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        bmp = CreateCompatibleBitmap(screen, w, h);
        old = SelectObject(dc, bmp);
        ReleaseDC(nullptr, screen);
    }
    ~MemBitmap() {
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    bool save(const std::wstring& path) {
        SelectObject(dc, old);
        const bool ok = savePng(bmp, path);
        old = SelectObject(dc, bmp);
        return ok;
    }
    std::vector<unsigned> pixels() {
        std::vector<unsigned> px(static_cast<size_t>(w) * static_cast<size_t>(h));
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SelectObject(dc, old);
        GetDIBits(dc, bmp, 0, static_cast<UINT>(h), px.data(), &bi, DIB_RGB_COLORS);
        old = SelectObject(dc, bmp);
        return px;
    }
};

// Capture of our OWN window (PrintWindow). Origin = window rect top-left.
struct Capture {
    std::vector<unsigned> px;
    int w = 0, h = 0;
    unsigned at(int x, int y) const {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0;
        return px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
    }
};

Capture captureWindow(HWND hwnd, const std::wstring& path = L"") {
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    MemBitmap mem(rc.right - rc.left, rc.bottom - rc.top);
    PrintWindow(hwnd, mem.dc, PW_RENDERFULLCONTENT);
    if (!path.empty()) mem.save(path);
    Capture c;
    c.w = mem.w;
    c.h = mem.h;
    c.px = mem.pixels();
    return c;
}

void fill(HDC dc, int x, int y, int w, int h, COLORREF color) {
    RECT rc{x, y, x + w, y + h};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

bool nearColor(unsigned px, COLORREF c, int tol = 10) {
    const int r = static_cast<int>((px >> 16) & 0xFF), g = static_cast<int>((px >> 8) & 0xFF), b = static_cast<int>(px & 0xFF);
    return std::abs(r - GetRValue(c)) <= tol && std::abs(g - GetGValue(c)) <= tol && std::abs(b - GetBValue(c)) <= tol;
}

// ---- self test ---------------------------------------------------------------------------------
struct Tester {
    FILE* out = nullptr;
    int checks = 0;
    int failures = 0;
    std::string prefix;
    void line(const char* fmt, ...) {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        printf("%s\n", buf);
        if (out) fprintf(out, "%s\n", buf);
    }
    void check(bool ok, const char* what) {
        ++checks;
        if (!ok) ++failures;
        line("%s  %s%s", ok ? "PASS" : "FAIL", prefix.c_str(), what);
    }
};

// Top-level window of THIS process with the given class (a real SnowFall instance may be running).
HWND findOwnWindow(const wchar_t* cls) {
    struct Q {
        const wchar_t* cls;
        HWND found;
    } q{cls, nullptr};
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            auto* qq = reinterpret_cast<Q*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid != GetCurrentProcessId()) return TRUE;
            wchar_t name[64] = {};
            GetClassNameW(w, name, ARRAYSIZE(name));
            if (lstrcmpW(name, qq->cls) != 0) return TRUE;
            qq->found = w;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&q));
    return q.found;
}

HWND ctl(HWND window, int id) { return uitest::findControl(window, id); }

void click(HWND window, int id) { SendMessageW(ctl(window, id), BM_CLICK, 0, 0); }

void selectTab(HWND window, int index) {
    click(window, IDC_NAV_FIRST + index);
}

void slide(HWND window, int id, int pos) {
    HWND tb = ctl(window, id);
    SendMessageW(tb, TBM_SETPOS, TRUE, pos);
    SendMessageW(GetParent(tb), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, pos), reinterpret_cast<LPARAM>(tb));
}

bool isChecked(HWND window, int id) { return SendMessageW(ctl(window, id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
int sliderPos(HWND window, int id) { return static_cast<int>(SendMessageW(ctl(window, id), TBM_GETPOS, 0, 0)); }

UINT windowDpiScale(HWND window) {
    // Layout DPI derived from the page width (560 DIP).
    RECT rc{};
    GetClientRect(GetParent(ctl(window, IDC_INTENSITY)), &rc);
    return static_cast<UINT>(MulDiv(rc.right, 96, 560));
}

// Measures every visible label/button text against its control: reports clipped text.
int checkClipping(Tester& t, HWND window, const char* tag) {
    struct Ctx {
        Tester* t;
        const char* tag;
        int problems;
        int dpi;
    } ctx{&t, tag, 0, static_cast<int>(windowDpiScale(window))};
    EnumChildWindows(
        window,
        [](HWND child, LPARAM lp) -> BOOL {
            Ctx& c = *reinterpret_cast<Ctx*>(lp);
            if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) return TRUE;
            wchar_t cls[32] = {};
            GetClassNameW(child, cls, ARRAYSIZE(cls));
            const bool isStatic = lstrcmpiW(cls, L"Static") == 0;
            const bool isButton = lstrcmpiW(cls, L"Button") == 0;
            if (!isStatic && !isButton) return TRUE;
            const std::wstring text = textOf(child);
            if (text.empty()) return TRUE;
            bool hasProp = false;
            const int extra = textAllowance(child, &hasProp);
            if (hasProp && extra < 0) return TRUE;
            const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
            RECT client{};
            GetClientRect(child, &client);
            HDC dc = GetDC(child);
            HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0)));
            int need = 0, have = 0;
            bool clipped = false;
            const LONG_PTR type = style & SS_TYPEMASK;
            if (hasProp || (isStatic && (style & SS_CENTERIMAGE))) {
                if (isStatic && (style & SS_ENDELLIPSIS)) {
                    // truncation by design
                } else {
                    RECT r{0, 0, 0, 0};
                    DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                    need = r.right + MulDiv(hasProp ? extra : 0, c.dpi, 96);
                    have = client.right;
                    clipped = need > have || r.bottom > client.bottom + 1;
                }
            } else if (isStatic && type != SS_ICON) {
                RECT r{0, 0, client.right, 0};
                DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                need = r.bottom;
                have = client.bottom;
                clipped = need > have;
            }
            SelectObject(dc, old);
            ReleaseDC(child, dc);
            if (clipped) {
                ++c.problems;
                c.t->line("      clipped [%s] id=%d need=%d have=%d text=\"%s\"", c.tag, GetDlgCtrlID(child), need, have,
                          narrow(text.substr(0, 60)).c_str());
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.problems;
}

void saveIconPreviews(Tester& t, const std::wstring& dir) {
    const int sizes[] = {16, 20, 24, 32, 48, 256};
    bool allOk = true;
    const int cell = 280, sheetW = cell * 4, rowH[] = {40, 40, 44, 52, 68, 276};
    int sheetH = 0;
    for (int h : rowH) sheetH += h;
    MemBitmap sheet(sheetW, sheetH);
    fill(sheet.dc, 0, 0, sheetW / 2, sheetH, RGB(243, 243, 243));
    fill(sheet.dc, sheetW / 2, 0, sheetW / 2, sheetH, RGB(32, 32, 32));
    int y = 0;
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(sizes)); ++i) {
        for (int variant = 0; variant < 2; ++variant) {
            HICON icon = TrayIcon::createAppIcon(sizes[i], variant == 1);
            char what[96];
            snprintf(what, sizeof(what), "createAppIcon(%d, %s) returns an icon", sizes[i], variant ? "paused" : "normal");
            t.check(icon != nullptr, what);
            if (!icon) {
                allOk = false;
                continue;
            }
            DrawIconEx(sheet.dc, variant * cell + 10, y + 6, icon, sizes[i], sizes[i], 0, nullptr, DI_NORMAL);
            DrawIconEx(sheet.dc, sheetW / 2 + variant * cell + 10, y + 6, icon, sizes[i], sizes[i], 0, nullptr, DI_NORMAL);
            DestroyIcon(icon);
        }
        y += rowH[i];
    }
    allOk = sheet.save(dir + L"\\icon_preview_sheet.png") && allOk;
    t.check(allOk, "icon preview sheet saved (icon_preview_sheet.png)");
}

void countdownChecks(Tester& t) {
    t.line("-- New Year countdown");
    setLanguage(Language::Russian);
    auto at = [](int y, int m, int d, int h = 12, int mi = 0) {
        SYSTEMTIME st{};
        st.wYear = static_cast<WORD>(y);
        st.wMonth = static_cast<WORD>(m);
        st.wDay = static_cast<WORD>(d);
        st.wHour = static_cast<WORD>(h);
        st.wMinute = static_cast<WORD>(mi);
        return st;
    };
    Countdown c = computeCountdown(at(2026, 11, 12));
    t.check(c.big == L"50" && c.caption == L"дней до Нового года", "12 Nov 2026 -> 50 дней");
    c = computeCountdown(at(2026, 12, 30));
    t.check(c.big == L"2" && c.caption == L"дня до Нового года", "30 Dec -> 2 дня");
    c = computeCountdown(at(2026, 12, 10));
    t.check(c.big == L"22" && c.caption == L"дня до Нового года", "10 Dec -> 22 дня");
    c = computeCountdown(at(2026, 12, 20));
    t.check(c.big == L"12" && c.caption == L"дней до Нового года", "20 Dec -> 12 дней");
    c = computeCountdown(at(2026, 12, 31, 15, 30));
    t.check(c.big == L"9" && c.caption == L"часов до Нового года", "31 Dec 15:30 -> 9 часов");
    c = computeCountdown(at(2026, 12, 31, 23, 30));
    t.check(c.big == L"1" && c.caption == L"час до Нового года", "31 Dec 23:30 -> 1 час");
    c = computeCountdown(at(2027, 1, 3));
    t.check(c.festive && c.big == L"С Новым годом!", "3 Jan -> «С Новым годом!»");
    c = computeCountdown(at(2027, 1, 8));
    t.check(!c.festive && c.big == L"358", "8 Jan -> 358 days (not festive)");
    c = computeCountdown(at(2028, 2, 28));
    t.check(c.big == L"308", "leap year 28 Feb 2028 -> 308");
    setLanguage(Language::English);
    c = computeCountdown(at(2026, 12, 30));
    t.check(c.caption == L"days until New Year", "English plural");
    setLanguage(Language::Russian);
}

void quickPanelChecks(Tester& t, Demo& d, HINSTANCE inst, HWND owner) {
    t.line("-- quick panel");
    d.settings = Settings{};
    d.settings.system.language = Language::Russian;
    d.settings.userPresets.push_back({"Вечер", WeatherSettings{WeatherPreset::Light, 20, -30, 5, 80, 60, 70}, 12, 60});
    d.tray.create(inst, owner, kTrayCommandMsg);
    d.tray.onQuickChange = [&d](const Settings& s) {
        d.settings = s;
        ++d.quickChanges;
    };
    for (int theme = 0; theme < 3; ++theme) {
        d.settings.system.uiTheme = static_cast<UiTheme>(theme);
        d.tray.setSettings(d.settings);
        d.tray.setState(trayState(d));
        // The panel closes when it loses activation; on a desktop in use another window can take the
        // focus right away, so allow a couple of re-opens.
        HWND qp = nullptr;
        for (int attempt = 0; attempt < 3 && !(qp && IsWindowVisible(qp)); ++attempt) {
            d.tray.showQuickPanel();
            pump(d.window, 150);
            qp = findOwnWindow(L"SnowFallQuickPanel");
        }
        if (!qp || !IsWindowVisible(qp)) {
            t.check(false, "quick panel opens");
            return;
        }
        captureWindow(qp, d.exeDir + L"\\quick_panel_" + kThemeTag[theme] + L".png");
        t.check(checkClipping(t, qp, "quick") == 0, theme == 0 ? "quick panel: no clipped text (night)"
                                                    : theme == 1 ? "quick panel: no clipped text (globe)"
                                                                 : "quick panel: no clipped text (postcard)");
        if (theme < 2) {
            d.tray.hideQuickPanel();
            pump(d.window, 50);
        }
    }
    HWND qp = findOwnWindow(L"SnowFallQuickPanel");
    t.check(qp && IsWindowVisible(qp), "quick panel opens (tray.showQuickPanel)");
    t.check(SendMessageW(qp, uitest::kMsgQueryTimers, 0, 0) == 1, "quick panel animation timer runs while shown");
    RECT pr{};
    GetWindowRect(qp, &pr);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(qp, MONITOR_DEFAULTTONEAREST), &mi);
    t.check(pr.left >= mi.rcWork.left && pr.right <= mi.rcWork.right && pr.top >= mi.rcWork.top && pr.bottom <= mi.rcWork.bottom,
            "quick panel lies inside the work area");
    t.check((GetWindowLongPtrW(qp, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) == (WS_EX_TOOLWINDOW | WS_EX_TOPMOST),
            "quick panel is a topmost tool window");
    int q = d.quickChanges;
    HWND tb = GetDlgItem(qp, IDC_QP_INTENSITY);
    SendMessageW(tb, TBM_SETPOS, TRUE, 85);
    SendMessageW(qp, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, 85), reinterpret_cast<LPARAM>(tb));
    t.check(d.quickChanges == q + 1 && d.settings.weather.intensity == 85, "intensity slider -> onQuickChange(intensity 85)");
    q = d.quickChanges;
    tb = GetDlgItem(qp, IDC_QP_WIND);
    SendMessageW(tb, TBM_SETPOS, TRUE, -60);
    SendMessageW(qp, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, -60), reinterpret_cast<LPARAM>(tb));
    t.check(d.quickChanges == q + 1 && d.settings.weather.wind == -60 && textOf(GetDlgItem(qp, IDC_QP_WIND_VAL)) == L"← 60",
            "wind slider -> onQuickChange(wind -60)");
    q = d.quickChanges;
    SendMessageW(GetDlgItem(qp, IDC_QP_THAW), BM_CLICK, 0, 0);
    t.check(d.quickChanges == q + 1 && d.settings.weather.preset == WeatherPreset::Thaw, "thaw mini-card -> preset Thaw");
    q = d.quickChanges;
    HWND chips = GetDlgItem(qp, IDC_QP_PRESETS);
    RECT ir{};
    SendMessageW(chips, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&ir));
    const LPARAM pt = MAKELPARAM((ir.left + ir.right) / 2, (ir.top + ir.bottom) / 2);
    SendMessageW(chips, WM_LBUTTONDOWN, MK_LBUTTON, pt);
    SendMessageW(chips, WM_LBUTTONUP, 0, pt);
    t.check(d.quickChanges == q + 1 && d.settings.weather.intensity == 20 && d.settings.weather.preset == WeatherPreset::Light &&
                d.settings.accumulation.maxHeightPxAt96Dpi == 12,
            "user preset chip click applies the preset");
    d.commands.clear();
    SendMessageW(GetDlgItem(qp, IDC_QP_FIREWORKS), BM_CLICK, 0, 0);
    SendMessageW(GetDlgItem(qp, IDC_QP_PAUSE), BM_CLICK, 0, 0);
    pump(d.window, 50);
    bool fw = false, pause = false;
    for (auto& c : d.commands) {
        fw = fw || c.first == static_cast<int>(TrayCommand::CallFireworks);
        pause = pause || c.first == static_cast<int>(TrayCommand::TogglePause);
    }
    t.check(fw && pause, "«Салют!» / «Пауза» buttons post CallFireworks / TogglePause");
    t.check(textOf(GetDlgItem(qp, IDC_QP_PAUSE)) == tr(StrId::MenuResume), "pause button relabels to «Продолжить» after setState");
    t.check(textOf(GetDlgItem(qp, IDC_QP_HINT)).find(L"Ctrl+Alt") != std::wstring::npos, "quick panel shows the interaction keys");
    q = d.quickChanges;
    SendMessageW(GetDlgItem(qp, IDC_QP_TOOL_MITTEN), BM_CLICK, 0, 0);
    t.check(d.quickChanges == q + 1 && d.settings.interaction.tool == Tool::Mitten, "tool icon «Варежка» -> onQuickChange(tool Mitten)");
    q = d.quickChanges;
    SendMessageW(GetDlgItem(qp, IDC_QP_EXCL_AROUND), BM_CLICK, 0, 0);
    t.check(d.quickChanges == q + 1 && d.settings.system.excludedSnowMode == ExcludedSnowMode::Around,
            "«обходит их окна» -> onQuickChange(excludedSnowMode Around)");
    t.check(textOf(GetDlgItem(qp, IDC_QP_EXCL_LBL)) == L"Исключения: 0", "exclusions count shown");
    // Keyboard: Esc posted to the focused control goes through the panel's message hook.
    HWND focusTarget = GetDlgItem(qp, IDC_QP_LIGHT);
    PostMessageW(focusTarget, WM_KEYDOWN, VK_ESCAPE, 0);
    pump(d.window, 100);
    t.check(!IsWindowVisible(qp), "Esc closes the quick panel");
    t.check(SendMessageW(qp, uitest::kMsgQueryTimers, 0, 0) == 0, "quick panel timer stopped when hidden");
    // «Исключить приложение…» opens the topmost picker right from the panel.
    d.tray.setSettings(d.settings);
    d.tray.showQuickPanel();
    pump(d.window, 200);
    q = d.quickChanges;
    SendMessageW(GetDlgItem(qp, IDC_QP_EXCL_PICK), BM_CLICK, 0, 0);
    pump(d.window, 200);
    HWND picker = findOwnWindow(L"SnowFallProcessPicker");
    t.check(picker && IsWindowVisible(picker) && (GetWindowLongPtrW(picker, GWL_EXSTYLE) & WS_EX_TOPMOST),
            "quick panel opens the (topmost) process picker");
    if (picker) {
        HWND list = GetDlgItem(picker, IDC_PICK_LIST);
        // Our own process has no visible app window at this point: take the first listed program.
        wchar_t first[MAX_PATH] = {};
        const bool any = SendMessageW(list, LB_GETCOUNT, 0, 0) > 0;
        if (any) {
            SendMessageW(list, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(first));
            SendMessageW(list, LB_SETSEL, TRUE, 0);
            SendMessageW(picker, WM_COMMAND, MAKEWPARAM(IDC_PICK_LIST, LBN_SELCHANGE), reinterpret_cast<LPARAM>(list));
            SendMessageW(GetDlgItem(picker, IDC_PICK_ADD), BM_CLICK, 0, 0);
        } else {
            SendMessageW(picker, WM_CLOSE, 0, 0);
        }
        pump(d.window, 100);
        t.check(!any || (d.quickChanges == q + 1 && d.settings.system.excludedProcesses.size() == 1 &&
                         equalsNoCase(widen(d.settings.system.excludedProcesses[0]), first)),
                "picker «Добавить» -> onQuickChange with the new exclusion");
    }
    d.tray.hideQuickPanel();
    d.tray.remove();
    d.tray.onQuickChange = nullptr;
    d.paused = false;
}

// One full pass over the settings window in the given theme.
void settingsSuite(Tester& t, Demo& d, UiTheme theme) {
    const std::wstring tag = kThemeTag[static_cast<int>(theme)];
    t.prefix = "[" + narrow(tag) + "] ";
    t.line("== settings window, theme %s", narrow(tag).c_str());
    d.settings = Settings{};
    d.settings.system.language = Language::Russian;
    d.settings.system.uiTheme = theme;
    d.changedCount = d.clearCount = d.fireworksCount = 0;
    d.window.show(d.settings);
    HWND w = d.window.hwnd();
    t.check(d.window.isOpen() && w && IsWindowVisible(w), "show() creates a visible window");
    if (!w) return;
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    t.check(lstrcmpW(cls, L"SnowFallSettings") == 0 && textOf(w) == L"SnowFall — настройки", "class and title");
    t.check((GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0 && GetWindow(w, GW_OWNER) == nullptr,
            "normal application window (taskbar / Alt+Tab)");
    {
        RECT wr{};
        GetWindowRect(w, &wr);
        const UINT dpi = GetDpiForWindow(w);
        const int wd = MulDiv(wr.right - wr.left, 96, static_cast<int>(dpi)), hd = MulDiv(wr.bottom - wr.top, 96, static_cast<int>(dpi));
        t.line("      window size %d x %d DIP", wd, hd);
        t.check(wd <= 1270 && hd <= 688, "fits 1366x768 @100 % and 1920x1080 @150 % (incl. taskbar)");
    }
    pump(d.window, 300);
    t.check(d.changedCount == 0, "show() does not fire onChanged");
    t.check(SendMessageW(w, uitest::kMsgQueryTimers, 0, 0) == 3, "animation + diagnostics timers run while visible");

    for (int i = 0; i < kTabCount; ++i) {
        selectTab(w, i);
        pump(d.window, 250);
        captureWindow(w, d.exeDir + L"\\settings_" + tag + L"_tab_" + std::to_wstring(i + 1) + L".png");
        char what[64];
        snprintf(what, sizeof(what), "tab %d shown via navigation", i + 1);
        t.check(IsWindowVisible(GetParent(ctl(w, i == 0 ? IDC_INTENSITY : i == 1 ? IDC_ACC_ENABLED : i == 2 ? IDC_INTERACTION
                                                               : i == 3 ? IDC_ANIMATIONS : i == 4 ? IDC_GARLAND : i == 5 ? IDC_STARTUP
                                                                                                             : IDC_LOG_PATH))) != FALSE,
                what);
        if (checkClipping(t, w, "ru") != 0) t.check(false, "no clipped text (Russian)");
    }
    t.check(true, "screenshots saved (settings_<theme>_tab_N.png)");

    auto expectChange = [&](int& counter, const char* what, bool valueOk) {
        const bool fired = d.changedCount == counter + 1;
        counter = d.changedCount;
        char buf[200];
        snprintf(buf, sizeof(buf), "%s -> onChanged fired once with the expected value", what);
        t.check(fired && valueOk, buf);
    };
    int n = d.changedCount;

    t.line("-- header");
    t.check(textOf(ctl(w, IDC_HEADER)).find(L"Ctrl+Alt") != std::wstring::npos, "header hint shows the interaction keys");
    t.check(isChecked(w, IDC_HDR_TOOL_SNOWBALL), "header tool selector: Snowball by default");
    click(w, IDC_HDR_TOOL_MITTEN);
    expectChange(n, "header tool icon «Варежка»", d.settings.interaction.tool == Tool::Mitten);
    t.check(isChecked(w, IDC_TOOL_MITTEN) && !isChecked(w, IDC_HDR_TOOL_SNOWBALL), "«Инструменты» segments follow the header icons");

    t.line("-- tab 1 (snow)");
    selectTab(w, 0);
    click(w, IDC_PRESET_BLIZZARD);
    expectChange(n, "preset card Blizzard", d.settings.weather.preset == WeatherPreset::Blizzard);
    t.check(!isChecked(w, IDC_PRESET_NORMAL) && isChecked(w, IDC_PRESET_BLIZZARD), "card selection moved");
    click(w, IDC_PRESET_THAW);
    expectChange(n, "preset card Thaw", d.settings.weather.preset == WeatherPreset::Thaw);
    SetFocus(ctl(w, IDC_PRESET_LIGHT));
    SendMessageW(w, WM_COMMAND, IDOK, 0);   // Enter on a focused card
    expectChange(n, "Enter on the Light card", d.settings.weather.preset == WeatherPreset::Light);
    slide(w, IDC_INTENSITY, 80);
    expectChange(n, "intensity 80", d.settings.weather.intensity == 80);
    t.check(textOf(ctl(w, IDC_INTENSITY_VAL)) == L"80 %", "intensity value label");
    slide(w, IDC_WIND, -40);
    expectChange(n, "wind -40", d.settings.weather.wind == -40);
    t.check(textOf(ctl(w, IDC_WIND_VAL)) == L"← 40", "wind label shows the left arrow");
    slide(w, IDC_GUSTINESS, 65);
    expectChange(n, "gustiness 65", d.settings.weather.gustiness == 65);
    slide(w, IDC_FLAKE_SIZE, 150);
    expectChange(n, "size 150", d.settings.weather.sizePercent == 150);
    slide(w, IDC_FALLSPEED, 25);
    expectChange(n, "fall speed 25", d.settings.weather.fallSpeedPercent == 25);
    slide(w, IDC_OPACITY, 50);
    expectChange(n, "opacity 50", d.settings.weather.opacityPercent == 50);
    slide(w, IDC_OPACITY, 50);
    t.check(d.changedCount == n, "unchanged slider position does not fire onChanged");
    t.check(isChecked(w, IDC_SKY_PER_MONITOR) && textOf(ctl(w, IDC_SKY_PER_MONITOR_HINT)) == tr(StrId::SkyPerMonitorHint),
            "«Свой снегопад на каждом мониторе» on by default, with its hint");
    click(w, IDC_SKY_PER_MONITOR);
    expectChange(n, "skyPerMonitor off", !d.settings.weather.skyPerMonitor);

    // «Мои пресеты»
    click(w, IDC_UP_SAVE);
    t.check(IsWindowVisible(ctl(w, IDC_UP_NAME)) && !IsWindowVisible(ctl(w, IDC_UP_SAVE)), "«Сохранить текущий как…» shows the inline name editor");
    SetWindowTextW(ctl(w, IDC_UP_NAME), L"   ");
    click(w, IDC_UP_OK);
    t.check(d.changedCount == n && textOf(ctl(w, IDC_UP_MSG)) == tr(StrId::ErrPresetLength), "empty name -> inline error");
    SetWindowTextW(ctl(w, IDC_UP_NAME), L"Мягкий вечер");
    click(w, IDC_UP_OK);
    expectChange(n, "save preset «Мягкий вечер»",
                 d.settings.userPresets.size() == 1 && d.settings.userPresets[0].name == "Мягкий вечер" &&
                     d.settings.userPresets[0].weather.intensity == 80 && d.settings.userPresets[0].weather.wind == -40);
    t.check(textOf(ctl(w, IDC_UP_MSG)) == tr(StrId::PresetSaved) && SendMessageW(ctl(w, IDC_UP_LIST), LB_GETCOUNT, 0, 0) == 1,
            "chip appears, «Пресет сохранён»");
    click(w, IDC_UP_SAVE);
    SetWindowTextW(ctl(w, IDC_UP_NAME), L"мягкий ВЕЧЕР");
    click(w, IDC_UP_OK);
    t.check(d.changedCount == n && textOf(ctl(w, IDC_UP_MSG)) == tr(StrId::ErrPresetDuplicate), "duplicate (case-insensitive) -> error");
    SetWindowTextW(ctl(w, IDC_UP_NAME), std::wstring(41, L'я').c_str());
    click(w, IDC_UP_OK);
    t.check(d.changedCount == n && (textOf(ctl(w, IDC_UP_MSG)) == tr(StrId::ErrPresetLength) || textOf(ctl(w, IDC_UP_NAME)).size() == 40),
            "name longer than 40 chars rejected or truncated");
    {
        MSG esc{};
        esc.hwnd = ctl(w, IDC_UP_NAME);
        esc.message = WM_KEYDOWN;
        esc.wParam = VK_ESCAPE;
        t.check(d.window.preTranslateMessage(&esc) && !IsWindowVisible(ctl(w, IDC_UP_NAME)), "Esc cancels the name editor (window stays)");
    }
    slide(w, IDC_INTENSITY, 10);
    expectChange(n, "intensity 10", d.settings.weather.intensity == 10);
    {
        HWND chips = ctl(w, IDC_UP_LIST);
        RECT ir{};
        SendMessageW(chips, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&ir));
        LPARAM pt = MAKELPARAM(ir.left + 30, (ir.top + ir.bottom) / 2);
        SendMessageW(chips, WM_LBUTTONDOWN, MK_LBUTTON, pt);
        SendMessageW(chips, WM_LBUTTONUP, 0, pt);
        expectChange(n, "chip click applies the preset", d.settings.weather.intensity == 80 && sliderPos(w, IDC_INTENSITY) == 80);
        SendMessageW(chips, LB_SETCURSEL, 0, 0);
        SendMessageW(chips, WM_KEYDOWN, VK_DELETE, 0);
        expectChange(n, "Delete on a focused chip removes it", d.settings.userPresets.empty());
        // × zone
        Settings many = d.settings;
        many.userPresets.push_back({"A", many.weather, 36, 90});
        d.window.update(many);
        d.settings = many;
        SendMessageW(chips, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&ir));
        pt = MAKELPARAM(ir.right - 8, (ir.top + ir.bottom) / 2);
        SendMessageW(chips, WM_LBUTTONDOWN, MK_LBUTTON, pt);
        SendMessageW(chips, WM_LBUTTONUP, 0, pt);
        expectChange(n, "click on × removes the chip", d.settings.userPresets.empty());
        // limit of 20
        many = d.settings;
        for (int i = 0; i < 20; ++i) many.userPresets.push_back({"P" + std::to_string(i), many.weather, 36, 90});
        d.window.update(many);
        d.settings = many;
        click(w, IDC_UP_SAVE);
        SetWindowTextW(ctl(w, IDC_UP_NAME), L"Ещё один");
        click(w, IDC_UP_OK);
        t.check(d.changedCount == n && textOf(ctl(w, IDC_UP_MSG)) == tr(StrId::ErrPresetTooMany), "21st preset -> «не больше 20»");
        pump(d.window, 50);
        {   // exactly one navigation item looks selected (sampled near the item's left edge)
            const Capture cap = captureWindow(w, d.exeDir + L"\\settings_" + tag + L"_tab_1_presets.png");
            RECT wr{};
            GetWindowRect(w, &wr);
            const Palette& P = palette(theme);
            const int k = static_cast<int>(GetDpiForWindow(w)) / 96;
            int filled = 0;
            for (int i = 0; i < kTabCount; ++i) {
                RECT nr{};
                GetWindowRect(ctl(w, IDC_NAV_FIRST + i), &nr);
                const int sx = nr.left - wr.left + (P.pillTabs ? 8 : 12) * k, sy = (nr.top + nr.bottom) / 2 - wr.top;
                if (nearColor(cap.at(sx, sy), P.navSel, 12)) ++filled;
            }
            t.check(filled == 1 && isChecked(w, IDC_NAV_FIRST), "only the current page's navigation item is filled");
        }
        many.userPresets.clear();
        d.window.update(many);
        d.settings = many;
    }

    t.line("-- tab 2 (drifts)");
    selectTab(w, 1);
    click(w, IDC_ACC_TASKBAR);
    expectChange(n, "onTaskbar off", !d.settings.accumulation.onTaskbar);
    slide(w, IDC_MAXHEIGHT, 70);
    expectChange(n, "max height 70", d.settings.accumulation.maxHeightPxAt96Dpi == 70);
    click(w, IDC_SHAKE_KEEP);
    expectChange(n, "keep snow on move", !d.settings.accumulation.shakeOffOnMove);
    click(w, IDC_SKIP_MAXIMIZED);
    expectChange(n, "skipMaximized off", !d.settings.accumulation.skipMaximized);
    click(w, IDC_DROP_ON_CLOSE);
    expectChange(n, "dropSnowOnClose off", !d.settings.accumulation.dropSnowOnClose);
    slide(w, IDC_DRIFT_OPACITY, 40);
    expectChange(n, "drift opacity 40", d.settings.accumulation.driftOpacityPercent == 40);
    {   // Real mouse drag of a skinned trackbar: no ghost thumbs, cheap per-tick handling.
        pump(d.window, 300);
        HWND tb = ctl(w, IDC_DRIFT_OPACITY);
        RECT th{};
        SendMessageW(tb, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&th));
        RECT ch{};
        SendMessageW(tb, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&ch));
        const int cx = (th.left + th.right) / 2, cy = (th.top + th.bottom) / 2;
        const int before = d.changedCount;
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        double maxMs = 0, sumMs = 0;
        SendMessageW(tb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
        int steps = 0;
        for (int x = cx; x >= ch.left; x -= 3) {
            QueryPerformanceCounter(&a);
            SendMessageW(tb, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, cy));
            QueryPerformanceCounter(&b);
            const double ms = static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
            maxMs = std::max(maxMs, ms);
            sumMs += ms;
            ++steps;
            if (steps % 8 == 0) UpdateWindow(w);   // no message pump: real mouse moves would reach the captured trackbar
        }
        UpdateWindow(w);   // still dragging: this is when a ghost trail would be visible
        // Read back what the trackbar itself currently shows: its own window DC (the DWM
        // redirection surface of OUR window only — independent of other windows on screen;
        // PrintWindow would re-render everything and hide ghosts).
        RECT tc{};
        GetClientRect(tb, &tc);
        Capture cap;
        {
            MemBitmap mem(tc.right, tc.bottom);
            HDC tdc = GetDC(tb);
            BitBlt(mem.dc, 0, 0, mem.w, mem.h, tdc, 0, 0, SRCCOPY);
            ReleaseDC(tb, tdc);
            cap.w = mem.w;
            cap.h = mem.h;
            cap.px = mem.pixels();
        }
        SendMessageW(tb, WM_LBUTTONUP, 0, MAKELPARAM(ch.left, cy));
        pump(d.window, 120);
        t.line("      drag: %d mouse moves, %d onChanged, avg %.2f / max %.2f ms per move (paint + onChanged), value %d", steps,
               d.changedCount - before, sumMs / std::max(1, steps), maxMs, d.settings.accumulation.driftOpacityPercent);
        t.check(d.changedCount > before + 5 && d.settings.accumulation.driftOpacityPercent == sliderPos(w, IDC_DRIFT_OPACITY) &&
                    d.settings.accumulation.driftOpacityPercent <= 15,
                "mouse drag fires onChanged on the way and ends at the left");
        t.check(sumMs / std::max(1, steps) < 4.0 && maxMs < 16.0, "per-move handling: average < 4 ms, never a dropped frame");
        const Palette& P = palette(theme);
        int ghost = 0;
        // The drawn thumb (circle + focus ring) is wider than the control's own thumb rect, so
        // look at the whole area it covered.
        const int kd = MulDiv(13, static_cast<int>(GetDpiForWindow(w)), 96);
        for (int y = cy - kd; y <= cy + kd; ++y)
            for (int x = cx - kd; x <= cx + kd; ++x)
                if (nearColor(cap.at(x, y), P.accent, 20)) ++ghost;
        t.line("      accent-coloured pixels left at the old thumb position: %d", ghost);
        t.check(ghost == 0, "no ghost thumb after dragging left");
        n = d.changedCount;
    }
    click(w, IDC_SHAKE_TO_CLEAR);
    expectChange(n, "shakeToClear on", d.settings.accumulation.shakeToClear);
    click(w, IDC_NATURAL_CAPS);
    expectChange(n, "naturalCaps off", !d.settings.accumulation.naturalCaps);
    click(w, IDC_ICICLES);
    expectChange(n, "icicles off", !d.settings.accumulation.icicles);
    t.check(isChecked(w, IDC_SNOWMEN_HEAVY) && sliderPos(w, IDC_SNOWMEN_INTERVAL) == 2 && sliderPos(w, IDC_SNOWMEN_MAX) == 3 &&
                textOf(ctl(w, IDC_SNOWMEN_INTERVAL_VAL)) == L"раз в 2 минуты" && textOf(ctl(w, IDC_SNOWMEN_MAX_VAL)) == L"3 снеговика",
            "falling-snowmen controls show the defaults");
    slide(w, IDC_SNOWMEN_INTERVAL, 1);
    expectChange(n, "snowmen interval 1", d.settings.accumulation.snowmenIntervalMinutes == 1);
    t.check(textOf(ctl(w, IDC_SNOWMEN_INTERVAL_VAL)) == L"раз в минуту", "«раз в минуту»");
    slide(w, IDC_SNOWMEN_INTERVAL, 21);
    expectChange(n, "snowmen interval 21", d.settings.accumulation.snowmenIntervalMinutes == 21);
    t.check(textOf(ctl(w, IDC_SNOWMEN_INTERVAL_VAL)) == L"раз в 21 минуту", "«раз в 21 минуту»");
    slide(w, IDC_SNOWMEN_INTERVAL, 15);
    expectChange(n, "snowmen interval 15", d.settings.accumulation.snowmenIntervalMinutes == 15);
    t.check(textOf(ctl(w, IDC_SNOWMEN_INTERVAL_VAL)) == L"раз в 15 минут", "«раз в 15 минут»");
    click(w, IDC_SNOWMEN_HEAVY);
    expectChange(n, "snowmenNeedHeavySnow off", !d.settings.accumulation.snowmenNeedHeavySnow);
    slide(w, IDC_SNOWMEN_MAX, 7);
    expectChange(n, "snowmen max 7", d.settings.accumulation.snowmenMaxAlive == 7);
    t.check(textOf(ctl(w, IDC_SNOWMEN_MAX_VAL)) == L"7 снеговиков", "«7 снеговиков»");
    click(w, IDC_FALLING_SNOWMEN);
    expectChange(n, "fallingSnowmen off", !d.settings.accumulation.fallingSnowmen);
    t.check(!IsWindowEnabled(ctl(w, IDC_SNOWMEN_INTERVAL)) && !IsWindowEnabled(ctl(w, IDC_SNOWMEN_HEAVY)) &&
                !IsWindowEnabled(ctl(w, IDC_SNOWMEN_MAX)),
            "snowmen sub-controls disabled when falling snowmen are off");
    slide(w, IDC_SNOWPLOW_DELAY, 45);
    expectChange(n, "snowplow delay 45", d.settings.accumulation.snowplowDelayMinutes == 45);
    t.check(textOf(ctl(w, IDC_SNOWPLOW_DELAY_VAL)) == L"45 мин", "snowplow delay value label");
    t.check(isChecked(w, IDC_SNOWPLOW_ALL) && textOf(ctl(w, IDC_SNOWPLOW_ALL_HINT)) == tr(StrId::SnowplowAllTaskbarsHint),
            "«На каждой панели задач» on by default, with its hint");
    click(w, IDC_SNOWPLOW_ALL);
    expectChange(n, "snowplowAllTaskbars off", !d.settings.accumulation.snowplowAllTaskbars);
    click(w, IDC_SNOWPLOW);
    expectChange(n, "snowplow off", !d.settings.accumulation.snowplow);
    t.check(!IsWindowEnabled(ctl(w, IDC_SNOWPLOW_ALL)) && !IsWindowEnabled(ctl(w, IDC_SNOWPLOW_ALL_HINT)),
            "«На каждой панели задач» disabled when the snowplow is off");
    t.check(!IsWindowEnabled(ctl(w, IDC_SNOWPLOW_DELAY)), "snowplow delay disabled when the snowplow is off");
    click(w, IDC_SNOWPLOW);
    expectChange(n, "snowplow on", d.settings.accumulation.snowplow);
    click(w, IDC_CLEAR_ALL);
    t.check(d.clearCount == 1 && d.changedCount == n, "«Очистить всё» calls onClearSnow only");
    click(w, IDC_ACC_ENABLED);
    expectChange(n, "accumulation off", !d.settings.accumulation.enabled);
    t.check(!IsWindowEnabled(ctl(w, IDC_ACC_WINDOWS)) && !IsWindowEnabled(ctl(w, IDC_ICICLES)), "dependent controls disabled");
    click(w, IDC_ACC_ENABLED);
    expectChange(n, "accumulation on", d.settings.accumulation.enabled);

    t.line("-- tab 3 (tools)");
    selectTab(w, 2);
    click(w, IDC_TOOL_SHOVEL);
    expectChange(n, "tool shovel", d.settings.interaction.tool == Tool::Shovel);
    t.check(isChecked(w, IDC_HDR_TOOL_SHOVEL), "header icons follow the segments");
    {
        const std::wstring tip = textOf(ctl(w, IDC_TOOL_TIP));
        t.check(tip.find(L"Ctrl+Alt") != std::wstring::npos && tip.find(tr(StrId::TipShovel)) != std::wstring::npos,
                "tool help shows the keys and the shovel tips");
    }
    click(w, IDC_SKIN_FIRST + 3);
    expectChange(n, "snowman skin #3", d.settings.interaction.snowmanSkin == kSnowmanSkins[2].id);
    click(w, IDC_SKIN_FIRST);
    expectChange(n, "snowman skin «Случайный»", d.settings.interaction.snowmanSkin == "random");
    slide(w, IDC_SHOVEL_WIDTH, 120);
    expectChange(n, "shovel width 120", d.settings.interaction.shovelWidthPxAt96Dpi == 120);
    slide(w, IDC_MITTEN_RADIUS, 40);
    expectChange(n, "mitten radius 40", d.settings.interaction.brushRadiusPxAt96Dpi == 40);
    click(w, IDC_HOLD_KEY);
    expectChange(n, "hold key off", !d.settings.interaction.holdKeyEnabled);
    click(w, IDC_BLOCK_CLICKS);
    expectChange(n, "blockClicksWhileHeld off", !d.settings.interaction.blockClicksWhileHeld);
    click(w, IDC_ALL_MONITORS);
    expectChange(n, "objectsOnAllMonitors on", d.settings.interaction.objectsOnAllMonitors);
    click(w, IDC_UMBRELLA);
    expectChange(n, "umbrellaCursor off", !d.settings.interaction.umbrellaCursor);
    click(w, IDC_INTERACTION);
    expectChange(n, "interaction off", !d.settings.interaction.enabled);
    t.check(!IsWindowEnabled(ctl(w, IDC_BLOCK_CLICKS)) && !IsWindowEnabled(ctl(w, IDC_TOOL_MITTEN)), "tool controls disabled");
    click(w, IDC_INTERACTION);
    expectChange(n, "interaction on", d.settings.interaction.enabled);

    t.line("-- tab 4 (performance)");
    selectTab(w, 3);
    click(w, IDC_PROFILE_HIGH);
    expectChange(n, "profile high", d.settings.performance.profile == PerfProfile::High);
    click(w, IDC_FPS_30);
    expectChange(n, "fps 30", d.settings.performance.fpsCap == 30);
    click(w, IDC_PAUSE_FULLSCREEN);
    expectChange(n, "pauseOnFullscreen off", !d.settings.performance.pauseOnFullscreen);
    click(w, IDC_REDUCE_BATTERY);
    expectChange(n, "reduceOnBattery off", !d.settings.performance.reduceOnBattery);
    click(w, IDC_ANIMATIONS);
    expectChange(n, "animations off", !d.settings.performance.animations);
    click(w, IDC_ANIMATIONS);
    expectChange(n, "animations on", d.settings.performance.animations);
    t.check(textOf(ctl(w, IDC_DIAG_PARTICLES)) == L"1840" && textOf(ctl(w, IDC_DIAG_BALLS)) == L"3 / 1", "diagnostics values");
    {
        const int before = d.diagCalls;
        pump(d.window, 1300);
        t.check(d.diagCalls - before >= 2, "diagnostics refreshed by the 500 ms timer");
    }

    t.line("-- tab 5 (festive)");
    selectTab(w, 4);
    click(w, IDC_GARLAND);
    expectChange(n, "garland on (default off)", d.settings.festive.garland);
    click(w, IDC_FIREWORKS);
    expectChange(n, "fireworks off", !d.settings.festive.fireworks);
    click(w, IDC_FIREWORKS_NOW);
    t.check(d.fireworksCount == 1 && d.changedCount == n, "«Салют!» calls onFireworks only");

    t.line("-- tab 6 (system)");
    selectTab(w, 5);
    click(w, IDC_STARTUP);
    expectChange(n, "startupWithWindows on", d.settings.startupWithWindows);
    click(w, IDC_LEFT_CLICK);
    expectChange(n, "trayLeftClickTogglesPause on", d.settings.system.trayLeftClickTogglesPause);
    click(w, IDC_LANG_EN);
    expectChange(n, "language English", d.settings.system.language == Language::English);
    t.check(textOf(ctl(w, IDC_CLOSE)) == L"Close" && textOf(w) == L"SnowFall — Settings", "UI re-labelled immediately");
    pump(d.window, 150);
    for (int i = 0; i < kTabCount; ++i) {
        selectTab(w, i);
        pump(d.window, 350);   // let button state transitions finish before the capture
        if (checkClipping(t, w, "en") != 0) t.check(false, "no clipped text (English)");
        captureWindow(w, d.exeDir + L"\\settings_" + tag + L"_tab_" + std::to_wstring(i + 1) + L"_en.png");
    }
    t.check(true, "English pass done (settings_<theme>_tab_N_en.png)");
    selectTab(w, 5);
    click(w, IDC_LANG_RU);
    expectChange(n, "language Russian", d.settings.system.language == Language::Russian);
    t.check(textOf(ctl(w, IDC_CLOSE)) == L"Закрыть", "re-labelled back to Russian");

    HWND hkPause = ctl(w, IDC_HOTKEY_PAUSE);
    HWND hkClear = ctl(w, IDC_HOTKEY_CLEAR);
    t.check(isHotkeyEdit(hkPause) && getHotkeyEditText(hkPause) == "Ctrl+Alt+S", "hotkey field shows the default Ctrl+Alt+S");
    SetFocus(hkPause);
    setHotkeyModifierOverride(MOD_CONTROL | MOD_ALT);
    SendMessageW(hkPause, WM_KEYDOWN, VK_CONTROL, 0);
    t.check(d.changedCount == n, "modifier keys alone change nothing");
    SendMessageW(hkPause, WM_KEYDOWN, 'P', 0);
    expectChange(n, "WM_KEYDOWN P with Ctrl+Alt", d.settings.hotkeys.pauseToggle == "Ctrl+Alt+P");
    SendMessageW(hkPause, WM_KEYDOWN, 'S', 0);
    expectChange(n, "WM_KEYDOWN S with Ctrl+Alt", d.settings.hotkeys.pauseToggle == "Ctrl+Alt+S");
    t.check(textOf(hkPause) == L"Ctrl+Alt+S", "capture field text is \"Ctrl+Alt+S\"");
    {
        MSG msg{};
        msg.hwnd = hkPause;
        msg.message = WM_KEYDOWN;
        msg.wParam = 'K';
        const bool consumed = d.window.preTranslateMessage(&msg);
        expectChange(n, "key routed by preTranslateMessage", consumed && d.settings.hotkeys.pauseToggle == "Ctrl+Alt+K");
    }
    setHotkeyModifierOverride(MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
    SendMessageW(hkPause, WM_SYSKEYDOWN, VK_F5, 0);
    expectChange(n, "Ctrl+Alt+Shift+Win+F5", d.settings.hotkeys.pauseToggle == "Ctrl+Alt+Shift+Win+F5");
    setHotkeyModifierOverride(0);
    SendMessageW(hkPause, WM_KEYDOWN, VK_BACK, 0);
    expectChange(n, "Backspace clears the hotkey", d.settings.hotkeys.pauseToggle.empty());
    t.check(textOf(ctl(w, IDC_HOTKEY_PAUSE_STATUS)) == tr(StrId::HotkeyDisabled), "status label: disabled");
    t.check(hotkeyEditHandleKey(hkClear, VK_F9, MOD_CONTROL | MOD_ALT), "seam: Ctrl+Alt+F9 captured");
    expectChange(n, "clear hotkey Ctrl+Alt+F9", d.settings.hotkeys.clearSnow == "Ctrl+Alt+F9");
    t.check(textOf(ctl(w, IDC_HOTKEY_CLEAR_STATUS)) == tr(StrId::HotkeyInUse), "status label shows the conflict text");
    setHotkeyModifierOverride(-1);

    HWND exclEdit = ctl(w, IDC_EXCL_EDIT);
    SetWindowTextW(exclEdit, L"C:\\Games\\Game.EXE");
    click(w, IDC_EXCL_ADD);
    expectChange(n, "add exclusion", d.settings.system.excludedProcesses.size() == 1 && d.settings.system.excludedProcesses[0] == "Game.EXE");
    SetWindowTextW(exclEdit, L"game.exe");
    click(w, IDC_EXCL_ADD);
    t.check(d.changedCount == n, "duplicate exclusion rejected");
    click(w, IDC_EXCL_AROUND);
    expectChange(n, "excluded apps: snow flows around", d.settings.system.excludedSnowMode == ExcludedSnowMode::Around);
    // Picker
    click(w, IDC_EXCL_PICK);
    pump(d.window, 200);
    HWND picker = findOwnWindow(L"SnowFallProcessPicker");
    t.check(picker && IsWindowVisible(picker), "«Выбрать из запущенных…» opens the picker");
    if (picker) {
        captureWindow(picker, d.exeDir + L"\\picker_" + tag + L".png");
        HWND list = GetDlgItem(picker, IDC_PICK_LIST);
        const LRESULT idx = SendMessageW(list, LB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(L"ui_demo.exe"));
        t.check(idx != LB_ERR, "picker lists ui_demo.exe (own process)");
        t.line("      picker lists %d programs", static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0)));
        if (idx != LB_ERR) {
            SendMessageW(list, LB_SETSEL, TRUE, idx);
            SendMessageW(picker, WM_COMMAND, MAKEWPARAM(IDC_PICK_LIST, LBN_SELCHANGE), reinterpret_cast<LPARAM>(list));
            SendMessageW(GetDlgItem(picker, IDC_PICK_ADD), BM_CLICK, 0, 0);
            pump(d.window, 50);
            expectChange(n, "picker «Добавить» adds ui_demo.exe",
                         d.settings.system.excludedProcesses.size() == 2 && d.settings.system.excludedProcesses[1] == "ui_demo.exe");
            t.check(!IsWindow(picker), "picker closes after adding");
        }
    }
    HWND list = ctl(w, IDC_EXCL_LIST);
    SendMessageW(list, LB_SETCURSEL, 0, 0);
    SendMessageW(GetParent(list), WM_COMMAND, MAKEWPARAM(IDC_EXCL_LIST, LBN_SELCHANGE), reinterpret_cast<LPARAM>(list));
    click(w, IDC_EXCL_REMOVE);
    expectChange(n, "remove exclusion", d.settings.system.excludedProcesses.size() == 1);

    t.line("-- tab 7 (about)");
    selectTab(w, 6);
    t.check(textOf(ctl(w, IDC_ABOUT_VERSION)).find(L"" SNOW_VERSION) != std::wstring::npos, "version shown");
    t.check(textOf(ctl(w, IDC_LOG_PATH)).find(L"ui_demo.log") != std::wstring::npos, "log path shown");
    {
        HWND gh = ctl(w, IDC_LINK_GITHUB), tg = ctl(w, IDC_LINK_TELEGRAM);
        const wchar_t* u1 = uitest::linkUrl(gh);
        const wchar_t* u2 = uitest::linkUrl(tg);
        t.check(textOf(ctl(w, IDC_POWERED_BY)) == L"Powered by DaveBugg", "«Powered by DaveBugg» next to the app name");
        t.check(gh && textOf(gh) == L"GitHub" && u1 && std::wstring(u1) == L"https://github.com/DaveBugg" &&
                    (GetWindowLongPtrW(gh, GWL_STYLE) & WS_TABSTOP),
                "GitHub link: name, URL, keyboard focusable");
        t.check(tg && textOf(tg) == L"Telegram" && u2 && std::wstring(u2) == L"https://t.me/dave_cli" &&
                    (GetWindowLongPtrW(tg, GWL_STYLE) & WS_TABSTOP),
                "Telegram link: name, URL, keyboard focusable");
    }

    t.line("-- update()");
    Settings pushed = d.settings;
    pushed.weather.intensity = 33;
    pushed.weather.wind = -7;
    pushed.weather.preset = WeatherPreset::Thaw;
    pushed.accumulation.onWindows = false;
    pushed.accumulation.icicles = true;
    pushed.interaction.tool = Tool::Snowball;
    pushed.interaction.umbrellaCursor = true;
    pushed.interaction.snowmanSkin = "king";
    pushed.weather.skyPerMonitor = true;
    pushed.accumulation.snowplowAllTaskbars = true;
    pushed.accumulation.fallingSnowmen = true;
    pushed.accumulation.snowmenIntervalMinutes = 33;
    pushed.accumulation.snowmenNeedHeavySnow = true;
    pushed.accumulation.snowmenMaxAlive = 9;
    pushed.system.excludedSnowMode = ExcludedSnowMode::Behind;
    pushed.festive.garland = true;
    pushed.performance.fpsCap = 60;
    pushed.hotkeys.interactionToggle = "Ctrl+Shift+F2";
    pushed.system.excludedProcesses = {"a.exe", "b.exe", "c.exe"};
    pushed.accumulation.driftOpacityPercent = 77;
    pushed.accumulation.snowplowDelayMinutes = 99;
    pushed.userPresets = {{"Один", pushed.weather, 30, 80}, {"Два", pushed.weather, 30, 80}};
    const int beforeUpdate = d.changedCount;
    d.window.update(pushed);
    pump(d.window, 50);
    t.check(d.changedCount == beforeUpdate, "update() does not fire onChanged");
    t.check(sliderPos(w, IDC_INTENSITY) == 33 && sliderPos(w, IDC_WIND) == -7 && sliderPos(w, IDC_DRIFT_OPACITY) == 77 &&
                sliderPos(w, IDC_SNOWPLOW_DELAY) == 99,
            "sliders reflect pushed values");
    t.check(isChecked(w, IDC_PRESET_THAW) && !isChecked(w, IDC_ACC_WINDOWS) && isChecked(w, IDC_ICICLES) && isChecked(w, IDC_UMBRELLA) &&
                isChecked(w, IDC_GARLAND) && isChecked(w, IDC_TOOL_SNOWBALL) && isChecked(w, IDC_FPS_60),
            "cards / toggles / segments reflect pushed values");
    {
        int king = 0;
        for (int i = 0; i < kSnowmanSkinCount; ++i)
            if (std::string(kSnowmanSkins[i].id) == "king") king = i + 1;
        t.check(isChecked(w, IDC_SKIN_FIRST + king) && isChecked(w, IDC_EXCL_BEHIND) && isChecked(w, IDC_HDR_TOOL_SNOWBALL),
                "skin / exclusion mode / header tool reflect pushed values");
        t.check(sliderPos(w, IDC_SNOWMEN_INTERVAL) == 33 && sliderPos(w, IDC_SNOWMEN_MAX) == 9 && isChecked(w, IDC_SNOWMEN_HEAVY) &&
                    IsWindowEnabled(ctl(w, IDC_SNOWMEN_MAX)) && textOf(ctl(w, IDC_SNOWMEN_INTERVAL_VAL)) == L"раз в 33 минуты",
                "falling-snowmen controls reflect pushed values");
        t.check(isChecked(w, IDC_SKY_PER_MONITOR), "skyPerMonitor reflects the pushed value");
        t.check(isChecked(w, IDC_SNOWPLOW_ALL), "snowplowAllTaskbars reflects the pushed value");
    }
    t.check(getHotkeyEditText(ctl(w, IDC_HOTKEY_INTERACTION)) == "Ctrl+Shift+F2" &&
                SendMessageW(ctl(w, IDC_EXCL_LIST), LB_GETCOUNT, 0, 0) == 3 && SendMessageW(ctl(w, IDC_UP_LIST), LB_GETCOUNT, 0, 0) == 2,
            "hotkey, exclusions, preset chips reflect pushed values");
    d.settings = pushed;
    n = d.changedCount;

    t.line("-- restore defaults");
    SendMessageW(w, uitest::kMsgRestoreDefaultsNoConfirm, 0, 0);
    Settings expected{};
    expected.userPresets = pushed.userPresets;
    expectChange(n, "restore defaults (user presets kept)", d.settings == expected);
    t.check(sliderPos(w, IDC_INTENSITY) == 55 && isChecked(w, IDC_PRESET_NORMAL) && isChecked(w, IDC_LANG_AUTO) &&
                isChecked(w, IDC_THEME_NIGHT) && isChecked(w, IDC_SWATCH_NIGHT),
            "controls show the defaults (theme back to «Зимняя ночь»)");

    t.line("-- keyboard");
    {
        selectTab(w, 0);
        HWND nav0 = ctl(w, IDC_NAV_FIRST);
        SetFocus(nav0);
        MSG msg{};
        msg.hwnd = nav0;
        msg.message = WM_KEYDOWN;
        msg.wParam = VK_TAB;
        const bool consumed = d.window.preTranslateMessage(&msg);
        HWND focus = GetFocus();
        t.check(consumed && focus != nav0 && IsChild(w, focus), "Tab moves the focus to the next control");
        MSG down{};
        down.hwnd = nav0;
        SetFocus(nav0);
        down.hwnd = nav0;
        down.message = WM_KEYDOWN;
        down.wParam = VK_DOWN;
        d.window.preTranslateMessage(&down);
        pump(d.window, 30);
        t.check(isChecked(w, IDC_NAV_FIRST + 1) && IsWindowVisible(GetParent(ctl(w, IDC_ACC_ENABLED))), "Down arrow on navigation switches page");
    }
    d.window.close();
    t.check(!d.window.isOpen() && d.window.hwnd() == nullptr, "close() destroys the window");
    t.prefix.clear();
}

// Re-colour, preview, timers, CPU — on a fresh window.
void themeAndPreviewChecks(Tester& t, Demo& d) {
    t.line("== theme switch, live preview, timers, CPU");
    d.settings = Settings{};
    d.settings.system.language = Language::Russian;
    d.window.show(d.settings);
    HWND w = d.window.hwnd();
    pump(d.window, 300);
    auto samplePoint = [&](int cx, int cy) {
        POINT p{cx, cy};
        ClientToScreen(w, &p);
        RECT wr{};
        GetWindowRect(w, &wr);
        const Capture c = captureWindow(w);
        return c.at(p.x - wr.left, p.y - wr.top);
    };
    RECT cr{};
    GetClientRect(w, &cr);
    const int sx = 4, sy = cr.bottom - 4;
    t.check(nearColor(samplePoint(sx, sy), palette(UiTheme::Night).nav), "Night: navigation column painted #081222");
    int n = d.changedCount;
    click(w, IDC_SWATCH_GLOBE);
    pump(d.window, 100);
    t.check(d.changedCount == n + 1 && d.settings.system.uiTheme == UiTheme::Globe, "header swatch «Снежный шар» -> onChanged(uiTheme Globe)");
    GetClientRect(w, &cr);
    t.check(nearColor(samplePoint(4, cr.bottom - 4), palette(UiTheme::Globe).bg), "Globe: window re-coloured #E6F1FB");
    t.check(isChecked(w, IDC_THEME_GLOBE), "«Система» segments follow the swatch");
    n = d.changedCount;
    click(w, IDC_NAV_FIRST + 5);
    click(w, IDC_THEME_POSTCARD);
    pump(d.window, 100);
    GetClientRect(w, &cr);
    t.check(d.changedCount == n + 1 && d.settings.system.uiTheme == UiTheme::Postcard && isChecked(w, IDC_SWATCH_POSTCARD),
            "«Система» segment «Новогодняя открытка» -> onChanged(uiTheme Postcard)");
    t.check(nearColor(samplePoint(4, cr.bottom - 4), palette(UiTheme::Postcard).bg), "Postcard: window re-coloured #FAECE7");
    click(w, IDC_SWATCH_NIGHT);
    pump(d.window, 100);

    // Live preview: drifts appear.
    click(w, IDC_NAV_FIRST);
    click(w, IDC_PREVIEW_RESET);
    pump(d.window, 100);
    HWND pv = ctl(w, IDC_PREVIEW);
    auto countSnow = [&]() {
        RECT pr{}, wr{};
        GetWindowRect(pv, &pr);
        GetWindowRect(w, &wr);
        const Capture c = captureWindow(w);
        int cnt = 0;
        for (int y = pr.top - wr.top; y < pr.bottom - wr.top; ++y)
            for (int x = pr.left - wr.left; x < pr.right - wr.left; ++x) {
                const unsigned p = c.at(x, y);
                if (((p >> 16) & 0xFF) >= 215 && ((p >> 8) & 0xFF) >= 215 && (p & 0xFF) >= 215) ++cnt;
            }
        return cnt;
    };
    const int before = countSnow();
    const LRESULT mass = SendMessageW(w, uitest::kMsgPreviewAdvance, 240, 0);
    pump(d.window, 60);
    const int after = countSnow();
    t.line("      preview: snow pixels %d -> %d, mass %lld px^2", before, after, static_cast<long long>(mass));
    t.check(mass > 0 && after > before + 150, "preview World advances and shows drifts");
    captureWindow(w, d.exeDir + L"\\settings_night_preview_drifts.png");
    click(w, IDC_PREVIEW_RESET);
    t.check(SendMessageW(w, uitest::kMsgPreviewAdvance, 0, 0) < mass, "«Сбросить превью» starts over (warm-up only)");

    // Timers.
    t.check(SendMessageW(w, uitest::kMsgQueryTimers, 0, 0) == 3, "timers run while visible");
    // CPU while visible (snow page: header + 4 cards + preview + countdown animate).
    FILETIME c0, e0, k0, u0, k1, u1;
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
    const ULONGLONG t0 = GetTickCount64();
    pump(d.window, 4000);
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k1, &u1);
    auto ft = [](const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    const double busy = static_cast<double>((ft(k1) - ft(k0)) + (ft(u1) - ft(u0))) / 10000.0;
    const double cpuVisible = 100.0 * busy / static_cast<double>(GetTickCount64() - t0);
    t.line("      CPU (window visible, «Снег» page, 20 fps animation): %.2f %% of one core", cpuVisible);
    t.check(cpuVisible < 10.0, "CPU while visible stays modest");
    ShowWindow(w, SW_MINIMIZE);
    pump(d.window, 100);
    t.check(SendMessageW(w, uitest::kMsgQueryTimers, 0, 0) == 0, "minimized: zero timers");
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
    const ULONGLONG t1 = GetTickCount64();
    pump(d.window, 2000);
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k1, &u1);
    const double busy2 = static_cast<double>((ft(k1) - ft(k0)) + (ft(u1) - ft(u0))) / 10000.0;
    t.line("      CPU (minimized): %.2f %% of one core", 100.0 * busy2 / static_cast<double>(GetTickCount64() - t1));
    ShowWindow(w, SW_RESTORE);
    pump(d.window, 100);
    t.check(SendMessageW(w, uitest::kMsgQueryTimers, 0, 0) == 3, "restored: timers back");
    ShowWindow(w, SW_HIDE);
    pump(d.window, 50);
    t.check(SendMessageW(w, uitest::kMsgQueryTimers, 0, 0) == 0, "hidden: zero timers");
    SendMessageW(w, WM_CLOSE, 0, 0);
    pump(d.window, 50);
    t.check(!d.window.isOpen(), "WM_CLOSE closes without quitting");
    MSG quit{};
    t.check(PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE) == FALSE, "no WM_QUIT posted by closing");
}

void dpiChecks(Tester& t, Demo& d) {
    t.line("== 150 %% layout (--dpi 144 seam)");
    uitest::setDpiOverride(144);
    for (int theme = 0; theme < 3; ++theme) {
        d.settings = Settings{};
        d.settings.system.language = Language::Russian;
        d.settings.system.uiTheme = static_cast<UiTheme>(theme);
        d.window.show(d.settings);
        HWND w = d.window.hwnd();
        int problems = 0;
        for (int i = 0; i < kTabCount; ++i) {
            click(w, IDC_NAV_FIRST + i);
            pump(d.window, 350);
            problems += checkClipping(t, w, "ru@144");
            if (i == 0 || i == 1) {
                captureWindow(w, d.exeDir + L"\\settings_" + kThemeTag[theme] + L"_tab_" + std::to_wstring(i + 1) + L"_dpi144.png");
            }
        }
        click(w, IDC_NAV_FIRST + 5);
        click(w, IDC_LANG_EN);
        for (int i = 0; i < kTabCount; ++i) {
            click(w, IDC_NAV_FIRST + i);
            pump(d.window, 60);
            problems += checkClipping(t, w, "en@144");
        }
        char what[96];
        snprintf(what, sizeof(what), "no clipped text at 144 DPI, RU+EN (%s)", narrow(kThemeTag[theme]).c_str());
        t.check(problems == 0, what);
        d.window.close();
    }
    uitest::setDpiOverride(0);
}

int runSelfTest(HINSTANCE inst, Demo& d) {
    Tester t;
    t.out = _wfopen((d.exeDir + L"\\ui_selftest.txt").c_str(), L"w");
    t.line("ui_demo selftest");

    t.line("-- hotkey format");
    t.check(formatHotkeyText(MOD_CONTROL | MOD_ALT, 'S') == "Ctrl+Alt+S", "Ctrl+Alt+S");
    t.check(formatHotkeyText(MOD_WIN | MOD_SHIFT | MOD_ALT | MOD_CONTROL, VK_F12) == "Ctrl+Alt+Shift+Win+F12", "modifier order");
    t.check(formatHotkeyText(MOD_SHIFT, VK_PRIOR) == "Shift+PageUp", "Shift+PageUp");
    t.check(formatHotkeyText(0, 'S').empty(), "no modifier -> empty");

    t.line("-- icons");
    saveIconPreviews(t, d.exeDir);
    countdownChecks(t);

    t.line("-- tray");
    HWND owner = createOwner(inst);
    setLanguage(Language::Russian);
    t.check(d.tray.create(inst, owner, kTrayCommandMsg), "tray icon added");
    d.tray.remove();
    {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = owner;
        nid.uID = 1;
        nid.uFlags = NIF_TIP;
        t.check(Shell_NotifyIconW(NIM_MODIFY, &nid) == FALSE, "tray icon removed (no ghost icon)");
    }
    {
        const auto apps = listRunningApps();
        t.line("      running apps with visible windows (before our window exists): %zu", apps.size());
    }
    d.window.initialize(inst, makeCallbacks());
    quickPanelChecks(t, d, inst, owner);

    for (int theme = 0; theme < 3; ++theme) settingsSuite(t, d, static_cast<UiTheme>(theme));
    themeAndPreviewChecks(t, d);
    dpiChecks(t, d);

    DestroyWindow(owner);
    t.line("checks: %d, failures: %d", t.checks, t.failures);
    t.line("RESULT: %s", t.failures == 0 ? "PASSED" : "FAILED");
    if (t.out) fclose(t.out);
    return t.failures == 0 ? 0 : 1;
}

int runInteractive(HINSTANCE inst, Demo& d) {
    HWND owner = createOwner(inst);
    if (!owner) return 1;
    setLanguage(d.settings.system.language);
    d.window.initialize(inst, makeCallbacks());
    d.tray.create(inst, owner, kTrayCommandMsg);
    d.tray.onQuickChange = [&d](const Settings& s) {
        d.settings = s;
        d.tray.setState(trayState(d));
        d.window.update(s);
    };
    d.tray.setSettings(d.settings);
    d.tray.setState(trayState(d));
    d.window.show(d.settings);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (d.window.preTranslateMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    d.window.close();
    d.tray.remove();
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (lstrcmpiW(argv[i], L"--selftest") == 0) selfTest = true;
        if (lstrcmpiW(argv[i], L"--dpi") == 0 && i + 1 < argc) uitest::setDpiOverride(static_cast<UINT>(_wtoi(argv[++i])));
    }
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    int rc = 0;
    {
        Demo demo;
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        demo.exeDir = path;
        demo.exeDir.erase(demo.exeDir.find_last_of(L'\\'));
        g_demo = &demo;
        rc = selfTest ? runSelfTest(inst, demo) : runInteractive(inst, demo);
        g_demo = nullptr;
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return rc;
}
