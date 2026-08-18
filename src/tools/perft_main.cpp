// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "chess/perft.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    using namespace mors;

    initialize_attacks();

    int depth = 5;
    if (argc >= 2) {
        const std::string_view text = argv[1];
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), depth);
        if (error != std::errc{} || end != text.data() + text.size() || depth < 0) {
            std::cerr << "invalid depth\n";
            return 2;
        }
    }

    const std::string_view fen = argc >= 3 ? std::string_view(argv[2]) : START_FEN;
    auto parsed = Position::from_fen(fen);
    if (!parsed) {
        std::cerr << "invalid FEN: " << parsed.error() << '\n';
        return 2;
    }

    Position position = std::move(*parsed);
    const auto started = std::chrono::steady_clock::now();
    const std::uint64_t nodes = perft(position, depth);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    std::cout << "depth " << depth << '\n'
              << "nodes " << nodes << '\n'
              << "time_ms " << milliseconds << '\n';

    if (milliseconds > 0)
        std::cout << "nps " << (nodes * 1000ULL / std::uint64_t(milliseconds)) << '\n';

    return 0;
}
