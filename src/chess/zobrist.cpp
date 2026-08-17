// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "zobrist.hpp"

#include <cstddef>

namespace mros::zobrist {
namespace {

class XorShift64Star final {
public:
    explicit constexpr XorShift64Star(Key seed) noexcept : state_(seed) {}

    [[nodiscard]] constexpr Key next() noexcept {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 2685821657736338717ULL;
    }

private:
    Key state_;
};

[[nodiscard]] constexpr Tables make_tables() noexcept {
    Tables tables{};
    XorShift64Star random(SEED);

    for (std::size_t piece_index = 0; piece_index < PIECE_NB; ++piece_index) {
        const Piece piece = Piece(piece_index);
        if (!is_ok(piece))
            continue;

        for (Key& key : tables.piece_square[piece_index])
            key = random.next();
    }

    for (Key& key : tables.en_passant)
        key = random.next();

    for (Key& key : tables.castling)
        key = random.next();

    tables.side = random.next();
    return tables;
}

} // namespace

constinit const Tables KEYS = make_tables();

} // namespace mros::zobrist
