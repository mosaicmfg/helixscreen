// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "subject_managed_panel.h"
#include "ui_observer_guard.h"

#include <lvgl.h>

#include <atomic>
#include <string>

namespace helix {

class MoonrakerClient;

/// Coarse pre-print phase. Forms the public contract of the tracker; XML and
/// UI bind on `print_phase` (int) to switch screens / show the right card.
///
/// Numeric values are stable — XML `bind_flag_if_eq ref_value="…"` references
/// them. Don't reorder.
enum class PrintPhase {
    IDLE = 0,          ///< No active print
    PREPARING = 1,     ///< START_PRINT entered, no sub-phase identified yet
    BED_MESH = 2,      ///< Bed mesh calibration in progress
    HEATING = 3,       ///< Heating bed/nozzle to first-layer temps
    FILAMENT_LOAD = 4, ///< CFS/IFS/AMS load sequence
    PURGE = 5,         ///< Flush / wipe before print
    PRINTING = 6,      ///< Real print gcode flowing
    PAUSED = 7,
    COMPLETE = 8,
    ERROR = 9,
    CANCELLED = 10,
};

/// Tracks coarse-grained pre-print phases (bed mesh, heating, filament load,
/// purge) so the UI can replace the generic "starting print" spinner with a
/// real progress card.
///
/// The tracker is **input-driven** — it consumes:
///   - `print_stats.state` / `print_duration` via existing PrinterState subjects
///   - The `notify_gcode_response` tag stream (`// [PRTOUCH_MOVE]`,
///     `// [PROBE_STEP_INFO]`, `// [G29_TIME]`, `// [box] …`, etc.)
///
/// On printers that don't emit the rich tag stream (Bambu, stock Klipper,
/// AD5M Forge), the tracker auto-degrades to IDLE → PREPARING → PRINTING
/// → COMPLETE / ERROR.
///
/// Thread-safety: subject mutations route through `helix::ui::queue_update`;
/// the gcode_response listener fires on the WebSocket thread.
///
/// Lifetime: singleton owned by the process, lives for the application
/// lifetime. Subject cleanup self-registers with `StaticSubjectRegistry`.
class PrintPhaseTracker {
  public:
    static PrintPhaseTracker& instance();

    /// Initialize subjects + register self-cleanup. Call after LVGL is up but
    /// before the discovery sequence completes (so XML can bind early).
    void init_subjects(bool register_xml = true);

    /// Tear down subjects (called by StaticSubjectRegistry pre-`lv_deinit`).
    void deinit_subjects();

    /// Subscribe to PrinterState's print_state_enum / print_duration subjects.
    /// Idempotent. Call after `PrinterState::init_subjects()`.
    void attach_observers();

    /// Register the `notify_gcode_response` listener on the given client.
    /// Must be called for every fresh MoonrakerClient (e.g. after reconnect).
    /// The handler name is "print_phase_tracker"; calling twice on the same
    /// client is harmless because register_method_callback is keyed by name.
    void attach_to_client(MoonrakerClient* client);

    /// Detach from a client (called on disconnect).
    void detach_from_client(MoonrakerClient* client);

    /// Reset all transient state (called on disconnect / new print).
    void reset();

    // ---- Subjects -----------------------------------------------------------

    /// Current PrintPhase value (int).
    lv_subject_t* get_phase_subject() {
        return &print_phase_;
    }

    /// Localized phase label (e.g. "Bed Mesh", "Heating", "Loading filament").
    lv_subject_t* get_phase_label_subject() {
        return &print_phase_label_;
    }

    /// Phase-specific detail string ("probe 482 / 625", "60.2 / 60 °C", etc).
    lv_subject_t* get_phase_detail_subject() {
        return &print_phase_detail_;
    }

    /// 0..1000 progress within the current phase. -1 = indeterminate.
    lv_subject_t* get_phase_progress_subject() {
        return &print_phase_progress_;
    }

    /// Estimated seconds remaining in the current phase. -1 = unknown.
    lv_subject_t* get_phase_eta_subject() {
        return &print_phase_eta_seconds_;
    }

  private:
    PrintPhaseTracker() = default;
    ~PrintPhaseTracker() = default;
    PrintPhaseTracker(const PrintPhaseTracker&) = delete;
    PrintPhaseTracker& operator=(const PrintPhaseTracker&) = delete;

    friend class PrintPhaseTrackerTestAccess;

    // ---- Phase machine (called on UI thread) --------------------------------

    void set_phase(PrintPhase phase);
    void on_print_job_state(int state_enum);
    void on_print_duration(int duration_seconds);

    // ---- gcode_response parsing ---------------------------------------------
    //
    // Entry point fires on the WebSocket thread; it queues a single update so
    // the rest of the parser pipeline runs on the UI thread. All `try_match_*`
    // members are UI-thread-only and may freely mutate member state (no mutex
    // needed — the UpdateQueue serializes everything).
    //
    // Each matcher returns `true` if it consumed the line; the dispatcher
    // short-circuits on first match. New tag families = new matcher.
    //
    // K2-/CFS-specific matchers carry a "[K2/CFS]" comment so it's obvious
    // where to fork when adding K1, Bambu, or RatOS variants later.

    void process_gcode_line(const std::string& line); // entry, any thread
    void dispatch_line_ui(const std::string& line);   // UI thread

    bool try_match_mesh_size(const std::string& line);
    bool try_match_probe_step(const std::string& line);
    bool try_match_mesh_begin(const std::string& line);
    bool try_match_g29_time(const std::string& line);
    bool try_match_filament_load(const std::string& line);
    bool try_match_purge_percent(const std::string& line);
    bool try_match_heating_hint(const std::string& line);

    /// Mirror current phase/detail/progress into PrinterState's legacy
    /// `print_start_*` subjects so the existing `preparing_overlay` UI in
    /// `print_status_panel.xml` shows the K2-rich progression. Called after
    /// every phase transition or progress mutation. PrintStartCollector also
    /// writes to those subjects on universal printers; on K2 the tracker's
    /// signals fire more often and dominate. See plan in
    /// `.claude/scratchpad/2026-05-05-pre-print-phase2-and-cfs-rev-eng.md`.
    void publish_legacy_print_start_state();

    // ---- State (UI-thread members) ------------------------------------------
    //
    // All mutated only on the UI thread (post queue_update). No locking needed.

    SubjectManager subjects_;
    bool subjects_initialized_ = false;
    bool observers_attached_ = false;

    lv_subject_t print_phase_{};
    lv_subject_t print_phase_label_{};
    lv_subject_t print_phase_detail_{};
    lv_subject_t print_phase_progress_{};
    lv_subject_t print_phase_eta_seconds_{};

    char print_phase_label_buf_[48]{};
    char print_phase_detail_buf_[64]{};

    ObserverGuard print_state_observer_;
    ObserverGuard print_duration_observer_;

    PrintPhase current_phase_ = PrintPhase::IDLE;

    // Bed-mesh tracking
    int mesh_probes_seen_ = 0;
    int mesh_probes_total_ = 0;          ///< From "Mesh X,Y: 25,25"; 0 = unknown
    float mesh_seconds_per_probe_ = 0.0f; ///< From "[G29_TIME]Time spent at each point: …"
    bool mesh_active_ = false;

    // Purge tracking — most-recent flush percent (0..100)
    int purge_percent_ = -1;
};

/// Convert a PrintPhase to a stable ASCII identifier for logging. Not for UI.
const char* print_phase_to_string(PrintPhase phase);

} // namespace helix
