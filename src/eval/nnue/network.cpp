// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "network.hpp"
#include "platform/numa.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <istream>
#include <limits>
#include <new>
#include <streambuf>
#include <stdexcept>
#include <utility>

namespace mors::nnue {
namespace {

constexpr std::size_t ALIGNMENT = 64;

class AlignedI16Buffer final {
public:
    AlignedI16Buffer() noexcept = default;

    explicit AlignedI16Buffer(std::size_t size, std::optional<unsigned> node = std::nullopt)
        : data_(static_cast<std::int16_t*>(node
              ? numa::allocate_on_node(size * sizeof(std::int16_t), *node)
              : ::operator new[](size * sizeof(std::int16_t), std::align_val_t{ALIGNMENT}))),
          size_(size), node_allocated_(node.has_value()) {}

    ~AlignedI16Buffer() { release(); }

    AlignedI16Buffer(AlignedI16Buffer&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          node_allocated_(std::exchange(other.node_allocated_, false)) {}

    AlignedI16Buffer& operator=(AlignedI16Buffer&& other) noexcept {
        if (this == &other)
            return *this;
        release();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        node_allocated_ = std::exchange(other.node_allocated_, false);
        return *this;
    }

    AlignedI16Buffer(const AlignedI16Buffer&) = delete;
    AlignedI16Buffer& operator=(const AlignedI16Buffer&) = delete;

    [[nodiscard]] std::int16_t* data() noexcept { return data_; }
    [[nodiscard]] const std::int16_t* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] std::span<const std::int16_t> span() const noexcept {
        return {data_, size_};
    }

private:
    void release() noexcept {
        if (node_allocated_)
            numa::free_on_node(data_, size_ * sizeof(std::int16_t));
        else
            ::operator delete[](data_, std::align_val_t{ALIGNMENT});
    }
    std::int16_t* data_ = nullptr;
    std::size_t size_ = 0;
    bool node_allocated_ = false;
};

[[nodiscard]] constexpr std::uint32_t read_u32_le(
    const std::array<std::byte, P2H32::HEADER_BYTES>& bytes,
    std::size_t offset
) noexcept {
    return std::uint32_t(std::to_integer<std::uint8_t>(bytes[offset]))
         | (std::uint32_t(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8)
         | (std::uint32_t(std::to_integer<std::uint8_t>(bytes[offset + 2])) << 16)
         | (std::uint32_t(std::to_integer<std::uint8_t>(bytes[offset + 3])) << 24);
}

[[nodiscard]] constexpr std::int32_t read_i32_le(
    const std::array<std::byte, P2H32::HEADER_BYTES>& bytes,
    std::size_t offset
) noexcept {
    return std::bit_cast<std::int32_t>(read_u32_le(bytes, offset));
}

[[nodiscard]] bool read_buffer(std::istream& input, AlignedI16Buffer& buffer) {
    static_assert(sizeof(std::int16_t) == 2);
    const std::size_t bytes = buffer.size() * sizeof(std::int16_t);
    if (bytes > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        return false;

    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(bytes));
    if (!input)
        return false;

    if constexpr (std::endian::native == std::endian::big) {
        for (std::size_t index = 0; index < buffer.size(); ++index) {
            const auto bits = std::bit_cast<std::uint16_t>(buffer.data()[index]);
            buffer.data()[index] = std::bit_cast<std::int16_t>(std::byteswap(bits));
        }
    }
    return true;
}

class MemoryBuffer final : public std::streambuf {
public:
    explicit MemoryBuffer(std::span<const std::byte> bytes) {
        char* const begin = const_cast<char*>(
            reinterpret_cast<const char*>(bytes.data())
        );
        setg(begin, begin, begin + bytes.size());
    }
};

} // namespace

struct Network::Impl final {
    explicit Impl(std::optional<unsigned> node = std::nullopt)
        : coarse_weights(P2H32::COARSE_WEIGHT_COUNT, node),
          coarse_biases(P2H32::COARSE_BIAS_COUNT, node),
          fine_weights(P2H32::FINE_WEIGHT_COUNT, node),
          fine_biases(P2H32::FINE_BIAS_COUNT, node),
          output_weights(P2H32::OUTPUT_WEIGHT_COUNT, node),
          output_biases(P2H32::OUTPUT_BIAS_COUNT, node) {}
    AlignedI16Buffer coarse_weights;
    AlignedI16Buffer coarse_biases;
    AlignedI16Buffer fine_weights;
    AlignedI16Buffer fine_biases;
    AlignedI16Buffer output_weights;
    AlignedI16Buffer output_biases;
    std::filesystem::path source;
    std::int32_t scale = P2H32::DEFAULT_SCALE;
    bool fast_output_weights = false;
};

Network::Network() = default;
Network::~Network() = default;
Network::Network(Network&&) noexcept = default;
Network& Network::operator=(Network&&) noexcept = default;

std::expected<Network, std::string> Network::load(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error)
        return std::unexpected("cannot determine NNUE file size: " + path.string());

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::unexpected("cannot open NNUE file: " + path.string());

    return load(input, file_size, path);
}

std::expected<Network, std::string> Network::load(
    std::span<const std::byte> bytes,
    std::filesystem::path source
) {
    MemoryBuffer buffer(bytes);
    std::istream input(&buffer);
    return load(input, bytes.size(), std::move(source));
}

std::expected<Network, std::string> Network::load(
    std::istream& input,
    std::uintmax_t size,
    std::filesystem::path source
) {
    const bool raw_payload = size == P2H32::PAYLOAD_BYTES;
    const bool headered = size == P2H32::PAYLOAD_BYTES + P2H32::HEADER_BYTES;
    if (!raw_payload && !headered) {
        return std::unexpected(
            "invalid P2-H32 file size: expected "
            + std::to_string(P2H32::PAYLOAD_BYTES) + " or "
            + std::to_string(P2H32::PAYLOAD_BYTES + P2H32::HEADER_BYTES)
            + " bytes, got " + std::to_string(size)
        );
    }

    std::int32_t scale = P2H32::DEFAULT_SCALE;
    if (headered) {
        std::array<std::byte, P2H32::HEADER_BYTES> header{};
        input.read(reinterpret_cast<char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
        if (!input)
            return std::unexpected("truncated P2-H32 header");

        const bool valid_header =
            read_u32_le(header, 0) == P2H32::FILE_MAGIC
            && read_u32_le(header, 4) == P2H32::FILE_VERSION
            && read_u32_le(header, 8) == P2H32::ARCHITECTURE_ID
            && read_u32_le(header, 12) == P2H32::INPUTS
            && read_u32_le(header, 16) == P2H32::WIDTH
            && read_u32_le(header, 20) == P2H32::FINE_BUCKETS
            && read_u32_le(header, 24) == P2H32::OUTPUT_BUCKETS
            && read_i32_le(header, 32) == P2H32::QA
            && read_i32_le(header, 36) == P2H32::QB;

        if (!valid_header)
            return std::unexpected("P2-H32 header does not match architecture");

        scale = read_i32_le(header, 28);
        if (scale <= 0)
            return std::unexpected("P2-H32 scale must be positive");
    }

    Network network;
    network.impl_ = std::make_unique<Impl>();
    network.impl_->source = std::move(source);
    network.impl_->scale = scale;

    if (!read_buffer(input, network.impl_->coarse_weights)
        || !read_buffer(input, network.impl_->coarse_biases)
        || !read_buffer(input, network.impl_->fine_weights)
        || !read_buffer(input, network.impl_->fine_biases)
        || !read_buffer(input, network.impl_->output_weights)
        || !read_buffer(input, network.impl_->output_biases)) {
        return std::unexpected("truncated P2-H32 payload");
    }

    if (input.peek() != std::char_traits<char>::eof())
        return std::unexpected("P2-H32 file contains trailing data");

    const auto weights = network.impl_->output_weights.span();
    network.impl_->fast_output_weights = std::all_of(
        weights.begin(), weights.end(), [](std::int16_t weight) {
            return weight >= -P2H32::FAST_OUTPUT_WEIGHT_MAX
                && weight <= P2H32::FAST_OUTPUT_WEIGHT_MAX;
        }
    );
    return network;
}

Network Network::clone(std::optional<unsigned> node) const {
    if (!valid()) throw std::invalid_argument("cannot clone an invalid network");
    Network replica;
    replica.impl_ = std::make_unique<Impl>(node);
    const auto copy = [](const AlignedI16Buffer& from, AlignedI16Buffer& to) {
        std::copy_n(from.data(), from.size(), to.data());
    };
    copy(impl_->coarse_weights, replica.impl_->coarse_weights);
    copy(impl_->coarse_biases, replica.impl_->coarse_biases);
    copy(impl_->fine_weights, replica.impl_->fine_weights);
    copy(impl_->fine_biases, replica.impl_->fine_biases);
    copy(impl_->output_weights, replica.impl_->output_weights);
    copy(impl_->output_biases, replica.impl_->output_biases);
    replica.impl_->source = impl_->source;
    replica.impl_->scale = impl_->scale;
    replica.impl_->fast_output_weights = impl_->fast_output_weights;
    return replica;
}

bool Network::valid() const noexcept {
    return impl_ != nullptr;
}

bool Network::has_fast_output_weights() const noexcept {
    return impl_ && impl_->fast_output_weights;
}

std::int32_t Network::scale() const noexcept {
    return impl_ ? impl_->scale : 0;
}

const std::filesystem::path& Network::source() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->source : empty;
}

std::size_t Network::memory_bytes() const noexcept {
    return valid() ? P2H32::PAYLOAD_BYTES : 0;
}

std::span<const std::int16_t> Network::coarse_weights() const noexcept {
    return impl_ ? impl_->coarse_weights.span() : std::span<const std::int16_t>{};
}

std::span<const std::int16_t> Network::coarse_biases() const noexcept {
    return impl_ ? impl_->coarse_biases.span() : std::span<const std::int16_t>{};
}

std::span<const std::int16_t> Network::fine_weights() const noexcept {
    return impl_ ? impl_->fine_weights.span() : std::span<const std::int16_t>{};
}

std::span<const std::int16_t> Network::fine_biases() const noexcept {
    return impl_ ? impl_->fine_biases.span() : std::span<const std::int16_t>{};
}

std::span<const std::int16_t> Network::output_weights() const noexcept {
    return impl_ ? impl_->output_weights.span() : std::span<const std::int16_t>{};
}

std::span<const std::int16_t> Network::output_biases() const noexcept {
    return impl_ ? impl_->output_biases.span() : std::span<const std::int16_t>{};
}

} // namespace mors::nnue
