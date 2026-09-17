// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "uci.hpp"

#include "chess/attacks.hpp"
#include "chess/movegen.hpp"
#include "chess/position.hpp"
#include "eval/nnue/network.hpp"
#include "eval/nnue/wdl.hpp"
#include "search/score.hpp"
#include "search/search.hpp"
#include "search/time.hpp"
#include "search/tt.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

extern "C" {
extern const unsigned char gMorsDefaultNetworkData[];
extern const unsigned char gMorsDefaultNetworkEnd[];
}

__asm__(
    ".section .rodata\n"
    ".global gMorsDefaultNetworkData\n"
    ".balign 64\n"
    "gMorsDefaultNetworkData:\n"
    ".incbin \"../networks/mors-p2h32-s14400M-o3183M-c+frc.mnue\"\n"
    ".global gMorsDefaultNetworkEnd\n"
    ".balign 1\n"
    "gMorsDefaultNetworkEnd:\n"
    ".text\n"
);

namespace mors {
namespace {

constexpr std::string_view DEFAULT_NETWORK_FILENAME =
    "mors-p2h32-s14400M-o3183M-c+frc.mnue";
constexpr int DEFAULT_BENCH_DEPTH = 12;
constexpr std::size_t DEFAULT_BENCH_HASH_MB = 16;

constexpr std::array<std::string_view, 50> SEARCH_BENCH_FENS{{
    "rnb1k2r/pp2bp1p/2p1pp2/q7/8/1P6/PBPPQPPP/2KR1BNR w kq - 4 9",
    "1r1qk1nr/pppn1ppp/3p4/3Pp1b1/2P5/2N2Q1P/PP2PPP1/R1B1KB1R w KQk - 3 9",
    "r3kbnr/pp3ppp/2n5/2P1pq2/N1Pp2b1/5N2/PP1BPPPP/R2QKB1R w KQkq - 0 9",
    "rnb1k2r/pppp2pp/8/8/2P2Bn1/2q4N/P3PPPP/R2QKB1R w KQkq - 0 9",
    "r2qk2r/ppp1bppp/2n1bn2/8/2NPp3/2P5/PP2BPPP/RNBQ1RK1 w kq - 1 9",
    "r1b1kb1r/1pqnppp1/p2p1n1p/8/3NP1PP/P1N5/1PP2P2/R1BQKB1R w KQkq - 1 9",
    "rnb1kb1r/pp3pp1/1qpnp2p/3p4/3PP2B/P1N2P1N/1PP3PP/R2QKB1R w KQkq - 0 9",
    "r1bqr1k1/pppp1ppp/2n5/3np1N1/4P3/2P5/PPP2PPP/R1BQ1RK1 w - - 0 9",
    "r1bqkb1r/3n1ppp/p1p1pn2/1p6/8/5NP1/PPQPPPBP/RNB2RK1 w kq - 0 9",
    "r2qk2r/ppp1bppp/2np2n1/3N4/2BPPpb1/5N2/PPP3PP/R1BQ1RK1 w kq - 4 9",
    "r1bqkbnr/3n1ppp/p3p3/2p5/Pp1P4/4PN2/1P2BPPP/RNBQ1RK1 w kq - 0 9",
    "r1bqk2r/ppp1bppp/2n1p3/3pP3/2PP1B2/2PQ4/P4PPP/R3KBNR w KQkq d6 0 9",
    "r1bqk1nr/1ppn1pb1/p2p2pp/4p3/P2PP3/2NB1N2/1PP2PPP/R1BQ1RK1 w kq - 0 9",
    "r1bq1rk1/pp1pppbp/5np1/4n3/2PN4/1PN3P1/P3PPBP/R1BQK2R w KQ - 1 9",
    "rn1qkb1r/1p2npp1/4p2p/p2pPb2/3P4/P1N5/1P2NPPP/R1BQKB1R w KQkq - 2 9",
    "r1bqk2r/1p1nbpp1/p2p1n1p/2pPp3/2P5/P1N1PN1P/1P3PP1/R1BQKB1R w KQkq - 1 9",
    "r1bqk2r/2p1bpp1/p1np1n2/1p2p2p/3PP3/2N1BP2/PPPQN1PP/2KR1B1R w kq - 0 9",
    "r1bqk2r/p2nppbp/2pp2p1/1p2Pn2/3P1P2/2N1BN2/PPPQ2PP/R3KB1R w KQkq - 3 9",
    "r1b1k2r/pp1n1ppp/2p1pn2/q2p4/1bPP4/1PN2NP1/P2BPPBP/R2QK2R w KQkq - 3 9",
    "r1bq1bnr/p1p4p/1pk2p2/3pp1pQ/3P4/4P1B1/PPP2PPP/RN2K1NR w KQ - 0 9",
    "rn1qkb1r/1bp2ppp/p3pn2/8/Pp1PP3/1B3P2/1P2N1PP/RNBQK2R w KQkq - 0 9",
    "rnbq1rk1/1p2ppbp/2p3p1/p2n4/3P4/2NB1N1P/PPP2PP1/R1BQ1RK1 w - - 0 9",
    "rn1qkb1r/1p3p2/p1p1pn1p/2Pp1bp1/3P1B2/2N1PN2/PP2BPPP/R2QK2R w KQkq - 0 9",
    "r1bqk2r/pp1n1pp1/3p1n1p/2pPp3/1bP1P3/2N1BP2/PP4PP/R2QKBNR w KQkq - 2 9",
    "r1bqkb1r/1p2np1p/p1npp1p1/8/3NPP2/2NBB3/PPP3PP/R2QK2R w KQkq - 0 9",
    "rnbq1rk1/p1p2pp1/1p3p1p/8/1bBP4/2N1P3/PP2NPPP/R2QK2R w KQ - 0 9",
    "r1bqkb1r/pp4pp/2p1p3/3pnp1n/2PP4/2NBPN2/PP3PPP/R2QK2R w KQkq - 0 9",
    "rn1q1rk1/p1ppbppp/b3pn2/1p6/2PP4/1P3NP1/P2BPPBP/RN1Q1RK1 w - - 0 9",
    "rnbq1rk1/p3npbp/1pp1p1p1/3p4/2PP1B2/2NBPN2/PP3PPP/2RQK2R w K - 0 9",
    "rnbqk2r/1pp2pbp/p2p2p1/3pP3/3P1P2/3B1N2/PPP3PP/R1BQK2R w KQkq - 0 9",
    "rn1q1rk1/pbp1ppbp/1p3np1/3p4/3PPP2/2N1BB1P/PPP3P1/R2QK1NR w KQ - 0 9",
    "rnbqk2r/pp2p1bp/2pn1pp1/3pN3/3P4/2P3P1/PP1NPPBP/R1BQ1RK1 w kq - 0 9",
    "r1bq1rk1/p1pp1ppp/1pn5/4P3/2P1n3/P3PN2/1P1B1PPP/R2QKB1R w KQ - 0 9",
    "r1bqkbnr/ppp2p2/2npp3/8/2PP1P1p/3NP1pP/PP4P1/RNBQKB1R w KQkq - 0 9",
    "r2qk2r/ppp3pp/2n1b3/3n4/1b6/2N1PN2/PP3PPP/R1BQKB1R w KQkq - 0 9",
    "r1bqk2r/pppnn1b1/3pp1pp/5p1P/4PP2/2PP1N2/PP2B1P1/RNBQK2R w KQkq - 1 9",
    "r1b1kb1r/1p1p1ppp/p1q1pn2/8/4P3/1P1B4/P1P2PPP/RNBQ1RK1 w kq - 0 9",
    "r1bqk1nr/pp1p1ppp/1b6/8/1n2P3/1N1B4/PP3PPP/RNBQK2R w KQkq - 5 9",
    "r2q1rk1/ppp1ppb1/2np1np1/6Bp/3PP1bP/2PQ1N2/PP1N1PP1/R3KB1R w KQ - 5 9",
    "rn1qkb1r/1bpp2pp/p3p3/3n1p2/Pp1P4/4PNB1/1PPNBPPP/R2QK2R w KQkq - 0 9",
    "r1b1kbnr/1pq2pp1/p1np3p/4p3/2B1P3/5N2/PPP2PPP/RNBQR1K1 w kq - 2 9",
    "r1b1kb1r/pp3ppp/1q2pn2/2pP4/2pn4/2N2NP1/PP2PPBP/R1BQ1RK1 w kq - 0 9",
    "r3kb1r/ppp1p2p/2np1np1/5q2/3P4/5N2/PPP2PPP/RNBQ1RK1 w kq - 0 9",
    "r1bqnrk1/pp1nbppp/3pp3/2p3B1/3PP3/2PB1N1P/PP3PP1/RN1Q1RK1 w - - 1 9",
    "r1bqr1k1/pp1nbppp/4pn2/2pp4/3P1B1P/2PBPN2/PP1N1PP1/R2QK2R w KQ - 1 9",
    "r1b1kb1r/1pq2ppp/p1np1n2/2p1p3/P3P3/2N2NP1/1PPP1PBP/R1BQR1K1 w kq - 0 9",
    "rnbq1rk1/1p2ppb1/2pp1npp/p7/P2PP3/2N1BN2/1PP1BPPP/R2Q1RK1 w - - 0 9",
    "r1bqk2r/pp1nppbp/2np4/6B1/2P5/2NQPN2/PP3PPP/R3KB1R w KQkq - 1 9",
    "rnbq1rk1/1p2ppb1/p1p2n1p/3p2p1/2PP4/2N1PNBP/PP3PP1/R2QKB1R w KQ - 1 9",
    "rn2k2r/ppq2p1p/2ppbp2/2b1p3/2B1P2N/3P4/PPP2PPP/RN1Q1RK1 w kq - 2 9",
}};

constexpr std::array<std::string_view, 4> SEARCH_BENCH_FRC_FENS{{
    "bb1n1rkr/ppp1Q1pp/3n1p2/3p4/3P4/6Pq/PPP1PP1P/BB1NNRKR w HFhf - 0 5",
    "nqbnrkrb/pppppppp/8/8/8/8/PPPPPPPP/NQBNRKRB w KQkq - 0 1",
    "bb1rknrq/pppppppp/8/5N2/2P5/3P4/nP2PPPP/BBNRK1RQ w GDgd - 0 5",
    "rbk2r1q/1ppbpnpp/3p1p1n/p7/1P2PP1P/6P1/P1PP4/RBKNBRNQ b FAfa - 2 7",
}};

[[nodiscard]] std::expected<nnue::Network, std::string> load_default_network() {
    const std::span bytes{
        reinterpret_cast<const std::byte*>(gMorsDefaultNetworkData),
        static_cast<std::size_t>(
            gMorsDefaultNetworkEnd - gMorsDefaultNetworkData
        )
    };
    return nnue::Network::load(
        bytes,
        std::filesystem::path("<internal>")
    );
}

template<typename Integer>
[[nodiscard]] bool parse_integer(std::string_view text, Integer& value) noexcept {
    const auto [end, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );
    return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] char promotion_character(PieceType type) noexcept {
    switch (type) {
    case KNIGHT: return 'n';
    case BISHOP: return 'b';
    case ROOK:   return 'r';
    case QUEEN:  return 'q';
    default:     return '?';
    }
}

[[nodiscard]] std::string move_to_uci(Move move, bool chess960) {
    if (move.is_none())
        return "0000";

    Square destination = move.to();
    if (move.type() == CASTLING && !chess960) {
        const Color color = rank_of(move.from()) == RANK_1 ? WHITE : BLACK;
        const bool king_side = file_of(move.to()) > file_of(move.from());
        destination = castling_king_to(color, king_side);
    }

    std::string text;
    text.reserve(5);
    text.push_back(char('a' + file_of(move.from())));
    text.push_back(char('1' + rank_of(move.from())));
    text.push_back(char('a' + file_of(destination)));
    text.push_back(char('1' + rank_of(destination)));
    if (move.type() == PROMOTION)
        text.push_back(promotion_character(move.promotion_type()));
    return text;
}

[[nodiscard]] std::optional<Move> parse_uci_move(
    Position& position,
    std::string_view text,
    bool chess960
) {
    if (text.size() != 4 && text.size() != 5)
        return std::nullopt;

    MoveList moves;
    generate_legal(position, moves);
    for (const Move move : moves)
        if (move_to_uci(move, chess960) == text)
            return move;
    return std::nullopt;
}

[[nodiscard]] Position start_position(bool chess960 = false) {
    auto parsed = Position::from_fen(START_FEN, chess960);
    if (!parsed)
        throw std::runtime_error("internal start FEN is invalid");
    return std::move(*parsed);
}

void emit_score(
    std::ostream& output,
    Value value,
    const Position& root
) {
    if (!is_mate_value(value)) {
        output << "score cp " << nnue::score_to_cp(value, root);
        return;
    }

    const int plies = VALUE_MATE - (value >= 0 ? value : -value);
    const int moves = (plies + 1) / 2;
    output << "score mate " << (value >= 0 ? moves : -moves);
}

[[nodiscard]] bool run_search_bench(
    TranspositionTable& table,
    const nnue::Network& network,
    SearchThreadPool& thread_pool,
    int depth,
    std::size_t hash_mb,
    std::ostream& output,
    bool quiet
) {
    using Clock = std::chrono::steady_clock;

    std::uint64_t total_nodes = 0;
    double total_seconds = 0.0;
    std::uint64_t checksum = 0;
    std::size_t position_index = 0;

    if (!quiet) {
        output << "--------------------------------------------------\n"
               << "      Variant       Nodes       Elapsed             NPS\n"
               << "--------------------------------------------------\n";
    }

    const auto run_position = [&](std::string_view fen, bool chess960) {
        auto parsed = Position::from_fen(fen, chess960);
        if (!parsed) {
            output << "info string invalid "
                   << (chess960 ? "FRC" : "classical")
                   << " bench position " << position_index
                   << ": " << parsed.error() << '\n';
            return false;
        }

        table.clear();
        thread_pool.clear();

        SearchLimits limits;
        limits.max_depth = depth;
        limits.start_time = Clock::now();

        SearchResult result;
        std::string error;
        const auto started = Clock::now();
        try {
            thread_pool.start(
                *parsed,
                limits,
                [&](const SearchResult& completed, std::string_view failure) {
                    result = completed;
                    error = failure;
                }
            );
            thread_pool.wait();
        } catch (const std::exception& exception) {
            output << "info string bench search failed at position "
                   << position_index << ": " << exception.what() << '\n';
            return false;
        }
        const double seconds =
            std::chrono::duration<double>(Clock::now() - started).count();

        if (!error.empty()) {
            output << "info string bench search failed at position "
                   << position_index << ": " << error << '\n';
            return false;
        }
        if (result.completed_depth != depth) {
            output << "info string incomplete bench position "
                   << position_index << " depth " << result.completed_depth
                   << " expected " << depth << '\n';
            return false;
        }

        const std::uint64_t nodes = result.stats.nodes;
        const std::uint64_t nps = seconds > 0.0
            ? static_cast<std::uint64_t>(
                static_cast<double>(nodes) / seconds
            )
            : 0;
        total_nodes += nodes;
        total_seconds += seconds;
        checksum = (checksum * 1'315'423'911ULL)
            ^ nodes
            ^ (static_cast<std::uint64_t>(result.best_move.raw()) << 32)
            ^ static_cast<std::uint64_t>(
                static_cast<std::int64_t>(result.value)
            )
            ^ (static_cast<std::uint64_t>(chess960) << 63);

        if (!quiet) {
            output << std::setw(3) << position_index
                   << std::setw(9) << (chess960 ? "frc" : "classic")
                   << std::setw(12) << nodes
                   << std::setw(13) << std::fixed << std::setprecision(3)
                   << seconds << "s"
                   << std::setw(16) << nps << " N/s\n";
        }
        ++position_index;
        return true;
    };

    for (const std::string_view fen : SEARCH_BENCH_FENS)
        if (!run_position(fen, false))
            return false;
    for (const std::string_view fen : SEARCH_BENCH_FRC_FENS)
        if (!run_position(fen, true))
            return false;

    table.clear();
    thread_pool.clear();

    const std::uint64_t total_nps = total_seconds > 0.0
        ? static_cast<std::uint64_t>(
            static_cast<double>(total_nodes) / total_seconds
        )
        : 0;

    if (!quiet) {
        output << "--------------------------------------------------\n"
               << std::setw(15) << total_nodes
               << std::setw(13) << std::fixed << std::setprecision(3)
               << total_seconds << "s"
               << std::setw(16) << total_nps << " N/s\n"
               << "--------------------------------------------------\n"
               << "depth " << depth
               << " hash " << hash_mb
               << " threads " << thread_pool.size()
               << " evaluator " << network.source().filename().string()
               << " positions " << position_index
               << " frc " << SEARCH_BENCH_FRC_FENS.size()
               << " checksum " << checksum << '\n';
    }

    output << "Bench: " << total_nodes << " nodes " << total_nps
           << " nps positions " << position_index
           << " frc " << SEARCH_BENCH_FRC_FENS.size()
           << " checksum " << checksum << '\n';
    return true;
}

class UciSession final {
public:
    explicit UciSession(nnue::Network network)
        : position_(start_position()),
          table_(DEFAULT_TT_SIZE_MB),
          network_(std::move(network)),
          thread_pool_(table_, network_) {}

    ~UciSession() {
        stop_search();
        syzygy::shutdown();
    }

    [[nodiscard]] bool process(std::string_view line, std::ostream& output) {
        std::istringstream stream{std::string(line)};
        std::string command;
        if (!(stream >> command))
            return true;

        if (command == "uci") {
            std::ostringstream response;
            response << "id name Dragon of MORS 0.2.1\n"
                     << "id author Theodore M. A. Øen (USA) & Codex (USA)\n"
                     << "option name Threads type spin default 1 min 1 max "
                     << MAX_SEARCH_THREADS << "\n"
                     << "option name Hash type spin default " << DEFAULT_TT_SIZE_MB
                     << " min 1 max " << MAX_TT_SIZE_MB << "\n"
                     << "option name NumaPolicy type combo default auto var auto var none\n"
                     << "option name Clear Hash type button\n"
                     << "option name SyzygyPath type string default <empty>\n"
                     << "option name SyzygyProbeLimit type spin default 7 min 0 max 7\n"
                     << "option name SyzygyProbeDepth type spin default 1 min 1 max 100\n"
                     << "option name Syzygy50MoveRule type check default true\n"
                     << "option name Ponder type check default true\n"
                     << "option name UCI_Chess960 type check default false\n"
                     << "option name EvalFile type string default " << DEFAULT_NETWORK_FILENAME << "\n"
                     << "option name Move Overhead type spin default "
                     << timeman::DEFAULT_MOVE_OVERHEAD_MS
                     << " min " << timeman::MIN_MOVE_OVERHEAD_MS
                     << " max " << timeman::MAX_MOVE_OVERHEAD_MS << "\n"
                     << "uciok\n";
            emit(output, response.str());
            return true;
        }
        if (command == "isready") {
            emit(output, "readyok\n");
            return true;
        }
        if (command == "stop") {
            stop_search();
            return true;
        }
        if (command == "quit") {
            stop_search();
            return false;
        }
        if (command == "ponderhit") {
            if (!thread_pool_.ponderhit())
                emit(output, "info string ponderhit ignored: no active ponder search\n");
            return true;
        }

        if (thread_pool_.searching()) {
            emit(output, "info string search busy, send stop first\n");
            return true;
        }

        if (command == "ucinewgame") {
            thread_pool_.clear();
            position_ = start_position(chess960_);
            prior_keys_.clear();
            table_.clear();
            time_manager_.new_game();
        } else if (command == "setoption") {
            handle_setoption(stream, output);
        } else if (command == "position") {
            handle_position(stream, output);
        } else if (command == "go") {
            handle_go(stream, output);
        } else if (command == "bench") {
            handle_bench(stream, output);
        } else if (command == "d") {
            emit(output, "info string fen " + position_.fen() + "\n");
        } else {
            emit(output, "info string unknown command: " + command + "\n");
        }
        return true;
    }

private:
    void emit(std::ostream& output, std::string_view text) {
        const std::lock_guard lock(output_mutex_);
        output << text;
        output.flush();
    }

    void stop_search() {
        thread_pool_.request_stop();
        thread_pool_.wait();
    }

    void handle_setoption(std::istringstream& stream, std::ostream& output) {
        std::string marker;
        if (!(stream >> marker) || marker != "name") {
            emit(output, "info string invalid setoption command\n");
            return;
        }

        std::string name;
        std::string token;
        while (stream >> token && token != "value") {
            if (!name.empty())
                name.push_back(' ');
            name += token;
        }

        if (name == "Clear Hash") {
            table_.clear();
            return;
        }
        if (token != "value") {
            emit(output, "info string missing option value\n");
            return;
        }

        std::string value_text;
        std::getline(stream, value_text);
        const std::size_t value_begin = value_text.find_first_not_of(" \t");
        if (value_begin == std::string::npos) {
            emit(output, "info string missing option value\n");
            return;
        }
        value_text.erase(0, value_begin);
        const std::size_t value_end = value_text.find_last_not_of(" \t");
        value_text.erase(value_end + 1);
        if (value_text.size() >= 2
            && value_text.front() == '"'
            && value_text.back() == '"') {
            value_text = value_text.substr(1, value_text.size() - 2);
        }

        if (name == "UCI_Chess960") {
            if (value_text != "true" && value_text != "false") {
                emit(output, "info string UCI_Chess960 must be true or false\n");
                return;
            }
            chess960_ = value_text == "true";
            position_.set_chess960(chess960_);
            return;
        }

        if (name == "Ponder") {
            if (value_text != "true" && value_text != "false") {
                emit(output, "info string Ponder must be true or false\n");
                return;
            }
            ponder_enabled_ = value_text == "true";
            return;
        }

        if (name == "SyzygyPath") {
            const bool loaded = syzygy::init(value_text == "<empty>" ? "" : value_text);
            table_.clear();
            emit(output, loaded
                ? "info string Syzygy available up to " + std::to_string(syzygy::max_pieces()) + " pieces\n"
                : "info string Syzygy initialization failed\n");
            return;
        }
        if (name == "Syzygy50MoveRule") {
            if (value_text != "true" && value_text != "false") {
                emit(output, "info string Syzygy50MoveRule must be true or false\n");
                return;
            }
            syzygy_options_.rule50 = value_text == "true";
            table_.clear();
            return;
        }
        if (name == "SyzygyProbeLimit" || name == "SyzygyProbeDepth") {
            int value = 0;
            const bool is_limit = name == "SyzygyProbeLimit";
            if (!parse_integer(value_text, value)
                || value < (is_limit ? 0 : 1) || value > (is_limit ? 7 : 100)) {
                emit(output, "info string invalid " + name + "\n");
                return;
            }
            (is_limit ? syzygy_options_.probe_limit : syzygy_options_.probe_depth) = value;
            table_.clear();
            return;
        }

        if (name == "EvalFile") {
            auto loaded =
                value_text.empty() || value_text == DEFAULT_NETWORK_FILENAME
                ? load_default_network()
                : nnue::Network::load(std::filesystem::path(value_text));
            if (!loaded) {
                emit(output, "info string EvalFile load failed: "
                           + loaded.error() + "\n");
                return;
            }
            try {
                thread_pool_.refresh_network(*loaded);
            } catch (const std::exception& error) {
                emit(output, "info string EvalFile replica preparation failed: "
                           + std::string(error.what()) + "\n");
                return;
            }
            network_ = std::move(*loaded);
            table_.clear();
            emit(output, "info string EvalFile loaded: "
                       + network_.source().string() + "\n");
            return;
        }

        if (name == "Move Overhead") {
            std::int64_t milliseconds = 0;
            if (!parse_integer(value_text, milliseconds)
                || milliseconds < timeman::MIN_MOVE_OVERHEAD_MS
                || milliseconds > timeman::MAX_MOVE_OVERHEAD_MS) {
                emit(output, "info string Move Overhead must be between 0 and 5000 ms\n");
                return;
            }
            time_manager_.set_move_overhead_ms(milliseconds);
            return;
        }

        if (name == "NumaPolicy") {
            if (value_text != "auto" && value_text != "none") {
                emit(output, "info string NumaPolicy must be auto or none\n");
                return;
            }
            try {
                thread_pool_.set_numa_policy(value_text == "auto"
                    ? numa::Policy::Auto : numa::Policy::None);
            } catch (const std::exception& error) {
                emit(output, "info string NumaPolicy change failed: "
                           + std::string(error.what()) + "\n");
            }
            return;
        }

        if (name == "Threads") {
            std::size_t threads = 0;
            if (!parse_integer(value_text, threads)
                || threads < 1 || threads > MAX_SEARCH_THREADS) {
                emit(
                    output,
                    "info string Threads must be between 1 and "
                        + std::to_string(MAX_SEARCH_THREADS) + "\n"
                );
                return;
            }
            try {
                thread_pool_.resize(threads);
            } catch (const std::exception& error) {
                emit(
                    output,
                    "info string Threads resize failed: "
                        + std::string(error.what()) + "\n"
                );
            }
            return;
        }

        if (name != "Hash") {
            emit(output, "info string unsupported option: " + name + "\n");
            return;
        }

        std::size_t megabytes = 0;
        if (!parse_integer(value_text, megabytes)
            || megabytes < 1 || megabytes > MAX_TT_SIZE_MB) {
            emit(output, "info string Hash must be between 1 and "
                + std::to_string(MAX_TT_SIZE_MB) + " MiB\n");
            return;
        }
        try {
            table_.resize(megabytes);
        } catch (const std::exception& error) {
            emit(output, "info string Hash resize failed: "
                       + std::string(error.what()) + "\n");
        }
    }

    void handle_position(std::istringstream& stream, std::ostream& output) {
        std::string kind;
        if (!(stream >> kind)) {
            emit(output, "info string invalid position command\n");
            return;
        }

        std::optional<Position> candidate;
        std::string token;
        bool has_moves = false;
        if (kind == "startpos") {
            candidate = start_position(chess960_);
        } else if (kind == "fen") {
            std::string fen;
            while (stream >> token && token != "moves") {
                if (!fen.empty())
                    fen.push_back(' ');
                fen += token;
            }
            has_moves = token == "moves";
            auto parsed = Position::from_fen(fen, chess960_);
            if (!parsed) {
                emit(output, "info string invalid FEN: " + parsed.error() + "\n");
                return;
            }
            candidate = std::move(*parsed);
        } else {
            emit(output, "info string expected startpos or fen\n");
            return;
        }

        std::vector<Key> history;
        if (has_moves || stream >> token) {
            if (token != "moves") {
                emit(output, "info string expected moves after position\n");
                return;
            }
            while (stream >> token) {
                const auto move = parse_uci_move(*candidate, token, chess960_);
                if (!move) {
                    emit(output, "info string illegal move in position: " + token + "\n");
                    return;
                }
                history.push_back(candidate->key());
                StateInfo state;
                candidate->do_move(*move, state);
            }
        }

        position_ = std::move(*candidate);
        prior_keys_ = std::move(history);
    }

    void handle_go(std::istringstream& stream, std::ostream& output) {
        timeman::GoParams params;
        bool saw_parameter = false;
        std::string token;
        while (stream >> token) {
            saw_parameter = true;
            std::string value_text;
            if (token == "ponder") {
                params.ponder = true;
                continue;
            }
            if (token == "infinite") {
                params.infinite = true;
                continue;
            }
            if (!(stream >> value_text)) {
                emit(output, "info string missing value after go " + token + "\n");
                return;
            }

            if (token == "depth") {
                if (!parse_integer(value_text, params.depth)
                    || params.depth < 1 || params.depth > MAX_PLY) {
                    emit(output, "info string depth must be between 1 and "
                               + std::to_string(MAX_PLY) + "\n");
                    return;
                }
            } else if (token == "nodes") {
                if (!parse_integer(value_text, params.nodes) || params.nodes == 0) {
                    emit(output, "info string nodes must be positive\n");
                    return;
                }
            } else if (token == "movetime") {
                if (!parse_integer(value_text, params.movetime) || params.movetime <= 0) {
                    emit(output, "info string movetime must be positive\n");
                    return;
                }
            } else if (token == "wtime") {
                if (!parse_integer(value_text, params.wtime)) {
                    emit(output, "info string invalid wtime\n");
                    return;
                }
            } else if (token == "btime") {
                if (!parse_integer(value_text, params.btime)) {
                    emit(output, "info string invalid btime\n");
                    return;
                }
            } else if (token == "winc") {
                if (!parse_integer(value_text, params.winc) || params.winc < 0) {
                    emit(output, "info string invalid winc\n");
                    return;
                }
            } else if (token == "binc") {
                if (!parse_integer(value_text, params.binc) || params.binc < 0) {
                    emit(output, "info string invalid binc\n");
                    return;
                }
            } else if (token == "movestogo") {
                if (!parse_integer(value_text, params.movestogo)
                    || params.movestogo <= 0) {
                    emit(output, "info string movestogo must be positive\n");
                    return;
                }
            } else {
                emit(output, "info string unsupported go token: " + token + "\n");
                return;
            }
        }
        if (!saw_parameter)
            params.infinite = true;

        SearchLimits limits;
        if (!time_manager_.build_limits(position_, params, limits)) {
            emit(output, "info string go requires a usable search limit\n");
            return;
        }

        const std::filesystem::path& network_source = network_.source();
        std::string network_name =
            network_source == std::filesystem::path("<internal>")
            ? std::string(DEFAULT_NETWORK_FILENAME)
            : network_source.filename().string();
        if (network_name.empty())
            network_name = network_source.string();

        constexpr std::size_t MEBIBYTE = 1U << 20;
        const std::size_t network_mib = network_.memory_bytes() / MEBIBYTE;
        const std::size_t thread_count = thread_pool_.size();
        std::ostringstream configuration;
        configuration << thread_pool_.configuration();
        if (thread_count == 1) {
            configuration << "info string Using 1 thread\n";
        } else {
            configuration
                << "info string Using " << thread_count
                << " threads\n";
        }
        configuration
            << "info string NNUE evaluation using " << network_name
            << " (" << network_mib << "MiB, P2-H32 ("
            << nnue::P2H32::COARSE_INPUTS << "->"
            << nnue::P2H32::COARSE_WIDTH << ", "
            << nnue::P2H32::FINE_INPUTS << "->"
            << nnue::P2H32::FINE_WIDTH << ", "
            << nnue::P2H32::OUTPUT_BUCKETS << "))\n";
        emit(output, configuration.str());

        MoveList legal_moves;
        generate_legal(position_, legal_moves);
        const Move fallback = legal_moves.empty() ? Move{} : legal_moves[0];
        Position root = position_;
        const auto started = std::chrono::steady_clock::now();
        limits.start_time = started;
        limits.prior_keys = prior_keys_;
        limits.syzygy = syzygy_options_;
        limits.iteration_callback =
            [this, root, started, output = &output](
                const SearchResult& iteration
            ) {
                const auto elapsed =
                    std::chrono::steady_clock::now() - started;
                const auto elapsed_count =
                    std::chrono::duration_cast<
                        std::chrono::milliseconds
                    >(elapsed).count();
                const std::uint64_t elapsed_ms = elapsed_count > 0
                    ? static_cast<std::uint64_t>(elapsed_count)
                    : 0;
                const std::uint64_t nps = elapsed_ms != 0
                    ? iteration.stats.nodes * 1'000 / elapsed_ms
                    : 0;
                const nnue::WdlTriplet wdl =
                    iteration.root_in_tb && iteration.value == VALUE_DRAW
                    ? nnue::WdlTriplet{0, 1'000, 0}
                    : nnue::score_to_wdl(iteration.value, root);

                std::ostringstream response;
                response << "info depth "
                         << iteration.completed_depth
                         << " seldepth "
                         << iteration.stats.seldepth << ' ';
                emit_score(response, iteration.value, root);
                response << " wdl " << wdl.win << ' '
                         << wdl.draw << ' ' << wdl.loss
                         << " nodes " << iteration.stats.nodes
                         << " tbhits " << iteration.stats.tb_hits
                         << " nps " << nps
                         << " hashfull " << table_.hashfull()
                         << " time " << elapsed_ms
                         << " pv";
                for (std::size_t index = 0;
                     index < iteration.pv_length;
                     ++index) {
                    response << ' '
                             << move_to_uci(
                                    iteration.principal_variation[index],
                                    root.chess960()
                                );
                }
                response << '\n';
                emit(*output, response.str());
            };

        const bool root_chess960 = root.chess960();
        try {
            thread_pool_.start(
                root,
                limits,
                [this, fallback, root_chess960, output = &output](
                    const SearchResult& result,
                    std::string_view error
                ) {
                    if (!error.empty()) {
                        emit(
                            *output,
                            "info string search failed: "
                                + std::string(error) + "\nbestmove "
                                + move_to_uci(fallback, root_chess960) + "\n"
                        );
                        return;
                    }

                    std::ostringstream response;
                    const Move best_move = result.best_move.is_none()
                        ? fallback
                        : result.best_move;
                    response << "bestmove "
                             << move_to_uci(best_move, root_chess960);
                    if (ponder_enabled_
                        && result.pv_length > 1
                        && result.principal_variation[0] == best_move) {
                        response << " ponder "
                                 << move_to_uci(
                                        result.principal_variation[1],
                                        root_chess960
                                    );
                    }
                    response << '\n';
                    emit(*output, response.str());
                }
            );
        } catch (const std::exception& error) {
            emit(output, "info string could not start search: "
                       + std::string(error.what()) + "\nbestmove "
                       + move_to_uci(fallback, position_.chess960()) + "\n");
        }
    }

    void handle_bench(std::istringstream& stream, std::ostream& output) {
        std::string argument;
        if (stream >> argument) {
            emit(output, "info string usage: bench\n");
            return;
        }

        constexpr std::size_t MEBIBYTE = 1U << 20;
        const std::size_t hash_mb =
            (table_.size_bytes() + MEBIBYTE - 1) / MEBIBYTE;
        if (!run_search_bench(
                table_,
                network_,
                thread_pool_,
                DEFAULT_BENCH_DEPTH,
                hash_mb,
                output,
                false
            )) {
            emit(output, "info string bench failed\n");
        }
    }

    Position position_;
    TranspositionTable table_;
    nnue::Network network_;
    std::vector<Key> prior_keys_;
    timeman::TimeManager time_manager_;
    SearchThreadPool thread_pool_;
    std::mutex output_mutex_;
    bool ponder_enabled_ = true;
    bool chess960_ = false;
    syzygy::Options syzygy_options_{};
};

} // namespace

int run_uci(std::istream& input, std::ostream& output) {
    output << "Dragon of MORS 0.2.1 by Theodore M. A. Øen (USA) & Codex (USA) "
              "(see AUTHORS file)\n";

    initialize_attacks();

    auto loaded = load_default_network();
    if (!loaded) {
        output << "info string NNUE load failed: " << loaded.error() << '\n';
        return 1;
    }

    UciSession session(std::move(*loaded));
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!session.process(line, output))
            break;
    }
    return 0;
}

int run_bench(int argc, char** argv) {
    const auto print_usage = [] {
        std::cerr
            << "usage: Dragon-of-MORS.exe bench [depth] [hash_mb] [threads]\n";
    };

    if (argc < 2 || std::string_view(argv[1]) != "bench" || argc > 5) {
        print_usage();
        return 1;
    }

    int depth = DEFAULT_BENCH_DEPTH;
    std::size_t hash_mb = DEFAULT_BENCH_HASH_MB;
    std::size_t threads = 1;
    const auto parse_optional = [&](int index, auto& value) {
        return argc <= index
            || parse_integer(std::string_view(argv[index]), value);
    };
    if (!parse_optional(2, depth)
        || !parse_optional(3, hash_mb)
        || !parse_optional(4, threads)
        || depth < 1 || depth > MAX_PLY
        || hash_mb < 1 || hash_mb > MAX_TT_SIZE_MB
        || threads < 1 || threads > MAX_SEARCH_THREADS) {
        print_usage();
        return 1;
    }

    try {
        initialize_attacks();
        auto loaded = load_default_network();
        if (!loaded) {
            std::cerr << "info string NNUE load failed: "
                      << loaded.error() << '\n';
            return 1;
        }

        TranspositionTable table(hash_mb);
        SearchThreadPool thread_pool(table, *loaded, threads);
        const bool ok = run_search_bench(
            table,
            *loaded,
            thread_pool,
            depth,
            hash_mb,
            std::cout,
            true
        );
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "info string bench failed: " << error.what() << '\n';
        return 1;
    }
}

int run_uci() {
    std::cout << std::unitbuf;
    return run_uci(std::cin, std::cout);
}

} // namespace mors
