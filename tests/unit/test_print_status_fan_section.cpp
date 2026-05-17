// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "printer_fan_state.h"

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
