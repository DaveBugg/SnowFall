// Hotkey string <-> (modifiers, virtual key) conversion, e.g. "Ctrl+Alt+S".
// Pure C++: modifier bits use the same values as Win32 MOD_* so they can be passed to
// RegisterHotKey directly.
#pragma once
#include <string>

namespace snow {

inline constexpr unsigned kModAlt = 0x0001;     // MOD_ALT
inline constexpr unsigned kModCtrl = 0x0002;    // MOD_CONTROL
inline constexpr unsigned kModShift = 0x0004;   // MOD_SHIFT
inline constexpr unsigned kModWin = 0x0008;     // MOD_WIN

struct Hotkey {
    unsigned modifiers = 0;   // kMod* bits
    unsigned vk = 0;          // Win32 virtual-key code; 0 = none
    bool valid() const { return vk != 0 && modifiers != 0; }
    bool operator==(const Hotkey&) const = default;
};

// Accepts tokens separated by '+', case-insensitive: Ctrl/Control, Alt, Shift, Win,
// letters A-Z, digits 0-9, F1..F24, and names: Space, Tab, Esc/Escape, Enter, Insert,
// Delete, Home, End, PageUp, PageDown, Up, Down, Left, Right, Pause.
// Returns false (and leaves `out` untouched) for empty/invalid strings or when there is
// no modifier (bare keys must not be registered globally).
bool parseHotkey(const std::string& text, Hotkey& out);

// Canonical form: "Ctrl+Alt+Shift+Win+<Key>". Empty string for an invalid hotkey.
std::string formatHotkey(const Hotkey& hk);

}  // namespace snow
