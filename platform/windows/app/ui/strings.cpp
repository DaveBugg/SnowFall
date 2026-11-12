#include "strings.h"

#include <windows.h>

namespace snow {
namespace {

struct Entry {
    const wchar_t* ru;
    const wchar_t* en;
};

const Entry kTable[] = {
#define X(id, ru, en) {ru, en},
#include "i18n/strings_table.inc"
#undef X
};

Language resolveAuto() {
    const LANGID id = GetUserDefaultUILanguage();
    return PRIMARYLANGID(id) == LANG_RUSSIAN ? Language::Russian : Language::English;
}

Language& current() {
    static Language lang = resolveAuto();
    return lang;
}

}  // namespace

void setLanguage(Language language) {
    current() = (language == Language::Auto) ? resolveAuto() : language;
}

Language effectiveLanguage() { return current(); }

const wchar_t* tr(StrId id) {
    const int i = static_cast<int>(id);
    if (i < 0 || i >= static_cast<int>(StrId::Count)) return L"";
    const Entry& e = kTable[i];
    return current() == Language::Russian ? e.ru : e.en;
}

}  // namespace snow
