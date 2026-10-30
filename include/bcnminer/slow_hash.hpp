#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace bcnminer {

using SlowHash = std::array<std::uint8_t, 32>;
using SlowHashPair = std::array<SlowHash, 2>;

SlowHash bytecoin_slow_hash(std::span<const std::uint8_t> input);
SlowHashPair bytecoin_slow_hash_pair(
    std::span<const std::uint8_t> first,
    std::span<const std::uint8_t> second);
bool bytecoin_slow_hash_multi_hash_supported();

}
