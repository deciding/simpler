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
 * The percounter scheduler's GM wire structs, shared by the host that builds
 * them, the AICPU that publishes the topology into them, and the AICore that
 * reads them.
 *
 * Split from host/percounter_plan.h, which keeps the parts only the host has
 * any business with: the mode request (it reads the environment, and <cstdlib>
 * does not belong in a header the AICore target compiles), the layout planner
 * and the table builder.
 *
 * Every offset here is from the region base, never a pointer, so the whole
 * region survives one memcpy to the device.
 */

#pragma once

#include <cstdint>
#include <type_traits>

#include "scheduler/scheduler_layout.h"
#include "scheduler/scheduler_types.h"

namespace simpler::hbg::percounter {

// =========================================================================
// GM tables
// =========================================================================

inline constexpr uint32_t PERCOUNTER_CORE_TYPE_COUNT = 2;  // 0 = AIC, 1 = AIV
inline constexpr uint32_t PERCOUNTER_TICKET_CAPACITY = SCHEDULER_CLUSTER_CAPACITY;

/**
 * One counter per 64 B cache line.
 *
 * Not a tuning choice. The atomic unit serialises per line rather than per
 * address, so N counters sharing a line cost N times the latency: measured at
 * 6371 cyc for 16 per line against 446 for one. Packing stays *correct* -- no
 * update is lost -- which is why this needs saying. See
 * docs/percounter-scheduler.md §4.
 */
struct alignas(64) PercounterCounter {
    uint32_t value;
    uint8_t reserved[60];
};
static_assert(sizeof(PercounterCounter) == 64, "a counter owns exactly one cache line");
static_assert(alignof(PercounterCounter) == 64, "counters must not straddle lines");

/**
 * One ticket per (core type, cluster). Also line-isolated: two tickets sharing
 * a line would serialise two clusters against each other for the same reason
 * the counters do.
 */
struct alignas(64) PercounterTicket {
    uint32_t next;
    uint8_t reserved[60];
};
static_assert(sizeof(PercounterTicket) == 64, "a ticket owns exactly one cache line");

/** Where a task's producers live in the flat fanin-address array. */
struct PercounterFanin {
    int32_t count;
    int32_t begin;
};
static_assert(sizeof(PercounterFanin) == 8, "percounter fanin record is a wire struct");
static_assert(std::is_trivially_copyable_v<PercounterFanin> && std::is_standard_layout_v<PercounterFanin>);

/**
 * The device-visible header: where every other segment is, and how long the two
 * order lists are. Offsets are from the percounter region's base, never
 * pointers, so the whole region survives one memcpy to the device.
 */
struct alignas(128) PercounterControl {
    uint64_t task_count;
    uint64_t edge_count;
    uint64_t order_count[PERCOUNTER_CORE_TYPE_COUNT];
    uint64_t orders_offset[PERCOUNTER_CORE_TYPE_COUNT];
    uint64_t tickets_offset;
    uint64_t fanin_offset;
    uint64_t fanin_addr_offset;
    uint64_t counters_offset;
    // Written by the AICPU after the handshake, not by the host: the cluster
    // count is discovered at bring-up. Two numbers are the whole of what a lane
    // needs to place itself, because scheduler_cluster_coordinate_from_worker is
    // a pure function of them plus the block index it already has. That is why
    // percounter needs no per-worker context array and no scheduler election.
    uint64_t cluster_count;
    uint64_t aiv_per_cluster;
    // Lanes batch their completions into this rather than incrementing once per
    // task: 108 lanes on one hot address would queue at S x ncores, the same law
    // the ticket obeys. The AICPU supervisor polls it against task_count.
    // Not volatile, and nothing in this struct is: a wire struct has to stay
    // trivially copyable to survive the memcpy to the device, and volatile
    // members would forfeit that. Cross-core visibility here is the same
    // explicit discipline the rest of the runtime uses -- atomicAdd to write,
    // ld_dev or an invalidate to read -- not a type qualifier.
    uint64_t resolved_task_count;
    // First failure wins; the supervisor breaks on any non-zero value.
    uint64_t lane_error;
    // Absolute device addresses the host resolves at bind, because none of them
    // is inside this region and the lane has no other way to reach them: the
    // task table lives in the shared-memory image and the callable table is
    // owned by registration.
    uint64_t graph_storage_address;
    uint64_t callable_addresses_address;
    uint64_t callable_addresses_count;
    // One DispatchPayload per lane. Resident double-buffers because its
    // scheduler fills the next payload while a worker runs the current one;
    // a percounter lane fills its own and then runs it, so there is never a
    // second one in flight.
    uint64_t dispatch_payloads_offset;
    uint8_t reserved[256 - 18 * sizeof(uint64_t)];
};
static_assert(sizeof(PercounterControl) == 256, "percounter control is two 128 B lines");
static_assert(std::is_trivially_copyable_v<PercounterControl> && std::is_standard_layout_v<PercounterControl>);


}  // namespace simpler::hbg::percounter
