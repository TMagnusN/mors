// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "see.hpp"

#include <cassert>

namespace mros {
namespace {

[[nodiscard]] Bitboard attackers_to(
    const Position& position,
    Square target,
    Bitboard occupied
) noexcept {
    // Compute the target's sliding rays once. Calling Position::attackers_to()
    // for both colors would repeat the most expensive part of this operation.
    const SliderAttacks sliders = slider_attacks(target, occupied);
    return ((pawn_attacks(BLACK, target) & position.pieces(WHITE, PAWN))
          | (pawn_attacks(WHITE, target) & position.pieces(BLACK, PAWN))
          | (knight_attacks(target) & position.pieces(KNIGHT))
          | (sliders.bishop
                & (position.pieces(BISHOP) | position.pieces(QUEEN)))
          | (sliders.rook
                & (position.pieces(ROOK) | position.pieces(QUEEN)))
          | (king_attacks(target) & position.pieces(KING)))
         & occupied;
}

[[nodiscard]] bool can_leave_for_exchange(
    const Position& position,
    Color side,
    Square from,
    Square target,
    Bitboard occupied
) noexcept {
    const Square king = position.king_square(side);

    // A move can expose a sliding attack on its king only if the moving piece
    // and king share a rank, file, or diagonal.
    if (line_bb(king, from) == EMPTY_BB)
        return true;

    clear_square(occupied, from);
    set_square(occupied, target);

    const Color enemy = ~side;
    const Bitboard survivors = occupied & ~square_bb(target);
    const SliderAttacks rays = slider_attacks(king, occupied);
    const Bitboard diagonal_attackers =
        survivors
        & (position.pieces(enemy, BISHOP) | position.pieces(enemy, QUEEN));
    const Bitboard orthogonal_attackers =
        survivors
        & (position.pieces(enemy, ROOK) | position.pieces(enemy, QUEEN));

    return ((rays.bishop & diagonal_attackers)
          | (rays.rook & orthogonal_attackers)) == EMPTY_BB;
}

struct Attacker final {
    PieceType type = NO_PIECE_TYPE;
    Square square = SQ_NONE;
};

[[nodiscard]] Attacker least_valuable_attacker(
    const Position& position,
    Color side,
    Bitboard candidates
) noexcept {
    assert(candidates != EMPTY_BB);

    for (int type = PAWN; type <= KING; ++type) {
        const PieceType piece_type = static_cast<PieceType>(type);
        const Bitboard pieces = candidates & position.pieces(side, piece_type);
        if (pieces != EMPTY_BB)
            return {piece_type, lsb(pieces)};
    }

    assert(false);
    return {};
}

[[nodiscard]] Square captured_square(
    Move move,
    Color side
) noexcept {
    return move.type() == EN_PASSANT
        ? move.to() - pawn_push(side)
        : move.to();
}

[[nodiscard]] SeeValue move_gain(
    const Position& position,
    Move move,
    Color side
) noexcept {
    const Piece captured = position.piece_on(captured_square(move, side));
    SeeValue gain = captured == NO_PIECE
        ? 0
        : see_piece_value(type_of(captured));

    if (move.type() == PROMOTION) {
        gain += see_piece_value(move.promotion_type())
              - see_piece_value(PAWN);
    }
    return gain;
}

} // namespace

bool see_ge(
    const Position& position,
    Move move,
    SeeValue threshold
) noexcept {
    assert(!move.is_none());
    const Color us = position.side_to_move();
    const Piece moving_piece = position.piece_on(move.from());
    assert(is_ok(moving_piece) && color_of(moving_piece) == us);
    assert(move.type() != PROMOTION || type_of(moving_piece) == PAWN);
    assert(move.type() != EN_PASSANT || type_of(moving_piece) == PAWN);

    // Castling has no material exchange on the king destination square.
    if (move.type() == CASTLING)
        return threshold <= 0;

    SeeValue balance = move_gain(position, move, us) - threshold;
    if (balance < 0)
        return false;

    const PieceType victim = move.type() == PROMOTION
        ? move.promotion_type()
        : type_of(moving_piece);
    balance -= see_piece_value(victim);
    if (balance >= 0)
        return true;

    const Square target = move.to();
    Bitboard occupied = position.pieces();
    clear_square(occupied, move.from());
    if (move.type() == EN_PASSANT)
        clear_square(occupied, captured_square(move, us));
    set_square(occupied, target);

    Bitboard attackers = attackers_to(position, target, occupied);
    const Bitboard diagonal_sliders = position.pieces(BISHOP)
                                    | position.pieces(QUEEN);
    const Bitboard orthogonal_sliders = position.pieces(ROOK)
                                      | position.pieces(QUEEN);
    Color side = ~us;
    while (true) {
        Bitboard our_attackers = attackers & position.pieces(side) & occupied;

        // Pins must be checked against the current exchange occupancy. A
        // snapshot cannot tell which pinner disappeared and misses pins that
        // are created later in the sequence.
        Bitboard non_king_attackers =
            our_attackers & ~position.pieces(side, KING);
        while (non_king_attackers != EMPTY_BB) {
            const Square from = pop_lsb(non_king_attackers);
            if (!can_leave_for_exchange(position, side, from, target, occupied))
                clear_square(our_attackers, from);
        }

        if (our_attackers == EMPTY_BB)
            break;

        const Attacker attacker = least_valuable_attacker(
            position,
            side,
            our_attackers
        );

        if (attacker.type == KING) {
            // Recompute after removing the king from its source square: doing
            // so can reveal a slider that was hidden behind the king itself.
            Bitboard king_capture_occupied = occupied;
            clear_square(king_capture_occupied, attacker.square);
            set_square(king_capture_occupied, target);
            const Bitboard defenders =
                attackers_to(position, target, king_capture_occupied)
                & position.pieces(~side)
                & king_capture_occupied
                & ~square_bb(target);
            if (defenders != EMPTY_BB)
                break;
        }

        clear_square(occupied, attacker.square);
        side = ~side;

        // The -1 encodes strict inequality while retaining an integer-only
        // threshold algorithm: balance >= 0 means the side to move can stop.
        balance = -balance - 1 - see_piece_value(attacker.type);
        if (balance >= 0)
            break;

        if (attacker.type == PAWN
            || attacker.type == BISHOP
            || attacker.type == QUEEN) {
            attackers |= bishop_attacks(target, occupied) & diagonal_sliders;
        }
        if (attacker.type == ROOK || attacker.type == QUEEN)
            attackers |= rook_attacks(target, occupied) & orthogonal_sliders;

        attackers &= occupied;
    }

    // The last side selected an exchange it cannot make profitable.
    return side != us;
}

} // namespace mros
