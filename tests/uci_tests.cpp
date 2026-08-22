// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Oen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "uci.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

bool expect_contains(
    std::string_view output,
    std::string_view expected,
    const char* message
) {
    if (output.contains(expected))
        return true;
    std::cerr << "FAIL UCI: " << message << '\n';
    return false;
}

bool expect_not_contains(
    std::string_view output,
    std::string_view unexpected,
    const char* message
) {
    if (!output.contains(unexpected))
        return true;
    std::cerr << "FAIL UCI: " << message << '\n';
    return false;
}

bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL UCI: " << message << '\n';
    return condition;
}

[[nodiscard]] std::size_t occurrence_count(
    std::string_view text,
    std::string_view needle
) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

[[nodiscard]] std::filesystem::path network_path() {
    for (const std::filesystem::path& candidate : {
             std::filesystem::path("networks/mors-p2h32-s14400M-o3183M-c+frc.mnue"),
             std::filesystem::path("../networks/mors-p2h32-s14400M-o3183M-c+frc.mnue"),
             std::filesystem::path("../../networks/mors-p2h32-s14400M-o3183M-c+frc.mnue")}) {
        if (std::filesystem::exists(candidate))
            return std::filesystem::absolute(candidate);
    }
    return {};
}

} // namespace

bool run_uci_tests() {
    const std::filesystem::path network = network_path();
    if (!expect(!network.empty(), "test network must be available"))
        return false;
    std::ostringstream commands;
    commands << "uci\n"
             << "isready\n"
             << "setoption name Threads value 1\n"
             << "setoption name Hash value 2\n"
             << "setoption name Clear Hash\n"
             << "setoption name EvalFile value " << network.string() << "\n"
             << "setoption name EvalFile value mors-p2h32-s14400M-o3183M-c+frc.mnue\n"
             << "setoption name EvalFile value __missing_mors_network__.mnue\n"
             << "setoption name Move Overhead value 25\n"
             << "position startpos moves e2e4 e7e5 g1f3\n"
             << "d\n"
             << "position fen rnbqkb1r/pp2pppp/3p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b KQkq - 0 1\n"
             << "d\n"
             << "position fen rnbqkb1r/pp2pppp/3p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b KQkq - moves f6e4\n"
             << "d\n"
             << "go movetime 20\n"
             << "stop\n"
             << "position fen 7k/8/5KQ1/8/8/8/8/8 w - - 0 1\n"
             << "go wtime 1000 btime 1000 winc 10 binc 10 movestogo 20\n"
             << "stop\n"
             << "quit\n";
    std::istringstream input{commands.str()};
    std::ostringstream output;
    if (mors::run_uci(input, output) != 0) {
        std::cerr << "FAIL UCI: command loop must exit successfully\n";
        return false;
    }

    const std::string text = output.str();
    const bool passed =
           expect_contains(
               text,
               "MORS 0.0.1-dev by Theodore Magnus Øen & Codex (see AUTHORS file)\n",
               "startup banner must credit both authors"
           )
        && expect_contains(text, "id name MORS 0.0.1-dev\n", "engine id must be emitted")
        && expect_contains(text, "id author Theodore Magnus Øen & Codex\n",
                           "author id must be emitted")
        && expect_contains(text, "uciok\n", "uci handshake must complete")
        && expect_contains(text, "readyok\n", "readiness handshake must complete")
        && expect_contains(text, "option name Threads type spin default 1 min 1 max 1\n",
                           "single-thread limit must be advertised")
        && expect_contains(text, "option name Move Overhead type spin",
                           "time safety option must be advertised")
        && expect_contains(text, "option name EvalFile type string",
                           "network path option must be advertised")
        && expect_contains(text, "info string Available processors: 0-",
                           "go must report available processors")
        && expect_contains(text, "info string Using 1 thread\n",
                           "go must report the active thread count")
        && expect_contains(
               text,
               "info string NNUE evaluation using mors-p2h32-s14400M-o3183M-c+frc.mnue "
               "(26MiB, P2-H32 (10240->768, 22528->256, 64))\n",
               "go must report the active NNUE architecture"
           )
        && expect_contains(text, "info string Network replica 1: Local memory.\n",
                           "go must report the network replica")
        && expect(
               text.find("info string Using 1 thread\n") < text.find("info depth 1 "),
               "search configuration must precede depth output"
           )
        && expect_contains(text, "info string EvalFile loaded:",
                           "EvalFile must load a replacement network")
        && expect_contains(text, "info string EvalFile loaded: <internal>\n",
                           "default EvalFile must restore the embedded network")
        && expect_contains(text, "info string EvalFile load failed:",
                           "invalid EvalFile must report an error")
        && expect_contains(text, "info string fen rnbqkbnr/pppp1ppp/",
                           "position moves must update the board")
        && expect_contains(
               text,
               "info string fen rnbqkb1r/pp2pppp/3p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b KQkq - 0 1\n",
               "complete six-field FEN must be accepted"
           )
        && expect_contains(
               text,
               "info string fen rnbqkb1r/pp2pppp/3p4/8/3Nn3/2N5/PPP2PPP/R1BQKB1R w KQkq - 0 2\n",
               "four-field FEN followed by moves must be accepted"
           )
        && expect_contains(text, "bestmove ", "search must emit bestmove")
        && expect_not_contains(text, "search busy",
                               "stop must join before the next command")
        && expect(occurrence_count(text, "bestmove ") == 2,
                  "both timed searches must emit bestmove");

    if (passed)
        std::cout << "PASS UCI timed search lifecycle\n";
    return passed;
}
