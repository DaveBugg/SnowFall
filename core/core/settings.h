// In-memory settings model. Persisted as JSON by settings/SettingsStore
// (%LOCALAPPDATA%/SnowFall/settings.json). Plain data, no Win32.
#pragma once
#include <string>
#include <vector>

namespace snow {

inline constexpr int kSettingsSchemaVersion = 1;

// Thaw («Оттепель»): sparse wet flakes, drifts slowly settle/melt, drops fall from edges,
// icicles grow faster.
enum class WeatherPreset { Light, Normal, Blizzard, Thaw };    // json: "light" | "normal" | "blizzard" | "thaw"
enum class PerfProfile { Low, Balanced, High };                // json: "low" | "balanced" | "high"
enum class Tool { Mitten, Shovel, Snowball };                  // json: "mitten" | "shovel" | "snowball"
enum class Language { Auto, Russian, English };                // json: "auto" | "ru" | "en"
// How falling snow treats windows of excluded applications (they never hold drifts):
// Behind — flakes pass BEHIND those windows (invisible over them); Around — flakes flow
// around those windows like around the umbrella cursor.
enum class ExcludedSnowMode { Behind, Around };                // json: "behind" | "around"
// Visual theme of the settings window (same features in every theme, different look):
// A «Зимняя ночь», B «Снежный шар», C «Новогодняя открытка».
enum class UiTheme { Night, Globe, Postcard };                 // json: "night" | "globe" | "postcard"

struct WeatherSettings {
    WeatherPreset preset = WeatherPreset::Normal;
    int intensity = 55;          // 0..100 %
    int wind = 10;               // -100..100 (negative = to the left)
    int gustiness = 20;          // 0..100 %
    int sizePercent = 100;       // 50..200 %
    int fallSpeedPercent = 100;  // 25..200 %
    int opacityPercent = 100;    // 10..100 %  — FALLING FLAKES (and snow dust) only
    // Each monitor gets its own snowfall starting at its own top edge (needed when monitors
    // are stacked: otherwise the upper monitor's taskbar catches all the snow and the lower
    // one stays empty). false: one sky over the whole virtual desktop. json: weather.skyPerMonitor
    bool skyPerMonitor = true;
    bool operator==(const WeatherSettings&) const = default;
};

struct AccumulationSettings {
    bool enabled = true;
    bool onWindows = true;
    bool onTaskbar = true;
    bool onScreenBottom = true;
    int maxHeightPxAt96Dpi = 36;   // 0..100
    bool shakeOffOnMove = true;    // false = "Сохранять", true = "Осыпать" (default)
    bool skipMaximized = true;     // no surface on maximized windows / windows touching the monitor top
    bool dropSnowOnClose = true;   // closed window's snow falls as particles instead of vanishing
    int driftOpacityPercent = 90;  // 10..100 %  — drifts only (json: accumulation.driftOpacityPercent)
    bool shakeToClear = false;     // shaking a window back and forth throws its snow cap off (opt-in)
    bool naturalCaps = true;       // irregular, lumpy max-height profile instead of a flat full layer
    bool fallingSnowmen = true;    // heavy accumulation: a snowman occasionally drops from the sky
    // Average time between falling snowmen, 1..60 min (each wait is randomised ±40%, never
    // shorter than 1 minute). json: accumulation.snowmenIntervalMinutes
    int snowmenIntervalMinutes = 2;
    // true: snowmen only fall while drifts are mostly full (heavy snow); false: on schedule,
    // onto any visible drift with enough snow to land on. json: accumulation.snowmenNeedHeavySnow
    bool snowmenNeedHeavySnow = true;
    int snowmenMaxAlive = 3;       // 1..10 auto snowmen at once. json: accumulation.snowmenMaxAlive
    bool snowplow = true;          // toy snowplow clears the taskbar if left uncleared too long
    int snowplowDelayMinutes = 10; // 1..120: how long the taskbar drift must stay (mostly) full
    // A snowplow on every taskbar (each with its own timer); taskbars that sit side by side at
    // the same height form one continuous route. false: primary taskbar only.
    // json: accumulation.snowplowAllTaskbars
    bool snowplowAllTaskbars = true;
    bool icicles = true;           // icicles grow under long-lying snow caps, break off on a jerk
    bool operator==(const AccumulationSettings&) const = default;
};

struct InteractionSettings {
    bool enabled = true;                 // interaction feature available at all
    bool holdKeyEnabled = true;          // hold Alt+Shift to enter interaction mode temporarily
    Tool tool = Tool::Snowball;          // default tool: snowball
    // Snowman look: "random" or a skin id from core/skins.h (e.g. "pirate"). json: interaction.snowmanSkin
    std::string snowmanSkin = "random";
    int brushRadiusPxAt96Dpi = 28;       // mitten radius, 12..48
    int shovelWidthPxAt96Dpi = 96;       // shovel width, 48..160
    // While the hold chord is held, EVERY left/right press goes to the tool (no desktop
    // rubber-band selection, no clicks into apps). Toggled mode keeps clicks off snow working.
    bool blockClicksWhileHeld = true;
    // Snowballs / snowmen (desktop layer) on every monitor instead of the primary one only.
    // The snowplow always stays on the primary monitor's taskbar.
    bool objectsOnAllMonitors = false;
    // Falling snow flows around the mouse pointer like under an umbrella (also outside
    // interaction mode); a fast mouse flick stirs a small gust.
    bool umbrellaCursor = true;
    bool operator==(const InteractionSettings&) const = default;
};

struct PerformanceSettings {
    PerfProfile profile = PerfProfile::Balanced;
    int fpsCap = 60;                     // 30 or 60
    bool pauseOnFullscreen = true;
    bool reduceOnBattery = true;
    bool animations = true;              // false: no falling particles / dust, static drifts only
    bool operator==(const PerformanceSettings&) const = default;
};

struct HotkeySettings {                  // "Ctrl+Alt+S" style; empty string = disabled
    std::string pauseToggle = "Ctrl+Alt+S";
    std::string clearSnow = "Ctrl+Alt+X";
    std::string interactionToggle = "Ctrl+Alt+I";
    bool operator==(const HotkeySettings&) const = default;
};

struct SystemSettings {
    Language language = Language::Auto;
    UiTheme uiTheme = UiTheme::Night;   // json: system.uiTheme
    // false (default): left click opens the quick panel (presets, intensity, pause) next to
    // the tray icon; true: left click toggles pause.
    bool trayLeftClickTogglesPause = false;
    std::vector<std::string> excludedProcesses;   // exe names (UTF-8), e.g. "game.exe"; case-insensitive
    ExcludedSnowMode excludedSnowMode = ExcludedSnowMode::Behind;   // json: system.excludedSnowMode
    bool operator==(const SystemSettings&) const = default;
};

struct FestiveSettings {                 // json: "festive"
    bool garland = false;                // blinking light garland along the taskbar top edge(s) (opt-in)
    bool fireworks = true;               // fireworks at local midnight on New Year's Eve (~90 s)
    bool operator==(const FestiveSettings&) const = default;
};

// A user-saved look («Мои пресеты»): weather + the drift look. json: "userPresets": [ ... ]
struct UserPreset {
    std::string name;                    // UTF-8, 1..40 chars, unique (case-insensitive)
    WeatherSettings weather;
    int maxHeightPxAt96Dpi = 36;
    int driftOpacityPercent = 90;
    bool operator==(const UserPreset&) const = default;
};

struct PrivacySettings {
    bool telemetry = false;              // always false: the app never sends anything
    bool operator==(const PrivacySettings&) const = default;
};

struct Settings {
    int schemaVersion = kSettingsSchemaVersion;
    bool enabled = true;                 // false = paused at startup
    bool startupWithWindows = false;
    WeatherSettings weather;
    AccumulationSettings accumulation;
    InteractionSettings interaction;
    PerformanceSettings performance;
    HotkeySettings hotkeys;
    SystemSettings system;
    FestiveSettings festive;
    std::vector<UserPreset> userPresets; // at most 20
    PrivacySettings privacy;
    bool operator==(const Settings&) const = default;
};

// Decided at runtime by the PerformanceGovernor (battery saver, profile...) and passed to
// the simulation together with Settings.
struct RuntimeTuning {
    float particleScale = 1.0f;          // multiplies the particle budget (0..1.5)
    bool animationsEnabled = true;
    bool operator==(const RuntimeTuning&) const = default;
};

}  // namespace snow
