#include "process_picker.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>

#include "d2d_paint.h"
#include "settings_ids.h"
#include "skin.h"
#include "strings.h"
#include "ui_util.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace snow::ui {

std::vector<RunningApp> listRunningApps() {
    std::vector<DWORD> pids;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            auto* out = reinterpret_cast<std::vector<DWORD>*>(lp);
            if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) != nullptr) return TRUE;
            if (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
            BOOL cloaked = FALSE;
            if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
            RECT r{};
            GetWindowRect(w, &r);
            if (r.right - r.left <= 1 || r.bottom - r.top <= 1) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid && std::find(out->begin(), out->end(), pid) == out->end()) out->push_back(pid);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&pids));

    std::vector<RunningApp> apps;
    for (DWORD pid : pids) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD len = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
            std::wstring full(path, len);
            const size_t slash = full.find_last_of(L"\\/");
            std::wstring name = slash == std::wstring::npos ? full : full.substr(slash + 1);
            bool dup = false;
            for (const RunningApp& a : apps) dup = dup || equalsNoCase(a.exeName, name);
            if (!dup && !name.empty()) apps.push_back({name, full});
        }
        CloseHandle(h);
    }
    std::sort(apps.begin(), apps.end(), [](const RunningApp& a, const RunningApp& b) {
        return CompareStringOrdinal(a.exeName.c_str(), -1, b.exeName.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return apps;
}

namespace {
const wchar_t kPickerClass[] = L"SnowFallProcessPicker";
}

struct ProcessPicker::Impl {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr, list = nullptr, hint = nullptr, add = nullptr, cancel = nullptr;
    UiTheme theme = UiTheme::Night;
    UINT dpi = 96;
    AddFn onAdd;
    std::vector<RunningApp> apps;
    std::vector<HICON> icons;
    HFONT font = nullptr;
    Skin skin;

    int px(int dip) const { return MulDiv(dip, static_cast<int>(dpi), 96); }

    void destroyResources() {
        for (HICON i : icons)
            if (i) DestroyIcon(i);
        icons.clear();
        if (font) DeleteObject(font);
        font = nullptr;
    }

    void layout() {
        const int w = 400, h = 460;
        SetWindowPos(hint, nullptr, px(16), px(12), px(w - 32), px(40), SWP_NOZORDER);
        SetWindowPos(list, nullptr, px(20), px(62), px(w - 40), px(h - 62 - 64), SWP_NOZORDER);
        SetWindowPos(add, nullptr, px(w - 16 - 130 - 8 - 110), px(h - 46), px(130), px(32), SWP_NOZORDER);
        SetWindowPos(cancel, nullptr, px(w - 16 - 110), px(h - 46), px(110), px(32), SWP_NOZORDER);
        SendMessageW(list, LB_SETITEMHEIGHT, 0, px(28));
        RECT wr{0, 0, px(w), px(h)};
        AdjustWindowRectExForDpi(&wr, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi);
        SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }

    void doAdd() {
        std::vector<std::wstring> names;
        const int n = static_cast<int>(SendMessageW(list, LB_GETSELCOUNT, 0, 0));
        if (n > 0) {
            std::vector<int> sel(static_cast<size_t>(n));
            SendMessageW(list, LB_GETSELITEMS, static_cast<WPARAM>(n), reinterpret_cast<LPARAM>(sel.data()));
            for (int i : sel)
                if (i >= 0 && static_cast<size_t>(i) < apps.size()) names.push_back(apps[static_cast<size_t>(i)].exeName);
        }
        AddFn fn = onAdd;
        DestroyWindow(hwnd);
        if (fn && !names.empty()) fn(names);
    }

    void drawItem(const DRAWITEMSTRUCT* di) {
        const Palette& p = palette(theme);
        RECT rc = di->rcItem;
        const bool sel = (di->itemState & ODS_SELECTED) != 0;
        HBRUSH b = CreateSolidBrush(sel ? mixColor(p.field, p.accent, p.dark ? 0.35f : 0.18f) : p.field);
        FillRect(di->hDC, &rc, b);
        DeleteObject(b);
        if (di->itemID == static_cast<UINT>(-1) || di->itemID >= apps.size()) return;
        const int icon = px(16);
        HICON hi = icons[di->itemID];
        if (hi) DrawIconEx(di->hDC, rc.left + px(8), rc.top + (rc.bottom - rc.top - icon) / 2, hi, icon, icon, 0, nullptr, DI_NORMAL);
        RECT tr = rc;
        tr.left += px(34);
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, p.fieldText);
        HGDIOBJ old = SelectObject(di->hDC, font);
        DrawTextW(di->hDC, apps[di->itemID].exeName.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(di->hDC, old);
        if (di->itemState & ODS_FOCUS) {
            HBRUSH fb = CreateSolidBrush(p.focus);
            FrameRect(di->hDC, &rc, fb);
            DeleteObject(fb);
        }
    }

    void paintBg(HDC dc) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const Palette& p = palette(theme);
        RECT lr{};
        GetWindowRect(list, &lr);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&lr), 2);
        const float k = 96.0f / static_cast<float>(dpi);
        Painter::instance().paint(dc, rc, dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F s) {
            Painter& pt = Painter::instance();
            pt.fill(D2D1::RectF(0, 0, s.width, s.height), col(p.page));
            const D2D1_RECT_F f = D2D1::RectF(static_cast<float>(lr.left) * k - 4, static_cast<float>(lr.top) * k - 4,
                                              static_cast<float>(lr.right) * k + 4, static_cast<float>(lr.bottom) * k + 4);
            pt.fillRound(f, 8, col(p.field));
            pt.strokeRound(f, 8, col(p.fieldBorder), 1.0f);
            if (apps.empty()) {
                TextStyle ts{pt.bodySize()};
                ts.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                pt.text(tr(StrId::PickerEmpty), f, ts, col(p.textSecondary));
            }
        });
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
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORLISTBOX:
            case WM_CTLCOLORBTN:
                if (self->skin.onCtlColor(msg, wp, lp, &r)) return r;
                break;
            case WM_MEASUREITEM: {
                auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
                mi->itemHeight = static_cast<UINT>(self->px(28));
                return TRUE;
            }
            case WM_DRAWITEM:
                if (wp == IDC_PICK_LIST) {
                    self->drawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
                    return TRUE;
                }
                break;
            case WM_ERASEBKGND:
                self->paintBg(reinterpret_cast<HDC>(wp));
                return 1;
            case WM_COMMAND:
                switch (LOWORD(wp)) {
                    case IDC_PICK_ADD:
                    case IDOK:
                        self->doAdd();
                        return 0;
                    case IDC_PICK_CANCEL:
                    case IDCANCEL:
                        DestroyWindow(h);
                        return 0;
                    case IDC_PICK_LIST:
                        if (HIWORD(wp) == LBN_DBLCLK) self->doAdd();
                        else if (HIWORD(wp) == LBN_SELCHANGE)
                            EnableWindow(self->add, SendMessageW(self->list, LB_GETSELCOUNT, 0, 0) > 0);
                        return 0;
                    default:
                        break;
                }
                break;
            case WM_CLOSE:
                DestroyWindow(h);
                return 0;
            case WM_NCDESTROY: {
                const LRESULT res = DefWindowProcW(h, msg, wp, lp);
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
                self->hwnd = self->list = self->hint = self->add = self->cancel = nullptr;
                self->skin.clear();
                self->destroyResources();
                return res;
            }
            default:
                break;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};

ProcessPicker::ProcessPicker() : d_(std::make_unique<Impl>()) {}
ProcessPicker::~ProcessPicker() { close(); }

HWND ProcessPicker::hwnd() const { return d_->hwnd; }

void ProcessPicker::close() {
    if (d_->hwnd) DestroyWindow(d_->hwnd);
}

bool ProcessPicker::preTranslateMessage(MSG* msg) {
    if (!d_->hwnd || !msg) return false;
    if (msg->hwnd != d_->hwnd && !IsChild(d_->hwnd, msg->hwnd)) return false;
    return IsDialogMessageW(d_->hwnd, msg) != FALSE;
}

bool ProcessPicker::open(HINSTANCE instance, HWND owner, UiTheme theme, AddFn onAdd) {
    Impl& d = *d_;
    if (d.hwnd) {
        SetForegroundWindow(d.hwnd);
        return true;
    }
    d.instance = instance ? instance : GetModuleHandleW(nullptr);
    d.theme = theme;
    d.onAdd = std::move(onAdd);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(d.instance, kPickerClass, &wc)) {
        wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Impl::proc;
        wc.hInstance = d.instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPickerClass;
        RegisterClassExW(&wc);
    }
    d.dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    RECT orc{};
    if (owner) GetWindowRect(owner, &orc);
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kPickerClass, tr(StrId::PickerTitle),
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, orc.left + 80, orc.top + 60, 300, 300,
                             owner, nullptr, d.instance, &d);
    if (!h) return false;
    const Palette& p = palette(theme);
    BOOL darkMode = p.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkMode, sizeof(darkMode));
    d.skin.pal = &p;
    d.skin.dpi = d.dpi;
    d.skin.setSurface(h, Surface::Page);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, d.dpi);
    d.font = CreateFontIndirectW(&ncm.lfMessageFont);

    d.hint = CreateWindowExW(0, L"STATIC", tr(StrId::PickerHint), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, h,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PICK_HINT)), d.instance, nullptr);
    d.skin.setTextRole(d.hint, TextRole::Secondary);
    d.list = CreateWindowExW(0, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                                 LBS_EXTENDEDSEL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                             0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PICK_LIST)), d.instance, nullptr);
    if (p.dark) SetWindowTheme(d.list, L"DarkMode_Explorer", nullptr);
    d.add = CreateWindowExW(0, L"BUTTON", tr(StrId::BtnAdd), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, h,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PICK_ADD)), d.instance, nullptr);
    d.cancel = CreateWindowExW(0, L"BUTTON", tr(StrId::BtnCancel), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10,
                               10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PICK_CANCEL)), d.instance, nullptr);
    Style bs;
    bs.role = Role::PrimaryButton;
    d.skin.set(d.add, bs);
    bs.role = Role::Button;
    d.skin.set(d.cancel, bs);
    for (HWND c : {d.hint, d.list, d.add, d.cancel}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(d.font), FALSE);
    setTextAllowance(d.add, 16);
    setTextAllowance(d.cancel, 16);

    d.apps = listRunningApps();
    for (const RunningApp& a : d.apps) {
        HICON smallIcon = nullptr;
        ExtractIconExW(a.path.c_str(), 0, nullptr, &smallIcon, 1);
        d.icons.push_back(smallIcon);
        SendMessageW(d.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(a.exeName.c_str()));
    }
    EnableWindow(d.add, FALSE);
    d.layout();
    ShowWindow(h, SW_SHOW);
    SetFocus(d.list);
    return true;
}

}  // namespace snow::ui
