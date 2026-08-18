// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "tt.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace mors {
namespace {

inline constexpr std::size_t ENTRIES_PER_CLUSTER = 3;
inline constexpr std::size_t BYTES_PER_MEGABYTE = 1024 * 1024;

inline constexpr std::uint8_t BOUND_MASK = 0x03;
inline constexpr std::uint8_t PV_MASK = 0x04;
inline constexpr unsigned GENERATION_SHIFT = 3;
inline constexpr std::uint8_t GENERATION_MASK = 0x1F;

inline constexpr Depth DEPTH_ENTRY_OFFSET = -3;
inline constexpr int AGE_PENALTY = 8;
inline constexpr int DEPTH_MARGIN = 4;
inline constexpr int PV_DEPTH_BONUS = 2;

inline constexpr std::uint64_t LANE_ONES = 0x0001'0001'0001'0001ULL;
inline constexpr std::uint64_t LANE_HIGHS = 0x8000'8000'8000'8000ULL;

[[nodiscard]] constexpr std::uint16_t signature_of(Key key) noexcept {
    return static_cast<std::uint16_t>(key);
}

[[nodiscard]] constexpr std::uint8_t encode_depth(Depth depth) noexcept {
    assert(depth > DEPTH_ENTRY_OFFSET);
    assert(depth <= DEPTH_ENTRY_OFFSET + std::numeric_limits<std::uint8_t>::max());
    return static_cast<std::uint8_t>(depth - DEPTH_ENTRY_OFFSET);
}

[[nodiscard]] constexpr Depth decode_depth(std::uint8_t depth) noexcept {
    return static_cast<Depth>(depth) + DEPTH_ENTRY_OFFSET;
}

[[nodiscard]] constexpr std::uint8_t pack_flags(
    Bound bound,
    bool pv,
    std::uint8_t generation
) noexcept {
    assert(is_ok(bound) && generation <= GENERATION_MASK);
    return static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(bound)
      | (pv ? PV_MASK : 0U)
      | static_cast<std::uint8_t>(generation << GENERATION_SHIFT)
    );
}

[[nodiscard]] constexpr Bound unpack_bound(std::uint8_t flags) noexcept {
    return static_cast<Bound>(flags & BOUND_MASK);
}

[[nodiscard]] constexpr bool unpack_pv(std::uint8_t flags) noexcept {
    return (flags & PV_MASK) != 0;
}

[[nodiscard]] constexpr std::uint8_t unpack_generation(std::uint8_t flags) noexcept {
    return static_cast<std::uint8_t>(flags >> GENERATION_SHIFT);
}

[[nodiscard]] constexpr std::uint8_t relative_age(
    std::uint8_t current,
    std::uint8_t stored
) noexcept {
    return static_cast<std::uint8_t>((current - stored) & GENERATION_MASK);
}

[[nodiscard]] constexpr bool fits_entry(Value value) noexcept {
    return value >= std::numeric_limits<std::int16_t>::min()
        && value <= std::numeric_limits<std::int16_t>::max();
}

} // namespace

namespace detail {

struct TTEntryData final {
    Move move{};
    std::int16_t value = 0;
    std::int16_t static_eval = 0;
    std::uint8_t depth = 0;
    std::uint8_t flags = 0;

    [[nodiscard]] constexpr bool occupied() const noexcept {
        return depth != 0;
    }

    [[nodiscard]] constexpr TTData read() const noexcept {
        return {
            .move = move,
            .value = static_cast<Value>(value),
            .static_eval = static_cast<Value>(static_eval),
            .depth = decode_depth(depth),
            .bound = unpack_bound(flags),
            .pv = unpack_pv(flags)
        };
    }
};

static_assert(sizeof(TTEntryData) == 8);
static_assert(std::is_trivially_copyable_v<TTEntryData>);

struct alignas(32) TTCluster final {
    // Three independent eight-byte payloads followed by three packed 16-bit
    // signatures. The unused fourth signature lane is the SWAR lookup guard.
    std::array<TTEntryData, ENTRIES_PER_CLUSTER> entries{};
    std::uint64_t signatures = 0;

    [[nodiscard]] constexpr std::uint16_t signature(std::size_t slot) const noexcept {
        assert(slot < ENTRIES_PER_CLUSTER);
        return static_cast<std::uint16_t>(signatures >> (slot * 16));
    }

    constexpr void set_signature(std::size_t slot, std::uint16_t value) noexcept {
        assert(slot < ENTRIES_PER_CLUSTER);
        const unsigned shift = static_cast<unsigned>(slot * 16);
        const std::uint64_t mask = 0xFFFFULL << shift;
        signatures = (signatures & ~mask) | (static_cast<std::uint64_t>(value) << shift);
    }

    [[nodiscard]] constexpr std::uint64_t matching_lanes(std::uint16_t value) const noexcept {
        const std::uint64_t needle = static_cast<std::uint64_t>(value) * LANE_ONES;
        const std::uint64_t difference = signatures ^ needle;
        return (difference - LANE_ONES) & ~difference & LANE_HIGHS;
    }
};

static_assert(sizeof(TTCluster) == 32);
static_assert(alignof(TTCluster) == 32);
static_assert(std::is_trivially_copyable_v<TTCluster>);

} // namespace detail

TTWriter::TTWriter(
    detail::TTCluster* cluster,
    std::uint8_t slot,
    std::uint16_t signature,
    std::uint8_t generation
) noexcept
    : cluster_(cluster),
      signature_(signature),
      slot_(slot),
      generation_(generation) {}

void TTWriter::write(const TTData& data, bool force) const noexcept {
    assert(valid() && slot_ < ENTRIES_PER_CLUSTER);
    assert(is_ok(data.bound));
    assert(data.depth > DEPTH_ENTRY_OFFSET);
    assert(data.depth <= DEPTH_ENTRY_OFFSET + std::numeric_limits<std::uint8_t>::max());
    assert(fits_entry(data.value) && fits_entry(data.static_eval));

    detail::TTEntryData& entry = cluster_->entries[slot_];
    const bool same_position = entry.occupied()
                            && cluster_->signature(slot_) == signature_;
    const std::uint8_t age = entry.occupied()
        ? relative_age(generation_, unpack_generation(entry.flags))
        : GENERATION_MASK;

    Move stored_move = data.move;
    if (same_position && stored_move.is_none())
        stored_move = entry.move;

    if (!force
        && same_position
        && data.bound != BOUND_EXACT
        && age == 0
        && data.depth + DEPTH_MARGIN + (data.pv ? PV_DEPTH_BONUS : 0) <= decode_depth(entry.depth)) {
        if (!data.move.is_none())
            entry.move = data.move;
        return;
    }

    entry = {
        .move = stored_move,
        .value = static_cast<std::int16_t>(data.value),
        .static_eval = static_cast<std::int16_t>(data.static_eval),
        .depth = encode_depth(data.depth),
        .flags = pack_flags(data.bound, data.pv, generation_)
    };
    cluster_->set_signature(slot_, signature_);
}

TranspositionTable::TranspositionTable() noexcept = default;

TranspositionTable::TranspositionTable(std::size_t megabytes) {
    resize(megabytes);
}

TranspositionTable::~TranspositionTable() = default;

void TranspositionTable::resize(std::size_t megabytes) {
    if (megabytes == 0)
        throw std::invalid_argument("TT size must be at least one megabyte");
    if (megabytes > std::numeric_limits<std::size_t>::max() / BYTES_PER_MEGABYTE)
        throw std::length_error("TT size is too large");

    const std::size_t bytes = megabytes * BYTES_PER_MEGABYTE;
    const std::size_t count = bytes / sizeof(detail::TTCluster);
    auto replacement = std::make_unique_for_overwrite<detail::TTCluster[]>(count);
    std::fill_n(replacement.get(), count, detail::TTCluster{});

    table_ = std::move(replacement);
    cluster_count_ = count;
    generation_ = 0;
}

void TranspositionTable::clear() noexcept {
    if (table_)
        std::fill_n(table_.get(), cluster_count_, detail::TTCluster{});
    generation_ = 0;
}

void TranspositionTable::new_search() noexcept {
    generation_ = static_cast<std::uint8_t>((generation_ + 1) & GENERATION_MASK);
}

std::size_t TranspositionTable::index(Key key) const noexcept {
    assert(cluster_count_ != 0);

    // Low 16 bits belong exclusively to the verification signature. Reduce
    // the remaining uniformly distributed 48 bits into the cluster range so
    // the index and signature never reuse key information.
    const std::uint64_t index_key = key >> 16;

#if defined(__SIZEOF_INT128__)
    __extension__ using UInt128 = unsigned __int128;
    return static_cast<std::size_t>(
        (static_cast<UInt128>(index_key) * cluster_count_) >> 48
    );
#else
#error "MORS TT requires a compiler with unsigned 128-bit integer support"
#endif
}

TTProbe TranspositionTable::probe(Key key) noexcept {
    assert(table_ && cluster_count_ != 0);
    detail::TTCluster& cluster = table_[index(key)];
    const std::uint16_t signature = signature_of(key);

    std::uint64_t matches = cluster.matching_lanes(signature);
    while (matches != 0) {
        const int bit = std::countr_zero(matches);
        matches &= matches - 1;
        const std::size_t slot = static_cast<std::size_t>(bit) / 16;

        if (slot < ENTRIES_PER_CLUSTER
            && cluster.signature(slot) == signature
            && cluster.entries[slot].occupied()) {
            return {
                .hit = true,
                .data = cluster.entries[slot].read(),
                .writer = TTWriter(
                    &cluster,
                    static_cast<std::uint8_t>(slot),
                    signature,
                    generation_
                )
            };
        }
    }

    std::size_t replacement = 0;
    int lowest_quality = std::numeric_limits<int>::max();
    for (std::size_t slot = 0; slot < ENTRIES_PER_CLUSTER; ++slot) {
        const detail::TTEntryData& candidate = cluster.entries[slot];
        if (!candidate.occupied()) {
            replacement = slot;
            break;
        }

        const int quality = decode_depth(candidate.depth)
                          - AGE_PENALTY * relative_age(
                                generation_,
                                unpack_generation(candidate.flags)
                            );
        if (quality < lowest_quality) {
            lowest_quality = quality;
            replacement = slot;
        }
    }

    return {
        .hit = false,
        .data = {},
        .writer = TTWriter(
            &cluster,
            static_cast<std::uint8_t>(replacement),
            signature,
            generation_
        )
    };
}

void TranspositionTable::prefetch(Key key) const noexcept {
    assert(table_ && cluster_count_ != 0);
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(&table_[index(key)], 0, 3);
#else
    (void)key;
#endif
}

int TranspositionTable::hashfull() const noexcept {
    if (!table_ || cluster_count_ == 0)
        return 0;

    const std::size_t sample_clusters = std::min<std::size_t>(cluster_count_, 1000);
    std::size_t occupied = 0;
    for (std::size_t cluster_index = 0; cluster_index < sample_clusters; ++cluster_index) {
        for (const detail::TTEntryData& entry : table_[cluster_index].entries) {
            occupied += entry.occupied()
                     && unpack_generation(entry.flags) == generation_;
        }
    }

    return static_cast<int>(
        occupied * 1000 / (sample_clusters * ENTRIES_PER_CLUSTER)
    );
}

std::size_t TranspositionTable::size_bytes() const noexcept {
    return cluster_count_ * sizeof(detail::TTCluster);
}

} // namespace mors
