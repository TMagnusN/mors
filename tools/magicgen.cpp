// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <array>
#include <bit>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

using U64 = std::uint64_t;

struct GeneratedMagic {
    U64 mask;
    U64 magic;
    std::uint32_t offset;
    std::uint8_t shift;
};

class SplitMix64 final {
public:
    explicit SplitMix64(U64 state) : state_(state) {}

    U64 next() {
        U64 value = (state_ += 0x9E3779B97F4A7C15ULL);
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31);
    }

    U64 sparse() { return next() & next() & next(); }

private:
    U64 state_;
};

constexpr bool inside(int file, int rank) {
    return file >= 0 && file < 8 && rank >= 0 && rank < 8;
}

constexpr U64 bit(int file, int rank) {
    return U64{1} << (rank * 8 + file);
}

U64 relevant_mask(int square, bool bishop) {
    constexpr int bishop_dirs[4][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    constexpr int rook_dirs[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
    const auto& dirs = bishop ? bishop_dirs : rook_dirs;

    const int source_file = square & 7;
    const int source_rank = square >> 3;
    U64 mask = 0;

    for (const auto& direction : dirs) {
        int file = source_file + direction[0];
        int rank = source_rank + direction[1];

        while (inside(file, rank)) {
            const int next_file = file + direction[0];
            const int next_rank = rank + direction[1];
            if (!inside(next_file, next_rank))
                break;

            mask |= bit(file, rank);
            file = next_file;
            rank = next_rank;
        }
    }

    return mask;
}

U64 reference_attacks(int square, U64 occupied, bool bishop) {
    constexpr int bishop_dirs[4][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    constexpr int rook_dirs[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
    const auto& dirs = bishop ? bishop_dirs : rook_dirs;

    const int source_file = square & 7;
    const int source_rank = square >> 3;
    U64 attacks = 0;

    for (const auto& direction : dirs) {
        int file = source_file + direction[0];
        int rank = source_rank + direction[1];

        while (inside(file, rank)) {
            const U64 destination = bit(file, rank);
            attacks |= destination;
            if (occupied & destination)
                break;
            file += direction[0];
            rank += direction[1];
        }
    }

    return attacks;
}

GeneratedMagic find_magic(int square, bool bishop, std::uint32_t offset, SplitMix64& rng) {
    const U64 mask = relevant_mask(square, bishop);
    const int bits = std::popcount(mask);
    const std::size_t size = std::size_t{1} << bits;
    const std::uint8_t shift = std::uint8_t(64 - bits);

    std::vector<U64> occupancies;
    std::vector<U64> references;
    occupancies.reserve(size);
    references.reserve(size);

    U64 subset = 0;
    do {
        occupancies.push_back(subset);
        references.push_back(reference_attacks(square, subset, bishop));
        subset = (subset - mask) & mask;
    } while (subset != 0);

    std::vector<U64> table(size);
    std::vector<std::uint32_t> epoch(size);
    std::uint32_t generation = 0;

    for (;;) {
        const U64 magic = rng.sparse();
        if (std::popcount((mask * magic) & 0xFF00000000000000ULL) < 6)
            continue;

        ++generation;
        bool valid = true;

        for (std::size_t i = 0; i < size; ++i) {
            const std::size_t index = (occupancies[i] * magic) >> shift;
            if (epoch[index] != generation) {
                epoch[index] = generation;
                table[index] = references[i];
            } else if (table[index] != references[i]) {
                valid = false;
                break;
            }
        }

        if (valid)
            return {mask, magic, offset, shift};
    }
}

void print_array(const char* name, const std::array<GeneratedMagic, 64>& values) {
    std::cout << "inline constexpr std::array<GeneratedMagic, 64> " << name << " = {{\n";
    for (const GeneratedMagic& value : values) {
        std::cout << "    GeneratedMagic{0x" << std::hex << std::setw(16) << std::setfill('0')
                  << value.mask << "ULL, 0x" << std::setw(16) << value.magic << "ULL, "
                  << std::dec << value.offset << "U, " << unsigned(value.shift) << "},\n";
    }
    std::cout << "}};\n\n";
}

} // namespace

int main() {
    SplitMix64 rng(0x4D524F532D4D4147ULL);
    std::array<GeneratedMagic, 64> bishops{};
    std::array<GeneratedMagic, 64> rooks{};
    std::uint32_t bishop_size = 0;
    std::uint32_t rook_size = 0;

    for (int square = 0; square < 64; ++square) {
        bishops[square] = find_magic(square, true, bishop_size, rng);
        bishop_size += std::uint32_t{1} << (64 - bishops[square].shift);
    }

    for (int square = 0; square < 64; ++square) {
        rooks[square] = find_magic(square, false, rook_size, rng);
        rook_size += std::uint32_t{1} << (64 - rooks[square].shift);
    }

    std::cout << "// MROS - generated deterministic magic bitboards\n"
                 "// Generated by tools/magicgen.cpp; do not edit by hand.\n"
                 "// Copyright (C) 2026 Theodore Magnus Øen\n"
                 "// SPDX-License-Identifier: AGPL-3.0-or-later\n\n"
                 "#pragma once\n\n"
                 "#include <array>\n"
                 "#include <cstddef>\n"
                 "#include <cstdint>\n\n"
                 "namespace mros::detail {\n\n"
                 "struct GeneratedMagic final {\n"
                 "    std::uint64_t mask;\n"
                 "    std::uint64_t magic;\n"
                 "    std::uint32_t offset;\n"
                 "    std::uint8_t shift;\n"
                 "};\n\n";

    std::cout << "inline constexpr std::size_t BISHOP_ATTACK_TABLE_SIZE = " << bishop_size << ";\n";
    std::cout << "inline constexpr std::size_t ROOK_ATTACK_TABLE_SIZE = " << rook_size << ";\n\n";
    print_array("BISHOP_MAGICS", bishops);
    print_array("ROOK_MAGICS", rooks);
    std::cout << "} // namespace mros::detail\n";
}
