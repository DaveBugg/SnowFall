// «Мои пресеты»: validation and conversion helpers shared by the settings window and the
// quick panel.
#pragma once
#include <string>

#include "core/settings.h"

namespace snow::ui {

inline constexpr int kMaxUserPresets = 20;
inline constexpr int kMaxPresetNameChars = 40;

enum class PresetNameError { None, Length, Duplicate, TooMany };

// `name` is the raw user input (trimmed internally).
PresetNameError validatePresetName(const std::wstring& name, const Settings& s);
UserPreset makeUserPreset(const Settings& s, const std::wstring& name);
void applyUserPreset(Settings& s, const UserPreset& p);   // weather + max height + drift opacity

}  // namespace snow::ui
