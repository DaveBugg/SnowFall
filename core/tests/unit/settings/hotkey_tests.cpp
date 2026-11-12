#include "settings/hotkey.h"
#include "snow_test.h"

using namespace snow;

namespace {

Hotkey parsedOk(const std::string& text) {
    Hotkey hk;
    const bool ok = parseHotkey(text, hk);
    if (!ok) printf("  unexpected hotkey parse failure: '%s'\n", text.c_str());
    CHECK(ok);
    return hk;
}

bool rejected(const std::string& text) {
    Hotkey hk{kModWin, 0x99};
    return !parseHotkey(text, hk) && hk == Hotkey{kModWin, 0x99};   // out untouched
}

std::string canonical(const std::string& text) { return formatHotkey(parsedOk(text)); }

}  // namespace

TEST(hotkey_defaults) {
    CHECK((parsedOk("Ctrl+Alt+S") == Hotkey{kModCtrl | kModAlt, 0x53}));
    CHECK((parsedOk("Ctrl+Alt+X") == Hotkey{kModCtrl | kModAlt, 0x58}));
    CHECK((parsedOk("Ctrl+Alt+I") == Hotkey{kModCtrl | kModAlt, 0x49}));
}

TEST(hotkey_modifiers) {
    static_assert(kModAlt == 1 && kModCtrl == 2 && kModShift == 4 && kModWin == 8, "must equal Win32 MOD_*");
    CHECK(parsedOk("Ctrl+A").modifiers == kModCtrl);
    CHECK(parsedOk("Control+A").modifiers == kModCtrl);
    CHECK(parsedOk("Alt+A").modifiers == kModAlt);
    CHECK(parsedOk("Shift+A").modifiers == kModShift);
    CHECK(parsedOk("Win+A").modifiers == kModWin);
    CHECK(parsedOk("Win+Shift+Alt+Ctrl+A").modifiers == (kModCtrl | kModAlt | kModShift | kModWin));
}

TEST(hotkey_all_key_names) {
    for (char c = 'A'; c <= 'Z'; ++c) {
        CHECK(parsedOk(std::string("Ctrl+") + c).vk == static_cast<unsigned>(c));
        CHECK(canonical(std::string("Ctrl+") + c) == std::string("Ctrl+") + c);
    }
    for (char c = '0'; c <= '9'; ++c) {
        CHECK(parsedOk(std::string("Alt+") + c).vk == static_cast<unsigned>(c));
        CHECK(canonical(std::string("Alt+") + c) == std::string("Alt+") + c);
    }
    for (unsigned n = 1; n <= 24; ++n) {
        const std::string text = "Shift+F" + std::to_string(n);
        CHECK(parsedOk(text).vk == 0x70 + n - 1);
        CHECK(canonical(text) == text);
    }
    struct Named {
        const char* name;
        unsigned vk;
    };
    const Named named[] = {
        {"Space", 0x20}, {"Tab", 0x09},    {"Esc", 0x1B},      {"Enter", 0x0D},
        {"Insert", 0x2D}, {"Delete", 0x2E}, {"Home", 0x24},     {"End", 0x23},
        {"PageUp", 0x21}, {"PageDown", 0x22}, {"Up", 0x26},     {"Down", 0x28},
        {"Left", 0x25},  {"Right", 0x27},  {"Pause", 0x13},
    };
    for (const Named& k : named) {
        const std::string text = std::string("Ctrl+Shift+") + k.name;
        CHECK(parsedOk(text).vk == k.vk);
        CHECK(canonical(text) == text);
    }
    CHECK(parsedOk("Ctrl+Escape").vk == 0x1B);
    CHECK(canonical("Ctrl+Escape") == "Ctrl+Esc");
}

TEST(hotkey_case_and_whitespace) {
    const Hotkey expected{kModCtrl | kModAlt, 0x53};
    CHECK(parsedOk("ctrl+alt+s") == expected);
    CHECK(parsedOk("CTRL+ALT+S") == expected);
    CHECK(parsedOk("cTrL+aLt+s") == expected);
    CHECK(parsedOk("ctrl + alt + s") == expected);
    CHECK(parsedOk("  Ctrl\t+ Alt +S  ") == expected);
    CHECK(parsedOk("alt+CONTROL+s") == expected);
    CHECK(canonical("shift+ctrl+pagedown") == "Ctrl+Shift+PageDown");
    CHECK(canonical("win + f12") == "Win+F12");
    CHECK(canonical("ALT+SPACE") == "Alt+Space");
}

TEST(hotkey_invalid) {
    CHECK(rejected(""));
    CHECK(rejected("   "));
    CHECK(rejected("+"));
    CHECK(rejected("Ctrl"));
    CHECK(rejected("Ctrl+"));
    CHECK(rejected("+S"));
    CHECK(rejected("Ctrl++S"));
    CHECK(rejected("Ctrl+Alt"));
    CHECK(rejected("Ctrl+Ctrl+S"));        // repeated modifier
    CHECK(rejected("Ctrl+Control+S"));
    CHECK(rejected("Ctrl+S+A"));           // two keys
    CHECK(rejected("Ctrl+S+Alt"));         // key must come last
    CHECK(rejected("Ctrl+F0"));
    CHECK(rejected("Ctrl+F25"));
    CHECK(rejected("Ctrl+F01"));
    CHECK(rejected("Ctrl+F1x"));
    CHECK(rejected("Ctrl+F100"));
    CHECK(rejected("Ctrl+Foo"));
    CHECK(rejected("Ctrl+AB"));
    CHECK(rejected("Ctrl+-"));
    CHECK(rejected("Ctrl+Page Up"));
    CHECK(rejected("Ctrl-Alt-S"));
    CHECK(rejected("Hyper+S"));
    CHECK(rejected("Ctrl+Ы"));
    CHECK(rejected("Ctrl+Alt+S,"));
}

TEST(hotkey_no_modifier_rejected) {
    CHECK(rejected("S"));
    CHECK(rejected("F5"));
    CHECK(rejected("Space"));
    CHECK(rejected("Pause"));
}

TEST(hotkey_format) {
    CHECK(formatHotkey(Hotkey{kModCtrl | kModAlt | kModShift | kModWin, 0x41}) == "Ctrl+Alt+Shift+Win+A");
    CHECK(formatHotkey(Hotkey{kModWin | kModAlt, 0x70}) == "Alt+Win+F1");
    CHECK(formatHotkey(Hotkey{}) == "");
    CHECK(formatHotkey(Hotkey{0, 0x41}) == "");              // no modifier
    CHECK(formatHotkey(Hotkey{kModCtrl, 0}) == "");          // no key
    CHECK(formatHotkey(Hotkey{kModCtrl, 0xFF}) == "");       // unknown key
    CHECK(formatHotkey(Hotkey{kModCtrl, 0x61}) == "");       // 'a' is VK_NUMPAD1, not supported
    CHECK(formatHotkey(Hotkey{0x4000, 0x41}) == "");         // only unknown modifier bits
    CHECK(formatHotkey(Hotkey{0x4000 | kModCtrl, 0x41}) == "Ctrl+A");   // MOD_NOREPEAT ignored

    // format(parse(x)) is a fixed point.
    for (const char* text : {"ctrl + alt + s", "WIN+shift+escape", "Shift+Alt+f24", "control+0"}) {
        const std::string once = canonical(text);
        CHECK(!once.empty() && canonical(once) == once);
    }
}
