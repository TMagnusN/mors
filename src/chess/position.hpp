// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "attacks.hpp"
#include "move.hpp"

namespace mors {

inline constexpr std::string_view START_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

struct StateInfo final {
    Key key = 0;
    CastlingRights castling_rights = NO_CASTLING;
    Square ep_square = SQ_NONE;
    std::uint16_t halfmove_clock = 0;
    std::uint16_t fullmove_number = 1;
    Piece captured_piece = NO_PIECE;
    Square captured_square = SQ_NONE;
};

class Position final {
public:
    Position() = default;

    [[nodiscard]] static std::expected<Position, std::string> from_fen(std::string_view fen);
    [[nodiscard]] std::string fen() const;

    [[nodiscard]] Piece piece_on(Square square) const noexcept;
    [[nodiscard]] Bitboard pieces() const noexcept { return occupied_; }
    [[nodiscard]] Bitboard pieces(Color color) const noexcept;
    [[nodiscard]] Bitboard pieces(PieceType type) const noexcept;
    [[nodiscard]] Bitboard pieces(Color color, PieceType type) const noexcept;

    [[nodiscard]] Color side_to_move() const noexcept { return side_to_move_; }
    [[nodiscard]] Key key() const noexcept { return key_; }
    [[nodiscard]] CastlingRights castling_rights() const noexcept { return castling_rights_; }
    [[nodiscard]] bool can_castle(CastlingRights rights) const noexcept;
    [[nodiscard]] Square ep_square() const noexcept { return ep_square_; }
    [[nodiscard]] std::uint16_t halfmove_clock() const noexcept { return halfmove_clock_; }
    [[nodiscard]] std::uint16_t fullmove_number() const noexcept { return fullmove_number_; }
    [[nodiscard]] Square king_square(Color color) const noexcept;

    [[nodiscard]] Bitboard attackers_to(
        Square square,
        Color by_color,
        Bitboard occupied
    ) const noexcept;

    [[nodiscard]] bool is_square_attacked(
        Square square,
        Color by_color,
        Bitboard occupied
    ) const noexcept {
        return attackers_to(square, by_color, occupied) != EMPTY_BB;
    }

    [[nodiscard]] bool is_square_attacked(Square square, Color by_color) const noexcept {
        return is_square_attacked(square, by_color, occupied_);
    }

    void do_move(Move move, StateInfo& state) noexcept;
    void undo_move(Move move, const StateInfo& state) noexcept;

    [[nodiscard]] bool is_consistent() const noexcept;

private:
    void put_piece(Piece piece, Square square) noexcept;
    void remove_piece(Square square) noexcept;
    void move_piece(Square from, Square to) noexcept;
    void clear_castling_right(CastlingRights right) noexcept;
    [[nodiscard]] Key compute_key() const noexcept;

    std::array<Piece, SQUARE_NB> board_{};
    std::array<Bitboard, PIECE_NB> piece_bitboards_{};
    std::array<Bitboard, COLOR_NB> color_bitboards_{};
    Bitboard occupied_ = EMPTY_BB;
    Key key_ = 0;

    Color side_to_move_ = WHITE;
    CastlingRights castling_rights_ = NO_CASTLING;
    Square ep_square_ = SQ_NONE;
    std::uint16_t halfmove_clock_ = 0;
    std::uint16_t fullmove_number_ = 1;
};

} // namespace mors
