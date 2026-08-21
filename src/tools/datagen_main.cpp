// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "bullet_format.hpp"

#include "chess/attacks.hpp"
#include "chess/movegen.hpp"
#include "eval/nnue/network.hpp"
#include "eval/nnue/wdl.hpp"
#include "search/score.hpp"
#include "search/search.hpp"
#include "search/tt.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mors {
namespace {

static_assert(std::endian::native == std::endian::little);

struct Config final {
    std::filesystem::path book;
    std::filesystem::path output;
    std::filesystem::path network = "networks/mors-p2h32-s14400M-o3183M-c+frc.mnue";
    std::uint64_t positions = 0;
    std::uint64_t nodes = 50'000;
    std::uint64_t seed = 1;
    std::size_t hash_mb = 16;
    unsigned workers = 1;
    int extend_min = 1;
    int extend_max = 40;
    bool append = false;
};

class SplitMix64 final {
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        std::uint64_t value = (state_ += 0x9E37'79B9'7F4A'7C15ULL);
        value = (value ^ (value >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return value ^ (value >> 31);
    }

    [[nodiscard]] std::uint64_t bounded(std::uint64_t bound) noexcept {
        if (bound <= 1)
            return 0;
        const std::uint64_t threshold = -bound % bound;
        for (;;) {
            const std::uint64_t value = next();
            if (value >= threshold)
                return value % bound;
        }
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] std::string usage() {
    return
        "MORS search-distillation data generator\n"
        "usage: mors-datagen-avx2.exe --book PATH --output PATH "
        "--positions N [options]\n\n"
        "  --network PATH      P2-H32 teacher network "
        "(default networks/mors-p2h32-s14400M-o3183M-c+frc.mnue)\n"
        "  --nodes N           search nodes per generated position "
        "(default 50000)\n"
        "  --hash MB           TT megabytes per worker (default 16)\n"
        "  --workers N         parallel generator workers (default 1)\n"
        "  --extend-min N      minimum random legal plies (default 1)\n"
        "  --extend-max N      maximum random legal plies (default 40)\n"
        "  --seed N            deterministic base RNG seed (default 1)\n"
        "  --append            append complete 32-byte records\n";
}

template<typename Integer>
[[nodiscard]] Integer parse_integer(
    std::string_view option,
    std::string_view text
) {
    Integer value{};
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [pointer, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || pointer != end)
        throw std::runtime_error(std::string(option) + " expects an integer");
    return value;
}

[[nodiscard]] Config parse_config(int argc, char* argv[]) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--help" || option == "-h") {
            std::cout << usage();
            std::exit(0);
        }
        if (option == "--append") {
            config.append = true;
            continue;
        }
        if (index + 1 >= argc)
            throw std::runtime_error(std::string(option) + " requires a value");
        const std::string_view value = argv[++index];

        if (option == "--book")
            config.book = value;
        else if (option == "--output")
            config.output = value;
        else if (option == "--network")
            config.network = value;
        else if (option == "--positions")
            config.positions = parse_integer<std::uint64_t>(option, value);
        else if (option == "--nodes")
            config.nodes = parse_integer<std::uint64_t>(option, value);
        else if (option == "--seed")
            config.seed = parse_integer<std::uint64_t>(option, value);
        else if (option == "--hash")
            config.hash_mb = parse_integer<std::size_t>(option, value);
        else if (option == "--workers")
            config.workers = parse_integer<unsigned>(option, value);
        else if (option == "--extend-min")
            config.extend_min = parse_integer<int>(option, value);
        else if (option == "--extend-max")
            config.extend_max = parse_integer<int>(option, value);
        else
            throw std::runtime_error("unknown option: " + std::string(option));
    }

    if (config.book.empty())
        throw std::runtime_error("--book is required");
    if (config.output.empty())
        throw std::runtime_error("--output is required");
    if (config.positions == 0)
        throw std::runtime_error("--positions must be greater than zero");
    if (config.nodes == 0)
        throw std::runtime_error("--nodes must be greater than zero");
    if (config.hash_mb == 0)
        throw std::runtime_error("--hash must be greater than zero");
    if (config.workers == 0)
        throw std::runtime_error("--workers must be greater than zero");
    if (config.extend_min < 1 || config.extend_max < config.extend_min
        || config.extend_max > MAX_PLY) {
        throw std::runtime_error(
            "extension range must satisfy 1 <= min <= max <= MAX_PLY"
        );
    }
    return config;
}

[[nodiscard]] std::string first_six_fen_fields(std::string_view line) {
    std::string fen;
    std::size_t cursor = 0;
    for (int field = 0; field < 6; ++field) {
        while (cursor < line.size() && line[cursor] == ' ')
            ++cursor;
        const std::size_t begin = cursor;
        while (cursor < line.size() && line[cursor] != ' ')
            ++cursor;
        if (begin == cursor)
            return {};
        if (!fen.empty())
            fen += ' ';
        fen.append(line.substr(begin, cursor - begin));
    }
    return fen;
}

[[nodiscard]] std::vector<Position> load_book(
    const std::filesystem::path& path
) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("failed to open book: " + path.string());

    std::vector<Position> positions;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#')
            continue;
        const std::string fen = first_six_fen_fields(
            std::string_view(line).substr(first)
        );
        auto parsed = Position::from_fen(fen);
        if (!parsed) {
            throw std::runtime_error(
                "invalid book position at line " + std::to_string(line_number)
                + ": " + parsed.error()
            );
        }
        positions.push_back(std::move(*parsed));
    }
    if (positions.empty())
        throw std::runtime_error("book contains no positions");
    return positions;
}

[[nodiscard]] std::uint8_t sample_white_result(
    const nnue::WdlTriplet& stm_wdl,
    Color side,
    SplitMix64& random
) noexcept {
    const int white_win = side == WHITE ? stm_wdl.win : stm_wdl.loss;
    const int draw = stm_wdl.draw;
    const int sample = static_cast<int>(random.bounded(1'000));
    if (sample < white_win)
        return 2;
    if (sample < white_win + draw)
        return 1;
    return 0;
}

struct SharedOutput final {
    std::ofstream stream;
    std::mutex mutex;
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> next_report{0};
    std::atomic_bool failed{false};
    std::string error;
};

[[nodiscard]] bool reserve_record(
    SharedOutput& output,
    std::uint64_t target,
    std::uint64_t& index
) noexcept {
    index = output.produced.load(std::memory_order_relaxed);
    while (index < target) {
        if (output.produced.compare_exchange_weak(
                index,
                index + 1,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void write_record(
    SharedOutput& output,
    const tools::BulletChessBoard& record
) {
    std::scoped_lock lock(output.mutex);
    output.stream.write(
        reinterpret_cast<const char*>(&record),
        sizeof(record)
    );
    if (!output.stream) {
        output.failed.store(true, std::memory_order_relaxed);
        output.error = "failed while writing output data";
    }
}

void generate_worker(
    unsigned worker_id,
    const Config& config,
    const std::vector<Position>& book,
    const nnue::Network& network,
    SharedOutput& output,
    std::chrono::steady_clock::time_point started
) {
    SplitMix64 random(
        config.seed
        + 0x9E37'79B9'7F4A'7C15ULL * (std::uint64_t(worker_id) + 1)
    );
    TranspositionTable table(config.hash_mb);
    std::vector<Key> prior_keys;
    prior_keys.reserve(static_cast<std::size_t>(config.extend_max));
    const std::uint64_t report_interval = std::max<std::uint64_t>(
        1'000,
        config.positions / 1'000
    );

    while (output.produced.load(std::memory_order_relaxed) < config.positions
           && !output.failed.load(std::memory_order_relaxed)) {
        Position position = book[static_cast<std::size_t>(
            random.bounded(book.size())
        )];
        prior_keys.clear();
        const int extension = config.extend_min + static_cast<int>(
            random.bounded(
                static_cast<std::uint64_t>(
                    config.extend_max - config.extend_min + 1
                )
            )
        );

        for (int ply = 0; ply < extension; ++ply) {
            MoveList moves;
            generate_legal(position, moves);
            if (moves.empty())
                break;

            prior_keys.push_back(position.key());
            const Move move = moves[static_cast<std::size_t>(
                random.bounded(moves.size())
            )];
            StateInfo state;
            position.do_move(move, state);

            SearchLimits limits;
            limits.max_depth = MAX_PLY;
            limits.max_nodes = config.nodes;
            limits.prior_keys = prior_keys;
            const SearchResult result = search(
                position,
                table,
                network,
                limits
            );
            if (result.completed_depth == 0 || result.value == VALUE_NONE)
                continue;

            const nnue::WdlTriplet wdl = nnue::score_to_wdl(
                result.value,
                position
            );
            const std::uint8_t white_result = sample_white_result(
                wdl,
                position.side_to_move(),
                random
            );
            const int score_cp = nnue::score_to_cp(result.value, position);
            const tools::BulletChessBoard record = tools::encode_bullet_record(
                position,
                score_cp,
                white_result
            );

            std::uint64_t record_index = 0;
            if (!reserve_record(output, config.positions, record_index))
                return;
            write_record(output, record);
            if (output.failed.load(std::memory_order_relaxed))
                return;

            const std::uint64_t count = record_index + 1;
            std::uint64_t expected = output.next_report.load(
                std::memory_order_relaxed
            );
            if (count >= expected
                && output.next_report.compare_exchange_strong(
                    expected,
                    count + report_interval,
                    std::memory_order_relaxed)) {
                const double seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started
                ).count();
                std::scoped_lock lock(output.mutex);
                std::cerr << "positions " << count << '/' << config.positions
                          << " | " << static_cast<std::uint64_t>(
                                 static_cast<double>(count) / std::max(0.001, seconds)
                             )
                          << " pos/sec\n";
            }
        }
    }
}

int run(int argc, char* argv[]) {
    const Config config = parse_config(argc, argv);
    initialize_attacks();
    const std::vector<Position> book = load_book(config.book);

    auto loaded = nnue::Network::load(config.network);
    if (!loaded)
        throw std::runtime_error("network load failed: " + loaded.error());
    const nnue::Network network = std::move(*loaded);

    if (!config.output.parent_path().empty())
        std::filesystem::create_directories(config.output.parent_path());
    if (config.append && std::filesystem::exists(config.output)) {
        const std::uintmax_t size = std::filesystem::file_size(config.output);
        if (size % sizeof(tools::BulletChessBoard) != 0) {
            throw std::runtime_error(
                "append target ends with a partial BulletFormat record"
            );
        }
    }

    SharedOutput output;
    output.stream.open(
        config.output,
        std::ios::binary
        | (config.append ? std::ios::app : std::ios::trunc)
    );
    if (!output.stream)
        throw std::runtime_error("failed to open output: " + config.output.string());
    output.next_report.store(
        std::min<std::uint64_t>(1'000, config.positions),
        std::memory_order_relaxed
    );

    std::cerr << "book positions : " << book.size() << '\n'
              << "target records : " << config.positions << '\n'
              << "nodes/record   : " << config.nodes << '\n'
              << "extension      : " << config.extend_min << ".."
              << config.extend_max << " plies\n"
              << "workers         : " << config.workers << '\n'
              << "hash/worker MB  : " << config.hash_mb << '\n'
              << "white WDL       : sampled from teacher search WDL\n"
              << "record format   : bulletformat ChessBoard (32 bytes)\n";

    const auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    workers.reserve(config.workers);
    for (unsigned id = 0; id < config.workers; ++id) {
        workers.emplace_back(
            generate_worker,
            id,
            std::cref(config),
            std::cref(book),
            std::cref(network),
            std::ref(output),
            started
        );
    }
    for (std::thread& worker : workers)
        worker.join();
    output.stream.flush();
    if (output.failed.load(std::memory_order_relaxed))
        throw std::runtime_error(output.error);
    if (!output.stream)
        throw std::runtime_error("failed to flush output data");

    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started
    ).count();
    const std::uint64_t count = output.produced.load(std::memory_order_relaxed);
    std::cerr << "done: " << count << " records, "
              << static_cast<std::uint64_t>(
                     static_cast<double>(count) / std::max(0.001, seconds)
                 )
              << " pos/sec, " << seconds << " seconds\n";
    return count == config.positions ? 0 : 1;
}

} // namespace
} // namespace mors

int main(int argc, char* argv[]) {
    try {
        return mors::run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
