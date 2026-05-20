// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_preheat_cooldown_char.cpp
 * @brief Characterization tests for filament preheat / post-op cooldown logic
 *
 * Run with: ./build/bin/helix-tests "[filament][preheat][char]"
 *
 * Covers the three coupled decisions that together prevent the
 * "manual preheat killed after load" regression:
 *
 * 1. Op-handler entry snapshots `prior_nozzle_target_` from the LIVE
 *    extruder-target subject (NOT the cached `nozzle_target_` member,
 *    which `set_material()` overwrites with a preset preview).
 * 2. `start_preheat_for_op()` only issues SET_HEATER_TEMPERATURE when the
 *    live target is below the material's minimum — never lowers it.
 * 3. `restore_heater_after_preheat()` schedules `PostOpCooldownManager`
 *    iff the snapshotted prior target was 0 (heater was off before).
 *
 * The actual FilamentPanel is tightly coupled to LVGL/Moonraker, so this
 * mirrors the decisions in a lightweight state machine, same pattern as
 * test_filament_bypass_routing_char.cpp.
 */

#include "../catch_amalgamated.hpp"

namespace {

class PreheatCooldownStateMachine {
  public:
    int live_extruder_target = 0; // What Klipper currently has set
    int min_extrude_temp = 170;
    int nozzle_current = 25;

    int prior_nozzle_target = 0;
    bool preheat_set_temperature_called = false;
    int preheat_set_temperature_value = 0;
    bool cooldown_scheduled = false;

    bool is_extrusion_allowed() const {
        return nozzle_current >= min_extrude_temp;
    }

    void on_op_button_pressed() {
        prior_nozzle_target = live_extruder_target;
    }

    void start_preheat_for_op(int preheat_target) {
        if (live_extruder_target < preheat_target) {
            preheat_set_temperature_called = true;
            preheat_set_temperature_value = preheat_target;
            live_extruder_target = preheat_target;
        }
    }

    void restore_heater_after_preheat() {
        if (prior_nozzle_target == 0) {
            cooldown_scheduled = true;
        }
        prior_nozzle_target = 0;
    }
};

} // namespace

// ============================================================================
// Snapshot semantics
// ============================================================================

TEST_CASE("Op-handler entry snapshots live extruder target", "[filament][preheat][char]") {
    PreheatCooldownStateMachine sm;

    SECTION("heater off — snapshot is 0") {
        sm.live_extruder_target = 0;
        sm.on_op_button_pressed();
        REQUIRE(sm.prior_nozzle_target == 0);
    }

    SECTION("user manually pre-heated to 240 — snapshot captures it") {
        sm.live_extruder_target = 240;
        sm.on_op_button_pressed();
        REQUIRE(sm.prior_nozzle_target == 240);
    }
}

// ============================================================================
// start_preheat_for_op — "never lower the setpoint"
// ============================================================================

TEST_CASE("Preheat never lowers the heater target", "[filament][preheat][char]") {
    PreheatCooldownStateMachine sm;

    SECTION("cold heater (target=0), PLA preheat to 200 — sets target") {
        sm.live_extruder_target = 0;
        sm.start_preheat_for_op(200);
        REQUIRE(sm.preheat_set_temperature_called);
        REQUIRE(sm.preheat_set_temperature_value == 200);
        REQUIRE(sm.live_extruder_target == 200);
    }

    SECTION("user set target=240 (ABS), PLA preheat to 200 — does NOT lower") {
        sm.live_extruder_target = 240;
        sm.start_preheat_for_op(200);
        REQUIRE_FALSE(sm.preheat_set_temperature_called);
        REQUIRE(sm.live_extruder_target == 240);
    }

    SECTION("user set target=200, PLA preheat to 200 — no-op") {
        sm.live_extruder_target = 200;
        sm.start_preheat_for_op(200);
        REQUIRE_FALSE(sm.preheat_set_temperature_called);
    }

    SECTION("user set target=150, PETG preheat to 230 — raises") {
        sm.live_extruder_target = 150;
        sm.start_preheat_for_op(230);
        REQUIRE(sm.preheat_set_temperature_called);
        REQUIRE(sm.preheat_set_temperature_value == 230);
    }
}

// ============================================================================
// Full op flows — the regression scenarios
// ============================================================================

TEST_CASE("Cold-start load schedules post-op cooldown", "[filament][preheat][char]") {
    // Baseline: user with heater off clicks Load. We preheat, run the load,
    // then schedule a cooldown so the heater doesn't stay on indefinitely.
    PreheatCooldownStateMachine sm;
    sm.live_extruder_target = 0;
    sm.nozzle_current = 25;

    sm.on_op_button_pressed();
    REQUIRE(sm.prior_nozzle_target == 0);
    REQUIRE_FALSE(sm.is_extrusion_allowed());

    sm.start_preheat_for_op(200);
    REQUIRE(sm.preheat_set_temperature_called);

    // ...heater warms up, load macro runs, then on-complete:
    sm.restore_heater_after_preheat();
    REQUIRE(sm.cooldown_scheduled);
}

TEST_CASE("Manual-preheat load does NOT schedule cooldown", "[filament][preheat][char]") {
    // Regression: user pre-heated to 240°C (ABS) to clear remnants, then
    // clicks Load with PLA selected. The hot-nozzle path skips preheat and
    // jumps to execute_load. Without the snapshot at handler entry, the
    // post-op cooldown would silently turn the heater off 120s later.
    PreheatCooldownStateMachine sm;
    sm.live_extruder_target = 240;
    sm.nozzle_current = 240;

    sm.on_op_button_pressed();
    REQUIRE(sm.prior_nozzle_target == 240);
    REQUIRE(sm.is_extrusion_allowed());

    // is_extrusion_allowed is true, so preheat is skipped. Load runs directly.
    sm.restore_heater_after_preheat();
    REQUIRE_FALSE(sm.cooldown_scheduled);
}

TEST_CASE("High manual setpoint + cold nozzle — preheat doesn't lower, no cooldown",
          "[filament][preheat][char]") {
    // Edge case: user commanded target=240 but the nozzle hasn't warmed up
    // yet (live target ahead of physical temperature). Clicking Load while
    // current<min_extrude_temp enters start_preheat_for_op with a high prior.
    // The guard must skip the SET_HEATER call AND restore must not cool down.
    PreheatCooldownStateMachine sm;
    sm.live_extruder_target = 240;
    sm.nozzle_current = 30;

    sm.on_op_button_pressed();
    REQUIRE(sm.prior_nozzle_target == 240);
    REQUIRE_FALSE(sm.is_extrusion_allowed());

    sm.start_preheat_for_op(200);
    REQUIRE_FALSE(sm.preheat_set_temperature_called);
    REQUIRE(sm.live_extruder_target == 240);

    sm.restore_heater_after_preheat();
    REQUIRE_FALSE(sm.cooldown_scheduled);
}
