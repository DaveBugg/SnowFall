#include "system/hotkey_map.h"

#include <vector>

namespace snow::mac {
namespace {

// kVK_ANSI_A .. kVK_ANSI_Z in alphabetical order (HIToolbox/Events.h).
constexpr unsigned kLetters[26] = {
    0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,   // A..M
    0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06,   // N..Z
};
// kVK_ANSI_0 .. kVK_ANSI_9.
constexpr unsigned kDigits[10] = {0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19};
// kVK_F1 .. kVK_F20.
constexpr unsigned kFKeys[20] = {0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D,
                                 0x67, 0x6F, 0x69, 0x6B, 0x71, 0x6A, 0x40, 0x4F, 0x50, 0x5A};

struct Named {
    unsigned vk, key;
};
constexpr Named kNamed[] = {
    {0x20, 0x31},   // Space    -> kVK_Space
    {0x09, 0x30},   // Tab      -> kVK_Tab
    {0x1B, 0x35},   // Escape   -> kVK_Escape
    {0x0D, 0x24},   // Enter    -> kVK_Return
    {0x2D, 0x72},   // Insert   -> kVK_Help (the Insert position on full-size keyboards)
    {0x2E, 0x75},   // Delete   -> kVK_ForwardDelete
    {0x24, 0x73},   // Home     -> kVK_Home
    {0x23, 0x77},   // End      -> kVK_End
    {0x21, 0x74},   // PageUp   -> kVK_PageUp
    {0x22, 0x79},   // PageDown -> kVK_PageDown
    {0x26, 0x7E},   // Up       -> kVK_UpArrow
    {0x28, 0x7D},   // Down     -> kVK_DownArrow
    {0x25, 0x7B},   // Left     -> kVK_LeftArrow
    {0x27, 0x7C},   // Right    -> kVK_RightArrow
};

std::vector<std::string> splitPlus(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '+') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

}  // namespace

bool vkToMacKeyCode(unsigned vk, unsigned& keyCode) {
    if (vk >= 0x41 && vk <= 0x5A) {
        keyCode = kLetters[vk - 0x41];
        return true;
    }
    if (vk >= 0x30 && vk <= 0x39) {
        keyCode = kDigits[vk - 0x30];
        return true;
    }
    if (vk >= 0x70 && vk < 0x70 + 20) {
        keyCode = kFKeys[vk - 0x70];
        return true;
    }
    for (const Named& n : kNamed) {
        if (n.vk == vk) {
            keyCode = n.key;
            return true;
        }
    }
    return false;   // Pause, F21..F24
}

unsigned modifiersToCarbon(unsigned mods) {
    unsigned c = 0;
    if (mods & kModCtrl) c |= kCarbonControl;
    if (mods & kModAlt) c |= kCarbonOption;
    if (mods & kModShift) c |= kCarbonShift;
    if (mods & kModWin) c |= kCarbonCmd;
    return c;
}

std::string canonicalHotkeyText(const std::string& text) {
    if (trim(text).empty()) return {};
    std::string norm;
    for (const std::string& raw : splitPlus(text)) {
        std::string t = trim(raw);
        const std::string l = lower(t);
        if (l == "option" || l == "opt" || t == "\xE2\x8C\xA5") t = "Alt";                       // ⌥
        else if (l == "cmd" || l == "command" || t == "\xE2\x8C\x98") t = "Win";                 // ⌘
        else if (l == "control" || l == "ctl" || t == "\xE2\x8C\x83") t = "Ctrl";                // ⌃
        else if (t == "\xE2\x87\xA7") t = "Shift";                                               // ⇧
        if (!norm.empty()) norm += '+';
        norm += t;
    }
    Hotkey hk;
    if (!parseHotkey(norm, hk)) return {};
    return formatHotkey(hk);
}

std::string macHotkeyDisplay(const std::string& canonical) {
    if (canonical.empty()) return {};
    std::string out;
    for (const std::string& part : splitPlus(canonical)) {
        if (!out.empty()) out += '+';
        if (part == "Alt") out += "Option";
        else if (part == "Win") out += "Cmd";
        else out += part;
    }
    return out;
}

}  // namespace snow::mac
