#include "system/text_util.h"

#include <cstdint>

namespace snow::mac {

std::string toUtf8(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t wc : w) {
        auto c = static_cast<std::uint32_t>(wc);
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::wstring fromUtf8(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        std::uint32_t c = 0xFFFD;
        std::size_t len = 1;
        if (b0 < 0x80) {
            c = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            len = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            len = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            len = 4;
        }
        if (len > 1) {
            if (i + len > s.size()) {
                len = 1;
            } else {
                c = b0 & (0xFF >> (len + 1));
                bool ok = true;
                for (std::size_t k = 1; k < len; ++k) {
                    const auto b = static_cast<unsigned char>(s[i + k]);
                    if ((b & 0xC0) != 0x80) {
                        ok = false;
                        break;
                    }
                    c = (c << 6) | (b & 0x3F);
                }
                if (!ok) {
                    c = 0xFFFD;
                    len = 1;
                }
            }
        }
        out += static_cast<wchar_t>(c);
        i += len;
    }
    return out;
}

std::string toLowerAscii(std::string s) {
    for (char& ch : s)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return s;
}

}  // namespace snow::mac
