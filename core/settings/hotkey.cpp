#include "settings/hotkey.h"

#include <string_view>

namespace snow {

namespace {

struct NamedKey {
    const char* name;      // canonical spelling (first entry for a vk wins when formatting)
    const char* lowered;   // lower-case form used for matching
    unsigned vk;
};

// Win32 virtual-key codes, spelled numerically so this file needs no <windows.h>.
constexpr NamedKey kNamedKeys[] = {
    {"Space", "space", 0x20},       // VK_SPACE
    {"Tab", "tab", 0x09},           // VK_TAB
    {"Esc", "esc", 0x1B},           // VK_ESCAPE
    {"Escape", "escape", 0x1B},
    {"Enter", "enter", 0x0D},       // VK_RETURN
    {"Insert", "insert", 0x2D},     // VK_INSERT
    {"Delete", "delete", 0x2E},     // VK_DELETE
    {"Home", "home", 0x24},         // VK_HOME
    {"End", "end", 0x23},           // VK_END
    {"PageUp", "pageup", 0x21},     // VK_PRIOR
    {"PageDown", "pagedown", 0x22}, // VK_NEXT
    {"Up", "up", 0x26},             // VK_UP
    {"Down", "down", 0x28},         // VK_DOWN
    {"Left", "left", 0x25},         // VK_LEFT
    {"Right", "right", 0x27},       // VK_RIGHT
    {"Pause", "pause", 0x13},       // VK_PAUSE
};

constexpr unsigned kVkF1 = 0x70;    // VK_F1 .. VK_F24 = 0x70 .. 0x87
constexpr unsigned kAllModifiers = kModAlt | kModCtrl | kModShift | kModWin;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string lowerTrimmed(std::string_view token) {
    while (!token.empty() && isSpace(token.front())) token.remove_prefix(1);
    while (!token.empty() && isSpace(token.back())) token.remove_suffix(1);
    std::string out(token);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

unsigned modifierFromToken(const std::string& t) {
    if (t == "ctrl" || t == "control") return kModCtrl;
    if (t == "alt") return kModAlt;
    if (t == "shift") return kModShift;
    if (t == "win") return kModWin;
    return 0;
}

unsigned keyFromToken(const std::string& t) {
    if (t.size() == 1) {
        const char c = t[0];
        if (c >= 'a' && c <= 'z') return static_cast<unsigned>(c - 'a') + 0x41;
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0') + 0x30;
        return 0;
    }
    if (t[0] == 'f' && t.size() <= 3 && t[1] >= '1' && t[1] <= '9') {   // F1..F24
        unsigned n = static_cast<unsigned>(t[1] - '0');
        if (t.size() == 3) {
            if (t[2] < '0' || t[2] > '9') return 0;
            n = n * 10 + static_cast<unsigned>(t[2] - '0');
        }
        return n <= 24 ? kVkF1 + n - 1 : 0;
    }
    for (const NamedKey& k : kNamedKeys) {
        if (t == k.lowered) return k.vk;
    }
    return 0;
}

std::string keyName(unsigned vk) {
    if ((vk >= 0x41 && vk <= 0x5A) || (vk >= 0x30 && vk <= 0x39)) {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= kVkF1 && vk < kVkF1 + 24) return "F" + std::to_string(vk - kVkF1 + 1);
    for (const NamedKey& k : kNamedKeys) {
        if (k.vk == vk) return k.name;
    }
    return {};
}

}  // namespace

bool parseHotkey(const std::string& text, Hotkey& out) {
    // Grammar: one or more distinct modifiers followed by exactly one key, joined by '+'.
    Hotkey hk;
    std::string_view rest(text);
    for (;;) {
        const size_t plus = rest.find('+');
        const bool last = plus == std::string_view::npos;
        const std::string token = lowerTrimmed(last ? rest : rest.substr(0, plus));
        if (token.empty()) return false;
        if (last) {
            hk.vk = keyFromToken(token);
            break;
        }
        const unsigned mod = modifierFromToken(token);
        if (mod == 0 || (hk.modifiers & mod) != 0) return false;   // not a modifier / repeated
        hk.modifiers |= mod;
        rest.remove_prefix(plus + 1);
    }
    if (!hk.valid()) return false;
    out = hk;
    return true;
}

std::string formatHotkey(const Hotkey& hk) {
    const unsigned mods = hk.modifiers & kAllModifiers;
    const std::string key = keyName(hk.vk);
    if (mods == 0 || key.empty()) return {};
    std::string out;
    if (mods & kModCtrl) out += "Ctrl+";
    if (mods & kModAlt) out += "Alt+";
    if (mods & kModShift) out += "Shift+";
    if (mods & kModWin) out += "Win+";
    return out + key;
}

}  // namespace snow
