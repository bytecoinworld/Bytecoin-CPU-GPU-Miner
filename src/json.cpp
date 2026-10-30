#include "bcnminer/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace bcnminer {

JsonValue::JsonValue(const std::nullptr_t) : value_(nullptr) {}
JsonValue::JsonValue(const bool value) : value_(value) {}
JsonValue::JsonValue(const double value) : value_(value) {}
JsonValue::JsonValue(std::string value) : value_(std::move(value)) {}
JsonValue::JsonValue(Array value) : value_(std::move(value)) {}
JsonValue::JsonValue(Object value) : value_(std::move(value)) {}

bool JsonValue::is_null() const {
    return std::holds_alternative<std::nullptr_t>(value_);
}

std::optional<bool> JsonValue::boolean() const {
    if (const auto* value = std::get_if<bool>(&value_)) {
        return *value;
    }
    return std::nullopt;
}

const std::string* JsonValue::string() const {
    return std::get_if<std::string>(&value_);
}

const JsonValue::Object* JsonValue::object() const {
    return std::get_if<Object>(&value_);
}

const JsonValue::Array* JsonValue::array() const {
    return std::get_if<Array>(&value_);
}

std::optional<double> JsonValue::number() const {
    if (const auto* value = std::get_if<double>(&value_)) {
        return *value;
    }
    return std::nullopt;
}

const JsonValue* JsonValue::get(const std::string_view key) const {
    const auto* values = object();
    if (values == nullptr) {
        return nullptr;
    }
    const auto it = values->find(key);
    return it == values->end() ? nullptr : &it->second;
}

std::string JsonValue::string_or(std::string fallback) const {
    const auto* value = string();
    return value == nullptr ? std::move(fallback) : *value;
}

namespace {

constexpr std::size_t max_json_depth = 64;

class JsonParser {
public:
    explicit JsonParser(const std::string_view input) : input_(input) {}

    JsonValue parse() {
        skip_whitespace();
        auto value = parse_value(0);
        skip_whitespace();
        if (position_ != input_.size()) {
            fail("Unexpected trailing JSON data.");
        }
        return value;
    }

private:
    [[noreturn]] void fail(const char* message) const {
        throw std::invalid_argument(
            std::string(message) + " At byte " + std::to_string(position_) + ".");
    }

    void skip_whitespace() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n')) {
            ++position_;
        }
    }

    bool consume(const char character) {
        if (position_ < input_.size() && input_[position_] == character) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(const char character) {
        if (!consume(character)) {
            fail("Unexpected JSON token.");
        }
    }

    JsonValue parse_value(const std::size_t depth) {
        if (depth > max_json_depth || position_ >= input_.size()) {
            fail("Invalid or excessively nested JSON.");
        }

        switch (input_[position_]) {
        case 'n':
            parse_literal("null");
            return JsonValue(nullptr);
        case 't':
            parse_literal("true");
            return JsonValue(true);
        case 'f':
            parse_literal("false");
            return JsonValue(false);
        case '"':
            return JsonValue(parse_string());
        case '[':
            return JsonValue(parse_array(depth + 1));
        case '{':
            return JsonValue(parse_object(depth + 1));
        default:
            return JsonValue(parse_number());
        }
    }

    void parse_literal(const std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            fail("Invalid JSON literal.");
        }
        position_ += literal.size();
    }

    static int hex_value(const char character) {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    }

    std::uint32_t parse_hex_quad() {
        if (input_.size() - position_ < 4) {
            fail("Incomplete JSON unicode escape.");
        }
        std::uint32_t codepoint = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto nibble = hex_value(input_[position_++]);
            if (nibble < 0) {
                fail("Invalid JSON unicode escape.");
            }
            codepoint = (codepoint << 4) | static_cast<std::uint32_t>(nibble);
        }
        return codepoint;
    }

    static void append_utf8(std::string& output, const std::uint32_t codepoint) {
        if (codepoint <= 0x7f) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }

    std::string parse_string() {
        expect('"');
        std::string output;
        while (position_ < input_.size()) {
            const auto character = input_[position_++];
            if (character == '"') {
                return output;
            }
            if (static_cast<unsigned char>(character) < 0x20) {
                fail("Control character in JSON string.");
            }
            if (character != '\\') {
                output.push_back(character);
                continue;
            }
            if (position_ >= input_.size()) {
                fail("Incomplete JSON escape.");
            }
            const auto escape = input_[position_++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                output.push_back(escape);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u': {
                auto codepoint = parse_hex_quad();
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (input_.size() - position_ < 6 ||
                        input_[position_] != '\\' ||
                        input_[position_ + 1] != 'u') {
                        fail("Missing low surrogate in JSON unicode escape.");
                    }
                    position_ += 2;
                    const auto low = parse_hex_quad();
                    if (low < 0xdc00 || low > 0xdfff) {
                        fail("Invalid low surrogate in JSON unicode escape.");
                    }
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
                        (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    fail("Unexpected low surrogate in JSON unicode escape.");
                }
                append_utf8(output, codepoint);
                break;
            }
            default:
                fail("Invalid JSON escape.");
            }
        }
        fail("Unterminated JSON string.");
    }

    JsonValue::Array parse_array(const std::size_t depth) {
        expect('[');
        skip_whitespace();
        JsonValue::Array values;
        if (consume(']')) {
            return values;
        }
        while (true) {
            skip_whitespace();
            values.push_back(parse_value(depth));
            skip_whitespace();
            if (consume(']')) {
                return values;
            }
            expect(',');
        }
    }

    JsonValue::Object parse_object(const std::size_t depth) {
        expect('{');
        skip_whitespace();
        JsonValue::Object values;
        if (consume('}')) {
            return values;
        }
        while (true) {
            skip_whitespace();
            if (position_ >= input_.size() || input_[position_] != '"') {
                fail("JSON object key must be a string.");
            }
            auto key = parse_string();
            skip_whitespace();
            expect(':');
            skip_whitespace();
            auto value = parse_value(depth);
            if (!values.emplace(std::move(key), std::move(value)).second) {
                fail("Duplicate JSON object key.");
            }
            skip_whitespace();
            if (consume('}')) {
                return values;
            }
            expect(',');
        }
    }

    double parse_number() {
        const auto start = position_;
        consume('-');
        if (consume('0')) {
            if (position_ < input_.size() &&
                input_[position_] >= '0' && input_[position_] <= '9') {
                fail("Leading zero in JSON number.");
            }
        } else {
            if (position_ >= input_.size() ||
                input_[position_] < '1' || input_[position_] > '9') {
                fail("Invalid JSON number.");
            }
            while (position_ < input_.size() &&
                   input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            const auto fraction_start = position_;
            while (position_ < input_.size() &&
                   input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (fraction_start == position_) {
                fail("Invalid JSON fraction.");
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const auto exponent_start = position_;
            while (position_ < input_.size() &&
                   input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (exponent_start == position_) {
                fail("Invalid JSON exponent.");
            }
        }

        double value = 0;
        const auto result = std::from_chars(
            input_.data() + start, input_.data() + position_, value);
        if (result.ec != std::errc{} || !std::isfinite(value)) {
            fail("JSON number is outside the supported range.");
        }
        return value;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

}

JsonValue parse_json(const std::string_view text) {
    return JsonParser(text).parse();
}

std::string json_escape(const std::string_view text) {
    constexpr char hex_digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(text.size() + 2);
    output.push_back('"');
    for (const auto character : text) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (byte < 0x20) {
                output += "\\u00";
                output.push_back(hex_digits[byte >> 4]);
                output.push_back(hex_digits[byte & 0x0f]);
            } else {
                output.push_back(character);
            }
        }
    }
    output.push_back('"');
    return output;
}

}
