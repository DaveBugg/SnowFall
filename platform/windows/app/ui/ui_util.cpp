#include "ui_util.h"

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <cwctype>

namespace snow::ui {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring format(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, args);
    va_end(args);
    return buf;
}

std::wstring windowText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    const int got = GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<size_t>(std::max(got, 0)));
    return text;
}

void setTextIfChanged(HWND hwnd, const std::wstring& text) {
    if (hwnd && windowText(hwnd) != text) SetWindowTextW(hwnd, text.c_str());
}

bool equalsNoCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

void setTextAllowance(HWND hwnd, int extraDip) {
    SetPropW(hwnd, L"SnowTextExtra", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(extraDip + 1000)));
}

int textAllowance(HWND hwnd, bool* present) {
    const auto v = reinterpret_cast<INT_PTR>(GetPropW(hwnd, L"SnowTextExtra"));
    if (present) *present = v != 0;
    return v ? static_cast<int>(v - 1000) : 0;
}

}  // namespace snow::ui
