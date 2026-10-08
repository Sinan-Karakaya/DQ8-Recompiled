#include "launcher_json.h"

#include <cctype>
#include <cstdlib>

namespace dq8::launcher {
namespace {
struct Parser {
    const std::string &text;
    size_t at = 0;
    std::string error;

    void space() {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
            ++at;
    }
    bool fail(const char *what) {
        if (error.empty())
            error = std::string(what) + " at offset " + std::to_string(at);
        return false;
    }
    bool literal(const char *word) {
        const std::string expected(word);
        if (text.compare(at, expected.size(), expected) != 0)
            return fail("unexpected text");
        at += expected.size();
        return true;
    }
    bool string(std::string &out) {
        if (at >= text.size() || text[at] != '"')
            return fail("expected a string");
        for (++at; at < text.size(); ++at) {
            const char c = text[at];
            if (c == '"') {
                ++at;
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (++at >= text.size())
                break;
            switch (text[at]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                // Basic plane only, as UTF-8; hashes.json never needs more.
                if (at + 4u >= text.size())
                    return fail("bad escape");
                const unsigned code = static_cast<unsigned>(std::strtoul(text.substr(at + 1u, 4u).c_str(), nullptr, 16));
                at += 4u;
                if (code < 0x80u) {
                    out += char(code);
                } else if (code < 0x800u) {
                    out += char(0xC0u | (code >> 6));
                    out += char(0x80u | (code & 0x3Fu));
                } else {
                    out += char(0xE0u | (code >> 12));
                    out += char(0x80u | ((code >> 6) & 0x3Fu));
                    out += char(0x80u | (code & 0x3Fu));
                }
                break;
            }
            default: out += text[at]; break;
            }
        }
        return fail("unterminated string");
    }
    bool value(JsonValue &out, int depth) {
        if (depth > 64)
            return fail("nesting too deep");
        space();
        if (at >= text.size())
            return fail("unexpected end");
        const char c = text[at];
        if (c == '{') {
            out.kind = JsonValue::Kind::Object;
            ++at;
            space();
            if (at < text.size() && text[at] == '}')
                return ++at, true;
            for (;;) {
                space();
                std::string key;
                if (!string(key))
                    return false;
                space();
                if (at >= text.size() || text[at++] != ':')
                    return fail("expected ':'");
                if (!value(out.object[key], depth + 1))
                    return false;
                space();
                if (at < text.size() && text[at] == ',') {
                    ++at;
                    continue;
                }
                if (at < text.size() && text[at] == '}')
                    return ++at, true;
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            out.kind = JsonValue::Kind::Array;
            ++at;
            space();
            if (at < text.size() && text[at] == ']')
                return ++at, true;
            for (;;) {
                out.array.emplace_back();
                if (!value(out.array.back(), depth + 1))
                    return false;
                space();
                if (at < text.size() && text[at] == ',') {
                    ++at;
                    continue;
                }
                if (at < text.size() && text[at] == ']')
                    return ++at, true;
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            out.kind = JsonValue::Kind::String;
            return string(out.string);
        }
        if (c == 't' || c == 'f') {
            out.kind = JsonValue::Kind::Bool;
            out.boolean = c == 't';
            return literal(c == 't' ? "true" : "false");
        }
        if (c == 'n')
            return literal("null");
        char *end = nullptr;
        out.number = std::strtod(text.c_str() + at, &end);
        if (end == text.c_str() + at)
            return fail("unexpected character");
        out.kind = JsonValue::Kind::Number;
        at = static_cast<size_t>(end - text.c_str());
        return true;
    }
};
} // namespace

const JsonValue &JsonValue::operator[](const std::string &key) const {
    static const JsonValue null;
    const auto it = object.find(key);
    return it == object.end() ? null : it->second;
}

bool parseJson(const std::string &text, JsonValue &out, std::string &error) {
    Parser parser{text};
    out = {};
    if (!parser.value(out, 0)) {
        error = parser.error;
        return false;
    }
    parser.space();
    if (parser.at != text.size()) {
        error = "trailing text at offset " + std::to_string(parser.at);
        return false;
    }
    return true;
}

} // namespace dq8::launcher
