// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace mros::nnue {

struct P2H32 final {
    static constexpr std::size_t COARSE_BUCKETS = 16;
    static constexpr std::size_t FINE_BUCKETS = 32;
    static constexpr std::size_t OUTPUT_BUCKETS = 64;

    static constexpr std::size_t COARSE_INPUTS = 10'240;
    static constexpr std::size_t FINE_INPUTS = 22'528;
    static constexpr std::size_t INPUTS = COARSE_INPUTS + FINE_INPUTS;

    static constexpr std::size_t COARSE_WIDTH = 768;
    static constexpr std::size_t FINE_WIDTH = 256;
    static constexpr std::size_t WIDTH = COARSE_WIDTH + FINE_WIDTH;

    static constexpr std::int32_t QA = 255;
    static constexpr std::int32_t QB = 64;
    static constexpr std::int32_t DEFAULT_SCALE = 400;

    static constexpr std::uint32_t FILE_MAGIC = 0x4555'4E4D;
    static constexpr std::uint32_t FILE_VERSION = 1;
    static constexpr std::uint32_t ARCHITECTURE_ID = 9;
    static constexpr std::size_t HEADER_BYTES = 40;

    static constexpr std::size_t COARSE_WEIGHT_COUNT = COARSE_INPUTS * COARSE_WIDTH;
    static constexpr std::size_t COARSE_BIAS_COUNT = COARSE_WIDTH;
    static constexpr std::size_t FINE_WEIGHT_COUNT = FINE_INPUTS * FINE_WIDTH;
    static constexpr std::size_t FINE_BIAS_COUNT = FINE_WIDTH;
    static constexpr std::size_t OUTPUT_WEIGHT_COUNT = OUTPUT_BUCKETS * 2 * WIDTH;
    static constexpr std::size_t OUTPUT_BIAS_COUNT = OUTPUT_BUCKETS;

    static constexpr std::size_t PAYLOAD_BYTES =
        2 * (COARSE_WEIGHT_COUNT
           + COARSE_BIAS_COUNT
           + FINE_WEIGHT_COUNT
           + FINE_BIAS_COUNT
           + OUTPUT_WEIGHT_COUNT
           + OUTPUT_BIAS_COUNT);
};

static_assert(P2H32::INPUTS == 32'768);
static_assert(P2H32::WIDTH == 1'024);
static_assert(P2H32::PAYLOAD_BYTES == 27'527'296);

class Network final {
public:
    Network();
    ~Network();

    Network(Network&&) noexcept;
    Network& operator=(Network&&) noexcept;
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    [[nodiscard]] static std::expected<Network, std::string> load(
        const std::filesystem::path& path
    );

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::int32_t scale() const noexcept;
    [[nodiscard]] const std::filesystem::path& source() const noexcept;
    [[nodiscard]] std::size_t memory_bytes() const noexcept;

    [[nodiscard]] std::span<const std::int16_t> coarse_weights() const noexcept;
    [[nodiscard]] std::span<const std::int16_t> coarse_biases() const noexcept;
    [[nodiscard]] std::span<const std::int16_t> fine_weights() const noexcept;
    [[nodiscard]] std::span<const std::int16_t> fine_biases() const noexcept;
    [[nodiscard]] std::span<const std::int16_t> output_weights() const noexcept;
    [[nodiscard]] std::span<const std::int16_t> output_biases() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mros::nnue
