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
 * Host-side planning for the percounter scheduler: which scheduling mode a run
 * asked for, and the GM tables that mode needs.
 *
 * Host-only. The mode request reads the environment, and <cstdlib> has no
 * business in a header the AICore target compiles.
 *
 * Everything here is `inline` and free-standing so it can be unit-tested.
 * create_scheduler_state, which calls it, sits in runtime_maker.cpp's anonymous
 * namespace and is therefore reachable from no test at all; the logic this
 * header holds must not inherit that.
 *
 * See docs/percounter-scheduler.md for the design and the hardware
 * measurements behind the two constants that look arbitrary here (the 64 B
 * counter stride and the per-cluster ticket).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "scheduler/percounter_types.h"
#include "scheduler/scheduler_layout.h"
#include "scheduler/scheduler_types.h"

namespace simpler::hbg::percounter {

// The scheduler a run asked for. Distinct from the mode it ends up publishing:
// AUTO resolves to one of the others by inspecting the graph, and a forced
// request that the graph cannot satisfy fails the bind rather than falling
// back.
enum class ModeRequest : uint8_t {
    AUTO = 0,
    LEGACY,
    RESIDENT,
    PERCOUNTER,
};

inline constexpr const char *SIMPLER_HBG_SCHEDULER_ENV = "SIMPLER_HBG_SCHEDULER";

/**
 * Parse a mode request spelling. Returns false for anything unrecognised,
 * including the empty string.
 *
 * An unrecognised value is rejected rather than ignored. A misspelled
 * environment variable that silently does nothing is the failure mode
 * .claude/rules/env-macro-gating.md exists to prevent: it does not fail where
 * it is set, it fails much later as a performance mystery.
 *
 * nullptr means "not set" and yields AUTO, which is the only way to get the
 * automatic choice.
 */
inline bool parse_mode_request(const char *text, ModeRequest *out)
{
    if (out == nullptr) return false;
    if (text == nullptr) {
        *out = ModeRequest::AUTO;
        return true;
    }
    if (std::strcmp(text, "auto") == 0) {
        *out = ModeRequest::AUTO;
        return true;
    }
    if (std::strcmp(text, "legacy") == 0) {
        *out = ModeRequest::LEGACY;
        return true;
    }
    if (std::strcmp(text, "resident") == 0) {
        *out = ModeRequest::RESIDENT;
        return true;
    }
    if (std::strcmp(text, "percounter") == 0) {
        *out = ModeRequest::PERCOUNTER;
        return true;
    }
    return false;
}

/**
 * Read SIMPLER_HBG_SCHEDULER. Split from parse_mode_request so the parsing is
 * testable without a test having to mutate the environment.
 *
 * An empty value is treated as unset, matching how the repo's other knobs
 * behave when a shell exports a variable with nothing in it.
 */
inline bool resolve_mode_request(ModeRequest *out)
{
    const char *text = std::getenv(SIMPLER_HBG_SCHEDULER_ENV);
    if (text != nullptr && text[0] == '\0') text = nullptr;
    return parse_mode_request(text, out);
}

/** The spelling a request came from, for log lines and error messages. */
inline const char *mode_request_name(ModeRequest request)
{
    switch (request) {
        case ModeRequest::AUTO: return "auto";
        case ModeRequest::LEGACY: return "legacy";
        case ModeRequest::RESIDENT: return "resident";
        case ModeRequest::PERCOUNTER: return "percounter";
    }
    return "?";
}

/**
 * Whether a request pins the scheduler. A pinned request that the graph cannot
 * satisfy is a bind failure; only AUTO is allowed to fall back.
 */
inline bool mode_request_is_forced(ModeRequest request)
{
    return request != ModeRequest::AUTO;
}

/** Host-side plan. Mirrors AicoreSchedulerLayout's role for the resident mode. */
struct PercounterLayout {
    uint64_t total_size;
    uint64_t task_count;
    uint64_t edge_count;
    uint64_t order_count[PERCOUNTER_CORE_TYPE_COUNT];
    uint64_t control_offset;
    uint64_t tickets_offset;
    uint64_t orders_offset[PERCOUNTER_CORE_TYPE_COUNT];
    uint64_t fanin_offset;
    uint64_t fanin_addr_offset;
    uint64_t counters_offset;
};

/**
 * Plan the percounter region.
 *
 * `order_count[t]` is how many tasks of core type `t` will be placed. It is a
 * parameter rather than something derived here because the caller has already
 * walked the graph to classify shapes, and walking it twice invites the two
 * walks to disagree.
 *
 * Note what is NOT here: per-queue order lists. The host cannot size those --
 * it does not know the cluster count at bind time, because post_handshake_init
 * discovers it on the AICPU. A host that guessed would strand every task in the
 * queues that turn out not to exist. One list per core type, sliced by stride
 * on the device, needs no topology at all.
 */
inline bool plan_layout(
    uint64_t task_count, uint64_t edge_count, const uint64_t order_count[PERCOUNTER_CORE_TYPE_COUNT],
    PercounterLayout *layout
)
{
    if (layout == nullptr || order_count == nullptr) return false;
    uint64_t placed = 0;
    for (uint32_t t = 0; t < PERCOUNTER_CORE_TYPE_COUNT; ++t) {
        if (order_count[t] > task_count) return false;
        placed += order_count[t];
    }
    // Every task is placed at most once, so the two lists cannot together
    // exceed the graph. They can be shorter: inline-completed tasks are already
    // done and belong in neither.
    if (placed > task_count) return false;

    PercounterLayout next{};
    next.task_count = task_count;
    next.edge_count = edge_count;
    for (uint32_t t = 0; t < PERCOUNTER_CORE_TYPE_COUNT; ++t) next.order_count[t] = order_count[t];

    uint64_t cursor = 0;
    uint64_t bytes = 0;
#define PERCOUNTER_RESERVE(count, type, field)                      \
    (scheduler_layout_checked_mul((count), sizeof(type), &bytes) && \
     scheduler_layout_reserve(&cursor, bytes, alignof(type), &next.field))

    if (!scheduler_layout_reserve(
            &cursor, sizeof(PercounterControl), alignof(PercounterControl), &next.control_offset
        ) ||
        !PERCOUNTER_RESERVE(
            static_cast<uint64_t>(PERCOUNTER_CORE_TYPE_COUNT) * PERCOUNTER_TICKET_CAPACITY, PercounterTicket,
            tickets_offset
        ) ||
        !PERCOUNTER_RESERVE(order_count[0], int32_t, orders_offset[0]) ||
        !PERCOUNTER_RESERVE(order_count[1], int32_t, orders_offset[1]) ||
        !PERCOUNTER_RESERVE(task_count, PercounterFanin, fanin_offset) ||
        !PERCOUNTER_RESERVE(edge_count, uint64_t, fanin_addr_offset) ||
        !PERCOUNTER_RESERVE(task_count, PercounterCounter, counters_offset) ||
        !scheduler_layout_checked_align(cursor, SCHEDULER_STATE_ALIGNMENT, &next.total_size)) {
#undef PERCOUNTER_RESERVE
        return false;
    }
#undef PERCOUNTER_RESERVE

    *layout = next;
    return true;
}

/**
 * Which order list a task belongs in, from its active-subtask mask.
 *
 * Deliberately the same envelope as the resident scheduler's
 * scheduler_metadata_single_subtask_slot: mask 1 is the cube core, mask 2 the
 * first vector core, and everything else — empty, AIV1 alone, or any multi-slot
 * MIX — is outside v0 and must have been rejected by the shape check before
 * reaching here. Mirroring it rather than widening it keeps one definition of
 * "a v0 shape" instead of two that can drift.
 */
inline bool core_type_index_from_active_mask(uint8_t active_mask, uint8_t *out)
{
    if (out == nullptr) return false;
    if (active_mask == 1U) {
        *out = 0;  // AIC
        return true;
    }
    if (active_mask == 2U) {
        *out = 1;  // AIV0
        return true;
    }
    return false;
}

/** Whether a request pins the run to a scheduler that executes on the device. */
inline bool mode_request_needs_device_scheduler(ModeRequest request)
{
    return request == ModeRequest::RESIDENT || request == ModeRequest::PERCOUNTER;
}

/** One task, as the table builder needs to see it. */
struct TaskInput {
    uint8_t core_type_index;   // 0 = AIC, 1 = AIV
    bool inline_completed;     // already done at bind: counter preset, placed in no list
    int32_t fanin_count;
    const int32_t *fanin_ids;  // producer task ids; may be null when count is 0
};

/** Why build_tables refused. Every one of these fails the bind. */
enum class BuildStatus : uint8_t {
    OK = 0,
    INVALID_ARGUMENT,
    BAD_CORE_TYPE,
    ORDER_COUNT_MISMATCH,   // the plan's list lengths disagree with the tasks
    EDGE_COUNT_MISMATCH,    // the plan's edge total disagrees with the tasks
    PRODUCER_NOT_BEFORE_CONSUMER,
    PRODUCER_OUT_OF_RANGE,
};

inline const char *build_status_name(BuildStatus status)
{
    switch (status) {
        case BuildStatus::OK: return "ok";
        case BuildStatus::INVALID_ARGUMENT: return "invalid argument";
        case BuildStatus::BAD_CORE_TYPE: return "core type out of range";
        case BuildStatus::ORDER_COUNT_MISMATCH: return "order count disagrees with the task list";
        case BuildStatus::EDGE_COUNT_MISMATCH: return "edge count disagrees with the task list";
        case BuildStatus::PRODUCER_NOT_BEFORE_CONSUMER: return "a fanin edge does not point backwards";
        case BuildStatus::PRODUCER_OUT_OF_RANGE: return "a fanin edge names no task";
    }
    return "?";
}

/** The task a failure is about, for the log line. -1 when it is not about one. */
struct BuildResult {
    BuildStatus status = BuildStatus::OK;
    int64_t task_id = -1;
    int32_t fanin_index = -1;
};

template <typename T>
inline T *region_at(void *base, uint64_t offset)
{
    return reinterpret_cast<T *>(static_cast<uint8_t *>(base) + offset);
}

/**
 * Fill the percounter region.
 *
 * `base` is the host staging mirror; `device_base` is where that image will
 * land, and is what the stored fanin addresses are computed against. The two
 * differ, which is the whole reason the addresses are written here rather than
 * derived on the device.
 *
 * Ascending task id is already a topological order in hbg -- append_fanin_or_fail
 * rejects any producer at or past what the run has claimed, and ids are minted
 * in order and never recycled. The whole design rests on that, so this checks it
 * per edge instead of trusting it: O(edges), and it turns a wrong assumption
 * into a bind failure rather than a device that hangs on a counter nobody will
 * ever set.
 */
inline bool build_tables(
    const TaskInput *tasks, uint64_t task_count, const PercounterLayout &layout, void *base, uint64_t device_base,
    BuildResult *result = nullptr
)
{
    const auto fail = [&](BuildStatus status, int64_t task_id, int32_t fanin_index) {
        if (result != nullptr) *result = BuildResult{status, task_id, fanin_index};
        return false;
    };
    if (result != nullptr) *result = BuildResult{};
    if (base == nullptr || (tasks == nullptr && task_count != 0) || task_count != layout.task_count)
        return fail(BuildStatus::INVALID_ARGUMENT, -1, -1);

    std::memset(base, 0, static_cast<size_t>(layout.total_size));

    auto *control = region_at<PercounterControl>(base, layout.control_offset);
    auto *fanin = region_at<PercounterFanin>(base, layout.fanin_offset);
    auto *fanin_addr = region_at<uint64_t>(base, layout.fanin_addr_offset);
    auto *counters = region_at<PercounterCounter>(base, layout.counters_offset);
    int32_t *orders[PERCOUNTER_CORE_TYPE_COUNT] = {
        region_at<int32_t>(base, layout.orders_offset[0]),
        region_at<int32_t>(base, layout.orders_offset[1]),
    };

    uint64_t placed[PERCOUNTER_CORE_TYPE_COUNT] = {0, 0};
    uint64_t edge_cursor = 0;

    for (uint64_t id = 0; id < task_count; ++id) {
        const TaskInput &task = tasks[id];
        if (task.core_type_index >= PERCOUNTER_CORE_TYPE_COUNT)
            return fail(BuildStatus::BAD_CORE_TYPE, static_cast<int64_t>(id), -1);
        if (task.fanin_count < 0 || (task.fanin_count > 0 && task.fanin_ids == nullptr))
            return fail(BuildStatus::INVALID_ARGUMENT, static_cast<int64_t>(id), -1);

        fanin[id].count = task.fanin_count;
        fanin[id].begin = static_cast<int32_t>(edge_cursor);
        for (int32_t e = 0; e < task.fanin_count; ++e) {
            if (edge_cursor >= layout.edge_count)
                return fail(BuildStatus::EDGE_COUNT_MISMATCH, static_cast<int64_t>(id), e);
            const int32_t producer = task.fanin_ids[e];
            if (producer < 0 || static_cast<uint64_t>(producer) >= task_count)
                return fail(BuildStatus::PRODUCER_OUT_OF_RANGE, static_cast<int64_t>(id), e);
            if (static_cast<uint64_t>(producer) >= id)
                return fail(BuildStatus::PRODUCER_NOT_BEFORE_CONSUMER, static_cast<int64_t>(id), e);
            fanin_addr[edge_cursor] =
                device_base + layout.counters_offset + static_cast<uint64_t>(producer) * sizeof(PercounterCounter);
            ++edge_cursor;
        }

        if (task.inline_completed) {
            // Already done before the run starts, so it is in no order list and
            // its consumers must not wait on it.
            counters[id].value = 1;
            continue;
        }
        const uint32_t type = task.core_type_index;
        if (placed[type] >= layout.order_count[type])
            return fail(BuildStatus::ORDER_COUNT_MISMATCH, static_cast<int64_t>(id), -1);
        // Appending in id order is what keeps each list ascending, which is what
        // makes any strided slice of it ascending, which is what makes the
        // device's per-cluster claim deadlock-free.
        orders[type][placed[type]] = static_cast<int32_t>(id);
        ++placed[type];
    }

    for (uint32_t t = 0; t < PERCOUNTER_CORE_TYPE_COUNT; ++t) {
        if (placed[t] != layout.order_count[t]) return fail(BuildStatus::ORDER_COUNT_MISMATCH, -1, -1);
    }
    if (edge_cursor != layout.edge_count) return fail(BuildStatus::EDGE_COUNT_MISMATCH, -1, -1);

    control->task_count = task_count;
    control->edge_count = layout.edge_count;
    control->tickets_offset = layout.tickets_offset;
    control->fanin_offset = layout.fanin_offset;
    control->fanin_addr_offset = layout.fanin_addr_offset;
    control->counters_offset = layout.counters_offset;
    for (uint32_t t = 0; t < PERCOUNTER_CORE_TYPE_COUNT; ++t) {
        control->order_count[t] = layout.order_count[t];
        control->orders_offset[t] = layout.orders_offset[t];
    }
    return true;
}

}  // namespace simpler::hbg::percounter
