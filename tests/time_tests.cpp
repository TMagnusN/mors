// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "chess/position.hpp"
#include "search/time.hpp"

#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL time management: " << message << '\n';
    return condition;
}

} // namespace

bool run_time_tests() {
    auto white = mros::Position::from_fen(mros::START_FEN);
    auto black = mros::Position::from_fen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1"
    );
    if (!expect(white.has_value() && black.has_value(),
                "time-control positions must parse")) {
        return false;
    }

    mros::timeman::TimeManager manager;
    mros::SearchLimits limits;
    mros::timeman::GoParams fixed;
    fixed.movetime = 250;
    if (!expect(manager.build_limits(*white, fixed, limits),
                "movetime must build limits")
        || !expect(limits.soft_time.count() == 250
                       && limits.hard_time.count() == 250,
                   "movetime must use identical soft and hard budgets")) {
        return false;
    }

    mros::timeman::GoParams clock;
    clock.wtime = 60'000;
    clock.btime = 10'000;
    clock.winc = 1'000;
    clock.binc = 100;
    if (!expect(manager.build_limits(*white, clock, limits),
                "white clock must build limits")
        || !expect(limits.soft_time.count() > 0,
                   "clock mode must produce a soft budget")
        || !expect(limits.hard_time >= limits.soft_time,
                   "hard budget must not be below soft budget")
        || !expect(limits.hard_time.count() < clock.wtime,
                   "white must retain clock reserve")) {
        return false;
    }

    mros::SearchLimits black_limits;
    if (!expect(manager.build_limits(*black, clock, black_limits),
                "black clock must build limits")
        || !expect(black_limits.hard_time.count() < clock.btime,
                   "side-to-move must select the black clock")) {
        return false;
    }

    mros::timeman::GoParams zero_clock;
    if (!expect(manager.build_limits(*white, zero_clock, limits),
                "zero clock must still return a move budget")
        || !expect(limits.soft_time.count() == 1
                       && limits.hard_time.count() == 1,
                   "zero clock fallback must be one millisecond")) {
        return false;
    }

    manager.set_move_overhead_ms(35);
    if (!expect(manager.move_overhead_ms() == 35,
                "Move Overhead option must update the manager")) {
        return false;
    }

    std::cout << "PASS time management\n";
    return true;
}
