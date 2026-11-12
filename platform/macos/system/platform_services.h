// Small macOS services used by the app shell: login item, global hotkeys, crash reports,
// Accessibility permission. Main thread. Objective-C++ header (no ObjC types exposed).
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "settings/hotkey.h"

namespace snow::mac {

// ---- Open at login (SMAppService.mainAppService, macOS 13+) ---------------------------
namespace autostart {
enum class Status { Disabled, Enabled, RequiresApproval, Unavailable };
Status status();
inline bool isEnabled() { const Status s = status(); return s == Status::Enabled || s == Status::RequiresApproval; }
// false + `error` on failure. RequiresApproval: the user has to allow it in System Settings
// > General > Login Items (openLoginItemsSettings()).
bool setEnabled(bool on, std::string* error);
void openLoginItemsSettings();
}  // namespace autostart

// ---- global hotkeys (Carbon RegisterEventHotKey: no permission needed) ------------------
class HotkeyManager {
public:
    HotkeyManager();
    ~HotkeyManager();
    // Registers `hk` under `id` (replacing a previous one). false when the key has no Mac
    // equivalent or Carbon refused it.
    bool registerHotkey(int id, const Hotkey& hk);
    void unregister(int id);
    void unregisterAll();
    std::function<void(int id)> onHotkey;   // main thread

    // Is the combination used by macOS itself (System Settings > Keyboard > Shortcuts)?
    static bool reservedBySystem(const Hotkey& hk);
    // Can it be registered right now (probe register + unregister)?
    static bool probe(const Hotkey& hk);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- crash reports -----------------------------------------------------------------------
// Uncaught Objective-C exceptions and fatal signals write <dir>/crash-<time>.txt (signal,
// backtrace of the crashing thread; no personal data) and then let the process die.
void installCrashHandler(const std::string& dir);

// ---- Accessibility (needed only for the interaction event tap) ---------------------------
bool accessibilityTrusted();
void requestAccessibility();         // adds the app to the list (system prompt)
void openAccessibilitySettings();    // System Settings > Privacy & Security > Accessibility

}  // namespace snow::mac
