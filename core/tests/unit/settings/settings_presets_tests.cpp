// Round-6 fields: thaw preset, icicles, umbrellaCursor, tray default, festive, userPresets.
#include <climits>

#include "settings/json.h"
#include "settings/settings_store.h"
#include "snow_test.h"

using namespace snow;

namespace {

Settings load(const std::string& text) {
    Settings s;
    std::string err;
    const bool ok = fromJson(text, s, &err);
    if (!ok) printf("  unexpected fromJson failure: %s\n", err.c_str());
    CHECK(ok);
    return s;
}

UserPreset preset(const std::string& name) {
    UserPreset p;
    p.name = name;
    return p;
}

std::vector<std::string> names(const Settings& s) {
    std::vector<std::string> out;
    for (const UserPreset& p : s.userPresets) out.push_back(p.name);
    return out;
}

}  // namespace

TEST(r6_new_defaults_in_json) {
    const Settings d;
    CHECK(d.system.trayLeftClickTogglesPause == false);
    CHECK(d.accumulation.icicles && d.interaction.umbrellaCursor);
    CHECK(!d.festive.garland && d.festive.fireworks);
    CHECK(d.userPresets.empty());
    json::Value root;
    CHECK(json::parse(toJson(d), root));
    CHECK(root.find("system")->find("trayLeftClickTogglesPause")->asBool(true) == false);
    CHECK(root.find("accumulation")->find("icicles")->asBool() == true);
    CHECK(root.find("interaction")->find("umbrellaCursor")->asBool() == true);
    CHECK(root.find("festive")->find("garland")->asBool() == false);
    CHECK(root.find("festive")->find("fireworks")->asBool() == true);
    CHECK(root.find("userPresets")->isArray() && root.find("userPresets")->size() == 0);
    // An older file without the new keys loads with the new defaults.
    CHECK(load(R"({"schemaVersion": 1, "accumulation": {}, "interaction": {}, "system": {}})") == Settings{});
}

TEST(r6_thaw_and_flags) {
    CHECK(load(R"({"schemaVersion": 1, "weather": {"preset": "thaw"}})").weather.preset == WeatherPreset::Thaw);
    CHECK(load(R"({"schemaVersion": 1, "weather": {"preset": "Thaw"}})").weather.preset == WeatherPreset::Normal);
    Settings t;
    t.weather.preset = WeatherPreset::Thaw;
    json::Value root;
    CHECK(json::parse(toJson(t), root));
    CHECK(root.find("weather")->find("preset")->asString() == "thaw");
    Settings bad;
    bad.weather.preset = static_cast<WeatherPreset>(4);
    sanitize(bad);
    CHECK(bad.weather.preset == WeatherPreset::Normal);

    const Settings s = load(R"({"schemaVersion": 1, "accumulation": {"icicles": false},
      "interaction": {"umbrellaCursor": false}, "system": {"trayLeftClickTogglesPause": true},
      "festive": {"garland": false, "fireworks": false}})");
    CHECK(!s.accumulation.icicles && !s.interaction.umbrellaCursor && s.system.trayLeftClickTogglesPause);
    CHECK(!s.festive.garland && !s.festive.fireworks);
    CHECK(load(toJson(s)) == s);

    // Wrong types keep the defaults.
    const Settings w = load(R"({"schemaVersion": 1, "accumulation": {"icicles": 0},
      "interaction": {"umbrellaCursor": "no"}, "festive": {"garland": null, "fireworks": [false]}})");
    CHECK(w == Settings{});
    CHECK(load(R"({"schemaVersion": 1, "festive": [true], "userPresets": {"name": "x"}})") == Settings{});
}

TEST(r6_presets_roundtrip) {
    Settings s;
    s.userPresets = {
        {"Тихий вечер", {WeatherPreset::Light, 20, -5, 10, 80, 60, 70}, 24, 60},
        {"Ёлочная метель", {WeatherPreset::Blizzard, 100, 90, 100, 200, 200, 100}, 100, 100},
        {"Thaw 🌨", {WeatherPreset::Thaw, 0, -100, 0, 50, 25, 10}, 0, 10},
    };
    Settings copy = s;
    sanitize(copy);
    CHECK(copy == s);
    const std::string text = toJson(s);
    const Settings back = load(text);
    CHECK(back == s);
    CHECK(toJson(back) == text);

    json::Value root;
    CHECK(json::parse(text, root));
    const json::Value& first = root.find("userPresets")->at(0);
    std::string keys;
    for (size_t i = 0; i < first.size(); ++i) keys += (i ? "," : "") + first.keyAt(i);
    CHECK(keys == "name,weather,maxHeightPxAt96Dpi,driftOpacityPercent");
    CHECK(first.find("name")->asString() == "Тихий вечер");
    CHECK(first.find("weather")->find("preset")->asString() == "light");
    CHECK(first.find("weather")->find("opacityPercent")->asNumber() == 70.0);
    CHECK(text.find("\"Ёлочная метель\"") != std::string::npos);   // raw UTF-8
}

TEST(r6_presets_clamps) {
    Settings s;
    UserPreset lo = preset("lo");
    lo.weather = {static_cast<WeatherPreset>(-3), -1, -101, -1, 49, 24, 9};
    lo.maxHeightPxAt96Dpi = -1;
    lo.driftOpacityPercent = 9;
    UserPreset hi = preset("hi");
    hi.weather = {WeatherPreset::Thaw, 101, 101, 101, 201, 201, 101};
    hi.maxHeightPxAt96Dpi = 101;
    hi.driftOpacityPercent = 101;
    UserPreset ext = preset("ext");
    ext.weather = {WeatherPreset::Light, INT_MAX, INT_MIN, INT_MAX, INT_MIN, INT_MAX, INT_MIN};
    ext.maxHeightPxAt96Dpi = INT_MAX;
    ext.driftOpacityPercent = INT_MIN;
    s.userPresets = {lo, hi, ext};
    sanitize(s);
    CHECK(s.userPresets.size() == 3);
    CHECK((s.userPresets[0].weather == WeatherSettings{WeatherPreset::Normal, 0, -100, 0, 50, 25, 10}));
    CHECK(s.userPresets[0].maxHeightPxAt96Dpi == 0 && s.userPresets[0].driftOpacityPercent == 10);
    CHECK((s.userPresets[1].weather == WeatherSettings{WeatherPreset::Thaw, 100, 100, 100, 200, 200, 100}));
    CHECK(s.userPresets[1].maxHeightPxAt96Dpi == 100 && s.userPresets[1].driftOpacityPercent == 100);
    CHECK((s.userPresets[2].weather == WeatherSettings{WeatherPreset::Light, 100, -100, 100, 50, 200, 10}));
    CHECK(s.userPresets[2].maxHeightPxAt96Dpi == 100 && s.userPresets[2].driftOpacityPercent == 10);

    // Same through JSON.
    const Settings j = load(R"({"schemaVersion": 1, "userPresets": [{"name": "j",
      "weather": {"preset": "storm", "intensity": 500, "wind": -500, "gustiness": -5, "sizePercent": 1,
                  "fallSpeedPercent": 900, "opacityPercent": 0},
      "maxHeightPxAt96Dpi": 1e9, "driftOpacityPercent": -1e9}]})");
    CHECK(j.userPresets.size() == 1);
    CHECK((j.userPresets[0].weather == WeatherSettings{WeatherPreset::Normal, 100, -100, 0, 50, 200, 10}));
    CHECK(j.userPresets[0].maxHeightPxAt96Dpi == 100 && j.userPresets[0].driftOpacityPercent == 10);
}

TEST(r6_presets_names) {
    Settings s;
    s.userPresets = {preset("  Вечер \t"), preset(""), preset("   "), preset("ВЕЧЕР"), preset("вечер"),
                     preset("Day"), preset("dAY "), preset("bad\xFF"), preset("Ёлка"), preset("ёЛКА")};
    sanitize(s);
    const std::vector<std::string> expected = {"Вечер", "Day", "Ёлка"};
    CHECK(names(s) == expected);

    // Length: 40 code points kept, longer truncated on a code-point boundary.
    std::string cyr40;
    for (int i = 0; i < 40; ++i) cyr40 += "Ж";   // 2 bytes each
    Settings len;
    len.userPresets = {preset(cyr40), preset(cyr40 + "ЖЖЖ"), preset(std::string(39, 'a') + "🌨🌨"),
                       preset(std::string(39, 'b') + " xyz"), preset(std::string(45, 'c'))};
    sanitize(len);
    CHECK(len.userPresets.size() == 4);   // the truncated 43-char name duplicates the first
    CHECK(len.userPresets[0].name == cyr40 && len.userPresets[0].name.size() == 80);
    CHECK(len.userPresets[1].name == std::string(39, 'a') + "🌨");    // 4-byte char kept whole
    CHECK(len.userPresets[2].name == std::string(39, 'b'));           // trailing space trimmed
    CHECK(len.userPresets[3].name == std::string(40, 'c'));
    for (const UserPreset& p : len.userPresets) {
        CHECK(json::write(json::Value(p.name)).find("�") == std::string::npos);   // valid UTF-8
    }
}

TEST(r6_presets_max_20) {
    Settings s;
    for (int i = 0; i < 30; ++i) s.userPresets.push_back(preset("P" + std::to_string(i)));
    sanitize(s);
    CHECK(s.userPresets.size() == 20);
    CHECK(s.userPresets.front().name == "P0" && s.userPresets.back().name == "P19");

    // Dropped (empty/duplicate) entries do not count toward the cap.
    Settings d;
    for (int i = 0; i < 50; ++i) d.userPresets.push_back(preset(i % 2 ? "" : "Q" + std::to_string(i / 2)));
    sanitize(d);
    CHECK(d.userPresets.size() == 20 && d.userPresets.back().name == "Q19");

    std::string text = R"({"schemaVersion": 1, "userPresets": [)";
    for (int i = 0; i < 25; ++i) text += (i ? "," : "") + std::string(R"({"name": "J)") + std::to_string(i) + "\"}";
    text += "]}";
    const Settings j = load(text);
    CHECK(j.userPresets.size() == 20 && j.userPresets.back().name == "J19");
}

TEST(r6_presets_bad_entries_skipped) {
    const Settings s = load(R"({"schemaVersion": 1, "userPresets": [
      5, null, "name", [], {}, {"name": 7}, {"name": null},
      {"name": "ok1"},
      {"name": "ok2", "weather": "windy", "maxHeightPxAt96Dpi": "high", "driftOpacityPercent": true},
      {"name": "ok3", "weather": {"intensity": "x", "wind": -30, "preset": 2}, "extra": {"a": 1}},
      {"name": "  "}
    ]})");
    const std::vector<std::string> expected = {"ok1", "ok2", "ok3"};
    CHECK(names(s) == expected);
    const UserPreset def;
    CHECK(s.userPresets[1].weather == def.weather);
    CHECK(s.userPresets[1].maxHeightPxAt96Dpi == def.maxHeightPxAt96Dpi);
    CHECK(s.userPresets[1].driftOpacityPercent == def.driftOpacityPercent);
    CHECK(s.userPresets[2].weather.intensity == def.weather.intensity);
    CHECK(s.userPresets[2].weather.wind == -30);
    CHECK(s.userPresets[2].weather.preset == WeatherPreset::Normal);
    // Not an array -> no presets, load still succeeds.
    CHECK(load(R"({"schemaVersion": 1, "userPresets": "none"})").userPresets.empty());
}
