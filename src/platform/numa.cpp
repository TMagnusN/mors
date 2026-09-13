// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "numa.hpp"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <filesystem>
#include <linux/mempolicy.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace mors::numa {
namespace {
#if defined(_WIN32)
std::vector<unsigned char> relationships(LOGICAL_PROCESSOR_RELATIONSHIP relation) {
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(relation, nullptr, &bytes);
    if (bytes == 0)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    std::vector<unsigned char> data(bytes);
    if (!GetLogicalProcessorInformationEx(relation,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(data.data()), &bytes))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    data.resize(bytes);
    return data;
}

template<class Visitor>
void visit_relationships(const std::vector<unsigned char>& data, Visitor visitor) {
    for (std::size_t offset = 0; offset < data.size();) {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(data.data() + offset);
        if (entry->Size == 0 || entry->Size > data.size() - offset)
            throw std::runtime_error("invalid Windows topology record");
        visitor(*entry);
        offset += entry->Size;
    }
}

std::vector<ULONG> selected_cpu_sets(bool thread) {
    ULONG count = 0;
    const BOOL queried = thread
        ? GetThreadSelectedCpuSets(GetCurrentThread(), nullptr, 0, &count)
        : GetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0, &count);
    if (!queried && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    std::vector<ULONG> ids(count);
    if (count != 0) {
        const BOOL ok = thread
            ? GetThreadSelectedCpuSets(GetCurrentThread(), ids.data(), count, &count)
            : GetProcessDefaultCpuSets(GetCurrentProcess(), ids.data(), count, &count);
        if (!ok)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
    return ids;
}
#elif defined(__linux__)
struct CpuMask final {
    explicit CpuMask(std::size_t count)
        : bytes(CPU_ALLOC_SIZE(count)), data(CPU_ALLOC(count)) {
        if (!data) throw std::bad_alloc();
        CPU_ZERO_S(bytes, data);
    }
    ~CpuMask() { CPU_FREE(data); }
    CpuMask(const CpuMask&) = delete;
    CpuMask& operator=(const CpuMask&) = delete;
    std::size_t bytes;
    cpu_set_t* data;
};

int read_number(const std::filesystem::path& path) {
    int value = -1;
    std::ifstream input(path);
    if (!(input >> value) || value < 0)
        throw std::runtime_error("cannot read CPU topology: " + path.string());
    return value;
}
#endif
} // namespace

Topology Topology::detect() {
    Topology topology;
    try {
#if defined(_WIN32)
        // Use the full node relationship: one NUMA node can span many groups.
        std::map<std::pair<unsigned, unsigned>, unsigned> nodes;
        visit_relationships(relationships(RelationNumaNodeEx), [&](const auto& entry) {
            const auto& node = entry.NumaNode;
            for (WORD g = 0; g < node.GroupCount; ++g) {
                const GROUP_AFFINITY& affinity = node.GroupMasks[g];
                for (unsigned bit = 0; bit < 64; ++bit)
                    if ((affinity.Mask & (KAFFINITY{1} << bit)) != 0)
                        nodes[{affinity.Group, bit}] = node.NodeNumber;
            }
        });
        unsigned core_id = 0;
        visit_relationships(relationships(RelationProcessorCore), [&](const auto& entry) {
            for (WORD g = 0; g < entry.Processor.GroupCount; ++g) {
                const auto& affinity = entry.Processor.GroupMask[g];
                for (unsigned bit = 0; bit < 64; ++bit) {
                    if ((affinity.Mask & (KAFFINITY{1} << bit)) == 0) continue;
                    const auto node = nodes.find({affinity.Group, bit});
                    if (node == nodes.end()) throw std::runtime_error("incomplete NUMA topology");
                    topology.cpus.push_back({affinity.Group, bit, node->second, core_id});
                }
            }
            ++core_id;
        });

        GROUP_AFFINITY thread_affinity{};
        if (!GetThreadGroupAffinity(GetCurrentThread(), &thread_affinity))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        KAFFINITY active_mask = 0;
        for (const auto& cpu : topology.cpus)
            if (cpu.group == thread_affinity.Group) active_mask |= KAFFINITY{1} << cpu.index;
        DWORD_PTR process_mask = 0, system_mask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        // A default group mask is not a restriction to that group on modern
        // Windows. A narrower explicit mask must not be broadened by us.
        const bool restricted_thread = thread_affinity.Mask != active_mask;
        const bool restricted_process = process_mask != 0 && process_mask != system_mask;
        std::erase_if(topology.cpus, [&](const Cpu& cpu) {
            if (!restricted_thread && !restricted_process) return false;
            if (cpu.group != thread_affinity.Group) return true;
            const auto bit = KAFFINITY{1} << cpu.index;
            return (restricted_thread && (thread_affinity.Mask & bit) == 0)
                || (restricted_process && (process_mask & bit) == 0);
        });

        // Thread-selected CPU sets override process-default CPU sets.
        auto selected = selected_cpu_sets(true);
        if (selected.empty()) selected = selected_cpu_sets(false);
        ULONG bytes = 0;
        GetSystemCpuSetInformation(nullptr, 0, &bytes, GetCurrentProcess(), 0);
        if (bytes == 0) throw std::runtime_error("CPU set information unavailable");
        std::vector<unsigned char> data(bytes);
        if (!GetSystemCpuSetInformation(
                reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(data.data()), bytes,
                &bytes, GetCurrentProcess(), 0))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        std::set<std::pair<unsigned, unsigned>> allowed;
        for (std::size_t offset = 0; offset < bytes;) {
            const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(data.data() + offset);
            if (info->Size == 0 || info->Size > bytes - offset)
                throw std::runtime_error("invalid CPU set record");
            if (info->Type == CpuSetInformation
                && (!info->CpuSet.Allocated || info->CpuSet.AllocatedToTargetProcess)
                && (selected.empty() || std::find(selected.begin(), selected.end(), info->CpuSet.Id) != selected.end()))
                allowed.emplace(info->CpuSet.Group, info->CpuSet.LogicalProcessorIndex);
            offset += info->Size;
        }
        std::erase_if(topology.cpus, [&](const Cpu& cpu) {
            return !allowed.contains({cpu.group, cpu.index});
        });
#elif defined(__linux__)
        // Dynamic masks also support sparse CPU ids and machines above 1024 CPUs.
        std::unique_ptr<CpuMask> allowed;
        std::size_t count = 128;
        for (;;) {
            allowed = std::make_unique<CpuMask>(count);
            if (sched_getaffinity(0, allowed->bytes, allowed->data) == 0) break;
            if (errno != EINVAL || count >= (1U << 20))
                throw std::system_error(errno, std::generic_category());
            count *= 2;
        }
        std::map<std::pair<int, int>, unsigned> cores;
        for (std::size_t id = 0; id < count; ++id) {
            if (!CPU_ISSET_S(id, allowed->bytes, allowed->data)) continue;
            const auto path = std::filesystem::path("/sys/devices/system/cpu") / ("cpu" + std::to_string(id));
            const auto core_key = std::pair{
                read_number(path / "topology/physical_package_id"),
                read_number(path / "topology/core_id")};
            auto [core, inserted] = cores.try_emplace(core_key, static_cast<unsigned>(cores.size()));
            (void)inserted;
            unsigned node = 0;
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                const auto name = entry.path().filename().string();
                if (name.starts_with("node") && name.size() > 4)
                    node = static_cast<unsigned>(std::stoul(name.substr(4)));
            }
            topology.cpus.push_back({0, static_cast<unsigned>(id), node, core->second});
        }
#else
        topology.diagnostic = "NUMA topology is unavailable on this platform";
#endif
        if (topology.cpus.empty() && topology.diagnostic.empty())
            topology.diagnostic = "no allowed CPUs found";
    } catch (const std::exception& error) {
        topology.cpus.clear();
        topology.diagnostic = std::string("NUMA detection unavailable: ") + error.what();
    }
    return topology;
}

std::string Topology::describe() const {
    if (cpus.empty()) return "unavailable (OS scheduling)";
    std::map<std::pair<unsigned, unsigned>, std::vector<unsigned>> groups;
    for (const auto& cpu : cpus) groups[{cpu.node, cpu.group}].push_back(cpu.index);
    std::ostringstream text;
    bool first = true;
    for (auto& [key, ids] : groups) {
        std::sort(ids.begin(), ids.end());
        if (!first) text << "; ";
        first = false;
        text << "node " << key.first << " group " << key.second << " CPUs ";
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i != 0) text << ',';
            text << ids[i];
            std::size_t end = i;
            while (end + 1 < ids.size() && ids[end + 1] == ids[end] + 1) ++end;
            if (end != i) text << '-' << ids[end];
            i = end;
        }
    }
    return text.str();
}

std::vector<Cpu> plan(const Topology& topology, std::size_t threads, Policy policy, std::uint64_t rotation) {
    if (policy == Policy::None || topology.cpus.empty() || threads == 0) return {};
    std::set<unsigned> nodes, groups;
    std::map<unsigned, std::map<unsigned, std::vector<Cpu>>> by_node;
    for (const auto& cpu : topology.cpus) {
        nodes.insert(cpu.node);
        groups.insert(cpu.group);
        by_node[cpu.node][cpu.core].push_back(cpu);
    }
    if (nodes.size() == 1 && groups.size() == 1) return {};

    std::vector<std::vector<std::vector<Cpu>>> ordered_nodes;
    std::size_t max_siblings = 0, max_cores = 0;
    for (auto& [node, cores] : by_node) {
        (void)node;
        std::vector<std::vector<Cpu>> ordered;
        for (auto& [core, siblings] : cores) {
            (void)core;
            std::sort(siblings.begin(), siblings.end(), [](const Cpu& a, const Cpu& b) {
                return std::tie(a.group, a.index) < std::tie(b.group, b.index);
            });
            max_siblings = std::max(max_siblings, siblings.size());
            ordered.push_back(std::move(siblings));
        }
        max_cores = std::max(max_cores, ordered.size());
        std::rotate(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(rotation % ordered.size()), ordered.end());
        ordered_nodes.push_back(std::move(ordered));
    }
    std::rotate(ordered_nodes.begin(), ordered_nodes.begin()
        + static_cast<std::ptrdiff_t>(rotation % ordered_nodes.size()), ordered_nodes.end());
    std::vector<Cpu> order;
    for (std::size_t sibling = 0; sibling < max_siblings; ++sibling)
        for (std::size_t core = 0; core < max_cores; ++core)
            for (const auto& node : ordered_nodes)
                if (core < node.size() && sibling < node[core].size())
                    order.push_back(node[core][sibling]);
    std::vector<Cpu> result;
    result.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) result.push_back(order[i % order.size()]);
    return result;
}

std::uint64_t process_rotation() noexcept {
#if defined(_WIN32)
    std::uint64_t value = GetCurrentProcessId();
    value ^= value >> 4;
    value *= 0x9E3779B97F4A7C15ULL;
    return value ^ (value >> 32);
#elif defined(__linux__)
    return static_cast<std::uint64_t>(getpid());
#else
    return 0;
#endif
}

void bind_current_thread(const Cpu& cpu) {
#if defined(_WIN32)
    if (cpu.index >= 64 || cpu.group > 65535) throw BindingError("invalid processor binding");
    GROUP_AFFINITY affinity{};
    affinity.Group = static_cast<WORD>(cpu.group);
    affinity.Mask = KAFFINITY{1} << cpu.index;
    if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr))
        throw BindingError("SetThreadGroupAffinity failed: " + std::to_string(GetLastError()));
#elif defined(__linux__)
    CpuMask affinity(static_cast<std::size_t>(cpu.index) + 1);
    CPU_SET_S(cpu.index, affinity.bytes, affinity.data);
    if (sched_setaffinity(0, affinity.bytes, affinity.data) != 0)
        throw BindingError("sched_setaffinity failed: " + std::to_string(errno));
#else
    (void)cpu;
    throw BindingError("CPU binding is unavailable on this platform");
#endif
}

void* allocate_on_node(std::size_t bytes, unsigned node) {
#if defined(_WIN32)
    void* memory = VirtualAllocExNuma(GetCurrentProcess(), nullptr, bytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, node);
    if (!memory) throw std::bad_alloc();
    return memory;
#elif defined(__linux__)
    void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) throw std::bad_alloc();
    constexpr unsigned bits = sizeof(unsigned long) * 8;
    std::vector<unsigned long> mask;
    try {
        mask.resize(static_cast<std::size_t>(node) / bits + 1);
        mask[node / bits] |= 1UL << (node % bits);
    } catch (...) {
        munmap(memory, bytes);
        throw;
    }
    if (syscall(SYS_mbind, memory, bytes, MPOL_PREFERRED, mask.data(),
                static_cast<unsigned long>(node) + 1, 0UL) != 0) {
        const int error = errno;
        munmap(memory, bytes);
        throw BindingError("mbind failed: " + std::to_string(error));
    }
    return memory;
#else
    (void)bytes;
    (void)node;
    throw BindingError("NUMA allocation is unavailable on this platform");
#endif
}

void free_on_node(void* memory, std::size_t bytes) noexcept {
#if defined(_WIN32)
    (void)bytes;
    if (memory) VirtualFree(memory, 0, MEM_RELEASE);
#elif defined(__linux__)
    if (memory) munmap(memory, bytes);
#else
    (void)memory;
    (void)bytes;
#endif
}
} // namespace mors::numa
