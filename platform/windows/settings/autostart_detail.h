// Internal helpers behind snow::autostart (not part of the public contract). They take the
// Run value name explicitly so unit tests can use a throw-away name instead of the real
// "SnowFall" value.
#pragma once
#include <string>

namespace snow::autostart::detail {

// The command line stored in the Run value: "<exePath>" --autostart
std::wstring commandLineFor(const std::wstring& exePath);

bool setEnabledNamed(const wchar_t* valueName, bool enabled, const std::wstring& exePath);
bool isEnabledNamed(const wchar_t* valueName, const std::wstring& exePath);

}  // namespace snow::autostart::detail
