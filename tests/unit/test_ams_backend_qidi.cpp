// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_qidi.h"
#include "ams_types.h"

#include "../catch_amalgamated.hpp"

#include "hv/json.hpp"

using json = nlohmann::json;

// Friend-class shim per L065 — exposes private parse helpers for unit tests.
// Mirrors the Ad5xIfsTestAccess pattern in test_ams_backend_ad5x_ifs.cpp.
class QidiBoxTestAccess {
  public:
    static void parse_vars(AmsBackendQidi& b, const json& v) {
        b.parse_save_variables(v);
    }
    static void handle_status(AmsBackendQidi& b, const json& n) {
        b.handle_status_update(n);
    }
};

// Build a Moonraker-shaped status notification carrying save_variables.
static json make_save_variables_notification(const json& variables) {
    return json{{"save_variables", json{{"variables", variables}}}};
}

// =====================================================================
// Type identification — pin down what the stub already advertises so
// later refactors don't silently change it.
// =====================================================================

TEST_CASE("QIDI Box type identification", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    REQUIRE(backend.get_type() == AmsType::QIDI_BOX);
    REQUIRE(backend.get_topology() == PathTopology::HUB);
}

TEST_CASE("QIDI Box default system_info shape", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    auto info = backend.get_system_info();

    REQUIRE(info.type == AmsType::QIDI_BOX);
    REQUIRE(info.total_slots == 4);
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.units[0].topology == PathTopology::HUB);
    // Unit must report as disconnected until enable_box=1 arrives.
    REQUIRE_FALSE(info.units[0].connected);
}

// =====================================================================
// parse_save_variables: enable_box gate
// =====================================================================
// `box_extras.py` reads `save_variables.variables.enable_box` and treats
// 0 as "Box installed but disabled" / 1 as "Box active." Mirror that
// onto AmsUnit::connected so the UI can show the right state.

TEST_CASE("QIDI Box parse_save_variables: enable_box=1 connects the unit",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE_FALSE(backend.get_system_info().units[0].connected);

    QidiBoxTestAccess::parse_vars(backend, json{{"enable_box", 1}});

    REQUIRE(backend.get_system_info().units[0].connected);
}

// =====================================================================
// parse_save_variables: box_count resizes the system
// =====================================================================
// `box_detect.py` writes save_variables.variables.box_count whenever USB
// enumeration changes. Each physical box = 4 slots, chainable up to 4
// boxes / 16 slots. The backend must resize the unit's slot vector to
// match so the UI shows the right slot count.

TEST_CASE("QIDI Box parse_save_variables: box_count=2 expands to 8 slots",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE(backend.get_system_info().total_slots == 4);

    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});

    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 8);
    REQUIRE(info.units[0].slot_count == 8);
    REQUIRE(info.units[0].slots.size() == 8);

    // Newly-added slots should be sensibly initialized.
    for (size_t i = 0; i < info.units[0].slots.size(); ++i) {
        REQUIRE(info.units[0].slots[i].slot_index == static_cast<int>(i));
        REQUIRE(info.units[0].slots[i].global_index == static_cast<int>(i));
    }
}

// =====================================================================
// parse_save_variables: per-slot state from slot<N> values
// =====================================================================
// box_stepper.py writes save_variables.variables.slot<N> as the slot's
// state machine cursor. From box_stepper.py LED-state mapping:
//   0   = empty / no filament
//   1   = filament loaded in box, retracted (available)
//   2   = filament loaded all the way to extruder
//   3   = mid-transition (loading/unloading in progress)
//   -1  = slot load failed
//   -2  = extruder load failed
//   -3  = runout-during-print detected by motion sensor

TEST_CASE("QIDI Box per-slot positive states map to SlotStatus",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"slot0", 0}, // empty
                                               {"slot1", 1}, // available (parked in box)
                                               {"slot2", 2}, // loaded to extruder
                                               {"slot3", 3}, // mid-transition
                                           });

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[0].status == SlotStatus::EMPTY);
    REQUIRE(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
    REQUIRE(info.units[0].slots[2].status == SlotStatus::LOADED);
    // Mid-transition: show as AVAILABLE so UI doesn't flicker — the
    // foreground action belongs on system_info_.action, not slot status.
    REQUIRE(info.units[0].slots[3].status == SlotStatus::AVAILABLE);
}

TEST_CASE("QIDI Box per-slot negative states map to BLOCKED",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    SECTION("-1 = slot load failed") {
        QidiBoxTestAccess::parse_vars(backend, json{{"slot0", -1}});
        REQUIRE(backend.get_system_info().units[0].slots[0].status ==
                SlotStatus::BLOCKED);
    }
    SECTION("-2 = extruder load failed") {
        QidiBoxTestAccess::parse_vars(backend, json{{"slot0", -2}});
        REQUIRE(backend.get_system_info().units[0].slots[0].status ==
                SlotStatus::BLOCKED);
    }
    SECTION("-3 = runout-during-print") {
        QidiBoxTestAccess::parse_vars(backend, json{{"slot0", -3}});
        REQUIRE(backend.get_system_info().units[0].slots[0].status ==
                SlotStatus::BLOCKED);
    }
}

// =====================================================================
// parse_save_variables: value_t<N> tool->slot mapping
// =====================================================================
// box_extras.py stores tool mappings as save_variables.variables.value_t<N>
// with value "slot<M>". This means "tool N prints from slot M." Default
// (when value_t<N> is missing) is tool N = slot N, which the resize
// code already establishes.

TEST_CASE("QIDI Box parse_save_variables: value_t<N>=slot<M> maps tool N to slot M",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"value_t0", "slot2"},
                                               {"value_t1", "slot3"},
                                           });

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[2].mapped_tool == 0);
    REQUIRE(info.units[0].slots[3].mapped_tool == 1);
}

// =====================================================================
// handle_status_update routes save_variables changes through to parse
// =====================================================================
// Moonraker delivers save_variables changes inside notify_status_update as
// `{"save_variables": {"variables": {...}}}`. The backend must extract the
// inner variables payload and feed it to parse_save_variables so live
// updates flow through.

TEST_CASE("QIDI Box handle_status_update applies save_variables changes",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE_FALSE(backend.get_system_info().units[0].connected);

    QidiBoxTestAccess::handle_status(
        backend, make_save_variables_notification(json{
                     {"enable_box", 1},
                     {"box_count", 2},
                 }));

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].connected);
    REQUIRE(info.total_slots == 8);
}

TEST_CASE("QIDI Box handle_status_update ignores unrelated keys",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    // Notification without save_variables shouldn't touch state.
    QidiBoxTestAccess::handle_status(
        backend, json{{"toolhead", {{"position", json::array({0, 0, 0, 0})}}}});

    REQUIRE_FALSE(backend.get_system_info().units[0].connected);
    REQUIRE(backend.get_system_info().total_slots == 4);
}

// =====================================================================
// last_load_slot: which slot is currently in the extruder
// =====================================================================
// box_extras.py is the source of truth for "which slot is loaded right
// now." Per-slot `slot<N>=2` is the secondary signal (and may be stale
// after error recovery). When last_load_slot is set, that slot must be
// LOADED and no other slot should claim LOADED.

TEST_CASE("QIDI Box last_load_slot promotes a single slot to LOADED",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"slot0", 1}, // available
                                               {"slot1", 1}, // available
                                               {"slot2", 1}, // available
                                               {"slot3", 1}, // available
                                               {"last_load_slot", "slot2"},
                                           });

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
    REQUIRE(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
    REQUIRE(info.units[0].slots[2].status == SlotStatus::LOADED);
    REQUIRE(info.units[0].slots[3].status == SlotStatus::AVAILABLE);
}

TEST_CASE("QIDI Box last_load_slot=slot-1 means nothing is in the extruder",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    // Seed slot2 as loaded, then explicitly clear via last_load_slot
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"slot2", 2}, // claims LOADED
                                               {"last_load_slot", "slot-1"},
                                           });

    REQUIRE(backend.get_system_info().units[0].slots[2].status ==
            SlotStatus::AVAILABLE);
}
