# Percounter P3: the shapes that actually stress it — plan

Living plan for phase P3 of [`percounter-scheduler.md`](percounter-scheduler.md).
P2 (device side) is done — see [`percounter-p2-plan.md`](percounter-p2-plan.md).

**Scope**: close the coverage gap P2 left, in simulation. Onboard is P4.

**Branch**: `percounter-scheduler` on the fork.

## What P2 actually proved, and what it did not

P2 ran percounter against `single_core_dag`, `multi_core_dag`,
`single_root`, `vector_example` and `empty_lifecycle`, and it passed all of
them. That establishes the lifecycle: bring-up, the claim, the fanin spin, the
counter publish, the completion batch, teardown.

**It does not establish behaviour under contention**, and the reason is easy to
miss. `multi_core_dag` has exactly the shapes percounter needs — a 32-wide
fanin, a random DAG, a 4096-task multi-root — but every one of them except
`mixed_chain_65` is marked `manual: True`, so the default run collects one
65-task chain and nothing else. A chain has one runnable task at a time: it
exercises the fanin spin thoroughly and the ticket **not at all**, because no
two lanes ever contend for a claim. Every percounter result so far comes from
graphs that cannot exhibit the failure the strided claim is designed to avoid.

That is the gap. It needs no new test — it needs `--manual include`.

## Steps

- [x] **1. Run the contention shapes** under percounter, with legacy and
      resident alongside for comparison. All already exist in
      `tests/st/a5/host_build_graph/multi_core_dag/`:

      | case | shape | what it stresses |
      | --- | --- | --- |
      | `mixed_multi_root_4096` | 4096 tasks, **zero edges** | the ticket alone — every lane claims flat out, no fanin ever blocks. The purest test of the strided claim. |
      | `mixed_fanin32_1024` | 32 roots, then 992 tasks × 32 producers | the fanin spin at width: 32 `ld_dev` reads per task before it may run |
      | `mixed_random_1024` | random 0–4 fanin | irregular readiness order, so lanes finish out of claim order |
      | `mixed_chain_1024` | 1024-long chain | the serial bound, at 16× the depth P2 ran |

      Tasks alternate AIC/AIV by `logical_id` parity, so both order lists are
      populated and both tickets are drawn from.

      **All twelve pass** — every case under all three modes, each in 2–3 s.
      `mixed_multi_root_4096` runs as `4096 tasks (2048 AIC, 2048 AIV, 0 edges)`
      on `8 clusters x 2 AIV`, so each cluster's AIC lane walks 256 tasks by the
      strided mapping and nothing else gates it.

      Selection by `--case`, not `-k`: a `SceneTestCase` collects as a single
      `test_run` node and fans its `CASES` out inside, so `-k` silently matches
      the whole node and `--manual include` alone would run all of them.

      The contention is real rather than nominal: the simulator gives each
      AICore its own OS thread (`device_runner.cpp:544`), so the 24 lanes draw
      from one GM ticket concurrently, through the same atomic the hardware
      would use. What it does **not** reproduce is the hardware memory model —
      the sim host is cache-coherent, so a missing `ld_dev` or a missing
      invalidate reads correctly here and would not on silicon. Cross-core
      visibility is therefore still unverified, and that is P4's to settle, not
      something these twelve passes speak to.

- [x] **2. Settle what "percounter really ran" rests on.** The design doc asks
      for an assertion. The honest answer is that one already exists and is
      stronger than a log grep: **a forced request cannot silently fall back.**
      Inside `create_scheduler_state`, every path that would publish some other
      mode is guarded by `mode_request_needs_device_scheduler`, and the tail is
      `if (mode_request == PERCOUNTER) return create_percounter_state(...)`. So
      a forced percounter request has exactly two outcomes: percounter is
      published, or the bind fails loudly. `graph_execution` is the live
      demonstration — it refuses rather than downgrading.

      A passing run under `SIMPLER_HBG_SCHEDULER=percounter` is therefore
      already the assertion. What remains is to say so where a reader will find
      it, and to check the guard really does cover every such path rather than
      assuming it.

      Checked by enumeration rather than assumed: `create_scheduler_state` has
      exactly three sites that publish another mode and return success, and
      under a forced device request none is reachable. The table is in
      [`percounter-scheduler.md`](percounter-scheduler.md) §5, where a reader
      choosing a mode will find it.

      **Nothing holds this mechanically.** A fourth selection site added without
      the guard would restore the silent downgrade, and every percounter number
      taken after it would really be resident's. Hardening it — routing all
      three sites through one helper that takes the request and refuses a device
      mode, so the property cannot be skipped by omission — is a worthwhile
      follow-up but is a change to the selection path rather than to percounter,
      so it is left for the user to call.

- [x] **3. Whatever step 1 finds.** Nothing. P2's two bugs were both in
      bring-up, which a chain reaches as readily as a DAG; the claim path had
      never run under contention, and it turns out to be correct as written.
      The ordering argument in `percounter_claim_index`'s doc comment — a
      strided slice of an ascending list is still ascending — holds in practice
      at 4096 tasks and 24 lanes.

## What a failure here would look like

Worth writing down before running, so a hang is diagnosed rather than guessed
at — all three of these present as the same host-side timeout:

- **A gap in the claim partition** — a task no lane ever claims. Its consumers
  spin on a counter nobody will set. `resolved_task_count` stalls below
  `task_count` and the supervisor reports it.
- **An overlap** — one task claimed twice. `task_state` gets written twice, so
  this one fails the golden comparison rather than hanging.
- **A non-ascending claim** — a lane waiting on a producer its own cluster has
  not claimed yet, which is the deadlock the ordering argument rules out. Same
  symptom as the gap.

`percounter_claim_index`'s unit tests pin the mapping's arithmetic; what they
cannot pin is that `cluster_count` and the cluster index a lane derives at run
time agree with the values the tests assume.

## Not in P3

- Onboard. Timing, chip swimlane, the resident comparison — P4.
- Load balance across sharded tickets. A short-task queue drains early and its
  lanes idle; the fallback is designed but its value is a P4 measurement.
- The `empty_lifecycle` legacy hang. Not percounter's, not ours to fix; it is
  in `KNOWN_ISSUES.md` and its cell is excluded rather than worked around.
