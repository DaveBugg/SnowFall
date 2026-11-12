// Uses a throw-away value name under HKCU\...\Run; the real "SnowFall" value is only
// read (to prove it is not modified) and the test value is always deleted at the end.
#include <windows.h>

#include "settings/autostart.h"
#include "settings/autostart_detail.h"
#include "snow_test.h"

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Reads a Run value; returns false when it does not exist.
bool readRunValue(const std::wstring& name, std::wstring& out) {
    wchar_t buf[2048] = {};
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, name.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     nullptr, buf, &bytes) != ERROR_SUCCESS) {
        return false;
    }
    out = buf;
    return true;
}

}  // namespace

TEST(autostart_command_line) {
    CHECK(snow::autostart::detail::commandLineFor(L"C:\\Program Files\\Snow\\SnowFall.exe") ==
          L"\"C:\\Program Files\\Snow\\SnowFall.exe\" --autostart");
    CHECK(std::wstring(snow::autostart::kValueName) == L"SnowFall");
}

TEST(autostart_registry_roundtrip) {
    using namespace snow::autostart::detail;
    const std::wstring name = L"SnowFallUnitTest_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring exe = L"C:\\Users\\Тест Юзер\\AppData\\Local\\SnowFall\\SnowFall.exe";
    const std::wstring other = L"C:\\Other\\SnowFall.exe";

    std::wstring realBefore, realAfter, value;
    const bool realExistedBefore = readRunValue(snow::autostart::kValueName, realBefore);

    CHECK(!readRunValue(name, value));
    CHECK(!isEnabledNamed(name.c_str(), exe));
    CHECK(setEnabledNamed(name.c_str(), false, exe));    // deleting an absent value succeeds
    CHECK(!setEnabledNamed(name.c_str(), true, L""));    // empty path is refused
    CHECK(!readRunValue(name, value));

    CHECK(setEnabledNamed(name.c_str(), true, exe));
    CHECK(readRunValue(name, value));
    CHECK(value == L"\"" + exe + L"\" --autostart");
    CHECK(isEnabledNamed(name.c_str(), exe));
    CHECK(isEnabledNamed(name.c_str(), L"c:\\users\\тест юзер\\APPDATA\\local\\snowfall\\SNOWFALL.EXE"));
    CHECK(!isEnabledNamed(name.c_str(), other));
    CHECK(!isEnabledNamed(name.c_str(), L""));

    // Re-registering with another path overwrites the value.
    CHECK(setEnabledNamed(name.c_str(), true, other));
    CHECK(isEnabledNamed(name.c_str(), other));
    CHECK(!isEnabledNamed(name.c_str(), exe));

    // An unquoted command line written by someone else is still recognised.
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        const std::wstring raw = L"C:\\Other\\SnowFall.exe --autostart";
        RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(raw.c_str()),
                       static_cast<DWORD>((raw.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        CHECK(isEnabledNamed(name.c_str(), other));
    }

    // Cleanup (also the "disable" path), then prove nothing is left behind.
    CHECK(setEnabledNamed(name.c_str(), false, exe));
    CHECK(!readRunValue(name, value));
    CHECK(!isEnabledNamed(name.c_str(), other));
    CHECK(setEnabledNamed(name.c_str(), false, exe));

    // The real value was not touched.
    const bool realExistsAfter = readRunValue(snow::autostart::kValueName, realAfter);
    CHECK(realExistedBefore == realExistsAfter);
    CHECK(realBefore == realAfter);
}
