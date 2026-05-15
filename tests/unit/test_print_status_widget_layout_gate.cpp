// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(HelixTestFixture, "Layout gate: detailed at colspan=1 falls back",
                 "[print_status][layout_gate]") {
    PrintStatusWidget w;
    w.set_config({{"layout_style", "detailed"}});
    w.on_size_changed(1, 2, 200, 400);
    REQUIRE(lv_subject_get_int(PrintStatusWidget::layout_effective_subject_for_test()) == 0);
}

TEST_CASE_METHOD(HelixTestFixture, "Layout gate: detailed at colspan=2 activates",
                 "[print_status][layout_gate]") {
    PrintStatusWidget w;
    w.set_config({{"layout_style", "detailed"}});
    w.on_size_changed(2, 2, 400, 400);
    REQUIRE(lv_subject_get_int(PrintStatusWidget::layout_effective_subject_for_test()) == 1);
    REQUIRE(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 0);
}

TEST_CASE_METHOD(HelixTestFixture, "Layout gate: colspan>=3 reveals filament line",
                 "[print_status][layout_gate]") {
    // Filament-active gate is (colspan>=3) AND (filament_used>0). Reset the
    // shared PrinterState singleton and seed used_mm so this test doesn't
    // depend on translation-unit ordering for a non-zero leftover value.
    // Tear down the static formatter first so its observers don't reference
    // the about-to-be-freed PrinterState subjects.
    PrintStatusWidget::destroy_formatter_for_test();
    auto& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);
    lv_subject_set_int(ps.get_print_filament_used_subject(), 1500); // 1.5m

    PrintStatusWidget w;
    w.set_config({{"layout_style", "detailed"}});
    w.on_size_changed(3, 2, 600, 400);
    REQUIRE(lv_subject_get_int(PrintStatusWidget::layout_effective_subject_for_test()) == 1);
    REQUIRE(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 1);
}

TEST_CASE_METHOD(HelixTestFixture, "Layout gate: library stays library regardless",
                 "[print_status][layout_gate]") {
    PrintStatusWidget w;
    // Prime: a detailed widget at colspan=3 sets effective=1
    w.set_config({{"layout_style", "detailed"}});
    w.on_size_changed(3, 2, 600, 400);
    REQUIRE(lv_subject_get_int(PrintStatusWidget::layout_effective_subject_for_test()) == 1);
    // Switch to library — must revert to 0 regardless of colspan
    w.set_config({{"layout_style", "library"}});
    w.on_size_changed(3, 3, 600, 600);
    REQUIRE(lv_subject_get_int(PrintStatusWidget::layout_effective_subject_for_test()) == 0);
}
