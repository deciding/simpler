/*
 * Copyright (c) PyPTO Contributors.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 * -----------------------------------------------------------------------------------------------------------
 */
/**
 * The percounter executor: every AICore lane schedules itself.
 *
 * A lane claims a task from its cluster's ticket, spins on its producers'
 * counters until each reads >= 1, runs the kernel, and publishes by
 * incrementing its own counter. There is no scheduler lane, no wake list, no
 * ready inbox and no stealing -- the whole of the mechanism is two atomics and
 * a device load.
 *
 * Design and the hardware measurements behind it: docs/percounter-scheduler.md.
 * Three of those measurements are load-bearing here and are cited where they
 * apply, because each one looks like an arbitrary choice otherwise.
 */

#pragma once

#include "scheduler/percounter_types.h"
#include "scheduler/scheduler_graph.h"
#include "scheduler/scheduler_memory.h"
#include "scheduler/scheduler_types.h"
#include "scheduler/scheduler_topology.h"
#include "scheduler/scheduler_watchdog.h"

namespace simpler::hbg::percounter {

// Each AICore translation unit declares its own; this one is no different, and
// the signature is the incore kernel_entry ABI.
typedef void (*PercounterKernelFunc)(__gm__ int64_t *);

/**
 * Read a counter the way the runtime reads any cross-core word.
 *
 * NOT a volatile dereference. Measured on silicon: a volatile load does a real
 * 249 cyc access and never observes a remote atomic at all, across two million
 * polls with the producer confirmed to have published. `ld_dev` reaches a
 * coherent point, costs ~98 cyc at realistic fanout, and is what
 * scheduler_gm_query already uses.
 *
 * Also not `AtomicAdd(ptr, 0)`: adding zero does not skip the read-modify-write,
 * so every poll would queue at the atomic unit and cost 169 cyc x consumers.
 * Loads do not queue.
 */
inline __aicore__ uint64_t percounter_observe(uint64_t counter_address) {
    __gm__ uint64_t *word = reinterpret_cast<__gm__ uint64_t *>(counter_address);
    return scheduler_gm_query(*word);
}

/**
 * How often a lane folds its local completion count into the shared one.
 *
 * Not per task: 108 lanes incrementing one address would queue at the atomic
 * unit's service interval times the lane count, the same law the ticket obeys
 * (~169 cyc x lanes). Not once at the end either -- the AICPU supervisor's
 * progress watchdog would see no progress and kill a healthy run.
 */
inline constexpr uint64_t PERCOUNTER_COMPLETION_BATCH = 32;

/** How often a spinning lane looks at the exit register and the error word. */
inline constexpr uint32_t PERCOUNTER_SPIN_CHECK_INTERVAL = 64;

struct LaneConfig {
    __gm__ PercounterControl *control;
    SchedulerGraphView graph;
    __gm__ uint64_t *callable_addresses;
    uint32_t callable_count;
    __gm__ DispatchPayload *payload;
    __gm__ PercounterTicket *ticket;
    __gm__ int64_t *order;
    uint64_t order_count;
    uint64_t cluster_index;
    uint64_t cluster_count;
};

/**
 * Wait until every producer of `task_id` has published.
 *
 * Bounded twice over, and both bounds matter on a shared card: the watchdog
 * ends a lane whose producer will never arrive, and the periodic check lets a
 * lane leave when the run is being torn down around it. A spin here that could
 * not be interrupted would hold the card to the op timeout, which kills
 * aicpu-sd rather than failing the run.
 *
 * Spins rather than sleeping, per .claude/rules/codestyle.md section 5: this is
 * the dispatch path, and a sleep quantum lands on every task that waits.
 */
inline __aicore__ bool
percounter_wait_for_fanin(const LaneConfig &cfg, int64_t task_id, uint64_t timeout_cycles, bool *aborted) {
    __gm__ PercounterFanin *fanin =
        scheduler_state_at<PercounterFanin>(cfg.control, scheduler_gm_query(cfg.control->fanin_offset));
    __gm__ uint64_t *addresses =
        scheduler_state_at<uint64_t>(cfg.control, scheduler_gm_query(cfg.control->fanin_addr_offset));
    // Device loads, not dereferences. These tables are written by the host and
    // reach the device by DMA, which does not invalidate this core's scalar
    // DCache -- so a plain load can return whatever the line held before the
    // image landed. Measured on silicon: a 64-task chain published 59 counters
    // and then read an address of 0x120000000000 for the 60th, a value from no
    // table, because that entry's line had never been refilled. Neighbouring
    // entries in already-valid lines read correctly, which is why the failure
    // looks positional rather than systematic.
    //
    // Invisible in simulation, where `__gm__` is ordinary process memory and
    // every load is coherent by construction. The counters were always read
    // this way (see percounter_observe); the tables were the omission.
    const uint64_t record = scheduler_gm_query_u32_pair(reinterpret_cast<__gm__ uint32_t *>(&fanin[task_id]));
    const int32_t count = static_cast<int32_t>(record & 0xffffffffULL);
    const int32_t begin = static_cast<int32_t>(record >> 32);

    for (int32_t i = 0; i < count; ++i) {
        const uint64_t address = scheduler_gm_query(addresses[begin + i]);
        const uint64_t start = get_sys_cnt_aicore();
        uint32_t checks = 0;
        while (percounter_observe(address) == 0) {
            if (++checks == PERCOUNTER_SPIN_CHECK_INTERVAL) {
                checks = 0;
                if (static_cast<uint32_t>(read_reg(RegId::DATA_MAIN_BASE)) == AICORE_EXIT_SIGNAL ||
                    scheduler_gm_query(cfg.control->lane_error) != 0) {
                    *aborted = true;
                    return false;
                }
                if (scheduler_watchdog_expired(start, get_sys_cnt_aicore(), timeout_cycles)) {
                    scheduler_gm_compare_exchange(
                        cfg.control->lane_error, uint64_t{0}, static_cast<uint64_t>(SchedulerGraphResult::TIMEOUT)
                    );
                    *aborted = true;
                    return false;
                }
            }
            SPIN_WAIT_HINT();
        }
    }
    return true;
}

/**
 * Run one claimed task: build its payload, call the kernel, publish the counter.
 *
 * The publication order is what a consumer's spin depends on. `execute_task`
 * ends in OUT_OF_ORDER_STORE_BARRIER, so the kernel's outputs are in memory
 * before the atomic that announces them -- a consumer that observes the counter
 * has observed the data.
 */
inline __aicore__ bool percounter_run_task(const LaneConfig &cfg, int64_t task_id) {
    __gm__ uint8_t *descriptor = scheduler_graph_descriptor(cfg.graph, task_id);
    __gm__ int32_t *kernel_ids = reinterpret_cast<__gm__ int32_t *>(descriptor + SCHEDULER_GRAPH_KERNEL_IDS_OFFSET);

    // v0 admits one active subtask, so exactly one of the three slots is set.
    // The shape check at bind rejected everything else, which is why this can
    // take the first it finds rather than re-deriving the mask.
    int32_t kernel_id = -1;
    int32_t subtask_slot = -1;
    for (int32_t slot = 0; slot < 3; ++slot) {
        if (kernel_ids[slot] >= 0) {
            kernel_id = kernel_ids[slot];
            subtask_slot = slot;
            break;
        }
    }

    // No kernel is not a failure: a DUMMY or inline task exists only to carry
    // dependencies, and publishing its counter is the whole of its job.
    if (kernel_id < 0) return true;

    uint64_t function_address = 0;
    if (!scheduler_lookup_callable_address(
            cfg.callable_addresses, cfg.callable_count, static_cast<uint16_t>(kernel_id), &function_address
        )) {
        return false;
    }

    SchedulerTaskInfo info{task_id, kernel_id, subtask_slot, subtask_slot == 0 ? CoreType::AIC : CoreType::AIV};
    if (scheduler_materialize_task_payload_resolved(cfg.graph, info, function_address, cfg.payload) !=
        SchedulerGraphResult::OK) {
        return false;
    }
    scheduler_writeback_dispatch_payload(cfg.payload);

    // The same three lines as aicore_executor.cpp's execute_task, which is in
    // that translation unit's anonymous namespace and so cannot be called from
    // here. The barrier is the part that matters and is not optional: it is the
    // publication fence a consumer's spin depends on, so the kernel's outputs
    // are in memory before the atomic that announces them.
    PercounterKernelFunc kernel = reinterpret_cast<PercounterKernelFunc>(cfg.payload->function_bin_addr);
    kernel(reinterpret_cast<__gm__ int64_t *>(cfg.payload->args));
    OUT_OF_ORDER_STORE_BARRIER();
    return true;
}

/**
 * The lane loop.
 *
 * Claims are strided rather than contiguous: this cluster takes indices
 * `cluster + k * cluster_count` of one ascending per-core-type list. A strided
 * slice of an ascending list is still ascending, which is what makes the design
 * deadlock-free -- the lowest-id incomplete task is always the next claim of
 * whichever cluster owns it. It is also why the host can build the lists
 * without knowing the topology, which it does not at bind time.
 */
inline __aicore__ void percounter_lane_loop(const LaneConfig &cfg, uint64_t timeout_cycles) {
    uint64_t local_done = 0;
    __gm__ PercounterCounter *counters =
        scheduler_state_at<PercounterCounter>(cfg.control, scheduler_gm_query(cfg.control->counters_offset));

    while (true) {
        if (static_cast<uint32_t>(read_reg(RegId::DATA_MAIN_BASE)) == AICORE_EXIT_SIGNAL) break;
        if (scheduler_gm_query(cfg.control->lane_error) != 0) break;

        const uint64_t claim = scheduler_gm_fetch_add(cfg.ticket->next, 1);
        uint64_t index = 0;
        // False means this cluster's share is exhausted. Every other index
        // belongs to some other cluster's stride, so there is nothing left here
        // to help with.
        if (!percounter_claim_index(cfg.cluster_index, cfg.cluster_count, claim, cfg.order_count, &index)) break;

        const int64_t task_id = static_cast<int64_t>(scheduler_gm_query(cfg.order[index]));
        if (task_id < 0 || static_cast<uint64_t>(task_id) >= cfg.graph.task_count) {
            scheduler_gm_compare_exchange(
                cfg.control->lane_error, uint64_t{0}, static_cast<uint64_t>(SchedulerGraphResult::INVALID_TASK_ID)
            );
            break;
        }

        bool aborted = false;
        if (!percounter_wait_for_fanin(cfg, task_id, timeout_cycles, &aborted)) break;

        if (!percounter_run_task(cfg, task_id)) {
            scheduler_gm_compare_exchange(
                cfg.control->lane_error, uint64_t{0}, static_cast<uint64_t>(SchedulerGraphResult::INVALID_CALLABLE)
            );
            break;
        }

        scheduler_gm_fetch_add(counters[task_id].value, 1);
        if (++local_done == PERCOUNTER_COMPLETION_BATCH) {
            scheduler_gm_fetch_add(cfg.control->resolved_task_count, local_done);
            local_done = 0;
        }
    }
    if (local_done != 0) scheduler_gm_fetch_add(cfg.control->resolved_task_count, local_done);
}

/**
 * A percounter lane, from READY to the exit acknowledgement.
 *
 * Shorter than its resident counterpart by everything resident needs a
 * scheduler for. There is no bootstrap: resident's scheduler AIVs classify the
 * whole graph before the gate opens, while a percounter lane has nothing to do
 * until it claims, so it goes straight from READY to the gate to the loop.
 */
inline __aicore__ void run_percounter_lane(__gm__ PercounterControl *control, int block_idx, CoreType core_type) {
    scheduler_observe_data_cache(control);
    const uint64_t timeout_cycles = scheduler_gm_query(control->scheduler_timeout_cycles);

    const bool is_aic = core_type == CoreType::AIC;
    const uint32_t type_index = is_aic ? 0U : 1U;
    SchedulerClusterCoordinate coordinate{-1, -1};
    if (!scheduler_cluster_coordinate_from_worker(
            block_idx, is_aic, static_cast<int32_t>(scheduler_gm_query(control->cluster_count)),
            static_cast<int32_t>(scheduler_gm_query(control->aiv_per_cluster)), &coordinate
        )) {
        // The AICPU validated the topology before publishing it, so failing here
        // means this lane is not in the topology that was published -- claiming
        // anyway would take another cluster's stride.
        scheduler_gm_compare_exchange(
            control->lane_error, uint64_t{0}, static_cast<uint64_t>(SchedulerGraphResult::INVALID_ARGUMENTS)
        );
        return;
    }

    LaneConfig cfg{};
    cfg.control = control;
    // Every field here goes through a device load. A plain dereference of this
    // block is the defect that produced the whole AIC failure: a lane that read
    // `dispatch_payloads_offset` as 0 placed its DispatchPayload at
    // `control + block_idx * 512`, which for an AIC block index lands inside the
    // ticket array, and the first thing payload materialization writes there is
    // the kernel entry address. That is where the foreign constant in a ticket
    // came from. AIV block indices are 32..95, so the same miscalculation lands
    // past the tickets and the failure looked core-type specific.
    cfg.graph = SchedulerGraphView{
        scheduler_gm_query(control->graph_storage_address), 0, scheduler_gm_query(control->task_count), 0
    };
    cfg.callable_addresses =
        reinterpret_cast<__gm__ uint64_t *>(scheduler_gm_query(control->callable_addresses_address));
    cfg.callable_count = static_cast<uint32_t>(scheduler_gm_query(control->callable_addresses_count));
    cfg.payload = scheduler_state_at<DispatchPayload>(
        control, scheduler_gm_query(control->dispatch_payloads_offset) +
                     static_cast<uint64_t>(block_idx) * sizeof(DispatchPayload)
    );
    cfg.ticket = scheduler_state_at<PercounterTicket>(
        control,
        scheduler_gm_query(control->tickets_offset) + (static_cast<uint64_t>(type_index) * PERCOUNTER_TICKET_CAPACITY +
                                                       static_cast<uint64_t>(coordinate.cluster_index)) *
                                                          sizeof(PercounterTicket)
    );
    cfg.order = scheduler_state_at<int64_t>(control, scheduler_gm_query(control->orders_offset[type_index]));
    cfg.order_count = scheduler_gm_query(control->order_count[type_index]);
    cfg.cluster_index = static_cast<uint64_t>(coordinate.cluster_index);
    cfg.cluster_count = scheduler_gm_query(control->cluster_count);

    percounter_lane_loop(cfg, timeout_cycles);
}

}  // namespace simpler::hbg::percounter
