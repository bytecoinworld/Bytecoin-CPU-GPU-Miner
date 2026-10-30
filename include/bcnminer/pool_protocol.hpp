#pragma once

#include "bcnminer/pow_worker.hpp"
#include "bcnminer/json.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bcnminer {

struct PoolJob {
    std::string job_id;
    std::vector<std::uint8_t> hashing_blob;
    std::size_t nonce_offset = 39;
    std::uint64_t target = 0;
};

struct PoolMessage {
    std::optional<PoolJob> job;
    std::optional<double> request_id;
    bool has_result = false;
    bool has_error = false;
    std::string session_id;
    std::string status;
    std::string error_message;
};

std::vector<std::uint8_t> decode_hex(std::string_view text);
std::string encode_hex(std::span<const std::uint8_t> bytes);
PoolJob parse_pool_job(
    const JsonValue& job_value,
    std::size_t nonce_offset = 39);
PoolMessage parse_pool_message(
    const JsonValue& message,
    std::size_t nonce_offset = 39);
bool hash_meets_pool_target(const SlowHash& hash, std::uint64_t target);

}
