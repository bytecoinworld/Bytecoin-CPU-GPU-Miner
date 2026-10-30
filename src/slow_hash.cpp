#include "bcnminer/slow_hash.hpp"

#include "hash.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(BCNMINER_USE_BYTECOIN_X86_HASH)
extern "C" int crypto_cn_slow_hash_double_supported();
extern "C" void crypto_cn_slow_hash_double(
    void* context0,
    const void* data0,
    std::size_t length0,
    cryptoHash* hash0,
    void* context1,
    const void* data1,
    std::size_t length1,
    cryptoHash* hash1);
#endif

namespace bcnminer {
namespace {

std::vector<std::max_align_t>& thread_context() {
    thread_local std::vector<std::max_align_t> context(
        (SLOW_HASH_CONTEXT_SIZE + sizeof(std::max_align_t) - 1) /
        sizeof(std::max_align_t));
    return context;
}

#if defined(BCNMINER_USE_BYTECOIN_X86_HASH)
std::vector<std::max_align_t> make_context() {
    return std::vector<std::max_align_t>(
        (SLOW_HASH_CONTEXT_SIZE + sizeof(std::max_align_t) - 1) /
        sizeof(std::max_align_t));
}

std::array<std::vector<std::max_align_t>, 2>& thread_pair_contexts() {
    thread_local std::array<std::vector<std::max_align_t>, 2> contexts{
        make_context(), make_context()};
    return contexts;
}
#endif

}

SlowHash bytecoin_slow_hash(const std::span<const std::uint8_t> input) {
    auto& context = thread_context();
    cryptoHash hash{};
    const std::uint8_t empty_input = 0;
    const auto* data = input.empty() ? &empty_input : input.data();

#if defined(BCNMINER_USE_BYTECOIN_X86_HASH)
    crypto_cn_slow_hash(context.data(), data, input.size(), &hash);
#else
    crypto_cn_slow_hash_platform_independent(
        context.data(), data, input.size(), &hash);
#endif

    SlowHash result{};
    std::copy(std::begin(hash.data), std::end(hash.data), result.begin());
    return result;
}

SlowHashPair bytecoin_slow_hash_pair(
    const std::span<const std::uint8_t> first,
    const std::span<const std::uint8_t> second) {
#if defined(BCNMINER_USE_BYTECOIN_X86_HASH)
    auto& contexts = thread_pair_contexts();
    std::array<cryptoHash, 2> hashes{};
    const std::uint8_t empty_first = 0;
    const std::uint8_t empty_second = 0;
    const auto* first_data = first.empty() ? &empty_first : first.data();
    const auto* second_data = second.empty() ? &empty_second : second.data();

    crypto_cn_slow_hash_double(
        contexts[0].data(),
        first_data,
        first.size(),
        &hashes[0],
        contexts[1].data(),
        second_data,
        second.size(),
        &hashes[1]);

    SlowHashPair result{};
    for (std::size_t hash_index = 0; hash_index < result.size(); ++hash_index) {
        std::copy(
            std::begin(hashes[hash_index].data),
            std::end(hashes[hash_index].data),
            result[hash_index].begin());
    }
    return result;
#else
    return {bytecoin_slow_hash(first), bytecoin_slow_hash(second)};
#endif
}

bool bytecoin_slow_hash_multi_hash_supported() {
#if defined(BCNMINER_USE_BYTECOIN_X86_HASH)
    return crypto_cn_slow_hash_double_supported() != 0;
#else
    return false;
#endif
}

}
