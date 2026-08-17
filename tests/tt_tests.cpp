// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search/tt.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace mros;

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
    return expect(hit.hit, "occupied zero signature must hit")
        && expect(hit.data.depth == DEPTH_UNSEARCHED, "unsearched depth encoding")
        && expect(hit.data.bound == BOUND_NONE, "bound-none entry remains occupied")
        && expect(hit.data.static_eval == 77, "evaluation-only entry round-trip");
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

} // namespace

bool run_tt_tests() {
    const bool passed = test_round_trip()
                     && test_zero_signature_and_empty_entries()
                     && test_write_policy_and_move_preservation()
                     && test_cluster_replacement()
                     && test_generation_aging()
                     && test_hashfull_and_clear();

    if (passed)
        std::cout << "PASS transposition table\n";
    return passed;
}
