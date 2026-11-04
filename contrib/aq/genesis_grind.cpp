// Copyright (c) 2026 The AQCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Multi-threaded nonce search for a single 80-byte block header under the AQ
// proof-of-work rule. Used by contrib/aq/genesis.py to mine genesis blocks.
//
// Usage: genesis_grind <80-byte header hex> <threshold> [threads]
// Prints the winning nonce in decimal, or "none" if the 2^32 nonce space is exhausted.

#include "../aqminer/aq_hash.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <header80hex> <threshold> [threads]\n", argv[0]);
        return 1;
    }
    const std::string hex{argv[1]};
    if (hex.size() != 160) {
        std::fprintf(stderr, "header must be 80 bytes\n");
        return 1;
    }
    aqminer::Header80 header;
    for (size_t i = 0; i < 80; ++i) header[i] = std::stoul(hex.substr(2 * i, 2), nullptr, 16);
    const uint32_t threshold = std::strtoul(argv[2], nullptr, 10);
    unsigned threads = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : std::thread::hardware_concurrency();
    if (threads == 0) threads = 1;
    std::fprintf(stderr, "SHA256 implementation: %s\n", SHA256AutoDetect().c_str());

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> hashes{0};
    std::mutex mu;
    std::optional<uint32_t> found;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            auto r = aqminer::Grind(header, threshold, t, uint64_t{1} << 32, threads, stop, hashes);
            if (r) {
                std::lock_guard<std::mutex> lock(mu);
                if (!found || *r < *found) found = r;
                stop = true;
            }
        });
    }
    for (auto& w : workers) w.join();
    std::fprintf(stderr, "hashes: %llu\n", (unsigned long long)hashes.load());
    if (found) {
        std::printf("%u\n", *found);
    } else {
        std::printf("none\n");
    }
    return 0;
}
