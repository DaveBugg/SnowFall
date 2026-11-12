// Small shared helpers for the UI module (UTF-8 <-> UTF-16, printf-style formatting).
#pragma once
#include <windows.h>

#include <string>

namespace snow::ui {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);
std::wstring format(const wchar_t* fmt, ...);
std::wstring windowText(HWND hwnd);
void setTextIfChanged(HWND hwnd, const std::wstring& text);
bool equalsNoCase(const std::wstring& a, const std::wstring& b);
std::wstring trim(const std::wstring& s);

// Window property used by the selftest clipping check: extra horizontal space (DIPs) a
// custom-drawn control needs beside its text; -1 = do not measure (graphic-only control).
void setTextAllowance(HWND hwnd, int extraDip);
int textAllowance(HWND hwnd, bool* present);

}  // namespace snow::ui
