// New Year countdown text, computed from the local date.
#pragma once
#include <windows.h>

#include <string>

namespace snow::ui {

struct Countdown {
    std::wstring big;       // "42", "5" (hours on Dec 31) or «С Новым годом!»
    std::wstring caption;   // «дня до Нового года» ...
    bool festive = false;   // Jan 1..7
};

Countdown computeCountdown(const SYSTEMTIME& localNow);
// Current local time, or the injected test date (see snow::uitest::setToday).
SYSTEMTIME countdownNow();
void setCountdownOverride(const SYSTEMTIME* t);

}  // namespace snow::ui
