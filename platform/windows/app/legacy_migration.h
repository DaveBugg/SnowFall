// One-time migration from the old product name "Windows Snow" (WindowsSnow.exe) to SnowFall.
// UI thread, called at startup before settings are loaded.
#pragma once
#include <string>

namespace snow::legacy {

// If the old app ("WindowsSnowHost" window) is running, ask it to exit so two snow
// overlays never run at the same time. Returns true if one was found.
bool stopOldInstance();

// Copies %LOCALAPPDATA%\WindowsSnow\settings.json into the new data directory when the new
// directory has no settings yet (the old folder is left untouched). Returns true if copied.
bool migrateSettings(const std::wstring& newDataDir);

// Removes the old "WindowsSnow" Run value. Returns true if it existed (the caller then
// re-enables autostart under the new name so the user's choice is kept).
bool takeOldAutostart();

}  // namespace snow::legacy
