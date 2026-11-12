// Hotkey capture control: a subclassed single-line EDIT. While focused, the next key combination
// with at least one modifier is captured and shown as canonical text ("Ctrl+Alt+S");
// Backspace/Delete without modifiers clears it (= hotkey disabled).
#pragma once
#include <windows.h>

#include <string>

namespace snow::ui {

// WM_COMMAND notification code sent to the parent when the captured hotkey changed.
inline constexpr WORD HKN_CHANGED = 0x4B01;

// Canonical "Ctrl+Alt+Shift+Win+Key" text for MOD_* bits + virtual key. Empty string when there
// is no modifier or the key is not one of the supported ones (A-Z, 0-9, F1..F24, Space, Tab,
// Esc, Enter, Insert, Delete, Home, End, PageUp, PageDown, Up, Down, Left, Right, Pause).
std::string formatHotkeyText(unsigned mods, UINT vk);

HWND createHotkeyEdit(HINSTANCE instance, HWND parent, int id);
bool isHotkeyEdit(HWND hwnd);
void setHotkeyEditText(HWND edit, const std::string& text);   // does not notify
std::string getHotkeyEditText(HWND edit);

// Core of the capture logic (also the test seam): feeds one non-modifier key press with the given
// MOD_* state. Returns true when the control's text changed (parent was notified).
bool hotkeyEditHandleKey(HWND edit, UINT vk, unsigned mods);

// Test seam: when >= 0, WM_KEYDOWN handling uses these MOD_* bits instead of GetKeyState().
void setHotkeyModifierOverride(int mods);

}  // namespace snow::ui
