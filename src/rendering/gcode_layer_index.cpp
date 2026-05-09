// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_layer_index.h"

#include "gcode_color_metadata.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace helix {
namespace gcode {

namespace {

// Layer detection tolerance for Z changes
constexpr float Z_EPSILON = 0.001f;

// Extract float parameter from G-code line (e.g., "Z1.2" -> 1.2)
bool extract_z_param(const char* line, size_t len, float& out_z) {
    // Find 'Z' parameter (case-insensitive)
    for (size_t i = 0; i < len; ++i) {
        if ((line[i] == 'Z' || line[i] == 'z') && i + 1 < len) {
            // Check if followed by a digit or sign
            char next = line[i + 1];
            if (next == '-' || next == '+' || next == '.' || (next >= '0' && next <= '9')) {
                char* end = nullptr;
                out_z = std::strtof(line + i + 1, &end);
                if (end != line + i + 1) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Check if line is a movement command (G0 or G1)
bool is_movement_command(const char* line, size_t len) {
    // Skip leading whitespace
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }

    // Check for G0 or G1
    if (i + 1 < len && (line[i] == 'G' || line[i] == 'g')) {
        char next = line[i + 1];
        if (next == '0' || next == '1') {
            // Make sure it's not G10, G100, etc.
            if (i + 2 >= len || line[i + 2] == ' ' || line[i + 2] == '\t' || line[i + 2] == ';' ||
                line[i + 2] == '\r' || line[i + 2] == '\n') {
                return true;
            }
            // G0 followed by coordinate is also valid (G0X10)
            char after = line[i + 2];
            if (after == 'X' || after == 'Y' || after == 'Z' || after == 'E' || after == 'F' ||
                after == 'x' || after == 'y' || after == 'z' || after == 'e' || after == 'f') {
                return true;
            }
        }
    }
    return false;
}

// Check if line contains E parameter with positive value (extrusion)
bool has_positive_extrusion(const char* line, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if ((line[i] == 'E' || line[i] == 'e') && i + 1 < len) {
            char next = line[i + 1];
            // Positive number (or number starting with digit)
            if ((next >= '0' && next <= '9') || next == '+') {
                return true;
            }
        }
    }
    return false;
}

// Extract filament/extruder color from metadata comment
// Thin wrapper over helix::gcode::parse_filament_color_palette() that also
// projects the legacy single-color field (palette[0]) for backward compat.
bool extract_filament_color(const char* line, size_t len, std::string& out_color,
                            std::vector<std::string>& out_palette) {
    if (len < 10 || line[0] != ';') {
        return false;
    }
    if (!helix::gcode::parse_filament_color_palette(std::string_view(line, len), out_palette)) {
        return false;
    }
    // First non-empty entry is the legacy "single color" surface.
    for (const auto& s : out_palette) {
        if (!s.empty()) {
            out_color = s;
            return true;
        }
    }
    return false;
}

// Check if line is a layer change marker
bool is_layer_marker(const char* line, size_t len) {
    // Look for ;LAYER_CHANGE or ; LAYER_CHANGE
    const char* pos = std::strstr(line, "LAYER_CHANGE");
    if (pos && pos < line + len) {
        return true;
    }
    // Also check lowercase
    // Manual search since strstr doesn't do case-insensitive
    for (size_t i = 0; i + 12 <= len; ++i) {
        if (line[i] == ';') {
            // Skip spaces after semicolon
            size_t j = i + 1;
            while (j < len && line[j] == ' ') {
                ++j;
            }
            // Compare case-insensitively
            if (j + 12 <= len) {
                bool match = true;
                const char* marker = "LAYER_CHANGE";
                for (int k = 0; k < 12 && match; ++k) {
                    char c = line[j + k];
                    char m = marker[k];
                    // Case-insensitive compare
                    if (c != m && c != (m + 32) && (c - 32) != m) {
                        match = false;
                    }
                }
                if (match) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // anonymous namespace

bool GCodeLayerIndex::build_from_file(const std::string& filepath) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // Clear any previous data
    entries_.clear();
    stats_ = LayerIndexStats{};
    source_path_ = filepath;

    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::error("[LayerIndex] Failed to open file: {}", filepath);
        return false;
    }

    // Get file size
    stats_.total_bytes = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    spdlog::debug("[LayerIndex] Building index for {} ({} bytes)", filepath, stats_.total_bytes);

    // Reserve estimated capacity (assume ~100 layers for now)
    entries_.reserve(100);

    // Read line by line
    std::string line;
    line.reserve(256);

    float current_z = -std::numeric_limits<float>::infinity();
    uint64_t current_layer_start = 0;
    uint64_t current_offset = 0;
    uint16_t current_layer_lines = 0;
    bool use_layer_markers = false;
    bool pending_layer_start = false;
    bool first_layer_started = false;

    while (std::getline(file, line)) {
        size_t line_len = line.length();
        stats_.total_lines++;

        // Check for layer marker
        if (is_layer_marker(line.c_str(), line_len)) {
            use_layer_markers = true;
            pending_layer_start = true;
            // We'll start the new layer when we see the next Z move
        }

        // Extract filament color from metadata (only if not already found)
        // Only check comment lines in the header (first ~1000 lines)
        if (stats_.filament_color.empty() && stats_.total_lines < 1000) {
            std::string color;
            std::vector<std::string> palette;
            if (extract_filament_color(line.c_str(), line_len, color, palette)) {
                stats_.filament_color = color;
                stats_.filament_palette = std::move(palette);
                spdlog::debug("[LayerIndex] Found filament palette ({} entries, first={})",
                              stats_.filament_palette.size(), color);
            }
        }

        // Track the first standalone T-command — streaming mode parses each
        // layer with a fresh GCodeParser, so without this, segments after a
        // PRINT_START toolchange in the prologue render as T0 (#776 / black-fill
        // bug for prints sliced to a non-T0 tool).
        if (stats_.initial_tool_index < 0 && line_len >= 2 && line[0] == 'T') {
            size_t i = 1;
            while (i < line_len && line[i] >= '0' && line[i] <= '9') {
                ++i;
            }
            if (i > 1 && (i == line_len || line[i] == ' ' || line[i] == '\t' ||
                          line[i] == '\r' || line[i] == ';')) {
                try {
                    stats_.initial_tool_index = std::stoi(line.substr(1, i - 1));
                    spdlog::debug("[LayerIndex] Initial tool: T{}",
                                  stats_.initial_tool_index);
                } catch (...) {
                    // Malformed T-line — leave as -1 and keep scanning
                }
            }
        }

        // Check for movement commands
        if (is_movement_command(line.c_str(), line_len)) {
            float z;
            if (extract_z_param(line.c_str(), line_len, z)) {
                // Z change detected
                bool is_new_layer = false;

                if (use_layer_markers) {
                    // Use marker-based layer detection
                    if (pending_layer_start) {
                        is_new_layer = true;
                        pending_layer_start = false;
                    }
                } else {
                    // Use Z-change based layer detection
                    if (z > current_z + Z_EPSILON) {
                        is_new_layer = true;
                    }
                }

                if (is_new_layer) {
                    // Finalize previous layer if any
                    if (first_layer_started && current_layer_lines > 0) {
                        StreamingLayerEntry& last = entries_.back();
                        last.byte_length =
                            static_cast<uint32_t>(current_offset - current_layer_start);
                        last.line_count = current_layer_lines;
                    }

                    // Start new layer
                    StreamingLayerEntry entry{};
                    entry.file_offset = current_offset;
                    entry.z_height = z;
                    entry.byte_length = 0; // Will be filled when layer ends
                    entry.line_count = 0;  // Will be filled when layer ends
                    entry.flags = 0;
                    entries_.push_back(entry);

                    if (!first_layer_started) {
                        stats_.min_z = z;
                        first_layer_started = true;
                    }
                    stats_.max_z = z;

                    current_z = z;
                    current_layer_start = current_offset;
                    current_layer_lines = 0;
                }
            }

            // Track extrusion vs travel
            if (has_positive_extrusion(line.c_str(), line_len)) {
                stats_.extrusion_moves++;
            } else {
                stats_.travel_moves++;
            }
        }

        current_layer_lines++;
        // Account for line length + newline character
        current_offset += line_len + 1;
    }

    // Finalize last layer
    if (first_layer_started && !entries_.empty()) {
        StreamingLayerEntry& last = entries_.back();
        last.byte_length = static_cast<uint32_t>(stats_.total_bytes - current_layer_start);
        last.line_count = current_layer_lines;
    }

    stats_.total_layers = entries_.size();

    // If no filament color found in header, scan the file footer (OrcaSlicer puts metadata at end)
    if (stats_.filament_color.empty() && stats_.total_bytes > 0) {
        // Read last 32KB of file to find metadata
        size_t footer_size = std::min(stats_.total_bytes, size_t(32768));
        file.clear();
        file.seekg(-static_cast<std::streamoff>(footer_size), std::ios::end);

        while (std::getline(file, line)) {
            std::string color;
            std::vector<std::string> palette;
            if (extract_filament_color(line.c_str(), line.length(), color, palette)) {
                stats_.filament_color = color;
                stats_.filament_palette = std::move(palette);
                spdlog::debug("[LayerIndex] Found filament palette in footer ({} entries, first={})",
                              stats_.filament_palette.size(), color);
                break;
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.build_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    spdlog::info("[LayerIndex] Built index: {} layers, {} lines, Z=[{:.2f}, {:.2f}], {:.1f}ms",
                 stats_.total_layers, stats_.total_lines, stats_.min_z, stats_.max_z,
                 stats_.build_time_ms);

    spdlog::debug("[LayerIndex] Memory usage: {} bytes ({} bytes/layer)", memory_usage_bytes(),
                  entries_.empty() ? 0 : memory_usage_bytes() / entries_.size());

    return !entries_.empty();
}

StreamingLayerEntry GCodeLayerIndex::get_entry(size_t layer_index) const {
    if (layer_index < entries_.size()) {
        return entries_[layer_index];
    }
    // Return invalid entry
    return StreamingLayerEntry{0, 0, 0.0f, 0, 0};
}

int GCodeLayerIndex::find_layer_at_z(float z) const {
    if (entries_.empty()) {
        return -1;
    }

    // Binary search for closest layer
    auto it = std::lower_bound(
        entries_.begin(), entries_.end(), z,
        [](const StreamingLayerEntry& entry, float target_z) { return entry.z_height < target_z; });

    if (it == entries_.end()) {
        return static_cast<int>(entries_.size() - 1);
    }

    size_t idx = std::distance(entries_.begin(), it);

    // Check if previous layer is closer
    if (idx > 0) {
        float dist_curr = std::abs(it->z_height - z);
        float dist_prev = std::abs((it - 1)->z_height - z);
        if (dist_prev < dist_curr) {
            return static_cast<int>(idx - 1);
        }
    }

    return static_cast<int>(idx);
}

float GCodeLayerIndex::get_layer_z(size_t layer_index) const {
    if (layer_index < entries_.size()) {
        return entries_[layer_index].z_height;
    }
    return 0.0f;
}

} // namespace gcode
} // namespace helix
