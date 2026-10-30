#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bcnminer {

class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;

    JsonValue() = default;
    explicit JsonValue(std::nullptr_t);
    explicit JsonValue(bool value);
    explicit JsonValue(double value);
    explicit JsonValue(std::string value);
    explicit JsonValue(Array value);
    explicit JsonValue(Object value);

    bool is_null() const;
    std::optional<bool> boolean() const;
    const std::string* string() const;
    const Object* object() const;
    const Array* array() const;
    std::optional<double> number() const;
    const JsonValue* get(std::string_view key) const;
    std::string string_or(std::string fallback = {}) const;

private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value_ =
        nullptr;
};

JsonValue parse_json(std::string_view text);
std::string json_escape(std::string_view text);

}
