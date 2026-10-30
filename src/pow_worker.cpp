#include "bcnminer/pow_worker.hpp"

#include <array>
#include <stdexcept>
#include <vector>

namespace bcnminer {
namespace {

constexpr std::uint64_t hash_counter_update_interval = 32;

std::uint64_t load_little_endian_64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

std::uint64_t multiply_64_wide(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& high) {
    const auto left_low = static_cast<std::uint32_t>(left);
    const auto left_high = static_cast<std::uint32_t>(left >> 32);
    const auto right_low = static_cast<std::uint32_t>(right);
    const auto right_high = static_cast<std::uint32_t>(right >> 32);

    const auto low_product = static_cast<std::uint64_t>(left_low) * right_low;
    const auto cross_left = static_cast<std::uint64_t>(left_low) * right_high;
    const auto cross_right = static_cast<std::uint64_t>(left_high) * right_low;
    const auto middle = (low_product >> 32) +
        static_cast<std::uint32_t>(cross_left) +
        static_cast<std::uint32_t>(cross_right);

    high = static_cast<std::uint64_t>(left_high) * right_high +
        (cross_left >> 32) + (cross_right >> 32) + (middle >> 32);
    return (middle << 32) | static_cast<std::uint32_t>(low_product);
}

bool add_with_carry(
    const std::uint64_t left,
    const std::uint64_t right,
    const bool carry_in) {
    const auto sum = left + right;
    return sum < left || (carry_in && sum == UINT64_MAX);
}

}

bool hash_meets_target(const SlowHash& hash, const SlowHash& target) {
    for (std::size_t i = hash.size(); i-- > 0;) {
        if (hash[i] != target[i]) {
            return hash[i] < target[i];
        }
    }

    return true;
}

bool hash_meets_difficulty(const SlowHash& hash, const std::uint64_t difficulty) {
    if (difficulty == 0) {
        return false;
    }

    std::array<std::uint64_t, 4> words{};
    std::array<std::uint64_t, 4> product_high{};
    std::array<std::uint64_t, 4> product_low{};
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] = load_little_endian_64(hash.data() + i * 8);
        product_low[i] =
            multiply_64_wide(words[i], difficulty, product_high[i]);
    }
    if (product_high[3] != 0) {
        return false;
    }

    bool carry = false;
    for (std::size_t i = 0; i < 3; ++i) {
        carry = add_with_carry(product_high[i], product_low[i + 1], carry);
    }
    return !carry;
}

std::optional<NonceSolution> scan_nonce_range(
    const std::span<const std::uint8_t> hashing_blob,
    const std::size_t nonce_offset,
    const std::uint32_t start_nonce,
    const std::uint64_t nonce_count,
    const SlowHash& target,
    const std::stop_token stop,
    std::atomic<std::uint64_t>* hashes_computed,
    const bool use_multi_hash) {
    if (nonce_offset > hashing_blob.size() ||
        hashing_blob.size() - nonce_offset < sizeof(std::uint32_t)) {
        throw std::invalid_argument("Nonce field must fit within the hashing blob.");
    }
    if (nonce_count > (std::uint64_t{1} << 32)) {
        throw std::invalid_argument("Nonce range cannot exceed the 32-bit nonce space.");
    }

    std::vector<std::uint8_t> blob(hashing_blob.begin(), hashing_blob.end());
    const bool use_paired_hash =
        use_multi_hash && bytecoin_slow_hash_multi_hash_supported();
    auto second_blob = use_paired_hash ? blob : std::vector<std::uint8_t>{};
    const auto write_nonce = [nonce_offset](
                                 std::vector<std::uint8_t>& target_blob,
                                 const std::uint32_t nonce) {
        target_blob[nonce_offset] = static_cast<std::uint8_t>(nonce);
        target_blob[nonce_offset + 1] = static_cast<std::uint8_t>(nonce >> 8);
        target_blob[nonce_offset + 2] = static_cast<std::uint8_t>(nonce >> 16);
        target_blob[nonce_offset + 3] = static_cast<std::uint8_t>(nonce >> 24);
    };
    std::uint64_t unreported_hashes = 0;
    const auto publish_hash_count = [&] {
        if (hashes_computed != nullptr && unreported_hashes != 0) {
            hashes_computed->fetch_add(
                unreported_hashes, std::memory_order_relaxed);
        }
        unreported_hashes = 0;
    };
    for (std::uint64_t index = 0; index < nonce_count;) {
        if (stop.stop_requested()) {
            publish_hash_count();
            return std::nullopt;
        }

        const auto nonce = static_cast<std::uint32_t>(start_nonce + index);
        write_nonce(blob, nonce);
        const auto batch_size =
            use_paired_hash && nonce_count - index >= 2
            ? std::uint64_t{2}
            : 1;
        SlowHash hash{};
        SlowHash second_hash{};
        if (batch_size == 2) {
            const auto second_nonce =
                static_cast<std::uint32_t>(start_nonce + index + 1);
            write_nonce(second_blob, second_nonce);
            const auto hashes = bytecoin_slow_hash_pair(blob, second_blob);
            hash = hashes[0];
            second_hash = hashes[1];
        } else {
            hash = bytecoin_slow_hash(blob);
        }

        unreported_hashes += batch_size;
        if (unreported_hashes >= hash_counter_update_interval) {
            publish_hash_count();
        }
        if (hash_meets_target(hash, target)) {
            publish_hash_count();
            return NonceSolution{nonce, hash};
        }
        if (batch_size == 2 && hash_meets_target(second_hash, target)) {
            publish_hash_count();
            return NonceSolution{
                static_cast<std::uint32_t>(nonce + 1), second_hash};
        }
        index += batch_size;
    }

    publish_hash_count();
    return std::nullopt;
}

}
