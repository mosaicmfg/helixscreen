// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

#include "lvgl.h"

namespace helix::ui {

/// Bag of widget pointers that make up the bypass-spool overlay: a small card
/// containing the spool icon, with a material label above and "Bypass" label
/// below — matching how lane spools display their material on the AMS path
/// canvases. Shared by both the Multi-Filament panel (single-AMS) and the
/// Multi-Filament Overview panel (multi-AMS).
///
/// The widgets are positioned as FLOATING siblings of a path-canvas, with the
/// owning panel responsible for placing the box at the canvas-computed bypass
/// coordinate. The canvas itself only draws the connecting lines.
struct BypassSpoolWidgets {
    lv_obj_t* box = nullptr;            ///< Card container with click target
    lv_obj_t* spool_canvas = nullptr;   ///< The spool icon inside the box
    lv_obj_t* bypass_label = nullptr;   ///< "Bypass" text below the box
    lv_obj_t* material_label = nullptr; ///< Material name above the box (hidden if empty)

    // Cached state for change-detection (avoids spurious invalidates on every
    // panel refresh). Not part of the public API surface — read via setters.
    uint32_t cached_color_rgb = 0;
    bool cached_has_spool = false;
    char cached_material[32] = {};
    int32_t cached_bypass_label_w = 0; ///< "Bypass" is constant — measure once

    [[nodiscard]] bool valid() const {
        return box != nullptr;
    }
};

/// Create the bypass spool overlay widgets as floating children of `parent`.
/// Clicks dispatch to the caller-supplied `on_click` LVGL event handler with
/// `user_data` carried via `lv_event_get_user_data()` — keeps the helper out
/// of the user-data slot of the spool widget itself.
BypassSpoolWidgets bypass_spool_create(lv_obj_t* parent, lv_event_cb_t on_click,
                                       void* user_data);

/// Destroy all owned widgets and zero the struct. Main-thread synchronous —
/// callers from within queued/observer callbacks must marshal first.
void bypass_spool_destroy(BypassSpoolWidgets& w);

/// Update the spool icon color (RGB 0xRRGGBB). No-op when unchanged.
void bypass_spool_set_color(BypassSpoolWidgets& w, uint32_t color_rgb);

/// Show filled spool when true, hollow outline when false. No-op when unchanged.
void bypass_spool_set_has_spool(BypassSpoolWidgets& w, bool has_spool);

/// Set the material label text above the spool. Empty string hides the label.
/// No-op when text is unchanged.
void bypass_spool_set_material(BypassSpoolWidgets& w, const char* material);

/// Position the spool box so its center sits at (`cx`, `cy`) in parent-relative
/// coordinates. The material label is placed above, the "Bypass" label below.
void bypass_spool_set_position(BypassSpoolWidgets& w, int32_t cx, int32_t cy);

} // namespace helix::ui
