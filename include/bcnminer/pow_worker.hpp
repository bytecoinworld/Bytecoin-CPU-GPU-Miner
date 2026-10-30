#pragma once

#include "bcnminer/slow_hash.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>

namespace bcnminer {

struct NonceSolution {
    std::uint32_t nonce;
    SlowHash hash;
};

bool hash_meets_target(const SlowHash& hash, const SlowHash& target);
bool hash_meets_difficulty(const SlowHash& hash, std::uint64_t difficulty);

// The target is interpreted as a little-endian 256-bit integer.
std::optional<NonceSolution> scan_nonce_range(
    std::span<const std::uint8_t> hashing_blob,
    std::size_t nonce_offset,
    std::uint32_t start_nonce,
    std::uint64_t nonce_count,
    const SlowHash& target,
    std::stop_token stop = {},
    std::atomic<std::uint64_t>* hashes_computed = nullptr,
    bool use_multi_hash = false);

}
