# Percounter P1: host-side tables — implementation plan

Living plan for phase P1 of [`percounter-scheduler.md`](percounter-scheduler.md).
Tick the boxes as steps land; the design doc stays the reference for *what* is
being built and *why*, this file only tracks *how* and *how far*.

**Scope**: host side and unit tests only. No device code — the AICPU mode
predicate and the AICore executor loop are P2. Everything here builds and tests
on CPU, so no card is taken.

**Branch**: `percounter-scheduler` on the fork.

**Precondition**: P0/P0b are done. The measurements those produced —
64 B counter padding, `ld_dev` for the spin, per-cluster tickets — are inputs to
this phase, not open questions. See `percounter-scheduler.md` §4.

## The correction this plan starts from

The design doc originally had the host build one task list per queue, keyed by
`(cluster, core_type)`. **That is not implementable**: the host does not know
the cluster count at bind time — `post_handshake_init` discovers it on the AICPU
and overwrites the `core_type`/`type_rank` the host guessed. Partitioning into
36 x 2 queues on a board that brings up 8 clusters would strand every task in
the 28 queues nothing claims, which is a deadlock rather than an imbalance.

P1 therefore builds **one ascending list per core type**, and the device maps
its queue onto a strided slice (`i = my_cluster + k * cluster_count`). A strided
slice of an ascending list is still ascending, so the deadlock-freedom argument
is unchanged, and the topology is only consulted where it is known. The design
doc §2 carries this.

## Steps

- [x] **1. Mode constants** — `SCHEDULER_RUNTIME_MODE_PERCOUNTER = 5` and
      `SCHEDULER_RUNTIME_MODE_LEGACY_REQUESTED = 6` (so a forced legacy choice
      is distinguishable from a fallback in logs and terminal records).
      *File*: `src/a5/runtime/host_build_graph/runtime/scheduler/scheduler_layout.h`

- [x] **2. Mode request parsing** — `SchedulerModeRequest{AUTO, LEGACY,
      RESIDENT, PERCOUNTER}`, a pure `parse_scheduler_mode_request(const char*)`
      that is testable without touching the environment, and a
      `resolve_scheduler_mode_request()` that reads `SIMPLER_HBG_SCHEDULER`.
      **An unrecognised value fails the bind** rather than being ignored, per
      [`env-macro-gating.md`](../.claude/rules/env-macro-gating.md).
      *File*: new `src/a5/runtime/host_build_graph/host/percounter_plan.h`

- [x] **3. Layout** — `percounter_plan_layout()`, modelled on
      `scheduler_plan_layout`. Segments: `PercounterControl`, `order[2][]`,
      `PercounterFanin[task]`, `fanin_addr[]`, `counter[task]` at **64 B
      stride**. Sizes pinned by `static_assert`.
      *File*: `percounter_plan.h`

- [x] **4. Table builder** — `build_percounter_tables()`: partition tasks into
      two ascending lists by core type, fill absolute GM fanin-counter
      addresses, preset counters for inline-completed tasks, and **assert
      `producer_id < consumer_id` on every edge**. That assertion is O(edges)
      and converts the plan's load-bearing assumption into a bind-time failure
      instead of a device hang.
      *File*: `percounter_plan.h`

- [x] **5. Wire into bind** — `create_scheduler_state` reads the mode request
      and branches. On percounter: v0 shape check, plan layout, acquire
      storage, build tables, publish the mode. **A forced mode that meets an
      unsupported shape fails the bind**; only `AUTO` falls back.
      *File*: `src/a5/runtime/host_build_graph/host/runtime_maker.cpp`
      Adds `create_percounter_state` beside it, which walks the tasks a second
      time rather than threading two more outputs through the resident walk.
      Registers no `scheduler_state_owners` entry: that record is typed by
      `AicoreSchedulerLayout` and exists only so the resident traces can be read
      back, and the storage is freed by the runner either way.

- [x] **6. Unit tests** — 26 cases, all passing — new case plus one CMakeLists entry.
      *Files*: `tests/ut/cpp/a5/runtime/host_build_graph/test_hbg_percounter_tables.cpp`,
      `tests/ut/cpp/a5/runtime/host_build_graph/CMakeLists.txt`

- [x] **7. Build and run** — CPU only, no card. **264/264 ut-cpp tests pass**,
      which is the run that matters: the percounter case alone does not compile
      `runtime_maker.cpp`, so only a whole-tree build exercises step 5. Built on
      a5x8 in an isolated clone (`~/simpler-pc`) rather than the shared
      `~/simpler` checkout; this Windows box has no C++ toolchain.

## Why steps 2-4 are header-only `inline`

`create_scheduler_state` sits in an anonymous namespace
(`runtime_maker.cpp:409` opens it, the function is at `:1012`), so nothing in
this repo can unit-test it. The new logic must not inherit that. The repo
already has the pattern to follow — `scheduler_plan_layout`
(`scheduler_types.h:1318`) and `resolve_runtime_timeout_config`
(`runtime_timeout_config.h:143`) are both `inline` in headers — and a test then
needs only an `INCLUDES` line, as `test_hbg_scheduler_contracts.cpp` does for
the `aicpu` directory.

The header is host-only (`host/percounter_plan.h`) because the mode request
reads the environment, and `<cstdlib>` has no business in a header the AICore
target compiles.

## Test coverage the case must have

| area | cases |
| --- | --- |
| mode request | each of the four valid spellings; an unrecognised value is **rejected**, not ignored; unset yields `AUTO` |
| layout | segment offsets and alignment; the counter stride really is 64 B; capacity edges (0 tasks, 1 task) |
| table build | both lists come out ascending; a fanin address resolves to exactly the producer's counter; inline-completed tasks are preset to 1; **a deliberately malformed edge (`producer_id > consumer_id`) is caught** |

## Out of scope for P1

AICPU mode predicate, AICore executor loop, and anything that runs on a card —
all P2 and later. Ticket-sharding load balance is a P4 measurement.
