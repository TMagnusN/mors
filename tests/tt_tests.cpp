// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search/tt.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace mors;

bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL transposition table: " << message << '\n';
    return condition;
}

void write(
    TranspositionTable& table,
    Key key,
    const TTData& data,
    bool force = false
) {
    TTProbe probe = table.probe(key);
    probe.writer.write(data, force);
}

static_assert(MAX_TT_SIZE_MB == 2'147'483'647ULL);
static_assert(MAX_TT_SIZE_MB * 1024ULL * 1024ULL == (1ULL << 51) - (1ULL << 20));

bool test_size_limit() {
    TranspositionTable table(1);
    const auto original_bytes = table.size_bytes();
    try {
        table.resize(MAX_TT_SIZE_MB + 1);
        return expect(false, "sizes beyond the signed 32-bit UCI limit must be rejected");
    } catch (const std::length_error&) {
        return expect(table.size_bytes() == original_bytes,
                      "rejected resize must preserve the existing table");
    }
}

bool test_round_trip() {
    TranspositionTable table(1);
    constexpr Key key = 0x1234'5678'9ABC'1357ULL;
    const Move move = Move::normal(E2, E4);
    const TTData expected{
        .move = move,
        .value = 123,
        .static_eval = -45,
        .depth = 12,
        .bound = BOUND_LOWER,
        .pv = true
    };

    TTProbe miss = table.probe(key);
    if (!expect(!miss.hit && miss.writer.valid(), "initial probe must miss"))
        return false;
    miss.writer.write(expected);

    table.prefetch(key);
    const TTProbe hit = table.probe(key);
    return expect(hit.hit, "stored key must hit")
        && expect(hit.data.move == expected.move, "move round-trip")
        && expect(hit.data.value == expected.value, "value round-trip")
        && expect(hit.data.static_eval == expected.static_eval, "evaluation round-trip")
        && expect(hit.data.depth == expected.depth, "depth round-trip")
        && expect(hit.data.bound == expected.bound, "bound round-trip")
        && expect(hit.data.pv == expected.pv, "PV flag round-trip")
        && expect(table.size_bytes() == 1024U * 1024U, "one-megabyte allocation")
        && expect(table.cluster_count() == 32'768, "32-byte cluster count");
}

bool test_zero_signature_and_empty_entries() {
    TranspositionTable table(1);
    constexpr Key zero_signature = 0x1357'9BDF'2468'0000ULL;
    constexpr Key busy_signature = 0x2468'ACE0'1357'FFFFULL;

    if (!expect(!table.probe(zero_signature).hit, "empty zero signature must miss"))
        return false;

    write(table, zero_signature, {
        .move = {},
        .value = VALUE_NONE,
        .static_eval = 77,
        .depth = DEPTH_UNSEARCHED,
        .bound = BOUND_NONE,
        .pv = false
    });

    const TTProbe hit = table.probe(zero_signature);
    if (!expect(hit.hit, "occupied zero signature must hit")
        || !expect(hit.data.depth == DEPTH_UNSEARCHED, "unsearched depth encoding")
        || !expect(hit.data.bound == BOUND_NONE, "bound-none entry remains occupied")
        || !expect(hit.data.static_eval == 77, "evaluation-only entry round-trip")) {
        return false;
    }

    write(table, busy_signature, {
        .move = Move::normal(H2, H4),
        .value = 88,
        .static_eval = -88,
        .depth = 8,
        .bound = BOUND_EXACT,
        .pv = true
    });
    const TTProbe busy_hit = table.probe(busy_signature);
    return expect(busy_hit.hit, "reserved busy signature must be remapped and hit")
        && expect(busy_hit.data.move == Move::normal(H2, H4),
                  "remapped busy signature move round-trip")
        && expect(busy_hit.data.value == 88,
                  "remapped busy signature payload round-trip");
}

bool test_write_policy_and_move_preservation() {
    TranspositionTable table(1);
    constexpr Key key = 0xCAFE'BEEF'1234'0081ULL;
    const Move first_move = Move::normal(E2, E4);
    const Move replacement_move = Move::normal(D2, D4);

    write(table, key, {
        .move = first_move,
        .value = 100,
        .static_eval = 20,
        .depth = 20,
        .bound = BOUND_UPPER,
        .pv = false
    });

    write(table, key, {
        .move = replacement_move,
        .value = -100,
        .static_eval = -20,
        .depth = 10,
        .bound = BOUND_LOWER,
        .pv = false
    });

    TTProbe protected_entry = table.probe(key);
    if (!expect(protected_entry.hit, "protected entry must remain present")
        || !expect(protected_entry.data.value == 100, "shallow data must not overwrite deep data")
        || !expect(protected_entry.data.move == replacement_move, "new TT move updates independently")) {
        return false;
    }

    protected_entry.writer.write({
        .move = {},
        .value = 250,
        .static_eval = 30,
        .depth = 1,
        .bound = BOUND_EXACT,
        .pv = true
    });

    const TTProbe exact = table.probe(key);
    return expect(exact.hit && exact.data.value == 250, "exact data must overwrite")
        && expect(exact.data.depth == 1, "exact depth must overwrite")
        && expect(exact.data.move == replacement_move, "empty write preserves old move")
        && expect(exact.data.pv, "exact PV flag must overwrite");
}

bool test_cluster_replacement() {
    TranspositionTable table(1);
    constexpr Key base = 0x2468'ACE0'1357'0000ULL;
    constexpr Key key0 = base | 0x0001ULL;
    constexpr Key key1 = base | 0x0002ULL;
    constexpr Key key2 = base | 0x0003ULL;
    constexpr Key key3 = base | 0x0004ULL;

    const auto store_depth = [&table](Key key, Depth depth) {
        write(table, key, {
            .move = Move::normal(B1, C3),
            .value = depth,
            .static_eval = 0,
            .depth = depth,
            .bound = BOUND_LOWER,
            .pv = false
        });
    };

    store_depth(key0, 2);
    store_depth(key1, 8);
    store_depth(key2, 12);
    store_depth(key3, 20);

    return expect(!table.probe(key0).hit, "shallowest entry must be replaced")
        && expect(table.probe(key1).hit, "deeper entry one must survive")
        && expect(table.probe(key2).hit, "deeper entry two must survive")
        && expect(table.probe(key3).hit, "replacement entry must hit");
}

bool test_generation_aging() {
    TranspositionTable table(1);
    constexpr Key key = 0xAAAA'5555'3333'00F0ULL;

    write(table, key, {
        .move = Move::normal(G1, F3),
        .value = 300,
        .static_eval = 10,
        .depth = 30,
        .bound = BOUND_LOWER,
        .pv = false
    });

    table.new_search();
    TTProbe stale = table.probe(key);
    stale.writer.write({
        .move = {},
        .value = 5,
        .static_eval = 1,
        .depth = 1,
        .bound = BOUND_UPPER,
        .pv = false
    });

    if (!expect(table.probe(key).data.depth == 1, "stale entry may be overwritten by shallow data"))
        return false;

    table.clear();
    for (int iteration = 0; iteration < 32; ++iteration)
        table.new_search();

    return expect(table.generation() == 0, "five-bit generation wraps after 32 searches");
}

bool test_hashfull_and_clear() {
    TranspositionTable table(1);

    // A one-megabyte table has 2^15 clusters. With the low 16 key bits
    // reserved for signatures, placing the cluster number at bit 49 targets
    // that exact cluster through the 48-bit range reduction.
    for (std::uint64_t cluster = 0; cluster < 1000; ++cluster) {
        for (std::uint64_t signature = 1; signature <= 3; ++signature) {
            const Key key = (cluster << 49) | signature;
            write(table, key, {
                .move = Move::normal(A2, A3),
                .value = 0,
                .static_eval = 0,
                .depth = 1,
                .bound = BOUND_UPPER,
                .pv = false
            });
        }
    }

    if (!expect(table.hashfull() == 1000, "sampled clusters must report full"))
        return false;

    table.new_search();
    if (!expect(table.hashfull() == 0, "old generations do not count as current hashfull"))
        return false;

    table.clear();
    return expect(table.generation() == 0, "clear resets generation")
        && expect(table.hashfull() == 0, "clear empties the table");
}

bool test_concurrent_publication() {
    TranspositionTable table(1);
    constexpr Key key = 0xBADC'0FFE'E000'4321ULL;
    const std::array<TTData, 4> patterns{{
        {
            .move = Move::normal(A2, A4),
            .value = 101,
            .static_eval = -101,
            .depth = 7,
            .bound = BOUND_UPPER,
            .pv = false
        },
        {
            .move = Move::normal(B2, B4),
            .value = 202,
            .static_eval = -202,
            .depth = 19,
            .bound = BOUND_LOWER,
            .pv = true
        },
        {
            .move = Move::normal(C2, C4),
            .value = 303,
            .static_eval = -303,
            .depth = 31,
            .bound = BOUND_EXACT,
            .pv = false
        },
        {
            .move = Move::normal(D2, D4),
            .value = 404,
            .static_eval = -404,
            .depth = 43,
            .bound = BOUND_NONE,
            .pv = true
        }
    }};
    const auto is_complete_pattern = [&patterns](const TTData& data) {
        for (const TTData& pattern : patterns) {
            if (data.move == pattern.move
                && data.value == pattern.value
                && data.static_eval == pattern.static_eval
                && data.depth == pattern.depth
                && data.bound == pattern.bound
                && data.pv == pattern.pv) {
                return true;
            }
        }
        return false;
    };

    write(table, key, patterns[0], true);

    constexpr std::size_t THREAD_COUNT = 8;
    constexpr int ITERATIONS = 20'000;
    std::atomic_bool start{false};
    std::atomic_bool failed{false};
    std::vector<std::thread> workers;
    workers.reserve(THREAD_COUNT);
    for (std::size_t id = 0; id < THREAD_COUNT; ++id) {
        workers.emplace_back([&, id] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (int iteration = 0;
                 iteration < ITERATIONS
                    && !failed.load(std::memory_order_relaxed);
                 ++iteration) {
                const TTProbe probe = table.probe(key);
                if (probe.hit && !is_complete_pattern(probe.data)) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }

                if ((iteration & 1) == 0) {
                    const std::size_t pattern = (
                        id + static_cast<std::size_t>(iteration)
                    ) % patterns.size();
                    probe.writer.write(patterns[pattern], true);
                } else if ((iteration & 255) == 1) {
                    (void) table.hashfull();
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers)
        worker.join();

    const TTProbe final = table.probe(key);
    return expect(!failed.load(std::memory_order_relaxed),
                  "concurrent readers must observe complete atomic payloads")
        && expect(final.hit && is_complete_pattern(final.data),
                  "concurrent publication must leave a valid entry");
}

} // namespace

bool run_tt_tests() {
    const bool passed = test_size_limit()
                     && test_round_trip()
                     && test_zero_signature_and_empty_entries()
                     && test_write_policy_and_move_preservation()
                     && test_cluster_replacement()
                     && test_generation_aging()
                     && test_hashfull_and_clear()
                     && test_concurrent_publication();

    if (passed)
        std::cout << "PASS transposition table\n";
    return passed;
}
