#include "app/strings_mac.h"

#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace snow::mac {
namespace {

struct Entry {
    const char* key;
    const wchar_t* ru;
    const wchar_t* en;
};
const Entry kTable[] = {
#define X(id, ru, en) {#id, ru, en},
#include "i18n/strings_table.inc"
#undef X
};
static_assert(sizeof kTable / sizeof kTable[0] == static_cast<std::size_t>(Str::Count), "string table mismatch");

// Mac wording: the shared table carries "<Id>Mac" variants for texts that mention Windows
// concepts (Dock instead of taskbar, Ctrl+Option, Open at login, ...); they replace the base
// entry automatically. The few left-overs without a Mac variant are overridden here.
struct Override {
    const char* key;
    NSString* ru;
    NSString* en;
};
const Override kOverrides[] = {
    {"MenuExit", @"Завершить SnowFall", @"Quit SnowFall"},
    {"HoldHintNone", @"Режим взаимодействия: горячая клавиша или меню в строке меню",
     @"Interaction mode: use its hotkey or the menu bar menu"},
    {"ChkLeftClick", @"Щелчок по значку в строке меню — пауза", @"Menu bar icon click: pause"},
    {"LblExeName", @"Имя приложения:", @"App name:"},
    {"ExeCue", @"например, Safari", @"for example, Safari"},
    {"ChkAllMonitors", @"Снежки и снеговики на всех экранах", @"Snowballs and snowmen on all displays"},
};

// For every entry: index of its "<Id>Mac" variant, or -1.
const std::vector<int>& macVariants() {
    static const std::vector<int> v = [] {
        const std::size_t n = sizeof kTable / sizeof kTable[0];
        std::vector<int> out(n, -1);
        for (std::size_t i = 0; i < n; ++i) {
            const std::string mac = std::string(kTable[i].key) + "Mac";
            for (std::size_t j = 0; j < n; ++j)
                if (mac == kTable[j].key) out[i] = static_cast<int>(j);
        }
        return out;
    }();
    return v;
}

bool g_russian = false;

NSString* fromWide(const wchar_t* w) {
    if (!w) return @"";
    return [[NSString alloc] initWithBytes:w
                                    length:std::wcslen(w) * sizeof(wchar_t)
                                  encoding:NSUTF32LittleEndianStringEncoding] ?: @"";
}

NSString* lookup(std::size_t i) {
    const Entry& e = kTable[i];
    for (const Override& o : kOverrides)
        if (std::strcmp(o.key, e.key) == 0) return g_russian ? o.ru : o.en;
    const int mac = macVariants()[i];
    const Entry& use = mac >= 0 ? kTable[mac] : e;
    return fromWide(g_russian ? use.ru : use.en);
}

bool systemPrefersRussian() {
    for (NSString* lang in [NSLocale preferredLanguages]) {
        if ([lang hasPrefix:@"ru"]) return true;
        if ([lang hasPrefix:@"en"]) return false;
    }
    return false;
}

}  // namespace

void setUiLanguage(Language lang) {
    switch (lang) {
        case Language::Russian: g_russian = true; break;
        case Language::English: g_russian = false; break;
        default: g_russian = systemPrefersRussian(); break;
    }
}

bool uiRussian() { return g_russian; }

NSString* tr(Str id) {
    const auto i = static_cast<std::size_t>(id);
    if (i >= static_cast<std::size_t>(Str::Count)) return @"";
    return lookup(i);
}

NSDictionary<NSString*, NSString*>* uiStringTable() {
    NSMutableDictionary<NSString*, NSString*>* d = [NSMutableDictionary dictionary];
    for (std::size_t i = 0; i < static_cast<std::size_t>(Str::Count); ++i) {
        NSString* s = [lookup(i) stringByReplacingOccurrencesOfString:@"%ls" withString:@"%@"];
        d[[NSString stringWithUTF8String:kTable[i].key]] = s;
    }
    return d;
}

NSArray<NSString*>* macOverriddenKeys() {
    NSMutableArray<NSString*>* a = [NSMutableArray array];
    for (const Override& o : kOverrides) [a addObject:[NSString stringWithUTF8String:o.key]];
    for (std::size_t i = 0; i < macVariants().size(); ++i)
        if (macVariants()[i] >= 0) [a addObject:[NSString stringWithUTF8String:kTable[i].key]];
    return a;
}

}  // namespace snow::mac
