// Menu-bar status item: monochrome template snowflake. Left click -> quick panel (Swift UI)
// or, without the Swift UI / with "left click pauses", the action chosen by the app; right
// click (or Ctrl+click) -> the same menu as the Windows tray. Main thread.
#pragma once
#import <AppKit/AppKit.h>

#include <functional>
#include <string>
#include <vector>

#include "core/settings.h"

namespace snow::mac {

enum class MenuCommand {
    LeftClick,          // the icon itself (app decides: quick panel / pause / menu)
    TogglePause,
    PresetLight,
    PresetNormal,
    PresetBlizzard,
    PresetThaw,
    ApplyUserPreset,    // + index
    ToggleInteraction,
    ToolMitten,
    ToolShovel,
    ToolSnowball,
    ClearSnow,
    CallSnowplow,
    Fireworks,
    ToggleHideAll,
    OpenSettings,
    ToggleAutostart,
    OpenSettingsFile,   // fallback UI only (no Swift settings window in this build)
    ReloadSettingsFile, // fallback UI only
    OpenLogFolder,
    Quit,
    OpenUpdateInfo,     // «Доступно обновление …» -> settings on the About page
};

struct StatusMenuState {
    bool paused = false;          // user pause or auto pause
    bool autoPaused = false;
    bool hidden = false;
    bool interaction = false;
    bool autostart = false;
    WeatherPreset preset = WeatherPreset::Normal;
    Tool tool = Tool::Snowball;
    std::vector<std::string> userPresets;
    std::string statusText;
    std::string holdKeys;         // "Ctrl+Option" or empty
    bool hasSettingsWindow = true;
    std::string updateVersion;    // newer version available ("" = none)
    bool operator==(const StatusMenuState&) const = default;
};

}  // namespace snow::mac

@interface SFStatusController : NSObject
- (instancetype)initWithHandler:(std::function<void(snow::mac::MenuCommand, int)>)handler;
- (void)setState:(const snow::mac::StatusMenuState&)state;
- (void)popUpMenu;
- (void)remove;
@property(nonatomic, readonly) NSStatusBarButton* button;
@end
