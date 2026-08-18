// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search/score.hpp"

#include <cstdint>
#include <iostream>

namespace {

using namespace mors;

static_assert(MAX_PLY == 240);
static_assert(VALUE_DRAW == 0);
static_assert(VALUE_EVAL_MAX < VALUE_TB_WIN_IN_MAX_PLY);
static_assert(VALUE_TB_WIN_IN_MAX_PLY <= VALUE_TB);
static_assert(VALUE_TB < VALUE_MATE_IN_MAX_PLY);
static_assert(VALUE_MATE_IN_MAX_PLY <= VALUE_MATE);
static_assert(VALUE_MATE < VALUE_INFINITE);
static_assert(VALUE_INFINITE < VALUE_NONE);

static_assert(is_valid_value(VALUE_DRAW));
static_assert(is_valid_value(VALUE_MATE));
static_assert(!is_valid_value(VALUE_INFINITE));
static_assert(!is_valid_value(VALUE_NONE));
static_assert(!is_win(VALUE_INFINITE));
static_assert(!is_win(VALUE_NONE));
static_assert(!is_loss(-VALUE_INFINITE));
static_assert(!is_decisive(VALUE_NONE));
static_assert(!is_mate_value(VALUE_NONE));
static_assert(is_eval_value(VALUE_EVAL_MAX));
static_assert(!is_eval_value(VALUE_TB_WIN_IN_MAX_PLY));
static_assert(value_to_tt(mate_in(MAX_PLY), MAX_PLY) == VALUE_MATE);
static_assert(value_to_tt(mated_in(MAX_PLY), MAX_PLY) == -VALUE_MATE);

bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL score: " << message << '\n';
    return condition;
}

bool test_bands_and_clamp() {
    return expect(is_decisive(VALUE_TB_WIN_IN_MAX_PLY), "positive decisive boundary")
        && expect(is_decisive(-VALUE_TB_WIN_IN_MAX_PLY), "negative decisive boundary")
        && expect(!is_decisive(VALUE_EVAL_MAX), "ordinary value is not decisive")
        && expect(is_mate_value(VALUE_MATE_IN_MAX_PLY), "positive mate boundary")
        && expect(is_mate_value(-VALUE_MATE_IN_MAX_PLY), "negative mate boundary")
        && expect(!is_mate_value(VALUE_TB), "tablebase value is not mate")
        && expect(clamp_eval(INT64_MAX) == VALUE_EVAL_MAX, "positive eval clamp")
        && expect(clamp_eval(INT64_MIN) == -VALUE_EVAL_MAX, "negative eval clamp")
        && expect(clamp_eval(123) == 123, "in-range eval is unchanged");
}

bool test_mate_helpers() {
    return expect(mate_in(0) == VALUE_MATE, "mate at root")
        && expect(mate_in(MAX_PLY) == VALUE_MATE_IN_MAX_PLY, "mate at maximum ply")
        && expect(mated_in(0) == -VALUE_MATE, "mated at root")
        && expect(mated_in(MAX_PLY) == -VALUE_MATE_IN_MAX_PLY, "mated at maximum ply");
}

bool test_tt_distance_round_trip() {
    constexpr int STORE_PLY = 5;
    constexpr int PROBE_PLY = 9;

    const Value win = mate_in(12);
    const Value loss = mated_in(12);
    const Value stored_win = value_to_tt(win, STORE_PLY);
    const Value stored_loss = value_to_tt(loss, STORE_PLY);

    return expect(stored_win == mate_in(7), "positive mate becomes position-relative")
        && expect(stored_loss == mated_in(7), "negative mate becomes position-relative")
        && expect(value_from_tt(stored_win, STORE_PLY, 0) == win, "positive mate same-ply round-trip")
        && expect(value_from_tt(stored_loss, STORE_PLY, 0) == loss, "negative mate same-ply round-trip")
        && expect(value_from_tt(stored_win, PROBE_PLY, 0) == mate_in(16), "positive mate shifts across roots")
        && expect(value_from_tt(stored_loss, PROBE_PLY, 0) == mated_in(16), "negative mate shifts across roots")
        && expect(value_to_tt(321, STORE_PLY) == 321, "ordinary store is unchanged")
        && expect(value_from_tt(-654, PROBE_PLY, 99) == -654, "ordinary probe is unchanged")
        && expect(value_from_tt(VALUE_NONE, PROBE_PLY, 99) == VALUE_NONE, "none sentinel survives probe");
}

bool test_rule50_demotion() {
    constexpr Value MATE_IN_8 = VALUE_MATE - 8;
    constexpr Value MATED_IN_8 = -VALUE_MATE + 8;
    constexpr Value TB_IN_6 = VALUE_TB - 6;
    constexpr Value TB_LOSS_IN_6 = -VALUE_TB + 6;

    return expect(value_from_tt(MATE_IN_8, 0, 92) == MATE_IN_8,
                  "mate fitting the rule-50 horizon survives")
        && expect(value_from_tt(MATE_IN_8, 0, 93) == VALUE_EVAL_MAX,
                  "overlong mate is demoted")
        && expect(value_from_tt(MATED_IN_8, 0, 93) == -VALUE_EVAL_MAX,
                  "overlong mated score is demoted")
        && expect(value_from_tt(TB_IN_6, 0, 94) == TB_IN_6,
                  "TB win fitting the rule-50 horizon survives")
        && expect(value_from_tt(TB_IN_6, 0, 95) == VALUE_EVAL_MAX,
                  "overlong TB win is demoted")
        && expect(value_from_tt(TB_LOSS_IN_6, 0, 95) == -VALUE_EVAL_MAX,
                  "overlong TB loss is demoted");
}

} // namespace

bool run_score_tests() {
    const bool passed = test_bands_and_clamp()
                     && test_mate_helpers()
                     && test_tt_distance_round_trip()
                     && test_rule50_demotion();

    if (passed)
        std::cout << "PASS search score\n";
    return passed;
}
