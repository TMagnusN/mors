// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "movegen.hpp"

#include <cassert>

namespace mors {
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

struct LegalGenInfo final {
    Square king = SQ_NONE;
    Bitboard occupied = EMPTY_BB;
    Bitboard us = EMPTY_BB;
    Bitboard them = EMPTY_BB;
    Bitboard capture_targets = EMPTY_BB;
    Bitboard checkers = EMPTY_BB;
    Bitboard pinned = EMPTY_BB;
    Bitboard evasion_mask = FULL_BB;
    bool double_check = false;
};

template<Color Us>
[[nodiscard]] LegalGenInfo make_legal_info(const Position& position) noexcept {
    constexpr Color Them = Us == WHITE ? BLACK : WHITE;
    LegalGenInfo info;
    info.king = position.king_square(Us);
    info.occupied = position.pieces();
    info.us = position.pieces(Us);
    info.them = position.pieces(Them);
    info.capture_targets = info.them & ~position.pieces(Them, KING);
    info.checkers = position.attackers_to(info.king, Them, info.occupied);
    info.double_check = more_than_one(info.checkers);

    if (info.checkers == EMPTY_BB) {
        info.evasion_mask = FULL_BB;
    } else if (info.double_check) {
        info.evasion_mask = EMPTY_BB;
    } else {
        const Square checker = lsb(info.checkers);
        info.evasion_mask = square_bb(checker) | between_bb(info.king, checker);
    }

    const SliderAttacks rays = slider_attacks(info.king, EMPTY_BB);
    Bitboard snipers =
        (rays.bishop & (position.pieces(Them, BISHOP) | position.pieces(Them, QUEEN)))
      | (rays.rook & (position.pieces(Them, ROOK) | position.pieces(Them, QUEEN)));

    while (snipers) {
        const Square sniper = pop_lsb(snipers);
        const Bitboard blockers = between_bb(info.king, sniper) & info.occupied;
        if (has_single_bit(blockers) && (blockers & info.us) != EMPTY_BB)
            info.pinned |= blockers;
    }

    return info;
}

template<PieceType Type>
[[nodiscard]] Bitboard legal_piece_attacks(
    Square from,
    Bitboard occupied
) noexcept {
    static_assert(Type == KNIGHT || Type == BISHOP || Type == ROOK || Type == QUEEN);

    if constexpr (Type == KNIGHT)
        return knight_attacks(from);
    else if constexpr (Type == BISHOP)
        return bishop_attacks(from, occupied);
    else if constexpr (Type == ROOK)
        return rook_attacks(from, occupied);
    else
        return queen_attacks(from, occupied);
}

template<Color Us, PieceType Type>
void generate_legal_pieces(
    const Position& position,
    const LegalGenInfo& info,
    MoveList& moves
) noexcept {
    constexpr Color Them = Us == WHITE ? BLACK : WHITE;
    const Bitboard forbidden = info.us | position.pieces(Them, KING);
    Bitboard pieces = position.pieces(Us, Type);

    while (pieces) {
        const Square from = pop_lsb(pieces);
        Bitboard destinations = legal_piece_attacks<Type>(from, info.occupied)
                              & ~forbidden
                              & info.evasion_mask;

        if (contains(info.pinned, from))
            destinations &= line_bb(info.king, from);

        while (destinations)
            moves.push(Move::normal(from, pop_lsb(destinations)));
    }
}

template<int Delta>
void append_pawn_targets(Bitboard targets, MoveList& moves) noexcept {
    while (targets) {
        const Square to = pop_lsb(targets);
        moves.push(Move::normal(Square(int(to) - Delta), to));
    }
}

template<int Delta>
void append_pawn_promotions(Bitboard targets, MoveList& moves) noexcept {
    while (targets) {
        const Square to = pop_lsb(targets);
        add_promotions(moves, Square(int(to) - Delta), to);
    }
}

template<Color Us>
void generate_legal_pawns(
    Position& position,
    const LegalGenInfo& info,
    MoveList& moves
) noexcept {
    constexpr Color Them = Us == WHITE ? BLACK : WHITE;
    constexpr Direction Push = Us == WHITE ? NORTH : SOUTH;
    constexpr Direction CaptureWest = Us == WHITE ? NORTH_WEST : SOUTH_WEST;
    constexpr Direction CaptureEast = Us == WHITE ? NORTH_EAST : SOUTH_EAST;
    constexpr int PushDelta = int(Push);
    constexpr int WestDelta = int(CaptureWest);
    constexpr int EastDelta = int(CaptureEast);
    constexpr Bitboard PromotionFrom = Us == WHITE ? RANK_7_BB : RANK_2_BB;
    constexpr Bitboard DoublePushMiddle = Us == WHITE ? RANK_3_BB : RANK_6_BB;
    constexpr Rank StartRank = Us == WHITE ? RANK_2 : RANK_7;
    constexpr Rank PromotionRank = Us == WHITE ? RANK_7 : RANK_2;

    const Bitboard pawns = position.pieces(Us, PAWN);
    const Bitboard unpinned = pawns & ~info.pinned;
    const Bitboard promotion_pawns = unpinned & PromotionFrom;
    const Bitboard ordinary_pawns = unpinned & ~PromotionFrom;
    const Bitboard empty = ~info.occupied;

    Bitboard one = shift<Push>(ordinary_pawns) & empty;
    Bitboard two = shift<Push>(one & DoublePushMiddle) & empty;
    one &= info.evasion_mask;
    two &= info.evasion_mask;
    append_pawn_targets<PushDelta>(one, moves);
    append_pawn_targets<2 * PushDelta>(two, moves);

    Bitboard promotions = shift<Push>(promotion_pawns) & empty & info.evasion_mask;
    append_pawn_promotions<PushDelta>(promotions, moves);

    Bitboard west = shift<CaptureWest>(ordinary_pawns)
                  & info.capture_targets
                  & info.evasion_mask;
    Bitboard east = shift<CaptureEast>(ordinary_pawns)
                  & info.capture_targets
                  & info.evasion_mask;
    append_pawn_targets<WestDelta>(west, moves);
    append_pawn_targets<EastDelta>(east, moves);

    Bitboard west_promotions = shift<CaptureWest>(promotion_pawns)
                             & info.capture_targets
                             & info.evasion_mask;
    Bitboard east_promotions = shift<CaptureEast>(promotion_pawns)
                             & info.capture_targets
                             & info.evasion_mask;
    append_pawn_promotions<WestDelta>(west_promotions, moves);
    append_pawn_promotions<EastDelta>(east_promotions, moves);

    Bitboard pinned = pawns & info.pinned;
    while (pinned) {
        const Square from = pop_lsb(pinned);
        const Bitboard allowed = line_bb(info.king, from) & info.evasion_mask;
        const int one_index = int(from) + PushDelta;

        if (one_index >= 0 && one_index < SQUARE_NB) {
            const Square to = Square(one_index);
            const Bitboard to_bb = square_bb(to);
            if ((info.occupied & to_bb) == EMPTY_BB) {
                if ((allowed & to_bb) != EMPTY_BB) {
                    if (rank_of(from) == PromotionRank)
                        add_promotions(moves, from, to);
                    else
                        moves.push(Move::normal(from, to));
                }

                if (rank_of(from) == StartRank) {
                    const Square double_to = Square(int(from) + 2 * PushDelta);
                    const Bitboard double_to_bb = square_bb(double_to);
                    if ((info.occupied & double_to_bb) == EMPTY_BB
                        && (allowed & double_to_bb) != EMPTY_BB) {
                        moves.push(Move::normal(from, double_to));
                    }
                }
            }
        }

        const int from_file = file_of(from);
        for (const int delta : {WestDelta, EastDelta}) {
            if ((delta == WestDelta && from_file == FILE_A)
                || (delta == EastDelta && from_file == FILE_H)) {
                continue;
            }

            const Square to = Square(int(from) + delta);
            const Bitboard to_bb = square_bb(to);
            if ((info.capture_targets & allowed & to_bb) == EMPTY_BB)
                continue;

            if (rank_of(from) == PromotionRank)
                add_promotions(moves, from, to);
            else
                moves.push(Move::normal(from, to));
        }
    }

    const Square ep = position.ep_square();
    if (ep == SQ_NONE)
        return;

    const Square captured = ep - pawn_push(Us);
    if (position.piece_on(captured) != make_piece(Them, PAWN))
        return;

    Bitboard ep_pawns = pawn_attacks<Them>(square_bb(ep)) & pawns;
    while (ep_pawns) {
        const Square from = pop_lsb(ep_pawns);
        const Move move = Move::en_passant(from, ep);
        StateInfo state;
        position.do_move(move, state);
        const bool legal = !position.is_square_attacked(position.king_square(Us), Them);
        position.undo_move(move, state);

        if (legal)
            moves.push(move);
    }
}

template<Color Us>
void generate_legal_king(
    const Position& position,
    const LegalGenInfo& info,
    MoveList& moves
) noexcept {
    constexpr Color Them = Us == WHITE ? BLACK : WHITE;
    const Bitboard forbidden = info.us | position.pieces(Them, KING);
    Bitboard destinations = king_attacks(info.king) & ~forbidden;
    const Bitboard king_bb = square_bb(info.king);

    while (destinations) {
        const Square to = pop_lsb(destinations);
        const Bitboard occupied_after = info.occupied & ~king_bb & ~square_bb(to);
        if (!position.is_square_attacked(to, Them, occupied_after))
            moves.push(Move::normal(info.king, to));
    }
}

template<Color Us>
void generate_direct_legal(Position& position, MoveList& moves) noexcept {
    const LegalGenInfo info = make_legal_info<Us>(position);

    if (!info.double_check) {
        generate_legal_pawns<Us>(position, info, moves);
        generate_legal_pieces<Us, KNIGHT>(position, info, moves);
        generate_legal_pieces<Us, BISHOP>(position, info, moves);
        generate_legal_pieces<Us, ROOK>(position, info, moves);
        generate_legal_pieces<Us, QUEEN>(position, info, moves);
    }

    generate_legal_king<Us>(position, info, moves);
    if (info.checkers == EMPTY_BB)
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
    if (position.side_to_move() == WHITE)
        generate_direct_legal<WHITE>(position, moves);
    else
        generate_direct_legal<BLACK>(position, moves);
}

} // namespace mors
