// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "platform/numa.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <set>
#include <thread>

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL NUMA: " << message << '\n';
    return condition;
}
}

bool run_numa_tests() {
    using namespace mors::numa;
    // Sparse node ids, two groups, and asymmetric SMT availability. These are
    // already-filtered allowed CPUs, not an assumption of a dense 0..N range.
    const Topology topology{{
        {0, 2, 3, 0}, {0, 10, 3, 0}, {0, 4, 3, 1}, {0, 12, 3, 1},
        {1, 7, 9, 2}, {1, 15, 9, 2}, {1, 9, 9, 3}
    }, {}};
    const auto bindings = plan(topology, 7, Policy::Auto);
    if (!expect(bindings.size() == 7, "every worker needs one binding")) return false;
    std::set<unsigned> first_cores;
    for (std::size_t i = 0; i < 4; ++i) first_cores.insert(bindings[i].core);
    if (!expect(first_cores.size() == 4, "physical cores must precede SMT siblings")
        || !expect(bindings[0].node != bindings[1].node, "spread first workers across nodes")) return false;
    for (std::uint64_t rotation = 0; rotation < 8; ++rotation) {
        const auto rotated = plan(topology, 10, Policy::Auto, rotation);
        for (const auto& cpu : rotated)
            if (!expect(std::find(topology.cpus.begin(), topology.cpus.end(), cpu) != topology.cpus.end(),
                        "rotation and oversubscription must respect allowed CPUs")) return false;
        std::set<std::pair<unsigned, unsigned>> unique;
        for (std::size_t i = 0; i < 7; ++i) unique.emplace(rotated[i].group, rotated[i].index);
        if (!expect(unique.size() == 7, "use every allowed CPU before oversubscribing")) return false;
    }
    if (!expect(plan(topology, 4, Policy::None).empty(), "none must leave scheduling to the OS")
        || !expect(plan({}, 4, Policy::Auto).empty(), "missing topology must fall back")
        || !expect(plan(topology, 0, Policy::Auto).empty(), "empty pool requires no binding")) return false;
    const Topology single{{{0, 2, 5, 0}, {0, 3, 5, 0}}, {}};
    const Topology multi_group{{{0, 63, 5, 0}, {2, 7, 5, 1}}, {}};
    if (!expect(plan(single, 2, Policy::Auto).empty(), "single node/group should use OS scheduling")
        || !expect(plan(multi_group, 2, Policy::Auto).size() == 2,
                   "a single node spanning groups still needs group-aware binding")
        || !expect(topology.describe().find("node 9 group 1 CPUs 7,9,15") != std::string::npos,
                   "report actual sparse CPU ids")) return false;

    const auto detected = Topology::detect();
    if (!detected.cpus.empty()) {
        // Bind only a disposable helper so the test runner's affinity is intact.
        std::exception_ptr error;
        std::thread helper([&] {
            try {
                const auto cpu = detected.cpus.front();
                bind_current_thread(cpu);
                const auto restricted = Topology::detect();
                if (restricted.cpus.size() != 1 || restricted.cpus.front().group != cpu.group
                    || restricted.cpus.front().index != cpu.index)
                    throw std::runtime_error("topology detection broadened thread affinity");
            } catch (...) { error = std::current_exception(); }
        });
        helper.join();
        if (error) {
            try { std::rethrow_exception(error); }
            catch (const std::exception& failure) {
                std::cerr << "FAIL NUMA native affinity: " << failure.what() << '\n';
                return false;
            }
        }
    } else {
        std::cout << "SKIP NUMA native affinity: " << detected.diagnostic << '\n';
    }
    std::cout << "PASS NUMA topology and affinity\n";
    return true;
}
