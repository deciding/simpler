# Percounter P4: onboard — plan

Living plan for phase P4 of [`percounter-scheduler.md`](percounter-scheduler.md).
P3 (contention shapes in simulation) is done — see
[`percounter-p3-plan.md`](percounter-p3-plan.md).

**Branch**: `percounter-scheduler` on the fork.

## Why simulation cannot have settled this

P3 passed 4096 tasks across 24 concurrently-running lanes, and that is real
evidence about the claim partition. It is **no evidence at all about memory
ordering**, and the reason is worth stating precisely because the passes look
so reassuring.

`scheduler_gm_query` — the read the whole fanin spin is built on — is two
different functions:

```cpp
#if defined(__CCE_AICORE__)
    return __builtin_cce_ld_dev(address, 0);   // onboard: bypasses the scalar DCache
#else
    return __atomic_load_n(&value, order);     // sim: a coherent load on the host CPU
#endif
```

The simulator runs one OS thread per AICore on a cache-coherent host, so under
`a5sim` every lane sees every other lane's write whether or not the code asked
to. **A missing `ld_dev`, a missing `dcci`, or a barrier in the wrong place all
pass in simulation and hang on silicon.** That is the class of defect P4 exists
to find, and P0 measured the primitives precisely because this is where the
design's assumptions live:

- the fanin spin must use `ld_dev`, not a volatile deref (which never observes a
  remote atomic) and not `AtomicAdd(ptr, 0)` (an RMW, `169 x consumers`)
- counters and tickets must each own a cache line, because the atomic unit
  serialises per line — 16 per line measured 6371 cyc against 446 for one

Simulation can confirm neither, since neither has any effect there.

## Steps

- [x] **1. Capability gate and the onboard build.** `onboard-arch-precheck a5`
      passes on this box and `task-submit` is on `PATH`, so per
      [`running-onboard.md`](../.claude/rules/running-onboard.md) hardware work
      proceeds without asking. `build_runtimes --platforms a5` builds both
      runtimes.

      **This is the first time `ccec` has compiled the percounter lane.** P2's
      a5sim build used g++-15, which takes the `#else` branch of every
      `__CCE_AICORE__` gate — so the SPR reads, the barriers and `ld_dev` itself
      were all stubs until now. `ccec` accepted them: `aicore_kernel.o` rebuilds
      clean, `libaicpu_kernel.so` carries the three percounter lifecycle
      symbols, `libhost_runtime.so` carries the selection string.

      (The build logs two `[ERROR] [AICPU] CMake configuration failed` lines and
      still returns 0. Both are a stale cache — `You have changed variables that
      require your cache to be deleted` — which the builder clears and retries
      successfully. Transient, not a defect, but it makes `rc` the only
      trustworthy signal in that log.)

- [ ] **1a. `single_core_dag` does not pass on silicon.** Resident passes on the
      same card in the same submission, so this is percounter's and not the
      box's. The smoke run is one case, `aic_chain_64`: a 64-task AIC chain,
      which on this silicon spreads over **32 clusters** rather than the
      simulator's 8, so every edge in it crosses clusters.

      Three observations across runs, each from the device log:

      | run | outcome |
      | --- | --- |
      | first | `lane reported error 10` (a fanin-spin timeout) — no locators yet |
      | with locators | `task=59 edge=0 cluster=27`, waiting on `0x120000000000`, while `counters base` is `0x1200000ce800`. **59/64 counters published; unpublished 59–63.** The waited-on address is not any counter's — `counters[58]` would be `0x1200000cf680` — so the lane read an entry that was not in the fanin-address table. |
      | after reading the tables with `ld_dev` | stalls earlier: `no progress; 24 of 64 resolved`, and **no lane claimed an error** — the supervisor's watchdog won the race, so the locators were unset |

      The stall point moving between runs (59, then 24) is itself evidence: a
      deterministic table bug would stop in the same place, and the host builds
      the identical table in simulation where it passes. `resolved` advances
      only when a lane exits, and each lane owns 2 of the 64 tasks, so 24 means
      12 lanes exited — the chain had progressed well past its start.

      What is fixed so far: the lane read `fanin[]`, the fanin-address array and
      the order list with **plain dereferences**. Only the counters went through
      `ld_dev`. Those tables arrive by DMA and the AICore scalar DCache is not
      coherent with it, so the reads are now device loads too, and the order
      list widened from `int32_t` to `int64_t` so that every host-published
      per-task table is uniformly 8 bytes and readable by one `ld_dev`. That was
      necessary but is evidently not sufficient.

      Instrumentation now in place, because guessing costs a card round-trip
      each: `percounter_report_stall` runs from **both** supervisor exits and
      prints the counter vector plus, for every value the lane recorded, the
      AICPU's authoritative reading of the same location. The AICPU is coherent
      with both the atomic unit and the host image, so the comparison separates
      the three candidates — producer never ran, table built wrong, lane could
      not read what was correct — and only the third is a defect simulation
      cannot reproduce.

      **It is intermittent, which is the most important fact about it.** Eight
      repeats of the same case: 2–3 pass, the rest stall, and the stall point
      moves between runs (24, 36, 44, 52, 55, 59, 60 of 64). A deterministic
      table or layout error would stop in the same place every time. So this is
      a race, and the ~30% pass rate is why a single green run must not be read
      as a fix — the run before these statistics passed, and meant nothing.

      What the instrumentation has since ruled out:

      - **The table reads are now correct.** At the stall the lane and the AICPU
        report *identical* `fanin{count,begin}` and the identical producer
        address. The `ld_dev` change did its job.
      - **The producer genuinely never published** — the AICPU reads the waited-on
        counter as 0 too. So it is not a counter a lane cannot see; it is a task
        no lane ever ran.
      - **The published image is correct at bring-up.** A check added to
        `percounter_post_handshake_init` verifies every ticket reads zero before
        the gate opens, and it never fires.

      What is left, and unexplained: at stall time the **ticket array contains
      pointer-like values** — repeatedly `19791210123680` = `0x1200000CE8E0`,
      which is an address inside the region itself (`&fanin_addr[60]`), in slots
      that were verified zero before the gate. A cluster whose ticket reads that
      instead of a small count would have its first claim land far past the end
      of its stride, and `percounter_claim_index` retires such a lane *silently*
      — leaving its tasks unclaimed and every consumer downstream spinning on
      counters nobody will set. That matches the symptom exactly. What writes
      those values is not yet known: the cluster mapping is bounds-checked
      (`scheduler_topology.h`), so no ticket write can land out of range, and
      the payload segment sits after the counters rather than before the
      tickets.

      The next instrument is a per-lane census — each lane recording its own
      cluster, ticket address, claims drawn and exit reason into a dedicated
      array — which ends the deduction by naming the lane and the pointer
      directly rather than inferring both from their consequences.

      **It is not intermittent, and it is not corruption.** Both of those
      earlier readings were wrong, and the census plus a per-case sweep replaced
      them with a single reproducible statement. Run each case alone:

      | case | core | claims/lane | fanin waits | result |
      | --- | --- | --- | --- | --- |
      | `aic_chain_64` | AIC | 2 | every task | **0 / 6** |
      | `aiv_chain_64` | AIV | ~1 | every task | 6 / 6 |
      | `mixed_diamond_8` | mixed | ≤1 | few | 6 / 6 |
      | `aic_multi_root_64` | AIC | 2 | **none** | 3 / 3 |

      The last row is what makes it precise. AIC with two claims and no fanin
      passes, so neither the core type nor the second claim is the fault; the
      broken ingredient is **a device-load spin on a cube core followed by
      another GM atomic**.

      The census names the failure exactly:

      ```
      lane 12  state=STRIDE_EXHAUSTED  cluster=12  claims=2  completed=1
               last_claim=19791210123680      <- should be 1
      ```

      The lane's second `atomicAdd` on its own ticket returns
      `19791210123680` (`0x1200000C9220`) instead of the old count.
      `percounter_claim_index` correctly judges that past the end of the stride
      and retires the lane, which has run one of its two tasks — so its second
      task never runs and every consumer below it spins forever. The stall point
      moving between runs was only ever *which* cluster reached its second claim
      first.

      **The value is a constant.** The same `19791210123680` appears on whichever
      lane fails, across every run — it is not random memory. And the raw region
      dump shows the control block completely intact (`task_count=64
      edge_count=63 order_count={64,0} tickets=256 fanin=5376 addr=5888
      counters=6400 clusters=32 aiv=2`), so nothing is overwriting anything.

      Two fixes tried and rejected, both informative:

      - **A `dsb` after the spin** (`scheduler_observe_dispatch_payload_barrier`)
        does not drain whatever the spin leaves behind. Kept, since it is
        correct on its own terms and costs nothing.
      - **Swapping the spin to `dcci`-invalidate + load**, P0's other working
        idiom, makes `last_claim` correct (0 rather than the constant) but makes
        *visibility* far worse — 12 of 64 tasks run instead of 44–60. So
        `ld_dev` is the right read and the defect is its interaction with the
        following atomic, not the read itself. Reverted.

      Why no earlier phase could have found it: simulation compiles `ld_dev` to
      a coherent host load; resident's scheduling runs on an elected **scheduler
      AIV**, so an AIC core there only ever executes a kernel and writes a
      register; and P0's `counter_deps` measured `ld_dev` polling on **AIV
      only**, never interleaved with an atomic on a cube core. Percounter is the
      first code in this runtime to put a claim loop on an AIC core.

      **The cause was none of the above, and the reading that found it came from
      asking why a counter read could possibly affect a ticket.** It cannot:
      they are unrelated cache lines and nothing in the design writes one from
      the other. Probing the ticket on both sides of the draw settled it in one
      run — `before` already held the foreign value, read by an independent
      `ld_dev`, and the live pointer matched the entry-time one. So the return
      path was innocent, the pointer was innocent, and the ticket word itself
      had been overwritten.

      Bisecting the lane loop found the writer immediately: skipping
      `percounter_run_task` stopped the corruption, and skipping only the kernel
      call did not. That leaves `scheduler_materialize_task_payload_resolved`,
      which writes nothing but `dispatch_payload->…` — so the payload pointer
      was wrong, and the arithmetic closes:

      ```
      cfg.payload = control + dispatch_payloads_offset + block_idx * 512
      control + 1536 == cluster 20's ticket      (1536 = 256 + 20*64)
      1536           == 0 + 3*512                 ⇒ offset read as 0, block_idx 3
      ```

      **The lane read `control->dispatch_payloads_offset` as zero.** The table
      reads had been converted to device loads; the control block's own fields
      had not, and it reaches the device by the same DMA with the same
      incoherent scalar DCache. The first thing materialization writes to the
      bogus payload is `function_bin_addr` — a kernel entry address, which is
      exactly the foreign constant that kept turning up in tickets, sitting just
      below the callable table.

      That also explains the core-type split mechanically, with no hardware
      asymmetry involved: an AIC `block_idx` is 0–31, so the miscalculated
      payload lands at offsets 0–15872, straddling the tickets, orders, fanin
      and counters; an AIV `block_idx` is 32–95, landing at ≥16384, inside the
      legitimate payload area, where it overwrites payload slots that are
      rewritten before every use and therefore does no visible harm.

      **Fix**: every `control->` field read on the lane goes through
      `scheduler_gm_query`, the same discipline the tables already had.

      | case | before | after |
      | --- | --- | --- |
      | `aic_chain_64` | 0 / 6 | **6 / 6** |
      | `aiv_chain_64` | 6 / 6 | 6 / 6 |
      | `mixed_diamond_8` | 6 / 6 | 6 / 6 |
      | all three in one process | 0 / 6 | **6 / 6** |

      The `dsb` added after the fanin spin was removed with the rest: it was
      justified by the spin-poisons-the-atomic reading, which this refutes, and
      an unexplained barrier on the dispatch path is a cost with a wrong story
      attached.

      All the instrumentation is gone rather than left behind a macro — a lane
      census, per-lane probes and a stall report are per-task GM writes and a
      permanent 7 KB of region, and [`env-macro-gating.md`](../.claude/rules/env-macro-gating.md)
      §1 prefers no gate to a gate. What they established is recorded here.

- [ ] **2. Scene tests onboard, all three modes.** The P2/P3 set on real
      silicon: `single_core_dag`, `multi_core_dag` (including the manual
      contention cases), `single_root`, `vector_example`. Excluded as in P3 and
      for the same reasons: `graph_execution` under a forced device mode
      (refused by contract) and `empty_lifecycle` under legacy (not ours).

- [ ] **3. Timing.** Device Total and the chip swimlane across the three modes,
      on the shapes where they differ: the 4096-task zero-edge case is the one
      percounter should win, since it is pure claim throughput with no
      dependency resolution for resident's scheduler to do.

- [ ] **4. Whatever steps 2–3 find.**

## Card discipline

From `a5_test`'s rules, which this box shares:

- **Only physical cards 0, 1, 2, 3.** Never `--device auto` — the picker can
  hand out 4–7, which belong to other users here.
- Everything that occupies a card goes through `task-submit`.
- Experiments finish in minutes. The scene tests above are seconds each.
- Device log to a per-run directory via `ASCEND_PROCESS_LOG_PATH`, so this
  run's log is not fished out of the shared `~/ascend/log/debug/` by pid
  guesswork.

The box is shared and currently busy — 0–3 held by a `pypto-lib` CI runner,
4–7 by another `simpler` pytest — so these runs queue rather than preempt.

## How a failure will present, and what it would mean

All of these reach the host as one generic code (`507018` /
`aclrtSynchronizeStreamWithTimeout failed`), so the device log decides which it
is — see `running-onboard.md`'s triage table.

| symptom | reading |
| --- | --- |
| hang, `resolved_task_count` stalled below `task_count` | a lane spinning on a counter it cannot **see**. The coherence bug sim cannot show: the producer published, the consumer's read did not observe it. |
| hang with no allocator/pool fatal and only `HandleTaskTimeout` | not a deadlock — a long or stalled op. Raise `SIMPLER_SCHEDULER_TIMEOUT_MS` before concluding. |
| wrong results rather than a hang | the claim partition overlapping, which P3 should have caught and did not reproduce |
| correct but far slower than resident | the line-isolation assumption failing, or ticket contention worse than P0's sharded measurement predicted |

## Not in P4

- Load balance across sharded tickets. A queue of short tasks drains early and
  its lanes idle; the fallback is designed but unmeasured.
- Docs pass — P5.
