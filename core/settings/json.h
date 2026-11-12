// Minimal JSON document model, parser and pretty writer (no third-party code).
//
// - Objects keep insertion order; setting an existing key replaces its value in place.
// - Strings are UTF-8. The parser rejects malformed UTF-8 and unpaired \u surrogates; the
//   writer emits non-ASCII characters verbatim and replaces malformed bytes with U+FFFD, so
//   its output is always valid UTF-8 JSON.
// - Numbers are doubles; integral values within +-2^53 are written without a fraction, so
//   integers round-trip exactly.
// - A leading UTF-8 BOM is tolerated; anything but whitespace after the document is an error.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace snow::json {

inline constexpr int kDefaultMaxDepth = 64;

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value();
    Value(std::nullptr_t);
    Value(bool b);
    Value(int n);
    Value(long long n);
    Value(double n);
    Value(const char* s);
    Value(std::string s);

    static Value array();
    static Value object();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Typed accessors; return `def` (or an empty string) when the type does not match.
    bool asBool(bool def = false) const;
    double asNumber(double def = 0.0) const;
    const std::string& asString() const;

    // Arrays: element count. Objects: member count. Otherwise 0.
    size_t size() const;
    // Array element / object member value by index (index must be < size()).
    const Value& at(size_t index) const;
    Value& at(size_t index);
    // Object member key by index.
    const std::string& keyAt(size_t index) const;

    // Object lookup; nullptr when this is not an object or the key is absent.
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    // Object insert-or-replace (a non-object value is turned into an empty object first).
    Value& set(std::string_view key, Value v);
    // Removes an object member; returns whether it existed.
    bool remove(std::string_view key);
    // Array append (a non-array value is turned into an empty array first).
    Value& push(Value v);

    bool operator==(const Value& other) const;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<std::string> keys_;   // object keys, parallel to items_
    std::vector<Value> items_;        // array elements or object values
};

// Parses a complete JSON document. On failure returns false, leaves `out` untouched and
// stores "JSON error at offset N: <reason>" in `error` (byte offset into `text`).
bool parse(std::string_view text, Value& out, std::string* error = nullptr,
           int maxDepth = kDefaultMaxDepth);

// Serializes with `indent` spaces per level ("\n" line ends, no trailing newline);
// indent <= 0 produces a compact single line.
std::string write(const Value& v, int indent = 2);

}  // namespace snow::json
