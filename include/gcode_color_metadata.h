// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace helix::gcode {

/**
 * @brief Parse a slicer comment line for the per-tool filament color palette.
 *
 * Slicers (OrcaSlicer, PrusaSlicer, Bambu Studio) emit metadata comments like:
 *   ; extruder_colour = #ED1C24;#00C1AE;#F4E2C1;#000000
 *   ; filament_colour = "#FF0000"
 *   ;extruder_color = #00FF00
 *
 * Recognized keys (case-insensitive): `extruder_colour`, `extruder_color`,
 * `filament_colour`, `filament_color`.
 *
 * The palette is slot-aligned: invalid or empty tokens between semicolons
 * become empty strings so callers can index by tool number. e.g.
 * `#A;;#B` → `{"#A", "", "#B"}`.
 *
 * @param line The full gcode comment line (with or without leading `;`).
 * @param out_palette Receives the parsed palette on success. Cleared first.
 * @return true if the line was a recognized filament-color line AND at least
 *         one valid `#RRGGBB[AA]` token was extracted; false otherwise.
 */
bool parse_filament_color_palette(std::string_view line,
                                  std::vector<std::string>& out_palette);

} // namespace helix::gcode
