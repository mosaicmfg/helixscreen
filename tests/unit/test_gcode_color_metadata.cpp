// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_color_metadata.h"

#include "../catch_amalgamated.hpp"

using helix::gcode::parse_filament_color_palette;

TEST_CASE("parse_filament_color_palette - keyword recognition", "[gcode][color_metadata]") {
    std::vector<std::string> palette;

    SECTION("extruder_colour matches") {
        REQUIRE(parse_filament_color_palette("; extruder_colour = #FF0000", palette));
        REQUIRE(palette == std::vector<std::string>{"#FF0000"});
    }

    SECTION("extruder_color (US spelling) matches") {
        REQUIRE(parse_filament_color_palette(";extruder_color=#00FF00", palette));
        REQUIRE(palette == std::vector<std::string>{"#00FF00"});
    }

    SECTION("filament_colour matches") {
        REQUIRE(parse_filament_color_palette("; filament_colour = #0000FF", palette));
        REQUIRE(palette == std::vector<std::string>{"#0000FF"});
    }

    SECTION("filament_color matches") {
        REQUIRE(parse_filament_color_palette("; filament_color = #ABCDEF", palette));
        REQUIRE(palette == std::vector<std::string>{"#ABCDEF"});
    }

    SECTION("Case insensitivity") {
        REQUIRE(parse_filament_color_palette("; EXTRUDER_COLOUR = #112233", palette));
        REQUIRE(palette == std::vector<std::string>{"#112233"});
    }

    SECTION("Unknown key rejected") {
        REQUIRE_FALSE(parse_filament_color_palette("; bed_colour = #FF0000", palette));
        REQUIRE(palette.empty());
    }

    SECTION("No '=' rejected") {
        REQUIRE_FALSE(parse_filament_color_palette("; extruder_colour", palette));
    }

    SECTION("Keyword in value (not key) rejected") {
        // Hash inside the value shouldn't smuggle keywords past the key check
        REQUIRE_FALSE(parse_filament_color_palette("; some_key = extruder_colour", palette));
    }
}

TEST_CASE("parse_filament_color_palette - palette extraction", "[gcode][color_metadata]") {
    std::vector<std::string> palette;

    SECTION("Multi-color semicolon-separated palette") {
        REQUIRE(parse_filament_color_palette(
            "; extruder_colour = #ED1C24;#00C1AE;#F4E2C1;#000000", palette));
        REQUIRE(palette ==
                std::vector<std::string>{"#ED1C24", "#00C1AE", "#F4E2C1", "#000000"});
    }

    SECTION("Slot alignment preserved with empty entries (#A;;#B)") {
        // REGRESSION GUARD: old gcode_parser.cpp::parse_extruder_color_metadata()
        // skipped empty tokens (yielded size-2 palette), which mis-aligned T2 to
        // palette[1]. The shared helper now pushes empty placeholders so callers
        // can index by tool number reliably.
        REQUIRE(parse_filament_color_palette("; extruder_colour = #FF0000;;#00FF00", palette));
        REQUIRE(palette.size() == 3);
        REQUIRE(palette == std::vector<std::string>{"#FF0000", "", "#00FF00"});
    }

    SECTION("CRLF line endings handled") {
        REQUIRE(parse_filament_color_palette("; extruder_colour = #FF0000\r\n", palette));
        REQUIRE(palette == std::vector<std::string>{"#FF0000"});
    }

    SECTION("Trailing comment after value") {
        // Slicers don't typically emit trailing comments, but if a downstream
        // tool inserts one, the second '#' token is invalid hex (size != 6/8)
        // and becomes an empty placeholder rather than a parse failure.
        REQUIRE(parse_filament_color_palette(
            "; extruder_colour = #FF0000 ; # not a color", palette));
        REQUIRE(palette.size() == 2);
        REQUIRE(palette[0] == "#FF0000");
        REQUIRE(palette[1].empty());
    }

    SECTION("Quoted values handled") {
        REQUIRE(parse_filament_color_palette("; filament_colour = \"#FF0000\"", palette));
        REQUIRE(palette == std::vector<std::string>{"#FF0000"});
    }

    SECTION("Whitespace around tokens trimmed") {
        REQUIRE(parse_filament_color_palette(
            ";extruder_colour=#AA0000 ; #00BB00 ;#0000CC", palette));
        REQUIRE(palette == std::vector<std::string>{"#AA0000", "#00BB00", "#0000CC"});
    }

    SECTION("8-digit RGBA accepted") {
        REQUIRE(parse_filament_color_palette("; extruder_colour = #11223344", palette));
        REQUIRE(palette == std::vector<std::string>{"#11223344"});
    }

    SECTION("Invalid hex length yields empty placeholder") {
        // "#XYZ" is invalid; treat as occupied-but-unknown so slot indices stay aligned
        REQUIRE(parse_filament_color_palette(
            "; extruder_colour = #FF0000;#XYZ;#00FF00", palette));
        REQUIRE(palette == std::vector<std::string>{"#FF0000", "", "#00FF00"});
    }

    SECTION("Empty value entirely returns false (no entries to use)") {
        REQUIRE_FALSE(parse_filament_color_palette("; extruder_colour = ", palette));
        REQUIRE(palette.empty());
    }

    SECTION("All-empty entries returns false") {
        REQUIRE_FALSE(parse_filament_color_palette("; extruder_colour = ;;;", palette));
        REQUIRE(palette.empty());
    }
}

TEST_CASE("parse_filament_color_palette - real-world OrcaSlicer output",
          "[gcode][color_metadata]") {
    std::vector<std::string> palette;

    SECTION("K2 Plus dual-filament (single-color print using filament 1)") {
        REQUIRE(parse_filament_color_palette(
            "; extruder_colour = #000000;#F3FDFD", palette));
        REQUIRE(palette.size() == 2);
        REQUIRE(palette[0] == "#000000");
        REQUIRE(palette[1] == "#F3FDFD");
    }

    SECTION("Output buffer is cleared on each call") {
        // Pre-populate with stale data
        palette = {"#STALE", "#STALE"};
        REQUIRE(parse_filament_color_palette("; extruder_colour = #FF0000", palette));
        REQUIRE(palette == std::vector<std::string>{"#FF0000"});
    }

    SECTION("Failed parse leaves palette empty") {
        palette = {"#STALE"};
        REQUIRE_FALSE(parse_filament_color_palette("not a color line", palette));
        REQUIRE(palette.empty());
    }
}
