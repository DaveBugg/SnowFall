#include "tray_icon.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>

#include "quick_panel.h"
#include "strings.h"

namespace snow {
namespace {

constexpr UINT kIconId = 1;
constexpr UINT_PTR kClickTimerId = 0x534E5731;   // 'SNW1' — delayed single-click action
constexpr UINT kMenuBase = 100;                  // menu item id = kMenuBase + TrayCommand
constexpr UINT kUserPresetBase = 300;            // «Мои пресеты» items: kUserPresetBase + index

UINT menuId(TrayCommand c) { return kMenuBase + static_cast<UINT>(c); }

void copyText(wchar_t* dst, size_t cap, const std::wstring& src) {
    wcsncpy_s(dst, cap, src.c_str(), _TRUNCATE);
}

}  // namespace

struct TrayIcon::Impl {
    HINSTANCE instance = nullptr;
    HWND owner = nullptr;
    UINT commandMsg = 0;
    UINT callbackMsg = 0;
    UINT taskbarCreatedMsg = 0;
    bool created = false;   // create() was called and remove() was not
    bool added = false;     // the shell currently has our icon
    bool menuOpen = false;
    TrayState state;
    HICON iconNormal = nullptr;
    HICON iconPaused = nullptr;
    int iconSize = 0;
    DWORD lastDoubleClickTick = 0;
    TrayIcon* self = nullptr;
    Settings settings;
    ui::QuickPanel panel;

    void openPanel() {
        RECT anchor{};
        NOTIFYICONIDENTIFIER nii{};
        nii.cbSize = sizeof(nii);
        nii.hWnd = owner;
        nii.uID = kIconId;
        if (!added || FAILED(Shell_NotifyIconGetRect(&nii, &anchor))) {
            POINT pt{};
            GetCursorPos(&pt);
            anchor = RECT{pt.x - 8, pt.y - 8, pt.x + 8, pt.y + 8};
        }
        panel.show(instance, anchor, settings, state,
                   [this](const Settings& s) {
                       settings = s;
                       if (self && self->onQuickChange) self->onQuickChange(s);
                   },
                   [this](TrayCommand c, LPARAM lp) { post(c, lp); });
    }

    void ensureIcons() {
        UINT dpi = owner ? GetDpiForWindow(owner) : 0;
        if (dpi == 0) dpi = GetDpiForSystem();
        int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
        if (size <= 0) size = 16;
        if (size == iconSize && iconNormal && iconPaused) return;
        destroyIcons();
        iconSize = size;
        iconNormal = TrayIcon::createAppIcon(size, false);
        iconPaused = TrayIcon::createAppIcon(size, true);
    }

    void destroyIcons() {
        if (iconNormal) DestroyIcon(iconNormal);
        if (iconPaused) DestroyIcon(iconPaused);
        iconNormal = iconPaused = nullptr;
        iconSize = 0;
    }

    std::wstring tooltip() const {
        std::wstring tip = tr(StrId::AppName);
        tip += L" — ";
        tip += state.hidden ? tr(StrId::StatusHidden) : state.paused ? tr(StrId::StatusPaused) : tr(StrId::StatusRunning);
        return tip;
    }

    NOTIFYICONDATAW baseData() const {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = owner;
        nid.uID = kIconId;
        return nid;
    }

    void fillVisuals(NOTIFYICONDATAW& nid) {
        ensureIcons();
        nid.uFlags |= NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = callbackMsg;
        nid.hIcon = (state.paused || state.hidden) ? iconPaused : iconNormal;
        copyText(nid.szTip, ARRAYSIZE(nid.szTip), tooltip());
    }

    bool add() {
        if (!owner) return false;
        NOTIFYICONDATAW nid = baseData();
        fillVisuals(nid);
        // A stale icon with the same hwnd+id (e.g. after an explorer restart race) would make
        // NIM_ADD fail, so delete first; the result of the delete is irrelevant.
        NOTIFYICONDATAW del = baseData();
        Shell_NotifyIconW(NIM_DELETE, &del);
        added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (added) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }
        return added;
    }

    void modify() {
        if (!added) return;
        NOTIFYICONDATAW nid = baseData();
        fillVisuals(nid);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void post(TrayCommand c, LPARAM lp = 0) const {
        if (owner) PostMessageW(owner, commandMsg, static_cast<WPARAM>(c), lp);
    }

    HMENU buildMenu() const {
        HMENU menu = CreatePopupMenu();
        if (!menu) return nullptr;
        auto item = [](HMENU m, TrayCommand c, StrId text, bool checked = false) {
            AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), menuId(c), tr(text));
        };

        item(menu, TrayCommand::TogglePause, state.paused ? StrId::MenuResume : StrId::MenuPause);

        HMENU intensity = CreatePopupMenu();
        item(intensity, TrayCommand::PresetLight, StrId::PresetLight);
        item(intensity, TrayCommand::PresetNormal, StrId::PresetNormal);
        item(intensity, TrayCommand::PresetBlizzard, StrId::PresetBlizzard);
        item(intensity, TrayCommand::PresetThaw, StrId::PresetThaw);
        CheckMenuRadioItem(intensity, 0, 3, static_cast<UINT>(std::clamp(static_cast<int>(state.preset), 0, 3)), MF_BYPOSITION);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(intensity), tr(StrId::MenuIntensity));

        HMENU mine = CreatePopupMenu();
        if (state.userPresetNames.empty()) {
            AppendMenuW(mine, MF_STRING | MF_GRAYED, kUserPresetBase, tr(StrId::MenuNoPresets));
        } else {
            for (size_t i = 0; i < state.userPresetNames.size() && i < 50; ++i) {
                std::wstring name = state.userPresetNames[i];
                for (size_t pos = 0; (pos = name.find(L'&', pos)) != std::wstring::npos; pos += 2) name.insert(pos, 1, L'&');
                AppendMenuW(mine, MF_STRING, kUserPresetBase + static_cast<UINT>(i), name.c_str());
            }
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(mine), tr(StrId::MenuUserPresets));

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        item(menu, TrayCommand::ToggleInteraction, StrId::MenuInteraction, state.interactionMode);

        HMENU tool = CreatePopupMenu();
        item(tool, TrayCommand::ToolMitten, StrId::ToolMitten);
        item(tool, TrayCommand::ToolShovel, StrId::ToolShovel);
        item(tool, TrayCommand::ToolSnowball, StrId::ToolSnowball);
        const TrayCommand toolCmd = state.tool == Tool::Shovel     ? TrayCommand::ToolShovel
                                    : state.tool == Tool::Snowball ? TrayCommand::ToolSnowball
                                                                   : TrayCommand::ToolMitten;
        CheckMenuRadioItem(tool, menuId(TrayCommand::ToolMitten), menuId(TrayCommand::ToolSnowball), menuId(toolCmd),
                           MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tool), tr(StrId::MenuTool));

        item(menu, TrayCommand::ClearSnow, StrId::MenuClearSnow);
        item(menu, TrayCommand::CallSnowplow, StrId::MenuCallSnowplow);
        item(menu, TrayCommand::CallFireworks, StrId::MenuFireworks);
        item(menu, TrayCommand::ToggleHideAll, StrId::MenuHideAll, state.hidden);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        item(menu, TrayCommand::OpenSettings, StrId::MenuSettings);
        item(menu, TrayCommand::ToggleAutostart, StrId::MenuAutostart, state.autostart);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        item(menu, TrayCommand::Exit, StrId::MenuExit);

        // Double click opens the settings: mark it as the default (bold) item.
        SetMenuDefaultItem(menu, menuId(TrayCommand::OpenSettings), FALSE);
        return menu;
    }

    void showMenu(POINT pt) {
        if (menuOpen || !owner) return;
        HMENU menu = buildMenu();
        if (!menu) return;
        menuOpen = true;
        // Classic tray-menu fix: without a foreground owner the menu would not close when the
        // user clicks elsewhere.
        SetForegroundWindow(owner);
        UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN;
        flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
        const int cmd = TrackPopupMenuEx(menu, flags, pt.x, pt.y, owner, nullptr);
        PostMessageW(owner, WM_NULL, 0, 0);
        DestroyMenu(menu);   // destroys the submenus too
        menuOpen = false;
        if (cmd >= static_cast<int>(kMenuBase) && cmd <= static_cast<int>(menuId(TrayCommand::OpenQuickPanel))) {
            post(static_cast<TrayCommand>(cmd - static_cast<int>(kMenuBase)));
        } else if (cmd >= static_cast<int>(kUserPresetBase) && cmd < static_cast<int>(kUserPresetBase) + 50) {
            post(TrayCommand::ApplyUserPreset, cmd - static_cast<int>(kUserPresetBase));
        }
    }

    void leftClickAction() {
        if (state.leftClickTogglesPause) {
            post(TrayCommand::TogglePause);
        } else if (panel.visible()) {
            panel.hide();
        } else if (GetTickCount64() - panel.lastHideTick() > 350) {   // this click just closed it
            openPanel();
        }
    }
};

TrayIcon::TrayIcon() : impl_(std::make_unique<Impl>()) { impl_->self = this; }

TrayIcon::~TrayIcon() {
    impl_->panel.destroy();
    remove();
}

bool TrayIcon::create(HINSTANCE instance, HWND owner, UINT commandMsg) {
    remove();
    impl_->instance = instance;
    impl_->owner = owner;
    impl_->commandMsg = commandMsg;
    if (!impl_->callbackMsg) impl_->callbackMsg = RegisterWindowMessageW(L"SnowFall.TrayCallback");
    if (!impl_->taskbarCreatedMsg) impl_->taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    if (!owner || !impl_->callbackMsg) return false;
    impl_->created = true;
    // If the shell is not ready yet the add fails; it is retried on "TaskbarCreated".
    return impl_->add();
}

void TrayIcon::remove() {
    if (!impl_) return;
    if (impl_->owner) KillTimer(impl_->owner, kClickTimerId);
    impl_->panel.hide();
    if (impl_->created && impl_->owner) {
        NOTIFYICONDATAW nid = impl_->baseData();
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    impl_->created = false;
    impl_->added = false;
    impl_->destroyIcons();
}

bool TrayIcon::handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    Impl& d = *impl_;
    if (!d.created || hwnd != d.owner) return false;

    if (d.taskbarCreatedMsg && msg == d.taskbarCreatedMsg) {
        // Explorer restarted (or the taskbar DPI changed): the icon is gone, add it again.
        d.destroyIcons();
        d.add();
        return false;   // not consumed: the owner may want to react too
    }

    if (msg == WM_TIMER && wp == kClickTimerId) {
        KillTimer(hwnd, kClickTimerId);
        d.leftClickAction();
        if (result) *result = 0;
        return true;
    }

    if (msg != d.callbackMsg) return false;
    if (result) *result = 0;
    if (HIWORD(lp) != kIconId) return true;

    switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            // The second click of a double click also produces NIN_SELECT: ignore it.
            if (GetTickCount() - d.lastDoubleClickTick <= GetDoubleClickTime()) break;
            if (d.state.leftClickTogglesPause) {
                // Wait for a possible double click (which opens the settings instead). The timer
                // also coalesces the duplicated NIN_KEYSELECT the shell sends for Enter.
                SetTimer(hwnd, kClickTimerId, GetDoubleClickTime(), nullptr);
            } else {
                d.leftClickAction();
            }
            break;
        case WM_LBUTTONDBLCLK:
            KillTimer(hwnd, kClickTimerId);
            d.lastDoubleClickTick = GetTickCount();
            d.panel.hide();
            d.post(TrayCommand::OpenSettings);
            break;
        case WM_CONTEXTMENU: {
            // NOTIFYICON_VERSION_4: wParam carries the anchor point (cursor, or the icon itself
            // when the menu was requested from the keyboard).
            POINT pt{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)};
            d.showMenu(pt);
            break;
        }
        default:
            break;
    }
    return true;
}

void TrayIcon::setState(const TrayState& state) {
    impl_->state = state;
    impl_->modify();
    if (impl_->panel.visible()) impl_->panel.update(impl_->settings, state);
}

void TrayIcon::setSettings(const Settings& settings) {
    impl_->settings = settings;
    if (impl_->panel.visible()) impl_->panel.update(settings, impl_->state);
}

void TrayIcon::showQuickPanel() {
    if (impl_->owner) impl_->openPanel();
}

void TrayIcon::hideQuickPanel() { impl_->panel.hide(); }

void TrayIcon::showBalloon(const std::wstring& title, const std::wstring& text, bool warning) {
    Impl& d = *impl_;
    if (!d.added) return;
    NOTIFYICONDATAW nid = d.baseData();
    nid.uFlags = NIF_INFO;
    copyText(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
    copyText(nid.szInfo, ARRAYSIZE(nid.szInfo), text);
    nid.dwInfoFlags = (warning ? NIIF_WARNING : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

}  // namespace snow
