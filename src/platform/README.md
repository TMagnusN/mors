# NUMA placement

`numa.hpp/.cpp` supplies topology discovery, pure worker planning, CPU binding,
and dedicated preferred-node page allocation. Search policy remains in the
search module. Supported native backends are modern Windows (CPU Sets and full
NUMA node relationships) and Linux; unavailable topology uses OS scheduling.

## Policy and allowed CPUs

`NumaPolicy` has `auto` and `none` values, defaulting to `auto`. Auto binds only
when the allowed CPUs span more than one NUMA node or Windows processor group.
One NUMA node can span several groups; group ids are not node ids.

Windows discovery intersects topology with narrower thread/process affinity
masks and selected CPU Sets, and excludes CPUs allocated to another process.
Linux uses a dynamic `sched_getaffinity` mask and sysfs core/node relationships.
The topology is captured on the controlling thread before workers are bound.
CPU restrictions must be established before creating the pool. Runtime external
affinity changes require restarting the engine to refresh this snapshot.

Planning alternates nodes, exhausts physical cores before SMT siblings, and
cycles through the allowed CPUs only when oversubscribed. Process-based rotation
reduces identical starting placements but is not coordination or CPU reservation.
Concurrent match processes should receive separate external affinities, or use
`none`. A single node/group uses OS scheduling without additional network copies.

## Worker and network lifetime

A worker binds itself, initializes private history, and signals readiness before
accepting work. Context, accumulator and PV allocation happens on that worker.
History clear is a synchronous maintenance job: every worker stages a replacement;
all replacements are published together only after success. Existing OS threads
survive `ucinewgame`.

Changing Threads or NumaPolicy constructs a replacement pool before retiring the
old idle pool. Failure preserves the previous pool. This temporarily requires
resources for both pools. Binding or Linux memory-policy failures during auto
configuration fall back to an unbound pool with a diagnostic. Allocation failures
propagate to the UCI option handler without replacing the current pool.

When binding is active, one representative worker per used NUMA node creates a
read-only NNUE replica. `Network::clone(node)` copies all weights and metadata
into dedicated pages. Windows uses `VirtualAllocExNuma`; Linux uses `mmap` with
`mbind(MPOL_PREFERRED)` before copying. The source network remains available for
loading/reconfiguration in addition to the active node replicas. `none` uses
that source directly.

EvalFile loading prepares all replicas while idle before moving the new source
into the original Network object. Failed preparation leaves the source and old
replicas intact. Pool control operations must be serialized by the caller.

## Reporting and limits

UCI reports actual allowed CPU ids grouped by node/group, the effective binding
mode, and worker/replica counts per used node. Preferred placement is an allocation
request, not verified physical residency; the OS can use remote memory. Worker
heap allocations also depend on OS and allocator behavior. No claim of verified
"Local memory" is printed.

The TT still uses its original shared allocation and initialization. Distributing
TT pages and parallel TT clearing are a separate stage. Fixed-node searches also
retain the exact shared atomic node budget, which can limit NUMA scaling.

## Validation

Tests cover synthetic sparse/asymmetric topologies, groups spanning one node,
physical-core ordering, rotations and oversubscription within allowed CPUs,
native helper-thread binding and affinity rediscovery, deep network copies and
golden evaluations, policy switches, history reset, failed reconfiguration, and
UCI reporting. A single-node machine cannot validate multi-node throughput or
physical page residency; those require multi-node hardware measurements.
