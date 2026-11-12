#include <climits>

#include "settings/json.h"
#include "settings/settings_store.h"
#include "snow_test.h"

using namespace snow;

namespace {

Settings loaded(const std::string& text) {
    Settings s;
    std::string err;
    const bool ok = fromJson(text, s, &err);
    if (!ok) printf("  unexpected fromJson failure: %s\n", err.c_str());
    CHECK(ok);
    return s;
}

// The example from spec section 11, verbatim.
const char kSpecExample[] = R"({
  "schemaVersion": 1,
  "enabled": true,
  "startupWithWindows": false,
  "weather": {
    "preset": "normal",
    "intensity": 55,
    "wind": 10,
    "gustiness": 20,
    "sizePercent": 100,
    "fallSpeedPercent": 100
  },
  "accumulation": {
    "enabled": true,
    "onWindows": true,
    "onTaskbar": true,
    "onScreenBottom": true,
    "maxHeightPxAt96Dpi": 36,
    "shakeOffOnMove": false
  },
  "interaction": {
    "enabled": true,
    "brushRadiusPxAt96Dpi": 28
  },
  "performance": {
    "profile": "balanced",
    "fpsCap": 60,
    "pauseOnFullscreen": true,
    "reduceOnBattery": true
  },
  "privacy": {
    "telemetry": false
  }
}
)";

// Every field set to a non-default, in-range value.
Settings nonDefault() {
    Settings s;
    s.enabled = false;
    s.startupWithWindows = true;
    s.weather = {WeatherPreset::Blizzard, 91, -42, 77, 150, 60, 35, false};
    s.accumulation = {false, false, false, false, 64, false, false, false, 45, true, false, false,
                      17, false, 7, false, 77, false, false};
    s.interaction = {false, false, Tool::Mitten, "pirate", 40, 120, false, true, false};
    s.performance = {PerfProfile::Low, 30, false, false, false};
    s.hotkeys = {"Ctrl+Shift+F9", "", "Alt+Win+PageDown"};
    s.system = {Language::Russian, UiTheme::Postcard, true, {"game.exe", "игра.exe", "player.exe"},
                ExcludedSnowMode::Around};
    s.festive = {false, false};
    s.userPresets = {
        {"Тихий вечер", {WeatherPreset::Light, 20, -5, 10, 80, 60, 70}, 24, 60},
        {"Blizzard!", {WeatherPreset::Blizzard, 100, 90, 100, 200, 200, 100, false}, 100, 100},
        {"Оттепель 🌨", {WeatherPreset::Thaw, 0, -100, 0, 50, 25, 10}, 0, 10},
    };
    return s;
}

}  // namespace

TEST(settings_defaults_roundtrip_identity) {
    const Settings defaults;
    const std::string text = toJson(defaults);
    Settings back = nonDefault();
    CHECK(fromJson(text, back));
    CHECK(back == defaults);
    CHECK(toJson(back) == text);
    CHECK(!text.empty() && text.back() == '\n');
}

TEST(settings_non_default_roundtrip_identity) {
    const Settings s = nonDefault();
    Settings copy = s;
    sanitize(copy);
    CHECK(copy == s);   // already sane: sanitize is a no-op
    const Settings back = loaded(toJson(s));
    CHECK(back == s);
    CHECK(back.system.excludedProcesses.size() == 3 && back.system.excludedProcesses[1] == "игра.exe");
}

TEST(settings_json_key_names) {
    json::Value root;
    CHECK(json::parse(toJson(Settings{}), root));
    const auto keys = [&](const char* sectionName) {
        const json::Value* v = sectionName ? root.find(sectionName) : &root;
        std::string joined;
        for (size_t i = 0; v && i < v->size(); ++i) joined += (i ? "," : "") + v->keyAt(i);
        return joined;
    };
    CHECK(keys(nullptr) ==
          "schemaVersion,enabled,startupWithWindows,weather,accumulation,interaction,performance,hotkeys,system,"
          "festive,userPresets,privacy");
    CHECK(keys("festive") == "garland,fireworks");
    CHECK(root.find("userPresets")->isArray() && root.find("userPresets")->size() == 0);
    CHECK(keys("weather") == "preset,intensity,wind,gustiness,sizePercent,fallSpeedPercent,opacityPercent,skyPerMonitor");
    CHECK(keys("accumulation") ==
          "enabled,onWindows,onTaskbar,onScreenBottom,maxHeightPxAt96Dpi,shakeOffOnMove,skipMaximized,dropSnowOnClose,"
          "driftOpacityPercent,shakeToClear,naturalCaps,fallingSnowmen,snowmenIntervalMinutes,"
          "snowmenNeedHeavySnow,snowmenMaxAlive,snowplow,snowplowDelayMinutes,snowplowAllTaskbars,icicles");
    CHECK(keys("interaction") == "enabled,holdKeyEnabled,tool,snowmanSkin,brushRadiusPxAt96Dpi,shovelWidthPxAt96Dpi,"
                                     "blockClicksWhileHeld,objectsOnAllMonitors,umbrellaCursor");
    CHECK(keys("performance") == "profile,fpsCap,pauseOnFullscreen,reduceOnBattery,animations");
    CHECK(keys("hotkeys") == "pauseToggle,clearSnow,interactionToggle");
    CHECK(keys("system") == "language,uiTheme,trayLeftClickTogglesPause,excludedProcesses,excludedSnowMode");
    CHECK(keys("privacy") == "telemetry");
    CHECK(root.find("schemaVersion")->asNumber() == kSettingsSchemaVersion);
    CHECK(root.find("system")->find("excludedProcesses")->isArray());
    CHECK(root.find("privacy")->find("telemetry")->isBool());
}

TEST(settings_spec_example_loads) {
    const Settings s = loaded(kSpecExample);
    CHECK(s.schemaVersion == 1);
    CHECK(s.enabled == true);
    CHECK(s.startupWithWindows == false);
    CHECK(s.weather.preset == WeatherPreset::Normal);
    CHECK(s.weather.intensity == 55);
    CHECK(s.weather.wind == 10);
    CHECK(s.weather.gustiness == 20);
    CHECK(s.weather.sizePercent == 100);
    CHECK(s.weather.fallSpeedPercent == 100);
    CHECK(s.accumulation.enabled && s.accumulation.onWindows && s.accumulation.onTaskbar);
    CHECK(s.accumulation.onScreenBottom);
    CHECK(s.accumulation.maxHeightPxAt96Dpi == 36);
    CHECK(s.accumulation.shakeOffOnMove == false);
    CHECK(s.interaction.enabled == true);
    CHECK(s.interaction.brushRadiusPxAt96Dpi == 28);
    CHECK(s.performance.profile == PerfProfile::Balanced);
    CHECK(s.performance.fpsCap == 60);
    CHECK(s.performance.pauseOnFullscreen && s.performance.reduceOnBattery);
    CHECK(s.privacy.telemetry == false);
    // Fields the example does not mention keep their defaults. The only intentional difference:
    // the spec example has shakeOffOnMove=false, while the app default is now «Осыпать» (true).
    Settings expected;
    expected.accumulation.shakeOffOnMove = false;
    CHECK(s == expected);
}

TEST(settings_spec_example_with_other_values) {
    // Same shape as the spec example but every value changed, to prove each key is read.
    const Settings s = loaded(R"({
      "schemaVersion": 1, "enabled": false, "startupWithWindows": true,
      "weather": {"preset": "blizzard", "intensity": 90, "wind": -30, "gustiness": 5,
                  "sizePercent": 180, "fallSpeedPercent": 40},
      "accumulation": {"enabled": false, "onWindows": false, "onTaskbar": false,
                       "onScreenBottom": false, "maxHeightPxAt96Dpi": 12, "shakeOffOnMove": true},
      "interaction": {"enabled": false, "brushRadiusPxAt96Dpi": 44},
      "performance": {"profile": "high", "fpsCap": 30, "pauseOnFullscreen": false,
                      "reduceOnBattery": false},
      "privacy": {"telemetry": false}
    })");
    CHECK(!s.enabled && s.startupWithWindows);
    CHECK(s.weather.preset == WeatherPreset::Blizzard && s.weather.intensity == 90 && s.weather.wind == -30);
    CHECK(s.weather.gustiness == 5 && s.weather.sizePercent == 180 && s.weather.fallSpeedPercent == 40);
    CHECK(!s.accumulation.enabled && !s.accumulation.onWindows && !s.accumulation.onTaskbar);
    CHECK(!s.accumulation.onScreenBottom && s.accumulation.maxHeightPxAt96Dpi == 12);
    CHECK(s.accumulation.shakeOffOnMove);
    CHECK(!s.interaction.enabled && s.interaction.brushRadiusPxAt96Dpi == 44);
    CHECK(s.performance.profile == PerfProfile::High && s.performance.fpsCap == 30);
    CHECK(!s.performance.pauseOnFullscreen && !s.performance.reduceOnBattery);
}

TEST(settings_invalid_json_leaves_out_untouched) {
    const Settings before = nonDefault();
    for (const char* text : {"", "garbage", "{\"enabled\": ", "[1, 2]", "42", "\"str\"", "null", "{} trailing"}) {
        Settings s = before;
        std::string err;
        CHECK(!fromJson(text, s, &err));
        CHECK(s == before);
        CHECK(!err.empty());
    }
    Settings s = before;
    CHECK(!fromJson("nope", s));   // error pointer is optional
    CHECK(loaded("{}") == Settings{});
    CHECK(loaded("\xEF\xBB\xBF{}") == Settings{});
}

TEST(settings_range_clamps) {
    const auto clampCase = [](int& (*field)(Settings&), int lo, int hi) {
        Settings s;
        field(s) = lo - 1000;
        sanitize(s);
        CHECK(field(s) == lo);
        field(s) = hi + 1000;
        sanitize(s);
        CHECK(field(s) == hi);
        field(s) = lo;
        sanitize(s);
        CHECK(field(s) == lo);
        field(s) = hi;
        sanitize(s);
        CHECK(field(s) == hi);
        field(s) = INT_MIN;
        sanitize(s);
        CHECK(field(s) == lo);
        field(s) = INT_MAX;
        sanitize(s);
        CHECK(field(s) == hi);
    };
    clampCase([](Settings& s) -> int& { return s.weather.intensity; }, 0, 100);
    clampCase([](Settings& s) -> int& { return s.weather.wind; }, -100, 100);
    clampCase([](Settings& s) -> int& { return s.weather.gustiness; }, 0, 100);
    clampCase([](Settings& s) -> int& { return s.weather.sizePercent; }, 50, 200);
    clampCase([](Settings& s) -> int& { return s.weather.fallSpeedPercent; }, 25, 200);
    clampCase([](Settings& s) -> int& { return s.weather.opacityPercent; }, 10, 100);
    clampCase([](Settings& s) -> int& { return s.accumulation.maxHeightPxAt96Dpi; }, 0, 100);
    clampCase([](Settings& s) -> int& { return s.accumulation.driftOpacityPercent; }, 10, 100);
    clampCase([](Settings& s) -> int& { return s.accumulation.snowplowDelayMinutes; }, 1, 120);
    clampCase([](Settings& s) -> int& { return s.accumulation.snowmenIntervalMinutes; }, 1, 60);
    clampCase([](Settings& s) -> int& { return s.accumulation.snowmenMaxAlive; }, 1, 10);
    clampCase([](Settings& s) -> int& { return s.interaction.brushRadiusPxAt96Dpi; }, 12, 48);
    clampCase([](Settings& s) -> int& { return s.interaction.shovelWidthPxAt96Dpi; }, 48, 160);
}

TEST(settings_range_clamps_via_json) {
    const Settings s = loaded(R"({"schemaVersion": 1,
      "weather": {"intensity": 500, "wind": -101, "gustiness": -1, "sizePercent": 49,
                  "fallSpeedPercent": 1e12, "opacityPercent": 0},
      "accumulation": {"maxHeightPxAt96Dpi": 101, "driftOpacityPercent": 5, "snowplowDelayMinutes": 999},
      "interaction": {"brushRadiusPxAt96Dpi": 11, "shovelWidthPxAt96Dpi": -1e300},
      "performance": {"fpsCap": 144}})");
    CHECK(s.weather.intensity == 100);
    CHECK(s.weather.wind == -100);
    CHECK(s.weather.gustiness == 0);
    CHECK(s.weather.sizePercent == 50);
    CHECK(s.weather.fallSpeedPercent == 200);
    CHECK(s.weather.opacityPercent == 10);
    CHECK(s.accumulation.maxHeightPxAt96Dpi == 100);
    CHECK(s.accumulation.driftOpacityPercent == 10);
    CHECK(s.accumulation.snowplowDelayMinutes == 120);
    CHECK(s.interaction.brushRadiusPxAt96Dpi == 12);
    CHECK(s.interaction.shovelWidthPxAt96Dpi == 48);
    CHECK(s.performance.fpsCap == 60);
    // Fractional numbers are rounded to the nearest integer.
    CHECK(loaded(R"({"schemaVersion": 1, "weather": {"intensity": 41.6, "wind": -7.5}})").weather.intensity == 42);
    CHECK(loaded(R"({"schemaVersion": 1, "weather": {"intensity": 41.6, "wind": -7.5}})").weather.wind == -8);
}

TEST(settings_fps_cap_snaps) {
    const auto snap = [](int fps) {
        Settings s;
        s.performance.fpsCap = fps;
        sanitize(s);
        return s.performance.fpsCap;
    };
    CHECK(snap(30) == 30);
    CHECK(snap(60) == 60);
    CHECK(snap(45) == 30);
    CHECK(snap(46) == 60);
    CHECK(snap(0) == 30);
    CHECK(snap(-5) == 30);
    CHECK(snap(1) == 30);
    CHECK(snap(59) == 60);
    CHECK(snap(144) == 60);
    CHECK(snap(INT_MAX) == 60);
    CHECK(snap(INT_MIN) == 30);
}

TEST(settings_sanitize_misc) {
    Settings s;
    s.schemaVersion = 99;
    s.privacy.telemetry = true;
    s.weather.preset = static_cast<WeatherPreset>(17);
    s.performance.profile = static_cast<PerfProfile>(-1);
    s.interaction.tool = static_cast<Tool>(3);
    s.system.language = static_cast<Language>(100);
    s.system.uiTheme = static_cast<UiTheme>(9);
    sanitize(s);
    CHECK(s.schemaVersion == kSettingsSchemaVersion);
    CHECK(s.privacy.telemetry == false);
    CHECK(s == Settings{});

    // Telemetry can never be switched on through the file either.
    CHECK(loaded(R"({"schemaVersion": 1, "privacy": {"telemetry": true}})").privacy.telemetry == false);
    Settings t;
    t.privacy.telemetry = true;
    CHECK(toJson(t) == toJson(Settings{}));
}

TEST(settings_sanitize_hotkeys) {
    Settings s;
    s.hotkeys.pauseToggle = "ctrl + alt + p";      // valid, non-canonical
    s.hotkeys.clearSnow = "S";                     // no modifier
    s.hotkeys.interactionToggle = "Ctrl+Nope";     // unknown key
    sanitize(s);
    CHECK(s.hotkeys.pauseToggle == "Ctrl+Alt+P");
    CHECK(s.hotkeys.clearSnow.empty());
    CHECK(s.hotkeys.interactionToggle.empty());

    const Settings j = loaded(R"({"schemaVersion": 1,
      "hotkeys": {"pauseToggle": "", "clearSnow": "WIN+f5", "interactionToggle": "Ctrl+Alt"}})");
    CHECK(j.hotkeys.pauseToggle.empty());          // explicitly disabled stays disabled
    CHECK(j.hotkeys.clearSnow == "Win+F5");
    CHECK(j.hotkeys.interactionToggle.empty());

    const Settings d = loaded(R"({"schemaVersion": 1, "hotkeys": {"clearSnow": 5}})");
    CHECK(d.hotkeys == HotkeySettings{});          // wrong type / missing -> defaults
}

TEST(settings_sanitize_excluded_processes) {
    Settings s;
    s.system.excludedProcesses = {"  Game.EXE ", "game.exe", "", "   ", "\tИГРА.exe\r\n", "игра.EXE",
                                  "other.exe", "bad\xFF.exe", "GAME.exe", "My Game.exe"};
    sanitize(s);
    const std::vector<std::string> expected = {"game.exe", "игра.exe", "other.exe", "my game.exe"};
    CHECK(s.system.excludedProcesses == expected);

    Settings big;
    for (int i = 0; i < 1000; ++i) big.system.excludedProcesses.push_back("App" + std::to_string(i) + ".exe");
    sanitize(big);
    CHECK(big.system.excludedProcesses.size() == 256);
    CHECK(big.system.excludedProcesses.front() == "app0.exe");
    CHECK(big.system.excludedProcesses.back() == "app255.exe");

    // Duplicates do not count towards the cap.
    Settings dup;
    for (int i = 0; i < 600; ++i) dup.system.excludedProcesses.push_back("p" + std::to_string(i / 2) + ".exe");
    sanitize(dup);
    CHECK(dup.system.excludedProcesses.size() == 256);
    CHECK(dup.system.excludedProcesses.back() == "p255.exe");

    const Settings j = loaded(R"({"schemaVersion": 1,
      "system": {"excludedProcesses": ["A.exe", 5, null, "a.EXE", {"x": 1}, " b.exe", ["c.exe"]]}})");
    const std::vector<std::string> expectedJson = {"a.exe", "b.exe"};
    CHECK(j.system.excludedProcesses == expectedJson);
    CHECK(loaded(R"({"schemaVersion": 1, "system": {"excludedProcesses": "a.exe"}})").system.excludedProcesses.empty());
}

TEST(settings_unknown_keys_ignored) {
    const Settings s = loaded(R"({
      "schemaVersion": 1, "futureFlag": true, "comment": "hello", "nested": {"a": [1, 2, {"b": null}]},
      "weather": {"intensity": 70, "snowflakeShape": "star", "extra": {"deep": []}},
      "privacy": {"telemetry": false, "crashReports": true},
      "unknownSection": {"enabled": false}
    })");
    Settings expected;
    expected.weather.intensity = 70;
    CHECK(s == expected);
}

TEST(settings_wrong_types_tolerated) {
    // Every value has the wrong type: each field keeps its default, the load succeeds.
    const Settings a = loaded(R"({
      "schemaVersion": 1, "enabled": "yes", "startupWithWindows": 1,
      "weather": {"preset": 2, "intensity": "55", "wind": true, "gustiness": null,
                  "sizePercent": [100], "fallSpeedPercent": {"v": 1}, "opacityPercent": "full"},
      "accumulation": {"enabled": 0, "onWindows": "true", "onTaskbar": null, "onScreenBottom": [],
                       "maxHeightPxAt96Dpi": "36", "shakeOffOnMove": 1, "skipMaximized": "no",
                       "dropSnowOnClose": {}, "driftOpacityPercent": "90", "shakeToClear": 0,
                       "naturalCaps": "y", "fallingSnowmen": null, "snowplow": [], "snowplowDelayMinutes": true},
      "interaction": {"enabled": 1, "holdKeyEnabled": "x", "tool": 1, "brushRadiusPxAt96Dpi": false,
                      "shovelWidthPxAt96Dpi": "96", "blockClicksWhileHeld": 0, "objectsOnAllMonitors": "true"},
      "performance": {"profile": true, "fpsCap": "60", "pauseOnFullscreen": 1, "reduceOnBattery": "1",
                      "animations": null},
      "hotkeys": {"pauseToggle": 1, "clearSnow": null, "interactionToggle": ["Ctrl+Alt+I"]},
      "system": {"language": 0, "uiTheme": 2, "trayLeftClickTogglesPause": "true", "excludedProcesses": {"0": "a.exe"}},
      "privacy": {"telemetry": "true"}
    })");
    CHECK(a == Settings{});

    // Sections with the wrong type are skipped as a whole; valid siblings still load.
    const Settings b = loaded(R"({
      "schemaVersion": 1, "enabled": false,
      "weather": [1, 2, 3], "accumulation": "all", "interaction": null, "performance": 60,
      "hotkeys": true, "system": [], "privacy": 0
    })");
    Settings expected;
    expected.enabled = false;
    CHECK(b == expected);

    // One bad field does not affect its neighbours.
    const Settings c = loaded(R"({"schemaVersion": 1, "weather": {"intensity": "loud", "wind": -55}})");
    CHECK(c.weather.intensity == 55 && c.weather.wind == -55);
}

TEST(settings_enum_strings) {
    const auto weatherPreset = [](const char* v) {
        return loaded(std::string(R"({"schemaVersion": 1, "weather": {"preset": ")") + v + "\"}}").weather.preset;
    };
    CHECK(weatherPreset("light") == WeatherPreset::Light);
    CHECK(weatherPreset("normal") == WeatherPreset::Normal);
    CHECK(weatherPreset("blizzard") == WeatherPreset::Blizzard);
    CHECK(weatherPreset("hurricane") == WeatherPreset::Normal);   // unknown -> default
    CHECK(weatherPreset("Light") == WeatherPreset::Normal);       // case-sensitive
    CHECK(weatherPreset("") == WeatherPreset::Normal);

    const auto profile = [](const char* v) {
        return loaded(std::string(R"({"schemaVersion": 1, "performance": {"profile": ")") + v + "\"}}")
            .performance.profile;
    };
    CHECK(profile("low") == PerfProfile::Low);
    CHECK(profile("balanced") == PerfProfile::Balanced);
    CHECK(profile("high") == PerfProfile::High);
    CHECK(profile("ultra") == PerfProfile::Balanced);

    const auto tool = [](const char* v) {
        return loaded(std::string(R"({"schemaVersion": 1, "interaction": {"tool": ")") + v + "\"}}").interaction.tool;
    };
    CHECK(tool("mitten") == Tool::Mitten);
    CHECK(tool("shovel") == Tool::Shovel);
    CHECK(tool("snowball") == Tool::Snowball);
    CHECK(tool("flamethrower") == Tool::Snowball);

    const auto language = [](const char* v) {
        return loaded(std::string(R"({"schemaVersion": 1, "system": {"language": ")") + v + "\"}}").system.language;
    };
    CHECK(language("auto") == Language::Auto);
    CHECK(language("ru") == Language::Russian);
    CHECK(language("en") == Language::English);
    CHECK(language("de") == Language::Auto);

    // Written strings match the documented ones.
    Settings s;
    s.weather.preset = WeatherPreset::Light;
    s.performance.profile = PerfProfile::High;
    s.interaction.tool = Tool::Shovel;
    s.system.language = Language::English;
    json::Value root;
    CHECK(json::parse(toJson(s), root));
    CHECK(root.find("weather")->find("preset")->asString() == "light");
    CHECK(root.find("performance")->find("profile")->asString() == "high");
    CHECK(root.find("interaction")->find("tool")->asString() == "shovel");
    CHECK(root.find("system")->find("language")->asString() == "en");
    CHECK(loaded(toJson(s)) == s);
}

TEST(settings_migration_0_to_1) {
    // Legacy flat layout without schemaVersion.
    const Settings s = loaded(R"({"preset": "blizzard", "intensity": 80, "wind": -25, "autostart": true,
                                  "enabled": false})");
    CHECK(s.schemaVersion == kSettingsSchemaVersion);
    CHECK(s.weather.preset == WeatherPreset::Blizzard);
    CHECK(s.weather.intensity == 80);
    CHECK(s.weather.wind == -25);
    CHECK(s.startupWithWindows == true);
    CHECK(s.enabled == false);
    CHECK(s.weather.gustiness == Settings{}.weather.gustiness);

    // Explicit version 0, legacy values out of range -> migrated, then clamped.
    const Settings z = loaded(R"({"schemaVersion": 0, "intensity": 900, "wind": "strong"})");
    CHECK(z.weather.intensity == 100);
    CHECK(z.weather.wind == Settings{}.weather.wind);

    // Keys already in the new place win over the legacy flat ones.
    const Settings both = loaded(R"({"intensity": 5, "wind": 7, "autostart": true, "startupWithWindows": false,
                                     "weather": {"intensity": 33}})");
    CHECK(both.weather.intensity == 33);
    CHECK(both.weather.wind == 7);
    CHECK(both.startupWithWindows == false);

    // A legacy file whose "weather" is not an object.
    CHECK(loaded(R"({"weather": 3, "intensity": 12})").weather.intensity == 12);

    // A version-1 file ignores flat keys: they are not part of the current schema.
    const Settings v1 = loaded(R"({"schemaVersion": 1, "intensity": 80, "autostart": true})");
    CHECK(v1 == Settings{});

    // Odd schemaVersion values are treated as "no version".
    CHECK(loaded(R"({"schemaVersion": "1", "intensity": 80})").weather.intensity == 80);
    CHECK(loaded(R"({"schemaVersion": -3, "intensity": 80})").weather.intensity == 80);
    CHECK(loaded(R"({"schemaVersion": null, "intensity": 80})").weather.intensity == 80);

    // The migrated result is written in the current format.
    json::Value root;
    CHECK(json::parse(toJson(s), root));
    CHECK(root.find("intensity") == nullptr && root.find("autostart") == nullptr);
    CHECK(root.find("weather")->find("intensity")->asNumber() == 80.0);
    CHECK(root.find("startupWithWindows")->asBool() == true);
}

TEST(settings_newer_schema_version_tolerated) {
    const Settings s = loaded(R"({
      "schemaVersion": 7, "enabled": false, "intensity": 99,
      "weather": {"preset": "thunderSnow", "intensity": 150, "wind": -20, "aurora": true},
      "performance": {"profile": "ultra", "fpsCap": 120},
      "futureSection": {"a": 1}
    })");
    CHECK(s.schemaVersion == kSettingsSchemaVersion);
    CHECK(s.enabled == false);
    CHECK(s.weather.preset == WeatherPreset::Normal);   // unknown enum -> default
    CHECK(s.weather.intensity == 100);                  // understood and clamped; flat key ignored
    CHECK(s.weather.wind == -20);
    CHECK(s.performance.profile == PerfProfile::Balanced);
    CHECK(s.performance.fpsCap == 60);
    CHECK(loaded(R"({"schemaVersion": 1e30})") == Settings{});
}

TEST(settings_accumulation_extras) {
    const Settings d;
    CHECK(d.accumulation.driftOpacityPercent == 90 && d.accumulation.snowplowDelayMinutes == 10);
    CHECK(!d.accumulation.shakeToClear && d.accumulation.naturalCaps && d.accumulation.fallingSnowmen &&
          d.accumulation.snowplow && d.accumulation.shakeOffOnMove);
    json::Value root;
    CHECK(json::parse(toJson(d), root));
    const json::Value* a = root.find("accumulation");
    CHECK(a->find("driftOpacityPercent")->asNumber() == 90.0);
    CHECK(a->find("snowplowDelayMinutes")->asNumber() == 10.0);
    for (const char* k : {"naturalCaps", "fallingSnowmen", "snowplow", "shakeOffOnMove"}) {
        CHECK(a->find(k) && a->find(k)->isBool() && a->find(k)->asBool());
    }
    CHECK(a->find("shakeToClear") && a->find("shakeToClear")->isBool() && !a->find("shakeToClear")->asBool());
    const Settings s = loaded(R"({"schemaVersion": 1, "accumulation": {"driftOpacityPercent": 33,
      "shakeToClear": false, "naturalCaps": false, "fallingSnowmen": false, "snowplow": false,
      "snowplowDelayMinutes": 45}})");
    CHECK(s.accumulation.driftOpacityPercent == 33 && s.accumulation.snowplowDelayMinutes == 45);
    CHECK(!s.accumulation.shakeToClear && !s.accumulation.naturalCaps && !s.accumulation.fallingSnowmen &&
          !s.accumulation.snowplow);
    CHECK(loaded(toJson(s)) == s);
    // Missing new keys (older v1 file) keep their defaults.
    CHECK(loaded(R"({"schemaVersion": 1, "accumulation": {"maxHeightPxAt96Dpi": 36}})") == Settings{});
}

TEST(settings_interaction_extras) {
    const Settings d;
    CHECK(d.interaction.blockClicksWhileHeld == true && d.interaction.objectsOnAllMonitors == false);
    json::Value root;
    CHECK(json::parse(toJson(d), root));
    const json::Value* i = root.find("interaction");
    CHECK(i->find("blockClicksWhileHeld") && i->find("blockClicksWhileHeld")->isBool() &&
          i->find("blockClicksWhileHeld")->asBool() == true);
    CHECK(i->find("objectsOnAllMonitors") && i->find("objectsOnAllMonitors")->isBool() &&
          i->find("objectsOnAllMonitors")->asBool() == false);
    const Settings s = loaded(R"({"schemaVersion": 1,
      "interaction": {"blockClicksWhileHeld": false, "objectsOnAllMonitors": true}})");
    CHECK(!s.interaction.blockClicksWhileHeld && s.interaction.objectsOnAllMonitors);
    CHECK(loaded(toJson(s)) == s);
    CHECK(loaded(R"({"schemaVersion": 1, "interaction": {"enabled": true}})") == Settings{});
}

TEST(settings_ui_theme) {
    CHECK(Settings{}.system.uiTheme == UiTheme::Night);
    json::Value root;
    CHECK(json::parse(toJson(Settings{}), root));
    CHECK(root.find("system")->find("uiTheme")->asString() == "night");
    const auto theme = [](const std::string& v) {
        return loaded(R"({"schemaVersion": 1, "system": {"uiTheme": )" + v + "}}").system.uiTheme;
    };
    CHECK(theme("\"night\"") == UiTheme::Night);
    CHECK(theme("\"globe\"") == UiTheme::Globe);
    CHECK(theme("\"postcard\"") == UiTheme::Postcard);
    CHECK(theme("\"Globe\"") == UiTheme::Night);
    CHECK(theme("\"aurora\"") == UiTheme::Night);
    CHECK(theme("1") == UiTheme::Night);
    CHECK(theme("null") == UiTheme::Night);
    for (UiTheme t : {UiTheme::Night, UiTheme::Globe, UiTheme::Postcard}) {
        Settings s;
        s.system.uiTheme = t;
        CHECK(loaded(toJson(s)) == s);
    }
    Settings bad;
    bad.system.uiTheme = static_cast<UiTheme>(-1);
    sanitize(bad);
    CHECK(bad.system.uiTheme == UiTheme::Night);
}

TEST(settings_snowmen_schedule) {
    const Settings d;
    CHECK(d.accumulation.snowmenIntervalMinutes == 2 && d.accumulation.snowmenNeedHeavySnow &&
          d.accumulation.snowmenMaxAlive == 3);
    json::Value root;
    CHECK(json::parse(toJson(d), root));
    const json::Value* a = root.find("accumulation");
    CHECK(a->find("snowmenIntervalMinutes")->asNumber() == 2.0);
    CHECK(a->find("snowmenNeedHeavySnow")->isBool() && a->find("snowmenNeedHeavySnow")->asBool());
    CHECK(a->find("snowmenMaxAlive")->asNumber() == 3.0);

    const Settings s = loaded(R"({"schemaVersion": 1, "accumulation": {"snowmenIntervalMinutes": 45,
      "snowmenNeedHeavySnow": false, "snowmenMaxAlive": 9}})");
    CHECK(s.accumulation.snowmenIntervalMinutes == 45 && !s.accumulation.snowmenNeedHeavySnow &&
          s.accumulation.snowmenMaxAlive == 9);
    CHECK(loaded(toJson(s)) == s);

    const Settings c = loaded(R"({"schemaVersion": 1, "accumulation": {"snowmenIntervalMinutes": 0,
      "snowmenMaxAlive": 11}})");
    CHECK(c.accumulation.snowmenIntervalMinutes == 1 && c.accumulation.snowmenMaxAlive == 10);
    const Settings c2 = loaded(R"({"schemaVersion": 1, "accumulation": {"snowmenIntervalMinutes": 600,
      "snowmenMaxAlive": -4}})");
    CHECK(c2.accumulation.snowmenIntervalMinutes == 60 && c2.accumulation.snowmenMaxAlive == 1);

    const Settings w = loaded(R"({"schemaVersion": 1, "accumulation": {"snowmenIntervalMinutes": "5",
      "snowmenNeedHeavySnow": 0, "snowmenMaxAlive": null}})");
    CHECK(w == Settings{});
}

TEST(settings_sky_per_monitor) {
    CHECK(Settings{}.weather.skyPerMonitor == true);
    json::Value root;
    CHECK(json::parse(toJson(Settings{}), root));
    CHECK(root.find("weather")->find("skyPerMonitor")->isBool());
    CHECK(root.find("weather")->find("skyPerMonitor")->asBool() == true);

    const Settings s = loaded(R"({"schemaVersion": 1, "weather": {"skyPerMonitor": false},
      "userPresets": [{"name": "one sky", "weather": {"skyPerMonitor": false}}, {"name": "default"}]})");
    CHECK(s.weather.skyPerMonitor == false);
    CHECK(s.userPresets.size() == 2);
    CHECK(s.userPresets[0].weather.skyPerMonitor == false);
    CHECK(s.userPresets[1].weather.skyPerMonitor == true);
    CHECK(loaded(toJson(s)) == s);

    json::Value written;
    CHECK(json::parse(toJson(s), written));
    CHECK(written.find("userPresets")->at(0).find("weather")->find("skyPerMonitor")->asBool(true) == false);

    // Wrong types keep the default.
    for (const char* v : {"0", "\"false\"", "null", "[]"}) {
        const Settings w = loaded(std::string(R"({"schemaVersion": 1, "weather": {"skyPerMonitor": )") + v +
                                  R"(}, "userPresets": [{"name": "p", "weather": {"skyPerMonitor": )" + v + "}}]}");
        CHECK(w.weather.skyPerMonitor == true);
        CHECK(w.userPresets.size() == 1 && w.userPresets[0].weather.skyPerMonitor == true);
    }
}

TEST(settings_snowplow_all_taskbars) {
    CHECK(Settings{}.accumulation.snowplowAllTaskbars == true);
    json::Value root;
    CHECK(json::parse(toJson(Settings{}), root));
    const json::Value* v = root.find("accumulation")->find("snowplowAllTaskbars");
    CHECK(v && v->isBool() && v->asBool() == true);

    const Settings s = loaded(R"({"schemaVersion": 1, "accumulation": {"snowplowAllTaskbars": false}})");
    CHECK(s.accumulation.snowplowAllTaskbars == false);
    CHECK(loaded(toJson(s)) == s);

    for (const char* bad : {"0", "\"false\"", "null", "{}"}) {
        const Settings w =
            loaded(std::string(R"({"schemaVersion": 1, "accumulation": {"snowplowAllTaskbars": )") + bad + "}}");
        CHECK(w == Settings{});
    }
}
