// UTF-8 <-> std::wstring (UTF-32 on macOS) helpers. The core contracts use std::wstring for
// paths (Windows heritage); on macOS everything else is UTF-8.
#pragma once
#include <string>

namespace snow::mac {

std::string toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);
std::string toLowerAscii(std::string s);

}  // namespace snow::mac
