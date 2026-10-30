#include "bcnminer/pow_worker.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stop_token>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}

int main() {
    bcnminer::SlowHash zero{};
    bcnminer::SlowHash maximum{};
    maximum.fill(0xff);
    require(bcnminer::hash_meets_target(zero, zero),
            "hash equal to target should qualify");
    require(bcnminer::hash_meets_target(zero, maximum),
            "hash below target should qualify");
    require(!bcnminer::hash_meets_target(maximum, zero),
            "hash above target should not qualify");
    require(bcnminer::hash_meets_difficulty(maximum, 1),
            "maximum hash should qualify at difficulty one");
    require(!bcnminer::hash_meets_difficulty(maximum, 2),
            "maximum hash should fail above difficulty one");
    require(!bcnminer::hash_meets_difficulty(zero, 0),
            "zero difficulty should be rejected");

    bcnminer::SlowHash boundary_hash{};
    boundary_hash.back() = 0x7f;
    require(bcnminer::hash_meets_difficulty(boundary_hash, 2),
            "hash just below the difficulty-two boundary should qualify");
    boundary_hash.back() = 0x80;
    require(!bcnminer::hash_meets_difficulty(boundary_hash, 2),
            "hash at the difficulty-two boundary should fail");

    boundary_hash.fill(0);
    boundary_hash[24] = 1;
    boundary_hash[16] = 1;
    boundary_hash[8] = 1;
    boundary_hash[0] = 1;
    require(bcnminer::hash_meets_difficulty(boundary_hash, UINT64_MAX),
            "maximum difficulty boundary hash should qualify");
    boundary_hash[0] = 2;
    require(!bcnminer::hash_meets_difficulty(boundary_hash, UINT64_MAX),
            "hash above the maximum difficulty boundary should fail");

    const std::array<std::uint8_t, 8> original_blob{
        0x10, 0x20, 0, 0, 0, 0, 0x30, 0x40};
    const auto solution = bcnminer::scan_nonce_range(
        original_blob, 2, 0x12345678, 1, maximum);
    require(solution.has_value(), "maximum target should accept first nonce");
    require(solution->nonce == 0x12345678,
            "scanner should return the nonce it wrote");

    auto expected_blob = original_blob;
    expected_blob[2] = 0x78;
    expected_blob[3] = 0x56;
    expected_blob[4] = 0x34;
    expected_blob[5] = 0x12;
    require(solution->hash == bcnminer::bytecoin_slow_hash(expected_blob),
            "scanner should hash the blob with the nonce encoded little-endian");
    std::atomic<std::uint64_t> counted_hashes{0};
    (void)bcnminer::scan_nonce_range(
        original_blob, 2, 0, 3, zero, {}, &counted_hashes);
    require(counted_hashes.load() == 3,
            "scanner should report each completed hash to the stats counter");
    std::atomic<std::uint64_t> paired_hashes{0};
    (void)bcnminer::scan_nonce_range(
        original_blob, 2, 0, 3, zero, {}, &paired_hashes, true);
    require(paired_hashes.load() == 3,
            "two-way scanner should count every completed nonce hash");

    auto second_nonce_blob = original_blob;
    second_nonce_blob[2] = 0x79;
    second_nonce_blob[3] = 0x56;
    second_nonce_blob[4] = 0x34;
    second_nonce_blob[5] = 0x12;
    const auto second_nonce_hash =
        bcnminer::bytecoin_slow_hash(second_nonce_blob);
    const auto expected_pair_nonce =
        bcnminer::hash_meets_target(solution->hash, second_nonce_hash)
        ? std::uint32_t{0x12345678}
        : std::uint32_t{0x12345679};
    const auto pair_solution = bcnminer::scan_nonce_range(
        original_blob,
        2,
        0x12345678,
        2,
        second_nonce_hash,
        {},
        nullptr,
        true);
    require(pair_solution.has_value() &&
                pair_solution->nonce == expected_pair_nonce,
            "two-way scanner should return the earliest qualifying nonce");

    std::atomic<std::uint64_t> live_hashes{0};
    std::jthread scanner([&](const std::stop_token stop) {
        (void)bcnminer::scan_nonce_range(
            original_blob,
            2,
            0,
            std::uint64_t{1} << 32,
            zero,
            stop,
            &live_hashes);
    });
    const auto counter_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (live_hashes.load(std::memory_order_relaxed) < 32 &&
           std::chrono::steady_clock::now() < counter_deadline) {
        std::this_thread::yield();
    }
    scanner.request_stop();
    scanner.join();
    require(live_hashes.load(std::memory_order_relaxed) >= 32,
            "scanner should publish hash progress while a nonce range is active");

    bool rejected_bad_nonce_offset = false;
    try {
        (void)bcnminer::scan_nonce_range(original_blob, 6, 0, 1, maximum);
    } catch (const std::invalid_argument&) {
        rejected_bad_nonce_offset = true;
    }
    require(rejected_bad_nonce_offset,
            "scanner should reject nonce fields outside the blob");

    std::stop_source stop_source;
    stop_source.request_stop();
    require(!bcnminer::scan_nonce_range(
                 original_blob, 2, 0, 1, maximum, stop_source.get_token())
                 .has_value(),
            "scanner should honor a requested stop before hashing");

    std::cout << "All proof-of-work worker tests passed.\n";
    return EXIT_SUCCESS;
}
