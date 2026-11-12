#include "settings/autostart.h"

#include <windows.h>

#include "diagnostics/log.h"
#include "settings/autostart_detail.h"

namespace snow::autostart {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Extracts the executable path from a Run command line: the quoted part, or the text up
// to the first space when it is not quoted.
std::wstring exeFromCommandLine(const std::wstring& cmd) {
    size_t begin = cmd.find_first_not_of(L" \t");
    if (begin == std::wstring::npos) return {};
    if (cmd[begin] == L'"') {
        const size_t end = cmd.find(L'"', begin + 1);
        return cmd.substr(begin + 1, end == std::wstring::npos ? end : end - begin - 1);
    }
    const size_t end = cmd.find_first_of(L" \t", begin);
    return cmd.substr(begin, end == std::wstring::npos ? end : end - begin);
}

}  // namespace

namespace detail {

std::wstring commandLineFor(const std::wstring& exePath) {
    return L"\"" + exePath + L"\" --autostart";
}

bool setEnabledNamed(const wchar_t* valueName, bool enabled, const std::wstring& exePath) {
    if (enabled && exePath.empty()) return false;
    HKEY key = nullptr;
    LSTATUS rc = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                 KEY_SET_VALUE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        SNOW_LOGW("autostart: cannot open the Run key (error %ld)", static_cast<long>(rc));
        return false;
    }
    if (enabled) {
        const std::wstring cmd = commandLineFor(exePath);
        rc = RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, valueName);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;   // already absent
    }
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        SNOW_LOGW("autostart: cannot %s the Run value (error %ld)", enabled ? "set" : "delete",
                  static_cast<long>(rc));
        return false;
    }
    return true;
}

bool isEnabledNamed(const wchar_t* valueName, const std::wstring& exePath) {
    if (exePath.empty()) return false;
    std::wstring buf(512, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        DWORD bytes = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
        const LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER, kRunKey, valueName,
                                        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                                        nullptr, buf.data(), &bytes);
        if (rc == ERROR_MORE_DATA) {
            buf.assign(bytes / sizeof(wchar_t) + 1, L'\0');
            continue;
        }
        if (rc != ERROR_SUCCESS) return false;
        buf.resize(wcsnlen(buf.c_str(), bytes / sizeof(wchar_t)));
        const std::wstring exe = exeFromCommandLine(buf);
        return CompareStringOrdinal(exe.c_str(), static_cast<int>(exe.size()), exePath.c_str(),
                                    static_cast<int>(exePath.size()), TRUE) == CSTR_EQUAL;
    }
    return false;
}

}  // namespace detail

bool setEnabled(bool enabled, const std::wstring& exePath) {
    return detail::setEnabledNamed(kValueName, enabled, exePath);
}

bool isEnabled(const std::wstring& exePath) { return detail::isEnabledNamed(kValueName, exePath); }

}  // namespace snow::autostart
