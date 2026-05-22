// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "lvgl/lvgl.h"
#include <string>
#include <vector>

namespace helix {
namespace ui {

/**
 * @brief Performance overlay: host CPU/memory + per-MCU load rows.
 *
 * Singleton. Call create(parent) once; subsequent calls return the cached root.
 * MCU rows are rebuilt whenever perf_mcu_names changes. The overlay registers a
 * string observer on perf_mcu_names to drive that rebuild.
 *
 * perf_mcu_names is a STATIC subject (registered in PerformanceState::init_subjects
 * and never destroyed). Per [L077] only DYNAMIC subjects need a SubjectLifetime
 * token, so no SubjectLifetime member is needed here.
 */
class UiOverlayPerformance {
  public:
    static UiOverlayPerformance& instance();

    lv_obj_t* create(lv_obj_t* parent);
    lv_obj_t* root() { return root_; }

  private:
    UiOverlayPerformance() = default;

    void rebuild_mcu_rows();

    lv_obj_t* root_     = nullptr;
    lv_obj_t* mcu_card_ = nullptr;

    // Observer on perf_mcu_names (static subject — no SubjectLifetime needed [L077]).
    ObserverGuard mcu_names_observer_;
};

} // namespace ui
} // namespace helix
