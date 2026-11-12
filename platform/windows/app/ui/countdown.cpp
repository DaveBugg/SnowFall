#include "countdown.h"

#include "strings.h"
#include "ui_util.h"

namespace snow::ui {
namespace {

bool g_haveOverride = false;
SYSTEMTIME g_override{};

int ruForm(int n) {   // 0: 1 день, 1: 2 дня, 2: 5 дней
    const int a = n % 100, b = n % 10;
    if (a >= 11 && a <= 14) return 2;
    if (b == 1) return 0;
    if (b >= 2 && b <= 4) return 1;
    return 2;
}

long long dayNumber(int y, int m, int d) {
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(m);
    st.wDay = static_cast<WORD>(d);
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>(u.QuadPart / (10000000ull * 86400ull));
}

}  // namespace

Countdown computeCountdown(const SYSTEMTIME& now) {
    Countdown c;
    if (now.wMonth == 1 && now.wDay <= 7) {
        c.big = tr(StrId::HappyNewYear);
        c.caption = tr(StrId::HappyHolidays);
        c.festive = true;
        return c;
    }
    if (now.wMonth == 12 && now.wDay == 31) {
        const int minutesLeft = 24 * 60 - (now.wHour * 60 + now.wMinute);
        const int hours = (minutesLeft + 59) / 60;
        c.big = format(L"%d", hours);
        const bool ru = effectiveLanguage() == Language::Russian;
        const StrId forms[3] = {StrId::CountdownHour1, StrId::CountdownHour2, StrId::CountdownHour5};
        c.caption = tr(ru ? forms[ruForm(hours)] : (hours == 1 ? StrId::CountdownHour1 : StrId::CountdownHour5));
        return c;
    }
    const long long days = dayNumber(now.wYear + 1, 1, 1) - dayNumber(now.wYear, now.wMonth, now.wDay);
    const int n = static_cast<int>(days);
    c.big = format(L"%d", n);
    const bool ru = effectiveLanguage() == Language::Russian;
    const StrId forms[3] = {StrId::CountdownDay1, StrId::CountdownDay2, StrId::CountdownDay5};
    c.caption = tr(ru ? forms[ruForm(n)] : (n == 1 ? StrId::CountdownDay1 : StrId::CountdownDay5));
    return c;
}

SYSTEMTIME countdownNow() {
    if (g_haveOverride) return g_override;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return st;
}

void setCountdownOverride(const SYSTEMTIME* t) {
    g_haveOverride = t != nullptr;
    if (t) g_override = *t;
}

}  // namespace snow::ui
