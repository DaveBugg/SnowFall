// Portable part of the settings module: JSON mapping, migrations and sanitize().
// No OS headers here; file I/O lives in the platform layer (SettingsStore).
#include "settings/settings_store.h"



#include <algorithm>
#include <cmath>
#include <iterator>
#include <string_view>
#include <utility>

#include "core/skins.h"
#include "diagnostics/log.h"
#include "settings/hotkey.h"
#include "settings/json.h"

// NOTE on logging: file paths contain the user name, so only error codes are logged.

namespace snow {

namespace {

constexpr size_t kMaxExcludedProcesses = 256;
constexpr size_t kMaxUserPresets = 20;
constexpr size_t kMaxPresetNameChars = 40;   // Unicode code points

// ---------------------------------------------------------------- enum <-> string

template <typename E>
struct EnumName {
    E value;
    const char* name;
};

constexpr EnumName<WeatherPreset> kPresetNames[] = {
    {WeatherPreset::Light, "light"}, {WeatherPreset::Normal, "normal"}, {WeatherPreset::Blizzard, "blizzard"},
    {WeatherPreset::Thaw, "thaw"}};
constexpr EnumName<PerfProfile> kProfileNames[] = {
    {PerfProfile::Low, "low"}, {PerfProfile::Balanced, "balanced"}, {PerfProfile::High, "high"}};
constexpr EnumName<Tool> kToolNames[] = {
    {Tool::Mitten, "mitten"}, {Tool::Shovel, "shovel"}, {Tool::Snowball, "snowball"}};
constexpr EnumName<UiTheme> kUiThemeNames[] = {
    {UiTheme::Night, "night"}, {UiTheme::Globe, "globe"}, {UiTheme::Postcard, "postcard"}};
constexpr EnumName<ExcludedSnowMode> kExcludedSnowModeNames[] = {
    {ExcludedSnowMode::Behind, "behind"}, {ExcludedSnowMode::Around, "around"}};
constexpr EnumName<Language> kLanguageNames[] = {
    {Language::Auto, "auto"}, {Language::Russian, "ru"}, {Language::English, "en"}};

template <typename E, size_t N>
const char* enumToString(const EnumName<E> (&table)[N], E value) {
    for (const auto& e : table) {
        if (e.value == value) return e.name;
    }
    return table[0].name;
}

// ---------------------------------------------------------------- tolerant readers
// Each reader leaves `out` alone when the key is missing or has the wrong type.

void readBool(const json::Value& obj, std::string_view key, bool& out) {
    if (const json::Value* v = obj.find(key); v && v->isBool()) out = v->asBool();
}

int toInt(double d) {
    if (!std::isfinite(d)) return 0;
    return static_cast<int>(std::llround(std::clamp(d, -2147483648.0, 2147483647.0)));
}

void readInt(const json::Value& obj, std::string_view key, int& out) {
    if (const json::Value* v = obj.find(key); v && v->isNumber()) out = toInt(v->asNumber());
}

void readString(const json::Value& obj, std::string_view key, std::string& out) {
    if (const json::Value* v = obj.find(key); v && v->isString()) out = v->asString();
}

template <typename E, size_t N>
void readEnum(const json::Value& obj, std::string_view key, const EnumName<E> (&table)[N], E& out) {
    const json::Value* v = obj.find(key);
    if (!v || !v->isString()) return;
    for (const auto& e : table) {
        if (v->asString() == e.name) {
            out = e.value;
            return;
        }
    }
}

// Section object or nullptr (missing / not an object).
const json::Value* section(const json::Value& root, std::string_view key) {
    const json::Value* v = root.find(key);
    return v && v->isObject() ? v : nullptr;
}

// ---------------------------------------------------------------- migrations
// kMigrations[N] upgrades a parsed document from schema version N to N + 1, in place,
// before it is mapped onto Settings. A file without "schemaVersion" counts as version 0.

// 0 -> 1: early prototype builds stored the weather knobs and the autostart flag as flat
// top-level keys:
//     { "preset": "normal", "intensity": 55, "wind": 10, "autostart": true }
// Version 1 moves "preset"/"intensity"/"wind" into the "weather" section and renames
// "autostart" to "startupWithWindows". Keys already present in the new location win.
void migrate0to1(json::Value& root) {
    if (json::Value* weather = root.find("weather"); !weather || !weather->isObject()) {
        root.set("weather", json::Value::object());
    }
    for (const char* key : {"preset", "intensity", "wind"}) {
        if (const json::Value* legacy = root.find(key)) {
            json::Value moved = *legacy;
            root.remove(key);
            json::Value* weather = root.find("weather");
            if (!weather->find(key)) weather->set(key, std::move(moved));
        }
    }
    if (const json::Value* legacy = root.find("autostart")) {
        json::Value moved = *legacy;
        root.remove("autostart");
        if (!root.find("startupWithWindows")) root.set("startupWithWindows", std::move(moved));
    }
    root.set("schemaVersion", 1);
}

using Migration = void (*)(json::Value&);
constexpr Migration kMigrations[] = {migrate0to1};
static_assert(std::size(kMigrations) == static_cast<size_t>(kSettingsSchemaVersion),
              "add a migration for every schema version bump");

// ---------------------------------------------------------------- mapping

json::Value weatherToTree(const WeatherSettings& ws) {
    json::Value w = json::Value::object();
    w.set("preset", enumToString(kPresetNames, ws.preset));
    w.set("intensity", ws.intensity);
    w.set("wind", ws.wind);
    w.set("gustiness", ws.gustiness);
    w.set("sizePercent", ws.sizePercent);
    w.set("fallSpeedPercent", ws.fallSpeedPercent);
    w.set("opacityPercent", ws.opacityPercent);
    w.set("skyPerMonitor", ws.skyPerMonitor);
    return w;
}

void weatherFromTree(const json::Value& w, WeatherSettings& ws) {
    readEnum(w, "preset", kPresetNames, ws.preset);
    readInt(w, "intensity", ws.intensity);
    readInt(w, "wind", ws.wind);
    readInt(w, "gustiness", ws.gustiness);
    readInt(w, "sizePercent", ws.sizePercent);
    readInt(w, "fallSpeedPercent", ws.fallSpeedPercent);
    readInt(w, "opacityPercent", ws.opacityPercent);
    readBool(w, "skyPerMonitor", ws.skyPerMonitor);
}

json::Value toTree(const Settings& s) {
    using json::Value;
    Value root = Value::object();
    root.set("schemaVersion", s.schemaVersion);
    root.set("enabled", s.enabled);
    root.set("startupWithWindows", s.startupWithWindows);

    root.set("weather", weatherToTree(s.weather));

    Value& a = root.set("accumulation", Value::object());
    a.set("enabled", s.accumulation.enabled);
    a.set("onWindows", s.accumulation.onWindows);
    a.set("onTaskbar", s.accumulation.onTaskbar);
    a.set("onScreenBottom", s.accumulation.onScreenBottom);
    a.set("maxHeightPxAt96Dpi", s.accumulation.maxHeightPxAt96Dpi);
    a.set("shakeOffOnMove", s.accumulation.shakeOffOnMove);
    a.set("skipMaximized", s.accumulation.skipMaximized);
    a.set("dropSnowOnClose", s.accumulation.dropSnowOnClose);
    a.set("driftOpacityPercent", s.accumulation.driftOpacityPercent);
    a.set("shakeToClear", s.accumulation.shakeToClear);
    a.set("naturalCaps", s.accumulation.naturalCaps);
    a.set("fallingSnowmen", s.accumulation.fallingSnowmen);
    a.set("snowmenIntervalMinutes", s.accumulation.snowmenIntervalMinutes);
    a.set("snowmenNeedHeavySnow", s.accumulation.snowmenNeedHeavySnow);
    a.set("snowmenMaxAlive", s.accumulation.snowmenMaxAlive);
    a.set("snowplow", s.accumulation.snowplow);
    a.set("snowplowDelayMinutes", s.accumulation.snowplowDelayMinutes);
    a.set("snowplowAllTaskbars", s.accumulation.snowplowAllTaskbars);
    a.set("icicles", s.accumulation.icicles);

    Value& i = root.set("interaction", Value::object());
    i.set("enabled", s.interaction.enabled);
    i.set("holdKeyEnabled", s.interaction.holdKeyEnabled);
    i.set("tool", enumToString(kToolNames, s.interaction.tool));
    i.set("snowmanSkin", s.interaction.snowmanSkin);
    i.set("brushRadiusPxAt96Dpi", s.interaction.brushRadiusPxAt96Dpi);
    i.set("shovelWidthPxAt96Dpi", s.interaction.shovelWidthPxAt96Dpi);
    i.set("blockClicksWhileHeld", s.interaction.blockClicksWhileHeld);
    i.set("objectsOnAllMonitors", s.interaction.objectsOnAllMonitors);
    i.set("umbrellaCursor", s.interaction.umbrellaCursor);

    Value& p = root.set("performance", Value::object());
    p.set("profile", enumToString(kProfileNames, s.performance.profile));
    p.set("fpsCap", s.performance.fpsCap);
    p.set("pauseOnFullscreen", s.performance.pauseOnFullscreen);
    p.set("reduceOnBattery", s.performance.reduceOnBattery);
    p.set("animations", s.performance.animations);

    Value& h = root.set("hotkeys", Value::object());
    h.set("pauseToggle", s.hotkeys.pauseToggle);
    h.set("clearSnow", s.hotkeys.clearSnow);
    h.set("interactionToggle", s.hotkeys.interactionToggle);

    Value& sys = root.set("system", Value::object());
    sys.set("language", enumToString(kLanguageNames, s.system.language));
    sys.set("uiTheme", enumToString(kUiThemeNames, s.system.uiTheme));
    sys.set("trayLeftClickTogglesPause", s.system.trayLeftClickTogglesPause);
    Value& excluded = sys.set("excludedProcesses", Value::array());
    for (const std::string& name : s.system.excludedProcesses) excluded.push(name);
    sys.set("excludedSnowMode", enumToString(kExcludedSnowModeNames, s.system.excludedSnowMode));

    Value& festive = root.set("festive", Value::object());
    festive.set("garland", s.festive.garland);
    festive.set("fireworks", s.festive.fireworks);

    Value& presets = root.set("userPresets", Value::array());
    for (const UserPreset& up : s.userPresets) {
        Value& entry = presets.push(Value::object());
        entry.set("name", up.name);
        entry.set("weather", weatherToTree(up.weather));
        entry.set("maxHeightPxAt96Dpi", up.maxHeightPxAt96Dpi);
        entry.set("driftOpacityPercent", up.driftOpacityPercent);
    }

    Value& privacy = root.set("privacy", Value::object());
    privacy.set("telemetry", s.privacy.telemetry);
    return root;
}

void fromTree(const json::Value& root, Settings& s) {
    readBool(root, "enabled", s.enabled);
    readBool(root, "startupWithWindows", s.startupWithWindows);

    if (const json::Value* w = section(root, "weather")) weatherFromTree(*w, s.weather);
    if (const json::Value* a = section(root, "accumulation")) {
        readBool(*a, "enabled", s.accumulation.enabled);
        readBool(*a, "onWindows", s.accumulation.onWindows);
        readBool(*a, "onTaskbar", s.accumulation.onTaskbar);
        readBool(*a, "onScreenBottom", s.accumulation.onScreenBottom);
        readInt(*a, "maxHeightPxAt96Dpi", s.accumulation.maxHeightPxAt96Dpi);
        readBool(*a, "shakeOffOnMove", s.accumulation.shakeOffOnMove);
        readBool(*a, "skipMaximized", s.accumulation.skipMaximized);
        readBool(*a, "dropSnowOnClose", s.accumulation.dropSnowOnClose);
        readInt(*a, "driftOpacityPercent", s.accumulation.driftOpacityPercent);
        readBool(*a, "shakeToClear", s.accumulation.shakeToClear);
        readBool(*a, "naturalCaps", s.accumulation.naturalCaps);
        readBool(*a, "fallingSnowmen", s.accumulation.fallingSnowmen);
        readInt(*a, "snowmenIntervalMinutes", s.accumulation.snowmenIntervalMinutes);
        readBool(*a, "snowmenNeedHeavySnow", s.accumulation.snowmenNeedHeavySnow);
        readInt(*a, "snowmenMaxAlive", s.accumulation.snowmenMaxAlive);
        readBool(*a, "snowplow", s.accumulation.snowplow);
        readInt(*a, "snowplowDelayMinutes", s.accumulation.snowplowDelayMinutes);
        readBool(*a, "snowplowAllTaskbars", s.accumulation.snowplowAllTaskbars);
        readBool(*a, "icicles", s.accumulation.icicles);
    }
    if (const json::Value* i = section(root, "interaction")) {
        readBool(*i, "enabled", s.interaction.enabled);
        readBool(*i, "holdKeyEnabled", s.interaction.holdKeyEnabled);
        readEnum(*i, "tool", kToolNames, s.interaction.tool);
        readString(*i, "snowmanSkin", s.interaction.snowmanSkin);
        readInt(*i, "brushRadiusPxAt96Dpi", s.interaction.brushRadiusPxAt96Dpi);
        readInt(*i, "shovelWidthPxAt96Dpi", s.interaction.shovelWidthPxAt96Dpi);
        readBool(*i, "blockClicksWhileHeld", s.interaction.blockClicksWhileHeld);
        readBool(*i, "objectsOnAllMonitors", s.interaction.objectsOnAllMonitors);
        readBool(*i, "umbrellaCursor", s.interaction.umbrellaCursor);
    }
    if (const json::Value* p = section(root, "performance")) {
        readEnum(*p, "profile", kProfileNames, s.performance.profile);
        readInt(*p, "fpsCap", s.performance.fpsCap);
        readBool(*p, "pauseOnFullscreen", s.performance.pauseOnFullscreen);
        readBool(*p, "reduceOnBattery", s.performance.reduceOnBattery);
        readBool(*p, "animations", s.performance.animations);
    }
    if (const json::Value* h = section(root, "hotkeys")) {
        readString(*h, "pauseToggle", s.hotkeys.pauseToggle);
        readString(*h, "clearSnow", s.hotkeys.clearSnow);
        readString(*h, "interactionToggle", s.hotkeys.interactionToggle);
    }
    if (const json::Value* sys = section(root, "system")) {
        readEnum(*sys, "language", kLanguageNames, s.system.language);
        readEnum(*sys, "uiTheme", kUiThemeNames, s.system.uiTheme);
        readBool(*sys, "trayLeftClickTogglesPause", s.system.trayLeftClickTogglesPause);
        readEnum(*sys, "excludedSnowMode", kExcludedSnowModeNames, s.system.excludedSnowMode);
        if (const json::Value* list = sys->find("excludedProcesses"); list && list->isArray()) {
            s.system.excludedProcesses.clear();
            for (size_t k = 0; k < list->size(); ++k) {
                if (list->at(k).isString()) s.system.excludedProcesses.push_back(list->at(k).asString());
            }
        }
    }
    if (const json::Value* f = section(root, "festive")) {
        readBool(*f, "garland", s.festive.garland);
        readBool(*f, "fireworks", s.festive.fireworks);
    }
    if (const json::Value* list = root.find("userPresets"); list && list->isArray()) {
        s.userPresets.clear();
        for (size_t k = 0; k < list->size(); ++k) {
            const json::Value& e = list->at(k);
            const json::Value* name = e.find("name");
            if (!e.isObject() || !name || !name->isString()) continue;   // bad entry: skipped
            UserPreset up;
            up.name = name->asString();
            if (const json::Value* w = section(e, "weather")) weatherFromTree(*w, up.weather);
            readInt(e, "maxHeightPxAt96Dpi", up.maxHeightPxAt96Dpi);
            readInt(e, "driftOpacityPercent", up.driftOpacityPercent);
            s.userPresets.push_back(std::move(up));
        }
    }
    // "privacy.telemetry" is deliberately not read: it is always false.
}

// ---------------------------------------------------------------- sanitize helpers

void trimAscii(std::string& text) {
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t begin = 0, end = text.size();
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    text = text.substr(begin, end - begin);
}

// Lower-cases a UTF-8 string without any OS dependency: ASCII, Latin-1 Supplement,
// Latin Extended-A (pairs), Greek and Cyrillic are folded; other code points are kept.
// Returns false for malformed UTF-8. (Used for case-insensitive process-name matching.)
bool lowerUtf8(std::string& name) {
    std::u32string cps;
    for (size_t i = 0; i < name.size();) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        char32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else return false;
        if (i + n > name.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(name[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += n;
        if (cp >= U'A' && cp <= U'Z') cp += 0x20;
        else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 0x20;                 // Latin-1
        else if (((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177)) && (cp & 1) == 0)
            cp += 1;                                                                  // Latin Ext-A pairs
        else if (((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) && (cp & 1) == 1)
            cp += 1;
        else if (cp == 0x178) cp = 0xFF;                                             // Ÿ -> ÿ
        else if (cp >= 0x391 && cp <= 0x3AB && cp != 0x3A2) cp += 0x20;              // Greek
        else if (cp >= 0x410 && cp <= 0x42F) cp += 0x20;                             // Cyrillic А-Я
        else if (cp >= 0x400 && cp <= 0x40F) cp += 0x50;                             // Ѐ-Џ (Ё ...)
        cps.push_back(cp);
    }
    std::string out;
    out.reserve(name.size());
    for (char32_t cp : cps) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    name = std::move(out);
    return true;
}


// Trims ASCII whitespace and lower-cases. Returns false for malformed UTF-8.
bool normalizeProcessName(std::string& name) {
    trimAscii(name);
    return lowerUtf8(name);
}

// Cuts a (valid) UTF-8 string after `maxChars` code points.
void truncateCodePoints(std::string& text, size_t maxChars) {
    size_t chars = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;   // continuation byte
        if (chars++ == maxChars) {
            text.resize(i);
            return;
        }
    }
}

void clampWeather(WeatherSettings& w) {
    w.intensity = std::clamp(w.intensity, 0, 100);
    w.wind = std::clamp(w.wind, -100, 100);
    w.gustiness = std::clamp(w.gustiness, 0, 100);
    w.sizePercent = std::clamp(w.sizePercent, 50, 200);
    w.fallSpeedPercent = std::clamp(w.fallSpeedPercent, 25, 200);
    w.opacityPercent = std::clamp(w.opacityPercent, 10, 100);
    if (w.preset < WeatherPreset::Light || w.preset > WeatherPreset::Thaw) w.preset = WeatherPreset::Normal;
}

// Names: trimmed, 1..40 code points, unique case-insensitively (first wins), max 20 entries.
void sanitizeUserPresets(std::vector<UserPreset>& presets) {
    std::vector<UserPreset> cleaned;
    std::vector<std::string> seen;   // lower-cased names
    for (UserPreset p : presets) {
        if (cleaned.size() >= kMaxUserPresets) break;
        trimAscii(p.name);
        std::string key = p.name;
        if (!lowerUtf8(key)) continue;   // malformed UTF-8
        truncateCodePoints(p.name, kMaxPresetNameChars);
        trimAscii(p.name);               // truncation may expose trailing spaces
        if (p.name.empty()) continue;
        key = p.name;
        lowerUtf8(key);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(std::move(key));
        clampWeather(p.weather);
        p.maxHeightPxAt96Dpi = std::clamp(p.maxHeightPxAt96Dpi, 0, 100);
        p.driftOpacityPercent = std::clamp(p.driftOpacityPercent, 10, 100);
        cleaned.push_back(std::move(p));
    }
    presets = std::move(cleaned);
}

// "random" or a known id from core/skins.h (trimmed, ASCII-lower-cased); anything else -> "random".
void sanitizeSnowmanSkin(std::string& skin) {
    trimAscii(skin);
    for (char& c : skin) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    for (const SnowmanSkin& known : kSnowmanSkins) {
        if (skin == known.id) return;
    }
    skin = "random";
}

// Invalid -> empty (disabled); valid -> canonical spelling.
void sanitizeHotkey(std::string& text) {
    Hotkey hk;
    text = parseHotkey(text, hk) ? formatHotkey(hk) : std::string();
}

}  // namespace

// ---------------------------------------------------------------- public: model

void sanitize(Settings& s) {
    s.schemaVersion = kSettingsSchemaVersion;

    clampWeather(s.weather);
    sanitizeUserPresets(s.userPresets);

    s.accumulation.maxHeightPxAt96Dpi = std::clamp(s.accumulation.maxHeightPxAt96Dpi, 0, 100);
    s.accumulation.driftOpacityPercent = std::clamp(s.accumulation.driftOpacityPercent, 10, 100);
    s.accumulation.snowplowDelayMinutes = std::clamp(s.accumulation.snowplowDelayMinutes, 1, 120);
    s.accumulation.snowmenIntervalMinutes = std::clamp(s.accumulation.snowmenIntervalMinutes, 1, 60);
    s.accumulation.snowmenMaxAlive = std::clamp(s.accumulation.snowmenMaxAlive, 1, 10);

    s.interaction.brushRadiusPxAt96Dpi = std::clamp(s.interaction.brushRadiusPxAt96Dpi, 12, 48);
    s.interaction.shovelWidthPxAt96Dpi = std::clamp(s.interaction.shovelWidthPxAt96Dpi, 48, 160);

    s.performance.fpsCap = s.performance.fpsCap <= 45 ? 30 : 60;

    // Enums that were set from out-of-range integers fall back to the defaults.
    if (s.performance.profile < PerfProfile::Low || s.performance.profile > PerfProfile::High) {
        s.performance.profile = PerfProfile::Balanced;
    }
    if (s.interaction.tool < Tool::Mitten || s.interaction.tool > Tool::Snowball) {
        s.interaction.tool = Tool::Snowball;
    }
    if (s.system.excludedSnowMode < ExcludedSnowMode::Behind ||
        s.system.excludedSnowMode > ExcludedSnowMode::Around) {
        s.system.excludedSnowMode = ExcludedSnowMode::Behind;
    }
    sanitizeSnowmanSkin(s.interaction.snowmanSkin);
    if (s.system.uiTheme < UiTheme::Night || s.system.uiTheme > UiTheme::Postcard) {
        s.system.uiTheme = UiTheme::Night;
    }
    if (s.system.language < Language::Auto || s.system.language > Language::English) {
        s.system.language = Language::Auto;
    }

    sanitizeHotkey(s.hotkeys.pauseToggle);
    sanitizeHotkey(s.hotkeys.clearSnow);
    sanitizeHotkey(s.hotkeys.interactionToggle);

    std::vector<std::string> cleaned;
    for (std::string name : s.system.excludedProcesses) {
        if (cleaned.size() >= kMaxExcludedProcesses) break;
        if (!normalizeProcessName(name) || name.empty()) continue;
        if (std::find(cleaned.begin(), cleaned.end(), name) == cleaned.end()) cleaned.push_back(std::move(name));
    }
    s.system.excludedProcesses = std::move(cleaned);

    s.privacy.telemetry = false;
}

std::string toJson(const Settings& s) {
    Settings clean = s;
    sanitize(clean);   // the file never contains out-of-range values
    return json::write(toTree(clean), 2) + "\n";
}

bool fromJson(const std::string& text, Settings& out, std::string* error) {
    json::Value root;
    if (!json::parse(text, root, error)) return false;
    if (!root.isObject()) {
        if (error) *error = "settings root is not a JSON object";
        return false;
    }

    int version = 0;
    if (const json::Value* v = root.find("schemaVersion"); v && v->isNumber()) {
        version = std::max(0, toInt(v->asNumber()));
    }
    if (version > kSettingsSchemaVersion) {
        SNOW_LOGW("settings: schemaVersion %d is newer than supported %d; loading known fields only",
                  version, kSettingsSchemaVersion);
    }
    for (int v = version; v < kSettingsSchemaVersion; ++v) kMigrations[v](root);

    Settings s;
    fromTree(root, s);
    sanitize(s);
    out = std::move(s);
    return true;
}

}  // namespace snow
