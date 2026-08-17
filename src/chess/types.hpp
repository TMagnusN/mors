// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cassert>
#include <cstdint>

namespace mros {

using Key = std::uint64_t;
using Value = std::int32_t;
using Depth = std::int32_t;

// Search values occupy disjoint ordinary, tablebase and mate bands. MAX_PLY
// also leaves sixteen guard states in the 256-entry NNUE worker stack.
inline constexpr int MAX_PLY = 240;

inline constexpr Value VALUE_DRAW               = 0;
inline constexpr Value VALUE_MATE               = 32'000;
inline constexpr Value VALUE_MATE_IN_MAX_PLY    = VALUE_MATE - MAX_PLY;
inline constexpr Value VALUE_TB                 = VALUE_MATE_IN_MAX_PLY - 1;
inline constexpr Value VALUE_TB_WIN_IN_MAX_PLY  = VALUE_TB - MAX_PLY;
inline constexpr Value VALUE_EVAL_MAX           = VALUE_TB_WIN_IN_MAX_PLY - 1;
inline constexpr Value VALUE_INFINITE           = VALUE_MATE + 1;
inline constexpr Value VALUE_NONE               = VALUE_INFINITE + 1;

static_assert(VALUE_DRAW == 0);
static_assert(VALUE_EVAL_MAX == 31'518);
static_assert(VALUE_TB_WIN_IN_MAX_PLY == 31'519);
static_assert(VALUE_TB == 31'759);
static_assert(VALUE_MATE_IN_MAX_PLY == 31'760);
static_assert(VALUE_NONE < (1 << 15));

inline constexpr Depth DEPTH_QS         = 0;
inline constexpr Depth DEPTH_UNSEARCHED = -2;


// ============================================================================
// Search bound
// ============================================================================

enum Bound : std::uint8_t {
    BOUND_NONE,
    BOUND_UPPER,
    BOUND_LOWER,
    BOUND_EXACT = BOUND_UPPER | BOUND_LOWER
};

constexpr bool is_ok(Bound bound) noexcept {
    return bound <= BOUND_EXACT;
}

// ============================================================================
// Color
// ============================================================================

enum Color : std::uint8_t {
    WHITE,
    BLACK,

    COLOR_NB
};

constexpr bool is_ok(Color c) noexcept { return c < COLOR_NB; }

constexpr Color operator~(Color c) noexcept {
    assert(is_ok(c));
    return Color(c ^ 1);
}


// ============================================================================
// Piece type
//
// 0 is intentionally shared by NO_PIECE_TYPE / ALL_PIECES.
// PAWN .. KING can be used directly as array indices. Value 7 is reserved.
// ============================================================================

enum PieceType : std::uint8_t {
    NO_PIECE_TYPE,

    PAWN,
    KNIGHT,
    BISHOP,
    ROOK,
    QUEEN,
    KING,

    ALL_PIECES = 0,
    PIECE_TYPE_NB = 8
};

constexpr bool is_ok(PieceType pt) noexcept { return pt >= PAWN && pt <= KING; }


// ============================================================================
// Piece
//
// bits 0..2 : PieceType
// bit 3     : Color
//
// White: 1..6
// Black: 9..14
// ============================================================================

enum Piece : std::uint8_t {
    NO_PIECE,

    W_PAWN   = PAWN,
    W_KNIGHT = KNIGHT,
    W_BISHOP = BISHOP,
    W_ROOK   = ROOK,
    W_QUEEN  = QUEEN,
    W_KING   = KING,

    B_PAWN   = PAWN   + 8,
    B_KNIGHT = KNIGHT + 8,
    B_BISHOP = BISHOP + 8,
    B_ROOK   = ROOK   + 8,
    B_QUEEN  = QUEEN  + 8,
    B_KING   = KING   + 8,

    PIECE_NB = 16
};

constexpr bool is_ok(Piece pc) noexcept {
    return pc != NO_PIECE && is_ok(PieceType(pc & 7)) && pc != Piece(8);
}

constexpr Piece make_piece(Color c, PieceType pt) noexcept {
    assert(is_ok(c) && is_ok(pt));
    return Piece((c << 3) | pt);
}

constexpr PieceType type_of(Piece pc) noexcept {
    return PieceType(pc & 7);
}

constexpr Color color_of(Piece pc) noexcept {
    assert(is_ok(pc));
    return Color(pc >> 3);
}

constexpr Piece operator~(Piece pc) noexcept {
    assert(is_ok(pc));
    return Piece(pc ^ 8);
}


// ============================================================================
// File / Rank
// ============================================================================

enum File : std::uint8_t {
    FILE_A,
    FILE_B,
    FILE_C,
    FILE_D,
    FILE_E,
    FILE_F,
    FILE_G,
    FILE_H,

    FILE_NB
};

enum Rank : std::uint8_t {
    RANK_1,
    RANK_2,
    RANK_3,
    RANK_4,
    RANK_5,
    RANK_6,
    RANK_7,
    RANK_8,

    RANK_NB
};

constexpr bool is_ok(File f) noexcept { return f < FILE_NB; }
constexpr bool is_ok(Rank r) noexcept { return r < RANK_NB; }


// ============================================================================
// Square
//
// A1 = 0 ... H1 = 7
// A2 = 8 ...
// H8 = 63
//
// This layout maps directly onto a 64-bit bitboard.
// ============================================================================

enum Square : std::uint8_t {
    A1, B1, C1, D1, E1, F1, G1, H1,
    A2, B2, C2, D2, E2, F2, G2, H2,
    A3, B3, C3, D3, E3, F3, G3, H3,
    A4, B4, C4, D4, E4, F4, G4, H4,
    A5, B5, C5, D5, E5, F5, G5, H5,
    A6, B6, C6, D6, E6, F6, G6, H6,
    A7, B7, C7, D7, E7, F7, G7, H7,
    A8, B8, C8, D8, E8, F8, G8, H8,

    SQ_NONE,
    SQUARE_NB = 64
};

constexpr bool is_ok(Square sq) noexcept { return sq < SQUARE_NB; }

constexpr Square make_square(File f, Rank r) noexcept {
    assert(is_ok(f) && is_ok(r));
    return Square((r << 3) | f);
}

constexpr File file_of(Square sq) noexcept {
    assert(is_ok(sq));
    return File(sq & 7);
}

constexpr Rank rank_of(Square sq) noexcept {
    assert(is_ok(sq));
    return Rank(sq >> 3);
}


// ============================================================================
// Direction
// ============================================================================

enum Direction : std::int8_t {
    NORTH =  8,
    EAST  =  1,
    SOUTH = -8,
    WEST  = -1,

    NORTH_EAST =  9,
    NORTH_WEST =  7,
    SOUTH_EAST = -7,
    SOUTH_WEST = -9
};

constexpr Direction pawn_push(Color c) noexcept {
    assert(is_ok(c));
    return c == WHITE ? NORTH : SOUTH;
}

// The result may be invalid when the operation crosses a board edge. Callers
// must establish that the destination is on the board before using it.
constexpr Square operator+(Square sq, Direction d) noexcept {
    return Square(int(sq) + int(d));
}

constexpr Square operator-(Square sq, Direction d) noexcept {
    return Square(int(sq) - int(d));
}


// ============================================================================
// Relative coordinates
// ============================================================================

constexpr Rank relative_rank(Color c, Rank r) noexcept {
    assert(is_ok(c) && is_ok(r));
    return Rank(r ^ (c * 7));
}

constexpr Rank relative_rank(Color c, Square sq) noexcept {
    return relative_rank(c, rank_of(sq));
}

constexpr Square relative_square(Color c, Square sq) noexcept {
    assert(is_ok(c) && is_ok(sq));
    return Square(sq ^ (c * 56));
}


// ============================================================================
// Castling rights
// ============================================================================

enum CastlingRights : std::uint8_t {
    NO_CASTLING      = 0,
    WHITE_KING_SIDE  = 1,
    WHITE_QUEEN_SIDE = 2,
    BLACK_KING_SIDE  = 4,
    BLACK_QUEEN_SIDE = 8,
    WHITE_CASTLING   = WHITE_KING_SIDE | WHITE_QUEEN_SIDE,
    BLACK_CASTLING   = BLACK_KING_SIDE | BLACK_QUEEN_SIDE,
    ANY_CASTLING     = WHITE_CASTLING | BLACK_CASTLING,

    CASTLING_RIGHT_NB = 16
};

constexpr CastlingRights operator|(CastlingRights lhs, CastlingRights rhs) noexcept {
    return CastlingRights(std::uint8_t(lhs) | std::uint8_t(rhs));
}

constexpr CastlingRights operator&(CastlingRights lhs, CastlingRights rhs) noexcept {
    return CastlingRights(std::uint8_t(lhs) & std::uint8_t(rhs));
}

constexpr CastlingRights& operator|=(CastlingRights& lhs, CastlingRights rhs) noexcept {
    return lhs = lhs | rhs;
}


// ============================================================================
// Sanity
// ============================================================================

static_assert(sizeof(Color)          == 1);
static_assert(sizeof(Key)            == 8);
static_assert(sizeof(Value)          == 4);
static_assert(sizeof(Depth)          == 4);
static_assert(sizeof(Bound)          == 1);
static_assert(sizeof(PieceType)      == 1);
static_assert(sizeof(Piece)          == 1);
static_assert(sizeof(File)           == 1);
static_assert(sizeof(Rank)           == 1);
static_assert(sizeof(Square)         == 1);
static_assert(sizeof(CastlingRights) == 1);

} // namespace mros
