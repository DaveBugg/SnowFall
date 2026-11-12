// Round-7 fields: tool default, interaction.snowmanSkin, system.excludedSnowMode, garland default.
#include "core/skins.h"
#include "settings/json.h"
#include "settings/settings_store.h"
#include "snow_test.h"

using namespace snow;

namespace {

Settings load7(const std::string& text) {
    Settings s;
    std::string err;
    const bool ok = fromJson(text, s, &err);
    if (!ok) printf("  unexpected fromJson failure: %s\n", err.c_str());
    CHECK(ok);
    return s;
}

std::string skinOf(const std::string& jsonValue) {
    return load7(R"({"schemaVersion": 1, "interaction": {"snowmanSkin": )" + jsonValue + "}}").interaction.snowmanSkin;
}

}  // namespace

TEST(r7_defaults_json) {
    const Settings d;
    CHECK(d.interaction.tool == Tool::Snowball);
    CHECK(d.interaction.snowmanSkin == "random");
    CHECK(d.system.excludedSnowMode == ExcludedSnowMode::Behind);
    CHECK(d.festive.garland == false && d.festive.fireworks == true);
    json::Value root;
    CHECK(json::parse(toJson(d), root));
    CHECK(root.find("interaction")->find("tool")->asString() == "snowball");
    CHECK(root.find("interaction")->find("snowmanSkin")->asString() == "random");
    CHECK(root.find("system")->find("excludedSnowMode")->asString() == "behind");
    CHECK(root.find("festive")->find("garland")->asBool(true) == false);
    CHECK(load7(R"({"schemaVersion": 1, "interaction": {}, "system": {}, "festive": {}})") == Settings{});
}

TEST(r7_roundtrip) {
    for (int i = 0; i < kSnowmanSkinCount; ++i) {
        Settings s;
        s.interaction.snowmanSkin = kSnowmanSkins[i].id;
        s.system.excludedSnowMode = (i % 2) ? ExcludedSnowMode::Around : ExcludedSnowMode::Behind;
        Settings copy = s;
        sanitize(copy);
        CHECK(copy == s);   // every table id is accepted as-is
        CHECK(load7(toJson(s)) == s);
    }
    CHECK(skinOf("\"pirate\"") == "pirate");
    CHECK(skinOf("\"random\"") == "random");
    CHECK(load7(R"({"schemaVersion": 1, "system": {"excludedSnowMode": "around"}})").system.excludedSnowMode ==
          ExcludedSnowMode::Around);
}

TEST(r7_unknown_skin_and_wrong_types) {
    CHECK(skinOf("\"zombie\"") == "random");
    CHECK(skinOf("\"\"") == "random");
    CHECK(skinOf("\"pirate \"") == "pirate");    // trimmed
    CHECK(skinOf("\"Pirate\"") == "pirate");     // ASCII case-insensitive
    CHECK(skinOf("\"пират\"") == "random");
    CHECK(skinOf("3") == "random");              // wrong type -> default
    CHECK(skinOf("null") == "random");
    CHECK(skinOf("[\"pirate\"]") == "random");
    Settings s;
    s.interaction.snowmanSkin = "no-such-skin";
    sanitize(s);
    CHECK(s.interaction.snowmanSkin == "random");

    const auto mode = [](const std::string& v) {
        return load7(R"({"schemaVersion": 1, "system": {"excludedSnowMode": )" + v + "}}").system.excludedSnowMode;
    };
    CHECK(mode("\"Around\"") == ExcludedSnowMode::Behind);
    CHECK(mode("\"through\"") == ExcludedSnowMode::Behind);
    CHECK(mode("1") == ExcludedSnowMode::Behind);
    CHECK(mode("true") == ExcludedSnowMode::Behind);
    Settings bad;
    bad.system.excludedSnowMode = static_cast<ExcludedSnowMode>(7);
    bad.interaction.tool = static_cast<Tool>(-1);
    sanitize(bad);
    CHECK(bad.system.excludedSnowMode == ExcludedSnowMode::Behind);
    CHECK(bad.interaction.tool == Tool::Snowball);
}
