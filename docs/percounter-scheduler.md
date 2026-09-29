# Percounter: a pull-based AICore scheduler

**Status**: P0/P0b measured (§4), P1 host side implemented; device side (P2) not started.
Target: a5 `host_build_graph` only.

A third device-scheduling mode next to `legacy` (AICPU 3S+1P) and `resident`
(one scheduler AIV per cluster). Percounter keeps resident's premise — resolve
dependencies on the AICore — but drops the scheduler role: every lane pulls its
own work, waits on its producers' counters, executes, and publishes one counter.
There is no wake list, no ready inbox, no directory, and no work stealing.

## 1. The model

```text
per lane (AIC and AIV alike):
  ty = my_core_type                               # AIC or AIV
  k  = atomicAdd(ticket[ty][my_cluster], 1)       # one ticket per cluster per type
  i  = my_cluster + k * cluster_count             # strided slice of one ascending list
  if i >= order_count[ty]: try another cluster's ticket, else drain and wait for EXIT
  t  = order[ty][i]
  for a in fanin_addr[e.fanin_begin .. +fanin_count]:
      spin until ld_dev(a) >= 1                   # ~98 cyc; spin only, never sleep
  materialize args into my DispatchPayload slot
  execute_task(payload)                           # skipped when INLINE or predicate FAIL
  atomicAdd(counter[t], 1)                        # publish completion
  local_done++                                    # flushed to resolved_task_count in batches
```

Two claims carry the design:

**Task id order is already a topological order.** `append_fanin_or_fail` rejects
any producer id at or past what the run has claimed so far, and hbg "mints them
in order and never recycles one"
([`orchestrator.cpp:1515-1545`](../src/common/host_build_graph/host/orchestrator.cpp)),
so every fanin edge points to a strictly lower id. Ascending `task_id` is
therefore a valid topological order: the host does not sort, it only partitions
tasks into one list per queue. `build_percounter_tables` still asserts
`producer_id < consumer_id` per edge — it is O(edges) and turns the load-bearing
assumption into a bind-time failure rather than a device hang.

**Splitting the order across queues cannot deadlock**, however many queues there
are. Every list is an ascending subsequence of one global topological order, and
round-robin assignment preserves that. Take T, the lowest-id incomplete task.
Every dependency of T has a smaller id, so it is complete. Every task ahead of T
in its own list has a smaller id, so it is complete too and that queue's cores
are free. T is therefore claimable and runnable. This holds as long as a claimed
task is never abandoned and cores are not preempted.

**But sharding buys throughput with load balance.** §4 shows one global ticket
caps a 96-core run at a claim per 16.2 µs, and a per-cluster ticket at 508 ns —
so v0 shards. The cost is that a queue whose tasks happen to be short drains
early and its cores idle while another queue still has work; a single global
ticket cannot have that problem. The pseudocode above therefore ends an empty
queue by trying another one, which is a second `atomicAdd` on a different
counter, not a work-stealing protocol. Whether static round-robin plus that
fallback is enough, or the partition needs to be duration-aware, is open and
P4 measures it.

## 2. GM layout

Everything lives in the existing scheduler state region
(`acquire_scheduler_state_storage`), which is rebuilt and re-uploaded on every
bind. **Counters are therefore zero at the start of each run with no explicit
reset**, and no epoch scheme is needed for v0.

| Structure | Contents | Note |
| --- | --- | --- |
| `SchedulerRunControl`, `SchedulerWorkerContext[]`, `DispatchPayload[]` | reused unchanged | AICPU handshake, supervisor and watchdog need no change |
| `SchedulerTaskMetadata[task]` | reused unchanged | already carries `kernel_ids[3]`, `active_mask`, `flags`, `timing_slot` |
| `PercounterFanin[task]` (new) | `fanin_count`, `fanin_begin` | the only per-task data `SchedulerTaskMetadata` lacks |
| `fanin_addr[]` (new) | `uint64_t` absolute GM address of each producer's counter | the host knows the device base at bind time |
| `counter[task]` (new) | `uint64_t`, **one per 64 B cache line** | 0 initially; host presets 1 for inline-completed tasks. Padding is not optional — see §4 |
| `order[2][]` (new) | one ascending `int32` task-id list **per core type**, not per queue | see below: the host cannot know the cluster count |
| `PercounterControl` (new) | `ticket[2][36]`, `order_count[2]`, segment offsets | **each ticket on its own 64 B line** — §4 shows why |

### Why the order lists are per core type, not per queue

**The host does not know the topology at bind time.** `post_handshake_init`
discovers the cluster count on the AICPU and overwrites the `core_type` and
`type_rank` the host guessed. A host that partitioned tasks into 36 x 2 queues
on a board that brings up 8 clusters would strand every task in the 28 queues
nothing ever claims — a deadlock, not an imbalance.

So the host builds **one ascending list per core type** and the device maps its
own queue onto a strided slice:

```
i = my_cluster + k * cluster_count
```

A strided slice of an ascending list is still ascending, so §1's
deadlock-freedom argument carries over unchanged, and `cluster_count` is only
needed where it is actually known. It also removes the per-queue arrays: the
only per-queue state left is the ticket itself.

`SchedulerRunControl` gains a `percounter_control_offset`; its `static_assert`
moves with it.

### Why the flags are not reinvented

`SchedulerTaskMetadata.flags` already answers the only question a percounter
lane asks beyond "which kernel":

- `SCHEDULER_TASK_INLINE` — a task whose `active_mask` is empty but which is not
  yet complete. The host synthesises an AIV0 mask so something schedules it, and
  dispatch sets `function_bin_addr = 0`. It carries dependencies only (a barrier
  or a placeholder producer). Distinct from *inline-completed* tasks, which were
  already done at bind: those are excluded from `order[]` and their counter is
  preset to 1.
- `SCHEDULER_TASK_HAS_PREDICATE` — the task carries a 24-byte
  `SchedulerDispatchPredicate` at payload offset 128. It is evaluated on device
  at dispatch; on FAIL the kernel is skipped but the task still completes, which
  releases its consumers.

Both mean "complete this task without calling a kernel" — one decided at bind,
one at dispatch.

## 3. Memory ordering

Two access classes exist in this runtime and they must not be mixed on one cache
line:

| Primitive | Implementation | Path |
| --- | --- | --- |
| `scheduler_gm_query` | `__builtin_cce_ld_dev` | device load, bypasses the core cache |
| `scheduler_gm_fetch_add` | `atomicAdd` | executed at the atomic unit |
| `scheduler_publish/observe_cache_line` | `dcci` + ordinary access | **writes back a whole line** |

`SchedulerTaskControl` encodes the rule: `state` and `wake_list_head` (both
atomic RMW targets) deliberately share line 0, while `next_waiter` (published
through the cache) is pinned to offset 64.

The counter array contains counters only, written by `atomicAdd` and read by
`__builtin_cce_ld_dev`. P0 measured what that buys: packing is **correct** but
costs linearly in counters per line, and an ordinary load — even a volatile one
— never observes a remote atomic. So the array is padded to one counter per line
and the spin reads through `ld_dev` (§4).

Publication order per task: `execute_task` ends in `OUT_OF_ORDER_STORE_BARRIER`
(`dsb(DSB_DDR)`), then `atomicAdd(counter[t], 1)`. A consumer that observes the
counter has therefore observed the outputs.

## 4. Measured costs that constrain the design

P0 measured the three properties this design rests on, on one card with 96 cores
(32 AIC + 64 AIV), 1 cycle = 1 ns. Probe and raw data:
`a5_test/ascendc/counter_deps/`, analysis in
`a5_test/docs/counter-deps-analysis.md`. Background figures come from the
earlier `atomic_latency` and `gm_lat` probes in the same repo.

| Quantity | Value |
| --- | --- |
| Empty-loop floor | 19.8 cyc |
| Uncontended `atomicAdd`, one counter per 64 B line | 431 cyc |
| Same-address service interval `S` | ~170–181 cyc, independent of core count |
| GM scalar read: L1 / L2 / HBM | 4.25 / ~88 / ~270 cyc |

### The atomic unit serialises per cache line

Counters at `stride` uint32 apart, each core chaining on its own:

| counters per 64 B line | cyc/atomic | vs one-per-line | correct? |
| --- | --- | --- | --- |
| 16 | 6371 | 14.8x | yes |
| 8 | 3259 | 7.6x | yes |
| 4 | 1931 | 4.5x | yes |
| 2 | 943 | 2.2x | yes |
| 1 | 431 | 1.0x | yes |

Packing is **correct** at every stride — the non-coherent machine does not lose
a neighbour's increment — but the cost tracks counters-per-line almost exactly.
So **each counter takes its own 64 B line**: 64 B per task, 640 KB for a 10k-task
graph, far inside the 64 MB L2. The compact `uint64 counter[task]` array this
design first assumed is out; the padding is the only change and it is free.

### The fanin spin: `ld_dev` is both correct and cheapest

95 cores spinning on one address that a 96th publishes once. Which idioms see
the write at all:

| read idiom | what it does | cyc/poll | observed it |
| --- | --- | --- | --- |
| `GlobalTensor::GetValue` | hoisted out of the loop | 7.3 | 0 / 285 |
| volatile `__gm__` deref | a real load, from a stale level | 248.7 | 0 / 285 |
| `AtomicAdd(ptr, 0)` | full RMW at the atomic unit | 16109 | 239 / 285 |
| `dcci(SINGLE_CACHE_LINE)` + `dsb` + deref | invalidate, drain, load | 1237 | **285 / 285** |
| **`__builtin_cce_ld_dev`** | **device load, no RMW, no drain** | **1208** | **285 / 285** |

The producer is confirmed to have published, so the two zeros are visibility
failures, not a missing write. Note the pair that matters: the volatile deref is
**slower (249 cyc) and wrong**, `ld_dev` is **faster (98 cyc) and right**. It is
not "a load with a keyword on it" — it reaches a coherent point, which is
exactly why `scheduler_gm_query` is built on it.

95 consumers is the worst case, not the operating point:

| consumers on one producer | `ld_dev` | `dcci` + deref | `AtomicAdd(ptr,0)` |
| --- | --- | --- | --- |
| 2 | **98** | 346 | 339 |
| 4 | **123** | 346 | 678 |
| 8 | **228** | 355 | 1351 |
| 95 | 1208 | 1237 | 16109 |

- `ld_dev` wins by 2.5–3.5x through fanout 8; 98 cyc is about one L2 access
  (`gm_lat` measures ~88 cyc), so a poll costs a read and nothing else.
- `dcci` works but pays the `dsb` pipeline drain on every poll.
- `AtomicAdd(ptr, 0)` scales as `169 x consumers` — the same `S x n` law as the
  ticket below. **Adding zero does not skip the read-modify-write**, so every
  poll queues at the atomic unit. Loads do not queue; RMWs do. It is the
  obvious-looking "atomic read" and the wrong instrument.

**A fanin check therefore costs ~100–230 cyc** — a quarter of one uncontended
atomic. A lane left spinning on a producer is affordable, so v0 needs no
lookahead window.

### The ticket is a congestion curve, and sharding flattens it

Each core claims 200 times, burning `work` cycles between claims. The claim is a
**dependent** chain, because the returned value is the task id. `L_core` in
cycles; **bold** = ticket-limited, plain = tracking `work`:

| cores | work=0 | 5000 | 8000 | 11000 | 14000 | 17000 | 20000 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 3 | **508** | 5000 | 8000 | 11000 | 14000 | 17000 | 20000 |
| 6 | **1016** | 5000 | 8000 | 11000 | 14000 | 17000 | 20000 |
| 12 | **2031** | 5000 | 8000 | 11000 | 14000 | 17000 | 20000 |
| 24 | **4060** | 5000 | 8000 | 11000 | 14000 | 17000 | 20000 |
| 48 | **8119** | **8119** | **8117** | 11000 | 14000 | 17000 | 20000 |
| 96 | **16184** | **16182** | **16175** | **16179** | **16182** | 17000 | 20000 |

```
floor = crossover = S x ncores,  S = 169.2 cyc, constant over a 32x core range
```

The floor *is* the crossover: below it the ticket sets the pace, above it the
ticket costs **0.14 cyc**.

**Read the marginal cost, not the total.** The table is the total interval, and
it includes the `work` the probe inserts, which makes the ticket look like a
cliff. What the ticket actually costs is `L_core - work`, and it declines
smoothly — at 96 cores: 16184 (work=0), 8175 (8000), **2182 (14000)**, ~0
(17000). While saturated the total is pinned at the floor, so the marginal is
`floor - work` by construction. Quoting the total as "the ticket costs 16.2 µs"
is wrong at any work > 0, and an earlier revision of this doc did exactly that.

That also reconciles this probe with `atomic_latency`'s sparse sweep, which
looked like it disagreed by 4x. At the same per-core spacing (~14000 cyc) sparse
reports `extra_shared = 3251` and this probe 2182 — the same quantity and the
same order. The knees agree too: period ~40 (spacing ~17.5k) against a measured
crossover between 14000 and 17000.

> Two further caveats. An earlier revision discarded the claim's return value;
> the atomic is posted, so the claim pipelined and the floor read 3686 cyc —
> 4.4x optimistic. And this probe's pacing (`next += work` anchored at `t0`)
> *maintains* synchronisation under saturation, so it measures the synchronised
> arrival case, where sparse's free-running private chain measures the
> desynchronised one. The gap is small here but the effect is unmeasured.

Since the crossover scales linearly with the cores sharing one ticket, the
number of tickets is the lever:

| sharding | cores/ticket | crossover |
| --- | --- | --- |
| one global | 96 | 16.2 µs |
| per core type | 64 AIV / 32 AIC | 10.8 / 5.4 µs |
| **per cluster** | **3** | **0.51 µs** |

A per-cluster ticket saturates at **508 cyc** with zero-length tasks — within
18% of a completely uncontended atomic (431 cyc). **v0 therefore shards the
ticket per cluster**, which is 36 counters and still no queue, no directory and
no stealing. The draft's claim that sharding "converges back on resident" was
wrong.

### Head-of-line blocking

A lane that has claimed a task spins on its fanin and cannot run a different
ready task meanwhile. Ascending task id is the only ordering v0 offers. The poll
cost above settles how much this hurts: at ~100 cyc a check, a blocked lane is
cheap to leave spinning, so v0 does not need a lookahead window.

## 5. Mode selection

`SIMPLER_HBG_SCHEDULER`, read once in `create_scheduler_state`, following the
`resolve_runtime_timeout_config` pattern:

| Value | Behaviour |
| --- | --- |
| unset / `auto` | today's automatic choice: Graph or unsupported shape falls back to legacy, otherwise resident |
| `legacy` | force legacy |
| `resident` | force resident; **bind fails** on an unsupported shape |
| `percounter` | force percounter; **bind fails** on an unsupported shape |
| anything else | bind fails |

An unknown value is rejected rather than ignored: a misspelled environment
variable that silently does nothing is the failure mode
[`env-macro-gating.md`](../.claude/rules/env-macro-gating.md) exists to prevent.

New constants in `scheduler_layout.h`: `SCHEDULER_RUNTIME_MODE_PERCOUNTER = 5`
and `SCHEDULER_RUNTIME_MODE_LEGACY_REQUESTED = 6` (so an explicit legacy choice
is distinguishable from a forced fallback in logs and terminal records). The
existing `Runtime::dev.scheduler_bootstrap.runtime_mode` carries the value to the
device; no new wire field is needed.

## 6. Touch list

| Layer | File | Change |
| --- | --- | --- |
| host | `host/runtime_maker.cpp` | parse the env var; v0 shape check; `percounter_plan_layout`; `build_percounter_tables` as a testable free function, **not** in an anonymous namespace |
| shared | `runtime/scheduler/scheduler_layout.h` | mode constants |
| shared | `runtime/scheduler/scheduler_types.h` | `PercounterControl`, `PercounterFanin`, `percounter_control_offset` |
| AICPU | `aicpu/aicore_scheduler_state.h` | admit PERCOUNTER in the resident predicates, or it falls into legacy and is rejected at `aicpu_executor.cpp:640` |
| AICore | `aicore/aicore_executor.cpp` | branch on PERCOUNTER after READY |
| AICore | `aicore/aicore_percounter_executor.cpp` (new) | the loop in §1 |

Reused as-is: R0/R1 handshake, context publish and the DMB release gate;
`scheduler_materialize_task_payload_resolved`; the callable address table;
R7/R8 supervisor, EXIT and `resolved_task_count`. `post_handshake_init` still
elects a scheduler lane — unused here, harmless, left alone in v0.

`scheduler_initialize_local_config` rejects non-zero reserved context fields;
percounter does not call it, so that check is not in the way.

## 7. Plan

| Phase | Work | Verification |
| --- | --- | --- |
| **P0** | ~~stride sweep, fanin spin cost, ticket crossover~~ | **done** — `a5_test/ascendc/counter_deps/`, results in §4 |
| **P0b** | ~~read idiom, realistic consumer counts, ticket sharding~~ | **done** — §4: `ld_dev` wins at ~98 cyc, ticket shards per cluster |
| **P1** | ~~host: env parsing, constants, layout, table build~~ | **done** — [`percounter-p1-plan.md`](percounter-p1-plan.md); 26 new cases, 264/264 ut-cpp passing |
| **P2** | ~~AICPU predicate, AICore branch, `run_percounter_executor`~~ | **done** — [`percounter-p2-plan.md`](percounter-p2-plan.md); percounter executes on a5sim and passes every scene test the other two modes pass, 264/264 ut-cpp |
| **P3** | a scene test built for percounter, and an assertion that it ran | a new wide/deep mixed AIC+AIV DAG — the existing tests are chains, which exercise the fanin spin but never the strided claim under contention — plus a programmatic check of the selected mode, so no result rests on reading a log |
| **P4** | onboard | `onboard-arch-precheck`, then `task-submit`; device Total and chip swimlane across the three modes |
| **P5** | docs | a percounter section in `RUNTIME_LOGIC.md`; register the env var; grep for stale references per [`doc-consistency.md`](../.claude/rules/doc-consistency.md) |

Hardware runs follow `a5_test`: explicit `--device 0|1|2|3` (never `auto`),
always through `task-submit`, and sized to finish in minutes.

## 8. Not supported in v0

MIX, SPMD, `sync_start` and Graph — the same envelope as resident. MIX and SPMD
need the counter to reach N rather than 1 and need co-resident placement; both
are follow-ups, and the `>= 1` completion test is written so it can become
`>= N`.
