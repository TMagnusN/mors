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
             << "setoption name NumaPolicy value none\n"
             << "setoption name NumaPolicy value invalid\n"
             << "setoption name Threads value 4\n"
             << "setoption name Threads value 0\n"
             << "setoption name Threads value 22529\n"
             << "setoption name Hash value 0\n"
             << "setoption name Hash value 8589934593\n"
             << "setoption name Hash value 2\n"
             << "setoption name Clear Hash\n"
             << "setoption name EvalFile value " << network.string() << "\n"
             << "setoption name EvalFile value mors-p2h32-s14400M-o3183M-c+frc.mnue\n"
             << "setoption name EvalFile value __missing_mors_network__.mnue\n"
             << "setoption name Move Overhead value 25\n"
             << "setoption name UCI_Chess960 value true\n"
             << "position fen 4k3/8/8/8/8/8/8/R5KR w AH - 0 1 moves g1h1\n"
             << "d\n"
             << "setoption name UCI_Chess960 value false\n"
             << "position fen r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1 moves e1g1\n"
             << "d\n"
             << "position startpos moves e2e4 e7e5 g1f3\n"
             << "d\n"
             << "position fen rnbqkb1r/pp2pppp/3p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b KQkq - 0 1\n"
             << "d\n"
             << "position fen rnbqkb1r/pp2pppp/3p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b KQkq - moves f6e4\n"
             << "d\n"
             << "go movetime 20\n"
             << "stop\n"
             << "ucinewgame\n"
             << "setoption name Threads value 1\n"
             << "setoption name NumaPolicy value auto\n"
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
               "Dragon of MORS 0.2.0 by Theodore M. A. Øen (USA) & Codex (USA) (see AUTHORS file)\n",
               "startup banner must credit both authors"
           )
        && expect_contains(text, "id name Dragon of MORS 0.2.0\n", "engine id must be emitted")
        && expect_contains(text, "id author Theodore M. A. Øen (USA) & Codex (USA)\n",
                           "author id must be emitted")
        && expect_contains(text, "option name Hash type spin default 256 min 1 max 8589934592\n",
                           "Hash must advertise the 8 PiB limit in MiB")
        && expect(occurrence_count(text,
                       "info string Hash must be between 1 and 8589934592 MiB\n") == 2,
                  "zero and above-limit Hash values must be rejected")
        && expect_not_contains(text, "Hash resize failed:",
                               "invalid sizes must be rejected before allocation")
        && expect_contains(text, "uciok\n", "uci handshake must complete")
        && expect_contains(text, "readyok\n", "readiness handshake must complete")
        && expect_contains(text, "option name Threads type spin default 1 min 1 max 22528\n",
                           "persistent thread-pool range must be advertised")
        && expect(occurrence_count(text,
                       "info string Threads must be between 1 and 22528\n") == 2,
                  "zero and above-limit thread counts must be rejected")
        && expect_not_contains(text, "Threads resize failed:",
                               "invalid thread counts must be rejected before allocation")
        && expect_contains(text, "option name Move Overhead type spin",
                           "time safety option must be advertised")
        && expect_contains(text, "option name EvalFile type string",
                           "network path option must be advertised")
        && expect_contains(text, "option name UCI_Chess960 type check default false\n",
                           "Chess960 mode must be advertised")
        && expect_contains(text, "option name NumaPolicy type combo default auto var auto var none\n",
                           "NUMA policy must be advertised")
        && expect_contains(text, "info string NumaPolicy must be auto or none\n",
                           "invalid NUMA policy must be rejected")
        && expect_contains(text, "info string NUMA policy: none (OS scheduling)\n",
                           "none must disable binding")
        && expect_contains(text, "info string NUMA policy: auto",
                           "auto must be restored")
        && expect_not_contains(text, "NumaPolicy change failed:",
                               "NUMA policy changes must succeed")
        && expect_not_contains(text, "EvalFile replica preparation failed:",
                               "valid replacement networks must prepare successfully")
        && expect_contains(text, "info string Available processors:",
                           "go must report available processors")
        && expect_contains(
               text,
               "info string Using 4 threads\n",
               "go must report active Lazy SMP workers"
           )
        && expect_contains(text, "info string Using 1 thread\n",
                           "pool must shrink back to one worker")
        && expect_contains(
               text,
               "info string NNUE evaluation using mors-p2h32-s14400M-o3183M-c+frc.mnue "
               "(26MiB, P2-H32 (10240->768, 22528->256, 64))\n",
               "go must report the active NNUE architecture"
           )
        && expect_contains(text, "info string Network replicas: 1 shared (OS placement)\n",
                           "unbound workers must report OS placement")
        && expect_not_contains(text, "Local memory.",
                               "must not claim verified physical locality")
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
        && expect_contains(
               text,
               "info string fen 4k3/8/8/8/8/8/8/R4RK1 b - - 1 1\n",
               "Chess960 rook-square castling input must be accepted"
           )
        && expect_contains(
               text,
               "info string fen r3k2r/8/8/8/8/8/8/R4RK1 b kq - 1 1\n",
               "classical king-destination castling input must remain accepted"
           )
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
