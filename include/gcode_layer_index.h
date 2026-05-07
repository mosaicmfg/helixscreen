// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace helix {
namespace gcode {

/**
 * @brief Compact layer entry for streaming G-code access
 *
 * Instead of storing all segment data in memory, this stores just the
 * file byte offsets needed to load layers on-demand. Each entry is
 * ~24 bytes vs ~80KB for a full layer with segment data.
 *
 * This enables viewing 10MB+ G-code files on memory-constrained devices
 * like AD5M (47MB RAM) by loading only the layers currently being viewed.
 */
struct StreamingLayerEntry {
    uint64_t file_offset; ///< Byte offset in file where layer starts
    uint32_t byte_length; ///< Number of bytes in this layer
    float z_height;       ///< Z coordinate of this layer (mm)
    uint16_t line_count;  ///< Number of G-code lines in this layer
    uint16_t flags;       ///< Reserved for future use (e.g., has_extrusion)

    /// Check if this entry is valid (has been populated)
    bool is_valid() const {
        return byte_length > 0;
    }
};

/**
 * @brief Statistics collected during index building
 */
struct LayerIndexStats {
    size_t total_layers{0};     ///< Number of layers found
    size_t total_lines{0};      ///< Total G-code lines processed
    size_t total_bytes{0};      ///< Total file size
    float min_z{0.0f};          ///< Minimum Z height
    float max_z{0.0f};          ///< Maximum Z height
    size_t extrusion_moves{0};  ///< Count of G1 E+ moves
    size_t travel_moves{0};     ///< Count of G0/G1 without extrusion
    double build_time_ms{0.0};  ///< Time to build index
    std::string filament_color; ///< Filament color hex (e.g., "#26A69A") from metadata
    int initial_tool_index{-1}; ///< First T-command seen in the file (-1 = none)
};

/**
 * @brief Layer index for streaming G-code access
 *
 * Provides random access to layers without loading the entire file.
 * Built with a single-pass scan of the file, recording byte offsets
 * for each layer boundary.
 *
 * Usage:
 * @code
 *   GCodeLayerIndex index;
 *   if (index.build_from_file("model.gcode")) {
 *       // Get layer 50's offset
 *       auto entry = index.get_entry(50);
 *       // Read just that layer's bytes from file
 *       // ... seek to entry.file_offset, read entry.byte_length bytes
 *   }
 * @endcode
 *
 * Memory usage: ~24 bytes × layer_count (e.g., 1000 layers = 24KB)
 */
class GCodeLayerIndex {
  public:
    GCodeLayerIndex() = default;
    ~GCodeLayerIndex() = default;

    // Non-copyable but moveable
    GCodeLayerIndex(const GCodeLayerIndex&) = delete;
    GCodeLayerIndex& operator=(const GCodeLayerIndex&) = delete;
    GCodeLayerIndex(GCodeLayerIndex&&) = default;
    GCodeLayerIndex& operator=(GCodeLayerIndex&&) = default;

    /**
     * @brief Build index from a G-code file
     *
     * Single-pass scan that identifies layer boundaries by detecting
     * Z-axis changes or ;LAYER_CHANGE markers. Records byte offset,
     * length, and line count for each layer.
     *
     * @param filepath Path to G-code file
     * @return true if successful, false on error
     */
    bool build_from_file(const std::string& filepath);

    /**
     * @brief Get entry for a specific layer
     *
     * @param layer_index Zero-based layer index
     * @return Layer entry, or invalid entry if out of range
     */
    StreamingLayerEntry get_entry(size_t layer_index) const;

    /**
     * @brief Get total number of layers
     * @return Layer count
     */
    size_t get_layer_count() const {
        return entries_.size();
    }

    /**
     * @brief Get file size that was indexed
     * @return File size in bytes
     */
    size_t get_file_size() const {
        return stats_.total_bytes;
    }

    /**
     * @brief Get index building statistics
     * @return Statistics from build process
     */
    const LayerIndexStats& get_stats() const {
        return stats_;
    }

    /**
     * @brief Check if index is populated
     * @return true if index has been built successfully
     */
    bool is_valid() const {
        return !entries_.empty();
    }

    /**
     * @brief Find layer index closest to Z height
     *
     * @param z Z coordinate to search for
     * @return Layer index (0-based), or -1 if no layers
     */
    int find_layer_at_z(float z) const;

    /**
     * @brief Get Z height for a layer
     *
     * @param layer_index Zero-based layer index
     * @return Z height in mm, or 0.0 if out of range
     */
    float get_layer_z(size_t layer_index) const;

    /**
     * @brief Get memory usage of this index
     * @return Approximate bytes used
     */
    size_t memory_usage_bytes() const {
        return sizeof(*this) + entries_.capacity() * sizeof(StreamingLayerEntry);
    }

    /**
     * @brief Clear the index to free memory
     */
    void clear() {
        entries_.clear();
        entries_.shrink_to_fit();
        stats_ = LayerIndexStats{};
        source_path_.clear();
    }

    /**
     * @brief Get source file path
     * @return Path used in build_from_file()
     */
    const std::string& get_source_path() const {
        return source_path_;
    }

  private:
    std::vector<StreamingLayerEntry> entries_;
    LayerIndexStats stats_;
    std::string source_path_;
};

} // namespace gcode
} // namespace helix
