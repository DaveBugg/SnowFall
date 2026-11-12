#include "app/strings_mac.h"

#include <cstring>
#include <cwchar>

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

// Mac wording where the shared table talks about Windows concepts.
struct Override {
    const char* key;
    NSString* ru;
    NSString* en;
};
const Override kOverrides[] = {
    {"MenuAutostart", @"Запускать при входе", @"Open at Login"},
    {"ChkStartup", @"Запускать при входе в систему", @"Open at login"},
    {"MenuExit", @"Завершить SnowFall", @"Quit SnowFall"},
    {"ChkOnTaskbar", @"На Dock", @"On the Dock"},
    {"ChkSnowplow", @"Снегоуборочная машинка на Dock", @"Snowplow on the Dock"},
    {"ChkSnowplowAllTaskbars", @"На каждом экране с Dock", @"On every screen with the Dock"},
    {"SnowplowAllTaskbarsHint", @"macOS показывает Dock на одном экране — машинка ездит там, где он",
     @"macOS shows the Dock on one screen at a time; the plow drives where it is"},
    {"ChkGarland", @"Гирлянда над Dock", @"Garland above the Dock"},
    {"FestiveText",
     @"Гирлянда мигает вдоль верхнего края Dock. Салют начнётся сам в полночь 31 декабря и продлится около полутора минут, а кнопка «Салют!» запускает его прямо сейчас.",
     @"The garland twinkles along the top edge of the Dock. Fireworks start by themselves at midnight on December 31 and last about a minute and a half; the «Fireworks!» button launches them right now."},
    {"ChkHoldKey", @"Удерживать Ctrl+Option для временного режима", @"Hold Ctrl+Option for temporary interaction mode"},
    {"HoldHintNone", @"Режим взаимодействия: горячая клавиша или меню в строке меню",
     @"Interaction mode: use its hotkey or the menu bar menu"},
    {"HelpText",
     @"Чтобы трогать снег, удерживайте Ctrl+Option или включите режим горячей клавишей либо в меню в строке меню.\n"
     @"• Левая кнопка по сугробу: варежка сметает снег, лопата расчищает полосу; правый щелчок — следующий инструмент.\n"
     @"• Снежок: тяните от сугроба — ком растёт; положите шар на шар, и получится снеговик.\n"
     @"• Снежки и снеговики живут позади окон: на основном мониторе или на всех (флажок выше).\n"
     @"• Для щелчков по снегу macOS попросит разрешение «Универсальный доступ».",
     @"To touch the snow, hold Ctrl+Option or turn the mode on with its hotkey or from the menu bar menu.\n"
     @"• Left-drag over a drift: the mitten brushes snow away, the shovel clears a strip; right-click picks the next tool.\n"
     @"• Snowball: drag from a drift and the ball grows; drop a ball onto another to build a snowman.\n"
     @"• Snowballs and snowmen live behind windows: on the primary display or on all displays (option above).\n"
     @"• To click on the snow, macOS asks for the Accessibility permission."},
    {"ChkLeftClick", @"Щелчок по значку в строке меню — пауза", @"Menu bar icon click: pause"},
    {"HotkeyHint", @"Щёлкните поле и нажмите сочетание с Ctrl, Option, Shift или Cmd. Backspace или Delete — отключить.",
     @"Click a field and press a combination with Ctrl, Option, Shift or Cmd. Backspace or Delete disables it."},
    {"LblExeName", @"Имя приложения:", @"App name:"},
    {"ExeCue", @"например, Safari", @"for example, Safari"},
    {"PickerHint", @"Программы с открытыми окнами. Выберите одну или несколько (Cmd, Shift) — их окна останутся без снега.",
     @"Apps with open windows. Select one or more (Cmd, Shift); their windows will stay free of snow."},
    {"AboutDescription",
     @"Интерактивный снег на рабочем столе macOS: снежинки падают поверх экрана, оседают на окнах и Dock, "
     @"а из сугробов можно катать снежки и собирать снеговиков.",
     @"Interactive snow for the macOS desktop: snowflakes fall over the screen, settle on windows and the Dock, "
     @"and drifts can be rolled into snowballs and snowmen."},
    {"ChkAllMonitors", @"Снежки и снеговики на всех экранах", @"Snowballs and snowmen on all displays"},
};

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
    return fromWide(g_russian ? e.ru : e.en);
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
    return a;
}

}  // namespace snow::mac
