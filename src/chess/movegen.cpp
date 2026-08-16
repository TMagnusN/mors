// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "movegen.hpp"

#include <cassert>

namespace mros {
namespace {

void add_promotions(MoveList& moves, Square from, Square to) noexcept {
    moves.push(Move::promotion(from, to, QUEEN));
    moves.push(Move::promotion(from, to, ROOK));
    moves.push(Move::promotion(from, to, BISHOP));
    moves.push(Move::promotion(from, to, KNIGHT));
}

template<Color Us>
void generate_pawns(const Position& position, MoveList& moves) noexcept {
    constexpr Color Them = Us == WHITE ? BLACK : WHITE;
    constexpr int push = Us == WHITE ? 8 : -8;
    constexpr int start_rank = Us == WHITE ? RANK_2 : RANK_7;
    constexpr int promotion_rank = Us == WHITE ? RANK_7 : RANK_2;

    const Bitboard occupied = position.pieces();
    const Bitboard capture_targets =
        position.pieces(Them) & ~position.pieces(Them, KING);
    Bitboard pawns = position.pieces(Us, PAWN);

    while (pawns) {
        const Square from = pop_lsb(pawns);
        const int from_file = file_of(from);
        const int from_rank = rank_of(from);
        const int one_index = int(from) + push;

        if (one_index >= 0 && one_index < SQUARE_NB) {
            const Square one = Square(one_index);
            if (!contains(occupied, one)) {
                if (from_rank == promotion_rank) {
                    add_promotions(moves, from, one);
                } else {
                    moves.push(Move::normal(from, one));

                    if (from_rank == start_rank) {
                        const Square two = Square(int(from) + 2 * push);
                        if (!contains(occupied, two))
                            moves.push(Move::normal(from, two));
                    }
                }
            }
        }

        const int target_rank = from_rank + (Us == WHITE ? 1 : -1);
        for (const int file_delta : {-1, 1}) {
            const int target_file = from_file + file_delta;
            if (target_file < FILE_A || target_file > FILE_H
                || target_rank < RANK_1 || target_rank > RANK_8) {
                continue;
            }

            const Square to = make_square(File(target_file), Rank(target_rank));
            if (contains(capture_targets, to)) {
                if (from_rank == promotion_rank)
                    add_promotions(moves, from, to);
                else
                    moves.push(Move::normal(from, to));
            } else if (to == position.ep_square()) {
                const Square captured = to - pawn_push(Us);
                if (position.piece_on(captured) == make_piece(Them, PAWN))
                    moves.push(Move::en_passant(from, to));
            }
        }
    }
}

template<Color Us, PieceType Type>
void generate_piece_moves(const Position& position, MoveList& moves) noexcept {
    constexpr Color Them = Us == WHITE ? BLACK : WHITE;
    const Bitboard allowed =
        ~(position.pieces(Us) | position.pieces(Them, KING));
    const Bitboard occupied = position.pieces();
    Bitboard pieces = position.pieces(Us, Type);

    while (pieces) {
        const Square from = pop_lsb(pieces);
        Bitboard destinations = EMPTY_BB;

        if constexpr (Type == KNIGHT)
            destinations = knight_attacks(from);
        else if constexpr (Type == BISHOP)
            destinations = bishop_attacks(from, occupied);
        else if constexpr (Type == ROOK)
            destinations = rook_attacks(from, occupied);
        else if constexpr (Type == QUEEN)
            destinations = queen_attacks(from, occupied);
        else if constexpr (Type == KING)
            destinations = king_attacks(from);

        destinations &= allowed;
        while (destinations)
            moves.push(Move::normal(from, pop_lsb(destinations)));
    }
}

template<Color Us>
void generate_castling(const Position& position, MoveList& moves) noexcept {
    constexpr Color Them = Us == WHITE ? BLACK : WHITE;
    constexpr CastlingRights king_side_right =
        Us == WHITE ? WHITE_KING_SIDE : BLACK_KING_SIDE;
    constexpr CastlingRights queen_side_right =
        Us == WHITE ? WHITE_QUEEN_SIDE : BLACK_QUEEN_SIDE;

    const Square king_from = relative_square(Us, E1);
    if (position.piece_on(king_from) != make_piece(Us, KING)
        || position.is_square_attacked(king_from, Them)) {
        return;
    }

    const Bitboard occupied_without_king = position.pieces() ^ square_bb(king_from);

    if (position.can_castle(king_side_right)) {
        const Square rook_from = relative_square(Us, H1);
        const Square through = relative_square(Us, F1);
        const Square destination = relative_square(Us, G1);

        if (position.piece_on(rook_from) == make_piece(Us, ROOK)
            && position.piece_on(through) == NO_PIECE
            && position.piece_on(destination) == NO_PIECE
            && !position.is_square_attacked(through, Them, occupied_without_king)
            && !position.is_square_attacked(destination, Them, occupied_without_king)) {
            moves.push(Move::castling(king_from, destination));
        }
    }

    if (position.can_castle(queen_side_right)) {
        const Square rook_from = relative_square(Us, A1);
        const Square extra_empty = relative_square(Us, B1);
        const Square destination = relative_square(Us, C1);
        const Square through = relative_square(Us, D1);

        if (position.piece_on(rook_from) == make_piece(Us, ROOK)
            && position.piece_on(extra_empty) == NO_PIECE
            && position.piece_on(destination) == NO_PIECE
            && position.piece_on(through) == NO_PIECE
            && !position.is_square_attacked(through, Them, occupied_without_king)
            && !position.is_square_attacked(destination, Them, occupied_without_king)) {
            moves.push(Move::castling(king_from, destination));
        }
    }
}

template<Color Us>
void generate_for_color(const Position& position, MoveList& moves) noexcept {
    generate_pawns<Us>(position, moves);
    generate_piece_moves<Us, KNIGHT>(position, moves);
    generate_piece_moves<Us, BISHOP>(position, moves);
    generate_piece_moves<Us, ROOK>(position, moves);
    generate_piece_moves<Us, QUEEN>(position, moves);
    generate_piece_moves<Us, KING>(position, moves);
    generate_castling<Us>(position, moves);
}

} // namespace

void generate_pseudo_legal(const Position& position, MoveList& moves) noexcept {
    assert(moves.empty() && position.is_consistent());
    if (position.side_to_move() == WHITE)
        generate_for_color<WHITE>(position, moves);
    else
        generate_for_color<BLACK>(position, moves);
}

void generate_legal(Position& position, MoveList& moves) noexcept {
    assert(moves.empty() && position.is_consistent());
    MoveList candidates;
    generate_pseudo_legal(position, candidates);

    const Color us = position.side_to_move();
    const Color them = ~us;

    for (const Move move : candidates) {
        StateInfo state;
        position.do_move(move, state);
        const bool legal = !position.is_square_attacked(position.king_square(us), them);
        position.undo_move(move, state);

        if (legal)
            moves.push(move);
    }
}

} // namespace mros
