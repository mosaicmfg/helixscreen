// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "app_globals.h"
#include "printer_fan_state.h"
#include "ui_panel_print_status.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans picks first of each type",
                 "[fan_state][drift]") {
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 50, true, std::nullopt});
    fans.push_back({"heater_fan hotend_fan", "Hotend", FanType::HEATER_FAN, 80, false,
                    std::nullopt});
    fans.push_back({"fan_generic chamber", "Chamber", FanType::GENERIC_FAN, 0, true, std::nullopt});
    state.set_fans_for_test(fans);

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part == "fan");
    REQUIRE(picked.hotend == "heater_fan hotend_fan");
    REQUIRE(picked.aux == "fan_generic chamber");
}

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans returns empty when no fans of type",
                 "[fan_state][drift]") {
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 50, true, std::nullopt});
    state.set_fans_for_test(fans);

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part == "fan");
    REQUIRE(picked.hotend.empty());
    REQUIRE(picked.aux.empty());
}

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans picks first not extras",
                 "[fan_state][drift]") {
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back({"fan", "Part 1", FanType::PART_COOLING, 0, true, std::nullopt});
    fans.push_back({"fan_generic part2", "Part 2", FanType::PART_COOLING, 0, true, std::nullopt});
    state.set_fans_for_test(fans);

    REQUIRE(state.classify_primary_fans().part == "fan");
}

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans treats controller/temp/generic/output as aux",
                 "[fan_state][drift]") {
    for (FanType aux_type : {FanType::CONTROLLER_FAN, FanType::TEMPERATURE_FAN,
                              FanType::GENERIC_FAN, FanType::OUTPUT_PIN_FAN}) {
        PrinterFanState state;
        std::vector<FanInfo> fans;
        fans.push_back({"fan_x", "X", aux_type, 0, false, std::nullopt});
        state.set_fans_for_test(fans);
        REQUIRE_FALSE(state.classify_primary_fans().aux.empty());
    }
}

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans handles empty fan list",
                 "[fan_state][drift]") {
    PrinterFanState state;
    state.set_fans_for_test({});

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part.empty());
    REQUIRE(picked.hotend.empty());
    REQUIRE(picked.aux.empty());
}

// =============================================================================
// Integration tests: PrintStatusPanel fan subjects
// =============================================================================

// Fixture that owns a PrintStatusPanel with subjects initialized.
// Uses the global PrinterState singleton (same as production).
struct FanPanelFixture : public HelixTestFixture {
    FanPanelFixture() : panel(get_printer_state(), nullptr) {
        panel.init_subjects();
    }
    ~FanPanelFixture() override = default;

    PrintStatusPanel panel;
};

TEST_CASE_METHOD(HelixTestFixture,
                 "classify_primary_fans via global PrinterState fans_ live path",
                 "[print_status][fans]") {
    // Drive through the global PrinterState's fan_state member (live path).
    auto& fs = get_printer_state().get_fan_state();
    std::vector<FanInfo> fans;
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 75, true, std::nullopt});
    fans.push_back({"heater_fan hotend_fan", "Hotend", FanType::HEATER_FAN, 100, false,
                    std::nullopt});
    fans.push_back({"fan_generic exhaust", "Exhaust", FanType::GENERIC_FAN, 0, true, std::nullopt});
    fs.set_fans_for_test(fans);

    auto picked = fs.classify_primary_fans();
    REQUIRE(picked.part == "fan");
    REQUIRE(picked.hotend == "heater_fan hotend_fan");
    REQUIRE(picked.aux == "fan_generic exhaust");

    // Clear the global fan state so later tests don't see this leftover.
    fs.set_fans_for_test({});
}

TEST_CASE_METHOD(FanPanelFixture,
                 "init_subjects registers all fan section subjects",
                 "[print_status][fans]") {
    // After init_subjects() the six fan-section subjects must be findable in
    // the global XML subject registry.
    REQUIRE(lv_xml_get_subject(nullptr, "print_status_fans_fit") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_status_aux_fan_present") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_status_fan_row_density") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_status_aux_icon_visible") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_status_aux_full_visible") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_status_aux_short_visible") != nullptr);
}

TEST_CASE_METHOD(FanPanelFixture,
                 "fans_fit subject defaults to 0 (hidden) after init_subjects",
                 "[print_status][fans]") {
    // The fan row must stay hidden (0) until recompute_fans_fit fires after
    // the widget tree is attached.  A non-zero default would flash the row.
    auto* s = lv_xml_get_subject(nullptr, "print_status_fans_fit");
    REQUIRE(s != nullptr);
    REQUIRE(lv_subject_get_int(s) == 0);
}

TEST_CASE_METHOD(FanPanelFixture,
                 "recompute_aux_composites: density=0, aux present -> icon + full label visible",
                 "[print_status][fans]") {
    // density=0 (full): icon+label+val — aux_icon_visible=1, full=1, short=0
    panel.recompute_aux_composites_for_test(0, true);

    auto* icon  = lv_xml_get_subject(nullptr, "print_status_aux_icon_visible");
    auto* full  = lv_xml_get_subject(nullptr, "print_status_aux_full_visible");
    auto* shortt = lv_xml_get_subject(nullptr, "print_status_aux_short_visible");
    REQUIRE(icon  != nullptr);
    REQUIRE(full  != nullptr);
    REQUIRE(shortt != nullptr);

    REQUIRE(lv_subject_get_int(icon)   == 1);
    REQUIRE(lv_subject_get_int(full)   == 1);
    REQUIRE(lv_subject_get_int(shortt) == 0);
}

TEST_CASE_METHOD(FanPanelFixture,
                 "recompute_aux_composites: density=1, aux present -> full only (no icon)",
                 "[print_status][fans]") {
    // density=1 (medium): label+val — icon hidden, full visible, short hidden
    panel.recompute_aux_composites_for_test(1, true);

    auto* icon  = lv_xml_get_subject(nullptr, "print_status_aux_icon_visible");
    auto* full  = lv_xml_get_subject(nullptr, "print_status_aux_full_visible");
    auto* shortt = lv_xml_get_subject(nullptr, "print_status_aux_short_visible");

    REQUIRE(lv_subject_get_int(icon)   == 0);
    REQUIRE(lv_subject_get_int(full)   == 1);
    REQUIRE(lv_subject_get_int(shortt) == 0);
}

TEST_CASE_METHOD(FanPanelFixture,
                 "recompute_aux_composites: density=2, aux present -> short only",
                 "[print_status][fans]") {
    // density=2 (compact): single-letter+val — icon hidden, full hidden, short visible
    panel.recompute_aux_composites_for_test(2, true);

    auto* icon  = lv_xml_get_subject(nullptr, "print_status_aux_icon_visible");
    auto* full  = lv_xml_get_subject(nullptr, "print_status_aux_full_visible");
    auto* shortt = lv_xml_get_subject(nullptr, "print_status_aux_short_visible");

    REQUIRE(lv_subject_get_int(icon)   == 0);
    REQUIRE(lv_subject_get_int(full)   == 0);
    REQUIRE(lv_subject_get_int(shortt) == 1);
}

TEST_CASE_METHOD(FanPanelFixture,
                 "recompute_aux_composites: no aux fan -> all hidden regardless of density",
                 "[print_status][fans]") {
    // When aux_present=false every composite subject must be 0.
    for (int d : {0, 1, 2}) {
        panel.recompute_aux_composites_for_test(d, false);

        auto* icon  = lv_xml_get_subject(nullptr, "print_status_aux_icon_visible");
        auto* full  = lv_xml_get_subject(nullptr, "print_status_aux_full_visible");
        auto* shortt = lv_xml_get_subject(nullptr, "print_status_aux_short_visible");

        INFO("density=" << d);
        REQUIRE(lv_subject_get_int(icon)   == 0);
        REQUIRE(lv_subject_get_int(full)   == 0);
        REQUIRE(lv_subject_get_int(shortt) == 0);
    }
}
