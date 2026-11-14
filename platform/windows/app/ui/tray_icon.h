// TrayIcon: notification-area icon + context menu. UI thread only.
//
// PUBLIC API IS A FIXED CONTRACT used by the platform app layer. Keep state behind Impl.
#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/settings.h"

namespace snow {

enum class TrayCommand {
    TogglePause,
    PresetLight,
    PresetNormal,
    PresetBlizzard,
    OpenSettings,
    ToggleInteraction,
    ToolMitten,
    ToolShovel,
    ToolSnowball,
    ClearSnow,
    ToggleHideAll,     // "Скрыть всё": hide overlays entirely (separate from pause)
    ToggleAutostart,
    Exit,
    CallSnowplow,      // send the toy snowplow along the taskbar now
    PresetThaw,        // «Оттепель»
    CallFireworks,     // «Салют!»
    ApplyUserPreset,   // lParam = index into Settings::userPresets
    OpenQuickPanel,    // show the quick panel next to the tray icon
    OpenUpdate,        // «Доступно обновление …»: settings window on the «О программе» page
};

// State reflected in the menu (check marks, labels) and tooltip.
struct TrayState {
    bool paused = false;
    bool hidden = false;
    bool interactionMode = false;
    bool autostart = false;
    WeatherPreset preset = WeatherPreset::Normal;
    Tool tool = Tool::Mitten;
    bool leftClickTogglesPause = true;
    std::vector<std::wstring> userPresetNames;   // «Мои пресеты» submenu (index = ApplyUserPreset lParam)
    std::wstring interactionKeys;                // current hold chord, e.g. L"Ctrl+Alt" (quick panel hint)
    std::wstring updateVersion;                  // newer release available ("0.1.3"); empty = no menu item
};

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Adds the icon. `owner` receives WM_COMMAND-free notifications: the tray posts
    // PostMessage(owner, commandMsg, (WPARAM)TrayCommand, 0) for every chosen command.
    // Internally uses its own callback message on `owner`; the owner's WndProc must forward
    // EVERY message to handleMessage() first.
    bool create(HINSTANCE instance, HWND owner, UINT commandMsg);
    void remove();

    // Returns true if the message was consumed (tray callback, TaskbarCreated re-add...).
    bool handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

    void setState(const TrayState& state);   // updates tooltip/icon; menu is built on demand
    void showBalloon(const std::wstring& title, const std::wstring& text, bool warning);

    // Quick panel (left click when TrayState::leftClickTogglesPause is false). The panel edits a
    // copy of the settings passed here and reports every change through onQuickChange (same
    // semantics as SettingsWindowCallbacks::onChanged); commands still arrive as commandMsg.
    void setSettings(const Settings& settings);
    std::function<void(const Settings&)> onQuickChange;
    void showQuickPanel();    // e.g. for TrayCommand::OpenQuickPanel from elsewhere
    void hideQuickPanel();

    // The app's snowflake icon generated procedurally (original artwork). size in px.
    static HICON createAppIcon(int size, bool paused = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snow
