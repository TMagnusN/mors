// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "position.hpp"
#include "zobrist.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
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

Bitboard rank_segment(Square first, Square second) noexcept {
    assert(is_ok(first) && is_ok(second) && rank_of(first) == rank_of(second));
    const int begin = std::min(int(file_of(first)), int(file_of(second)));
    const int end = std::max(int(file_of(first)), int(file_of(second)));
    Bitboard result = EMPTY_BB;
    for (int file = begin; file <= end; ++file)
        result |= square_bb(make_square(File(file), rank_of(first)));
    return result;
}

bool valid_en_passant_square(
    const Position& position,
    Square ep_square,
    Color capturing_color
) noexcept {
    if (!is_ok(ep_square) || !is_ok(capturing_color))
        return false;

    const Rank expected_rank = capturing_color == WHITE ? RANK_6 : RANK_3;
    if (rank_of(ep_square) != expected_rank)
        return false;

    const Square captured_square = ep_square - pawn_push(capturing_color);
    const Square source_square = ep_square + pawn_push(capturing_color);
    return is_ok(captured_square)
        && is_ok(source_square)
        && position.piece_on(ep_square) == NO_PIECE
        && position.piece_on(source_square) == NO_PIECE
        && position.piece_on(captured_square)
            == make_piece(~capturing_color, PAWN)
        && (pawn_attacks(~capturing_color, ep_square)
            & position.pieces(capturing_color, PAWN)) != EMPTY_BB;
}

} // namespace

std::expected<Position, std::string> Position::from_fen(
    std::string_view fen_text,
    bool chess960
) {
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
    position.chess960_ = chess960;
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

    if (popcount(position.pieces(WHITE, KING)) != 1
        || popcount(position.pieces(BLACK, KING)) != 1) {
        return std::unexpected("FEN must contain exactly one king per color");
    }

    if (castling_field != "-") {
        if (castling_field.contains('-'))
            return std::unexpected("invalid castling rights field");

        for (const char token : castling_field) {
            const bool black = std::islower(static_cast<unsigned char>(token)) != 0;
            const Color color = black ? BLACK : WHITE;
            const char upper = char(std::toupper(static_cast<unsigned char>(token)));
            const Square king_from = position.king_square(color);
            const Rank home_rank = color == WHITE ? RANK_1 : RANK_8;
            if (rank_of(king_from) != home_rank)
                return std::unexpected("castling king is not on its home rank");

            Square rook_from = SQ_NONE;
            if (upper == 'K') {
                for (int rook_file = FILE_H;
                     rook_file > int(file_of(king_from));
                     --rook_file) {
                    const Square candidate = make_square(File(rook_file), home_rank);
                    if (position.piece_on(candidate) == make_piece(color, ROOK)) {
                        rook_from = candidate;
                        break;
                    }
                }
            } else if (upper == 'Q') {
                for (int rook_file = FILE_A;
                     rook_file < int(file_of(king_from));
                     ++rook_file) {
                    const Square candidate = make_square(File(rook_file), home_rank);
                    if (position.piece_on(candidate) == make_piece(color, ROOK)) {
                        rook_from = candidate;
                        break;
                    }
                }
            } else if (upper >= 'A' && upper <= 'H') {
                rook_from = make_square(File(upper - 'A'), home_rank);
            } else {
                return std::unexpected("invalid castling rights field");
            }

            if (!is_ok(rook_from) || !position.set_castling_right(color, rook_from))
                return std::unexpected("invalid or duplicate castling right");
        }
    }

    if (ep_field != "-") {
        if (ep_field.size() != 2
            || ep_field[0] < 'a' || ep_field[0] > 'h'
            || (ep_field[1] != '3' && ep_field[1] != '6')) {
            return std::unexpected("invalid en-passant field");
        }

        const Square candidate = make_square(
            File(ep_field[0] - 'a'),
            Rank(ep_field[1] - '1')
        );
        if (valid_en_passant_square(
                position,
                candidate,
                position.side_to_move_)) {
            position.ep_square_ = candidate;
        }
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
        const auto append_right = [this, &result](
            CastlingRights right,
            char classical
        ) {
            if (!can_castle(right))
                return;
            if (!chess960_) {
                result += classical;
                return;
            }
            const char file = char('A' + file_of(castling_rook_square(right)));
            result += right == BLACK_KING_SIDE || right == BLACK_QUEEN_SIDE
                ? char(std::tolower(static_cast<unsigned char>(file)))
                : file;
        };
        append_right(WHITE_KING_SIDE, 'K');
        append_right(WHITE_QUEEN_SIDE, 'Q');
        append_right(BLACK_KING_SIDE, 'k');
        append_right(BLACK_QUEEN_SIDE, 'q');
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

Square Position::castling_rook_square(CastlingRights right) const noexcept {
    assert(right == WHITE_KING_SIDE || right == WHITE_QUEEN_SIDE
        || right == BLACK_KING_SIDE || right == BLACK_QUEEN_SIDE);
    return castling_rook_squares_[std::uint8_t(right)];
}

Bitboard Position::castling_path(CastlingRights right) const noexcept {
    assert(right == WHITE_KING_SIDE || right == WHITE_QUEEN_SIDE
        || right == BLACK_KING_SIDE || right == BLACK_QUEEN_SIDE);
    return castling_paths_[std::uint8_t(right)];
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

    clear_castling_rights_by_square(from);
    clear_castling_rights_by_square(to);

    if (castling_rights_ != old_castling_rights) {
        key_ ^= castling_key(old_castling_rights);
        key_ ^= castling_key(castling_rights_);
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
        const Square rook_from = to;
        const bool king_side = file_of(rook_from) > file_of(from);
        const Square king_to = castling_king_to(us, king_side);
        const Square rook_to = castling_rook_to(us, king_side);
        assert(piece_on(rook_from) == make_piece(us, ROOK));
        remove_piece(from);
        remove_piece(rook_from);
        put_piece(make_piece(us, KING), king_to);
        put_piece(make_piece(us, ROOK), rook_to);
        break;
    }
    }

    if (type_of(moving_piece) == PAWN) {
        halfmove_clock_ = 0;
        if (int(to) - int(from) == 16 || int(from) - int(to) == 16) {
            const Square candidate = from + pawn_push(us);
            if (valid_en_passant_square(*this, candidate, ~us)) {
                ep_square_ = candidate;
                key_ ^= zobrist::en_passant(file_of(ep_square_));
            }
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
        const Square rook_from = to;
        const bool king_side = file_of(rook_from) > file_of(from);
        const Square king_to = castling_king_to(us, king_side);
        const Square rook_to = castling_rook_to(us, king_side);
        remove_piece_unkeyed(king_to);
        remove_piece_unkeyed(rook_to);
        put_piece_unkeyed(make_piece(us, KING), from);
        put_piece_unkeyed(make_piece(us, ROOK), rook_from);
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
    Key result = castling_key(castling_rights_);

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

    for (const CastlingRights right : {
             WHITE_KING_SIDE, WHITE_QUEEN_SIDE,
             BLACK_KING_SIDE, BLACK_QUEEN_SIDE}) {
        if (!can_castle(right))
            continue;
        const Color color =
            right == WHITE_KING_SIDE || right == WHITE_QUEEN_SIDE
            ? WHITE : BLACK;
        const Square rook = castling_rook_square(right);
        const Square king = king_square(color);
        if (!is_ok(rook)
            || piece_on(rook) != make_piece(color, ROOK)
            || (castling_rights_mask_[king] & right) == NO_CASTLING
            || (castling_rights_mask_[rook] & right) == NO_CASTLING) {
            return false;
        }
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

bool Position::set_castling_right(Color color, Square rook_from) noexcept {
    if (!is_ok(color) || !is_ok(rook_from)
        || popcount(pieces(color, KING)) != 1) {
        return false;
    }

    const Square king_from = king_square(color);
    const Rank home_rank = color == WHITE ? RANK_1 : RANK_8;
    if (rank_of(king_from) != home_rank
        || rank_of(rook_from) != home_rank
        || king_from == rook_from
        || piece_on(rook_from) != make_piece(color, ROOK)) {
        return false;
    }

    const bool king_side = file_of(rook_from) > file_of(king_from);
    const CastlingRights right = castling_right(color, king_side);
    if (can_castle(right))
        return false;

    const Square king_to = castling_king_to(color, king_side);
    const Square rook_to = castling_rook_to(color, king_side);
    castling_rights_ |= right;
    castling_rights_mask_[king_from] |= right;
    castling_rights_mask_[rook_from] |= right;
    castling_rook_squares_[std::uint8_t(right)] = rook_from;
    castling_paths_[std::uint8_t(right)] =
        (rank_segment(king_from, king_to) | rank_segment(rook_from, rook_to))
        & ~(square_bb(king_from) | square_bb(rook_from));
    return true;
}

Key Position::castling_key(CastlingRights rights) const noexcept {
    Key result = zobrist::castling(rights);
    for (const CastlingRights right : {
             WHITE_KING_SIDE, WHITE_QUEEN_SIDE,
             BLACK_KING_SIDE, BLACK_QUEEN_SIDE}) {
        if ((rights & right) == NO_CASTLING)
            continue;
        const Square rook = castling_rook_squares_[std::uint8_t(right)];
        assert(is_ok(rook));
        result ^= zobrist::castling_rook(right, file_of(rook));
    }
    return result;
}

void Position::clear_castling_right(CastlingRights right) noexcept {
    castling_rights_ = CastlingRights(
        std::uint8_t(castling_rights_) & ~std::uint8_t(right)
    );
}

void Position::clear_castling_rights_by_square(Square square) noexcept {
    assert(is_ok(square));
    castling_rights_ = CastlingRights(
        std::uint8_t(castling_rights_)
        & ~std::uint8_t(castling_rights_mask_[square])
    );
}

} // namespace mors
