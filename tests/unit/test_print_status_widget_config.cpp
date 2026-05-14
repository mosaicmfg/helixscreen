// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(HelixTestFixture, "PrintStatusWidget config defaults", "[print_status][config]") {
    PrintStatusWidget w;
    nlohmann::json empty = nlohmann::json::object();
    w.set_config(empty);
    REQUIRE(w.layout_style_for_test() == "library");
    REQUIRE(w.nozzle_tool_override_for_test() == "auto");
}

TEST_CASE_METHOD(HelixTestFixture, "PrintStatusWidget config sets detailed", "[print_status][config]") {
    PrintStatusWidget w;
    nlohmann::json cfg = {{"layout_style", "detailed"}, {"nozzle_tool_override", "extruder1"}};
    w.set_config(cfg);
    REQUIRE(w.layout_style_for_test() == "detailed");
    REQUIRE(w.nozzle_tool_override_for_test() == "extruder1");
}

TEST_CASE_METHOD(HelixTestFixture, "PrintStatusWidget rejects invalid layout_style", "[print_status][config]") {
    PrintStatusWidget w;
    nlohmann::json cfg = {{"layout_style", "bogus"}};
    w.set_config(cfg);
    REQUIRE(w.layout_style_for_test() == "library"); // unchanged
}
