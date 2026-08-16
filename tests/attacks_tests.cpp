// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "chess/attacks.hpp"

#include <cstdint>
#include <iostream>

namespace {

class SplitMix64 final {
public:
    explicit SplitMix64(std::uint64_t state) : state_(state) {}

    std::uint64_t next() noexcept {
        std::uint64_t value = (state_ += 0x9E3779B97F4A7C15ULL);
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31);
    }

private:
    std::uint64_t state_;
};

bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

bool run_attacks_tests() {
    using namespace mros;

    initialize_attacks();
    bool passed = true;

    passed &= expect(square_bb(A1) == 1ULL, "A1 bit");
    passed &= expect(square_bb(H8) == (1ULL << 63), "H8 bit");
    passed &= expect(knight_attacks(A1) == (square_bb(B3) | square_bb(C2)), "A1 knight");
    passed &= expect(
        king_attacks(H8) == (square_bb(G8) | square_bb(G7) | square_bb(H7)),
        "H8 king"
    );
    passed &= expect(pawn_attacks(WHITE, A2) == square_bb(B3), "white pawn A2");
    passed &= expect(pawn_attacks(BLACK, A7) == square_bb(B6), "black pawn A7");
    passed &= expect(between_bb(A1, A8)
                     == (square_bb(A2) | square_bb(A3) | square_bb(A4)
                         | square_bb(A5) | square_bb(A6) | square_bb(A7)),
                     "A1-A8 between");
    passed &= expect(between_bb(C3, F6) == (square_bb(D4) | square_bb(E5)),
                     "C3-F6 between");
    passed &= expect(between_bb(A1, B3) == EMPTY_BB, "unaligned between");

    SplitMix64 rng(0x4D524F532D544553ULL);
    for (int square_index = 0; square_index < SQUARE_NB && passed; ++square_index) {
        const Square square = Square(square_index);
        for (int sample = 0; sample < 20'000; ++sample) {
            const Bitboard occupied = rng.next() | square_bb(square);
            const Bitboard expected_bishop =
                detail::reference_bishop_attacks(square, occupied);
            const Bitboard expected_rook = detail::reference_rook_attacks(square, occupied);
            const SliderAttacks actual = slider_attacks(square, occupied);

            if (actual.bishop != expected_bishop || actual.rook != expected_rook) {
                std::cerr << "FAIL: slider mismatch at square " << square_index
                          << " sample " << sample << '\n';
                passed = false;
                break;
            }
        }
    }

    if (passed)
        std::cout << "PASS attacks\n";
    return passed;
}
