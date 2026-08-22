// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search/see.hpp"

#include <iostream>
#include <string_view>

namespace {

using namespace mors;

static_assert(see_piece_value(PAWN) == 100);
static_assert(see_piece_value(KNIGHT) == 320);
static_assert(see_piece_value(BISHOP) == 330);
static_assert(see_piece_value(ROOK) == 500);
static_assert(see_piece_value(QUEEN) == 900);
static_assert(see_piece_value(KING) == 0);

bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL see: " << message << '\n';
    return condition;
}

bool expect_see(
    std::string_view fen,
    Move move,
    SeeValue threshold,
    bool expected,
    const char* message
) {
    auto parsed = Position::from_fen(fen);
    if (!expect(parsed.has_value(), "test FEN must parse"))
        return false;

    const bool actual = see_ge(*parsed, move, threshold);
    if (actual != expected) {
        std::cerr << "FAIL see: " << message
                  << " threshold=" << threshold
                  << " expected=" << expected
                  << " actual=" << actual << '\n';
        return false;
    }
    return true;
}

bool test_basic_thresholds() {
    constexpr std::string_view WIN_QUEEN =
        "4k3/8/8/3q4/4P3/8/8/4K3 w - - 0 1";
    constexpr std::string_view EVEN_ROOKS =
        "3qk3/8/8/3r4/8/8/8/3RK3 w - - 0 1";
    constexpr std::string_view QUIET_KNIGHT =
        "4k3/8/7p/8/8/5N2/8/4K3 w - - 0 1";

    return expect_see(WIN_QUEEN, Move::normal(E4, D5), 900, true,
                      "pawn capture wins exactly a queen")
        && expect_see(WIN_QUEEN, Move::normal(E4, D5), 901, false,
                      "pawn capture does not exceed a queen")
        && expect_see(EVEN_ROOKS, Move::normal(D1, D5), 0, true,
                      "equal rook exchange is non-losing")
        && expect_see(EVEN_ROOKS, Move::normal(D1, D5), 1, false,
                      "equal rook exchange does not gain material")
        && expect_see(QUIET_KNIGHT, Move::normal(F3, E5), 0, true,
                      "safe quiet move is non-losing")
        && expect_see(QUIET_KNIGHT, Move::normal(F3, G5), 0, false,
                      "attacked quiet move loses its knight");
}

bool test_special_moves() {
    constexpr std::string_view EN_PASSANT_FEN =
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1";
    constexpr std::string_view PROMOTION_FEN =
        "4k3/P7/8/8/8/8/8/4K3 w - - 0 1";
    constexpr std::string_view CAPTURE_PROMOTION_FEN =
        "1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1";
    constexpr std::string_view CASTLING_FEN =
        "4k3/8/8/8/8/8/8/4K2R w K - 0 1";

    return expect_see(EN_PASSANT_FEN, Move::en_passant(E5, D6), 100, true,
                      "en-passant wins one pawn")
        && expect_see(EN_PASSANT_FEN, Move::en_passant(E5, D6), 101, false,
                      "en-passant does not win more than one pawn")
        && expect_see(PROMOTION_FEN, Move::promotion(A7, A8, QUEEN), 800, true,
                      "quiet queen promotion gains queen minus pawn")
        && expect_see(PROMOTION_FEN, Move::promotion(A7, A8, QUEEN), 801, false,
                      "quiet promotion threshold is exact")
        && expect_see(CAPTURE_PROMOTION_FEN,
                      Move::promotion(A7, B8, QUEEN), 1300, true,
                      "capture promotion includes rook and promotion gain")
        && expect_see(CAPTURE_PROMOTION_FEN,
                      Move::promotion(A7, B8, QUEEN), 1301, false,
                      "capture promotion threshold is exact")
        && expect_see(CASTLING_FEN, Move::castling(E1, H1), 0, true,
                      "castling is material-neutral")
        && expect_see(CASTLING_FEN, Move::castling(E1, H1), 1, false,
                      "castling has no positive material gain");
}

bool test_pin_and_king_safety() {
    constexpr std::string_view PINNED_RECAPTURE =
        "4r1k1/8/6b1/8/8/3PR3/8/4K3 b - - 0 1";
    constexpr std::string_view ALONG_PIN_RECAPTURE =
        "4r1k1/1q6/8/8/4P3/4R3/8/4K3 b - - 0 1";
    constexpr std::string_view PROTECTED_TARGET =
        "4k3/3p4/8/8/6B1/8/8/3QK3 w - - 0 1";
    constexpr std::string_view UNPROTECTED_TARGET =
        "4k3/3p4/8/8/8/8/8/3QK3 w - - 0 1";
    constexpr std::string_view PINNER_LEAVES_EXCHANGE =
        "2r4k/8/8/8/8/4qp2/2RN2B1/2K5 w - - 0 1";

    return expect_see(PINNED_RECAPTURE, Move::normal(G6, D3), 0, true,
                      "pinned rook cannot recapture off the king line")
        && expect_see(ALONG_PIN_RECAPTURE, Move::normal(B7, E4), 0, false,
                      "pinned rook may recapture along the king line")
        && expect_see(PROTECTED_TARGET, Move::normal(D1, D7), 0, true,
                      "king cannot recapture a protected queen")
        && expect_see(UNPROTECTED_TARGET, Move::normal(D1, D7), 0, false,
                      "king may recapture an unprotected queen")
        && expect_see(PINNER_LEAVES_EXCHANGE,
                      Move::normal(G2, F3), 100, true,
                      "a departing pinner releases only its own pinned attacker")
        && expect_see(PINNER_LEAVES_EXCHANGE,
                      Move::normal(G2, F3), 101, false,
                      "released pinned attacker makes the pawn win exact");
}

bool test_xray_reveal() {
    constexpr std::string_view ORTHOGONAL_XRAY =
        "k2q4/3r4/8/3p4/4Q3/8/8/K2R4 w - - 0 1";
    constexpr std::string_view DIAGONAL_XRAY =
        "6qk/5b2/8/3p4/4Q3/1B6/8/K7 w - - 0 1";

    return expect_see(ORTHOGONAL_XRAY, Move::normal(E4, D5), -800, true,
                      "orthogonal x-ray sequence loses exactly eight pawns")
        && expect_see(ORTHOGONAL_XRAY, Move::normal(E4, D5), -799, false,
                      "revealed rook-line queen changes the exchange result")
        && expect_see(DIAGONAL_XRAY, Move::normal(E4, D5), -800, true,
                      "diagonal x-ray sequence loses exactly eight pawns")
        && expect_see(DIAGONAL_XRAY, Move::normal(E4, D5), -799, false,
                      "revealed bishop-line queen changes the exchange result");
}

} // namespace

bool run_see_tests() {
    const bool passed = test_basic_thresholds()
                     && test_special_moves()
                     && test_pin_and_king_safety()
                     && test_xray_reveal();

    if (passed)
        std::cout << "PASS static exchange evaluation\n";
    return passed;
}
