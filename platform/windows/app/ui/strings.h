// UI localisation: Russian (primary) and English. UI thread only.
// The integrator may reuse these strings (statuses, balloons) through tr().
#pragma once
#include "core/settings.h"

namespace snow {

enum class StrId : int {
#define X(id, ru, en) id,
#include "strings_table.inc"
#undef X
    Count
};

// Selects the UI language. Language::Auto resolves through GetUserDefaultUILanguage():
// Russian when the primary language is LANG_RUSSIAN, English otherwise.
void setLanguage(Language language);

// The language actually in effect (never Auto).
Language effectiveLanguage();

// Never returns nullptr; the pointer stays valid for the process lifetime.
const wchar_t* tr(StrId id);

}  // namespace snow
