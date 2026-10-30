#include "bcnminer/pool_protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}

int main() {
    const auto message = bcnminer::parse_json(
        R"({"id":1,"result":{"id":"worker-1","job":{"job_id":"job-a","blob":"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233343536373839","target":"ffffffff"}}})");
    const auto parsed = bcnminer::parse_pool_message(message);
    require(parsed.request_id == 1, "login response request id should be parsed");
    require(parsed.session_id == "worker-1",
            "pool session id should be parsed for submissions");
    require(parsed.job.has_value(), "login response should contain a mining job");
    require(parsed.job->job_id == "job-a", "job identifier should be parsed");
    require(parsed.job->nonce_offset == 39,
            "CryptoNight jobs should use the configured default nonce offset");
    require(parsed.job->hashing_blob.size() == 57,
            "job blob should be decoded from hexadecimal");
    require(parsed.job->target == UINT64_MAX,
            "maximum compact target should expand to maximum 64-bit target");

    bcnminer::SlowHash hash{};
    require(bcnminer::hash_meets_pool_target(hash, parsed.job->target),
            "zero hash should meet maximum pool target");
    for (std::size_t i = 24; i < hash.size(); ++i) {
        hash[i] = 0xff;
    }
    require(!bcnminer::hash_meets_pool_target(hash, parsed.job->target - 1),
            "hash above the pool target should be rejected");

    const auto notification = bcnminer::parse_pool_message(
        bcnminer::parse_json(
            R"({"jsonrpc":"2.0","method":"job","params":{"job_id":"job-b","blob":"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233343536373839","target":"01000000"}})"));
    require(notification.job.has_value() &&
                notification.job->job_id == "job-b",
            "job notifications should be parsed");
    require(notification.job->target == UINT64_MAX / UINT32_MAX,
            "compact target should use Bytecoin-compatible little-endian expansion");

    bool rejected_bad_target = false;
    try {
        (void)bcnminer::parse_pool_job(bcnminer::parse_json(
            R"({"job_id":"bad","blob":"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233343536373839","target":"00000000"})"));
    } catch (const std::invalid_argument&) {
        rejected_bad_target = true;
    }
    require(rejected_bad_target, "zero compact target must be rejected");

    bool rejected_bad_blob = false;
    try {
        (void)bcnminer::decode_hex("123z");
    } catch (const std::invalid_argument&) {
        rejected_bad_blob = true;
    }
    require(rejected_bad_blob, "invalid hexadecimal input must be rejected");

    std::cout << "All pool protocol tests passed.\n";
    return EXIT_SUCCESS;
}
