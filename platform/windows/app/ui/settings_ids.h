// Control ids of the settings window, the quick panel and the process picker, plus test seams.
#pragma once
#include <windows.h>

namespace snow::ui {

inline constexpr int kTabCount = 7;   // Снег, Сугробы, Инструменты, Производительность, Праздник, Система, О программе

enum CtlId : int {
    IDC_FIRST = 1000,
    IDC_HEADER = IDC_FIRST,
    IDC_NAV_FIRST,                            // 7 navigation items
    IDC_NAV_LAST = IDC_NAV_FIRST + kTabCount - 1,
    IDC_SWATCH_NIGHT,                         // header theme swatches (radio group)
    IDC_SWATCH_GLOBE,
    IDC_SWATCH_POSTCARD,
    IDC_HDR_TOOL_SNOWBALL,                    // header tool icons (radio group: snowball, mitten, shovel)
    IDC_HDR_TOOL_MITTEN,
    IDC_HDR_TOOL_SHOVEL,
    IDC_PREVIEW,
    IDC_PREVIEW_RESET,
    IDC_COUNTDOWN,
    IDC_STATUS_LINE,
    IDC_RESTORE,
    IDC_CLOSE,

    // «Снег»
    IDC_PRESET_LIGHT,                         // preset scene cards (radio group, contiguous)
    IDC_PRESET_NORMAL,
    IDC_PRESET_BLIZZARD,
    IDC_PRESET_THAW,
    IDC_UP_HEADING,
    IDC_UP_LIST,                              // user preset chips
    IDC_UP_SAVE,
    IDC_UP_NAME,
    IDC_UP_OK,
    IDC_UP_MSG,
    IDC_INTENSITY,
    IDC_INTENSITY_VAL,
    IDC_WIND,
    IDC_WIND_VAL,
    IDC_GUSTINESS,
    IDC_GUSTINESS_VAL,
    IDC_FLAKE_SIZE,
    IDC_FLAKE_SIZE_VAL,
    IDC_FALLSPEED,
    IDC_FALLSPEED_VAL,
    IDC_OPACITY,
    IDC_OPACITY_VAL,
    IDC_SKY_PER_MONITOR,
    IDC_SKY_PER_MONITOR_HINT,

    // «Сугробы»
    IDC_ACC_ENABLED,
    IDC_ACC_WINDOWS,
    IDC_ACC_TASKBAR,
    IDC_ACC_BOTTOM,
    IDC_MAXHEIGHT,
    IDC_MAXHEIGHT_VAL,
    IDC_SHAKE_KEEP,
    IDC_SHAKE_DROP,
    IDC_SKIP_MAXIMIZED,
    IDC_DROP_ON_CLOSE,
    IDC_DRIFT_OPACITY,
    IDC_DRIFT_OPACITY_VAL,
    IDC_SHAKE_TO_CLEAR,
    IDC_NATURAL_CAPS,
    IDC_ICICLES,
    IDC_FALLING_SNOWMEN,
    IDC_SNOWMEN_INTERVAL,
    IDC_SNOWMEN_INTERVAL_VAL,
    IDC_SNOWMEN_HEAVY,
    IDC_SNOWMEN_MAX,
    IDC_SNOWMEN_MAX_VAL,
    IDC_SNOWPLOW,
    IDC_SNOWPLOW_DELAY,
    IDC_SNOWPLOW_DELAY_VAL,
    IDC_SNOWPLOW_ALL,
    IDC_SNOWPLOW_ALL_HINT,
    IDC_CLEAR_ALL,

    // «Инструменты»
    IDC_INTERACTION,
    IDC_HOLD_KEY,
    IDC_BLOCK_CLICKS,
    IDC_BLOCK_CLICKS_HINT,
    IDC_ALL_MONITORS,
    IDC_UMBRELLA,
    IDC_TOOL_MITTEN,
    IDC_TOOL_SHOVEL,
    IDC_TOOL_SNOWBALL,
    IDC_TOOL_TIP,
    IDC_SKIN_HEADING,
    IDC_SKIN_FIRST,                           // «Случайный» + every kSnowmanSkins entry (radio group)
    IDC_SKIN_LAST = IDC_SKIN_FIRST + 40,
    IDC_MITTEN_RADIUS,
    IDC_MITTEN_RADIUS_VAL,
    IDC_SHOVEL_WIDTH,
    IDC_SHOVEL_WIDTH_VAL,

    // «Производительность»
    IDC_PROFILE_LOW,
    IDC_PROFILE_BALANCED,
    IDC_PROFILE_HIGH,
    IDC_FPS_30,
    IDC_FPS_60,
    IDC_PAUSE_FULLSCREEN,
    IDC_REDUCE_BATTERY,
    IDC_ANIMATIONS,
    IDC_DIAG_FPS,
    IDC_DIAG_FRAME,
    IDC_DIAG_SIM,
    IDC_DIAG_PARTICLES,
    IDC_DIAG_SURFACES,
    IDC_DIAG_BALLS,
    IDC_DIAG_MONITORS,
    IDC_DIAG_WINDOWS,
    IDC_DIAG_RESETS,
    IDC_DIAG_POWER,
    IDC_DIAG_STATUS,

    // «Праздник»
    IDC_GARLAND,
    IDC_FIREWORKS,
    IDC_FIREWORKS_NOW,
    IDC_FESTIVE_SCENE,

    // «Система»
    IDC_STARTUP,
    IDC_LEFT_CLICK,
    IDC_LANG_AUTO,                            // language segments (radio group, contiguous)
    IDC_LANG_RU,
    IDC_LANG_EN,
    IDC_THEME_NIGHT,                          // theme segments (radio group, contiguous)
    IDC_THEME_GLOBE,
    IDC_THEME_POSTCARD,
    IDC_HOTKEY_PAUSE,
    IDC_HOTKEY_PAUSE_STATUS,
    IDC_HOTKEY_CLEAR,
    IDC_HOTKEY_CLEAR_STATUS,
    IDC_HOTKEY_INTERACTION,
    IDC_HOTKEY_INTERACTION_STATUS,
    IDC_EXCL_LIST,
    IDC_EXCL_EDIT,
    IDC_EXCL_ADD,
    IDC_EXCL_REMOVE,
    IDC_EXCL_PICK,
    IDC_EXCL_MODE_LBL,
    IDC_EXCL_BEHIND,                          // excludedSnowMode (radio group)
    IDC_EXCL_AROUND,

    // «О программе»
    IDC_ABOUT_ICON,
    IDC_ABOUT_VERSION,
    IDC_LOG_PATH,
    IDC_OPEN_LOG_FOLDER,
    IDC_ABOUT_TITLE,
    IDC_POWERED_BY,
    IDC_LINK_GITHUB,
    IDC_LINK_TELEGRAM,

    // Process picker window
    IDC_PICK_LIST = 1500,
    IDC_PICK_ADD,
    IDC_PICK_CANCEL,
    IDC_PICK_HINT,

    // Quick panel window
    IDC_QP_LIGHT = 1600,                      // mini scene cards (radio group, contiguous)
    IDC_QP_NORMAL,
    IDC_QP_BLIZZARD,
    IDC_QP_THAW,
    IDC_QP_INTENSITY,
    IDC_QP_INTENSITY_LBL,
    IDC_QP_INTENSITY_VAL,
    IDC_QP_WIND,
    IDC_QP_WIND_LBL,
    IDC_QP_WIND_VAL,
    IDC_QP_PRESETS_LBL,
    IDC_QP_PRESETS,
    IDC_QP_PAUSE,
    IDC_QP_CLEAR,
    IDC_QP_FIREWORKS,
    IDC_QP_SETTINGS,
    IDC_QP_HINT,
    IDC_QP_TOOL_LBL,
    IDC_QP_TOOL_SNOWBALL,                     // radio group: snowball, mitten, shovel
    IDC_QP_TOOL_MITTEN,
    IDC_QP_TOOL_SHOVEL,
    IDC_QP_EXCL_LBL,
    IDC_QP_EXCL_PICK,
    IDC_QP_EXCL_BEHIND,
    IDC_QP_EXCL_AROUND,
};

}  // namespace snow::ui

// Seams used by tests/integration/ui (not part of the app-facing contract).
namespace snow::uitest {

// Sent to the settings window: restores defaults exactly like the button, without the
// confirmation MessageBox.
inline constexpr UINT kMsgRestoreDefaultsNoConfirm = WM_APP + 0x51;
// Sent to the settings window or the quick panel: returns bit 0 = animation timer running,
// bit 1 = diagnostics timer running.
inline constexpr UINT kMsgQueryTimers = WM_APP + 0x52;
// Sent to the settings window: advances the live preview by wParam simulated seconds
// synchronously and repaints it; returns the preview's total snow mass (px^2, truncated).
inline constexpr UINT kMsgPreviewAdvance = WM_APP + 0x53;

// Forces the layout DPI of settings windows created afterwards (0 = use the window's real DPI).
void setDpiOverride(UINT dpi);

// Finds a control of the settings window by id (searches all pages / containers).
HWND findControl(HWND settingsWindow, int id);

// URL a link button of the «О программе» page opens (nullptr if `link` is not a link).
const wchar_t* linkUrl(HWND link);

// Injects "now" for the New Year countdown (nullptr = real clock).
void setToday(const SYSTEMTIME* localTime);

}  // namespace snow::uitest
