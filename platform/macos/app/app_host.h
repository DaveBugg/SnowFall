// AppHost (macOS): process lifetime and wiring of every module, the Mac counterpart of
// platform/windows/app/app_host.*. Main thread only. Objective-C++ header.
#pragma once
#import <AppKit/AppKit.h>

#include <memory>
#include <string>
#include <vector>

#include "app/interaction_controller.h"
#include "app/status_item.h"
#include "core/layout.h"
#include "core/settings.h"
#include "core/snapshot.h"
#include "overlay/overlay_system.h"
#include "runtime/performance_governor.h"
#include "runtime/sim_runner.h"
#include "settings/settings_store.h"
#include "system/platform_services.h"
#include "system/system_events.h"
#include "tracker/window_tracker.h"

#import "SFBridge.h"

namespace snow::mac {

struct LaunchOptions {
    bool openSettings = false;
    bool customDataDir = false;
    bool firstRun = false;          // no settings file existed
    std::string logPath;
};

SimPlatformHooks makeMacSimHooks();

class AppHost {
public:
    AppHost();
    ~AppHost();

    bool initialize(const LaunchOptions& options);
    void shutdown();

    // Commands (menu, hotkeys, UI bridge, remote instances).
    void applySettings(Settings next, bool fromUi);
    void applySettingsJson(const std::string& json);
    void togglePause() { setPaused(settings_.enabled); }
    void clearSnow() { runner_.clearSnow(); }
    void callSnowplow() { runner_.triggerSnowplow(); }
    void fireworks() { runner_.triggerFireworks(); }
    void openSettings();
    void quit();
    void remoteCommand(const std::string& cmd);

    // Data for the UI bridge.
    std::string settingsJson() const;
    std::string diagnosticsJson() const;
    std::string validateHotkey(const std::string& text) const;   // "" = OK
    const Settings& settings() const { return settings_; }
    std::string logPath() const { return logPath_; }

private:
    void onLayout(std::shared_ptr<const DesktopLayout> layout);
    void onMenu(MenuCommand cmd, int arg);
    void onHotkey(int id);
    void setPaused(bool paused);
    void applyUserPreset(std::size_t index);
    void registerHotkeys();
    void updateGovernor();
    void updateStatusItem();
    void pushSettingsToUi(bool fromUi);
    void scheduleSave();
    void saveNow();
    void showAccessibilityExplanation();
    void showFirstRunWelcome();
    void showRenderFailure(const std::string& why);
    std::string statusText() const;
    std::string holdKeysText() const;
    void setupUi();

    std::string logPath_;
    std::string dataDir_;
    SettingsStore store_;
    Settings settings_;
    bool running_ = false;
    bool saveFailedReported_ = false;

    SnapshotExchange snapshots_;
    SimRunner runner_{snapshots_, makeMacSimHooks()};
    OverlaySystem overlay_;
    WindowTracker tracker_;
    InteractionController interaction_{runner_, snapshots_};
    SystemEvents system_;
    HotkeyManager hotkeys_;
    bool hotkeyOk_[4] = {};

    std::shared_ptr<const DesktopLayout> layout_;
    std::vector<ScreenInfo> screens_;
    GovernorInputs inputs_;
    GovernorDecision decision_;
    bool decisionValid_ = false;
    bool renderOk_ = false;

    SFStatusController* status_ = nil;
    id<SFAppActions> actions_ = nil;
    id<SFSettingsWindow> settingsWindow_ = nil;
    id<SFQuickPanel> quickPanel_ = nil;
    NSTimer* saveTimer_ = nil;
};

}  // namespace snow::mac
