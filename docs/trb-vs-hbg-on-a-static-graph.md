# trb vs hbg on the same static graph — prediction, then measurement

**Status: measured.** The prediction below was written first and is left
unedited so it can be scored; the numbers and the scorecard are at the end. One
of its claims was wrong in a way worth keeping on the page.

The question: given **one static graph that both runtimes can run**, which is
faster, and why?

## The prediction

**hbg with a device scheduler (`resident` or `percounter`) should win, probably
by a lot.** hbg with `legacy` should be close to trb, possibly slightly ahead.
Two situations flip it, and both matter.

## Why — the load-bearing reason is which core schedules

| runtime | scheduling runs on | per-task AICPU MMIO |
| --- | --- | --- |
| **trb** | **AICPU** | **yes** |
| hbg + legacy | AICPU | yes |
| hbg + **resident / percounter** | **AICore** | **no** |

trb's `aicore/aicore_executor.cpp` includes only `dispatch_payload.h` and
`runtime.h` — not `scheduler.h`, not `orchestrator.h`, not `tensormap.h` — and
its `SchedulerState` lives in the arena, which the AICore cannot see. The AICPU
side runs `sched_thread_num_ = nthreads - 1` scheduler threads. So **trb
schedules on the same tier as hbg's legacy scheduler.**

That tier has a hard cost, measured: AICPU MMIO is `Device-nGnRE`, **~95 ns and
strictly serial** — one LDR drains before the next issues, so a thread owning 32
cores spends ~3 µs per polling round just asking whether anything finished.
Measured on a5 silicon (P4), moving scheduling off that tier is worth:

| graph | hbg-legacy (AICPU) | hbg-percounter (AICore) | ratio |
| --- | --- | --- | --- |
| 4096 independent tasks | 3725 µs | 518 µs | **7.2×** |
| 1024-long chain | 5763 µs | 2657 µs | 2.2× |
| 32-wide fanin × 1024 | 1167 µs | 288 µs | 4.1× |

trb sits on the slow side of that line.

## Why — a static graph cancels trb's advantage and keeps its costs

trb's reason to exist is **deriving dependencies automatically**: the caller
writes no edges, and the orchestrator recovers them by asking a TensorMap who
last wrote each buffer. On a **static** graph:

- hbg does the same work **on the host, at build time, once**, on a much faster
  CPU and off the device's critical path.
- trb redoes it **on the AICPU, on every submit** — a hash lookup plus overlap
  test per input tensor, an insert per output.

Same information; hbg precomputes it into a `fanin` array, trb recomputes it on
device. A static graph makes that recomputation pure overhead.

The reclamation machinery is the same story. `fanout_refcount`, `SCOPE_BIT`, the
`CONSUMED` transition, the `last_task_alive` watermark, the 500 ms deadlock
backstop, an allocation that can block — on a static graph **that fits**, none
of it buys anything. hbg keeps the whole graph resident and simply does not have
these code paths.

## Two situations that flip it, and they are not small

**1. A graph too large for hbg.** hbg's memory is ∝ **task count** (whole-graph
resident); trb's is ∝ **ring window W** (fixed). A graph where hbg reports
`Graph Too Large` may run fine on trb. That is not "slower", it is "runs at
all".

**2. hbg's build phase is on the critical path.** hbg must finish building the
entire graph on the host and upload it before the device starts — a serial
stage, which the repo has a whole skill (`hbg-bind-phases`) to measure. trb runs
its orchestrator thread and its scheduler threads **concurrently**
(`serial_orch_sched` exists as an option precisely because concurrent is the
default), so submission of task N+1 overlaps execution of task N.

**The P4 numbers above are device wall and exclude that stage entirely.** On a
long graph the host build could eat what hbg gains on device, and only an
end-to-end measurement would show it.

## Summary of the prediction

| case | expected |
| --- | --- |
| small/medium static graph that fits | **hbg + percounter/resident clearly faster** |
| same, against hbg-legacy | close; trb possibly slightly behind, since it also pays TensorMap |
| very large graph | **trb wins** — hbg cannot run it |
| long graph, host build a large share | **unknown** — depends on end-to-end, not device wall |

## How it will be measured

`multi_core_dag` is hbg-only (`@scene_test(runtime="host_build_graph")`) and trb
has no equivalent, so the comparison needs a trb port of its orchestration.
That port is mechanical and, importantly, **can be made equivalent**: both
runtimes expose `set_dependencies(const TaskId*, uint32_t)`,
`rt_submit_aic_task` / `rt_submit_aiv_task`, `add_scalar` and — the one that
matters — `add_no_dep`.

`add_no_dep` is what keeps the graphs identical. Every task in `multi_core_dag`
shares one `task_state` tensor, and the hbg orchestration adds it with
`task.add_no_dep(task_state)` so it creates no edge; all edges come from
`set_dependencies`. Without that call on the trb side, trb's TensorMap would
derive an edge from the shared buffer for **every** task and silently serialise
the whole graph — the two runtimes would then be running different graphs and
any number would be meaningless.

Planned matrix: `mixed_chain_1024`, `mixed_fanin32_1024`, `mixed_random_1024`,
`mixed_multi_root_4096` × {hbg-legacy, hbg-resident, hbg-percounter, trb},
reporting **both** device wall and end-to-end, since the prediction's most
uncertain claim is specifically about the difference between the two.

The port landed at `tests/st/a5/tensormap_and_ringbuffer/multi_core_dag/`. The
only source change is the namespace, `simpler::hbg::Tensor` →
`simpler::tmr::Tensor`, plus the decorator's `runtime=`. **`add_no_dep` is
byte-for-byte identical in the two runtimes** — same comment, same tag, same
"skips OverlapMap lookup, depends on creator only" semantics — and
`task_state` is a host-supplied external tensor whose `owner_task_id` is
invalid, so it yields no edge on either side. Every edge comes from
`set_dependencies`. Passes in simulation and onboard.

## Measured

a5 silicon, `--rounds 5` with round 0 dropped as warm-up, one submission.
**Single sample per point** — unlike the P4 tables, this has no repeat, so treat
gaps under ~20% as noise. All 16 runs passed their zero-tolerance golden.

**Device wall (µs), lower is better:**

| graph | hbg:legacy | hbg:resident | **hbg:percounter** | **trb** |
| --- | --- | --- | --- | --- |
| `mixed_chain_1024` | 5530 | 6770 | **2689** | 6436 |
| `mixed_fanin32_1024` | 1283 | 7584 | **287** | 6274 |
| `mixed_random_1024` | 1428 | 813 | **213** | 2474 |
| `mixed_multi_root_4096` | 3804 | 660 | **574** | 5747 |

**Host overhead (end-to-end − device wall, µs):**

| graph | legacy | resident | percounter | **trb** |
| --- | --- | --- | --- | --- |
| `mixed_chain_1024` | 1968 | 2450 | 1980 | **1600** |
| `mixed_fanin32_1024` | 2152 | 2832 | 2837 | **1901** |
| `mixed_random_1024` | 1986 | 2302 | 2150 | **1478** |
| `mixed_multi_root_4096` | 5460 | 5957 | 6666 | **3823** |

## Scorecard

**Right — hbg + percounter wins on every graph**, by 2.4× to 22× on device wall
and on end-to-end too. The stated reason holds: trb schedules on the AICPU, the
same tier as hbg-legacy, and pays that tier's serialised MMIO.

**Right — trb lands near hbg-legacy, slightly behind**, on three of the four:
16% behind on the chain, 1.7× on random, 1.5× on multi-root.

**Wrong — `fanin32`, where trb is 4.9× *behind* legacy**, not near it. The
prediction had no mechanism that could produce this, and the one it missed is
the more interesting half of the comparison:

> **trb materialises every fanin edge.** `orch_wire_live_fanin_task` runs
> `for_each_fanin_slot_state(*payload, ...)` and calls `dep_pool.prepend` per
> edge, so 992 consumers × 32 producers becomes **31,744 `DepListEntry` nodes
> and 31,744 link operations**.
>
> **hbg's wake list materialises one edge at a time.** `classify_fanin_state`
> scans from `fanin_count - 1` downward and hangs the consumer on its *last
> unmet* producer only, with a monotone cursor — 992 registrations, re-hung as
> producers complete.

Same graph, and trb does **32× the edge work**. This also explains hbg-resident
collapsing on the same graph (7584 µs): it uses the wake list, so it links one
edge at a time, but all 992 consumers contend on a single `wake_list_head` with
a device CAS. Wide fanin punishes trb and resident for two different reasons.

**Right in direction, too small to matter — hbg's host build is real.** trb's
host overhead is the lowest on all four graphs, and the gap widens with task
count: 3823 µs against percounter's 6666 µs at 4096 tasks. hbg must finish
building the whole graph on the host before the device starts; trb overlaps
submission with execution. But it does not flip the result — end-to-end,
percounter is still 7240 µs against trb's 9570 µs.

## What this does not say

- **Nothing about a graph hbg cannot hold.** The memory argument above is
  untested; every graph here fits comfortably. A graph large enough to trip
  `Graph Too Large` remains trb's clearest win and is unmeasured.
- **Nothing about trb's actual purpose.** This compares the two on a *static*
  graph, which is the case that removes trb's reason to exist — the caller here
  declares every edge by hand. It says nothing about an eager or dynamic
  workload where the dependencies are not known ahead of time.
- Single sample, one card, one shape family.
