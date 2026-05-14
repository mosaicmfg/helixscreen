// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "app_globals.h"
#include "printer_state.h"
#include "tool_state.h"
#include "ui_observer_guard.h"  // SubjectLifetime

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

struct FormatterScope {
    FormatterScope() { PrintStatusWidget::ensure_formatter_for_test(); }
    ~FormatterScope() { PrintStatusWidget::release_formatter_for_test(); }
};

TEST_CASE_METHOD(HelixTestFixture, "Tool override: pinned reads per-tool subject",
                 "[print_status][tool_override]") {
    auto& ts = ToolState::instance();
    ts.init_subjects(false);
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    // Register extruder + extruder1 so dynamic subjects exist
    ps.init_extruders({"extruder", "extruder1"});

    lv_subject_set_int(ts.get_tool_count_subject(), 2);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    FormatterScope fs;

    // Seed per-tool and active-extruder subjects with distinct values
    SubjectLifetime lt;
    auto* e1_temp = ps.get_extruder_temp_subject("extruder1", lt);
    REQUIRE(e1_temp != nullptr);
    lv_subject_set_int(e1_temp, 25000);                                 // 250 °C
    lv_subject_set_int(ps.get_active_extruder_temp_subject(), 19000);   // 190 °C (distinct)
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    // Pin the formatter to extruder1
    PrintStatusWidget::set_nozzle_tool_override_for_test("extruder1");
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    auto* nozzle_sub = lv_xml_get_subject(nullptr, "print_status_nozzle_text");
    REQUIRE(nozzle_sub != nullptr);
    // Should read extruder1 (250°C), not active_extruder (190°C)
    REQUIRE(std::string(lv_subject_get_string(nozzle_sub)).find("250") != std::string::npos);
}

TEST_CASE_METHOD(HelixTestFixture, "Tool override: stale pin falls back to auto",
                 "[print_status][tool_override]") {
    auto& ts = ToolState::instance();
    ts.init_subjects(false);
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    lv_subject_set_int(ts.get_tool_count_subject(), 1);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    FormatterScope fs;

    // Pin to a ghost extruder that doesn't exist — should fall back to auto
    PrintStatusWidget::set_nozzle_tool_override_for_test("extruder7_ghost");
    lv_subject_set_int(ps.get_active_extruder_temp_subject(), 12345);   // 123 °C
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

    auto* nozzle_sub = lv_xml_get_subject(nullptr, "print_status_nozzle_text");
    REQUIRE(nozzle_sub != nullptr);
    // Auto fallback reads active_extruder (123°C)
    REQUIRE(std::string(lv_subject_get_string(nozzle_sub)).find("123") != std::string::npos);
}
