#include "bcnminer/pool_protocol.hpp"

#include <stdexcept>

namespace bcnminer {
namespace {

const JsonValue* required_field(
    const JsonValue& object,
    const std::string_view field) {
    const auto* value = object.get(field);
    if (value == nullptr) {
        throw std::invalid_argument(
            "Pool message is missing required field '" + std::string(field) + "'.");
    }
    return value;
}

std::string required_string(
    const JsonValue& object,
    const std::string_view field) {
    const auto* value = required_field(object, field)->string();
    if (value == nullptr || value->empty()) {
        throw std::invalid_argument(
            "Pool field '" + std::string(field) + "' must be a non-empty string.");
    }
    return *value;
}

std::optional<PoolJob> find_job(
    const JsonValue& value,
    const std::size_t nonce_offset) {
    const auto* job = value.get("job");
    if (job == nullptr || job->is_null()) {
        return std::nullopt;
    }
    return parse_pool_job(*job, nonce_offset);
}

}

std::vector<std::uint8_t> decode_hex(const std::string_view text) {
    if (text.empty() || text.size() % 2 != 0) {
        throw std::invalid_argument("Hexadecimal data must have a positive even length.");
    }

    const auto nibble = [](const char character) -> int {
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
    };

    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const auto high = nibble(text[i]);
        const auto low = nibble(text[i + 1]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("Invalid hexadecimal data from pool.");
        }
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return bytes;
}

std::string encode_hex(const std::span<const std::uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 0x0f]);
    }
    return text;
}

PoolJob parse_pool_job(
    const JsonValue& job_value,
    const std::size_t nonce_offset) {
    const auto* object = job_value.object();
    if (object == nullptr) {
        throw std::invalid_argument("Pool job must be a JSON object.");
    }

    PoolJob job;
    job.job_id = required_string(job_value, "job_id");
    job.hashing_blob = decode_hex(required_string(job_value, "blob"));
    const auto target_bytes = decode_hex(required_string(job_value, "target"));
    if (target_bytes.size() != sizeof(std::uint32_t) &&
        target_bytes.size() != sizeof(job.target)) {
        throw std::invalid_argument(
            "CryptoNote Stratum target must be 4 or 8 bytes.");
    }
    if (target_bytes.size() == sizeof(std::uint32_t)) {
        const auto target32 = static_cast<std::uint32_t>(target_bytes[0]) |
            (static_cast<std::uint32_t>(target_bytes[1]) << 8) |
            (static_cast<std::uint32_t>(target_bytes[2]) << 16) |
            (static_cast<std::uint32_t>(target_bytes[3]) << 24);
        if (target32 == 0) {
            throw std::invalid_argument("Pool supplied a zero share target.");
        }
        job.target = UINT64_MAX / (UINT32_MAX / target32);
    } else {
        for (std::size_t i = 0; i < target_bytes.size(); ++i) {
            job.target |= static_cast<std::uint64_t>(target_bytes[i]) << (8 * i);
        }
    }
    job.nonce_offset = nonce_offset;
    if (nonce_offset > job.hashing_blob.size() ||
        job.hashing_blob.size() - nonce_offset < sizeof(std::uint32_t)) {
        throw std::invalid_argument(
            "Pool job blob does not contain a complete nonce at the configured offset.");
    }
    return job;
}

PoolMessage parse_pool_message(
    const JsonValue& message,
    const std::size_t nonce_offset) {
    if (message.object() == nullptr) {
        throw std::invalid_argument("Pool response must be a JSON object.");
    }

    PoolMessage parsed;
    if (const auto* id = message.get("id"); id != nullptr) {
        parsed.request_id = id->number();
    }
    parsed.has_result = message.get("result") != nullptr;
    parsed.has_error = message.get("error") != nullptr &&
        !message.get("error")->is_null();

    if (const auto* error = message.get("error"); error != nullptr &&
        !error->is_null()) {
    if (const auto* error_text = error->get("message"); error_text != nullptr) {
        parsed.error_message =
            error_text->string_or("Pool returned an unspecified error.");
    } else if (const auto* error_string = error->string(); error_string != nullptr) {
        parsed.error_message = *error_string;
        } else {
            parsed.error_message = "Pool returned an error response.";
        }
    }

    if (const auto* result = message.get("result"); result != nullptr) {
        if (const auto* session_id = result->get("id"); session_id != nullptr) {
            parsed.session_id = session_id->string_or();
        }
        if (const auto* status = result->get("status"); status != nullptr) {
            parsed.status = status->string_or();
        }
        parsed.job = find_job(*result, nonce_offset);
    }

    if (const auto* method = message.get("method");
        method != nullptr && method->string_or() == "job") {
        const auto* params = message.get("params");
        if (params == nullptr) {
            throw std::invalid_argument("Pool job notification is missing params.");
        }
        parsed.job = find_job(*params, nonce_offset);
        if (!parsed.job.has_value()) {
            parsed.job = parse_pool_job(*params, nonce_offset);
        }
    }

    return parsed;
}

bool hash_meets_pool_target(const SlowHash& hash, const std::uint64_t target) {
    std::uint64_t hash_word = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        hash_word |= static_cast<std::uint64_t>(hash[24 + i]) << (8 * i);
    }
    return hash_word <= target;
}

}
