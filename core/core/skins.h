// Snowman skins: named combinations of snowman parts (hats, scarves, accessories).
// Shared by the simulation (which builds snowmen from them) and the UI (skin picker).
// Plain data, no Win32.
#pragma once
#include <cstdint>

namespace snow {

// Shape::Accessory variants (see core/snapshot.h). Each sprite is drawn centred at its
// position with rotation; "anchor" says where the simulation places it on the snowman.
enum class Accessory : std::uint8_t {
    Sunglasses = 0,   // dark shades across both eyes          anchor: eyes line of the head
    RoundGlasses,     // thin round spectacles                   anchor: eyes line
    EyePatch,         // black pirate patch over one eye + strap anchor: one eye (+X)
    Headphones,       // big over-ear headphones with band       anchor: around the head
    Earmuffs,         // fluffy earmuffs with band               anchor: around the head
    BowTie,           // red bow tie                              anchor: neck (between head & body)
    Crown,            // golden crown with gems                   anchor: top of the head (hat slot)
    TricornHat,       // black pirate tricorn with skull mark     anchor: hat slot
    CowboyHat,        // brown cowboy hat                         anchor: hat slot
    ChefHat,          // white chef toque                         anchor: hat slot
    CatEars,          // two pointy cat ears (pink inside)        anchor: hat slot
    Whiskers,         // three whiskers per side                  anchor: nose line
    Mustache,         // curly black mustache                     anchor: under the nose
    Beard,            // fluffy white beard                       anchor: lower half of the head
    Pipe,             // wooden smoking pipe (no smoke)           anchor: mouth corner (+X)
    GooglyEye,        // googly eye (white ball, black pupil)     anchor: each eye (replaces coal)
    Flower,           // small flower with petals                 anchor: hat side or button slot
    Broom,            // straw broom held in a stick hand         anchor: end of one arm (+X)
    Bandana,          // red neck bandana (triangle)              anchor: neck
    Monocle,          // golden monocle with chain                anchor: one eye (+X)
    Count
};

struct SkinPart {
    // Exactly one of: a Hat variant (0..3), a Scarf variant (0..3), or an Accessory.
    enum class Kind : std::uint8_t { Hat, Scarf, Accessory } kind;
    std::uint8_t variant;   // Hat/Scarf variant or static_cast<uint8_t>(Accessory)
};

struct SnowmanSkin {
    const char* id;         // settings value (interaction.snowmanSkin)
    const wchar_t* nameRu;
    const wchar_t* nameEn;
    SkinPart parts[5];      // unused slots: kind = Accessory, variant = 0xFF
    bool googlyEyes;        // GooglyEye instead of coal eyes
};

inline constexpr std::uint8_t kNoPart = 0xFF;
#define SNOW_ACC(a) SkinPart{SkinPart::Kind::Accessory, static_cast<std::uint8_t>(Accessory::a)}
#define SNOW_HAT(v) SkinPart{SkinPart::Kind::Hat, v}
#define SNOW_SCARF(v) SkinPart{SkinPart::Kind::Scarf, v}
#define SNOW_NONE SkinPart{SkinPart::Kind::Accessory, kNoPart}

inline constexpr SnowmanSkin kSnowmanSkins[] = {
    {"classic", L"Классический", L"Classic", {SNOW_HAT(0), SNOW_SCARF(0), SNOW_NONE, SNOW_NONE, SNOW_NONE}, false},
    {"santa", L"Дед Мороз", L"Santa", {SNOW_HAT(3), SNOW_ACC(Beard), SNOW_SCARF(0), SNOW_NONE, SNOW_NONE}, false},
    {"pirate", L"Пират", L"Pirate", {SNOW_ACC(TricornHat), SNOW_ACC(EyePatch), SNOW_ACC(Bandana), SNOW_NONE, SNOW_NONE}, false},
    {"dj", L"Диджей", L"DJ", {SNOW_ACC(Headphones), SNOW_ACC(Sunglasses), SNOW_SCARF(2), SNOW_NONE, SNOW_NONE}, false},
    {"king", L"Король", L"King", {SNOW_ACC(Crown), SNOW_ACC(Monocle), SNOW_ACC(BowTie), SNOW_NONE, SNOW_NONE}, false},
    {"scientist", L"Учёный", L"Scientist", {SNOW_ACC(RoundGlasses), SNOW_ACC(BowTie), SNOW_ACC(Mustache), SNOW_NONE, SNOW_NONE}, false},
    {"cowboy", L"Ковбой", L"Cowboy", {SNOW_ACC(CowboyHat), SNOW_ACC(Bandana), SNOW_ACC(Mustache), SNOW_NONE, SNOW_NONE}, false},
    {"chef", L"Шеф-повар", L"Chef", {SNOW_ACC(ChefHat), SNOW_ACC(Mustache), SNOW_SCARF(1), SNOW_NONE, SNOW_NONE}, false},
    {"cat", L"Котик", L"Kitty", {SNOW_ACC(CatEars), SNOW_ACC(Whiskers), SNOW_ACC(BowTie), SNOW_NONE, SNOW_NONE}, false},
    {"grandpa", L"Дедушка", L"Grandpa", {SNOW_HAT(2), SNOW_ACC(Pipe), SNOW_ACC(Earmuffs), SNOW_ACC(RoundGlasses), SNOW_NONE}, false},
    {"janitor", L"Дворник", L"Janitor", {SNOW_HAT(1), SNOW_ACC(Broom), SNOW_SCARF(3), SNOW_NONE, SNOW_NONE}, false},
    {"silly", L"Чудик", L"Silly", {SNOW_ACC(Flower), SNOW_ACC(BowTie), SNOW_SCARF(1), SNOW_NONE, SNOW_NONE}, true},
};
inline constexpr int kSnowmanSkinCount = static_cast<int>(sizeof kSnowmanSkins / sizeof kSnowmanSkins[0]);

#undef SNOW_ACC
#undef SNOW_HAT
#undef SNOW_SCARF
#undef SNOW_NONE

}  // namespace snow
