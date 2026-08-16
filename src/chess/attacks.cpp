// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "attacks.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace mros {

namespace detail {

#if defined(MROS_MAGIC)
std::array<Bitboard, BISHOP_ATTACK_TABLE_SIZE> bishop_magic_attacks{};
std::array<Bitboard, ROOK_ATTACK_TABLE_SIZE> rook_magic_attacks{};
#endif

} // namespace detail

namespace {

using AttackTable = std::array<Bitboard, SQUARE_NB>;
using GeometryTable = std::array<std::array<Bitboard, SQUARE_NB>, SQUARE_NB>;

std::array<AttackTable, COLOR_NB> pawn_attacks_table{};
AttackTable knight_attacks_table{};
AttackTable king_attacks_table{};
GeometryTable line_table{};
GeometryTable between_table{};

std::once_flag initialization_flag;
bool attacks_initialized = false;

constexpr bool inside(int file, int rank) noexcept {
    return file >= FILE_A && file <= FILE_H && rank >= RANK_1 && rank <= RANK_8;
}

constexpr Bitboard bit_at(int file, int rank) noexcept {
    return Bitboard{1} << (rank * 8 + file);
}

template<std::size_t N>
Bitboard leaper_attacks(Square square, const std::array<std::array<int, 2>, N>& offsets) noexcept {
    const int source_file = file_of(square);
    const int source_rank = rank_of(square);
    Bitboard result = EMPTY_BB;

    for (const auto& offset : offsets) {
        const int file = source_file + offset[0];
        const int rank = source_rank + offset[1];
        if (inside(file, rank))
            result |= bit_at(file, rank);
    }

    return result;
}

Bitboard ray_attacks(
    Square square,
    Bitboard occupied,
    const std::array<std::array<int, 2>, 4>& directions
) noexcept {
    const int source_file = file_of(square);
    const int source_rank = rank_of(square);
    Bitboard result = EMPTY_BB;

    for (const auto& direction : directions) {
        int file = source_file + direction[0];
        int rank = source_rank + direction[1];

        while (inside(file, rank)) {
            const Bitboard destination = bit_at(file, rank);
            result |= destination;
            if (occupied & destination)
                break;
            file += direction[0];
            rank += direction[1];
        }
    }

    return result;
}

constexpr std::array<std::array<int, 2>, 4> BISHOP_DIRECTIONS = {{
    {{1, 1}}, {{-1, 1}}, {{1, -1}}, {{-1, -1}}
}};

constexpr std::array<std::array<int, 2>, 4> ROOK_DIRECTIONS = {{
    {{0, 1}}, {{1, 0}}, {{0, -1}}, {{-1, 0}}
}};

constexpr Bitboard directional_line(Square square, int file_delta, int rank_delta) noexcept {
    Bitboard result = EMPTY_BB;
    int file = int(file_of(square)) + file_delta;
    int rank = int(rank_of(square)) + rank_delta;

    while (inside(file, rank)) {
        result |= bit_at(file, rank);
        file += file_delta;
        rank += rank_delta;
    }

    return result;
}

[[maybe_unused]] constexpr Bitboard complete_line(
    Square square,
    int first_file_delta,
    int first_rank_delta,
    int second_file_delta,
    int second_rank_delta
) noexcept {
    return directional_line(square, first_file_delta, first_rank_delta)
         | directional_line(square, second_file_delta, second_rank_delta);
}

#if defined(MROS_DUAL_HQ)

inline constexpr auto rank_attacks_table = []() constexpr {
    std::array<std::array<std::uint8_t, 64>, FILE_NB> table{};

    for (int source_file = FILE_A; source_file <= FILE_H; ++source_file) {
        for (unsigned inner_occupancy = 0; inner_occupancy < 64; ++inner_occupancy) {
            const unsigned occupied = inner_occupancy << 1;
            unsigned result = 0;

            for (int file = source_file - 1; file >= FILE_A; --file) {
                result |= 1U << file;
                if (occupied & (1U << file))
                    break;
            }

            for (int file = source_file + 1; file <= FILE_H; ++file) {
                result |= 1U << file;
                if (occupied & (1U << file))
                    break;
            }

            table[source_file][inner_occupancy] = std::uint8_t(result);
        }
    }

    return table;
}();

void initialize_slider_backend() {}

#else

template<std::size_t N>
void initialize_magic_table(
    const std::array<detail::GeneratedMagic, SQUARE_NB>& magics,
    std::array<Bitboard, N>& table,
    bool bishop
) {
    for (int index = 0; index < SQUARE_NB; ++index) {
        const Square square = Square(index);
        const detail::GeneratedMagic& magic = magics[index];
        Bitboard subset = 0;

        do {
            table[detail::magic_index(magic, subset)] = bishop
                ? detail::reference_bishop_attacks(square, subset)
                : detail::reference_rook_attacks(square, subset);
            subset = (subset - magic.mask) & magic.mask;
        } while (subset != 0);
    }
}

void initialize_slider_backend() {
    initialize_magic_table(
        detail::BISHOP_MAGICS,
        detail::bishop_magic_attacks,
        true
    );
    initialize_magic_table(
        detail::ROOK_MAGICS,
        detail::rook_magic_attacks,
        false
    );
}

#endif

void initialize_geometry() {
    constexpr std::array<std::array<int, 2>, 8> KNIGHT_OFFSETS = {{
        {{1, 2}}, {{2, 1}}, {{2, -1}}, {{1, -2}},
        {{-1, -2}}, {{-2, -1}}, {{-2, 1}}, {{-1, 2}}
    }};
    constexpr std::array<std::array<int, 2>, 8> KING_OFFSETS = {{
        {{0, 1}}, {{1, 1}}, {{1, 0}}, {{1, -1}},
        {{0, -1}}, {{-1, -1}}, {{-1, 0}}, {{-1, 1}}
    }};

    for (int index = 0; index < SQUARE_NB; ++index) {
        const Square square = Square(index);
        pawn_attacks_table[WHITE][index] = mros::pawn_attacks<WHITE>(square_bb(square));
        pawn_attacks_table[BLACK][index] = mros::pawn_attacks<BLACK>(square_bb(square));
        knight_attacks_table[index] = leaper_attacks(square, KNIGHT_OFFSETS);
        king_attacks_table[index] = leaper_attacks(square, KING_OFFSETS);
    }

    for (int first_index = 0; first_index < SQUARE_NB; ++first_index) {
        const Square first = Square(first_index);
        const Bitboard first_bishop = detail::reference_bishop_attacks(first, EMPTY_BB);
        const Bitboard first_rook = detail::reference_rook_attacks(first, EMPTY_BB);

        for (int second_index = 0; second_index < SQUARE_NB; ++second_index) {
            const Square second = Square(second_index);
            const bool bishop_aligned = contains(first_bishop, second);
            const bool rook_aligned = contains(first_rook, second);

            if (!bishop_aligned && !rook_aligned)
                continue;

            const auto reference = bishop_aligned
                ? detail::reference_bishop_attacks
                : detail::reference_rook_attacks;

            line_table[first_index][second_index] =
                (reference(first, EMPTY_BB) & reference(second, EMPTY_BB))
                | square_bb(first) | square_bb(second);

            between_table[first_index][second_index] =
                reference(first, square_bb(second))
                & reference(second, square_bb(first));
        }
    }
}

} // namespace

#if defined(MROS_DUAL_HQ)

const std::array<detail::DualHqEntry, SQUARE_NB> detail::dual_hq_entries = []() constexpr {
    std::array<detail::DualHqEntry, SQUARE_NB> entries{};

    for (int index = 0; index < SQUARE_NB; ++index) {
        const Square square = Square(index);
        detail::DualHqEntry& entry = entries[index];
        entry.masks[0] = file_bb(square) ^ square_bb(square);
        entry.masks[1] = complete_line(square, 1, 1, -1, -1);
        entry.masks[2] = EMPTY_BB;
        entry.masks[3] = complete_line(square, -1, 1, 1, -1);
        entry.origin = square_bb(square);
        entry.reversed_origin = std::byteswap(entry.origin);
        entry.rank_attacks_lookup = rank_attacks_table[file_of(square)].data();
        entry.rank_shift = 8U * unsigned(rank_of(square));
    }

    return entries;
}();

#endif

namespace detail {

bool attacks_ready() noexcept { return attacks_initialized; }

Bitboard reference_bishop_attacks(Square square, Bitboard occupied) noexcept {
    assert(is_ok(square));
    return ray_attacks(square, occupied, BISHOP_DIRECTIONS);
}

Bitboard reference_rook_attacks(Square square, Bitboard occupied) noexcept {
    assert(is_ok(square));
    return ray_attacks(square, occupied, ROOK_DIRECTIONS);
}

} // namespace detail

void initialize_attacks() {
    std::call_once(initialization_flag, [] {
        initialize_geometry();
        initialize_slider_backend();
        attacks_initialized = true;
    });
}

Bitboard pawn_attacks(Color color, Square square) noexcept {
    assert(attacks_initialized && is_ok(color) && is_ok(square));
    return pawn_attacks_table[color][square];
}

Bitboard knight_attacks(Square square) noexcept {
    assert(attacks_initialized && is_ok(square));
    return knight_attacks_table[square];
}

Bitboard king_attacks(Square square) noexcept {
    assert(attacks_initialized && is_ok(square));
    return king_attacks_table[square];
}

Bitboard attacks(Piece piece, Square square, Bitboard occupied) noexcept {
    assert(is_ok(piece) && is_ok(square));
    switch (type_of(piece)) {
    case PAWN:   return pawn_attacks(color_of(piece), square);
    case KNIGHT: return knight_attacks(square);
    case BISHOP: return bishop_attacks(square, occupied);
    case ROOK:   return rook_attacks(square, occupied);
    case QUEEN:  return queen_attacks(square, occupied);
    case KING:   return king_attacks(square);
    default:     return EMPTY_BB;
    }
}

Bitboard line_bb(Square first, Square second) noexcept {
    assert(attacks_initialized && is_ok(first) && is_ok(second));
    return line_table[first][second];
}

Bitboard between_bb(Square first, Square second) noexcept {
    assert(attacks_initialized && is_ok(first) && is_ok(second));
    return between_table[first][second];
}

} // namespace mros
