// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "position.hpp"
#include "zobrist.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <sstream>
#include <string>
#include <system_error>

namespace mors {
namespace {

Piece piece_from_char(char value) noexcept {
    switch (value) {
    case 'P': return W_PAWN;
    case 'N': return W_KNIGHT;
    case 'B': return W_BISHOP;
    case 'R': return W_ROOK;
    case 'Q': return W_QUEEN;
    case 'K': return W_KING;
    case 'p': return B_PAWN;
    case 'n': return B_KNIGHT;
    case 'b': return B_BISHOP;
    case 'r': return B_ROOK;
    case 'q': return B_QUEEN;
    case 'k': return B_KING;
    default:  return NO_PIECE;
    }
}

char char_from_piece(Piece piece) noexcept {
    constexpr char PIECE_CHARS[PIECE_NB] = {
        ' ', 'P', 'N', 'B', 'R', 'Q', 'K', ' ',
        ' ', 'p', 'n', 'b', 'r', 'q', 'k', ' '
    };
    assert(piece < PIECE_NB);
    return PIECE_CHARS[piece];
}

template<typename Integer>
bool parse_integer(std::string_view text, Integer& result) noexcept {
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto [pointer, error] = std::from_chars(begin, end, result);
    return error == std::errc{} && pointer == end;
}

Square relative_square_unchecked(Color color, Square white_square) noexcept {
    return color == WHITE ? white_square : Square(int(white_square) + 56);
}

} // namespace

std::expected<Position, std::string> Position::from_fen(std::string_view fen_text) {
    std::istringstream stream{std::string(fen_text)};
    std::string board_field;
    std::string side_field;
    std::string castling_field;
    std::string ep_field;
    std::string halfmove_field;
    std::string fullmove_field;
    std::string trailing;

    // The first four fields describe the actual position. Like Stockfish,
    // accept FEN/EPD input without the two move counters; GUIs and opening
    // tools commonly omit them. Missing counters use their initial values.
    if (!(stream >> board_field >> side_field >> castling_field >> ep_field))
        return std::unexpected("FEN must contain at least four fields");

    halfmove_field = "0";
    fullmove_field = "1";
    if (stream >> halfmove_field) {
        if (stream >> fullmove_field && stream >> trailing)
            return std::unexpected("FEN must contain at most six fields");
    }

    Position position;
    int rank = RANK_8;
    int file = FILE_A;

    for (const char token : board_field) {
        if (token == '/') {
            if (file != FILE_NB || rank == RANK_1)
                return std::unexpected("invalid FEN board row");
            --rank;
            file = FILE_A;
            continue;
        }

        if (token >= '1' && token <= '8') {
            file += token - '0';
            if (file > FILE_NB)
                return std::unexpected("too many squares in FEN row");
            continue;
        }

        const Piece piece = piece_from_char(token);
        if (piece == NO_PIECE || file >= FILE_NB || rank < RANK_1)
            return std::unexpected("invalid piece placement in FEN");

        position.put_piece(piece, make_square(File(file), Rank(rank)));
        ++file;
    }

    if (rank != RANK_1 || file != FILE_NB)
        return std::unexpected("FEN board does not contain eight complete rows");

    if (side_field == "w")
        position.side_to_move_ = WHITE;
    else if (side_field == "b")
        position.side_to_move_ = BLACK;
    else
        return std::unexpected("invalid side-to-move field");

    if (castling_field != "-") {
        for (const char right : castling_field) {
            switch (right) {
            case 'K': position.castling_rights_ |= WHITE_KING_SIDE; break;
            case 'Q': position.castling_rights_ |= WHITE_QUEEN_SIDE; break;
            case 'k': position.castling_rights_ |= BLACK_KING_SIDE; break;
            case 'q': position.castling_rights_ |= BLACK_QUEEN_SIDE; break;
            default: return std::unexpected("invalid castling rights field");
            }
        }
    }

    if (ep_field != "-") {
        if (ep_field.size() != 2
            || ep_field[0] < 'a' || ep_field[0] > 'h'
            || (ep_field[1] != '3' && ep_field[1] != '6')) {
            return std::unexpected("invalid en-passant field");
        }

        position.ep_square_ = make_square(
            File(ep_field[0] - 'a'),
            Rank(ep_field[1] - '1')
        );
    }

    unsigned halfmove = 0;
    unsigned fullmove = 0;
    if (!parse_integer(halfmove_field, halfmove) || halfmove > UINT16_MAX)
        return std::unexpected("invalid halfmove clock");
    if (!parse_integer(fullmove_field, fullmove) || fullmove > UINT16_MAX)
        return std::unexpected("invalid fullmove number");

    // Stockfish also accepts the common non-standard fullmove value zero.
    fullmove = std::max(fullmove, 1U);

    position.halfmove_clock_ = std::uint16_t(halfmove);
    position.fullmove_number_ = std::uint16_t(fullmove);

    if (popcount(position.pieces(WHITE, KING)) != 1
        || popcount(position.pieces(BLACK, KING)) != 1) {
        return std::unexpected("FEN must contain exactly one king per color");
    }

    position.key_ = position.compute_key();

    if (!position.is_consistent())
        return std::unexpected("inconsistent FEN position");

    return position;
}

std::string Position::fen() const {
    assert(is_consistent());
    std::string result;

    for (int rank = RANK_8; rank >= RANK_1; --rank) {
        int empty = 0;
        for (int file = FILE_A; file <= FILE_H; ++file) {
            const Piece piece = piece_on(make_square(File(file), Rank(rank)));
            if (piece == NO_PIECE) {
                ++empty;
                continue;
            }

            if (empty != 0) {
                result += char('0' + empty);
                empty = 0;
            }
            result += char_from_piece(piece);
        }

        if (empty != 0)
            result += char('0' + empty);
        if (rank != RANK_1)
            result += '/';
    }

    result += side_to_move_ == WHITE ? " w " : " b ";

    if (castling_rights_ == NO_CASTLING) {
        result += '-';
    } else {
        if (can_castle(WHITE_KING_SIDE))  result += 'K';
        if (can_castle(WHITE_QUEEN_SIDE)) result += 'Q';
        if (can_castle(BLACK_KING_SIDE))  result += 'k';
        if (can_castle(BLACK_QUEEN_SIDE)) result += 'q';
    }

    result += ' ';
    if (ep_square_ == SQ_NONE) {
        result += '-';
    } else {
        result += char('a' + file_of(ep_square_));
        result += char('1' + rank_of(ep_square_));
    }

    result += ' ' + std::to_string(halfmove_clock_);
    result += ' ' + std::to_string(fullmove_number_);
    return result;
}

Piece Position::piece_on(Square square) const noexcept {
    assert(is_ok(square));
    return board_[square];
}

Bitboard Position::pieces(Color color) const noexcept {
    assert(is_ok(color));
    return color_bitboards_[color];
}

Bitboard Position::pieces(PieceType type) const noexcept {
    assert(is_ok(type));
    return piece_bitboards_[make_piece(WHITE, type)]
         | piece_bitboards_[make_piece(BLACK, type)];
}

Bitboard Position::pieces(Color color, PieceType type) const noexcept {
    assert(is_ok(color) && is_ok(type));
    return piece_bitboards_[make_piece(color, type)];
}

bool Position::can_castle(CastlingRights rights) const noexcept {
    return (castling_rights_ & rights) != NO_CASTLING;
}

Square Position::king_square(Color color) const noexcept {
    const Bitboard king = pieces(color, KING);
    assert(has_single_bit(king));
    return lsb(king);
}

Bitboard Position::attackers_to(
    Square square,
    Color by_color,
    Bitboard occupied
) const noexcept {
    assert(is_ok(square) && is_ok(by_color));
    const SliderAttacks sliders = slider_attacks(square, occupied);

    return (pawn_attacks(~by_color, square) & pieces(by_color, PAWN))
         | (knight_attacks(square) & pieces(by_color, KNIGHT))
         | (sliders.bishop & (pieces(by_color, BISHOP) | pieces(by_color, QUEEN)))
         | (sliders.rook & (pieces(by_color, ROOK) | pieces(by_color, QUEEN)))
         | (king_attacks(square) & pieces(by_color, KING));
}

void Position::do_move(Move move, StateInfo& state) noexcept {
    assert(!move.is_none() && is_consistent());
    const Square from = move.from();
    const Square to = move.to();
    const Color us = side_to_move_;
    const Piece moving_piece = piece_on(from);
    assert(is_ok(moving_piece) && color_of(moving_piece) == us);

    state.castling_rights = castling_rights_;
    state.key = key_;
    state.ep_square = ep_square_;
    state.halfmove_clock = halfmove_clock_;
    state.fullmove_number = fullmove_number_;
    state.captured_piece = NO_PIECE;
    state.captured_square = SQ_NONE;

    if (ep_square_ != SQ_NONE)
        key_ ^= zobrist::en_passant(file_of(ep_square_));
    ep_square_ = SQ_NONE;
    ++halfmove_clock_;

    if (move.type() == EN_PASSANT) {
        state.captured_square = to - pawn_push(us);
        state.captured_piece = piece_on(state.captured_square);
        assert(state.captured_piece == make_piece(~us, PAWN));
        remove_piece(state.captured_square);
    } else if (move.type() != CASTLING && piece_on(to) != NO_PIECE) {
        state.captured_square = to;
        state.captured_piece = piece_on(to);
        assert(color_of(state.captured_piece) == ~us);
        remove_piece(to);
    }

    const CastlingRights old_castling_rights = castling_rights_;
    if (type_of(moving_piece) == KING)
        clear_castling_right(us == WHITE ? WHITE_CASTLING : BLACK_CASTLING);

    if (from == A1 || to == A1) clear_castling_right(WHITE_QUEEN_SIDE);
    if (from == H1 || to == H1) clear_castling_right(WHITE_KING_SIDE);
    if (from == A8 || to == A8) clear_castling_right(BLACK_QUEEN_SIDE);
    if (from == H8 || to == H8) clear_castling_right(BLACK_KING_SIDE);

    if (castling_rights_ != old_castling_rights) {
        key_ ^= zobrist::castling(old_castling_rights);
        key_ ^= zobrist::castling(castling_rights_);
    }

    switch (move.type()) {
    case NORMAL:
        move_piece(from, to);
        break;

    case PROMOTION:
        assert(type_of(moving_piece) == PAWN);
        remove_piece(from);
        put_piece(make_piece(us, move.promotion_type()), to);
        break;

    case EN_PASSANT:
        assert(type_of(moving_piece) == PAWN);
        move_piece(from, to);
        break;

    case CASTLING: {
        assert(type_of(moving_piece) == KING);
        const bool king_side = file_of(to) == FILE_G;
        const Square rook_from = relative_square_unchecked(us, king_side ? H1 : A1);
        const Square rook_to = relative_square_unchecked(us, king_side ? F1 : D1);
        assert(piece_on(rook_from) == make_piece(us, ROOK));
        move_piece(from, to);
        move_piece(rook_from, rook_to);
        break;
    }
    }

    if (type_of(moving_piece) == PAWN) {
        halfmove_clock_ = 0;
        if (int(to) - int(from) == 16 || int(from) - int(to) == 16) {
            ep_square_ = from + pawn_push(us);
            key_ ^= zobrist::en_passant(file_of(ep_square_));
        }
    }

    if (state.captured_piece != NO_PIECE)
        halfmove_clock_ = 0;

    if (us == BLACK)
        ++fullmove_number_;
    side_to_move_ = ~us;
    key_ ^= zobrist::side();
    assert(is_consistent());
}

void Position::undo_move(Move move, const StateInfo& state) noexcept {
    assert(!move.is_none() && is_consistent());
    side_to_move_ = ~side_to_move_;
    const Color us = side_to_move_;
    const Square from = move.from();
    const Square to = move.to();

    const auto put_piece_unkeyed = [this](Piece piece, Square square) noexcept {
        assert(is_ok(piece) && is_ok(square) && board_[square] == NO_PIECE);
        const Bitboard bit = square_bb(square);
        board_[square] = piece;
        piece_bitboards_[piece] |= bit;
        color_bitboards_[color_of(piece)] |= bit;
        occupied_ |= bit;
    };

    const auto remove_piece_unkeyed = [this](Square square) noexcept {
        assert(is_ok(square));
        const Piece piece = board_[square];
        assert(is_ok(piece));
        const Bitboard bit = square_bb(square);
        board_[square] = NO_PIECE;
        piece_bitboards_[piece] &= ~bit;
        color_bitboards_[color_of(piece)] &= ~bit;
        occupied_ &= ~bit;
    };

    const auto move_piece_unkeyed = [this](Square source, Square destination) noexcept {
        assert(is_ok(source) && is_ok(destination)
               && board_[source] != NO_PIECE && board_[destination] == NO_PIECE);
        const Piece piece = board_[source];
        const Bitboard move_mask = square_bb(source) | square_bb(destination);
        board_[source] = NO_PIECE;
        board_[destination] = piece;
        piece_bitboards_[piece] ^= move_mask;
        color_bitboards_[color_of(piece)] ^= move_mask;
        occupied_ ^= move_mask;
    };

    switch (move.type()) {
    case NORMAL:
        move_piece_unkeyed(to, from);
        break;

    case PROMOTION:
        remove_piece_unkeyed(to);
        put_piece_unkeyed(make_piece(us, PAWN), from);
        break;

    case EN_PASSANT:
        move_piece_unkeyed(to, from);
        break;

    case CASTLING: {
        const bool king_side = file_of(to) == FILE_G;
        const Square rook_from = relative_square_unchecked(us, king_side ? H1 : A1);
        const Square rook_to = relative_square_unchecked(us, king_side ? F1 : D1);
        move_piece_unkeyed(to, from);
        move_piece_unkeyed(rook_to, rook_from);
        break;
    }
    }

    if (state.captured_piece != NO_PIECE)
        put_piece_unkeyed(state.captured_piece, state.captured_square);

    castling_rights_ = state.castling_rights;
    ep_square_ = state.ep_square;
    halfmove_clock_ = state.halfmove_clock;
    fullmove_number_ = state.fullmove_number;
    key_ = state.key;
    assert(is_consistent());
}

void Position::do_null_move(StateInfo& state) noexcept {
    assert(is_consistent());
    state.castling_rights = castling_rights_;
    state.key = key_;
    state.ep_square = ep_square_;
    state.halfmove_clock = halfmove_clock_;
    state.fullmove_number = fullmove_number_;
    state.captured_piece = NO_PIECE;
    state.captured_square = SQ_NONE;

    if (ep_square_ != SQ_NONE)
        key_ ^= zobrist::en_passant(file_of(ep_square_));
    ep_square_ = SQ_NONE;
    ++halfmove_clock_;
    if (side_to_move_ == BLACK)
        ++fullmove_number_;
    side_to_move_ = ~side_to_move_;
    key_ ^= zobrist::side();
    assert(is_consistent());
}

void Position::undo_null_move(const StateInfo& state) noexcept {
    assert(is_consistent());
    side_to_move_ = ~side_to_move_;
    castling_rights_ = state.castling_rights;
    ep_square_ = state.ep_square;
    halfmove_clock_ = state.halfmove_clock;
    fullmove_number_ = state.fullmove_number;
    key_ = state.key;
    assert(is_consistent());
}

Key Position::compute_key() const noexcept {
    Key result = zobrist::castling(castling_rights_);

    for (int index = 0; index < SQUARE_NB; ++index) {
        const Piece piece = board_[index];
        if (piece != NO_PIECE)
            result ^= zobrist::piece_square(piece, Square(index));
    }

    if (side_to_move_ == BLACK)
        result ^= zobrist::side();
    if (ep_square_ != SQ_NONE)
        result ^= zobrist::en_passant(file_of(ep_square_));

    return result;
}

bool Position::is_consistent() const noexcept {
    if (!is_ok(side_to_move_))
        return false;
    if (ep_square_ != SQ_NONE && !is_ok(ep_square_))
        return false;

    std::array<Bitboard, PIECE_NB> expected_pieces{};
    std::array<Bitboard, COLOR_NB> expected_colors{};
    Bitboard expected_occupied = EMPTY_BB;

    for (int index = 0; index < SQUARE_NB; ++index) {
        const Piece piece = board_[index];
        if (piece == NO_PIECE)
            continue;
        if (!is_ok(piece))
            return false;

        const Bitboard square = square_bb(Square(index));
        expected_pieces[piece] |= square;
        expected_colors[color_of(piece)] |= square;
        expected_occupied |= square;
    }

    return expected_pieces == piece_bitboards_
        && expected_colors == color_bitboards_
        && expected_occupied == occupied_
        && key_ == compute_key()
        && (color_bitboards_[WHITE] & color_bitboards_[BLACK]) == EMPTY_BB
        && popcount(pieces(WHITE, KING)) == 1
        && popcount(pieces(BLACK, KING)) == 1;
}

void Position::put_piece(Piece piece, Square square) noexcept {
    assert(is_ok(piece) && is_ok(square) && board_[square] == NO_PIECE);
    const Bitboard bit = square_bb(square);
    board_[square] = piece;
    piece_bitboards_[piece] |= bit;
    color_bitboards_[color_of(piece)] |= bit;
    occupied_ |= bit;
    key_ ^= zobrist::piece_square(piece, square);
}

void Position::remove_piece(Square square) noexcept {
    assert(is_ok(square));
    const Piece piece = board_[square];
    assert(is_ok(piece));
    const Bitboard bit = square_bb(square);
    board_[square] = NO_PIECE;
    piece_bitboards_[piece] &= ~bit;
    color_bitboards_[color_of(piece)] &= ~bit;
    occupied_ &= ~bit;
    key_ ^= zobrist::piece_square(piece, square);
}

void Position::move_piece(Square from, Square to) noexcept {
    assert(is_ok(from) && is_ok(to) && board_[from] != NO_PIECE && board_[to] == NO_PIECE);
    const Piece piece = board_[from];
    const Bitboard move_mask = square_bb(from) | square_bb(to);
    board_[from] = NO_PIECE;
    board_[to] = piece;
    piece_bitboards_[piece] ^= move_mask;
    color_bitboards_[color_of(piece)] ^= move_mask;
    occupied_ ^= move_mask;
    key_ ^= zobrist::piece_square(piece, from);
    key_ ^= zobrist::piece_square(piece, to);
}

void Position::clear_castling_right(CastlingRights right) noexcept {
    castling_rights_ = CastlingRights(
        std::uint8_t(castling_rights_) & ~std::uint8_t(right)
    );
}

} // namespace mors
