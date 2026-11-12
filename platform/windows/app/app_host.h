// AppHost: process lifetime, hidden host window, tray, hotkeys, wiring of all modules.
// Everything here runs on the UI thread.
#pragma once
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "app/interaction_controller.h"
#include "app/performance_governor.h"
#include "app/sim_runner.h"
#include "app/ui/settings_window.h"
#include "app/ui/tray_icon.h"
#include "core/layout.h"
#include "core/settings.h"
#include "core/snapshot.h"
#include "tracker/window_tracker.h"
#include "rendering/d3d11/overlay_system.h"
#include "settings/settings_store.h"

namespace snow {

struct LaunchOptions {
    bool autostart = false;      // started by the Run key: stay silent in the tray
    bool openSettings = false;
    bool customDataDir = false;  // --data-dir given (portable/tests): no legacy migration
};

class AppHost {
public:
    static constexpr wchar_t kHostClass[] = L"SnowFallHost";
    // Messages other instances may post to the host window.
    // wParam: 1 = open settings, 2 = exit, 3 = clear snow, 4 = toggle pause, 5 = call snowplow,
    // 6 = toggle interaction mode, 7 = fireworks
    static constexpr UINT kMsgRemote = WM_APP + 120;

    AppHost();
    ~AppHost();

    bool initialize(HINSTANCE instance, const LaunchOptions& options);
    int run();

private:
    enum : UINT {
        kMsgTray = WM_APP + 100,
        kMsgRenderFailed = WM_APP + 101,
        kMsgCycleTool = WM_APP + 102,
    };
    enum : UINT_PTR { kTimerSave = 2 };
    enum : int { kHotkeyPause = 1, kHotkeyClear = 2, kHotkeyInteraction = 3, kHotkeyProbe = 99 };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void onLayout(std::shared_ptr<const DesktopLayout> layout);
    bool ownPopupOpen() const;    // a menu / message box / topmost popup of ours is visible
    void onTrayCommand(TrayCommand cmd);
    void onHotkey(int id);
    void applySettings(Settings next, bool fromSettingsWindow);
    void scheduleSave();
    void saveNow();
    void registerHotkeys(bool notifyConflicts);
    void unregisterHotkeys();
    std::wstring validateHotkey(const std::string& text);
    void refreshPowerState();
    void updateGovernor();
    void updateTray();
    void openSettings();
    void syncTrackerWindows();
    void setPaused(bool paused);
    void applyUserPreset(std::size_t index);
    void clearSnow();
    void shutdown();
    DiagnosticsInfo diagnostics() const;
    std::wstring statusText() const;
    std::wstring holdKeysText() const;   // e.g. L"Ctrl+Alt"; empty when hold mode is off
    const wchar_t* tx(const wchar_t* ru, const wchar_t* en) const { return russian_ ? ru : en; }
    void applyLanguage();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    bool running_ = false;
    bool russian_ = true;
    std::wstring exePath_;
    std::wstring logPath_;

    SettingsStore store_;
    Settings settings_;
    bool saveFailedReported_ = false;

    SnapshotExchange snapshots_;
    SimRunner runner_{snapshots_};
    OverlaySystem overlay_;
    WindowTracker tracker_;
    InteractionController interaction_{runner_, snapshots_};
    TrayIcon tray_;
    SettingsWindow settingsWindow_;
    HWND trackedSettingsHwnd_ = nullptr;
    TrayState lastTray_;
    bool trayStateValid_ = false;

    std::shared_ptr<const DesktopLayout> layout_;
    std::vector<MonitorInfo> monitors_;
    GovernorInputs inputs_;
    GovernorDecision decision_;
    bool decisionValid_ = false;
    bool sessionNotify_ = false;
    bool hotkeyRegistered_[4] = {};
};

}  // namespace snow
