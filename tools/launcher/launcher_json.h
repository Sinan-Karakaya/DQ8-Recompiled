// Just enough JSON to read config/*/hashes.json: objects, arrays, strings,
// numbers and literals.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dq8::launcher {

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    // A member, or a null value when there is none.
    const JsonValue &operator[](const std::string &key) const;
};

// Parses `text`; false (with error) when it is not JSON.
bool parseJson(const std::string &text, JsonValue &out, std::string &error);

} // namespace dq8::launcher
