// UI strings for the macOS shell: the shared RU/EN table of core/i18n/strings_table.inc, with
// every "<Id>Mac" variant substituted under its base id (Dock instead of taskbar, "Open at
// login", Ctrl+Option, ...) plus a few local overrides.
// Objective-C++ (returns NSString). Main thread.
#pragma once
#import <Foundation/Foundation.h>

#include "core/settings.h"

namespace snow::mac {

enum class Str : int {
#define X(id, ru, en) id,
#include "i18n/strings_table.inc"
#undef X
    Count
};

// Language::Auto follows the system's preferred languages.
void setUiLanguage(Language lang);
bool uiRussian();

NSString* tr(Str id);
// Every string keyed by its StrId name (e.g. @"TabSnow"), for SFAppActions -uiStrings.
// printf-style "%ls" placeholders are converted to "%@" so Swift's String(format:) can use them.
NSDictionary<NSString*, NSString*>* uiStringTable();
// Key names that have a macOS-specific override (tests / diagnostics).
NSArray<NSString*>* macOverriddenKeys();

}  // namespace snow::mac
