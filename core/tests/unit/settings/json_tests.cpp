#include "settings/json.h"
#include "snow_test.h"

using snow::json::Value;
namespace json = snow::json;

namespace {

Value parsed(const std::string& text) {
    Value v;
    std::string err;
    const bool ok = json::parse(text, v, &err);
    if (!ok) printf("  unexpected parse failure: %s\n", err.c_str());
    CHECK(ok);
    return v;
}

bool fails(const std::string& text, std::string* err = nullptr) {
    Value v = Value(123);
    std::string e;
    const bool ok = json::parse(text, v, &e);
    if (err) *err = e;
    // On failure the output must be untouched and the message must carry an offset.
    return !ok && v == Value(123) && e.find("JSON error at offset ") == 0;
}

}  // namespace

TEST(json_scalars) {
    CHECK(parsed("null").isNull());
    CHECK(parsed("true").asBool() == true);
    CHECK(parsed("false").isBool() && parsed("false").asBool(true) == false);
    CHECK(parsed("  42  ").asNumber() == 42.0);
    CHECK(parsed("\"hi\"").asString() == "hi");
    CHECK(parsed("\"\"").isString() && parsed("\"\"").asString().empty());
}

TEST(json_numbers) {
    CHECK(parsed("0").asNumber() == 0.0);
    CHECK(parsed("-0").asNumber() == 0.0);
    CHECK(parsed("-17").asNumber() == -17.0);
    CHECK_NEAR(parsed("3.25").asNumber(), 3.25, 0);
    CHECK_NEAR(parsed("1e3").asNumber(), 1000.0, 0);
    CHECK_NEAR(parsed("1.5E+2").asNumber(), 150.0, 0);
    CHECK_NEAR(parsed("25e-1").asNumber(), 2.5, 0);
    CHECK(parsed("9007199254740991").asNumber() == 9007199254740991.0);

    CHECK(json::write(Value(55)) == "55");
    CHECK(json::write(Value(-100)) == "-100");
    CHECK(json::write(Value(0.5)) == "0.5");
    CHECK(json::write(Value(1e3)) == "1000");
    CHECK(json::write(Value(-0.0)) == "0");
    CHECK(json::write(Value(2147483647)) == "2147483647");
    CHECK(json::write(Value(9007199254740991LL)) == "9007199254740991");
    CHECK(json::write(Value(std::nan(""))) == "null");
    // Non-integers round-trip through the shortest representation.
    for (double d : {0.1, -2.75, 1e-7, 1.7976931348623157e308, 123456.789}) {
        CHECK(parsed(json::write(Value(d))).asNumber() == d);
    }

    CHECK(fails("01"));
    CHECK(fails("+1"));
    CHECK(fails("1."));
    CHECK(fails(".5"));
    CHECK(fails("1e"));
    CHECK(fails("1e+"));
    CHECK(fails("-"));
    CHECK(fails("0x10"));
    CHECK(fails("1e999"));
    CHECK(fails("NaN"));
    CHECK(fails("Infinity"));
}

TEST(json_string_escapes) {
    // The inputs live in variables: MSVC mis-stringifies raw strings / \u inside macro arguments.
    const std::string Q = "\"";
    const std::string U = std::string(1, char(92)) + "u";   // backslash-u, the JSON escape prefix
    const std::string plain = std::string("a\"b") + char(92) + "c/d\b\f\n\r\t";
    const std::string simple = R"("a\"b\\c\/d\b\f\n\r\t")";
    const std::string bmp = Q + U + "0041" + U + "00e9" + U + "0416" + U + "20AC" + Q;
    const std::string nul = Q + U + "0000" + Q;
    const std::string pair1 = Q + U + "d83c" + U + "df28" + Q;   // U+1F328 (cloud with snow)
    const std::string pair2 = Q + U + "DBFF" + U + "DFFF" + Q;   // U+10FFFF
    CHECK(simple.size() == 22 && bmp.size() == 26 && pair1.size() == 14);
    CHECK(parsed(simple).asString() == plain);
    CHECK(parsed(bmp).asString() == "A\xC3\xA9\xD0\x96\xE2\x82\xAC");
    CHECK(parsed(nul).asString() == std::string(1, '\0'));
    CHECK(parsed(pair1).asString() == "\xF0\x9F\x8C\xA8");
    CHECK(parsed(pair2).asString() == "\xF4\x8F\xBF\xBF");

    const std::string writtenSimple = R"("a\"b\\c/\b\f\n\r\t")";
    const std::string writtenControl = Q + U + "0001" + U + "001f" + Q;
    CHECK(writtenSimple.size() == 20);
    CHECK(json::write(Value(plain.substr(0, 4) + plain.substr(4, 2) + plain.substr(7))) == writtenSimple);
    CHECK(json::write(Value(std::string("\x01\x1f", 2))) == writtenControl);
    CHECK(json::write(Value(std::string(1, '\0'))) == nul);
    CHECK(parsed(writtenSimple).asString() == plain.substr(0, 6) + plain.substr(7));

    const std::string bad[] = {
        Q + char(92) + "x41" + Q,               // unknown escape
        Q + U + "12" + Q,                       // short escape
        Q + U + "12G4" + Q,                     // bad hex digit
        Q + U + "d83c" + Q,                     // lone high surrogate
        Q + U + "d83c" + U + "0041" + Q,        // high surrogate + non-surrogate
        Q + U + "d83c" + U + "d83c" + Q,        // high surrogate + high surrogate
        Q + U + "df28" + Q,                     // lone low surrogate
        Q + U + "df28" + U + "d83c" + Q,        // reversed pair
        "\"line\nbreak\"",                      // raw control characters
        "\"tab\there\"",
        "\"unterminated",
        Q + "bad" + char(92),
        Q + "bad" + U,
    };
    for (const std::string& text : bad) {
        const bool rejected = fails(text);
        if (!rejected) printf("  should have been rejected: %s\n", text.c_str());
        CHECK(rejected);
    }
}

TEST(json_unicode_cyrillic) {
    const std::string text = "{\"имя\": \"Снежинка ❄ 🌨\", \"список\": [\"игра.exe\", \"Ёлка\"]}";
    const Value v = parsed(text);
    CHECK(v.find("имя") && v.find("имя")->asString() == "Снежинка ❄ 🌨");
    CHECK(v.find("список") && v.find("список")->size() == 2);
    CHECK(v.find("список")->at(1).asString() == "Ёлка");
    // Non-ASCII is written verbatim (not \u-escaped) and round-trips.
    const std::string out = json::write(v, 0);
    CHECK(out == "{\"имя\":\"Снежинка ❄ 🌨\",\"список\":[\"игра.exe\",\"Ёлка\"]}");
    CHECK(parsed(out) == v);
}

TEST(json_invalid_utf8) {
    CHECK(fails("\"\xFF\""));
    CHECK(fails("\"\xC0\xAF\""));           // overlong
    CHECK(fails("\"\xD0\""));               // truncated sequence
    CHECK(fails("\"\xED\xA0\x80\""));       // encoded surrogate
    CHECK(fails("\"\xF4\x90\x80\x80\""));   // > U+10FFFF
    CHECK(fails("\"\x80\""));               // stray continuation byte
    // The writer never emits malformed UTF-8: bad bytes become U+FFFD.
    const std::string out = json::write(Value(std::string("a\xFF" "b\xD0")));
    CHECK(out == "\"a\xEF\xBF\xBD" "b\xEF\xBF\xBD\"");
    CHECK(parsed(out).isString());
}

TEST(json_nested_structures) {
    const Value v = parsed(R"({"a": {"b": [1, {"c": [true, null, "x"]}, []], "d": {}}, "e": [[], [[2]]]})");
    CHECK(v.isObject() && v.size() == 2);
    const Value* b = v.find("a")->find("b");
    CHECK(b && b->isArray() && b->size() == 3);
    CHECK(b->at(0).asNumber() == 1.0);
    const Value* c = b->at(1).find("c");
    CHECK(c && c->size() == 3 && c->at(0).asBool() && c->at(1).isNull() && c->at(2).asString() == "x");
    CHECK(b->at(2).isArray() && b->at(2).size() == 0);
    CHECK(v.find("a")->find("d")->isObject() && v.find("a")->find("d")->size() == 0);
    CHECK(v.find("e")->at(1).at(0).at(0).asNumber() == 2.0);
    CHECK(v.find("missing") == nullptr);
    CHECK(v.find("e")->find("a") == nullptr);   // find on an array
    CHECK(parsed(json::write(v)) == v);
    CHECK(parsed(json::write(v, 0)) == v);
}

TEST(json_object_order_and_mutation) {
    const Value v = parsed(R"({"zeta": 1, "alpha": 2, "mid": 3})");
    CHECK(v.keyAt(0) == "zeta" && v.keyAt(1) == "alpha" && v.keyAt(2) == "mid");
    CHECK(json::write(v, 0) == R"({"zeta":1,"alpha":2,"mid":3})");

    Value o = Value::object();
    o.set("b", 1);
    o.set("a", "x");
    o.set("b", true);   // replace keeps the position
    CHECK(o.size() == 2 && o.keyAt(0) == "b" && o.at(0).asBool());
    CHECK(o.remove("b") && !o.remove("b") && o.size() == 1 && o.keyAt(0) == "a");

    // Duplicate keys: the last value wins.
    const Value dup = parsed(R"({"k": 1, "k": 2})");
    CHECK(dup.size() == 1 && dup.find("k")->asNumber() == 2.0);

    // Typed accessors fall back on a type mismatch.
    CHECK(Value("x").asNumber(7.0) == 7.0);
    CHECK(Value(1).asBool(true) == true);
    CHECK(Value(1).asString().empty());
    CHECK(!(Value(1) == Value("1")));
    CHECK(!(Value(nullptr) == Value(false)));
}

TEST(json_pretty_output) {
    Value o = Value::object();
    o.set("a", 1);
    Value& arr = o.set("list", Value::array());
    arr.push("x");
    arr.push(false);
    o.set("emptyObj", Value::object());
    o.set("emptyArr", Value::array());
    const std::string expected =
        "{\n"
        "  \"a\": 1,\n"
        "  \"list\": [\n"
        "    \"x\",\n"
        "    false\n"
        "  ],\n"
        "  \"emptyObj\": {},\n"
        "  \"emptyArr\": []\n"
        "}";
    CHECK(json::write(o) == expected);
    CHECK(json::write(o, 0) == R"({"a":1,"list":["x",false],"emptyObj":{},"emptyArr":[]})");
    CHECK(parsed(expected) == o);
}

TEST(json_whitespace_and_bom) {
    CHECK(parsed("\xEF\xBB\xBF{\"a\": 1}").find("a")->asNumber() == 1.0);
    CHECK(parsed(" \t\r\n[ 1 ,\n 2 ]\r\n ").size() == 2);
    CHECK(fails("\xEF\xBB\xBF"));
    CHECK(fails("{\"a\": 1}\xEF\xBB\xBF"));   // BOM is only tolerated at the start
}

TEST(json_errors) {
    CHECK(fails(""));
    CHECK(fails("   "));
    CHECK(fails("{"));
    CHECK(fails("{\"a\""));
    CHECK(fails("{\"a\":"));
    CHECK(fails("{\"a\": 1"));
    CHECK(fails("{\"a\": 1,"));
    CHECK(fails("[1, 2"));
    CHECK(fails("{\"a\": 1,}"));       // trailing comma
    CHECK(fails("[1, 2,]"));
    CHECK(fails("[,]"));
    CHECK(fails("{a: 1}"));            // unquoted key
    CHECK(fails("{'a': 1}"));
    CHECK(fails("{\"a\" 1}"));
    CHECK(fails("{\"a\": 1 \"b\": 2}"));
    CHECK(fails("[1 2]"));
    CHECK(fails("tru"));
    CHECK(fails("nul"));
    CHECK(fails("True"));
    CHECK(fails("garbage"));
    CHECK(fails("{} x"));              // trailing garbage
    CHECK(fails("{}{}"));
    CHECK(fails("1 2"));
    CHECK(fails("// comment\n{}"));
    CHECK(fails(std::string("{\"a\": 1}\0", 9)));

    std::string err;
    CHECK(fails("{\"a\": 1,}", &err));
    CHECK(err == "JSON error at offset 8: expected a string key");
    CHECK(fails("[1, 2] tail", &err));
    CHECK(err.find("offset 7:") != std::string::npos);
    CHECK(fails("{\"a\": tru}", &err));
    CHECK(err.find("offset 6:") != std::string::npos);
}

TEST(json_depth_limit) {
    const auto nested = [](int depth, char open, char close) {
        return std::string(static_cast<size_t>(depth), open) + std::string(static_cast<size_t>(depth), close);
    };
    CHECK(parsed(nested(json::kDefaultMaxDepth, '[', ']')).isArray());
    std::string err;
    CHECK(fails(nested(json::kDefaultMaxDepth + 1, '[', ']'), &err));
    CHECK(err.find("nesting too deep") != std::string::npos);
    CHECK(fails(std::string(100000, '[')));   // must not overflow the stack

    std::string objects;
    for (int i = 0; i < json::kDefaultMaxDepth + 1; ++i) objects += "{\"a\":";
    objects += "1";
    objects += std::string(static_cast<size_t>(json::kDefaultMaxDepth) + 1, '}');
    CHECK(fails(objects));

    Value v;
    CHECK(json::parse("[[1]]", v, nullptr, 2));
    CHECK(!json::parse("[[[1]]]", v, nullptr, 2));
}
