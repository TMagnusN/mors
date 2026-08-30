// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "tt.hpp"

#include <algorithm>
#include <array>
#include <atomic>
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

inline constexpr std::uint16_t EMPTY_SIGNATURE = 0;
inline constexpr std::uint16_t BUSY_SIGNATURE = 0xFFFF;
inline constexpr std::uint64_t LANE_ONES = 0x0001'0001'0001'0001ULL;
inline constexpr std::uint64_t LANE_HIGHS = 0x8000'8000'8000'8000ULL;

[[nodiscard]] constexpr std::uint16_t signature_of(Key key) noexcept {
    const std::uint16_t signature = static_cast<std::uint16_t>(key);
    // Zero marks an empty lane and 0xFFFF marks a writer-owned lane. Folding
    // those two values into neighboring signatures costs two verification
    // values but keeps publication inside the existing 32-byte cluster.
    if (signature == EMPTY_SIGNATURE)
        return 1;
    if (signature == BUSY_SIGNATURE)
        return BUSY_SIGNATURE - 1;
    return signature;
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

[[nodiscard]] constexpr std::uint16_t lane(
    std::uint64_t signatures,
    std::size_t slot
) noexcept {
    assert(slot < ENTRIES_PER_CLUSTER);
    return static_cast<std::uint16_t>(signatures >> (slot * 16));
}

[[nodiscard]] constexpr std::uint64_t set_lane(
    std::uint64_t signatures,
    std::size_t slot,
    std::uint16_t signature
) noexcept {
    assert(slot < ENTRIES_PER_CLUSTER);
    const unsigned shift = static_cast<unsigned>(slot * 16);
    const std::uint64_t mask = 0xFFFFULL << shift;
    return (signatures & ~mask)
         | (static_cast<std::uint64_t>(signature) << shift);
}

[[nodiscard]] constexpr std::uint64_t matching_lanes(
    std::uint64_t signatures,
    std::uint16_t signature
) noexcept {
    const std::uint64_t needle = static_cast<std::uint64_t>(signature) * LANE_ONES;
    const std::uint64_t difference = signatures ^ needle;
    return (difference - LANE_ONES) & ~difference & LANE_HIGHS;
}

[[nodiscard]] constexpr std::uint64_t pack_entry(
    const TTData& data,
    std::uint8_t generation
) noexcept {
    return static_cast<std::uint64_t>(data.move.raw())
         | (static_cast<std::uint64_t>(
                static_cast<std::uint16_t>(static_cast<std::int16_t>(data.value))
            ) << 16)
         | (static_cast<std::uint64_t>(
                static_cast<std::uint16_t>(static_cast<std::int16_t>(data.static_eval))
            ) << 32)
         | (static_cast<std::uint64_t>(encode_depth(data.depth)) << 48)
         | (static_cast<std::uint64_t>(
                pack_flags(data.bound, data.pv, generation)
            ) << 56);
}

[[nodiscard]] constexpr TTData unpack_entry(std::uint64_t payload) noexcept {
    const std::uint16_t move = static_cast<std::uint16_t>(payload);
    const std::uint8_t depth = static_cast<std::uint8_t>(payload >> 48);
    const std::uint8_t flags = static_cast<std::uint8_t>(payload >> 56);
    return {
        .move = std::bit_cast<Move>(move),
        .value = static_cast<Value>(
            static_cast<std::int16_t>(static_cast<std::uint16_t>(payload >> 16))
        ),
        .static_eval = static_cast<Value>(
            static_cast<std::int16_t>(static_cast<std::uint16_t>(payload >> 32))
        ),
        .depth = decode_depth(depth),
        .bound = unpack_bound(flags),
        .pv = unpack_pv(flags)
    };
}

[[nodiscard]] constexpr bool occupied(std::uint64_t payload) noexcept {
    return static_cast<std::uint8_t>(payload >> 48) != 0;
}

} // namespace

namespace detail {

struct alignas(32) TTCluster final {
    TTCluster() noexcept {
        for (std::atomic<std::uint64_t>& entry : entries)
            entry.store(0, std::memory_order_relaxed);
        signatures.store(0, std::memory_order_relaxed);
    }

    // A writer claims one 16-bit signature lane with BUSY_SIGNATURE, stores
    // the complete 64-bit payload, then publishes the final signature with a
    // release CAS. Readers verify the lane before and after loading payload.
    std::array<std::atomic<std::uint64_t>, ENTRIES_PER_CLUSTER> entries;
    std::atomic<std::uint64_t> signatures;
};

static_assert(sizeof(TTCluster) == 32);
static_assert(alignof(TTCluster) == 32);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

} // namespace detail

namespace {

[[nodiscard]] bool claim_slot(
    detail::TTCluster& cluster,
    std::size_t slot,
    std::uint16_t expected_signature
) noexcept {
    std::uint64_t expected = cluster.signatures.load(std::memory_order_acquire);
    while (lane(expected, slot) == expected_signature) {
        const std::uint64_t desired = set_lane(expected, slot, BUSY_SIGNATURE);
        if (cluster.signatures.compare_exchange_weak(
                expected,
                desired,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void publish_slot(
    detail::TTCluster& cluster,
    std::size_t slot,
    std::uint16_t signature
) noexcept {
    std::uint64_t expected = cluster.signatures.load(std::memory_order_relaxed);
    while (lane(expected, slot) == BUSY_SIGNATURE) {
        const std::uint64_t desired = set_lane(expected, slot, signature);
        if (cluster.signatures.compare_exchange_weak(
                expected,
                desired,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return;
        }
    }
    assert(false && "TT writer lost ownership of its signature lane");
}

} // namespace

TTWriter::TTWriter(
    detail::TTCluster* cluster,
    std::uint8_t slot,
    std::uint16_t signature,
    std::uint16_t expected_signature,
    std::uint8_t generation
) noexcept
    : cluster_(cluster),
      signature_(signature),
      expected_signature_(expected_signature),
      slot_(slot),
      generation_(generation) {}

void TTWriter::write(const TTData& data, bool force) const noexcept {
    assert(valid() && slot_ < ENTRIES_PER_CLUSTER);
    assert(is_ok(data.bound));
    assert(data.depth > DEPTH_ENTRY_OFFSET);
    assert(data.depth <= DEPTH_ENTRY_OFFSET + std::numeric_limits<std::uint8_t>::max());
    assert(fits_entry(data.value) && fits_entry(data.static_eval));

    if (!claim_slot(*cluster_, slot_, expected_signature_))
        return;

    const std::uint64_t old_payload = cluster_->entries[slot_].load(
        std::memory_order_relaxed
    );
    const bool has_old_entry = occupied(old_payload);
    const bool same_position = has_old_entry
                            && expected_signature_ == signature_;
    const TTData old_data = has_old_entry ? unpack_entry(old_payload) : TTData{};
    const std::uint8_t old_flags = static_cast<std::uint8_t>(old_payload >> 56);
    const std::uint8_t age = has_old_entry
        ? relative_age(generation_, unpack_generation(old_flags))
        : GENERATION_MASK;

    Move stored_move = data.move;
    if (same_position && stored_move.is_none())
        stored_move = old_data.move;

    if (!force
        && same_position
        && data.bound != BOUND_EXACT
        && age == 0
        && data.depth + DEPTH_MARGIN + (data.pv ? PV_DEPTH_BONUS : 0) <= old_data.depth) {
        if (!data.move.is_none()) {
            TTData move_update = old_data;
            move_update.move = data.move;
            cluster_->entries[slot_].store(
                pack_entry(move_update, unpack_generation(old_flags)),
                std::memory_order_relaxed
            );
        }
        publish_slot(*cluster_, slot_, signature_);
        return;
    }

    const TTData replacement{
        .move = stored_move,
        .value = data.value,
        .static_eval = data.static_eval,
        .depth = data.depth,
        .bound = data.bound,
        .pv = data.pv
    };
    cluster_->entries[slot_].store(
        pack_entry(replacement, generation_),
        std::memory_order_relaxed
    );
    publish_slot(*cluster_, slot_, signature_);
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
    auto replacement = std::make_unique<detail::TTCluster[]>(count);

    table_ = std::move(replacement);
    cluster_count_ = count;
    generation_.store(0, std::memory_order_relaxed);
}

void TranspositionTable::clear() noexcept {
    if (table_) {
        for (std::size_t cluster_index = 0;
             cluster_index < cluster_count_;
             ++cluster_index) {
            detail::TTCluster& cluster = table_[cluster_index];
            for (std::atomic<std::uint64_t>& entry : cluster.entries)
                entry.store(0, std::memory_order_relaxed);
            cluster.signatures.store(0, std::memory_order_relaxed);
        }
    }
    generation_.store(0, std::memory_order_relaxed);
}

void TranspositionTable::new_search() noexcept {
    std::uint8_t current = generation_.load(std::memory_order_relaxed);
    while (!generation_.compare_exchange_weak(
        current,
        static_cast<std::uint8_t>((current + 1) & GENERATION_MASK),
        std::memory_order_relaxed,
        std::memory_order_relaxed
    )) {}
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
    const std::uint8_t generation = generation_.load(std::memory_order_relaxed);

    for (;;) {
        const std::uint64_t signatures = cluster.signatures.load(
            std::memory_order_acquire
        );
        std::uint64_t matches = matching_lanes(signatures, signature);
        bool retry = false;
        while (matches != 0) {
            const int bit = std::countr_zero(matches);
            matches &= matches - 1;
            const std::size_t slot = static_cast<std::size_t>(bit) / 16;
            if (slot >= ENTRIES_PER_CLUSTER)
                continue;

            const std::uint64_t payload = cluster.entries[slot].load(
                std::memory_order_acquire
            );
            const std::uint16_t after = lane(
                cluster.signatures.load(std::memory_order_acquire),
                slot
            );
            if (after != signature) {
                retry = true;
                break;
            }
            if (occupied(payload)) {
                return {
                    .hit = true,
                    .data = unpack_entry(payload),
                    .writer = TTWriter(
                        &cluster,
                        static_cast<std::uint8_t>(slot),
                        signature,
                        signature,
                        generation
                    )
                };
            }
        }
        if (retry)
            continue;

        std::size_t replacement = ENTRIES_PER_CLUSTER;
        std::uint16_t replacement_signature = EMPTY_SIGNATURE;
        int lowest_quality = std::numeric_limits<int>::max();
        for (std::size_t slot = 0; slot < ENTRIES_PER_CLUSTER; ++slot) {
            const std::uint16_t before = lane(signatures, slot);
            if (before == BUSY_SIGNATURE)
                continue;

            const std::uint64_t payload = cluster.entries[slot].load(
                std::memory_order_acquire
            );
            const std::uint16_t after = lane(
                cluster.signatures.load(std::memory_order_acquire),
                slot
            );
            if (after != before) {
                retry = true;
                break;
            }
            if (before == EMPTY_SIGNATURE || !occupied(payload)) {
                replacement = slot;
                replacement_signature = before;
                break;
            }

            const TTData candidate = unpack_entry(payload);
            const std::uint8_t flags = static_cast<std::uint8_t>(payload >> 56);
            const int quality = candidate.depth
                              - AGE_PENALTY * relative_age(
                                    generation,
                                    unpack_generation(flags)
                                );
            if (quality < lowest_quality) {
                lowest_quality = quality;
                replacement = slot;
                replacement_signature = before;
            }
        }
        if (retry || replacement == ENTRIES_PER_CLUSTER)
            continue;

        return {
            .hit = false,
            .data = {},
            .writer = TTWriter(
                &cluster,
                static_cast<std::uint8_t>(replacement),
                signature,
                replacement_signature,
                generation
            )
        };
    }
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

    const std::uint8_t generation = generation_.load(std::memory_order_relaxed);
    const std::size_t sample_clusters = std::min<std::size_t>(cluster_count_, 1000);
    std::size_t occupied_count = 0;
    for (std::size_t cluster_index = 0; cluster_index < sample_clusters; ++cluster_index) {
        const detail::TTCluster& cluster = table_[cluster_index];
        const std::uint64_t signatures = cluster.signatures.load(
            std::memory_order_acquire
        );
        for (std::size_t slot = 0; slot < ENTRIES_PER_CLUSTER; ++slot) {
            const std::uint16_t before = lane(signatures, slot);
            if (before == EMPTY_SIGNATURE || before == BUSY_SIGNATURE)
                continue;
            const std::uint64_t payload = cluster.entries[slot].load(
                std::memory_order_acquire
            );
            const std::uint16_t after = lane(
                cluster.signatures.load(std::memory_order_acquire),
                slot
            );
            if (after != before || !occupied(payload))
                continue;
            const std::uint8_t flags = static_cast<std::uint8_t>(payload >> 56);
            occupied_count += unpack_generation(flags) == generation;
        }
    }

    return static_cast<int>(
        occupied_count * 1000 / (sample_clusters * ENTRIES_PER_CLUSTER)
    );
}

std::size_t TranspositionTable::size_bytes() const noexcept {
    return cluster_count_ * sizeof(detail::TTCluster);
}

} // namespace mors
