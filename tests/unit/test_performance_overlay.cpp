// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_performance_overlay.cpp
 * @brief XML snapshot test: PerformanceOverlay rebuilds MCU rows when
 *        perf_mcu_names changes.
 *
 * Creates the real performance_overlay XML component, drives perf_mcu_names
 * through PerformanceState::push_sample_for_testing, drains the UpdateQueue,
 * and asserts the mcu_card child count matches the MCU count in the sample.
 */

#include "helix_sparkline.h"
#include "performance_state.h"
#include "ui_overlay_performance.h"
#include "ui_update_queue.h"

#include "../catch_amalgamated.hpp"
#include "../test_fixtures.h"
#include "../test_helpers/update_queue_test_access.h"

using helix::perf::McuStat;
using helix::perf::PerfSample;
using helix::perf::PerformanceState;
using helix::ui::UpdateQueue;
using helix::ui::UpdateQueueTestAccess;
using helix::ui::UiOverlayPerformance;

// ============================================================================
// Fixture
// ============================================================================

namespace {

/**
 * @brief XMLTestFixture subclass that additionally:
 *   - registers helix_sparkline custom widget (needed by perf_metric_row)
 *   - registers overlay_panel, header_bar, components/perf_metric_row,
 *     and performance_overlay XML components
 *   - inits + deinits PerformanceState subjects
 *   - resets UiOverlayPerformance singleton before/after each test
 */
class PerfOverlayFixture : public XMLTestFixture {
  public:
    PerfOverlayFixture() : XMLTestFixture() {
        // helix_sparkline is a native C++ widget, not an XML file.
        // register_xml_components() (called from application.cpp) handles this
        // in production; tests must do it manually.
        if (!s_perf_xml_registered) {
            helix::ui::register_helix_sparkline_widget();

            // overlay_panel depends on header_bar; register in dependency order.
            register_component("header_bar");
            register_component("overlay_panel");
            register_component("components/perf_metric_row");
            register_component("performance_overlay");

            s_perf_xml_registered = true;
        }

        PerformanceState::instance().init_subjects();

        // Guarantee a clean singleton regardless of prior test order.
        UiOverlayPerformance::instance().reset_for_testing();
    }

    ~PerfOverlayFixture() override {
        // Teardown order matters:
        // 1. Delete the overlay widget (removes all LVGL observers on perf subjects).
        // 2. Reset the singleton (clears cached root/card pointers + mcu_names_observer_).
        // 3. Deinit perf subjects (safe — no observers remain).
        //
        // XMLTestFixture::~XMLTestFixture() runs AFTER this destructor and will
        // delete the test screen. By the time it does so, the overlay widget is
        // already gone (step 1), so the screen deletion is clean.
        lv_obj_t* root = UiOverlayPerformance::instance().root();
        if (root && lv_obj_is_valid(root)) {
            lv_obj_delete(root);
        }
        UiOverlayPerformance::instance().reset_for_testing();

        PerformanceState::instance().deinit_subjects();
    }

    // Non-copyable
    PerfOverlayFixture(const PerfOverlayFixture&)            = delete;
    PerfOverlayFixture& operator=(const PerfOverlayFixture&) = delete;

  private:
    // Performance XML components only need to be registered once per process.
    static bool s_perf_xml_registered;
};

bool PerfOverlayFixture::s_perf_xml_registered = false;

} // namespace

// ============================================================================
// Tests
// ============================================================================

TEST_CASE_METHOD(PerfOverlayFixture,
                 "PerformanceOverlay renders MCU rows dynamically",
                 "[performance][xml]") {

    // --- initial state: 1 MCU ---
    {
        PerfSample s;
        s.host_cpu_pct = 30.0f;
        McuStat a;
        a.name = "mcu";
        a.load = 0.10f;
        s.mcus = {a};
        PerformanceState::instance().push_sample_for_testing(s);
    }
    // push_sample_for_testing calls apply_sample directly; the observe_string
    // callback for perf_mcu_names defers rebuild_mcu_rows via queue_update.
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    // create() registers the observer on perf_mcu_names and will immediately
    // find mcu_card (initially empty, before the next drain). Because
    // observe_string is non-immediate, the first rebuild fires on the next drain.
    auto* root = UiOverlayPerformance::instance().create(lv_screen_active());
    REQUIRE(root != nullptr);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);

    // Drain: fires the initial observe_string callback triggered by attaching
    // the observer (LVGL calls the cb once on subscription).
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    auto* card = lv_obj_find_by_name(root, "mcu_card");
    REQUIRE(card != nullptr);
    REQUIRE(lv_obj_get_child_count(card) == 1);

    // --- second sample: 3 MCUs -> overlay must rebuild to 3 rows ---
    {
        PerfSample s;
        s.host_cpu_pct = 30.0f;
        McuStat a;
        a.name = "mcu";
        a.load = 0.10f;
        McuStat b;
        b.name = "mcu sb";
        b.load = 0.20f;
        McuStat c;
        c.name = "mcu helper";
        c.load = 0.30f;
        s.mcus = {a, b, c};
        PerformanceState::instance().push_sample_for_testing(s);
    }
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    REQUIRE(lv_obj_get_child_count(card) == 3);
}
