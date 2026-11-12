#include "user_presets.h"

#include "ui_util.h"

namespace snow::ui {

PresetNameError validatePresetName(const std::wstring& raw, const Settings& s) {
    const std::wstring name = trim(raw);
    if (name.empty() || static_cast<int>(name.size()) > kMaxPresetNameChars) return PresetNameError::Length;
    for (const UserPreset& p : s.userPresets) {
        if (equalsNoCase(widen(p.name), name)) return PresetNameError::Duplicate;
    }
    if (static_cast<int>(s.userPresets.size()) >= kMaxUserPresets) return PresetNameError::TooMany;
    return PresetNameError::None;
}

UserPreset makeUserPreset(const Settings& s, const std::wstring& name) {
    UserPreset p;
    p.name = narrow(trim(name));
    p.weather = s.weather;
    p.maxHeightPxAt96Dpi = s.accumulation.maxHeightPxAt96Dpi;
    p.driftOpacityPercent = s.accumulation.driftOpacityPercent;
    return p;
}

void applyUserPreset(Settings& s, const UserPreset& p) {
    s.weather = p.weather;
    s.accumulation.maxHeightPxAt96Dpi = p.maxHeightPxAt96Dpi;
    s.accumulation.driftOpacityPercent = p.driftOpacityPercent;
}

}  // namespace snow::ui
