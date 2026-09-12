// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "chess/movegen.hpp"
#include "eval/nnue/network.hpp"
#include "eval/nnue/output.hpp"
#include "eval/nnue/wdl.hpp"
#include "eval/nnue/worker.hpp"
#include "search/score.hpp"

#include <array>
#include <bit>
#include <limits>
#include <vector>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

using namespace mors;

[[nodiscard]] std::filesystem::path network_path() {
    constexpr std::array<std::string_view, 3> CANDIDATES{
        "../networks/mors-p2h32-s14400M-o3183M-c+frc.mnue",
        "networks/mors-p2h32-s14400M-o3183M-c+frc.mnue",
        "../../networks/mors-p2h32-s14400M-o3183M-c+frc.mnue"
    };
    for (const std::string_view candidate : CANDIDATES) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error)
            return std::filesystem::path(candidate);
    }
    return {};
}

bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL nnue: " << message << '\n';
    return condition;
}

struct Golden final {
    std::string_view fen;
    Value expected;
};

bool test_output_kernels() {
#if defined(__AVX2__)
    using nnue::detail::Accumulator;
    Accumulator first{}, second{}, first_weights{}, second_weights{};
    const auto check = [&](bool narrow) {
        const auto expected = nnue::detail::dot_pair_scalar(
            first, first_weights.data(), second, second_weights.data());
        return expect(nnue::detail::dot_pair_avx2_wide(
                          first, first_weights.data(), second, second_weights.data()) == expected,
                      "wide SIMD raw sum must equal scalar before scaling/clamping")
            && (!narrow || expect(nnue::detail::dot_pair_avx2_narrow(
                          first, first_weights.data(), second, second_weights.data()) == expected,
                      "narrow SIMD raw sum must equal scalar before scaling/clamping"));
    };

    // Maximal lanes and totals exceeding INT32_MAX, including negative totals.
    first.fill(32'767);
    second.fill(255);
    for (const std::int16_t weight : {std::int16_t{128}, std::int16_t{-128}}) {
        first_weights.fill(weight);
        second_weights.fill(weight);
        if (!check(true)) return false;
    }
    // Large cancellation must remain exact before the final evaluation clamp.
    first_weights.fill(128);
    second_weights.fill(-128);
    second_weights.back() = -127;
    if (!check(true)) return false;

    std::uint64_t state = 0xD4E12C77AB890531ULL;
    const auto random = [&]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    constexpr std::array<std::int16_t, 9> EDGES{
        -32'768, -1, 0, 1, 127, 254, 255, 256, 32'767
    };
    for (std::size_t sample = 0; sample < 2'000; ++sample) {
        const bool narrow = sample < 1'000;
        for (std::size_t i = 0; i < first.size(); ++i) {
            first[i] = sample % 2 == 0 ? EDGES[random() % EDGES.size()]
                : std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(random()));
            second[i] = sample % 2 == 0 ? EDGES[random() % EDGES.size()]
                : std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(random()));
            first_weights[i] = narrow ? static_cast<std::int16_t>(int(random() % 257) - 128)
                : std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(random()));
            second_weights[i] = narrow ? static_cast<std::int16_t>(int(random() % 257) - 128)
                : std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(random()));
        }
        if (!check(narrow)) return false;
    }
    first.fill(255);
    second.fill(255);
    first_weights.fill(std::numeric_limits<std::int16_t>::min());
    second_weights.fill(std::numeric_limits<std::int16_t>::max());
    if (!check(false)) return false;
#endif
    return true;
}

bool test_output_weight_dispatch() {
    using Shape = nnue::P2H32;
    std::vector<std::byte> payload(Shape::PAYLOAD_BYTES);
    constexpr std::size_t OFFSET = 2 * (Shape::COARSE_WEIGHT_COUNT
        + Shape::COARSE_BIAS_COUNT + Shape::FINE_WEIGHT_COUNT + Shape::FINE_BIAS_COUNT);
    constexpr std::size_t LAST = OFFSET + 2 * (Shape::OUTPUT_WEIGHT_COUNT - 1);
    for (const int weight : {0, -128, 128, -129, 129, -32'768, 32'767}) {
        const auto bits = static_cast<std::uint16_t>(weight);
        payload[LAST] = static_cast<std::byte>(bits & 255U);
        payload[LAST + 1] = static_cast<std::byte>(bits >> 8);
        auto loaded = nnue::Network::load(payload, "output-dispatch-test");
        if (!expect(loaded.has_value(), "wide-weight external network must remain loadable")
            || !expect(loaded->has_fast_output_weights() == (weight >= -128 && weight <= 128),
                       "all output buckets must be checked for narrow SIMD eligibility"))
            return false;
    }
    return true;
}

bool test_golden(const nnue::Network& network) {
    constexpr std::array<Golden, 5> CASES{{
        {START_FEN, 63},
        {
            "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10",
            -93
        },
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", -43},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", -505},
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 350}
    }};

    nnue::Worker worker;
    bool passed = true;
    for (const Golden& test : CASES) {
        auto parsed = Position::from_fen(test.fen);
        if (!expect(parsed.has_value(), "golden FEN must parse"))
            return false;

        worker.reset();
        const Value reference = nnue::evaluate_reference(*parsed, network);
        const Value incremental = worker.evaluate(*parsed, network);
        const bool current =
               expect(is_eval_value(reference),
                      "reference output must remain an ordinary value")
            && expect(is_eval_value(incremental),
                      "worker output must remain an ordinary value")
            && expect(reference == test.expected,
                      "reference must match MagnusChessX golden")
            && expect(incremental == reference,
                      "worker must match reference at root");
        if (!current) {
            std::cerr << "  fen: " << test.fen << " expected " << test.expected
                      << " reference " << reference
                      << " worker " << incremental << '\n';
            passed = false;
        }
    }
    return passed;
}

bool test_incremental_moves(const nnue::Network& network) {
    constexpr std::array<std::string_view, 9> CASES{{
        START_FEN,
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
        "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1",
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
        "4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 1",
        "1r2k3/P7/8/8/8/8/7p/4K3 w - - 0 1",
        "4k3/7P/8/8/8/8/p7/4K3 b - - 0 1",
        "4k3/8/8/8/8/8/8/R5KR w AH - 0 1",
        "4k3/8/8/8/8/8/8/4KR2 w F - 0 1"
    }};

    bool saw_castling = false;
    bool saw_en_passant = false;
    bool saw_promotion = false;

    for (const std::string_view fen : CASES) {
        auto parsed = Position::from_fen(fen);
        if (!expect(parsed.has_value(), "incremental FEN must parse"))
            return false;

        Position position = std::move(*parsed);
        nnue::Worker worker;
        const Value root = worker.evaluate(position, network);
        if (!expect(root == nnue::evaluate_reference(position, network),
                    "root accumulator must match refresh")) {
            return false;
        }

        MoveList moves;
        generate_legal(position, moves);
        for (const Move move : moves) {
            saw_castling |= move.type() == CASTLING;
            saw_en_passant |= move.type() == EN_PASSANT;
            saw_promotion |= move.type() == PROMOTION;

            worker.push(position, move);
            StateInfo state;
            position.do_move(move, state);

            const Value incremental = worker.evaluate(position, network);
            const Value reference = nnue::evaluate_reference(position, network);
            if (!expect(incremental == reference, "incremental move must match full refresh")) {
                std::cerr << "  parent fen: " << fen << " move raw " << move.raw()
                          << " incremental " << incremental
                          << " reference " << reference << '\n';
                return false;
            }

            position.undo_move(move, state);
            worker.pop();
            if (!expect(worker.evaluate(position, network) == root,
                        "pop must restore root accumulator")) {
                return false;
            }
        }
    }

    return expect(saw_castling, "special-move suite must cover castling")
        && expect(saw_en_passant, "special-move suite must cover en-passant")
        && expect(saw_promotion, "special-move suite must cover promotion");
}

bool test_incremental_line(const nnue::Network& network) {
    constexpr std::size_t MAX_PLIES = 96;
    auto parsed = Position::from_fen(START_FEN);
    if (!expect(parsed.has_value(), "line-test FEN must parse"))
        return false;

    Position position = std::move(*parsed);
    nnue::Worker worker;
    std::array<Move, MAX_PLIES> played{};
    std::array<StateInfo, MAX_PLIES> states{};
    std::size_t ply = 0;

    while (ply < MAX_PLIES) {
        MoveList legal;
        generate_legal(position, legal);
        if (legal.empty())
            break;

        const std::size_t choice = (ply * 17 + 3) % legal.size();
        played[ply] = legal[choice];
        worker.push(position, played[ply]);
        position.do_move(played[ply], states[ply]);
        ++ply;

        if (!expect(worker.evaluate(position, network)
                        == nnue::evaluate_reference(position, network),
                    "deep incremental accumulator must match full refresh")) {
            return false;
        }
    }

    if (!expect(ply >= 32, "line test must reach at least 32 plies"))
        return false;

    while (ply > 0) {
        --ply;
        position.undo_move(played[ply], states[ply]);
        worker.pop();
        if (!expect(worker.evaluate(position, network)
                        == nnue::evaluate_reference(position, network),
                    "deep pop must restore the previous accumulator")) {
            return false;
        }
    }
    return expect(worker.size() == 1, "worker must return to its root state");
}

bool test_lazy_incremental_line(const nnue::Network& network) {
    constexpr std::size_t MAX_PLIES = 80;
    auto parsed = Position::from_fen(START_FEN);
    if (!expect(parsed.has_value(), "lazy-line FEN must parse"))
        return false;

    Position position = std::move(*parsed);
    nnue::Worker worker;
    std::array<Move, MAX_PLIES> played{};
    std::array<StateInfo, MAX_PLIES> states{};
    std::size_t ply = 0;

    // Do not evaluate the root or every child: this exercises both forward
    // lazy updates and reconstruction back from a freshly refreshed leaf.
    while (ply < MAX_PLIES) {
        MoveList legal;
        generate_legal(position, legal);
        if (legal.empty())
            break;

        const std::size_t choice = (ply * 29 + 11) % legal.size();
        played[ply] = legal[choice];
        worker.push(position, played[ply]);
        position.do_move(played[ply], states[ply]);
        ++ply;

        if (ply % 7 == 0
            && !expect(worker.evaluate(position, network)
                           == nnue::evaluate_reference(position, network),
                       "lazy forward accumulator must match full refresh")) {
            return false;
        }
    }

    if (!expect(ply >= 32, "lazy line must reach at least 32 plies")
        || !expect(worker.evaluate(position, network)
                       == nnue::evaluate_reference(position, network),
                   "lazy leaf accumulator must match full refresh")) {
        return false;
    }

    while (ply > 0) {
        --ply;
        position.undo_move(played[ply], states[ply]);
        worker.pop();
        if (ply % 5 == 0
            && !expect(worker.evaluate(position, network)
                           == nnue::evaluate_reference(position, network),
                       "lazy pop accumulator must match full refresh")) {
            return false;
        }
    }
    return expect(worker.size() == 1, "lazy worker must return to its root state");
}

bool test_mirror_cache(const nnue::Network& network) {
    constexpr std::array<std::string_view, 2> FENS{
        "r3k2r/ppp2ppp/2n1b3/3pP3/1P1P1N2/2P1B2P/P4PP1/R1Q1K2R w - - 0 1",
        "r3k2r/ppp2ppp/2n1b3/3pP3/1P1P1N2/2P1B2P/P4PP1/R1QK3R w - - 0 1"
    };
    std::array<Position, 2> positions{};
    for (std::size_t index = 0; index < positions.size(); ++index) {
        auto parsed = Position::from_fen(FENS[index]);
        if (!expect(parsed.has_value(), "mirror-cache FEN must parse"))
            return false;
        positions[index] = *parsed;
    }

    nnue::Worker worker;
    for (std::size_t iteration = 0; iteration < 32; ++iteration) {
        const Position& position = positions[iteration & 1U];
        worker.reset();
        if (!expect(worker.evaluate(position, network)
                        == nnue::evaluate_reference(position, network),
                    "D/E mirror cache entry must match full refresh")) {
            return false;
        }
    }
    return true;
}

bool test_wdl_model() {
    auto parsed = Position::from_fen(START_FEN);
    if (!expect(parsed.has_value(), "WDL start position must parse"))
        return false;

    const nnue::WdlTriplet draw = nnue::score_to_wdl(0, *parsed);
    const nnue::WdlTriplet positive = nnue::score_to_wdl(116, *parsed);
    const nnue::WdlTriplet negative = nnue::score_to_wdl(-116, *parsed);
    const nnue::WdlTriplet win = nnue::score_to_wdl(VALUE_MATE, *parsed);
    const nnue::WdlTriplet loss = nnue::score_to_wdl(-VALUE_MATE, *parsed);

    return expect(nnue::score_to_cp(116, *parsed) == 100,
                  "full-material P2-H32 cp calibration")
        && expect(draw.win + draw.draw + draw.loss == 1'000,
                  "draw WDL must sum to 1000")
        && expect(positive.win + positive.draw + positive.loss == 1'000,
                  "positive WDL must sum to 1000")
        && expect(positive.win == negative.loss
                      && positive.draw == negative.draw
                      && positive.loss == negative.win,
                  "ordinary WDL must be score-symmetric")
        && expect(win.win == 1'000 && win.draw == 0 && win.loss == 0,
                  "decisive win WDL")
        && expect(loss.win == 0 && loss.draw == 0 && loss.loss == 1'000,
                  "decisive loss WDL");
}

} // namespace

bool run_nnue_tests() {
    const std::filesystem::path path = network_path();
    if (!expect(!path.empty(), "mors-p2h32-s14400M-o3183M-c+frc.mnue must be available"))
        return false;

    auto loaded = mors::nnue::Network::load(path);
    if (!expect(loaded.has_value(), "production P2-H32 network must load")) {
        if (!loaded)
            std::cerr << "  " << loaded.error() << '\n';
        return false;
    }

    const bool base = expect(loaded->valid(), "loaded network must be valid")
                   && expect(
                            loaded->memory_bytes() == mors::nnue::P2H32::PAYLOAD_BYTES,
                            "network payload size"
                        );
    const bool kernels = base && test_output_kernels() && test_output_weight_dispatch()
        && expect(loaded->has_fast_output_weights(), "production network must select narrow SIMD");
    const bool golden = kernels && test_golden(*loaded);
    const bool incremental = golden && test_incremental_moves(*loaded);
    const bool line = incremental && test_incremental_line(*loaded);
    const bool lazy = line && test_lazy_incremental_line(*loaded);
    const bool passed = lazy && test_mirror_cache(*loaded) && test_wdl_model();

    if (passed)
        std::cout << "PASS nnue p2-h32 (" << mors::nnue::worker_backend() << ")\n";
    return passed;
}
