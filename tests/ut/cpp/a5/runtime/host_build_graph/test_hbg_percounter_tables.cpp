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

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "percounter_plan.h"
#include "scheduler/scheduler_layout.h"

namespace {

using simpler::hbg::percounter::ModeRequest;

// --- mode request --------------------------------------------------------

TEST(PercounterModeRequest, AcceptsEveryValidSpelling) {
    ModeRequest request = ModeRequest::PERCOUNTER;
    EXPECT_TRUE(simpler::hbg::percounter::parse_mode_request("auto", &request));
    EXPECT_EQ(request, ModeRequest::AUTO);
    EXPECT_TRUE(simpler::hbg::percounter::parse_mode_request("legacy", &request));
    EXPECT_EQ(request, ModeRequest::LEGACY);
    EXPECT_TRUE(simpler::hbg::percounter::parse_mode_request("resident", &request));
    EXPECT_EQ(request, ModeRequest::RESIDENT);
    EXPECT_TRUE(simpler::hbg::percounter::parse_mode_request("percounter", &request));
    EXPECT_EQ(request, ModeRequest::PERCOUNTER);
}

TEST(PercounterModeRequest, UnsetMeansAuto) {
    ModeRequest request = ModeRequest::PERCOUNTER;
    EXPECT_TRUE(simpler::hbg::percounter::parse_mode_request(nullptr, &request));
    EXPECT_EQ(request, ModeRequest::AUTO);
}

// The point of the whole exercise: a misspelled knob must fail where it is set,
// not silently select something else and surface later as a performance
// mystery. See .claude/rules/env-macro-gating.md.
TEST(PercounterModeRequest, RejectsAnythingUnrecognised) {
    ModeRequest request = ModeRequest::AUTO;
    EXPECT_FALSE(simpler::hbg::percounter::parse_mode_request("percounters", &request));
    EXPECT_FALSE(simpler::hbg::percounter::parse_mode_request("Percounter", &request));
    EXPECT_FALSE(simpler::hbg::percounter::parse_mode_request("per counter", &request));
    EXPECT_FALSE(simpler::hbg::percounter::parse_mode_request("", &request));
    EXPECT_FALSE(simpler::hbg::percounter::parse_mode_request("1", &request));
    EXPECT_FALSE(simpler::hbg::percounter::parse_mode_request("residentx", &request));
}

TEST(PercounterModeRequest, RejectsNullOut) {
    EXPECT_FALSE(simpler::hbg::percounter::parse_mode_request("auto", nullptr));
}

TEST(PercounterModeRequest, OnlyAutoIsUnforced) {
    EXPECT_FALSE(simpler::hbg::percounter::mode_request_is_forced(ModeRequest::AUTO));
    EXPECT_TRUE(simpler::hbg::percounter::mode_request_is_forced(ModeRequest::LEGACY));
    EXPECT_TRUE(simpler::hbg::percounter::mode_request_is_forced(ModeRequest::RESIDENT));
    EXPECT_TRUE(simpler::hbg::percounter::mode_request_is_forced(ModeRequest::PERCOUNTER));
}

TEST(PercounterModeRequest, NamesRoundTrip) {
    for (const ModeRequest request :
         {ModeRequest::AUTO, ModeRequest::LEGACY, ModeRequest::RESIDENT, ModeRequest::PERCOUNTER}) {
        ModeRequest parsed = ModeRequest::AUTO;
        const char *name = simpler::hbg::percounter::mode_request_name(request);
        ASSERT_TRUE(simpler::hbg::percounter::parse_mode_request(name, &parsed)) << name;
        EXPECT_EQ(parsed, request) << name;
    }
}

// Restores whatever the environment held, so the case cannot leak into another
// one through a variable the whole process shares.
class ScopedSchedulerEnv {
public:
    explicit ScopedSchedulerEnv(const char *value)
    {
        const char *previous = std::getenv(simpler::hbg::percounter::SIMPLER_HBG_SCHEDULER_ENV);
        had_previous_ = previous != nullptr;
        if (had_previous_) previous_ = previous;
        apply(value);
    }
    ~ScopedSchedulerEnv() { apply(had_previous_ ? previous_.c_str() : nullptr); }

    ScopedSchedulerEnv(const ScopedSchedulerEnv &) = delete;
    ScopedSchedulerEnv &operator=(const ScopedSchedulerEnv &) = delete;

private:
    static void apply(const char *value)
    {
        if (value == nullptr) {
            ::unsetenv(simpler::hbg::percounter::SIMPLER_HBG_SCHEDULER_ENV);
        } else {
            ::setenv(simpler::hbg::percounter::SIMPLER_HBG_SCHEDULER_ENV, value, 1);
        }
    }

    bool had_previous_ = false;
    std::string previous_;
};

TEST(PercounterModeRequest, ResolveReadsTheEnvironment) {
    ModeRequest request = ModeRequest::AUTO;
    {
        ScopedSchedulerEnv env("percounter");
        ASSERT_TRUE(simpler::hbg::percounter::resolve_mode_request(&request));
        EXPECT_EQ(request, ModeRequest::PERCOUNTER);
    }
    {
        ScopedSchedulerEnv env(nullptr);
        ASSERT_TRUE(simpler::hbg::percounter::resolve_mode_request(&request));
        EXPECT_EQ(request, ModeRequest::AUTO);
    }
    {
        ScopedSchedulerEnv env("nonsense");
        EXPECT_FALSE(simpler::hbg::percounter::resolve_mode_request(&request));
    }
}

// An exported-but-empty variable is how a shell spells "I did not set this".
TEST(PercounterModeRequest, EmptyEnvironmentValueMeansUnset) {
    ModeRequest request = ModeRequest::PERCOUNTER;
    ScopedSchedulerEnv env("");
    ASSERT_TRUE(simpler::hbg::percounter::resolve_mode_request(&request));
    EXPECT_EQ(request, ModeRequest::AUTO);
}

// --- mode constants ------------------------------------------------------

// 0 is reserved for "nothing selected", and every published mode has to be
// distinct or a terminal record cannot say which scheduler ran.
TEST(PercounterModeConstants, AreDistinctAndNonZero) {
    const uint32_t modes[] = {
        SCHEDULER_RUNTIME_MODE_RESIDENT_PENDING,
        SCHEDULER_RUNTIME_MODE_RESIDENT_READY,
        SCHEDULER_RUNTIME_MODE_LEGACY_GRAPH,
        SCHEDULER_RUNTIME_MODE_LEGACY_UNSUPPORTED_SHAPE,
        SCHEDULER_RUNTIME_MODE_PERCOUNTER,
        SCHEDULER_RUNTIME_MODE_LEGACY_REQUESTED,
    };
    for (size_t i = 0; i < std::size(modes); ++i) {
        EXPECT_NE(modes[i], 0u) << "index " << i;
        for (size_t j = i + 1; j < std::size(modes); ++j) {
            EXPECT_NE(modes[i], modes[j]) << "indices " << i << " and " << j;
        }
    }
}

// --- layout --------------------------------------------------------------

namespace pc = simpler::hbg::percounter;

namespace {

// Aligned like the real region, which acquire_scheduler_state_storage hands out
// at SCHEDULER_STATE_ALIGNMENT.
class Region {
public:
    explicit Region(uint64_t size) : bytes_(size + SCHEDULER_STATE_ALIGNMENT)
    {
        storage_.resize(static_cast<size_t>(bytes_), 0);
        auto raw = reinterpret_cast<uintptr_t>(storage_.data());
        auto aligned = (raw + SCHEDULER_STATE_ALIGNMENT - 1) & ~(uintptr_t{SCHEDULER_STATE_ALIGNMENT} - 1);
        base_ = reinterpret_cast<void *>(aligned);
    }
    void *base() const { return base_; }

private:
    uint64_t bytes_;
    std::vector<uint8_t> storage_;
    void *base_ = nullptr;
};

bool is_aligned(uint64_t offset, uint64_t alignment) { return (offset & (alignment - 1)) == 0; }

}  // namespace

TEST(PercounterLayout, SegmentsAreAlignedAndOrdered) {
    const uint64_t order_count[2] = {3, 5};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(/*task_count=*/8, /*edge_count=*/11, order_count, &layout));

    EXPECT_EQ(layout.task_count, 8u);
    EXPECT_EQ(layout.edge_count, 11u);
    EXPECT_TRUE(is_aligned(layout.control_offset, alignof(pc::PercounterControl)));
    EXPECT_TRUE(is_aligned(layout.tickets_offset, alignof(pc::PercounterTicket)));
    EXPECT_TRUE(is_aligned(layout.counters_offset, alignof(pc::PercounterCounter)));
    EXPECT_TRUE(is_aligned(layout.total_size, SCHEDULER_STATE_ALIGNMENT));

    // Every segment must fit before the next one starts.
    EXPECT_GE(layout.tickets_offset, layout.control_offset + sizeof(pc::PercounterControl));
    EXPECT_GE(layout.counters_offset + 8 * sizeof(pc::PercounterCounter), layout.counters_offset);
    EXPECT_LE(layout.counters_offset + 8 * sizeof(pc::PercounterCounter), layout.total_size);
}

// The measured reason the whole array is padded: the atomic unit serialises per
// cache line, so two counters in one line cost double. A change that packs them
// stays correct and quietly runs 15x slower, which is exactly the kind of
// regression a test has to catch.
TEST(PercounterLayout, CountersAreOnePerCacheLine) {
    const uint64_t order_count[2] = {2, 2};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(4, 0, order_count, &layout));
    EXPECT_EQ(sizeof(pc::PercounterCounter), 64u);
    EXPECT_EQ(layout.counters_offset % 64u, 0u);
}

TEST(PercounterLayout, RejectsOrderCountsLargerThanTheGraph) {
    pc::PercounterLayout layout{};
    const uint64_t too_many[2] = {3, 3};  // 6 placements for 4 tasks
    EXPECT_FALSE(pc::plan_layout(4, 0, too_many, &layout));
    const uint64_t one_too_many[2] = {5, 0};
    EXPECT_FALSE(pc::plan_layout(4, 0, one_too_many, &layout));
}

TEST(PercounterLayout, RejectsNullOut) {
    const uint64_t order_count[2] = {0, 0};
    EXPECT_FALSE(pc::plan_layout(0, 0, order_count, nullptr));
}

TEST(PercounterLayout, HandlesAnEmptyGraph) {
    const uint64_t order_count[2] = {0, 0};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(0, 0, order_count, &layout));
    EXPECT_GT(layout.total_size, 0u);  // the control header still exists
}

// --- table build ---------------------------------------------------------

namespace {

constexpr uint64_t kDeviceBase = 0x7000'0000'0000ull;

// A chain 0 -> 1 -> 2 -> 3, alternating core type, with task 0 already done.
struct ChainGraph {
    std::vector<std::vector<int32_t>> fanin{{}, {0}, {1}, {2}};
    std::vector<pc::TaskInput> tasks;

    ChainGraph()
    {
        for (size_t i = 0; i < fanin.size(); ++i) {
            tasks.push_back(pc::TaskInput{
                static_cast<uint8_t>(i % 2), /*inline_completed=*/i == 0,
                static_cast<int32_t>(fanin[i].size()), fanin[i].empty() ? nullptr : fanin[i].data()});
        }
    }
    uint64_t edges() const { return 3; }
};

}  // namespace

TEST(PercounterTables, PlacesEachTaskInItsCoreTypeListInAscendingOrder) {
    ChainGraph graph;
    // task 0 is inline-completed, so only 1,2,3 are placed: AIV gets 1 and 3,
    // AIC gets 2.
    const uint64_t order_count[2] = {1, 2};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(4, graph.edges(), order_count, &layout));
    Region region(layout.total_size);

    pc::BuildResult result{};
    ASSERT_TRUE(pc::build_tables(graph.tasks.data(), 4, layout, region.base(), kDeviceBase, &result))
        << pc::build_status_name(result.status);

    const auto *aic = pc::region_at<int32_t>(region.base(), layout.orders_offset[0]);
    const auto *aiv = pc::region_at<int32_t>(region.base(), layout.orders_offset[1]);
    EXPECT_EQ(aic[0], 2);
    EXPECT_EQ(aiv[0], 1);
    EXPECT_EQ(aiv[1], 3);
    // Ascending is the property the device's strided claim depends on.
    EXPECT_LT(aiv[0], aiv[1]);
}

TEST(PercounterTables, FaninAddressesPointAtTheProducersCounter) {
    ChainGraph graph;
    const uint64_t order_count[2] = {1, 2};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(4, graph.edges(), order_count, &layout));
    Region region(layout.total_size);
    ASSERT_TRUE(pc::build_tables(graph.tasks.data(), 4, layout, region.base(), kDeviceBase));

    const auto *fanin = pc::region_at<pc::PercounterFanin>(region.base(), layout.fanin_offset);
    const auto *addr = pc::region_at<uint64_t>(region.base(), layout.fanin_addr_offset);

    EXPECT_EQ(fanin[0].count, 0);
    for (int32_t consumer = 1; consumer <= 3; ++consumer) {
        ASSERT_EQ(fanin[consumer].count, 1);
        const uint64_t want =
            kDeviceBase + layout.counters_offset + static_cast<uint64_t>(consumer - 1) * sizeof(pc::PercounterCounter);
        EXPECT_EQ(addr[fanin[consumer].begin], want) << "consumer " << consumer;
    }
}

TEST(PercounterTables, PresetsInlineCompletedCountersAndLeavesTheRestAtZero) {
    ChainGraph graph;
    const uint64_t order_count[2] = {1, 2};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(4, graph.edges(), order_count, &layout));
    Region region(layout.total_size);
    ASSERT_TRUE(pc::build_tables(graph.tasks.data(), 4, layout, region.base(), kDeviceBase));

    const auto *counters = pc::region_at<pc::PercounterCounter>(region.base(), layout.counters_offset);
    EXPECT_EQ(counters[0].value, 1u) << "an inline-completed task must not be waited on";
    EXPECT_EQ(counters[1].value, 0u);
    EXPECT_EQ(counters[2].value, 0u);
    EXPECT_EQ(counters[3].value, 0u);
}

TEST(PercounterTables, ControlHeaderMirrorsThePlan) {
    ChainGraph graph;
    const uint64_t order_count[2] = {1, 2};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(4, graph.edges(), order_count, &layout));
    Region region(layout.total_size);
    ASSERT_TRUE(pc::build_tables(graph.tasks.data(), 4, layout, region.base(), kDeviceBase));

    const auto *control = pc::region_at<pc::PercounterControl>(region.base(), layout.control_offset);
    EXPECT_EQ(control->task_count, 4u);
    EXPECT_EQ(control->edge_count, graph.edges());
    EXPECT_EQ(control->order_count[0], 1u);
    EXPECT_EQ(control->order_count[1], 2u);
    EXPECT_EQ(control->counters_offset, layout.counters_offset);
    EXPECT_EQ(control->tickets_offset, layout.tickets_offset);
}

// The load-bearing assumption of the whole design: ascending task id is already
// a topological order, so a producer's id is always lower than its consumer's.
// If that ever stops holding, a lane spins forever on a counter nobody will set
// -- a device hang with no diagnosis. Checking it per edge turns that into a
// bind failure.
TEST(PercounterTables, RejectsAnEdgeThatDoesNotPointBackwards) {
    std::vector<int32_t> forward{2};  // task 1 waiting on task 2
    std::vector<int32_t> none{};
    const pc::TaskInput tasks[3] = {
        {0, false, 0, nullptr},
        {0, false, 1, forward.data()},
        {0, false, 0, nullptr},
    };
    const uint64_t order_count[2] = {3, 0};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(3, 1, order_count, &layout));
    Region region(layout.total_size);

    pc::BuildResult result{};
    EXPECT_FALSE(pc::build_tables(tasks, 3, layout, region.base(), kDeviceBase, &result));
    EXPECT_EQ(result.status, pc::BuildStatus::PRODUCER_NOT_BEFORE_CONSUMER);
    EXPECT_EQ(result.task_id, 1);
    EXPECT_EQ(result.fanin_index, 0);
}

TEST(PercounterTables, RejectsSelfEdges) {
    std::vector<int32_t> self{0};
    const pc::TaskInput tasks[1] = {{0, false, 1, self.data()}};
    const uint64_t order_count[2] = {1, 0};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(1, 1, order_count, &layout));
    Region region(layout.total_size);

    pc::BuildResult result{};
    EXPECT_FALSE(pc::build_tables(tasks, 1, layout, region.base(), kDeviceBase, &result));
    EXPECT_EQ(result.status, pc::BuildStatus::PRODUCER_NOT_BEFORE_CONSUMER);
}

TEST(PercounterTables, RejectsAProducerOutsideTheGraph) {
    std::vector<int32_t> bad{7};
    const pc::TaskInput tasks[2] = {{0, false, 0, nullptr}, {0, false, 1, bad.data()}};
    const uint64_t order_count[2] = {2, 0};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(2, 1, order_count, &layout));
    Region region(layout.total_size);

    pc::BuildResult result{};
    EXPECT_FALSE(pc::build_tables(tasks, 2, layout, region.base(), kDeviceBase, &result));
    EXPECT_EQ(result.status, pc::BuildStatus::PRODUCER_OUT_OF_RANGE);
}

TEST(PercounterTables, RejectsABadCoreType) {
    const pc::TaskInput tasks[1] = {{9, false, 0, nullptr}};
    const uint64_t order_count[2] = {1, 0};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(1, 0, order_count, &layout));
    Region region(layout.total_size);

    pc::BuildResult result{};
    EXPECT_FALSE(pc::build_tables(tasks, 1, layout, region.base(), kDeviceBase, &result));
    EXPECT_EQ(result.status, pc::BuildStatus::BAD_CORE_TYPE);
}

// A plan that disagrees with the graph must not half-fill the region: the
// device would read a list whose tail is still zeroed and dispatch task 0
// repeatedly.
TEST(PercounterTables, RejectsAPlanThatDisagreesWithTheTasks) {
    const pc::TaskInput tasks[2] = {{0, false, 0, nullptr}, {0, false, 0, nullptr}};
    const uint64_t too_few[2] = {1, 0};  // two AIC tasks, room for one
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(2, 0, too_few, &layout));
    Region region(layout.total_size);

    pc::BuildResult result{};
    EXPECT_FALSE(pc::build_tables(tasks, 2, layout, region.base(), kDeviceBase, &result));
    EXPECT_EQ(result.status, pc::BuildStatus::ORDER_COUNT_MISMATCH);

    // A plan too LARGE passes plan_layout (it only bounds the total against the
    // graph) and has to be caught by the tail check, after the walk has placed
    // fewer tasks than promised. An inline-completed task is placed in no list,
    // which is the easy way to get there.
    const pc::TaskInput with_inline[2] = {{0, true, 0, nullptr}, {0, false, 0, nullptr}};
    const uint64_t too_many[2] = {2, 0};  // one placeable AIC task, room for two
    ASSERT_TRUE(pc::plan_layout(2, 0, too_many, &layout));
    Region region2(layout.total_size);
    EXPECT_FALSE(pc::build_tables(with_inline, 2, layout, region2.base(), kDeviceBase, &result));
    EXPECT_EQ(result.status, pc::BuildStatus::ORDER_COUNT_MISMATCH);
}

// plan_layout owns the "cannot possibly fit" check; build_tables owns the
// "does not actually match" one. Keeping them separate is why the case above
// has to construct its mismatch so carefully.
TEST(PercounterLayout, BoundsTheTotalPlacementNotEachList) {
    pc::PercounterLayout layout{};
    const uint64_t split[2] = {2, 2};  // 4 placements, 4 tasks: fine
    EXPECT_TRUE(pc::plan_layout(4, 0, split, &layout));
    const uint64_t over[2] = {3, 2};  // 5 placements, 4 tasks: impossible
    EXPECT_FALSE(pc::plan_layout(4, 0, over, &layout));
}

TEST(PercounterTables, RejectsAnEdgeCountThatDisagrees) {
    std::vector<int32_t> one{0};
    const pc::TaskInput tasks[2] = {{0, false, 0, nullptr}, {0, false, 1, one.data()}};
    const uint64_t order_count[2] = {2, 0};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(2, /*edge_count=*/0, order_count, &layout));
    Region region(layout.total_size);

    pc::BuildResult result{};
    EXPECT_FALSE(pc::build_tables(tasks, 2, layout, region.base(), kDeviceBase, &result));
    EXPECT_EQ(result.status, pc::BuildStatus::EDGE_COUNT_MISMATCH);
}

TEST(PercounterTables, BuildsAnEmptyGraph) {
    const uint64_t order_count[2] = {0, 0};
    pc::PercounterLayout layout{};
    ASSERT_TRUE(pc::plan_layout(0, 0, order_count, &layout));
    Region region(layout.total_size);
    pc::BuildResult result{};
    EXPECT_TRUE(pc::build_tables(nullptr, 0, layout, region.base(), kDeviceBase, &result))
        << pc::build_status_name(result.status);
}

}  // namespace
