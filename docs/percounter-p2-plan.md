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

- [x] **3. AICPU bring-up** — a percounter branch that reuses the resident
      lifecycle for discovery, hand-off, gate and shutdown, and writes the two
      topology numbers where `post_handshake_init` would have written contexts.
      *Files*: `aicpu/aicpu_executor.cpp`, `aicpu/aicore_lifecycle.cpp`
      Branches at three points inside the shared `init`, rather than forking it:
      configuration, hand-off, and skipping the bootstrap barrier. Adds
      `percounter_supervise`, which the resident supervisor cannot serve because
      it reads a `SchedulerWorkerContext`. The wire structs moved to
      `runtime/scheduler/percounter_types.h`: the AICPU needs them and
      `host/percounter_plan.h` reads the environment, which has no business in a
      header the device target compiles.

- [x] **4. AICore entry branch** — dispatch to `run_percounter_executor` on
      `SCHEDULER_RUNTIME_MODE_PERCOUNTER`; today anything that is not one of the
      two resident values goes to `legacy_aicore_execute`.
      *File*: `aicore/aicore_executor.cpp`

- [x] **5. The executor loop** — the pseudocode in
      `percounter-scheduler.md` §1: claim, spin on fanin with `ld_dev`,
      materialize the payload, execute, publish the counter, batch the completion
      count. Every spin bounded and checked against EXIT and `scheduler_error`.
      *File*: new `aicore/aicore_percounter_executor.cpp`

- [x] **5b. Unit tests for the loop.s pure logic** — the strided claim and the
      v0 core-type envelope, extracted so they can be tested at all. 33 cases.
      They do not execute the loop; they pin the two properties whose failure
      mode is a hang rather than a wrong answer.

- [x] **6. Sim scene tests** — percounter executes, and passes everything the
      other two modes pass. The host log confirms the mode rather than the pass
      doing so (`A5 HBG: selected percounter for 64 tasks (64 AIC, 0 AIV, 63
      edges)`, `topology 8 clusters x 2 AIV`), so a silent fallback could not
      have produced these results.

      | test | legacy | resident | percounter |
      | --- | --- | --- | --- |
      | `single_core_dag` | pass | pass | pass |
      | `multi_core_dag` | pass | pass | pass |
      | `single_root` (3) | pass | pass | pass |
      | `vector_example` | pass | pass | pass |
      | `empty_lifecycle` | **hangs** | pass | pass |
      | `graph_execution` (3) | pass | refused | refused |

      Two cells are not percounter's, and both were mislabelled before being
      read: `vector_example` and `graph_execution` are `manual: ["a5sim"]`, so
      they need `--manual include` or nothing is collected at all.

      - **`graph_execution` refused** under both device modes is the designed
        contract, not a failure: `A5 HBG: SIMPLER_HBG_SCHEDULER=percounter
        cannot run a graph-execution task (id=1)`. Only AUTO may fall back
        silently, and AUTO passes all three cases.
      - **`empty_lifecycle` hangs under forced legacy** — a pre-existing legacy
        defect this knob is simply the first thing to reach; AUTO sends an empty
        graph to resident. Logged in `KNOWN_ISSUES.md`; see step 6a.

      The g++-15 blocker is gone: `gxx_linux-64=15.3.0` from conda-forge into a
      home prefix, shimmed to the bare names `simpler_setup` looks for. No root,
      and nothing outside the user's home.

      Two bugs only a real run could have found, both in the bring-up
      protocol rather than the scheduling:

      - **The lane never reported its handshake.** It waited for `aicpu_ready`
        while `handshake_partition` waited for the report that wait was supposed
        to follow. Percounter needs no per-worker context, which is what made it
        look as though it needed no report either — but the AICPU publishes its
        reply only to workers a report gave it a `reg_addr` for.
      - **The supervisor returned straight out of `run()`**, skipping the
        teardown every mode owes its peers, so `shutdown_signaled_` stayed one
        short forever and four threads blamed `AICPU_SHUTDOWN_BARRIER_TIMEOUT`
        on a graph that had in fact completed.

      *Files*: `aicore/aicore_executor.cpp`, `aicpu/aicpu_executor.cpp`

      **Neither is ours to fix, and neither is re-run.** The two cells are
      excluded from the percounter matrix rather than worked around: the refusal
      is the contract behaving correctly, and the hang is legacy's termination
      logic. The hang is recorded in `KNOWN_ISSUES.md` — with `total_tasks_ ==
      0` the P thread can neither complete (`completed_` is published only from
      a completion event, and an empty graph produces none) nor time out (the
      stall latch is guarded by `total > 0`, so "no tasks" reads as "nothing
      outstanding, not a stall"), leaving no exit. Percounter runs
      `empty_lifecycle` correctly, so nothing here blocks it.

- [x] **7. Build a5sim** — no card. `build_runtimes --platforms a5sim` compiles
      all three targets, which is the only build that touches the a5 AICore and
      AICPU sources at all: **the ut-cpp tree does not compile them**, so its
      264 passing tests say nothing about this code. Running a scene test is
      still blocked, see step 6.

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
