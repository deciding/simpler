# Percounter: a pull-based AICore scheduler

**Status**: design, not implemented; P0 hardware measurements done (§4).
Target: a5 `host_build_graph` only.

A third device-scheduling mode next to `legacy` (AICPU 3S+1P) and `resident`
(one scheduler AIV per cluster). Percounter keeps resident's premise — resolve
dependencies on the AICore — but drops the scheduler role: every lane pulls its
own work, waits on its producers' counters, executes, and publishes one counter.
There is no wake list, no ready inbox, no directory, and no work stealing.

## 1. The model

```text
per lane (AIC and AIV alike):
  k = atomicAdd(ticket[my_core_type], 1)          # claim the next task
  if k >= order_count[my_core_type]: drain, wait for EXIT
  t = order[my_core_type][k]
  for a in fanin_addr[e.fanin_begin .. +fanin_count]:
      spin until ld_dev(a) >= 1                   # spin only, never sleep
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
tasks into one list per core type. `build_percounter_tables` still asserts
`producer_id < consumer_id` per edge — it is O(edges) and turns the load-bearing
assumption into a bind-time failure rather than a device hang.

**Splitting the order by core type cannot deadlock.** Both lists are ascending
subsequences of one global topological order. Take T, the lowest-id incomplete
task. Every dependency of T has a smaller id, so it is complete. Every task
ahead of T in its own list has a smaller id, so it is complete too and its core
is free. T is therefore claimable and runnable. This holds as long as a claimed
task is never abandoned and cores are not preempted.

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
| `order[2][]` (new) | one ascending `int32` task-id list per core type | |
| `PercounterControl` (new) | `ticket[2]`, `order_count[2]`, segment offsets | each ticket on its own cache line |

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

The counter array contains counters only, written exclusively by `atomicAdd`.
P0 measured what that buys: packing is **correct** but costs linearly in
counters per line, and a plain load never observes a remote atomic at all. So
the array is padded to one counter per line and the spin uses an explicit
invalidate (or an atomic read — §4).

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

### A remote counter is only visible through an invalidate

95 cores spinning on one address that a 96th publishes once:

| read idiom | cyc/poll | observed it |
| --- | --- | --- |
| `GlobalTensor::GetValue` | 7.3 | 0 / 285 |
| volatile `__gm__` deref | 248.7 | 0 / 285 |
| `dcci(SINGLE_CACHE_LINE)` + `dsb` + deref | 1229 | **285 / 285** |

The producer is confirmed to have published, so the two zeros are visibility
failures, not a missing write. A plain load — even a volatile one doing a real
249 cyc access — never observes a remote atomic. This is the runtime's own rule
arriving from the other side: `scheduler_observe_cache_line` is load-bearing.

**1229 cyc is an upper bound, not the design figure.** Most of it is the `dsb`
pipeline drain, not the read. Two cheaper idioms are untested and both should
beat it:

- `AtomicAdd(ptr, 0)` — an atomic read through the atomic unit, which the stride
  result already proves is coherent across cores. Uncontended that is ~431 cyc,
  2.9x better; and a real DAG has a handful of consumers per producer, not 95,
  so it sits in the uncontended regime.
- `__builtin_cce_ld_dev` — what `scheduler_gm_query` uses: a device load with no
  RMW, so plausibly cheaper still.

P0b closes this before any spin loop is written.

### The ticket is a congestion curve, not a fixed cost

Each core claims 200 times, burning `work` cycles between claims. The claim is a
**dependent** chain, because the returned value is the task id:

| work (cyc) | cyc per claim | limited by |
| --- | --- | --- |
| 0 | 16177 | ticket |
| 1000 | 16181 | ticket |
| 5000 | 16182 | ticket |
| 20000 | 20000.1 | work |

> An earlier revision discarded the return value. The atomic is posted, so the
> claim pipelined and the floor read 3686 cyc — 4.4x optimistic. Percounter
> cannot use that number.

This is the same congestion law `atomic_latency`'s sparse sweep found, and the
two agree: a shared address saturates when the arrival rate exceeds the service
rate, i.e. when cores touch it more often than `S x ncores` apart.

```
crossover ~= S x (cores sharing the ticket) ~= 181 x 96 ~= 17.4 us
```

Below it the ticket sets the ceiling; **above it the ticket is nearly free** —
at work=20000 it added 0.14 cyc. The threshold scales **linearly with the number
of cores on that ticket**, which is the lever:

| ticket sharding | cores per ticket | crossover |
| --- | --- | --- |
| one global | 96 | ~17.4 µs |
| per core type | 64 (AIV) / 32 (AIC) | ~11.6 / ~5.8 µs |
| per cluster | 3 | **~0.54 µs** |

So sharding does not merely help, it removes the problem: per-cluster tickets
put any task longer than ~0.5 µs in the free regime. That is a much smaller step
than resident's inbox-plus-steal — a ticket per cluster is still a counter, with
no queue, no directory and no stealing — so the earlier framing of sharding as
"converging back on resident" was wrong.

v0 keeps one ticket per core type so the baseline stays measurable, and P4
compares it against per-cluster sharding.

### Head-of-line blocking

A lane that has claimed a task spins on its fanin and cannot run a different
ready task meanwhile. Ascending task id is the only ordering v0 offers. The poll
cost above sets how much this hurts, which is another reason P0b matters: at
431 cyc a blocked lane is cheap to leave spinning, at 1229 it is not.

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
| **P0b** | the spin's read idiom: `AtomicAdd(ptr, 0)` and `__builtin_cce_ld_dev` against the measured `dcci` upper bound, at realistic consumer counts (2–8, not 95). Also per-cluster ticket sharding. | one card, minutes; extends the same probe |
| **P1** | host: env parsing, constants, layout, table build | C++ unit tests under `tests/ut/cpp/a5/runtime/host_build_graph/`: env values incl. invalid, layout, fanin addresses, order partition, counter presets |
| **P2** | AICPU predicate, AICore branch, `run_percounter_executor` | builds; a5sim |
| **P3** | sim scene tests | `vector_example`, `single_core_dag`, `multi_core_dag`, `empty_lifecycle` across all three modes, plus a new wide/deep mixed AIC+AIV DAG and an assertion that percounter really ran |
| **P4** | onboard | `onboard-arch-precheck`, then `task-submit`; device Total and chip swimlane across the three modes |
| **P5** | docs | a percounter section in `RUNTIME_LOGIC.md`; register the env var; grep for stale references per [`doc-consistency.md`](../.claude/rules/doc-consistency.md) |

Hardware runs follow `a5_test`: explicit `--device 0|1|2|3` (never `auto`),
always through `task-submit`, and sized to finish in minutes.

## 8. Not supported in v0

MIX, SPMD, `sync_start` and Graph — the same envelope as resident. MIX and SPMD
need the counter to reach N rather than 1 and need co-resident placement; both
are follow-ups, and the `>= 1` completion test is written so it can become
`>= N`.
