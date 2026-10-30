#include "bcnminer/slow_hash.hpp"
#include "hash.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iostream>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<std::uint8_t> decode_hex(const std::string& text) {
    require(text.size() % 2 == 0, "test vector hex input must have even length");
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2);

    for (std::size_t i = 0; i < text.size(); i += 2) {
        const auto value = std::stoul(text.substr(i, 2), nullptr, 16);
        bytes.push_back(static_cast<std::uint8_t>(value));
    }

    return bytes;
}

std::string encode_hex(const bcnminer::SlowHash& hash) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(hash.size() * 2);

    for (const auto byte : hash) {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 0x0f]);
    }

    return text;
}

}

int main(int argc, char** argv) {
    require(argc == 2, "expected path to Bytecoin slow-hash test vectors");

    std::ifstream vectors(argv[1]);
    require(vectors.is_open(), "could not open Bytecoin slow-hash test vectors");

    std::size_t count = 0;
    std::vector<std::uint8_t> first_pair_input;
    std::vector<std::uint8_t> second_pair_input;
    std::string line;
    while (std::getline(vectors, line)) {
        if (line.empty()) {
            continue;
        }

        std::istringstream parser(line);
        std::string expected;
        std::string input_hex;
        require(static_cast<bool>(parser >> expected >> input_hex),
                "malformed Bytecoin slow-hash test vector");
        const auto input = decode_hex(input_hex);
        const auto actual = bcnminer::bytecoin_slow_hash(input);
        require(encode_hex(actual) == expected,
                "Bytecoin CryptoNight vector " + std::to_string(count + 1) +
                    " did not match");

        if (count == 0) {
            first_pair_input = input;
        } else if (count == 1) {
            second_pair_input = input;
        }

        if (count == 0) {
            std::vector<std::max_align_t> scratch(
                (SLOW_HASH_CONTEXT_SIZE + sizeof(std::max_align_t) - 1) /
                sizeof(std::max_align_t));
            cryptoHash portable_hash{};
            crypto_cn_slow_hash_platform_independent(
                scratch.data(), input.data(), input.size(), &portable_hash);
            require(std::equal(
                        std::begin(portable_hash.data),
                        std::end(portable_hash.data),
                        actual.begin()),
                    "portable Bytecoin hash did not match the first upstream vector");
        }

        ++count;
    }

    require(count == 4, "expected all four upstream Bytecoin slow-hash vectors");
    const auto pair_hashes = bcnminer::bytecoin_slow_hash_pair(
        first_pair_input, second_pair_input);
    require(
        pair_hashes[0] == bcnminer::bytecoin_slow_hash(first_pair_input),
        "first interleaved Bytecoin hash did not match the single-hash path");
    require(
        pair_hashes[1] == bcnminer::bytecoin_slow_hash(second_pair_input),
        "second interleaved Bytecoin hash did not match the single-hash path");
    std::cout << "All " << count
              << " Bytecoin CryptoNight vectors and two-hash API checks passed.\n";
    return EXIT_SUCCESS;
}
