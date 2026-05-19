// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_start_profile.cpp
 * @brief Unit tests for PrintStartProfile JSON-driven pattern matching
 *
 * Tests the profile loading, signal format matching, regex response patterns,
 * and progress calculation. No LVGL or Moonraker required - pure logic tests.
 */

#include "print_start_profile.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Helper to load default profile (works with or without JSON file)
// ============================================================================

static std::shared_ptr<PrintStartProfile> get_default_profile() {
    return PrintStartProfile::load_default();
}

static std::shared_ptr<PrintStartProfile> get_forge_x_profile() {
    return PrintStartProfile::load("forge_x");
}

// ============================================================================
// Default Profile Loading Tests
// ============================================================================

TEST_CASE("PrintStartProfile: default profile loads successfully", "[profile][print]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    SECTION("Profile has a name") {
        REQUIRE_FALSE(profile->name().empty());
        // Could be "Generic" (from JSON) or "Generic (built-in)" (fallback)
        REQUIRE(profile->name().find("Generic") != std::string::npos);
    }

    SECTION("Profile has weighted progress mode") {
        REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::WEIGHTED);
    }

    SECTION("Default profile has no signal formats") {
        REQUIRE_FALSE(profile->has_signal_formats());
    }
}

// ============================================================================
// Forge-X Profile Loading Tests
// ============================================================================

TEST_CASE("PrintStartProfile: forge_x profile loads with signal formats", "[profile][print]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    // If forge_x.json is missing, we'll get the default profile
    // Only run forge_x-specific tests if we actually loaded it
    if (profile->name().find("Forge") != std::string::npos) {
        SECTION("Profile has sequential progress mode") {
            REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::SEQUENTIAL);
        }

        SECTION("Profile has signal formats") {
            REQUIRE(profile->has_signal_formats());
        }

        SECTION("Profile name and description") {
            REQUIRE(profile->name() == "Forge-X Mod");
            REQUIRE_FALSE(profile->description().empty());
        }
    }
}

// ============================================================================
// Default Profile Response Pattern Matching Tests
// (Same cases as test_print_start_collector.cpp to ensure parity)
// ============================================================================

TEST_CASE("PrintStartProfile: default patterns match homing commands", "[profile][print][homing]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("G28", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    REQUIRE(profile->try_match_pattern("G28 X Y Z", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    REQUIRE(profile->try_match_pattern("Homing axes", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    REQUIRE(profile->try_match_pattern("Home All Axes", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    REQUIRE(profile->try_match_pattern("// homing started", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    // Negative cases
    REQUIRE_FALSE(profile->try_match_pattern("G29", result));
    REQUIRE_FALSE(profile->try_match_pattern("M104", result));
}

TEST_CASE("PrintStartProfile: default patterns match heating bed commands",
          "[profile][print][heating]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("M190 S60", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    REQUIRE(profile->try_match_pattern("M140 S60", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    REQUIRE(profile->try_match_pattern("Heating bed to 60", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    REQUIRE(profile->try_match_pattern("Heat Bed", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    REQUIRE(profile->try_match_pattern("BED_TEMP=60", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    REQUIRE(profile->try_match_pattern("bed heating", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    // Real Voron V2 macro: M190 S{BED_TEMP}
    REQUIRE(profile->try_match_pattern("M190 S110", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    // Negative: setting to 0 (cooling) should not match bed heating
    REQUIRE_FALSE(profile->try_match_pattern("M140 S0", result));

    // M104 S200 matches HEATING_NOZZLE, not HEATING_BED
    REQUIRE(profile->try_match_pattern("M104 S200", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);
}

TEST_CASE("PrintStartProfile: default patterns match heating nozzle commands",
          "[profile][print][heating]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("M109 S200", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    REQUIRE(profile->try_match_pattern("M104 S200", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    REQUIRE(profile->try_match_pattern("M104 S150", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    REQUIRE(profile->try_match_pattern("Heating nozzle to 200", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    REQUIRE(profile->try_match_pattern("Heating hotend", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    REQUIRE(profile->try_match_pattern("Heating extruder", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    REQUIRE(profile->try_match_pattern("EXTRUDER_TEMP=200", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    // Negative: cooling command should not match
    REQUIRE_FALSE(profile->try_match_pattern("M104 S0", result));

    // M190 S60 matches HEATING_BED, not HEATING_NOZZLE
    REQUIRE(profile->try_match_pattern("M190 S60", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);
}

TEST_CASE("PrintStartProfile: default patterns match QGL commands", "[profile][print][leveling]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("QUAD_GANTRY_LEVEL", result));
    REQUIRE(result.phase == PrintStartPhase::QGL);

    REQUIRE(profile->try_match_pattern("quad gantry level", result));
    REQUIRE(result.phase == PrintStartPhase::QGL);

    REQUIRE(profile->try_match_pattern("Running QGL", result));
    REQUIRE(result.phase == PrintStartPhase::QGL);

    // Z_TILT_ADJUST matches Z_TILT, not QGL
    REQUIRE(profile->try_match_pattern("Z_TILT_ADJUST", result));
    REQUIRE(result.phase == PrintStartPhase::Z_TILT);
}

TEST_CASE("PrintStartProfile: default patterns match Z_TILT commands",
          "[profile][print][leveling]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("Z_TILT_ADJUST", result));
    REQUIRE(result.phase == PrintStartPhase::Z_TILT);

    REQUIRE(profile->try_match_pattern("z_tilt_adjust", result));
    REQUIRE(result.phase == PrintStartPhase::Z_TILT);

    REQUIRE(profile->try_match_pattern("z tilt adjust", result));
    REQUIRE(result.phase == PrintStartPhase::Z_TILT);

    // QUAD_GANTRY_LEVEL matches QGL, not Z_TILT
    REQUIRE(profile->try_match_pattern("QUAD_GANTRY_LEVEL", result));
    REQUIRE(result.phase == PrintStartPhase::QGL);
}

TEST_CASE("PrintStartProfile: default patterns match bed mesh commands", "[profile][print][mesh]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("BED_MESH_CALIBRATE", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);

    REQUIRE(profile->try_match_pattern("BED_MESH_PROFILE LOAD=default", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);

    REQUIRE(profile->try_match_pattern("Loading bed mesh", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);

    REQUIRE(profile->try_match_pattern("mesh loading", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);

    REQUIRE(profile->try_match_pattern("BED_MESH_CALIBRATE PROFILE=adaptive ADAPTIVE=1", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);

    // Negative
    REQUIRE_FALSE(profile->try_match_pattern("BED_MESH_CLEAR", result));
}

TEST_CASE("PrintStartProfile: default patterns match cleaning commands",
          "[profile][print][cleaning]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("CLEAN_NOZZLE", result));
    REQUIRE(result.phase == PrintStartPhase::CLEANING);

    REQUIRE(profile->try_match_pattern("NOZZLE_CLEAN", result));
    REQUIRE(result.phase == PrintStartPhase::CLEANING);

    REQUIRE(profile->try_match_pattern("WIPE_NOZZLE", result));
    REQUIRE(result.phase == PrintStartPhase::CLEANING);

    REQUIRE(profile->try_match_pattern("nozzle wipe", result));
    REQUIRE(result.phase == PrintStartPhase::CLEANING);

    REQUIRE(profile->try_match_pattern("clean nozzle", result));
    REQUIRE(result.phase == PrintStartPhase::CLEANING);

    // PURGE_LINE matches PURGING, not CLEANING
    REQUIRE(profile->try_match_pattern("PURGE_LINE", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);
}

TEST_CASE("PrintStartProfile: default patterns match purging commands",
          "[profile][print][purging]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("VORON_PURGE", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("LINE_PURGE", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("PURGE_LINE", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("Prime Line", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("PrimeLine", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("Priming extruder", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("KAMP_ADAPTIVE_PURGE", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("purge line done", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    // CLEAN_NOZZLE matches CLEANING, not PURGING
    REQUIRE(profile->try_match_pattern("CLEAN_NOZZLE", result));
    REQUIRE(result.phase == PrintStartPhase::CLEANING);
}

// ============================================================================
// Default Profile Real Voron V2 Macro Test
// ============================================================================

TEST_CASE("PrintStartProfile: default patterns match Voron V2 START_PRINT lines",
          "[profile][print][voron]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    struct TestCase {
        std::string line;
        PrintStartPhase expected_phase;
        const char* description;
    };

    std::vector<TestCase> voron_lines = {
        {"M104 S150", PrintStartPhase::HEATING_NOZZLE, "mesh temp heating"},
        {"M190 S110", PrintStartPhase::HEATING_BED, "bed temp wait"},
        {"G28", PrintStartPhase::HOMING, "home all"},
        {"clean_nozzle", PrintStartPhase::CLEANING, "nozzle clean macro"},
        {"QUAD_GANTRY_LEVEL", PrintStartPhase::QGL, "quad gantry level"},
        {"G28 Z", PrintStartPhase::HOMING, "home Z after QGL"},
        {"BED_MESH_CALIBRATE PROFILE=adaptive ADAPTIVE=1", PrintStartPhase::BED_MESH,
         "adaptive bed mesh"},
        {"M109 S250", PrintStartPhase::HEATING_NOZZLE, "extruder temp wait"},
        {"VORON_PURGE", PrintStartPhase::PURGING, "voron purge"},
    };

    for (const auto& tc : voron_lines) {
        CAPTURE(tc.description, tc.line);
        REQUIRE(profile->try_match_pattern(tc.line, result));
        REQUIRE(result.phase == tc.expected_phase);
    }
}

// ============================================================================
// Default Profile AD5M Macro Test
// ============================================================================

TEST_CASE("PrintStartProfile: default patterns match AD5M START_PRINT lines",
          "[profile][print][ad5m]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    struct TestCase {
        std::string line;
        PrintStartPhase expected_phase;
        const char* description;
    };

    std::vector<TestCase> ad5m_lines = {
        {"M140 S60", PrintStartPhase::HEATING_BED, "set bed temp"},
        {"M104 S200", PrintStartPhase::HEATING_NOZZLE, "set nozzle temp"},
        {"G28", PrintStartPhase::HOMING, "home all"},
        {"BED_MESH_CALIBRATE mesh_min=-100,-100 mesh_max=100,100", PrintStartPhase::BED_MESH,
         "KAMP mesh calibrate"},
        {"BED_MESH_PROFILE LOAD=auto", PrintStartPhase::BED_MESH, "load auto mesh profile"},
        {"LINE_PURGE", PrintStartPhase::PURGING, "KAMP line purge"},
    };

    for (const auto& tc : ad5m_lines) {
        CAPTURE(tc.description, tc.line);
        REQUIRE(profile->try_match_pattern(tc.line, result));
        REQUIRE(result.phase == tc.expected_phase);
    }
}

// ============================================================================
// Phase Weight Tests
// ============================================================================

TEST_CASE("PrintStartProfile: phase weights match expected values", "[profile][print]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    SECTION("Known phases have non-zero weights") {
        REQUIRE(profile->get_phase_weight(PrintStartPhase::HOMING) == 10);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_BED) == 20);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_NOZZLE) == 20);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::QGL) == 15);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::Z_TILT) == 15);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::BED_MESH) == 10);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::CLEANING) == 5);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::PURGING) == 5);
    }

    SECTION("Unknown/unused phases return 0") {
        REQUIRE(profile->get_phase_weight(PrintStartPhase::IDLE) == 0);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::COMPLETE) == 0);
    }
}

TEST_CASE("PrintStartProfile: forge_x phase weights", "[profile][print]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    // Only test if forge_x loaded (not default fallback)
    if (profile->name().find("Forge") != std::string::npos) {
        REQUIRE(profile->get_phase_weight(PrintStartPhase::INITIALIZING) == 5);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::HOMING) == 5);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_BED) == 15);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_NOZZLE) == 15);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::CLEANING) == 20);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::BED_MESH) == 25);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::PURGING) == 10);
    }
}

// ============================================================================
// Forge-X Signal Format Matching Tests
// ============================================================================

TEST_CASE("PrintStartProfile: forge_x signal format matching for all 14 states",
          "[profile][print][signal]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    // Only run if forge_x loaded
    if (!profile->has_signal_formats()) {
        SKIP("forge_x.json not available, skipping signal format tests");
    }

    PrintStartProfile::MatchResult result;

    struct SignalTest {
        std::string line;
        PrintStartPhase expected_phase;
        int expected_progress;
        const char* description;
    };

    // clang-format off
    std::vector<SignalTest> signals = {
        {"// State: PREPARING...",           PrintStartPhase::INITIALIZING,    3,   "preparing"},
        {"// State: MD5 CHECK",              PrintStartPhase::INITIALIZING,    5,   "md5 check"},
        {"// State: HOMING...",              PrintStartPhase::HOMING,          10,  "homing"},
        {"// State: PREPARE CLEANING...",    PrintStartPhase::CLEANING,        15,  "prepare cleaning"},
        {"// State: HEATING...",             PrintStartPhase::HEATING_BED,     25,  "heating"},
        {"// State: CLEANING START SOON",    PrintStartPhase::CLEANING,        30,  "cleaning start soon"},
        {"// State: CLEANING...",            PrintStartPhase::CLEANING,        38,  "cleaning"},
        {"// State: COOLING DOWN...",        PrintStartPhase::CLEANING,        45,  "cooling down"},
        {"// State: FINISHING CLEANING...",   PrintStartPhase::CLEANING,        55,  "finishing cleaning"},
        {"// State: DONE!",                  PrintStartPhase::CLEANING,        57,  "done"},
        {"// State: KAMP LEVELING...",       PrintStartPhase::BED_MESH,        60,  "kamp leveling"},
        {"// State: WAIT FOR TEMPERATURE...", PrintStartPhase::HEATING_NOZZLE, 82,  "wait for temp"},
        {"// State: KAMP PRIMING...",        PrintStartPhase::PURGING,         90,  "kamp priming"},
        {"// State: PRINTING...",            PrintStartPhase::COMPLETE,         100, "printing"},
    };
    // clang-format on

    for (const auto& tc : signals) {
        CAPTURE(tc.description, tc.line);
        REQUIRE(profile->try_match_signal(tc.line, result));
        REQUIRE(result.phase == tc.expected_phase);
        REQUIRE(result.progress == tc.expected_progress);
    }
}

TEST_CASE("PrintStartProfile: forge_x KAMP LEVELING message says 'Creating bed mesh'",
          "[profile][print][signal]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    if (!profile->has_signal_formats()) {
        SKIP("forge_x.json not available, skipping bed mesh message test");
    }

    PrintStartProfile::MatchResult result;
    REQUIRE(profile->try_match_signal("// State: KAMP LEVELING...", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);
    REQUIRE(result.message == "Creating bed mesh...");
}

// ============================================================================
// Signal Format Matching with Surrounding Context
// ============================================================================

TEST_CASE("PrintStartProfile: signal matching with surrounding text", "[profile][print][signal]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    if (!profile->has_signal_formats()) {
        SKIP("forge_x.json not available, skipping signal context tests");
    }

    PrintStartProfile::MatchResult result;

    SECTION("Prefix found within longer line") {
        // The prefix "// State: " can appear anywhere in the line
        REQUIRE(profile->try_match_signal("// State: HOMING...", result));
        REQUIRE(result.phase == PrintStartPhase::HOMING);
    }

    SECTION("Unrecognized value after prefix does not match") {
        REQUIRE_FALSE(profile->try_match_signal("// State: UNKNOWN_STATE", result));
    }

    SECTION("Empty value after prefix does not match") {
        REQUIRE_FALSE(profile->try_match_signal("// State: ", result));
    }

    SECTION("Line without the prefix does not match") {
        REQUIRE_FALSE(profile->try_match_signal("State: HOMING...", result));
    }
}

// ============================================================================
// Forge-X Response Pattern Matching (Temperature Wait Lines)
// ============================================================================

TEST_CASE("PrintStartProfile: forge_x response patterns match temperature wait lines",
          "[profile][print][pattern]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    // Only test if forge_x loaded
    if (profile->name().find("Forge") == std::string::npos) {
        SKIP("forge_x.json not available, skipping response pattern tests");
    }

    PrintStartProfile::MatchResult result;

    SECTION("Bed temperature wait line with capture group") {
        REQUIRE(profile->try_match_pattern("// Wait bed temperature to reach 60", result));
        REQUIRE(result.phase == PrintStartPhase::HEATING_BED);
        // $1 should be substituted with "60"
        REQUIRE(result.message.find("60") != std::string::npos);
    }

    SECTION("Extruder temperature wait line with capture group") {
        REQUIRE(profile->try_match_pattern("// Wait extruder temperature to reach 210", result));
        REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);
        // $1 should be substituted with "210"
        REQUIRE(result.message.find("210") != std::string::npos);
    }

    SECTION("Different temperature values") {
        REQUIRE(profile->try_match_pattern("// Wait bed temperature to reach 110", result));
        REQUIRE(result.message.find("110") != std::string::npos);

        REQUIRE(profile->try_match_pattern("// Wait extruder temperature to reach 250", result));
        REQUIRE(result.message.find("250") != std::string::npos);
    }

    SECTION("Non-matching lines") {
        REQUIRE_FALSE(profile->try_match_pattern("Wait for temperature", result));
        REQUIRE_FALSE(profile->try_match_pattern("// Set bed temperature to 60", result));
    }
}

// ============================================================================
// Progress Mode Detection
// ============================================================================

TEST_CASE("PrintStartProfile: progress mode detection", "[profile][print]") {
    SECTION("Default profile uses weighted mode") {
        auto profile = get_default_profile();
        REQUIRE(profile != nullptr);
        REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::WEIGHTED);
    }

    SECTION("Forge-X profile uses sequential mode") {
        auto profile = get_forge_x_profile();
        REQUIRE(profile != nullptr);

        if (profile->name().find("Forge") != std::string::npos) {
            REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::SEQUENTIAL);
        }
    }
}

// ============================================================================
// Missing Profile Fallback
// ============================================================================

TEST_CASE("PrintStartProfile: missing profile falls back to default", "[profile][print]") {
    auto profile = PrintStartProfile::load("nonexistent_profile_xyz");
    REQUIRE(profile != nullptr);

    // Should get the default profile (either from JSON or built-in)
    REQUIRE(profile->name().find("Generic") != std::string::npos);

    // Should still have working patterns
    PrintStartProfile::MatchResult result;
    REQUIRE(profile->try_match_pattern("G28", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);
}

// ============================================================================
// Malformed JSON Handling
// ============================================================================

TEST_CASE("PrintStartProfile: graceful handling of edge cases", "[profile][print]") {
    SECTION("Empty profile name loads default") {
        auto profile = PrintStartProfile::load("");
        REQUIRE(profile != nullptr);
        // Either loads the default or falls back to built-in
    }

    SECTION("Profile with path traversal loads default") {
        auto profile = PrintStartProfile::load("../../../etc/passwd");
        REQUIRE(profile != nullptr);
        // File won't exist, should fall back to default
    }

    SECTION("Default profile is always available (built-in fallback)") {
        // Even if no JSON files exist, load_default() should return a usable profile
        auto profile = PrintStartProfile::load_default();
        REQUIRE(profile != nullptr);
        REQUIRE_FALSE(profile->name().empty());

        // Built-in patterns should work
        PrintStartProfile::MatchResult result;
        REQUIRE(profile->try_match_pattern("G28", result));
        REQUIRE(result.phase == PrintStartPhase::HOMING);
    }
}

// ============================================================================
// Noise Rejection Tests (same as test_print_start_collector.cpp)
// ============================================================================

TEST_CASE("PrintStartProfile: default patterns reject noise lines", "[profile][print][negative]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    std::vector<std::string> noise_lines = {
        "ok",
        "// Klipper state: Ready",
        "T:210.5 /210.0 B:60.2 /60.0",
        "echo: Command completed",
        "TOOLHEAD_PARK_MACRO",
        "SET_LED LED=nozzle RED=1",
        "M141 S45", // Chamber temp (not bed or nozzle)
        "AFC_PARK",
        "SMART_PARK",
        "TOOLCHANGE TOOL=0",
        "BED_MESH_CLEAR",
        "SET_AFC_TOOLCHANGES TOOLCHANGES=0",
    };

    for (const auto& line : noise_lines) {
        CAPTURE(line);
        // Should not match any signal format
        REQUIRE_FALSE(profile->try_match_signal(line, result));
        // Should not match any response pattern
        REQUIRE_FALSE(profile->try_match_pattern(line, result));
    }
}

// ============================================================================
// Capture Group Substitution Tests
// ============================================================================

TEST_CASE("PrintStartProfile: capture group substitution in message templates",
          "[profile][print][pattern]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("Forge") == std::string::npos) {
        SKIP("forge_x.json not available");
    }

    PrintStartProfile::MatchResult result;

    SECTION("Single capture group substitution") {
        REQUIRE(profile->try_match_pattern("// Wait bed temperature to reach 75", result));
        // Template is "Heating bed to $1 C..." -> "Heating bed to 75 C..."
        REQUIRE(result.message.find("75") != std::string::npos);
    }

    SECTION("Capture group with large number") {
        REQUIRE(profile->try_match_pattern("// Wait extruder temperature to reach 300", result));
        REQUIRE(result.message.find("300") != std::string::npos);
    }
}

// ============================================================================
// Creality K1 Profile Tests
// ============================================================================

static std::shared_ptr<PrintStartProfile> get_creality_k1_profile() {
    return PrintStartProfile::load("creality_k1");
}

TEST_CASE("PrintStartProfile: creality_k1 profile loads successfully", "[profile][print][k1]") {
    auto profile = get_creality_k1_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("K1") == std::string::npos) {
        SKIP("creality_k1.json not available");
    }

    SECTION("Profile has correct name") {
        REQUIRE(profile->name() == "Creality K1");
    }

    SECTION("Profile uses weighted progress mode") {
        REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::WEIGHTED);
    }

    SECTION("Profile has signal formats for pre-preparation") {
        REQUIRE(profile->has_signal_formats());
    }
}

TEST_CASE("PrintStartProfile: creality_k1 phase weights", "[profile][print][k1]") {
    auto profile = get_creality_k1_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("K1") == std::string::npos) {
        SKIP("creality_k1.json not available");
    }

    REQUIRE(profile->get_phase_weight(PrintStartPhase::INITIALIZING) == 5);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::HOMING) == 10);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::CLEANING) == 15);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::BED_MESH) == 15);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_NOZZLE) == 30);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::PURGING) == 20);
}

TEST_CASE("PrintStartProfile: creality_k1 patterns match real K1C gcode responses",
          "[profile][print][k1]") {
    auto profile = get_creality_k1_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("K1") == std::string::npos) {
        SKIP("creality_k1.json not available");
    }

    PrintStartProfile::MatchResult result;

    // These are actual gcode responses captured from a K1C print start
    struct TestCase {
        std::string line;
        PrintStartPhase expected_phase;
        const char* description;
    };

    // clang-format off
    std::vector<TestCase> k1c_lines = {
        {"// not prepare.",                   PrintStartPhase::INITIALIZING,    "START_PRINT no pre-prep"},
        {"// x_axes: xyz",                    PrintStartPhase::HOMING,          "CX_ROUGH_G28 complete"},
        {"// [CLEAR_NOZZLE_QUICK] src_pos[2]:3.28", PrintStartPhase::CLEANING, "CX_NOZZLE_CLEAR"},
        {"CX_PRINT_LEVELING_CALIBRATION",     PrintStartPhase::BED_MESH,        "leveling calibration"},
        {"// probe at 50.000,50.000 is z=1.23", PrintStartPhase::BED_MESH,     "probe point"},
        {"// can_break_flag = 0",             PrintStartPhase::HEATING_NOZZLE,  "heating wait started"},
        {"// can_break_flag = 3",             PrintStartPhase::PURGING,         "heating done"},
        {"// can_break_flag is 3",            PrintStartPhase::PURGING,         "heating done (alt)"},
    };
    // clang-format on

    for (const auto& tc : k1c_lines) {
        CAPTURE(tc.description, tc.line);
        REQUIRE(profile->try_match_pattern(tc.line, result));
        REQUIRE(result.phase == tc.expected_phase);
    }
}

TEST_CASE("PrintStartProfile: creality_k1 signal format matches 'print prepared'",
          "[profile][print][k1][signal]") {
    auto profile = get_creality_k1_profile();
    REQUIRE(profile != nullptr);

    if (!profile->has_signal_formats()) {
        SKIP("creality_k1.json not available or no signal formats");
    }

    PrintStartProfile::MatchResult result;

    SECTION("print prepared signal triggers COMPLETE") {
        REQUIRE(profile->try_match_signal("// print prepared", result));
        REQUIRE(result.phase == PrintStartPhase::COMPLETE);
    }

    SECTION("Unrelated messages don't match signal format") {
        REQUIRE_FALSE(profile->try_match_signal("// x_axes: xyz", result));
        REQUIRE_FALSE(profile->try_match_signal("// wait temp end", result));
    }
}

TEST_CASE("PrintStartProfile: creality_k1 rejects noise from K1C responses",
          "[profile][print][k1][negative]") {
    auto profile = get_creality_k1_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("K1") == std::string::npos) {
        SKIP("creality_k1.json not available");
    }

    PrintStartProfile::MatchResult result;

    // Real K1C noise lines that should NOT match any phase
    std::vector<std::string> noise = {
        "// wait temp end",
        "// wait temp start",
        "// x_park = -104.5",
        "// y_park = 104.5",
        "// Run Current: 0.56A Hold Current: 0.56A",
        "B:56.8 /55.0 T0:175.3 /220.0",
        "File opened:Cube_PLA_25m49s.gcode Size:224837",
        "File selected",
        "Done printing file",
    };

    for (const auto& line : noise) {
        CAPTURE(line);
        REQUIRE_FALSE(profile->try_match_pattern(line, result));
    }
}

TEST_CASE("PrintStartProfile: creality_k1 full print sequence progression",
          "[profile][print][k1]") {
    auto profile = get_creality_k1_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("K1") == std::string::npos) {
        SKIP("creality_k1.json not available");
    }

    PrintStartProfile::MatchResult result;

    // Walk through the real K1C print sequence and verify phases advance
    // and weighted progress increases
    std::set<PrintStartPhase> detected;
    int total_weight = 0;

    auto process = [&](const std::string& line) {
        if (profile->try_match_pattern(line, result)) {
            if (detected.insert(result.phase).second) {
                total_weight += profile->get_phase_weight(result.phase);
            }
        }
    };

    process("// not prepare.");
    REQUIRE(detected.count(PrintStartPhase::INITIALIZING) == 1);
    REQUIRE(total_weight == 5);

    process("// x_axes: xyz");
    REQUIRE(detected.count(PrintStartPhase::HOMING) == 1);
    REQUIRE(total_weight == 15);

    process("// [CLEAR_NOZZLE_QUICK] src_pos[2]:3.28");
    REQUIRE(detected.count(PrintStartPhase::CLEANING) == 1);
    REQUIRE(total_weight == 30);

    process("CX_PRINT_LEVELING_CALIBRATION");
    REQUIRE(detected.count(PrintStartPhase::BED_MESH) == 1);
    REQUIRE(total_weight == 45);

    process("// can_break_flag = 0");
    REQUIRE(detected.count(PrintStartPhase::HEATING_NOZZLE) == 1);
    REQUIRE(total_weight == 75);

    // Repeated temp reports should not add new phases
    process("B:56.8 /55.0 T0:175.3 /220.0");
    REQUIRE(total_weight == 75);

    process("// can_break_flag = 3");
    REQUIRE(detected.count(PrintStartPhase::PURGING) == 1);
    REQUIRE(total_weight == 95);
}

// ============================================================================
// Snapmaker U1 Profile Tests
//
// Ground truth captured from a live U1 print on 2026-05-19 by tailing
// /var/log/messages and observing every line that hit
// PrintStartCollector::on_gcode_response. The Snapmaker U1 PRINT_START
// gcode_macro is essentially empty — every preprint phase is driven by
// slicer-injected gcode running on a Klipper fork that emits state
// transitions via SET_ACTION_CODE. These tests codify the response
// strings actually observed so a future profile edit can't silently
// regress phase detection.
// ============================================================================

static std::shared_ptr<PrintStartProfile> get_snapmaker_u1_profile() {
    return PrintStartProfile::load("snapmaker_u1");
}

TEST_CASE("PrintStartProfile: snapmaker_u1 profile loads",
          "[profile][print][snapmaker]") {
    auto profile = get_snapmaker_u1_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("Snapmaker") == std::string::npos) {
        SKIP("snapmaker_u1.json not available");
    }

    REQUIRE(profile->name() == "Snapmaker U1");
    REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::WEIGHTED);
    REQUIRE(profile->has_signal_formats());
}

TEST_CASE("PrintStartProfile: snapmaker_u1 action-code signals route to correct phase",
          "[profile][print][snapmaker][signal]") {
    auto profile = get_snapmaker_u1_profile();
    REQUIRE(profile != nullptr);
    if (profile->name().find("Snapmaker") == std::string::npos) {
        SKIP("snapmaker_u1.json not available");
    }

    PrintStartProfile::MatchResult result;

    // Each "// Success: Set action code <CODE>" line was observed verbatim in
    // the 2026-05-19 capture. The mapping below is what the user actually
    // sees; getting the message strings right matters because there is no
    // other surface that explains which preprint sub-phase is running.
    struct ActionCase {
        std::string line;
        PrintStartPhase expected_phase;
        const char* expected_message;
    };
    std::vector<ActionCase> cases = {
        {"// Success: Set action code BED_PREHEATING",
         PrintStartPhase::HEATING_BED, "Heating Bed..."},
        {"// Success: Set action code BED_PRESCANNING",
         PrintStartPhase::BED_MESH, "Pre-scanning Bed..."},
        {"// Success: Set action code BED_LEVELING",
         PrintStartPhase::BED_MESH, "Levelling Bed..."},
        {"// Success: Set action code DETECT_PLATE",
         PrintStartPhase::BED_MESH, "Detecting Plate..."},
        {"// Success: Set action code PRINT_BED_DETECTING",
         PrintStartPhase::BED_MESH, "Inspecting Bed..."},
        {"// Success: Set action code PRINT_SWITCH_CHECKING",
         PrintStartPhase::INITIALIZING, "Checking Extruder..."},
        {"// Success: Set action code PRINT_PREEXTRUDING",
         PrintStartPhase::PURGING, "Pre-extruding..."},
        {"// Success: Set action code PRINT_RESUMING",
         PrintStartPhase::INITIALIZING, "Resuming Print..."},
        {"// Success: Set action code PRINT_REPLENISHING",
         PrintStartPhase::INITIALIZING, "Replenishing Filament..."},
    };

    for (const auto& c : cases) {
        CAPTURE(c.line);
        REQUIRE(profile->try_match_signal(c.line, result));
        REQUIRE(result.phase == c.expected_phase);
        REQUIRE(result.message == c.expected_message);
    }

    // IDLE action codes intentionally have no mapping — they bracket
    // every real phase and should not steer the UI.
    REQUIRE_FALSE(profile->try_match_signal(
        "// Success: Set action code IDLE", result));

    // Future Snapmaker firmware revisions may emit action codes we
    // don't know about yet; they must fall through cleanly (no false
    // match into an unrelated phase).
    REQUIRE_FALSE(profile->try_match_signal(
        "// Success: Set action code FOO_UNKNOWN", result));

    // try_match_signal trims trailing whitespace (parser handles \r
    // line endings from some firmware variants).
    REQUIRE(profile->try_match_signal(
        "// Success: Set action code BED_PREHEATING\r", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);
}

TEST_CASE("PrintStartProfile: snapmaker_u1 response patterns match real preprint lines",
          "[profile][print][snapmaker]") {
    auto profile = get_snapmaker_u1_profile();
    REQUIRE(profile != nullptr);
    if (profile->name().find("Snapmaker") == std::string::npos) {
        SKIP("snapmaker_u1.json not available");
    }

    PrintStartProfile::MatchResult result;

    // Klipper emits one of these per axis-trigger during G28. Both flavours
    // (single axis, both axes) appeared in the capture. The pattern must
    // anchor on the prefix so generic "trigger" strings don't false-match.
    REQUIRE(profile->try_match_pattern(
        "// trigger_mcu_pos: {'stepper_y': 29734, 'stepper_x': 26265}", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    REQUIRE(profile->try_match_pattern(
        "// trigger_mcu_pos: {'stepper_x': -1832, 'stepper_y': -1561}", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    // Single-point Z probe before bed mesh. Snapmaker writes the probe
    // origin via this signature once per probe initiation.
    REQUIRE(profile->try_match_pattern(
        "// probe_start_x: 5.30000, probe_start_y: 4.90156", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);

    // Negative cases — these must NOT match (they used to under the
    // pre-2026-05-19 profile which keyed on command names that never
    // actually appear in gcode_response).
    REQUIRE_FALSE(profile->try_match_pattern("G28 X Y", result));
    REQUIRE_FALSE(profile->try_match_pattern("M109 S250", result));
    REQUIRE_FALSE(profile->try_match_pattern("BED_MESH_CALIBRATE", result));
    REQUIRE_FALSE(profile->try_match_pattern("VORON_PURGE", result));
    REQUIRE_FALSE(profile->try_match_pattern("CLEAN_NOZZLE", result));
}

TEST_CASE("PrintStartProfile: snapmaker_u1 phase weights sum reasonably",
          "[profile][print][snapmaker]") {
    auto profile = get_snapmaker_u1_profile();
    REQUIRE(profile != nullptr);
    if (profile->name().find("Snapmaker") == std::string::npos) {
        SKIP("snapmaker_u1.json not available");
    }

    REQUIRE(profile->get_phase_weight(PrintStartPhase::HOMING) == 5);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_BED) == 25);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_NOZZLE) == 20);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::BED_MESH) == 35);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::INITIALIZING) == 5);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::CLEANING) == 5);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::PURGING) == 5);
}
