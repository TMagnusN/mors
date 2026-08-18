// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search/tt.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

class SplitMix64 final {
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        std::uint64_t value = (state_ += 0x9E37'79B9'7F4A'7C15ULL);
        value = (value ^ (value >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return value ^ (value >> 31);
    }

private:
    std::uint64_t state_;
};

void run_probe_benchmark(
    std::string_view label,
    mors::TranspositionTable& table,
    const std::vector<mors::Key>& keys,
    std::size_t iterations
) {
    std::uint64_t checksum = 0;
    std::size_t hits = 0;
    const std::size_t mask = keys.size() - 1;

    const auto begin = Clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        const mors::TTProbe probe = table.probe(keys[iteration & mask]);
        hits += probe.hit;
        checksum += static_cast<std::uint64_t>(probe.data.move.raw())
                  + static_cast<std::uint64_t>(probe.data.depth + 3);
    }
    const auto end = Clock::now();

    const double seconds = std::chrono::duration<double>(end - begin).count();
    const double probes_per_second = static_cast<double>(iterations) / seconds;
    const double nanoseconds = seconds * 1.0e9 / static_cast<double>(iterations);

    std::cout << std::left << std::setw(6) << label
              << " probes " << iterations
              << " hits " << hits
              << " ns/probe " << std::fixed << std::setprecision(2) << nanoseconds
              << " Mprobe/s " << std::setprecision(2) << probes_per_second / 1.0e6
              << " checksum " << checksum << '\n';
}

} // namespace

int main() {
    constexpr std::size_t KEY_COUNT = 1U << 18;
    constexpr std::size_t ITERATIONS = 1U << 24;

    mors::TranspositionTable table(16);
    std::vector<mors::Key> hit_keys(KEY_COUNT);
    std::vector<mors::Key> miss_keys(KEY_COUNT);
    SplitMix64 random(114514);

    for (std::size_t index = 0; index < KEY_COUNT; ++index) {
        hit_keys[index] = random.next();
        const mors::TTProbe probe = table.probe(hit_keys[index]);
        probe.writer.write({
            .move = mors::Move::normal(mors::B1, mors::C3),
            .value = static_cast<mors::Value>(index & 1023U),
            .static_eval = static_cast<mors::Value>(index & 511U),
            .depth = static_cast<mors::Depth>(1 + (index & 63U)),
            .bound = mors::BOUND_LOWER,
            .pv = (index & 7U) == 0
        });
        miss_keys[index] = random.next();
    }

    std::cout << "TT 16 MiB, 32-byte clusters, 3 entries/cluster\n";
    run_probe_benchmark("hit", table, hit_keys, ITERATIONS);
    run_probe_benchmark("miss", table, miss_keys, ITERATIONS);
    return 0;
}
