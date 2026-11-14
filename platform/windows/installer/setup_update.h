// SnowFallSetup /UPDATE (started by the app's updater as "/S /UPDATE"): a silent install that
// keeps the user's current choices instead of the silent defaults (which would e.g. turn
// autostart on). Header-only so the unit tests can check it without the installer.
#pragma once
#include <cwchar>

namespace snow::setup {

// What the existing installation has now (each item: present AND pointing at our exe).
struct ExistingInstall {
    bool startMenuShortcut = false;
    bool desktopShortcut = false;
    bool autostart = false;   // HKCU\...\Run\SnowFall starts our exe
};

struct InstallChoices {
    bool startMenu = true;    // silent defaults
    bool desktop = false;
    bool autostart = true;
};

inline bool isUpdateSwitch(const wchar_t* arg) { return arg && _wcsicmp(arg, L"/UPDATE") == 0; }

// /UPDATE: every choice follows the existing installation (the app is relaunched normally).
inline InstallChoices keepExistingChoices(const ExistingInstall& e) {
    InstallChoices c;
    c.startMenu = e.startMenuShortcut;
    c.desktop = e.desktopShortcut;
    c.autostart = e.autostart;
    return c;
}

}  // namespace snow::setup
