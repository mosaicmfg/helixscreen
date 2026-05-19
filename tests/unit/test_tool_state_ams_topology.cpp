// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend.h"
#include "ams_state.h"
#include "lvgl_test_fixture.h"
#include "tool_state.h"
#include "ui_update_queue.h"

#include "../catch_amalgamated.hpp"

using helix::ToolState;
using helix::ToolTopology;
using helix::ui::UpdateQueue;

namespace {

// LVGLTestFixture brings up LVGL so ToolState::init_subjects() can register
// subjects (init uses lv_subject_init_*). HelixTestFixture alone does not.
struct ToolStateFixture : public LVGLTestFixture {
    ToolStateFixture() {
        ToolState::instance().init_subjects(/*register_xml=*/false);
    }
    ~ToolStateFixture() override {
        ToolState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] set_ams_topology populates 15 tools",
                 "[tool-state][ams][ams-topology]") {
    ToolTopology topo;
    topo.tool_count = 15;
    topo.active_tool = 0;
    topo.tool_to_slot.resize(15);
    for (int i = 0; i < 15; ++i)
        topo.tool_to_slot[i] = i; // 1:1 default
    topo.tool_name_prefix = "T";

    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ToolState::instance().tool_count() == 15);
    REQUIRE(ToolState::instance().active_tool_index() == 0);
    REQUIRE(ToolState::instance().tools()[14].name == "T14");
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] active tool updates without rebuilding tools list",
                 "[tool-state][ams][ams-topology]") {
    ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_name_prefix = "T";
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    int initial_version =
        lv_subject_get_int(ToolState::instance().get_tools_version_subject());

    topo.active_tool = 2;
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ToolState::instance().active_tool_index() == 2);
    REQUIRE(lv_subject_get_int(ToolState::instance().get_tools_version_subject()) ==
            initial_version); // No rebuild
    // Pin list shape + content so a future "bump version but silently rebuild"
    // regression also fails this test.
    REQUIRE(ToolState::instance().tool_count() == 4);
    REQUIRE(ToolState::instance().tools()[0].name == "T0");
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] clear_ams_topology releases override",
                 "[tool-state][ams][ams-topology]") {
    ToolTopology topo;
    topo.tool_count = 6;
    topo.active_tool = 3;
    topo.tool_to_slot = {0, 1, 2, 3, 4, 5};
    topo.tool_name_prefix = "T";
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(ToolState::instance().tool_count() == 6);

    ToolState::instance().clear_ams_topology();
    UpdateQueue::instance().drain();

    // After clear, tools_ is empty (callers must invoke init_tools again to repopulate)
    REQUIRE(ToolState::instance().tool_count() == 0);
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] AFC mock with 4 lanes drives ToolState",
                 "[tool-state][ams][afc][ams-topology]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(/*register_xml=*/false);

    auto mock = AmsBackend::create_mock(4);
    REQUIRE(mock != nullptr);
    auto caps = mock->get_tool_mapping_capabilities();
    if (!caps.supported)
        return; // Mock lacks tool multiplexing; skipped.

    ams.set_backend(std::move(mock));
    ams.sync_from_backend();
    UpdateQueue::instance().drain();

    REQUIRE(ToolState::instance().tool_count() == 4);
    REQUIRE(ToolState::instance().ams_topology_active());

    // Defensive cleanup: drop the override + backend so later tests (which share
    // the AmsState / ToolState singletons) don't inherit a stale topology.
    ToolState::instance().clear_ams_topology();
    ams.clear_backends();
    UpdateQueue::instance().drain();
}
