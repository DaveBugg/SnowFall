#include "quick_panel.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <vector>

#include "d2d_paint.h"
#include "compositor.h"
#include "decor.h"
#include "process_picker.h"
#include "settings_ids.h"
#include "skin.h"
#include "strings.h"
#include "theme.h"
#include "ui_util.h"
#include "user_presets.h"

namespace snow::ui {
namespace {

const wchar_t kPanelClass[] = L"SnowFallQuickPanel";
constexpr UINT_PTR kAnimTimer = 1;
constexpr UINT_PTR kChipsSub = 0x51434850;   // 'QCHP'
constexpr int kW = 344, kH = 510, kHeaderH = 54;

const StrId kShortNames[4] = {StrId::PresetLight, StrId::PresetNormal, StrId::PresetBlizzard, StrId::PresetThaw};

HHOOK g_hook = nullptr;
HWND g_activePanel = nullptr;
HWND g_activePicker = nullptr;   // picker opened from the panel (lives on after the panel hides)

// Keyboard navigation for the panel and its picker without help from the host's message loop.
LRESULT CALLBACK getMsgHook(int code, WPARAM wp, LPARAM lp) {
    if (g_activePicker && !IsWindow(g_activePicker)) g_activePicker = nullptr;
    if (code >= 0 && wp == PM_REMOVE) {
        MSG* m = reinterpret_cast<MSG*>(lp);
        if (m->message >= WM_KEYFIRST && m->message <= WM_KEYLAST) {
            for (HWND dlg : {g_activePanel, g_activePicker}) {
                if (!dlg || (m->hwnd != dlg && !IsChild(dlg, m->hwnd))) continue;
                if (IsDialogMessageW(dlg, m)) {
                    m->message = WM_NULL;
                    m->wParam = 0;
                    m->lParam = 0;
                }
                break;
            }
        }
    }
    const LRESULT r = CallNextHookEx(g_hook, code, wp, lp);
    if (!g_activePanel && !g_activePicker && g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    return r;
}

void ensureHook() {
    if (!g_hook) g_hook = SetWindowsHookExW(WH_GETMESSAGE, getMsgHook, nullptr, GetCurrentThreadId());
}

const Tool kQpTools[3] = {Tool::Snowball, Tool::Mitten, Tool::Shovel};
const StrId kQpToolNames[3] = {StrId::ToolSnowball, StrId::ToolMitten, StrId::ToolShovel};

}  // namespace

struct QuickPanel::Impl {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    Settings s;
    TrayState state;
    ChangeFn onChange;
    CommandFn onCommand;
    Skin skin;
    SceneAnim scenes[4];
    HeaderAnim headerAnim;
    UINT dpi = 96;
    HFONT font = nullptr, fontHeading = nullptr;
    std::vector<HWND> controls;
    bool timer = false;
    ULONGLONG lastTick = 0, hiddenAt = 0;
    int updating = 0;

    const Palette& pal() const { return palette(s.system.uiTheme); }
    int px(int dip) const { return MulDiv(dip, static_cast<int>(dpi), 96); }
    HWND item(int id) const { return GetDlgItem(hwnd, id); }

    HWND add(const wchar_t* cls, DWORD style, int id, const wchar_t* text, int x, int y, int w, int h) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        controls.push_back(c);
        return c;
    }

    void build() {
        skin.setSurface(hwnd, Surface::Window);
        HWND hint = add(L"STATIC", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS | WS_GROUP, IDC_QP_HINT, L"", 14, kHeaderH + 4, 318, 22);
        skin.setTextRole(hint, TextRole::Value);
        setTextAllowance(hint, -1);
        for (int i = 0; i < 4; ++i) {
            HWND c = add(L"BUTTON", BS_AUTORADIOBUTTON | (i == 0 ? (WS_TABSTOP | WS_GROUP) : 0), IDC_QP_LIGHT + i, tr(kShortNames[i]),
                         12 + i * 81, kHeaderH + 28, 77, 74);
            Style st;
            st.role = Role::SceneCard;
            st.surface = Surface::Window;
            st.scene = &scenes[i];
            st.compact = true;
            skin.set(c, st);
            setTextAllowance(c, 14);
            scenes[i].ensure(static_cast<WeatherPreset>(i), 4321u + static_cast<unsigned>(i) * 31u);
        }
        auto sliderRow = [&](int y, StrId text, int lblId, int id, int valId, int lo, int hi) {
            HWND l = add(L"STATIC", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | WS_GROUP, lblId, tr(text), 14, y, 72, 28);
            skin.setTextRole(l, TextRole::Normal);
            HWND tb = add(TRACKBAR_CLASSW, TBS_HORZ | TBS_BOTH | TBS_NOTICKS | WS_TABSTOP | WS_GROUP, id, L"", 86, y, 186, 28);
            SendMessageW(tb, TBM_SETRANGEMIN, FALSE, lo);
            SendMessageW(tb, TBM_SETRANGEMAX, FALSE, hi);
            SendMessageW(tb, TBM_SETPAGESIZE, 0, 10);
            Style st;
            st.role = Role::Slider;
            st.surface = Surface::Window;
            skin.set(tb, st);
            HWND v = add(L"STATIC", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | WS_GROUP, valId, L"", 278, y, 56, 28);
            skin.setTextRole(v, TextRole::Value);
        };
        sliderRow(kHeaderH + 108, StrId::QuickIntensity, IDC_QP_INTENSITY_LBL, IDC_QP_INTENSITY, IDC_QP_INTENSITY_VAL, 0, 100);
        sliderRow(kHeaderH + 140, StrId::QuickWind, IDC_QP_WIND_LBL, IDC_QP_WIND, IDC_QP_WIND_VAL, -100, 100);
        // Tool selector (icons).
        HWND tl = add(L"STATIC", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | WS_GROUP, IDC_QP_TOOL_LBL, tr(StrId::GroupTool), 14, kHeaderH + 174, 96, 34);
        skin.setTextRole(tl, TextRole::Normal);
        for (int i = 0; i < 3; ++i) {
            HWND b = add(L"BUTTON", BS_AUTORADIOBUTTON | (i == 0 ? (WS_TABSTOP | WS_GROUP) : 0), IDC_QP_TOOL_SNOWBALL + i, tr(kQpToolNames[i]),
                         112 + i * 46, kHeaderH + 174, 42, 34);
            Style st;
            st.role = Role::ToolIcon;
            st.surface = Surface::Window;
            st.glyph = static_cast<int>(kQpTools[i]);
            skin.set(b, st);
            setTextAllowance(b, -1);
        }
        HWND h = add(L"STATIC", SS_OWNERDRAW | SS_NOPREFIX | WS_GROUP, IDC_QP_PRESETS_LBL, tr(StrId::GroupUserPresets), 14, kHeaderH + 214, 316, 22);
        Style hs;
        hs.role = Role::Heading;
        hs.surface = Surface::Window;
        skin.set(h, hs);
        setTextAllowance(h, 4);
        HWND chips = add(L"LISTBOX", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_MULTICOLUMN | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_HSCROLL |
                                         WS_TABSTOP | WS_GROUP,
                         IDC_QP_PRESETS, L"", 10, kHeaderH + 238, 324, 52);
        Style cs;
        cs.role = Role::Chips;
        cs.surface = Surface::Window;
        skin.set(chips, cs);
        SetWindowSubclass(chips, chipsProc, kChipsSub, reinterpret_cast<DWORD_PTR>(this));
        auto btn = [&](int id, const wchar_t* text, int x, int y, bool primary) {
            HWND b = add(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, id, text, x, y, 156, 32);
            Style st;
            st.role = primary ? Role::PrimaryButton : Role::Button;
            st.surface = Surface::Window;
            skin.set(b, st);
            setTextAllowance(b, 18);
        };
        // Exclusions: count, picker, snow mode.
        HWND el = add(L"STATIC", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | WS_GROUP, IDC_QP_EXCL_LBL, L"", 14, kHeaderH + 296, 128, 30);
        skin.setTextRole(el, TextRole::Normal);
        {
            HWND b = add(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, IDC_QP_EXCL_PICK, tr(StrId::BtnExcludeApp), 144, kHeaderH + 296, 188, 30);
            Style st;
            st.role = Role::Button;
            st.surface = Surface::Window;
            skin.set(b, st);
            setTextAllowance(b, 18);
            const StrId modes[2] = {StrId::ExclBehind, StrId::ExclAround};
            for (int i = 0; i < 2; ++i) {
                HWND m = add(L"BUTTON", BS_AUTORADIOBUTTON | (i == 0 ? (WS_TABSTOP | WS_GROUP) : 0), IDC_QP_EXCL_BEHIND + i, tr(modes[i]),
                             12 + i * 160, kHeaderH + 332, 160, 30);
                Style ms;
                ms.role = Role::Segment;
                ms.surface = Surface::Window;
                ms.segIndex = i;
                ms.segCount = 2;
                skin.set(m, ms);
                setTextAllowance(m, 14);
            }
        }
        btn(IDC_QP_PAUSE, tr(state.paused ? StrId::MenuResume : StrId::MenuPause), 12, kH - 80, true);
        btn(IDC_QP_CLEAR, tr(StrId::MenuClearSnow), 176, kH - 80, false);
        btn(IDC_QP_FIREWORKS, tr(StrId::BtnFireworks), 12, kH - 42, false);
        btn(IDC_QP_SETTINGS, tr(StrId::MenuSettings), 176, kH - 42, false);
        SendMessageW(chips, LB_SETITEMHEIGHT, 0, px(34));
        SendMessageW(chips, LB_SETCOLUMNWIDTH, px(106), 0);   // three chips fit without scrolling
    }

    void createFonts() {
        if (font) DeleteObject(font);
        if (fontHeading) DeleteObject(fontHeading);
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        font = CreateFontIndirectW(&ncm.lfMessageFont);
        Painter::instance().setBodyFont(ncm.lfMessageFont.lfFaceName,
                                        static_cast<float>(std::abs(ncm.lfMessageFont.lfHeight)) * 96.0f / static_cast<float>(dpi));
        LOGFONTW head = ncm.lfMessageFont;
        head.lfHeight = -static_cast<LONG>(static_cast<float>(std::abs(ncm.lfMessageFont.lfHeight)) * 1.1f + 0.5f);
        head.lfWeight = FW_SEMIBOLD;
        wcsncpy_s(head.lfFaceName, Painter::instance().displayFamily().c_str(), _TRUNCATE);
        fontHeading = CreateFontIndirectW(&head);
        for (HWND c : controls) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(GetDlgCtrlID(c) == IDC_QP_PRESETS_LBL ? fontHeading : font), FALSE);
    }

    void sync() {
        if (!hwnd) return;
        ++updating;
        const int preset = std::clamp(static_cast<int>(s.weather.preset), 0, 3);
        for (int i = 0; i < 4; ++i) SendMessageW(item(IDC_QP_LIGHT + i), BM_SETCHECK, i == preset ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(item(IDC_QP_INTENSITY), TBM_SETPOS, TRUE, s.weather.intensity);
        SendMessageW(item(IDC_QP_WIND), TBM_SETPOS, TRUE, s.weather.wind);
        setTextIfChanged(item(IDC_QP_INTENSITY_VAL), format(L"%d %%", s.weather.intensity));
        const int w = s.weather.wind;
        setTextIfChanged(item(IDC_QP_WIND_VAL), w < 0 ? format(L"← %d", -w) : w > 0 ? format(L"%d →", w) : std::wstring(L"0"));
        setTextIfChanged(item(IDC_QP_PAUSE), tr(state.paused ? StrId::MenuResume : StrId::MenuPause));
        setTextIfChanged(item(IDC_QP_HINT), state.interactionKeys.empty() ? std::wstring(tr(StrId::HoldHintShortNone))
                                                                          : format(tr(StrId::HoldHintShortFmt), state.interactionKeys.c_str()));
        for (int i = 0; i < 3; ++i)
            SendMessageW(item(IDC_QP_TOOL_SNOWBALL + i), BM_SETCHECK, s.interaction.tool == kQpTools[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        const int mode = s.system.excludedSnowMode == ExcludedSnowMode::Around ? 1 : 0;
        for (int i = 0; i < 2; ++i) SendMessageW(item(IDC_QP_EXCL_BEHIND + i), BM_SETCHECK, i == mode ? BST_CHECKED : BST_UNCHECKED, 0);
        setTextIfChanged(item(IDC_QP_EXCL_LBL), format(tr(StrId::QuickExclusionsFmt), static_cast<int>(s.system.excludedProcesses.size())));
        HWND chips = item(IDC_QP_PRESETS);
        SendMessageW(chips, LB_RESETCONTENT, 0, 0);
        for (const UserPreset& p : s.userPresets) SendMessageW(chips, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(p.name).c_str()));
        --updating;
        InvalidateRect(hwnd, nullptr, TRUE);
    }

    void changed() {
        if (onChange) onChange(s);
    }

    void command(TrayCommand c, LPARAM lp = 0) {
        if (onCommand) onCommand(c, lp);
    }

    Layer bgLayer;

    // Static panel background (D2D, cached) + animated header strip (compositor).
    void paintBg(HDC dc, bool headerOnly = false) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const Palette& P = pal();
        Painter& pt = Painter::instance();
        const StrId status = state.hidden ? StrId::StatusHidden : state.paused ? StrId::StatusPaused : StrId::StatusRunning;
        wchar_t key[160];
        swprintf_s(key, L"qp|%d|%ls|%ls", static_cast<int>(s.system.uiTheme), tr(status), tr(StrId::AppName));
        bgLayer.render(key, rc.right, rc.bottom, dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F size) {
            pt.fill(D2D1::RectF(0, 0, size.width, size.height), col(P.bg));
            const D2D1_RECT_F head = D2D1::RectF(0, 0, size.width, static_cast<float>(kHeaderH));
            pt.vgradient(head, col(P.headerTop), col(P.headerBottom));
            if (P.garland) drawGarlandStatic(pt, -10, size.width + 10, kHeaderH - 14.0f, 4);
            drawLogo(pt, P, 28, kHeaderH / 2.0f - (P.garland ? 5.0f : 0.0f), 24);
            TextStyle ts{pt.bodySize() * 1.3f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
            ts.display = true;
            ts.valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
            pt.text(tr(StrId::AppName), D2D1::RectF(50, 7, size.width - 12, 30), ts, col(P.headerText));
            TextStyle ss{pt.bodySize() * 0.95f};
            ss.valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
            pt.text(tr(status), D2D1::RectF(51, 29, size.width - 12, 46), ss, col(P.headerTextSecondary));
            pt.rt()->DrawRectangle(D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f), pt.brush(col(P.cardBorder)), 1.0f);
        });
        if (!headerOnly) BitBlt(dc, 0, 0, rc.right, rc.bottom, bgLayer.dc(), 0, 0, SRCCOPY);
        headerAnim.ensure(0xBADA55u, 30);
        Compositor& c = Compositor::instance();
        const int hh = px(kHeaderH) - 1;
        if (c.begin(bgLayer, rc.right - 2, hh - 1, 1, 1)) {   // inside the 1 px border
            const float sc = static_cast<float>(dpi) / 96.0f;
            headerAnim.flakes.draw(c, 0, 0, static_cast<float>(c.width()), static_cast<float>(c.height()), sc, P.snow);
            if (P.garland) drawGarlandLights(c, sc, -10 - 1 / sc, static_cast<float>(rc.right) / sc + 10, kHeaderH - 14.0f - 1 / sc, 4, headerAnim.time);
            c.end(dc, 1, 1);
        }
    }

    void startTimer(bool on) {
        if (on && !timer) {
            lastTick = GetTickCount64();
            SetTimer(hwnd, kAnimTimer, 40, nullptr);
            timer = true;
        } else if (!on && timer) {
            KillTimer(hwnd, kAnimTimer);
            timer = false;
        }
    }

    unsigned ticks = 0;
    void onTick() {
        const ULONGLONG now = GetTickCount64();
        const float dt = std::min(0.1f, static_cast<float>(now - lastTick) / 1000.0f);
        lastTick = now;
        headerAnim.step(dt);
        if (HDC dc = GetDC(hwnd)) {
            paintBg(dc, true);
            ReleaseDC(hwnd, dc);
        }
        if ((++ticks % 2) == 0) {
            for (int i = 0; i < 4; ++i) {
                scenes[i].step(dt * 2);
                skin.repaintNow(item(IDC_QP_LIGHT + i));
            }
        }
    }

    void onCommandMsg(int id, int code, HWND src) {
        if (updating) return;
        if (id >= IDC_QP_LIGHT && id <= IDC_QP_THAW && code == BN_CLICKED) {
            if (SendMessageW(src, BM_GETCHECK, 0, 0) != BST_CHECKED) return;
            const auto p = static_cast<WeatherPreset>(id - IDC_QP_LIGHT);
            if (s.weather.preset == p) return;
            s.weather.preset = p;
            changed();
            return;
        }
        if (code != BN_CLICKED) return;
        if (id >= IDC_QP_TOOL_SNOWBALL && id <= IDC_QP_TOOL_SHOVEL) {
            if (SendMessageW(src, BM_GETCHECK, 0, 0) != BST_CHECKED) return;
            const Tool t = kQpTools[id - IDC_QP_TOOL_SNOWBALL];
            if (s.interaction.tool == t) return;
            s.interaction.tool = t;
            changed();
            return;
        }
        if (id == IDC_QP_EXCL_BEHIND || id == IDC_QP_EXCL_AROUND) {
            if (SendMessageW(src, BM_GETCHECK, 0, 0) != BST_CHECKED) return;
            const auto m = id == IDC_QP_EXCL_AROUND ? ExcludedSnowMode::Around : ExcludedSnowMode::Behind;
            if (s.system.excludedSnowMode == m) return;
            s.system.excludedSnowMode = m;
            changed();
            return;
        }
        switch (id) {
            case IDC_QP_EXCL_PICK:
                openPicker();
                break;
            case IDC_QP_PAUSE: command(TrayCommand::TogglePause); break;
            case IDC_QP_CLEAR: command(TrayCommand::ClearSnow); break;
            case IDC_QP_FIREWORKS: command(TrayCommand::CallFireworks); break;
            case IDC_QP_SETTINGS:
                hideNow();
                command(TrayCommand::OpenSettings);
                break;
            case IDCANCEL: hideNow(); break;
            case IDOK: {
                HWND f = GetFocus();
                if (f == item(IDC_QP_PRESETS)) applyPreset(static_cast<int>(SendMessageW(f, LB_GETCURSEL, 0, 0)));
                else if (const Style* st = skin.find(f); st && st->role == Role::SceneCard) SendMessageW(f, BM_CLICK, 0, 0);
                break;
            }
            default: break;
        }
    }

    void onScroll(HWND src) {
        if (updating) return;
        const int v = static_cast<int>(SendMessageW(src, TBM_GETPOS, 0, 0));
        if (src == item(IDC_QP_INTENSITY)) {
            setTextIfChanged(item(IDC_QP_INTENSITY_VAL), format(L"%d %%", v));
            if (s.weather.intensity == v) return;
            s.weather.intensity = v;
            changed();
        } else if (src == item(IDC_QP_WIND)) {
            setTextIfChanged(item(IDC_QP_WIND_VAL), v < 0 ? format(L"← %d", -v) : v > 0 ? format(L"%d →", v) : std::wstring(L"0"));
            if (s.weather.wind == v) return;
            s.weather.wind = v;
            changed();
        }
    }

    ProcessPicker picker;
    void openPicker() {
        picker.open(instance, hwnd, s.system.uiTheme, [this](const std::vector<std::wstring>& names) {
            bool added = false;
            for (const std::wstring& n : names) {
                bool dup = false;
                for (const std::string& e : s.system.excludedProcesses) dup = dup || equalsNoCase(widen(e), n);
                if (!dup && !n.empty()) {
                    s.system.excludedProcesses.push_back(narrow(n));
                    added = true;
                }
            }
            if (!added) return;
            sync();
            changed();
        });
        g_activePicker = picker.hwnd();
        if (g_activePicker) ensureHook();
    }

    void applyPreset(int index) {
        if (index < 0 || static_cast<size_t>(index) >= s.userPresets.size()) return;
        applyUserPreset(s, s.userPresets[static_cast<size_t>(index)]);
        sync();
        changed();
    }

    void hideNow() {
        if (!hwnd || !IsWindowVisible(hwnd)) return;
        startTimer(false);
        ShowWindow(hwnd, SW_HIDE);
        hiddenAt = GetTickCount64();
        g_activePanel = nullptr;
        if (g_hook && !g_activePicker) {
            UnhookWindowsHookEx(g_hook);
            g_hook = nullptr;
        }
    }

    static LRESULT CALLBACK chipsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
        Impl* self = reinterpret_cast<Impl*>(ref);
        switch (msg) {
            case WM_LBUTTONUP: {
                const LRESULT hit = SendMessageW(h, LB_ITEMFROMPOINT, 0, lp);
                const LRESULT r = DefSubclassProc(h, msg, wp, lp);
                if (HIWORD(hit) == 0) self->applyPreset(LOWORD(hit));
                return r;
            }
            case WM_KEYDOWN:
                if (wp == VK_SPACE) {
                    self->applyPreset(static_cast<int>(SendMessageW(h, LB_GETCURSEL, 0, 0)));
                    return 0;
                }
                break;
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
                InvalidateRect(h, nullptr, FALSE);
                break;
            case WM_ERASEBKGND: {
                RECT rc{};
                GetClientRect(h, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, surfaceBrush(self->pal(), Surface::Window));
                return 1;
            }
            case WM_PAINT:
                if (SendMessageW(h, LB_GETCOUNT, 0, 0) == 0) {
                    PAINTSTRUCT ps;
                    HDC dc = BeginPaint(h, &ps);
                    RECT rc{};
                    GetClientRect(h, &rc);
                    const Palette& P = self->pal();
                    Painter::instance().paint(dc, rc, self->dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F sz) {
                        Painter& pt = Painter::instance();
                        pt.fill(D2D1::RectF(0, 0, sz.width, sz.height), col(P.bg));
                        TextStyle ts{pt.bodySize() * 0.95f};
                        ts.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                        pt.text(tr(StrId::QuickNoPresets), D2D1::RectF(4, 0, sz.width - 4, 34), ts, col(P.textSecondary));
                        if (GetFocus() == h) pt.strokeRound(D2D1::RectF(2, 2, sz.width - 2, 32), 8, col(P.focus), 1.5f);
                    });
                    EndPaint(h, &ps);
                    return 0;
                }
                break;
            case WM_NCDESTROY:
                RemoveWindowSubclass(h, chipsProc, kChipsSub);
                break;
            default:
                break;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd = h;
            return DefWindowProcW(h, msg, wp, lp);
        }
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        LRESULT r = 0;
        switch (msg) {
            case WM_NOTIFY:
                if (self->skin.onNotify(lp, &r)) return r;
                break;
            case WM_DRAWITEM:
                if (self->skin.onDrawItem(lp)) return TRUE;
                break;
            case WM_MEASUREITEM:
                reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = static_cast<UINT>(self->px(34));
                return TRUE;
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN:
            case WM_CTLCOLORLISTBOX:
                if (self->skin.onCtlColor(msg, wp, lp, &r)) return r;
                break;
            case WM_ERASEBKGND:
                self->paintBg(reinterpret_cast<HDC>(wp));
                return 1;
            case WM_PRINTCLIENT:
                self->paintBg(reinterpret_cast<HDC>(wp));
                return 0;
            case WM_COMMAND:
                self->onCommandMsg(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
                return 0;
            case WM_HSCROLL:
                self->onScroll(reinterpret_cast<HWND>(lp));
                return 0;
            case WM_TIMER:
                if (wp == kAnimTimer) self->onTick();
                return 0;
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE) self->hideNow();
                break;
            case uitest::kMsgQueryTimers:
                return self->timer ? 1 : 0;
            case WM_CLOSE:
                self->hideNow();
                return 0;
            case WM_NCDESTROY: {
                const LRESULT res = DefWindowProcW(h, msg, wp, lp);
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
                if (g_activePanel == h) g_activePanel = nullptr;
                if (g_hook && !g_activePanel && !g_activePicker) {
                    UnhookWindowsHookEx(g_hook);
                    g_hook = nullptr;
                }
                self->hwnd = nullptr;
                self->timer = false;
                self->controls.clear();
                self->skin.clear();
                if (self->font) DeleteObject(self->font);
                if (self->fontHeading) DeleteObject(self->fontHeading);
                self->font = self->fontHeading = nullptr;
                return res;
            }
            default:
                break;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};

QuickPanel::QuickPanel() : d_(std::make_unique<Impl>()) {}
QuickPanel::~QuickPanel() { destroy(); }

void QuickPanel::destroy() {
    if (d_->hwnd) DestroyWindow(d_->hwnd);
}

bool QuickPanel::visible() const { return d_->hwnd && IsWindowVisible(d_->hwnd); }
HWND QuickPanel::hwnd() const { return d_->hwnd; }
ULONGLONG QuickPanel::lastHideTick() const { return d_->hiddenAt; }
void QuickPanel::hide() { d_->hideNow(); }

void QuickPanel::update(const Settings& s, const TrayState& st) {
    Impl& d = *d_;
    const bool themeChanged = d.s.system.uiTheme != s.system.uiTheme;
    d.s = s;
    d.state = st;
    if (!d.hwnd) return;
    d.skin.pal = &d.pal();
    if (themeChanged) RedrawWindow(d.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    d.sync();
}

void QuickPanel::show(HINSTANCE instance, const RECT& anchor, const Settings& s, const TrayState& st, ChangeFn onChange,
                      CommandFn onCommand) {
    Impl& d = *d_;
    d.instance = instance ? instance : GetModuleHandleW(nullptr);
    d.s = s;
    d.state = st;
    d.onChange = std::move(onChange);
    d.onCommand = std::move(onCommand);

    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    UINT mdpi = 96;
    {
        UINT dx = 96, dy = 96;
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            auto fn = reinterpret_cast<GetDpiForMonitorFn>(reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor")));
            if (fn && SUCCEEDED(fn(mon, 0, &dx, &dy))) mdpi = dx;
            FreeLibrary(shcore);
        }
    }
    if (d.hwnd && d.dpi != mdpi) destroy();
    d.dpi = mdpi;

    if (!d.hwnd) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        if (!GetClassInfoExW(d.instance, kPanelClass, &wc)) {
            wc = {};
            wc.cbSize = sizeof(wc);
            wc.style = CS_DROPSHADOW;
            wc.lpfnWndProc = Impl::proc;
            wc.hInstance = d.instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kPanelClass;
            RegisterClassExW(&wc);
        }
        HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT, kPanelClass, tr(StrId::AppName),
                                 WS_POPUP | WS_CLIPCHILDREN, mi.rcWork.left, mi.rcWork.top, d.px(kW), d.px(kH), nullptr, nullptr,
                                 d.instance, &d);
        if (!h) return;
        const int corner = 2;   // DWMWCP_ROUND
        DwmSetWindowAttribute(h, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
        d.skin.pal = &d.pal();
        d.skin.dpi = d.dpi;
        d.build();
        d.createFonts();
    }
    d.skin.pal = &d.pal();
    d.skin.dpi = d.dpi;
    SetWindowTextW(d.item(IDC_QP_INTENSITY_LBL), tr(StrId::QuickIntensity));
    SetWindowTextW(d.item(IDC_QP_WIND_LBL), tr(StrId::QuickWind));
    SetWindowTextW(d.item(IDC_QP_PRESETS_LBL), tr(StrId::GroupUserPresets));
    SetWindowTextW(d.item(IDC_QP_CLEAR), tr(StrId::MenuClearSnow));
    SetWindowTextW(d.item(IDC_QP_FIREWORKS), tr(StrId::BtnFireworks));
    SetWindowTextW(d.item(IDC_QP_SETTINGS), tr(StrId::MenuSettings));
    SetWindowTextW(d.item(IDC_QP_TOOL_LBL), tr(StrId::GroupTool));
    SetWindowTextW(d.item(IDC_QP_EXCL_PICK), tr(StrId::BtnExcludeApp));
    SetWindowTextW(d.item(IDC_QP_EXCL_BEHIND), tr(StrId::ExclBehind));
    SetWindowTextW(d.item(IDC_QP_EXCL_AROUND), tr(StrId::ExclAround));
    for (int i = 0; i < 3; ++i) SetWindowTextW(d.item(IDC_QP_TOOL_SNOWBALL + i), tr(kQpToolNames[i]));
    for (int i = 0; i < 4; ++i) SetWindowTextW(d.item(IDC_QP_LIGHT + i), tr(kShortNames[i]));
    d.sync();

    // Position next to the tray icon, inside the work area.
    const int w = d.px(kW), h = d.px(kH), gap = d.px(10);
    int x = (anchor.left + anchor.right) / 2 - w / 2;
    int y = anchor.top - gap - h;
    if (anchor.top < mi.rcWork.top + h / 2) y = anchor.bottom + gap;                 // taskbar on top
    if (anchor.left >= mi.rcWork.right) { x = anchor.left - gap - w; y = anchor.bottom - h; }   // taskbar on the right
    if (anchor.right <= mi.rcWork.left) { x = anchor.right + gap; y = anchor.bottom - h; }      // taskbar on the left
    x = std::clamp<int>(x, mi.rcWork.left + gap, std::max<int>(mi.rcWork.left + gap, mi.rcWork.right - gap - w));
    y = std::clamp<int>(y, mi.rcWork.top + gap, std::max<int>(mi.rcWork.top + gap, mi.rcWork.bottom - gap - h));
    SetWindowPos(d.hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(d.hwnd);
    const int preset = std::clamp(static_cast<int>(s.weather.preset), 0, 3);
    SetFocus(d.item(IDC_QP_LIGHT + preset));
    if (!g_hook) g_hook = SetWindowsHookExW(WH_GETMESSAGE, getMsgHook, nullptr, GetCurrentThreadId());
    g_activePanel = d.hwnd;
    d.startTimer(true);
}

}  // namespace snow::ui
