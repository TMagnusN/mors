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

#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
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
#include <thread>
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

[[nodiscard]] std::optional<Square> parse_square(std::string_view text) noexcept {
    if (text.size() != 2
        || text[0] < 'a' || text[0] > 'h'
        || text[1] < '1' || text[1] > '8') {
        return std::nullopt;
    }
    return make_square(
        File(text[0] - 'a'),
        Rank(text[1] - '1')
    );
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

[[nodiscard]] std::optional<PieceType> parse_promotion(char character) noexcept {
    switch (character) {
    case 'n': return KNIGHT;
    case 'b': return BISHOP;
    case 'r': return ROOK;
    case 'q': return QUEEN;
    default:  return std::nullopt;
    }
}

[[nodiscard]] std::string move_to_uci(Move move) {
    if (move.is_none())
        return "0000";

    std::string text;
    text.reserve(5);
    text.push_back(char('a' + file_of(move.from())));
    text.push_back(char('1' + rank_of(move.from())));
    text.push_back(char('a' + file_of(move.to())));
    text.push_back(char('1' + rank_of(move.to())));
    if (move.type() == PROMOTION)
        text.push_back(promotion_character(move.promotion_type()));
    return text;
}

[[nodiscard]] std::optional<Move> parse_uci_move(
    Position& position,
    std::string_view text
) {
    if (text.size() != 4 && text.size() != 5)
        return std::nullopt;

    const auto from = parse_square(text.substr(0, 2));
    const auto to = parse_square(text.substr(2, 2));
    if (!from || !to)
        return std::nullopt;

    std::optional<PieceType> promotion;
    if (text.size() == 5) {
        promotion = parse_promotion(text[4]);
        if (!promotion)
            return std::nullopt;
    }

    MoveList moves;
    generate_legal(position, moves);
    for (const Move move : moves) {
        if (move.from() != *from || move.to() != *to)
            continue;
        if (move.type() == PROMOTION) {
            if (promotion && move.promotion_type() == *promotion)
                return move;
        } else if (!promotion) {
            return move;
        }
    }
    return std::nullopt;
}

[[nodiscard]] Position start_position() {
    auto parsed = Position::from_fen(START_FEN);
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

class UciSession final {
public:
    explicit UciSession(nnue::Network network)
        : position_(start_position()),
          table_(DEFAULT_TT_SIZE_MB),
          network_(std::move(network)) {}

    ~UciSession() {
        stop_search();
    }

    [[nodiscard]] bool process(std::string_view line, std::ostream& output) {
        join_finished_search();

        std::istringstream stream{std::string(line)};
        std::string command;
        if (!(stream >> command))
            return true;

        if (command == "uci") {
            std::ostringstream response;
            response << "id name MORS 0.0.1-dev\n"
                     << "id author Theodore Magnus Oen\n"
                     << "option name Threads type spin default 1 min 1 max 1\n"
                     << "option name Hash type spin default " << DEFAULT_TT_SIZE_MB
                     << " min 1 max 32768\n"
                     << "option name Clear Hash type button\n"
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

        if (search_running_.load(std::memory_order_acquire)) {
            emit(output, "info string search busy, send stop first\n");
            return true;
        }

        if (command == "ucinewgame") {
            position_ = start_position();
            prior_keys_.clear();
            table_.clear();
            time_manager_.new_game();
        } else if (command == "setoption") {
            handle_setoption(stream, output);
        } else if (command == "position") {
            handle_position(stream, output);
        } else if (command == "go") {
            handle_go(stream, output);
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

    void join_finished_search() {
        if (search_thread_.joinable()
            && !search_running_.load(std::memory_order_acquire)) {
            search_thread_.join();
        }
    }

    void stop_search() {
        stop_requested_.store(true, std::memory_order_release);
        if (search_thread_.joinable())
            search_thread_.join();
        search_running_.store(false, std::memory_order_release);
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

        if (name == "Threads") {
            std::size_t threads = 0;
            if (!parse_integer(value_text, threads) || threads != 1) {
                emit(output, "info string Threads must be 1\n");
                return;
            }
            return;
        }

        if (name != "Hash") {
            emit(output, "info string unsupported option: " + name + "\n");
            return;
        }

        std::size_t megabytes = 0;
        if (!parse_integer(value_text, megabytes)
            || megabytes < 1 || megabytes > 32'768) {
            emit(output, "info string Hash must be between 1 and 32768 MB\n");
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
        if (kind == "startpos") {
            candidate = start_position();
        } else if (kind == "fen") {
            std::array<std::string, 6> fields;
            for (std::string& field : fields) {
                if (!(stream >> field)) {
                    emit(output, "info string incomplete FEN\n");
                    return;
                }
            }
            std::string fen = fields[0];
            for (std::size_t index = 1; index < fields.size(); ++index) {
                fen.push_back(' ');
                fen += fields[index];
            }
            auto parsed = Position::from_fen(fen);
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
        if (stream >> token) {
            if (token != "moves") {
                emit(output, "info string expected moves after position\n");
                return;
            }
            while (stream >> token) {
                const auto move = parse_uci_move(*candidate, token);
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

        const unsigned processor_count = std::thread::hardware_concurrency();
        const unsigned last_processor =
            processor_count == 0 ? 0 : processor_count - 1;
        const std::filesystem::path& network_source = network_.source();
        std::string network_name =
            network_source == std::filesystem::path("<internal>")
            ? std::string(DEFAULT_NETWORK_FILENAME)
            : network_source.filename().string();
        if (network_name.empty())
            network_name = network_source.string();

        constexpr std::size_t MEBIBYTE = 1U << 20;
        const std::size_t network_mib = network_.memory_bytes() / MEBIBYTE;
        std::ostringstream configuration;
        configuration
            << "info string Available processors: 0-" << last_processor << '\n'
            << "info string Using 1 thread\n"
            << "info string NNUE evaluation using " << network_name
            << " (" << network_mib << "MiB, P2-H32 ("
            << nnue::P2H32::COARSE_INPUTS << "->"
            << nnue::P2H32::COARSE_WIDTH << ", "
            << nnue::P2H32::FINE_INPUTS << "->"
            << nnue::P2H32::FINE_WIDTH << ", "
            << nnue::P2H32::OUTPUT_BUCKETS << "))\n"
            << "info string Network replica 1: Local memory.\n";
        emit(output, configuration.str());

        MoveList legal_moves;
        generate_legal(position_, legal_moves);
        const Move fallback = legal_moves.empty() ? Move{} : legal_moves[0];
        Position root = position_;
        std::vector<Key> history = prior_keys_;
        const auto started = std::chrono::steady_clock::now();
        limits.stop = &stop_requested_;
        limits.start_time = started;
        limits.prior_keys = {};

        stop_requested_.store(false, std::memory_order_release);
        search_running_.store(true, std::memory_order_release);
        try {
            search_thread_ = std::thread(
                [this,
                 root = std::move(root),
                 history = std::move(history),
                 limits,
                 fallback,
                 started,
                 output = &output]() mutable {
                    limits.prior_keys = history;
                    try {
                        limits.iteration_callback =
                            [this, &root, started, output](
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
                                    nnue::score_to_wdl(iteration.value, root);

                                std::ostringstream response;
                                response << "info depth "
                                         << iteration.completed_depth
                                         << " seldepth "
                                         << iteration.stats.seldepth << ' ';
                                emit_score(response, iteration.value, root);
                                response << " wdl " << wdl.win << ' '
                                         << wdl.draw << ' ' << wdl.loss
                                         << " nodes " << iteration.stats.nodes
                                         << " nps " << nps
                                         << " hashfull " << table_.hashfull()
                                         << " time " << elapsed_ms
                                         << " pv";
                                for (std::size_t index = 0;
                                     index < iteration.pv_length;
                                     ++index) {
                                    response << ' '
                                             << move_to_uci(
                                                    iteration
                                                        .principal_variation[index]
                                                );
                                }
                                response << '\n';
                                emit(*output, response.str());
                            };

                        const SearchResult result = search(
                            root,
                            table_,
                            network_,
                            limits
                        );

                        std::ostringstream response;
                        const Move best_move = result.best_move.is_none()
                            ? fallback
                            : result.best_move;
                        response << "bestmove " << move_to_uci(best_move) << '\n';
                        emit(*output, response.str());
                    } catch (const std::exception& error) {
                        emit(*output,
                             "info string search failed: "
                                 + std::string(error.what()) + "\nbestmove "
                                 + move_to_uci(fallback) + "\n");
                    }
                    search_running_.store(false, std::memory_order_release);
                }
            );
        } catch (const std::exception& error) {
            search_running_.store(false, std::memory_order_release);
            emit(output, "info string could not start search: "
                       + std::string(error.what()) + "\nbestmove "
                       + move_to_uci(fallback) + "\n");
        }
    }

    Position position_;
    TranspositionTable table_;
    nnue::Network network_;
    std::vector<Key> prior_keys_;
    timeman::TimeManager time_manager_;
    std::atomic_bool stop_requested_{false};
    std::atomic_bool search_running_{false};
    std::thread search_thread_;
    std::mutex output_mutex_;
};

} // namespace

int run_uci(std::istream& input, std::ostream& output) {
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

int run_uci() {
    std::cout << std::unitbuf;
    return run_uci(std::cin, std::cout);
}

} // namespace mors
