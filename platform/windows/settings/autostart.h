// "Start with Windows" via HKCU\Software\Microsoft\Windows\CurrentVersion\Run
// (per-user, no elevation, no services).
#pragma once
#include <string>

namespace snow::autostart {

// Value name used under the Run key.
inline constexpr wchar_t kValueName[] = L"SnowFall";

// Registers `exePath` (quoted, plus " --autostart") or removes the value. Returns success.
bool setEnabled(bool enabled, const std::wstring& exePath);
// True when the Run value exists and points at `exePath` (case-insensitive path compare).
bool isEnabled(const std::wstring& exePath);

}  // namespace snow::autostart
