// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mors::numa {

enum class Policy { Auto, None };

struct Cpu final {
    unsigned group = 0; // Windows processor group; zero on Linux.
    unsigned index = 0; // Group-relative on Windows, OS CPU id on Linux.
    unsigned node = 0;
    unsigned core = 0;  // Unique within this topology.
    bool operator==(const Cpu&) const = default;
};

struct Topology final {
    std::vector<Cpu> cpus; // Only CPUs allowed to the detecting thread/process.
    std::string diagnostic;
    [[nodiscard]] static Topology detect();
    [[nodiscard]] std::string describe() const;
};

// Pure planning, also used with synthetic topologies in tests. Exhaust physical
// cores before SMT siblings; rotate placement between processes. An empty plan
// means OS scheduling. Each binding always belongs to the allowed CPU set.
[[nodiscard]] std::vector<Cpu> plan(
    const Topology& topology, std::size_t threads, Policy policy,
    std::uint64_t rotation = 0
);
[[nodiscard]] std::uint64_t process_rotation() noexcept;

class BindingError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
void bind_current_thread(const Cpu& cpu);

// Dedicated page allocations avoid reusing heap pages first touched elsewhere.
// Node is a preference, not a guarantee of physical page residency.
[[nodiscard]] void* allocate_on_node(std::size_t bytes, unsigned node);
void free_on_node(void* memory, std::size_t bytes) noexcept;

} // namespace mors::numa
