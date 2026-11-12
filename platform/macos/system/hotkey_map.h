// Hotkeys on macOS: the settings keep the portable Windows-style spelling of
// core/settings/hotkey.h ("Ctrl+Alt+S"), so settings files are interchangeable between
// platforms. On the Mac, Alt is the Option key and Win is the Command key:
//   stored "Ctrl+Alt+S"  <->  shown "Ctrl+Option+S",   stored "Win+Shift+F1" <-> "Cmd+Shift+F1"
// Pure C++ (no Carbon headers); the Carbon constants are reproduced as numbers and checked
// against <Carbon/Carbon.h> in tests/mac_unit_tests.cpp.
#pragma once
#include <string>

#include "settings/hotkey.h"

namespace snow::mac {

// Carbon modifier bits (Events.h): cmdKey, shiftKey, optionKey, controlKey.
inline constexpr unsigned kCarbonCmd = 0x0100;
inline constexpr unsigned kCarbonShift = 0x0200;
inline constexpr unsigned kCarbonOption = 0x0800;
inline constexpr unsigned kCarbonControl = 0x1000;

// Windows virtual-key code (as produced by parseHotkey) -> macOS virtual key code (kVK_*).
// false when the key does not exist on Mac keyboards (Pause, F21..F24).
bool vkToMacKeyCode(unsigned vk, unsigned& keyCode);
// kMod* bits -> Carbon modifier mask for RegisterEventHotKey.
unsigned modifiersToCarbon(unsigned mods);

// Accepts Mac spellings too: Option/Opt/⌥ -> Alt, Cmd/Command/⌘ -> Win, Control/⌃ -> Ctrl,
// ⇧ -> Shift; returns the canonical stored form ("" when invalid).
std::string canonicalHotkeyText(const std::string& text);
// Canonical stored form -> what the Mac UI shows ("Ctrl+Option+S"). "" stays "".
std::string macHotkeyDisplay(const std::string& canonical);

}  // namespace snow::mac
