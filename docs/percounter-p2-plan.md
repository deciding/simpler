# Percounter P2: device side — implementation plan

Living plan for phase P2 of [`percounter-scheduler.md`](percounter-scheduler.md).
P1 (host tables) is done — see [`percounter-p1-plan.md`](percounter-p1-plan.md).

**Scope**: the AICPU bring-up path and the AICore executor loop, verified in
simulation. Onboard runs are P4.

**Branch**: `percounter-scheduler` on the fork.

## What percounter needs from the AICPU, and what it does not

The resident mode's AICPU path does six things. Percounter needs five of them
unchanged and replaces the sixth:

| step | resident | percounter |
| --- | --- | --- |
| discover cores | `pre_handshake_init`, `handshake_partition` | same |
| publish per-core config | `post_handshake_init` writes `SchedulerWorkerContext[108]`, elects a scheduler AIV per cluster | **two integers** — see below |
| hand off | `publish_context_partition` sets `aicpu_ready` | same shape |
| wait for bootstrap | schedulers set `bootstrap_complete` | same |
| open the gate | `release_partition` → `DATA_MAIN_BASE = IDLE` | same |
| supervise, shut down | `resolved_task_count` watchdog, EXIT + ACK | same |

**The whole per-worker context array collapses to two numbers.**
`scheduler_cluster_coordinate_from_worker` is a pure function of
`(worker_id, is_aic, cluster_count, aiv_per_cluster)`
(`scheduler_topology.h:26`), so once the AICPU has published `cluster_count` and
`aiv_per_cluster` into `PercounterControl`, every lane computes its own cluster
index from the `block_idx` it was already given. No election, because percounter
has no scheduler role: every lane pulls its own work.

## Steps

- [x] **1. Control block gains the topology** — `cluster_count` and
      `aiv_per_cluster` in `PercounterControl`, written by the AICPU after the
      handshake rather than by the host, which cannot know them at bind.
      *File*: `host/percounter_plan.h`

- [x] **2. AICPU mode predicates** — `is_percounter` and `is_device` added, and
      `is_explicit_legacy` now admits `LEGACY_REQUESTED`.
      **`runtime_enabled` stays resident-only on purpose** until step 3 lands:
      widening it first would send a percounter run into the resident bring-up,
      which publishes no context for it and would spin in
      `wait_bootstrap_complete` to the scheduler timeout — a hang on a shared
      card. Until then percounter falls to the legacy branch and is rejected
      there, loudly, with the teardown legacy already owns.
      *File*: `aicpu/aicore_scheduler_state.h`

- [ ] **3. AICPU bring-up** — a percounter branch that reuses the resident
      lifecycle for discovery, hand-off, gate and shutdown, and writes the two
      topology numbers where `post_handshake_init` would have written contexts.
      *Files*: `aicpu/aicpu_executor.cpp`, `aicpu/aicore_lifecycle.cpp`

- [ ] **4. AICore entry branch** — dispatch to `run_percounter_executor` on
      `SCHEDULER_RUNTIME_MODE_PERCOUNTER`; today anything that is not one of the
      two resident values goes to `legacy_aicore_execute`.
      *File*: `aicore/aicore_executor.cpp`

- [ ] **5. The executor loop** — the pseudocode in
      `percounter-scheduler.md` §1: claim, spin on fanin with `ld_dev`,
      materialize the payload, execute, publish the counter, batch the completion
      count. Every spin bounded and checked against EXIT and `scheduler_error`.
      *File*: new `aicore/aicore_percounter_executor.cpp`

- [ ] **6. Sim scene tests** — `vector_example`, `single_core_dag`,
      `multi_core_dag`, `empty_lifecycle` under all three modes, plus an
      assertion that percounter actually ran.
      *Files*: `tests/st/a5/host_build_graph/…`

- [ ] **7. Build and run on a5sim** — no card.

## Constraints carried from P0

- The fanin spin reads with `__builtin_cce_ld_dev`. Not a volatile deref, which
  never observes a remote atomic; not `AtomicAdd(ptr, 0)`, which is an RMW and
  queues as `169 x consumers`. Measured: `percounter-scheduler.md` §4.
- Counters are one per 64 B line; the ticket likewise. Already enforced by
  `static_assert` in `percounter_plan.h`.
- Spin, never sleep, on the dispatch path — [`codestyle.md`](../.claude/rules/codestyle.md) §5.
- No logging on the AICPU hot path — `codestyle.md` §7.

## Open, and deliberately not settled in P2

- **Load balance across sharded tickets.** A queue whose tasks are short drains
  early and its lanes idle. The fallback ("try another cluster's ticket") is in
  the design but its value is a P4 measurement.
- **Head-of-line blocking.** A lane that has claimed a task spins on its fanin
  and cannot run something else. At ~98 cyc a check this is affordable, so v0
  ships without a lookahead window.
