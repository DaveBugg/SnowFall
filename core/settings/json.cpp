#include "settings/json.h"

#include <charconv>
#include <cmath>
#include <system_error>
#include <utility>

namespace snow::json {

// ---------------------------------------------------------------- Value

Value::Value() = default;
Value::Value(std::nullptr_t) {}
Value::Value(bool b) : type_(Type::Bool), bool_(b) {}
Value::Value(int n) : type_(Type::Number), number_(n) {}
Value::Value(long long n) : type_(Type::Number), number_(static_cast<double>(n)) {}
Value::Value(double n) : type_(Type::Number), number_(n) {}
Value::Value(const char* s) : type_(Type::String), string_(s ? s : "") {}
Value::Value(std::string s) : type_(Type::String), string_(std::move(s)) {}

Value Value::array() {
    Value v;
    v.type_ = Type::Array;
    return v;
}

Value Value::object() {
    Value v;
    v.type_ = Type::Object;
    return v;
}

bool Value::asBool(bool def) const { return type_ == Type::Bool ? bool_ : def; }
double Value::asNumber(double def) const { return type_ == Type::Number ? number_ : def; }

const std::string& Value::asString() const {
    static const std::string empty;
    return type_ == Type::String ? string_ : empty;
}

size_t Value::size() const { return items_.size(); }
const Value& Value::at(size_t index) const { return items_[index]; }
Value& Value::at(size_t index) { return items_[index]; }
const std::string& Value::keyAt(size_t index) const { return keys_[index]; }

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) return &items_[i];
    }
    return nullptr;
}

Value* Value::find(std::string_view key) {
    return const_cast<Value*>(std::as_const(*this).find(key));
}

Value& Value::set(std::string_view key, Value v) {
    if (type_ != Type::Object) *this = object();
    if (Value* existing = find(key)) {
        *existing = std::move(v);
        return *existing;
    }
    keys_.emplace_back(key);
    items_.push_back(std::move(v));
    return items_.back();
}

bool Value::remove(std::string_view key) {
    if (type_ != Type::Object) return false;
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(i));
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

Value& Value::push(Value v) {
    if (type_ != Type::Array) *this = array();
    items_.push_back(std::move(v));
    return items_.back();
}

bool Value::operator==(const Value& other) const {
    if (type_ != other.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return bool_ == other.bool_;
        case Type::Number: return number_ == other.number_;
        case Type::String: return string_ == other.string_;
        case Type::Array: return items_ == other.items_;
        case Type::Object: return keys_ == other.keys_ && items_ == other.items_;
    }
    return false;
}

// ---------------------------------------------------------------- UTF-8 helpers

namespace {

void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Length (2..4) of the well-formed multi-byte UTF-8 sequence starting at s[i], or 0 when
// it is malformed (bad lead/continuation byte, overlong form, surrogate, > U+10FFFF,
// truncated).
size_t utf8SequenceLength(std::string_view s, size_t i) {
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char lead = byte(i);
    size_t len = 0;
    unsigned cp = 0;
    unsigned minCp = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        len = 2, cp = lead & 0x1Fu, minCp = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        len = 3, cp = lead & 0x0Fu, minCp = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        len = 4, cp = lead & 0x07u, minCp = 0x10000;
    } else {
        return 0;
    }
    if (i + len > s.size()) return 0;
    for (size_t k = 1; k < len; ++k) {
        const unsigned char c = byte(i + k);
        if ((c & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (c & 0x3Fu);
    }
    if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

// ---------------------------------------------------------------- Parser

class Parser {
public:
    Parser(std::string_view text, int maxDepth) : s_(text), maxDepth_(maxDepth) {}

    bool run(Value& out, std::string* error) {
        if (s_.size() >= 3 && s_.substr(0, 3) == "\xEF\xBB\xBF") i_ = 3;   // UTF-8 BOM
        Value v;
        bool ok = false;
        skipWs();
        if (i_ >= s_.size()) {
            fail("empty document");
        } else if (parseValue(v, 1)) {
            skipWs();
            ok = i_ == s_.size() || fail("unexpected characters after the JSON document");
        }
        if (!ok) {
            if (error) {
                *error = "JSON error at offset " + std::to_string(errorPos_) + ": " + error_;
            }
            return false;
        }
        out = std::move(v);
        return true;
    }

private:
    bool fail(const char* message) {
        if (error_.empty()) {
            error_ = message;
            errorPos_ = i_;
        }
        return false;
    }

    bool atEnd() const { return i_ >= s_.size(); }

    void skipWs() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++i_;
        }
    }

    bool literal(std::string_view word) {
        if (s_.substr(i_, word.size()) != word) return fail("invalid literal");
        i_ += word.size();
        return true;
    }

    bool parseValue(Value& out, int depth) {
        if (atEnd()) return fail("unexpected end of input");
        switch (s_[i_]) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string str;
                if (!parseString(str)) return false;
                out = Value(std::move(str));
                return true;
            }
            case 't':
                if (!literal("true")) return false;
                out = Value(true);
                return true;
            case 'f':
                if (!literal("false")) return false;
                out = Value(false);
                return true;
            case 'n':
                if (!literal("null")) return false;
                out = Value(nullptr);
                return true;
            default: return parseNumber(out);
        }
    }

    bool parseObject(Value& out, int depth) {
        if (depth > maxDepth_) return fail("nesting too deep");
        ++i_;   // '{'
        out = Value::object();
        skipWs();
        if (!atEnd() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            skipWs();
            if (atEnd()) return fail("unexpected end of input in object");
            if (s_[i_] != '"') return fail("expected a string key");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (atEnd()) return fail("unexpected end of input in object");
            if (s_[i_] != ':') return fail("expected ':' after object key");
            ++i_;
            skipWs();
            Value member;
            if (!parseValue(member, depth + 1)) return false;
            out.set(key, std::move(member));   // duplicate key: the last one wins
            skipWs();
            if (atEnd()) return fail("unexpected end of input in object");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            return fail("expected ',' or '}' in object");
        }
    }

    bool parseArray(Value& out, int depth) {
        if (depth > maxDepth_) return fail("nesting too deep");
        ++i_;   // '['
        out = Value::array();
        skipWs();
        if (!atEnd() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            skipWs();
            Value element;
            if (!parseValue(element, depth + 1)) return false;
            out.push(std::move(element));
            skipWs();
            if (atEnd()) return fail("unexpected end of input in array");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                return true;
            }
            return fail("expected ',' or ']' in array");
        }
    }

    bool parseHex4(unsigned& out) {
        if (i_ + 4 > s_.size()) return fail("truncated \\u escape");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_ + static_cast<size_t>(k)];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a') + 10;
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A') + 10;
            else return fail("invalid hex digit in \\u escape");
            v = (v << 4) | digit;
        }
        i_ += 4;
        out = v;
        return true;
    }

    bool parseString(std::string& out) {
        ++i_;   // opening quote
        for (;;) {
            if (atEnd()) return fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') {
                ++i_;
                return true;
            }
            if (c < 0x20) return fail("unescaped control character in string");
            if (c == '\\') {
                ++i_;
                if (atEnd()) return fail("unterminated string");
                const char e = s_[i_];
                ++i_;
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!parseHex4(cp)) return false;
                        if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("unpaired low surrogate");
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (s_.substr(i_, 2) != "\\u") return fail("unpaired high surrogate");
                            i_ += 2;
                            unsigned low = 0;
                            if (!parseHex4(low)) return false;
                            if (low < 0xDC00 || low > 0xDFFF) {
                                return fail("unpaired high surrogate");
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default:
                        --i_;
                        return fail("invalid escape sequence");
                }
                continue;
            }
            if (c < 0x80) {
                out += static_cast<char>(c);
                ++i_;
                continue;
            }
            const size_t len = utf8SequenceLength(s_, i_);
            if (len == 0) return fail("invalid UTF-8 sequence in string");
            out.append(s_.substr(i_, len));
            i_ += len;
        }
    }

    bool isDigit() const { return !atEnd() && s_[i_] >= '0' && s_[i_] <= '9'; }

    bool parseNumber(Value& out) {
        const size_t start = i_;
        if (!atEnd() && s_[i_] == '-') ++i_;
        if (!isDigit()) {
            i_ = start;
            return fail("unexpected character");
        }
        if (s_[i_] == '0') {
            ++i_;
        } else {
            while (isDigit()) ++i_;
        }
        if (!atEnd() && s_[i_] == '.') {
            ++i_;
            if (!isDigit()) return fail("expected a digit after the decimal point");
            while (isDigit()) ++i_;
        }
        if (!atEnd() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (!atEnd() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (!isDigit()) return fail("expected a digit in the exponent");
            while (isDigit()) ++i_;
        }
        double d = 0.0;
        const auto res = std::from_chars(s_.data() + start, s_.data() + i_, d);
        if (res.ec != std::errc() || res.ptr != s_.data() + i_) {
            i_ = start;
            return fail("number out of range");
        }
        out = Value(d);
        return true;
    }

    std::string_view s_;
    size_t i_ = 0;
    int maxDepth_;
    std::string error_;
    size_t errorPos_ = 0;
};

// ---------------------------------------------------------------- Writer

void writeString(std::string& out, std::string_view s) {
    static const char kHex[] = "0123456789abcdef";
    out += '"';
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        out += "\\u00";
                        out += kHex[c >> 4];
                        out += kHex[c & 0xF];
                    } else {
                        out += static_cast<char>(c);
                    }
            }
            ++i;
            continue;
        }
        const size_t len = utf8SequenceLength(s, i);
        if (len == 0) {
            out += "\xEF\xBF\xBD";   // U+FFFD for a malformed byte
            ++i;
        } else {
            out.append(s.substr(i, len));
            i += len;
        }
    }
    out += '"';
}

void writeNumber(std::string& out, double d) {
    if (!std::isfinite(d)) {
        out += "null";   // JSON has no NaN/Infinity
        return;
    }
    char buf[40];
    std::to_chars_result res{};
    if (d == std::floor(d) && std::fabs(d) < 9007199254740992.0) {
        res = std::to_chars(buf, buf + sizeof(buf), static_cast<long long>(d));
    } else {
        res = std::to_chars(buf, buf + sizeof(buf), d);   // shortest round-trip form
    }
    out.append(buf, res.ptr);
}

void newline(std::string& out, int indent, int level) {
    if (indent <= 0) return;
    out += '\n';
    out.append(static_cast<size_t>(indent) * static_cast<size_t>(level), ' ');
}

void writeValue(std::string& out, const Value& v, int indent, int level) {
    switch (v.type()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
        case Value::Type::Number: writeNumber(out, v.asNumber()); break;
        case Value::Type::String: writeString(out, v.asString()); break;
        case Value::Type::Array:
        case Value::Type::Object: {
            const bool isObject = v.isObject();
            out += isObject ? '{' : '[';
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) out += ',';
                newline(out, indent, level + 1);
                if (isObject) {
                    writeString(out, v.keyAt(i));
                    out += indent > 0 ? ": " : ":";
                }
                writeValue(out, v.at(i), indent, level + 1);
            }
            if (v.size()) newline(out, indent, level);
            out += isObject ? '}' : ']';
            break;
        }
    }
}

}  // namespace

bool parse(std::string_view text, Value& out, std::string* error, int maxDepth) {
    return Parser(text, maxDepth).run(out, error);
}

std::string write(const Value& v, int indent) {
    std::string out;
    writeValue(out, v, indent, 0);
    return out;
}

}  // namespace snow::json
