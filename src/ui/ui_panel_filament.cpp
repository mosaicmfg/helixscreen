// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_filament.h"

#include "ui_ams_edit_modal.h"
#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_nav_manager.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"
#include "ui_spool_canvas.h"
#include "ui_subject_registry.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "filament_database.h"
#include "filament_sensor_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_executor.h"
#include "macro_param_cache.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "post_op_cooldown_manager.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "temperature_service.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <memory>

using namespace helix;

// Preset material names (indexed by material ID: 0=PLA, 1=PETG, 2=ABS, 3=TPU)
// Temperatures looked up from filament_database.h
static constexpr const char* PRESET_MATERIAL_NAMES[] = {"PLA", "PETG", "ABS", "TPU"};
static constexpr int PRESET_COUNT = 4;

// Format string for safety warning (used in constructor and set_limits)
static constexpr const char* SAFETY_WARNING_FMT = "Heat to at least %d°C for filament operations";

using helix::ui::observe_int_async;
using helix::ui::observe_int_sync;
using helix::ui::temperature::centi_to_degrees;
using helix::ui::temperature::format_target_or_off;
using helix::ui::temperature::get_heating_state_color;

// Shared MacroParamModal for filament operations that accept parameters
helix::MacroParamModal& get_filament_param_modal() {
    static helix::MacroParamModal modal;
    return modal;
}

// ============================================================================
// CONSTRUCTOR
// ============================================================================

FilamentPanel::FilamentPanel(PrinterState& printer_state, MoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    // Initialize buffer contents with default values
    std::snprintf(temp_display_buf_, sizeof(temp_display_buf_), "%d / %d°C", nozzle_current_,
                  nozzle_target_);
    std::snprintf(status_buf_, sizeof(status_buf_), "%s", "Select material to begin");
    std::snprintf(warning_temps_buf_, sizeof(warning_temps_buf_), "Current: %d°C | Target: %d°C",
                  nozzle_current_, nozzle_target_);
    std::snprintf(safety_warning_text_buf_, sizeof(safety_warning_text_buf_), SAFETY_WARNING_FMT,
                  min_extrude_temp_);
    format_target_or_off(0, material_nozzle_buf_, sizeof(material_nozzle_buf_));
    format_target_or_off(0, material_bed_buf_, sizeof(material_bed_buf_));
    std::snprintf(nozzle_current_buf_, sizeof(nozzle_current_buf_), "%d°C", nozzle_current_);
    format_target_or_off(0, nozzle_target_buf_, sizeof(nozzle_target_buf_));
    std::snprintf(bed_current_buf_, sizeof(bed_current_buf_), "%d°C", bed_current_);
    format_target_or_off(0, bed_target_buf_, sizeof(bed_target_buf_));

    // Register XML event callbacks
    register_xml_callbacks({
        {"filament_manage_slots_cb", on_manage_slots_clicked},
        {"on_filament_load", on_load_clicked},
        {"on_filament_unload", on_unload_clicked},
        {"on_filament_extrude", on_extrude_clicked},
        {"on_filament_purge", on_purge_clicked},
        {"on_filament_retract", on_retract_clicked},
        // Material preset buttons
        {"on_filament_preset_pla", on_preset_pla_clicked},
        {"on_filament_preset_petg", on_preset_petg_clicked},
        {"on_filament_preset_abs", on_preset_abs_clicked},
        {"on_filament_preset_tpu", on_preset_tpu_clicked},
        {"on_filament_preset_spool", on_preset_spool_clicked},
        // Temperature tap targets
        {"on_filament_nozzle_temp_tap", on_nozzle_temp_tap_clicked},
        {"on_filament_bed_temp_tap", on_bed_temp_tap_clicked},
        {"on_filament_nozzle_target_tap", on_nozzle_target_tap_clicked},
        {"on_filament_bed_target_tap", on_bed_target_tap_clicked},
        {"on_filament_chamber_target_tap", on_filament_chamber_target_tap},
        // Purge amount buttons
        {"on_filament_purge_5mm", on_purge_5mm_clicked},
        {"on_filament_purge_10mm", on_purge_10mm_clicked},
        {"on_filament_purge_25mm", on_purge_25mm_clicked},
        // Cooldown button
        {"on_filament_cooldown", on_cooldown_clicked},
        // Extruder selector dropdown
        {"on_extruder_dropdown_changed", on_extruder_dropdown_changed},
        // External spool edit
        {"on_external_spool_edit", on_external_spool_edit_clicked},
    });

    // Subscribe to PrinterState temperatures using bundle pattern
    // NOTE: Observers must defer UI updates via ui_async_call to avoid render-phase assertions
    // [L029]
    temp_observers_.setup_async(
        this, printer_state_,
        [](FilamentPanel* self, int raw) { self->nozzle_current_ = centi_to_degrees(raw); },
        [](FilamentPanel* self, int raw) { self->nozzle_target_ = centi_to_degrees(raw); },
        [](FilamentPanel* self, int raw) { self->bed_current_ = centi_to_degrees(raw); },
        [](FilamentPanel* self, int raw) { self->bed_target_ = centi_to_degrees(raw); },
        [](FilamentPanel* self) { self->update_all_temps(); });

    // Subscribe to chamber temperature (optional - only if printer has chamber)
    // Note: We check are_subjects_initialized() because observers may fire immediately
    // upon registration, but subjects aren't initialized until init_subjects() is called.
    chamber_temp_observer_ = observe_int_sync<FilamentPanel>(
        printer_state_.get_chamber_temp_subject(), this, [](FilamentPanel* self, int raw) {
            self->chamber_current_ = centi_to_degrees(raw);
            if (self->are_subjects_initialized()) {
                self->update_chamber_temp_display();
                self->update_status();
            }
        });
    chamber_target_observer_ = observe_int_sync<FilamentPanel>(
        printer_state_.get_chamber_target_subject(), this, [](FilamentPanel* self, int raw) {
            self->chamber_target_ = raw; // Store centidegrees (matches PrinterState format)
            if (self->are_subjects_initialized()) {
                self->update_chamber_temp_display();
            }
        });

    // Subscribe to active tool changes for dynamic nozzle label + dropdown sync
    active_tool_observer_ = observe_int_sync<FilamentPanel>(
        helix::ToolState::instance().get_active_tool_subject(), this,
        [](FilamentPanel* self, int tool_idx) {
            self->update_nozzle_label();
            if (self->extruder_dropdown_ && tool_idx >= 0) {
                lv_dropdown_set_selected(self->extruder_dropdown_, static_cast<uint32_t>(tool_idx));
            }
        });
    update_nozzle_label();

    // Note: Chamber temperature display is initialized by observer callbacks
    // and refresh_all_displays() on panel activation.
    // We don't call update_chamber_temp_display() here because subjects
    // aren't initialized yet (init_subjects() is called after construction).
}

FilamentPanel::~FilamentPanel() {
    deinit_subjects();

    // Guard destructor handles timer cleanup automatically

    // Clean up warning dialogs if open (prevents memory leak and use-after-free)
    if (lv_is_initialized()) {
        if (load_warning_dialog_) {
            helix::ui::modal_hide(load_warning_dialog_);
            load_warning_dialog_ = nullptr;
        }
        if (unload_warning_dialog_) {
            helix::ui::modal_hide(unload_warning_dialog_);
            unload_warning_dialog_ = nullptr;
        }
    }
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void FilamentPanel::init_subjects() {
    init_subjects_guarded([this]() {
        // Initialize subjects with default values
        UI_MANAGED_SUBJECT_STRING(temp_display_subject_, temp_display_buf_, temp_display_buf_,
                                  "filament_temp_display", subjects_);
        UI_MANAGED_SUBJECT_STRING(status_subject_, status_buf_, status_buf_, "filament_status",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(material_selected_subject_, -1, "filament_material_selected",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(extrusion_allowed_subject_, 0, "filament_extrusion_allowed",
                               subjects_); // false (cold at start)
        UI_MANAGED_SUBJECT_INT(safety_warning_visible_subject_, 1,
                               "filament_safety_warning_visible",
                               subjects_); // true (cold at start)
        UI_MANAGED_SUBJECT_STRING(warning_temps_subject_, warning_temps_buf_, warning_temps_buf_,
                                  "filament_warning_temps", subjects_);
        UI_MANAGED_SUBJECT_STRING(safety_warning_text_subject_, safety_warning_text_buf_,
                                  safety_warning_text_buf_, "filament_safety_warning_text",
                                  subjects_);

        // Material temperature display subjects (for right side preset displays)
        UI_MANAGED_SUBJECT_STRING(material_nozzle_temp_subject_, material_nozzle_buf_,
                                  material_nozzle_buf_, "filament_material_nozzle_temp", subjects_);
        UI_MANAGED_SUBJECT_STRING(material_bed_temp_subject_, material_bed_buf_, material_bed_buf_,
                                  "filament_material_bed_temp", subjects_);

        // Nozzle label (dynamic for multi-tool)
        UI_MANAGED_SUBJECT_STRING(nozzle_label_subject_, nozzle_label_buf_, "Nozzle",
                                  "filament_nozzle_label", subjects_);

        // Left card temperature subjects (current and target for nozzle/bed)
        UI_MANAGED_SUBJECT_STRING(nozzle_current_subject_, nozzle_current_buf_, nozzle_current_buf_,
                                  "filament_nozzle_current", subjects_);
        UI_MANAGED_SUBJECT_STRING(nozzle_target_subject_, nozzle_target_buf_, nozzle_target_buf_,
                                  "filament_nozzle_target", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_current_subject_, bed_current_buf_, bed_current_buf_,
                                  "filament_bed_current", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_target_subject_, bed_target_buf_, bed_target_buf_,
                                  "filament_bed_target", subjects_);
        UI_MANAGED_SUBJECT_STRING(chamber_current_subject_, chamber_current_buf_,
                                  chamber_current_buf_, "filament_chamber_current", subjects_);
        UI_MANAGED_SUBJECT_STRING(chamber_target_subject_, chamber_target_buf_, chamber_target_buf_,
                                  "filament_chamber_target", subjects_);

        // Operation in progress subject (for disabling buttons during filament ops)
        operation_guard_.init_subject("filament_operation_in_progress", subjects_);

        // Cooldown button visibility (1 when nozzle or bed target > 0)
        UI_MANAGED_SUBJECT_INT(nozzle_heating_subject_, 0, "filament_nozzle_heating", subjects_);

        // Purge amount button active states (boolean: 0=inactive, 1=active)
        // Using separate subjects because bind_style doesn't work well with multiple ref_values
        UI_MANAGED_SUBJECT_INT(purge_5mm_active_subject_, 0, "filament_purge_5mm_active",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(purge_10mm_active_subject_, 1, "filament_purge_10mm_active",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(purge_25mm_active_subject_, 0, "filament_purge_25mm_active",
                               subjects_);

        // Preset button temperature label subjects (populated from filament DB in setup)
        static constexpr const char* preset_subject_names[] = {
            "filament_preset_pla_temps", "filament_preset_petg_temps", "filament_preset_abs_temps",
            "filament_preset_tpu_temps"};
        for (int i = 0; i < PRESET_COUNT; i++) {
            preset_temps_bufs_[i][0] = '\0';
            UI_MANAGED_SUBJECT_STRING(preset_temps_subjects_[i], preset_temps_bufs_[i],
                                      preset_temps_bufs_[i], preset_subject_names[i], subjects_);
        }

        // Card title subject (dynamic: "Multi-Filament" or "External Spool")
        std::strncpy(card_title_buf_, lv_tr("Multi-Filament"), sizeof(card_title_buf_) - 1);
        UI_MANAGED_SUBJECT_STRING(card_title_subject_, card_title_buf_, card_title_buf_,
                                  "filament_card_title", subjects_);

        spdlog::debug("[{}] temp={}/{}°C, material={}", get_name(), nozzle_current_, nozzle_target_,
                      selected_material_);
    });
}

void FilamentPanel::deinit_subjects() {
    // Cancel any pending preheat without notification (panel is being torn down)
    if (pending_preheat_op_ != PreheatOp::NONE) {
        pending_preheat_op_ = PreheatOp::NONE;
        pending_preheat_target_ = 0;
        // Don't schedule delayed cooldown during teardown — just cool down immediately
        if (prior_nozzle_target_ == 0 && api_) {
            api_->set_temperature(
                printer_state_.active_extruder_name(), 0, []() {},
                [](const MoonrakerError& /*err*/) {});
        }
        prior_nozzle_target_ = 0;
    }
    external_spool_observer_.reset();
    temp_observers_.clear();
    deinit_subjects_base(subjects_);
}

void FilamentPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    // Filament macros now resolved via StandardMacros singleton (auto-detected or user-configured)
    spdlog::debug("[{}] Setting up (events handled declaratively via XML)", get_name());

    // Find preset buttons (for visual state updates)
    const char* preset_names[] = {"preset_pla", "preset_petg", "preset_abs", "preset_tpu"};
    for (int i = 0; i < 4; i++) {
        preset_buttons_[i] = lv_obj_find_by_name(panel_, preset_names[i]);
    }

    // Action buttons (btn_load, btn_unload, btn_purge) - disabled state managed by XML bindings

    // Find safety warning card
    safety_warning_ = lv_obj_find_by_name(panel_, "safety_warning");

    // Find status icon for dynamic updates
    status_icon_ = lv_obj_find_by_name(panel_, "status_icon");

    // Find temperature labels for color updates
    nozzle_current_label_ = lv_obj_find_by_name(panel_, "nozzle_current_temp");
    bed_current_label_ = lv_obj_find_by_name(panel_, "bed_current_temp");
    chamber_current_label_ = lv_obj_find_by_name(panel_, "chamber_current_temp");

    // Find temp graph for dynamic sizing when bottom card changes
    temp_graph_card_ = lv_obj_find_by_name(panel_, "temp_graph_card");

    // Find spool card widgets (serves both Multi-Filament and External Spool modes)
    spool_card_ = lv_obj_find_by_name(panel_, "spool_card");
    spool_card_header_row_ = lv_obj_find_by_name(panel_, "spool_card_header_row");
    extruder_selector_group_ = lv_obj_find_by_name(panel_, "extruder_selector_group");
    extruder_dropdown_ = lv_obj_find_by_name(panel_, "extruder_dropdown");
    btn_manage_slots_ = lv_obj_find_by_name(panel_, "btn_manage_slots");
    ams_manage_row_ = lv_obj_find_by_name(panel_, "ams_manage_row");

    // Find external spool row widgets
    external_spool_row_ = lv_obj_find_by_name(panel_, "external_spool_row");
    external_spool_container_ = lv_obj_find_by_name(panel_, "external_spool_container");
    external_spool_material_label_ = lv_obj_find_by_name(panel_, "external_spool_material_label");
    external_spool_color_label_ = lv_obj_find_by_name(panel_, "external_spool_color_label");

    // Find spool preset widgets
    spool_preset_row_ = lv_obj_find_by_name(panel_, "spool_preset_row");
    spool_preset_button_ = lv_obj_find_by_name(panel_, "preset_spool");
    spool_preset_label_ = lv_obj_find_by_name(panel_, "spool_preset_label");
    spool_preset_temps_ = lv_obj_find_by_name(panel_, "spool_preset_temps");

    // Setup external spool display (creates canvas, wires observer)
    setup_external_spool_display();

    // Setup spool preset button (show if active material doesn't match standard presets)
    update_spool_preset();

    // Populate extruder dropdown and set card visibility
    populate_extruder_dropdown();
    update_multi_filament_card_visibility();

    // Rebuild dropdown if tool list changes
    tools_version_observer_ =
        observe_int_sync<FilamentPanel>(helix::ToolState::instance().get_tools_version_subject(),
                                        this, [](FilamentPanel* self, int) {
                                            self->populate_extruder_dropdown();
                                            self->update_multi_filament_card_visibility();
                                        });

    // Subscribe to AMS type to update card row visibility. Graph vs spool
    // sizing is owned by apply_left_column_sizing() (called from
    // update_multi_filament_card_visibility). At MICRO/TINY with no AMS the
    // spool card has almost no content, so the graph becomes the flex filler.
    ams_type_observer_ = observe_int_sync<FilamentPanel>(
        AmsState::instance().get_ams_type_subject(),
        this,
        [](FilamentPanel* self, int /*ams_type*/) {
            self->update_multi_filament_card_visibility();
        });


    // End operation guard when AMS action returns to idle (load/unload complete)
    ams_action_observer_ = observe_int_sync<FilamentPanel>(
        AmsState::instance().get_ams_action_subject(), this, [](FilamentPanel* self, int action) {
            if (action == static_cast<int>(AmsAction::IDLE) && self->operation_guard_.is_active()) {
                spdlog::debug("[{}] AMS action returned to idle, ending operation guard",
                              self->get_name());
                self->operation_guard_.end();
            }
        });

    // Populate preset button temperature labels from filament database
    update_preset_button_temps();

    // Initialize visual state
    update_preset_buttons_visual();
    update_temp_display();
    update_left_card_temps();
    update_material_temp_display();
    update_status();
    update_status_icon_for_state();
    update_warning_text();
    update_safety_state();

    // Trigger initial purge button selection (notifies bind_style observers)
    handle_purge_amount_select(purge_amount_);

    // Setup combined temperature graph if TemperatureService is available
    if (temp_control_panel_) {
        lv_obj_t* graph_container = lv_obj_find_by_name(panel_, "temp_graph_container");
        if (graph_container) {
            temp_control_panel_->setup_mini_combined_graph(graph_container);
            spdlog::debug("[{}] Temperature graph initialized", get_name());
        } else {
            spdlog::warn("[{}] temp_graph_container not found in XML", get_name());
        }
    }

    // Make the graph card clickable to open the unified temp graph overlay
    if (temp_graph_card_) {
        lv_obj_add_flag(temp_graph_card_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            temp_graph_card_,
            [](lv_event_t* e) {
                auto* self = static_cast<FilamentPanel*>(lv_event_get_user_data(e));
                if (self) {
                    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::GraphOnly,
                                                         self->parent_screen_);
                }
            },
            LV_EVENT_CLICKED, this);
    }

    // AMS mini status widget is now created declaratively via XML <ams_mini_status/>

    spdlog::debug("[{}] Setup complete!", get_name());
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

void FilamentPanel::update_preset_button_temps() {
    for (int i = 0; i < PRESET_COUNT; i++) {
        auto mat = filament::find_material(PRESET_MATERIAL_NAMES[i]);
        if (mat) {
            std::snprintf(preset_temps_bufs_[i], sizeof(preset_temps_bufs_[i]), "%d°C / %d°C",
                          mat->nozzle_recommended(), mat->bed_temp);
        } else {
            std::snprintf(preset_temps_bufs_[i], sizeof(preset_temps_bufs_[i]), "---");
        }
        lv_subject_copy_string(&preset_temps_subjects_[i], preset_temps_bufs_[i]);
    }
}

void FilamentPanel::update_temp_display() {
    std::snprintf(temp_display_buf_, sizeof(temp_display_buf_), "%d / %d°C", nozzle_current_,
                  nozzle_target_);
    lv_subject_copy_string(&temp_display_subject_, temp_display_buf_);
}

void FilamentPanel::update_status_icon(const char* icon_name, const char* variant) {
    if (!status_icon_)
        return;

    // Update icon imperatively using ui_icon API
    ui_icon_set_source(status_icon_, icon_name);
    ui_icon_set_variant(status_icon_, variant);
}

void FilamentPanel::update_status() {
    const char* status_msg;

    // First check if nozzle is ready for extrusion (highest priority for filament operations)
    if (helix::ui::temperature::is_extrusion_safe(nozzle_current_, min_extrude_temp_)) {
        // Hot enough - ready to load
        status_msg = "Ready to load";
        update_status_icon("check", "success");
    } else if (nozzle_target_ >= min_extrude_temp_) {
        // Nozzle heating in progress — show current AND target so the user can
        // see ramp progress (#8: previously only target was visible, leaving
        // the user staring at a static "Heating to 230°C..." for several
        // minutes with no sign of life from the firmware).
        std::snprintf(status_buf_, sizeof(status_buf_), lv_tr("Heating: %d / %d°C"),
                      nozzle_current_, nozzle_target_);
        lv_subject_copy_string(&status_subject_, status_buf_);
        update_status_icon("flash", "warning");
        return; // Already updated, exit early
    } else if (chamber_target_ > 0 && chamber_current_ < centi_to_degrees(chamber_target_) - 5) {
        // Chamber is heating (show only if nozzle is cold)
        std::snprintf(status_buf_, sizeof(status_buf_), lv_tr("Chamber heating to %d°C..."),
                      centi_to_degrees(chamber_target_));
        lv_subject_copy_string(&status_subject_, status_buf_);
        update_status_icon("fire", "warning");
        return;
    } else if (chamber_target_ > 0 && chamber_current_ >= centi_to_degrees(chamber_target_) - 5 &&
               chamber_current_ <= centi_to_degrees(chamber_target_) + 2) {
        // Chamber at target (show only if nozzle is cold)
        std::snprintf(status_buf_, sizeof(status_buf_), lv_tr("Chamber at %d°C"),
                      centi_to_degrees(chamber_target_));
        lv_subject_copy_string(&status_subject_, status_buf_);
        update_status_icon("check", "success");
        return;
    } else {
        // Cold - needs material selection
        status_msg = "Select material to begin";
        update_status_icon("cooldown", "secondary");
    }

    lv_subject_copy_string(&status_subject_, status_msg);
}

void FilamentPanel::update_warning_text() {
    std::snprintf(warning_temps_buf_, sizeof(warning_temps_buf_), "Current: %d°C | Target: %d°C",
                  nozzle_current_, nozzle_target_);
    lv_subject_copy_string(&warning_temps_subject_, warning_temps_buf_);
}

void FilamentPanel::update_safety_state() {
    bool allowed = helix::ui::temperature::is_extrusion_safe(nozzle_current_, min_extrude_temp_);

    // Hide the safety warning (and enable buttons) if we have a known spool material,
    // since the load/unload handlers will auto-preheat to the correct temperature.
    bool has_known_spool = has_active_spool_material();
    bool show_warning = !allowed && !has_known_spool;

    lv_subject_set_int(&extrusion_allowed_subject_, allowed ? 1 : 0);
    lv_subject_set_int(&safety_warning_visible_subject_, show_warning ? 1 : 0);

    spdlog::trace("[{}] Safety state updated: allowed={}, known_spool={} (temp={}°C)", get_name(),
                  allowed, has_known_spool, nozzle_current_);
}

void FilamentPanel::update_preset_buttons_visual() {
    for (int i = 0; i < 4; i++) {
        if (!preset_buttons_[i])
            continue;

        if (i == selected_material_) {
            lv_obj_add_state(preset_buttons_[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(preset_buttons_[i], LV_STATE_CHECKED);
        }
    }
    // Deselect spool preset when a standard preset is selected
    if (spool_preset_button_ && selected_material_ >= 0) {
        lv_obj_remove_state(spool_preset_button_, LV_STATE_CHECKED);
    }
}

void FilamentPanel::check_and_auto_select_preset() {
    // Check if both nozzle and bed targets match any preset
    int matching_preset = -1;
    for (int i = 0; i < PRESET_COUNT; i++) {
        auto mat = filament::find_material(PRESET_MATERIAL_NAMES[i]);
        if (mat && nozzle_target_ == mat->nozzle_recommended() && bed_target_ == mat->bed_temp) {
            matching_preset = i;
            break;
        }
    }

    // Only update if selection changed
    if (matching_preset != selected_material_) {
        selected_material_ = matching_preset;
        lv_subject_set_int(&material_selected_subject_, selected_material_);
        update_preset_buttons_visual();

        if (matching_preset >= 0) {
            spdlog::debug("[{}] Auto-selected preset: {} (nozzle={}°C, bed={}°C)", get_name(),
                          PRESET_MATERIAL_NAMES[matching_preset], nozzle_target_, bed_target_);
        } else {
            spdlog::debug("[{}] No matching preset for nozzle={}°C, bed={}°C", get_name(),
                          nozzle_target_, bed_target_);
        }
    }
}

void FilamentPanel::update_nozzle_label() {
    auto label = helix::ToolState::instance().nozzle_label();
    std::snprintf(nozzle_label_buf_, sizeof(nozzle_label_buf_), "%s", label.c_str());
    if (subjects_initialized_) {
        lv_subject_copy_string(&nozzle_label_subject_, nozzle_label_buf_);
    }
}

void FilamentPanel::update_all_temps() {
    // Unified update handler for temperature observer bundle.
    // Called on UI thread after any temperature value changes.
    if (!panel_)
        return;

    // Always update current-temp-dependent displays
    update_left_card_temps();
    update_temp_display();
    update_warning_text();
    update_safety_state();
    update_status();

    // Only update target-dependent displays when targets actually changed.
    // Current temps change frequently during heating (~1Hz × 4 subjects),
    // but preset matching and material display only depend on targets.
    bool targets_changed =
        (nozzle_target_ != prev_nozzle_target_ || bed_target_ != prev_bed_target_);
    if (targets_changed) {
        prev_nozzle_target_ = nozzle_target_;
        prev_bed_target_ = bed_target_;
        update_material_temp_display();
        check_and_auto_select_preset();
        lv_subject_set_int(&nozzle_heating_subject_,
                           (nozzle_target_ > 0 || bed_target_ > 0) ? 1 : 0);

        // Cancel pending cooldown if user manually changed heater target
        if (nozzle_target_ != 0) {
            PostOpCooldownManager::instance().cancel();
        }
    }

    // Check if pending preheat target has been reached
    check_pending_preheat();
}

// ============================================================================
// INSTANCE HANDLERS
// ============================================================================

void FilamentPanel::handle_preset_button(int material_id) {
    // Delegate state update and display refresh to the public API
    set_material(material_id);

    // Send temperature commands to printer (nozzle, bed, and chamber if applicable)
    if (api_ && selected_material_ == material_id) {
        api_->set_temperature(
            printer_state_.active_extruder_name(), static_cast<double>(nozzle_target_),
            [target = nozzle_target_]() {
                NOTIFY_SUCCESS(lv_tr("Nozzle target set to {}°C"), target);
            },
            [](const MoonrakerError& error) {
                NOTIFY_ERROR(lv_tr("Failed to set nozzle temp: {}"), error.user_message());
            });
        api_->set_temperature(
            "heater_bed", static_cast<double>(bed_target_),
            [target = bed_target_]() { NOTIFY_SUCCESS(lv_tr("Bed target set to {}°C"), target); },
            [](const MoonrakerError& error) {
                NOTIFY_ERROR(lv_tr("Failed to set bed temp: {}"), error.user_message());
            });

        // Set chamber temperature if preset specifies one and printer has a chamber heater
        if (chamber_target_ > 0) {
            const auto& heater_name = printer_state_.get_discovery().chamber_heater_name();
            if (!heater_name.empty()) {
                int target = centi_to_degrees(chamber_target_);
                api_->set_temperature(
                    heater_name, static_cast<double>(target),
                    [target]() { NOTIFY_SUCCESS(lv_tr("Chamber target set to {}°C"), target); },
                    [](const MoonrakerError& err) {
                        NOTIFY_ERROR(lv_tr("Failed to set chamber temp: {}"), err.user_message());
                    });
            }
        }
    }
}

void FilamentPanel::handle_nozzle_temp_tap() {
    spdlog::debug("[{}] Opening custom nozzle temperature keypad", get_name());

    ui_keypad_config_t config = {.initial_value =
                                     static_cast<float>(nozzle_target_ > 0 ? nozzle_target_ : 200),
                                 .min_value = 0.0f,
                                 .max_value = static_cast<float>(nozzle_max_temp_),
                                 .title_label = lv_tr("Nozzle Temperature"),
                                 .unit_label = "°C",
                                 .allow_decimal = false,
                                 .allow_negative = false,
                                 .callback = custom_nozzle_keypad_cb,
                                 .user_data = this};

    ui_keypad_show(&config);
}

void FilamentPanel::handle_bed_temp_tap() {
    spdlog::debug("[{}] Opening custom bed temperature keypad", get_name());

    ui_keypad_config_t config = {.initial_value =
                                     static_cast<float>(bed_target_ > 0 ? bed_target_ : 60),
                                 .min_value = 0.0f,
                                 .max_value = static_cast<float>(bed_max_temp_),
                                 .title_label = lv_tr("Bed Temperature"),
                                 .unit_label = "°C",
                                 .allow_decimal = false,
                                 .allow_negative = false,
                                 .callback = custom_bed_keypad_cb,
                                 .user_data = this};

    ui_keypad_show(&config);
}

void FilamentPanel::handle_chamber_temp_tap() {
    spdlog::debug("[{}] Opening custom chamber temperature keypad", get_name());

    ui_keypad_config_t config = {.initial_value = static_cast<float>(
                                     chamber_target_ > 0 ? centi_to_degrees(chamber_target_) : 50),
                                 .min_value = 0.0f,
                                 .max_value = static_cast<float>(chamber_max_temp_),
                                 .title_label = lv_tr("Chamber Temperature"),
                                 .unit_label = "°C",
                                 .allow_decimal = false,
                                 .allow_negative = false,
                                 .callback =
                                     [](float value, void* user_data) {
                                         auto* self = static_cast<FilamentPanel*>(user_data);
                                         if (self) {
                                             self->handle_custom_chamber_confirmed(value);
                                         }
                                     },
                                 .user_data = this};

    ui_keypad_show(&config);
}

void FilamentPanel::handle_custom_chamber_confirmed(float value) {
    spdlog::info("[{}] Custom chamber temperature confirmed: {}°C", get_name(),
                 static_cast<int>(value));

    chamber_target_ = static_cast<int>(value * 10);
    update_chamber_temp_display();

    if (api_) {
        const auto& heater_name = printer_state_.get_discovery().chamber_heater_name();
        if (heater_name.empty()) {
            NOTIFY_ERROR(lv_tr("Chamber heater not found in printer configuration"));
            return;
        }

        int target = static_cast<int>(value);
        api_->set_temperature(
            heater_name, static_cast<double>(target),
            [target]() { NOTIFY_SUCCESS(lv_tr("Chamber target set to {}°C"), target); },
            [](const MoonrakerError& err) {
                NOTIFY_ERROR(lv_tr("Failed to set chamber temp: {}"), err.user_message());
            });
    }
}

void FilamentPanel::handle_custom_nozzle_confirmed(float value) {
    spdlog::info("[{}] Custom nozzle temperature confirmed: {}°C", get_name(),
                 static_cast<int>(value));

    nozzle_target_ = static_cast<int>(value);
    // Deselect any preset since user set custom temp
    selected_material_ = -1;
    lv_subject_set_int(&material_selected_subject_, selected_material_);
    update_preset_buttons_visual();
    update_temp_display();
    update_material_temp_display();
    update_status();

    // Send temperature command to printer
    if (api_) {
        api_->set_temperature(
            printer_state_.active_extruder_name(), static_cast<double>(nozzle_target_),
            [target = nozzle_target_]() {
                NOTIFY_SUCCESS(lv_tr("Nozzle target set to {}°C"), target);
            },
            [](const MoonrakerError& error) {
                NOTIFY_ERROR(lv_tr("Failed to set nozzle temp: {}"), error.user_message());
            });
    }
}

void FilamentPanel::handle_custom_bed_confirmed(float value) {
    spdlog::info("[{}] Custom bed temperature confirmed: {}°C", get_name(),
                 static_cast<int>(value));

    bed_target_ = static_cast<int>(value);
    // Deselect any preset since user set custom temp
    selected_material_ = -1;
    lv_subject_set_int(&material_selected_subject_, selected_material_);
    update_preset_buttons_visual();
    update_material_temp_display();

    // Send temperature command to printer
    if (api_) {
        api_->set_temperature(
            "heater_bed", static_cast<double>(bed_target_),
            [target = bed_target_]() { NOTIFY_SUCCESS(lv_tr("Bed target set to {}°C"), target); },
            [](const MoonrakerError& error) {
                NOTIFY_ERROR(lv_tr("Failed to set bed temp: {}"), error.user_message());
            });
    }
}

void FilamentPanel::update_material_temp_display() {
    // Use centralized formatting with em dash for heater-off state
    format_target_or_off(nozzle_target_, material_nozzle_buf_, sizeof(material_nozzle_buf_));
    format_target_or_off(bed_target_, material_bed_buf_, sizeof(material_bed_buf_));
    lv_subject_copy_string(&material_nozzle_temp_subject_, material_nozzle_buf_);
    lv_subject_copy_string(&material_bed_temp_subject_, material_bed_buf_);
}

void FilamentPanel::update_chamber_temp_display() {
    // chamber_current_ is already in degrees (observer converts), chamber_target_ is centidegrees
    std::snprintf(chamber_current_buf_, sizeof(chamber_current_buf_), "%d°C", chamber_current_);
    format_target_or_off(centi_to_degrees(chamber_target_), chamber_target_buf_,
                         sizeof(chamber_target_buf_));
    lv_subject_copy_string(&chamber_current_subject_, chamber_current_buf_);
    lv_subject_copy_string(&chamber_target_subject_, chamber_target_buf_);

    // Apply 4-state heating color (matches nozzle/bed)
    if (chamber_current_label_) {
        int target_deg = centi_to_degrees(chamber_target_);
        lv_color_t color = get_heating_state_color(chamber_current_, target_deg);
        lv_obj_set_style_text_color(chamber_current_label_, color, LV_PART_MAIN);
    }
}

void FilamentPanel::update_left_card_temps() {
    // Update current temps
    std::snprintf(nozzle_current_buf_, sizeof(nozzle_current_buf_), "%d°C", nozzle_current_);
    std::snprintf(bed_current_buf_, sizeof(bed_current_buf_), "%d°C", bed_current_);
    lv_subject_copy_string(&nozzle_current_subject_, nozzle_current_buf_);
    lv_subject_copy_string(&bed_current_subject_, bed_current_buf_);

    // Update target temps using centralized formatting with em dash for heater-off state
    format_target_or_off(nozzle_target_, nozzle_target_buf_, sizeof(nozzle_target_buf_));
    format_target_or_off(bed_target_, bed_target_buf_, sizeof(bed_target_buf_));
    lv_subject_copy_string(&nozzle_target_subject_, nozzle_target_buf_);
    lv_subject_copy_string(&bed_target_subject_, bed_target_buf_);

    // Update temperature label colors using 4-state heating logic
    // (matches temp_display widget: gray=off, red=heating, green=at-temp, blue=cooling)
    if (nozzle_current_label_) {
        lv_color_t nozzle_color = get_heating_state_color(nozzle_current_, nozzle_target_);
        lv_obj_set_style_text_color(nozzle_current_label_, nozzle_color, LV_PART_MAIN);
    }
    if (bed_current_label_) {
        lv_color_t bed_color = get_heating_state_color(bed_current_, bed_target_);
        lv_obj_set_style_text_color(bed_current_label_, bed_color, LV_PART_MAIN);
    }
}

void FilamentPanel::update_status_icon_for_state() {
    // Determine icon and color based on current state
    if (nozzle_target_ == 0 && bed_target_ == 0) {
        // Idle - no target set
        update_status_icon("info", "secondary");
    } else if (nozzle_current_ < nozzle_target_ - 5 || bed_current_ < bed_target_ - 5) {
        // Heating
        update_status_icon("fire", "warning");
    } else if (nozzle_current_ > nozzle_target_ + 5 && nozzle_target_ > 0) {
        // Cooling down
        update_status_icon("cooldown", "info");
    } else {
        // At temperature
        update_status_icon("check", "success");
    }
}

// set_operation_in_progress removed — replaced by OperationTimeoutGuard

void FilamentPanel::handle_purge_amount_select(int amount) {
    purge_amount_ = amount;
    // Update boolean subjects for each button (only one active at a time)
    lv_subject_set_int(&purge_5mm_active_subject_, amount == 5 ? 1 : 0);
    lv_subject_set_int(&purge_10mm_active_subject_, amount == 10 ? 1 : 0);
    lv_subject_set_int(&purge_25mm_active_subject_, amount == 25 ? 1 : 0);
    spdlog::debug("[{}] Purge amount set to {}mm", get_name(), amount);
}

void FilamentPanel::handle_load_button() {
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }

    // Cancel existing preheat if user taps again
    if (pending_preheat_op_ != PreheatOp::NONE) {
        cancel_pending_preheat();
        return;
    }

    if (!is_extrusion_allowed()) {
        start_preheat_for_op(PreheatOp::LOAD);
        return;
    }

    // Check if toolhead sensor shows filament already present
    auto& sensor_mgr = helix::FilamentSensorManager::instance();
    if (sensor_mgr.is_master_enabled() &&
        sensor_mgr.is_sensor_available(helix::FilamentSensorRole::TOOLHEAD) &&
        sensor_mgr.is_filament_detected(helix::FilamentSensorRole::TOOLHEAD)) {
        // Filament appears to already be loaded - show warning
        spdlog::info("[{}] Toolhead sensor shows filament present - showing load warning",
                     get_name());
        show_load_warning();
        return;
    }

    // No sensor or no filament detected - proceed directly
    execute_load();
}

void FilamentPanel::handle_unload_button() {
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }

    // Cancel existing preheat if user taps again
    if (pending_preheat_op_ != PreheatOp::NONE) {
        cancel_pending_preheat();
        return;
    }

    if (!is_extrusion_allowed()) {
        start_preheat_for_op(PreheatOp::UNLOAD);
        return;
    }

    // Check if toolhead sensor shows no filament (nothing to unload)
    auto& sensor_mgr = helix::FilamentSensorManager::instance();
    if (sensor_mgr.is_master_enabled() &&
        sensor_mgr.is_sensor_available(helix::FilamentSensorRole::TOOLHEAD) &&
        !sensor_mgr.is_filament_detected(helix::FilamentSensorRole::TOOLHEAD)) {
        // No filament detected - show warning
        spdlog::info("[{}] Toolhead sensor shows no filament - showing unload warning", get_name());
        show_unload_warning();
        return;
    }

    // Sensor not available or filament detected - proceed directly
    execute_unload();
}

void FilamentPanel::handle_extrude_button() {
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }

    if (pending_preheat_op_ != PreheatOp::NONE) {
        cancel_pending_preheat();
        return;
    }

    if (!is_extrusion_allowed()) {
        start_preheat_for_op(PreheatOp::EXTRUDE);
        return;
    }

    execute_extrude();
}

void FilamentPanel::execute_extrude() {
    spdlog::info("[{}] Extruding {}mm", get_name(), purge_amount_);

    if (!api_) {
        return;
    }

    // Inline G-code: M83 = relative extrusion, G1 E{amount} F{speed}
    operation_guard_.begin(OPERATION_TIMEOUT_MS,
                           [] { NOTIFY_WARNING(lv_tr("Filament operation timed out")); });
    int speed_mm_min = helix::SettingsManager::instance().get_extrude_speed() * 60;
    spdlog::info("[{}] Extruding {}mm at F{}", get_name(), purge_amount_, speed_mm_min);
    std::string gcode = fmt::format("M83\nG1 E{} F{}", purge_amount_, speed_mm_min);
    NOTIFY_INFO(lv_tr("Extruding {}mm..."), purge_amount_);

    api_->execute_gcode(
        gcode,
        [this, amount = purge_amount_]() {
            helix::ui::async_call(
                [](void* ud) { static_cast<FilamentPanel*>(ud)->operation_guard_.end(); }, this);
            NOTIFY_SUCCESS(lv_tr("Extrude complete ({}mm)"), amount);
        },
        [this](const MoonrakerError& error) {
            helix::ui::async_call(
                [](void* ud) { static_cast<FilamentPanel*>(ud)->operation_guard_.end(); }, this);
            if (error.type == MoonrakerErrorType::TIMEOUT) {
                NOTIFY_WARNING(lv_tr("Extrude may still be running — response timed out"));
            } else {
                NOTIFY_ERROR(lv_tr("Extrude failed: {}"), error.user_message());
            }
        },
        MoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

void FilamentPanel::handle_purge_button() {
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }

    if (pending_preheat_op_ != PreheatOp::NONE) {
        cancel_pending_preheat();
        return;
    }

    if (!is_extrusion_allowed()) {
        start_preheat_for_op(PreheatOp::PURGE);
        return;
    }

    execute_purge();
}

void FilamentPanel::execute_purge() {
    spdlog::info("[{}] Purging", get_name());

    if (!api_) {
        return;
    }

    // Try StandardMacros Purge slot first (PURGE, PURGE_LINE, PRIME_LINE, etc.)
    const auto& info = StandardMacros::instance().get(StandardMacroSlot::Purge);
    if (!info.is_empty()) {
        std::string macro_name = info.get_macro();
        auto cached = MacroParamCache::instance().get(macro_name);

        // Pre-fill PURGE_TEMP from active material if available
        std::string purge_temp_default;
        auto active = helix::get_active_material();
        if (active) {
            int recommended = active->material_info.nozzle_recommended();
            if (recommended > 0) {
                purge_temp_default = std::to_string(recommended);
                spdlog::info("[{}] Active material '{}' recommends PURGE_TEMP={}", get_name(),
                             active->display_name, recommended);
            }
        }

        if (cached.knowledge == MacroParamKnowledge::KNOWN_PARAMS) {
            // Override PURGE_TEMP default with active material temp
            auto params = cached.params;
            if (!purge_temp_default.empty()) {
                for (auto& p : params) {
                    if (p.name == "PURGE_TEMP") {
                        p.default_value = purge_temp_default;
                        break;
                    }
                }
            }
            spdlog::info("[{}] Purge macro '{}' has params, showing modal", get_name(), macro_name);
            get_filament_param_modal().show_for_macro(
                lv_screen_active(), macro_name, params,
                [this, macro_name](const MacroParamResult& result) {
                    run_filament_macro(macro_name, "Purg", result);
                });
            return;
        }

        if (cached.knowledge == MacroParamKnowledge::UNKNOWN) {
            spdlog::info("[{}] Purge macro '{}' params unknown, showing raw input", get_name(),
                         macro_name);
            get_filament_param_modal().show_for_unknown_params(
                lv_screen_active(), macro_name, [this, macro_name](const MacroParamResult& result) {
                    run_filament_macro(macro_name, "Purg", result);
                });
            return;
        }

        // KNOWN_NO_PARAMS — auto-pass PURGE_TEMP and execute directly
        MacroParamResult result;
        if (!purge_temp_default.empty()) {
            result.params["PURGE_TEMP"] = purge_temp_default;
        }
        run_filament_macro(macro_name, "Purg", result);
        return;
    }

    // Fallback: extrude a fixed 50mm at 10mm/s (M83 = relative extrusion)
    constexpr int PURGE_FALLBACK_MM = 50;
    constexpr int PURGE_FALLBACK_SPEED_MM_MIN = 10 * 60; // 10 mm/s → 600 mm/min
    operation_guard_.begin(OPERATION_TIMEOUT_MS,
                           [] { NOTIFY_WARNING(lv_tr("Filament operation timed out")); });
    spdlog::info("[{}] Purge fallback: extruding {}mm at F{}", get_name(), PURGE_FALLBACK_MM,
                 PURGE_FALLBACK_SPEED_MM_MIN);
    std::string gcode =
        fmt::format("M83\nG1 E{} F{}", PURGE_FALLBACK_MM, PURGE_FALLBACK_SPEED_MM_MIN);
    NOTIFY_INFO(lv_tr("Purging {}mm..."), PURGE_FALLBACK_MM);

    api_->execute_gcode(
        gcode,
        [this]() {
            helix::ui::async_call(
                [](void* ud) { static_cast<FilamentPanel*>(ud)->operation_guard_.end(); }, this);
            NOTIFY_SUCCESS(lv_tr("Purge complete"));
        },
        [this](const MoonrakerError& error) {
            helix::ui::async_call(
                [](void* ud) { static_cast<FilamentPanel*>(ud)->operation_guard_.end(); }, this);
            if (error.type == MoonrakerErrorType::TIMEOUT) {
                NOTIFY_WARNING(lv_tr("Purge may still be running — response timed out"));
            } else {
                NOTIFY_ERROR(lv_tr("Purge failed: {}"), error.user_message());
            }
        },
        MoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

void FilamentPanel::handle_retract_button() {
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }

    if (pending_preheat_op_ != PreheatOp::NONE) {
        cancel_pending_preheat();
        return;
    }

    if (!is_extrusion_allowed()) {
        start_preheat_for_op(PreheatOp::RETRACT);
        return;
    }

    execute_retract();
}

void FilamentPanel::execute_retract() {
    spdlog::info("[{}] Retracting {}mm", get_name(), purge_amount_);

    if (!api_) {
        return;
    }

    // Inline G-code: M83 = relative extrusion, negative E = retract
    operation_guard_.begin(OPERATION_TIMEOUT_MS,
                           [] { NOTIFY_WARNING(lv_tr("Filament operation timed out")); });
    int speed_mm_min = helix::SettingsManager::instance().get_extrude_speed() * 60;
    spdlog::info("[{}] Retracting {}mm at F{}", get_name(), purge_amount_, speed_mm_min);
    std::string gcode = fmt::format("M83\nG1 E-{} F{}", purge_amount_, speed_mm_min);
    NOTIFY_INFO(lv_tr("Retracting {}mm..."), purge_amount_);

    api_->execute_gcode(
        gcode,
        [this, amount = purge_amount_]() {
            helix::ui::async_call(
                [](void* ud) { static_cast<FilamentPanel*>(ud)->operation_guard_.end(); }, this);
            NOTIFY_SUCCESS(lv_tr("Retract complete ({}mm)"), amount);
        },
        [this](const MoonrakerError& error) {
            helix::ui::async_call(
                [](void* ud) { static_cast<FilamentPanel*>(ud)->operation_guard_.end(); }, this);
            if (error.type == MoonrakerErrorType::TIMEOUT) {
                NOTIFY_WARNING(lv_tr("Retract may still be running — response timed out"));
            } else {
                NOTIFY_ERROR(lv_tr("Retract failed: {}"), error.user_message());
            }
        },
        MoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

// ============================================================================
// EXTRUDER DROPDOWN
// ============================================================================

// Left-column sizing: graph always grows to fill, spool card hugs its
// content. Works for every breakpoint and every AMS mode — previous
// "graph fixed per-breakpoint, spool fills" arrangement truncated the
// AMS card at MEDIUM/SMALL when the fixed graph height ate too much.
void FilamentPanel::apply_left_column_sizing(bool /*external_spool_mode*/) {
    if (!temp_graph_card_ || !spool_card_)
        return;

    lv_obj_set_height(temp_graph_card_, 0);
    lv_obj_set_flex_grow(temp_graph_card_, 1);
    lv_obj_set_height(spool_card_, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(spool_card_, 0);
}

void FilamentPanel::update_multi_filament_card_visibility() {
    if (!spool_card_)
        return;

    bool has_ams = (lv_subject_get_int(AmsState::instance().get_ams_type_subject()) != 0);
    bool multi_tool = helix::ToolState::instance().is_multi_tool();

    // Card is ALWAYS visible
    lv_obj_remove_flag(spool_card_, LV_OBJ_FLAG_HIDDEN);

    // AMS manage row visible when AMS or multi-tool
    if (ams_manage_row_) {
        if (has_ams || multi_tool) {
            lv_obj_remove_flag(ams_manage_row_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ams_manage_row_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    bool external_spool_mode = !has_ams && !multi_tool;

    // External spool row visible when no AMS and no multi-tool
    if (external_spool_row_) {
        if (external_spool_mode) {
            lv_obj_remove_flag(external_spool_row_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(external_spool_row_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Header row (icon + title) hidden in external spool mode
    if (spool_card_header_row_) {
        if (external_spool_mode) {
            lv_obj_add_flag(spool_card_header_row_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(spool_card_header_row_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Center the external spool row within the card when in external spool mode
    if (external_spool_mode) {
        lv_obj_set_style_flex_main_place(spool_card_, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_flex_cross_place(spool_card_, LV_FLEX_ALIGN_CENTER, 0);
    } else {
        lv_obj_set_style_flex_main_place(spool_card_, LV_FLEX_ALIGN_START, 0);
        lv_obj_set_style_flex_cross_place(spool_card_, LV_FLEX_ALIGN_START, 0);
    }

    // Swap which card is the flex filler. Without AMS at MICRO/TINY the spool
    // card has almost no content, so hand the freed space to the temp graph.
    apply_left_column_sizing(external_spool_mode);

    // Update card title dynamically (for AMS/multi-tool modes)
    const char* title = external_spool_mode ? lv_tr("External Spool") : lv_tr("Multi-Filament");
    std::strncpy(card_title_buf_, title, sizeof(card_title_buf_) - 1);
    card_title_buf_[sizeof(card_title_buf_) - 1] = '\0';
    lv_subject_copy_string(&card_title_subject_, card_title_buf_);

    spdlog::debug("[{}] Multi-filament card: ams={}, multi_tool={}, title={}", get_name(), has_ams,
                  multi_tool, title);
}

void FilamentPanel::setup_external_spool_display() {
    if (!external_spool_container_)
        return;

    // Create 48x48 spool canvas inside the container
    external_spool_canvas_ = ui_spool_canvas_create(external_spool_container_, 48);
    if (!external_spool_canvas_) {
        spdlog::warn("[{}] Failed to create external spool canvas", get_name());
        return;
    }
    // L071: Canvas absorbs clicks — pass through to parent row's event_cb
    lv_obj_remove_flag(external_spool_canvas_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(external_spool_canvas_, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Set initial state from AmsState
    update_external_spool_from_state();

    // Observe external spool color changes to reactively update display
    external_spool_observer_ =
        observe_int_sync<FilamentPanel>(AmsState::instance().get_external_spool_color_subject(),
                                        this, [](FilamentPanel* self, int /*color_int*/) {
                                            self->update_external_spool_from_state();
                                            self->update_spool_preset();
                                        });

    spdlog::debug("[{}] External spool display initialized", get_name());
}

void FilamentPanel::update_external_spool_from_state() {
    if (!external_spool_canvas_)
        return;

    auto ext = AmsState::instance().get_external_spool_info();
    if (ext.has_value()) {
        ui_spool_canvas_set_color(external_spool_canvas_, lv_color_hex(ext->color_rgb));
        float fill =
            (ext->total_weight_g > 0) ? ext->remaining_weight_g / ext->total_weight_g : 1.0f;
        ui_spool_canvas_set_fill_level(external_spool_canvas_, fill);

        // Update labels with material info
        if (external_spool_material_label_) {
            std::string mat_text;
            if (!ext->brand.empty() && !ext->material.empty()) {
                mat_text = ext->brand + " " + ext->material;
            } else if (!ext->material.empty()) {
                mat_text = ext->material;
            } else {
                mat_text = "Unknown";
            }
            // Append Spoolman spool ID when linked (e.g., "Prusament PLA #129")
            if (ext->spoolman_id > 0) {
                char id_buf[16];
                snprintf(id_buf, sizeof(id_buf), " #%d", ext->spoolman_id);
                mat_text += id_buf; // i18n: do not translate — Spoolman ID
            }
            lv_label_set_text(external_spool_material_label_, mat_text.c_str());
        }
        if (external_spool_color_label_) {
            // Build second line: color name + remaining weight in grams
            std::string detail;
            if (!ext->color_name.empty()) {
                detail = ext->color_name;
            }
            if (ext->remaining_weight_g > 0) {
                char weight_str[16];
                snprintf(weight_str, sizeof(weight_str), "%.0fg", ext->remaining_weight_g);
                if (!detail.empty()) {
                    detail += " · ";
                }
                detail += weight_str;
            }
            lv_label_set_text(external_spool_color_label_, detail.c_str());
        }
    } else {
        // No spool assigned - show muted empty spool
        ui_spool_canvas_set_color(external_spool_canvas_, lv_color_hex(0x505050));
        ui_spool_canvas_set_fill_level(external_spool_canvas_, 0.0f);

        if (external_spool_material_label_) {
            lv_label_set_text(external_spool_material_label_, lv_tr("No spool assigned"));
        }
        if (external_spool_color_label_) {
            lv_label_set_text(external_spool_color_label_, lv_tr("Tap to assign"));
        }
    }
    ui_spool_canvas_redraw(external_spool_canvas_);
}

void FilamentPanel::show_external_spool_edit_modal() {
    if (!parent_screen_) {
        spdlog::warn("[{}] Cannot show edit modal - no parent screen", get_name());
        return;
    }

    if (!edit_modal_) {
        edit_modal_ = std::make_unique<helix::ui::AmsEditModal>();
    }

    auto ext = AmsState::instance().get_external_spool_info();
    SlotInfo initial_info = ext.value_or(SlotInfo{});
    initial_info.slot_index = -2;
    initial_info.global_index = -2;

    edit_modal_->set_completion_callback([](const helix::ui::AmsEditModal::EditResult& result) {
        if (result.saved) {
            if (result.slot_info.spoolman_id > 0 || !result.slot_info.material.empty()) {
                AmsState::instance().set_external_spool_info(result.slot_info);
            } else {
                AmsState::instance().clear_external_spool_info();
            }
            NOTIFY_INFO(lv_tr("External spool updated"));
        }
    });
    edit_modal_->show_for_slot(parent_screen_, -2, initial_info, api_);
}

void FilamentPanel::on_external_spool_edit_clicked(lv_event_t* /*e*/) {
    get_global_filament_panel().show_external_spool_edit_modal();
}

void FilamentPanel::populate_extruder_dropdown() {
    if (!extruder_dropdown_)
        return;

    auto& ts = helix::ToolState::instance();
    bool has_ams = (lv_subject_get_int(AmsState::instance().get_ams_type_subject()) != 0);
    if (!ts.is_multi_tool() || !has_ams) {
        if (extruder_selector_group_)
            lv_obj_add_flag(extruder_selector_group_, LV_OBJ_FLAG_HIDDEN);
        if (btn_manage_slots_)
            lv_obj_remove_flag(btn_manage_slots_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Multi-tool with AMS: show dropdown group, hide Manage button
    if (extruder_selector_group_)
        lv_obj_remove_flag(extruder_selector_group_, LV_OBJ_FLAG_HIDDEN);
    if (btn_manage_slots_)
        lv_obj_add_flag(btn_manage_slots_, LV_OBJ_FLAG_HIDDEN);

    // Build options string ("T0\nT1\nT2")
    std::string options;
    for (const auto& tool : ts.tools()) {
        if (!options.empty())
            options += '\n';
        options += tool.name;
    }
    lv_dropdown_set_options(extruder_dropdown_, options.c_str());

    // Sync selection to active tool
    int active = ts.active_tool_index();
    if (active >= 0 && active < ts.tool_count()) {
        lv_dropdown_set_selected(extruder_dropdown_, static_cast<uint32_t>(active));
    }

    spdlog::debug("[{}] Extruder dropdown populated: {} tools, active=T{}", get_name(),
                  ts.tool_count(), active);
}

void FilamentPanel::handle_extruder_changed() {
    if (!extruder_dropdown_)
        return;

    int selected = static_cast<int>(lv_dropdown_get_selected(extruder_dropdown_));
    auto& ts = helix::ToolState::instance();

    if (selected == ts.active_tool_index())
        return;

    if (selected < 0 || selected >= static_cast<int>(ts.tools().size())) {
        spdlog::warn("[{}] Invalid extruder index {}", get_name(), selected);
        return;
    }

    spdlog::info("[{}] User selected tool T{}", get_name(), selected);

    ts.request_tool_change(
        selected, api_, [selected]() { NOTIFY_SUCCESS(lv_tr("Switched to T{}"), selected); },
        [this](const std::string& error) {
            NOTIFY_ERROR(lv_tr("Tool change failed: {}"), error);
            // Revert dropdown to actual active tool on UI thread
            helix::ui::async_call(
                [](void* ctx) {
                    auto* panel = static_cast<FilamentPanel*>(ctx);
                    if (panel->extruder_dropdown_) {
                        int active = helix::ToolState::instance().active_tool_index();
                        if (active >= 0) {
                            lv_dropdown_set_selected(panel->extruder_dropdown_,
                                                     static_cast<uint32_t>(active));
                        }
                    }
                },
                this);
        });
}

void FilamentPanel::on_extruder_dropdown_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_extruder_dropdown_changed");
    LV_UNUSED(e);
    get_global_filament_panel().handle_extruder_changed();
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// STATIC TRAMPOLINES
// ============================================================================

void FilamentPanel::on_manage_slots_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_manage_slots_clicked");
    LV_UNUSED(e);

    spdlog::info("[FilamentPanel] Opening AMS panel overlay");
    navigate_to_ams_panel();

    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_load_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_load_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_load_button();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_unload_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_unload_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_unload_button();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_extrude_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_extrude_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_extrude_button();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_purge_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_purge_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_purge_button();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_retract_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_retract_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_retract_button();
    LVGL_SAFE_EVENT_CB_END();
}

// Material preset callbacks (XML event_cb - use global singleton)
void FilamentPanel::on_preset_pla_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_pla_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_preset_button(0);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_preset_petg_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_petg_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_preset_button(1);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_preset_abs_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_abs_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_preset_button(2);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_preset_tpu_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_tpu_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_preset_button(3);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_preset_spool_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_spool_clicked");
    get_global_filament_panel().handle_spool_preset_button();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::handle_spool_preset_button() {
    if (!cached_active_material_.has_value())
        return;

    const auto& mat = cached_active_material_->material_info;
    nozzle_target_ = mat.nozzle_recommended();
    bed_target_ = mat.bed_temp;

    // Deselect all fixed presets
    selected_material_ = -1;
    lv_subject_set_int(&material_selected_subject_, -1);
    update_preset_buttons_visual();

    // Highlight spool preset
    if (spool_preset_button_) {
        lv_obj_add_state(spool_preset_button_, LV_STATE_CHECKED);
    }

    update_temp_display();
    update_material_temp_display();
    update_status();

    // Send temperature commands
    if (api_) {
        api_->set_temperature(
            printer_state_.active_extruder_name(), static_cast<double>(nozzle_target_),
            [t = nozzle_target_]() { NOTIFY_SUCCESS(lv_tr("Nozzle target set to {}°C"), t); },
            [](const MoonrakerError& err) {
                NOTIFY_ERROR(lv_tr("Failed to set nozzle temp: {}"), err.user_message());
            });
        api_->set_temperature(
            "heater_bed", static_cast<double>(bed_target_),
            [t = bed_target_]() { NOTIFY_SUCCESS(lv_tr("Bed target set to {}°C"), t); },
            [](const MoonrakerError& err) {
                NOTIFY_ERROR(lv_tr("Failed to set bed temp: {}"), err.user_message());
            });
    }

    spdlog::info("[{}] Spool preset applied: {} (nozzle={}°C, bed={}°C)", get_name(),
                 cached_active_material_->display_name, nozzle_target_, bed_target_);
}

void FilamentPanel::update_spool_preset() {
    cached_active_material_ = helix::get_active_material();

    if (!spool_preset_button_)
        return;

    if (!cached_active_material_.has_value()) {
        lv_obj_add_flag(spool_preset_button_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const auto& active = *cached_active_material_;

    // Check if material matches an existing preset — if so, don't show spool button
    for (int i = 0; i < PRESET_COUNT; i++) {
        std::string preset_lower(PRESET_MATERIAL_NAMES[i]);
        std::string mat_lower(active.material_name);
        std::transform(preset_lower.begin(), preset_lower.end(), preset_lower.begin(), ::tolower);
        std::transform(mat_lower.begin(), mat_lower.end(), mat_lower.begin(), ::tolower);
        if (preset_lower == mat_lower) {
            lv_obj_add_flag(spool_preset_button_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
    }

    // Novel material — show spool preset button.
    // Using lv_label_set_text directly: text is dynamic (material name + computed temps)
    // so subject binding is not practical here.
    lv_obj_remove_flag(spool_preset_button_, LV_OBJ_FLAG_HIDDEN);

    if (spool_preset_label_) {
        lv_label_set_text(spool_preset_label_, active.material_name.c_str());
    }
    if (spool_preset_temps_) {
        auto text = fmt::format("{} / {}°C", active.material_info.nozzle_recommended(),
                                active.material_info.bed_temp);
        lv_label_set_text(spool_preset_temps_, text.c_str());
    }

    // Deselect spool button initially (user must tap)
    if (spool_preset_button_) {
        lv_obj_remove_state(spool_preset_button_, LV_STATE_CHECKED);
    }

    spdlog::debug("[{}] Spool preset shown: {} ({}°C / {}°C)", get_name(), active.display_name,
                  active.material_info.nozzle_recommended(), active.material_info.bed_temp);
}

// Temperature tap callbacks (XML event_cb - use global singleton)
void FilamentPanel::on_nozzle_temp_tap_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_nozzle_temp_tap_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_nozzle_temp_tap();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_bed_temp_tap_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_bed_temp_tap_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_bed_temp_tap();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::custom_nozzle_keypad_cb(float value, void* user_data) {
    auto* self = static_cast<FilamentPanel*>(user_data);
    if (self) {
        self->handle_custom_nozzle_confirmed(value);
    }
}

void FilamentPanel::custom_bed_keypad_cb(float value, void* user_data) {
    auto* self = static_cast<FilamentPanel*>(user_data);
    if (self) {
        self->handle_custom_bed_confirmed(value);
    }
}

void FilamentPanel::on_nozzle_target_tap_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_nozzle_target_tap_clicked");
    LV_UNUSED(e);
    spdlog::debug("[FilamentPanel] on_nozzle_target_tap_clicked TRIGGERED");
    get_global_filament_panel().handle_nozzle_temp_tap();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_bed_target_tap_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_bed_target_tap_clicked");
    LV_UNUSED(e);
    spdlog::debug("[FilamentPanel] on_bed_target_tap_clicked TRIGGERED");
    get_global_filament_panel().handle_bed_temp_tap();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_filament_chamber_target_tap(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_filament_chamber_target_tap");
    LV_UNUSED(e);
    spdlog::debug("[FilamentPanel] on_filament_chamber_target_tap TRIGGERED");
    get_global_filament_panel().handle_chamber_temp_tap();
    LVGL_SAFE_EVENT_CB_END();
}

// Purge amount callbacks (XML event_cb - use global singleton)
void FilamentPanel::on_purge_5mm_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_purge_5mm_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_purge_amount_select(5);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_purge_10mm_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_purge_10mm_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_purge_amount_select(10);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_purge_25mm_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_purge_25mm_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_purge_amount_select(25);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_cooldown_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_cooldown_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_cooldown();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::handle_cooldown() {
    spdlog::info("[{}] Cooldown requested - turning off heaters", get_name());

    if (api_) {
        // Build default cooldown gcode, including chamber if printer has one
        std::string default_gcode = "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\n"
                                    "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0";

        const auto& discovery = printer_state_.get_discovery();
        if (discovery.has_chamber_heater()) {
            char chamber_gcode[128];
            if (helix::ui::temperature::build_heater_off_gcode(
                    discovery.chamber_heater_name(), chamber_gcode, sizeof(chamber_gcode))) {
                default_gcode += "\n";
                default_gcode += chamber_gcode;
            }
        }

        // Use configured cooldown macro (user-overridable in settings.json)
        auto* cfg = helix::Config::get_instance();
        helix::MacroConfig default_cooldown{"Cool Down", default_gcode};
        auto cooldown = cfg ? cfg->get_macro("cooldown", default_cooldown) : default_cooldown;

        api_->execute_gcode(
            cooldown.gcode, []() { NOTIFY_SUCCESS(lv_tr("Heaters off")); },
            [](const MoonrakerError& error) {
                NOTIFY_ERROR(lv_tr("Failed to turn off heaters: {}"), error.user_message());
            });
    }

    // Clear material selection since we're cooling down
    selected_material_ = -1;
    lv_subject_set_int(&material_selected_subject_, selected_material_);
    update_preset_buttons_visual();
}

// ============================================================================
// PUBLIC API
// ============================================================================

void FilamentPanel::set_temp(int current, int target) {
    // Validate temperature ranges
    helix::ui::temperature::validate_and_clamp_pair(current, target, nozzle_min_temp_ * 10,
                                                    nozzle_max_temp_ * 10, "Filament");

    nozzle_current_ = current;
    nozzle_target_ = target;

    update_temp_display();
    update_status();
    update_warning_text();
    update_safety_state();
}

void FilamentPanel::get_temp(int* current, int* target) const {
    if (current)
        *current = nozzle_current_;
    if (target)
        *target = nozzle_target_;
}

void FilamentPanel::set_material(int material_id) {
    if (material_id < 0 || material_id >= PRESET_COUNT) {
        spdlog::error("[{}] Invalid material ID {} (valid: 0-{})", get_name(), material_id,
                      PRESET_COUNT - 1);
        return;
    }

    auto mat = filament::find_material(PRESET_MATERIAL_NAMES[material_id]);
    if (!mat) {
        spdlog::error("[{}] Material '{}' not found in database", get_name(),
                      PRESET_MATERIAL_NAMES[material_id]);
        return;
    }

    selected_material_ = material_id;
    nozzle_target_ = mat->nozzle_recommended();
    bed_target_ = mat->bed_temp;

    // Set chamber target from material preset (clear if material has no chamber requirement)
    if (printer_state_.get_discovery().has_chamber_heater()) {
        chamber_target_ = mat->chamber_temp_c > 0
                              ? helix::ui::temperature::degrees_to_centi(mat->chamber_temp_c)
                              : 0;
        update_chamber_temp_display();
    }

    lv_subject_set_int(&material_selected_subject_, selected_material_);
    update_preset_buttons_visual();
    update_temp_display();
    update_material_temp_display();
    update_status();

    spdlog::info("[{}] Material set: {} (nozzle={}°C, bed={}°C, chamber={}°C)", get_name(),
                 PRESET_MATERIAL_NAMES[material_id], nozzle_target_, bed_target_,
                 mat->chamber_temp_c);
}

bool FilamentPanel::is_extrusion_allowed() const {
    return helix::ui::temperature::is_extrusion_safe(nozzle_current_, min_extrude_temp_);
}

bool FilamentPanel::has_active_spool_material() const {
    // Check if there's a known spool with valid material info (external spool or AMS active slot)
    auto ext = AmsState::instance().get_external_spool_info();
    if (ext.has_value()) {
        auto active = helix::build_active_material(*ext);
        if (active.material_info.nozzle_min > 0) {
            return true;
        }
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend) {
        AmsSystemInfo sys_info = backend->get_system_info();
        const SlotInfo* active_slot = sys_info.get_active_slot();
        if (active_slot) {
            auto active = helix::build_active_material(*active_slot);
            if (active.material_info.nozzle_min > 0) {
                return true;
            }
        }
    }

    return false;
}

FilamentPanel::PreheatTempResult FilamentPanel::resolve_preheat_temp() const {
    // Priority 1: External spool
    auto ext = AmsState::instance().get_external_spool_info();
    if (ext.has_value()) {
        auto active = helix::build_active_material(*ext);
        if (active.material_info.nozzle_min > 0) {
            return {active.material_info.nozzle_min, active.display_name};
        }
    }

    // Priority 2: AMS active slot (loaded filament)
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend) {
        AmsSystemInfo sys_info = backend->get_system_info();
        const SlotInfo* active_slot = sys_info.get_active_slot();
        if (active_slot) {
            auto active = helix::build_active_material(*active_slot);
            if (active.material_info.nozzle_min > 0) {
                return {active.material_info.nozzle_min, active.display_name};
            }
        }
    }

    // Priority 3: Selected material preset
    if (selected_material_ >= 0 && selected_material_ < PRESET_COUNT) {
        auto mat = filament::find_material(PRESET_MATERIAL_NAMES[selected_material_]);
        if (mat) {
            return {mat->nozzle_min, PRESET_MATERIAL_NAMES[selected_material_]};
        }
    }

    // Priority 4: Fallback to min_extrude_temp_
    return {min_extrude_temp_, ""};
}

const char* FilamentPanel::preheat_op_name(PreheatOp op) {
    switch (op) {
    case PreheatOp::LOAD:
        return "load";
    case PreheatOp::UNLOAD:
        return "unload";
    case PreheatOp::EXTRUDE:
        return "extrude";
    case PreheatOp::RETRACT:
        return "retract";
    case PreheatOp::PURGE:
        return "purge";
    default:
        return "unknown";
    }
}

void FilamentPanel::start_preheat_for_op(PreheatOp op) {
    auto [target, material_name] = resolve_preheat_temp();

    // Snapshot heater state before we change it
    prior_nozzle_target_ = nozzle_target_;
    pending_preheat_op_ = op;
    pending_preheat_target_ = target;

    if (api_) {
        api_->set_temperature(
            printer_state_.active_extruder_name(), static_cast<double>(target), []() {},
            [](const MoonrakerError& /*err*/) {});
    }

    if (material_name.empty()) {
        NOTIFY_INFO(lv_tr("Heating to {}°C..."), target);
    } else {
        NOTIFY_INFO(lv_tr("Heating to {}°C for {}..."), target, material_name);
    }

    spdlog::info("[{}] Starting preheat to {}°C ({}) for {}", get_name(), target,
                 material_name.empty() ? "fallback" : material_name, preheat_op_name(op));
}

void FilamentPanel::check_pending_preheat() {
    if (pending_preheat_op_ == PreheatOp::NONE) {
        return;
    }

    constexpr int TEMP_THRESHOLD = 5;
    if (nozzle_current_ < (pending_preheat_target_ - TEMP_THRESHOLD)) {
        return;
    }

    PreheatOp op = pending_preheat_op_;
    pending_preheat_op_ = PreheatOp::NONE;
    pending_preheat_target_ = 0;

    spdlog::info("[{}] Preheat complete, executing {}", get_name(), preheat_op_name(op));

    switch (op) {
    case PreheatOp::LOAD:
        execute_load();
        break;
    case PreheatOp::UNLOAD:
        execute_unload();
        break;
    case PreheatOp::EXTRUDE:
        execute_extrude();
        break;
    case PreheatOp::RETRACT:
        execute_retract();
        break;
    case PreheatOp::PURGE:
        execute_purge();
        break;
    default:
        break;
    }
}

void FilamentPanel::cancel_pending_preheat() {
    if (pending_preheat_op_ == PreheatOp::NONE) {
        return;
    }

    spdlog::info("[{}] Preheat cancelled", get_name());
    pending_preheat_op_ = PreheatOp::NONE;
    pending_preheat_target_ = 0;

    // Cancel any pending cooldown timer
    PostOpCooldownManager::instance().cancel();

    // Restore heater to prior state immediately (no delay for cancel)
    if (prior_nozzle_target_ == 0 && api_) {
        api_->set_temperature(
            printer_state_.active_extruder_name(), 0, []() {},
            [](const MoonrakerError& /*err*/) {});
    }
    prior_nozzle_target_ = 0;

    NOTIFY_INFO(lv_tr("Preheat cancelled"));
}

void FilamentPanel::restore_heater_after_preheat() {
    if (prior_nozzle_target_ == 0) {
        PostOpCooldownManager::instance().schedule();
    }
    prior_nozzle_target_ = 0;
}

void FilamentPanel::set_limits(int min_temp, int max_temp, int min_extrude_temp) {
    nozzle_min_temp_ = min_temp;
    nozzle_max_temp_ = max_temp;

    // Update min_extrude_temp and safety warning text if changed
    if (min_extrude_temp_ != min_extrude_temp) {
        min_extrude_temp_ = min_extrude_temp;
        std::snprintf(safety_warning_text_buf_, sizeof(safety_warning_text_buf_),
                      SAFETY_WARNING_FMT, min_extrude_temp_);
        lv_subject_copy_string(&safety_warning_text_subject_, safety_warning_text_buf_);
        spdlog::info("[{}] Min extrusion temp updated: {}°C", get_name(), min_extrude_temp_);
    }

    spdlog::debug("[{}] Nozzle temperature limits updated: {}-{}°C", get_name(), min_temp,
                  max_temp);
}

// ============================================================================
// FILAMENT SENSOR WARNING HELPERS
// ============================================================================

void FilamentPanel::execute_load() {
    // When an AMS backend is active and requires slot selection, prefer loading
    // the currently-active slot directly — the filament panel's AMS card already
    // shows which slot is active, asking the user to pick again in the AMS panel
    // is redundant and confusing. Only fall through to the AMS-panel redirect
    // when no slot is currently active (e.g. cold start, no tool selected).
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend && backend->requires_slot_selection_for_load()) {
        AmsSystemInfo sys = backend->get_system_info();
        if (sys.current_slot >= 0) {
            spdlog::info("[{}] Loading filament directly into active slot {} (no redirect)",
                         get_name(), sys.current_slot);
            AmsError err = backend->load_filament(sys.current_slot);
            if (!err.success()) {
                NOTIFY_ERROR("{}", err.user_msg);
            }
            return;
        }
        spdlog::info("[{}] AMS backend active ({}), no active slot — redirecting to AMS panel",
                     get_name(), ams_type_to_string(backend->get_type()));
        NOTIFY_INFO(lv_tr("Select a filament slot to load"));
        navigate_to_ams_panel();
        return;
    }

    const auto& info = StandardMacros::instance().get(StandardMacroSlot::LoadFilament);
    if (!info.is_empty()) {
        std::string macro_name = info.get_macro();
        auto cached = MacroParamCache::instance().get(macro_name);

        if (cached.knowledge == MacroParamKnowledge::KNOWN_PARAMS) {
            spdlog::info("[{}] Load macro '{}' has params, showing modal", get_name(), macro_name);
            get_filament_param_modal().show_for_macro(
                lv_screen_active(), macro_name, cached.params,
                [this, macro_name](const MacroParamResult& result) {
                    run_filament_macro(macro_name, "Load", result);
                });
            return;
        }

        if (cached.knowledge == MacroParamKnowledge::UNKNOWN) {
            spdlog::info("[{}] Load macro '{}' params unknown, showing raw input", get_name(),
                         macro_name);
            get_filament_param_modal().show_for_unknown_params(
                lv_screen_active(), macro_name, [this, macro_name](const MacroParamResult& result) {
                    run_filament_macro(macro_name, "Load", result);
                });
            return;
        }

        // KNOWN_NO_PARAMS — execute directly
        run_filament_macro(macro_name, "Load", {});
        return;
    }

    // Fallback: fast move through bowden (56mm at 20mm/s) then slow push into
    // melt zone (24mm at 5mm/s). M83 = relative extrusion mode.
    constexpr int LOAD_FAST_MM = 56;
    constexpr int LOAD_FAST_SPEED = 20 * 60; // 20 mm/s → 1200 mm/min
    constexpr int LOAD_SLOW_MM = 24;
    constexpr int LOAD_SLOW_SPEED = 5 * 60; // 5 mm/s → 300 mm/min
    operation_guard_.begin(OPERATION_TIMEOUT_MS,
                           [] { NOTIFY_WARNING(lv_tr("Filament operation timed out")); });
    spdlog::info("[{}] Load fallback: {}mm fast + {}mm slow", get_name(), LOAD_FAST_MM,
                 LOAD_SLOW_MM);
    std::string gcode = fmt::format("M83\nG1 E{} F{}\nG1 E{} F{}", LOAD_FAST_MM, LOAD_FAST_SPEED,
                                    LOAD_SLOW_MM, LOAD_SLOW_SPEED);
    NOTIFY_INFO(lv_tr("Loading filament ({}mm)..."), LOAD_FAST_MM + LOAD_SLOW_MM);

    api_->execute_gcode(
        gcode,
        [this]() {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->restore_heater_after_preheat();
                },
                this);
            NOTIFY_SUCCESS(lv_tr("Filament loaded"));
        },
        [this](const MoonrakerError& error) {
            helix::ui::async_call(
                [](void* ud) { static_cast<FilamentPanel*>(ud)->operation_guard_.end(); }, this);
            if (error.type == MoonrakerErrorType::TIMEOUT) {
                NOTIFY_WARNING(lv_tr("Load may still be running — response timed out"));
            } else {
                NOTIFY_ERROR(lv_tr("Filament load failed: {}"), error.user_message());
            }
        },
        MoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

void FilamentPanel::execute_unload() {
    // When an AMS backend is active, route unload through it so the backend's
    // tool change sequence runs (retract, cut, purge) instead of raw extrusion.
    // Note: bypass unload stays routed through the backend — AFC calls the user's
    // unload_filament macro itself when bypass is enabled.
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend) {
        AmsSystemInfo sys_info = backend->get_system_info();
        if (!sys_info.filament_loaded && sys_info.current_slot < 0) {
            NOTIFY_WARNING(lv_tr("No filament loaded to unload"));
            return;
        }

        operation_guard_.begin(OPERATION_TIMEOUT_MS,
                               [] { NOTIFY_WARNING(lv_tr("Filament operation timed out")); });
        spdlog::info("[{}] Unloading filament via AMS backend ({})", get_name(),
                     ams_type_to_string(backend->get_type()));
        NOTIFY_INFO(lv_tr("Unloading filament..."));
        AmsError err = backend->unload_filament();
        if (!err.success()) {
            operation_guard_.end();
            NOTIFY_ERROR(lv_tr("Unload failed: {}"), err.user_msg);
        }
        // Guard ends via timeout — backend tracks completion via state observer
        return;
    }

    const auto& info = StandardMacros::instance().get(StandardMacroSlot::UnloadFilament);
    if (!info.is_empty()) {
        std::string macro_name = info.get_macro();
        auto cached = MacroParamCache::instance().get(macro_name);

        if (cached.knowledge == MacroParamKnowledge::KNOWN_PARAMS) {
            spdlog::info("[{}] Unload macro '{}' has params, showing modal", get_name(),
                         macro_name);
            get_filament_param_modal().show_for_macro(
                lv_screen_active(), macro_name, cached.params,
                [this, macro_name](const MacroParamResult& result) {
                    run_filament_macro(macro_name, "Unload", result);
                });
            return;
        }

        if (cached.knowledge == MacroParamKnowledge::UNKNOWN) {
            spdlog::info("[{}] Unload macro '{}' params unknown, showing raw input", get_name(),
                         macro_name);
            get_filament_param_modal().show_for_unknown_params(
                lv_screen_active(), macro_name, [this, macro_name](const MacroParamResult& result) {
                    run_filament_macro(macro_name, "Unload", result);
                });
            return;
        }

        // KNOWN_NO_PARAMS — execute directly
        run_filament_macro(macro_name, "Unload", {});
        return;
    }

    // Fallback: tip-shape (push 3mm, quick pull 5mm, dwell) then retract 80mm.
    // M83 = relative extrusion mode.
    constexpr int UNLOAD_MM = 80;
    constexpr int UNLOAD_SPEED = 20 * 60;   // 20 mm/s → 1200 mm/min
    constexpr int TIP_PUSH_SPEED = 5 * 60;  // 5 mm/s → 300 mm/min
    constexpr int TIP_PULL_SPEED = 60 * 60; // 60 mm/s → 3600 mm/min
    operation_guard_.begin(OPERATION_TIMEOUT_MS,
                           [] { NOTIFY_WARNING(lv_tr("Filament operation timed out")); });
    spdlog::info("[{}] Unload fallback: tip-shape + {}mm retract", get_name(), UNLOAD_MM);
    std::string gcode = fmt::format("M83\nG1 E3 F{}\nG1 E-5 F{}\nG4 P500\nG1 E-{} F{}",
                                    TIP_PUSH_SPEED, TIP_PULL_SPEED, UNLOAD_MM, UNLOAD_SPEED);
    NOTIFY_INFO(lv_tr("Unloading filament ({}mm)..."), UNLOAD_MM);

    api_->execute_gcode(
        gcode,
        [this]() {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->restore_heater_after_preheat();
                },
                this);
            NOTIFY_SUCCESS(lv_tr("Filament unloaded"));
        },
        [this](const MoonrakerError& error) {
            helix::ui::async_call(
                [](void* ud) { static_cast<FilamentPanel*>(ud)->operation_guard_.end(); }, this);
            if (error.type == MoonrakerErrorType::TIMEOUT) {
                NOTIFY_WARNING(lv_tr("Unload may still be running — response timed out"));
            } else {
                NOTIFY_ERROR(lv_tr("Filament unload failed: {}"), error.user_message());
            }
        },
        MoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

void FilamentPanel::run_filament_macro(const std::string& macro_name, const std::string& op_label,
                                       const MacroParamResult& params) {
    if (!api_) {
        return;
    }

    operation_guard_.begin(OPERATION_TIMEOUT_MS,
                           [] { NOTIFY_WARNING(lv_tr("Filament operation timed out")); });
    spdlog::info("[{}] Running '{}' ({})", get_name(), macro_name, op_label);

    std::string gcode = helix::build_macro_gcode(macro_name, params);
    // FilamentPanel is a global singleton, so `this` capture is safe [L012]
    api_->execute_gcode(
        gcode,
        [this]() {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->restore_heater_after_preheat();
                },
                this);
            NOTIFY_SUCCESS(lv_tr("Macro complete"));
        },
        [this](const MoonrakerError& error) {
            helix::ui::async_call(
                [](void* ud) { static_cast<FilamentPanel*>(ud)->operation_guard_.end(); }, this);
            if (error.type == MoonrakerErrorType::TIMEOUT) {
                NOTIFY_WARNING(lv_tr("Macro may still be running — response timed out"));
            } else {
                NOTIFY_ERROR(lv_tr("Macro failed: {}"), error.user_message());
            }
        },
        MoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

void FilamentPanel::show_load_warning() {
    // Close any existing dialog first
    if (load_warning_dialog_) {
        helix::ui::modal_hide(load_warning_dialog_);
        load_warning_dialog_ = nullptr;
    }

    load_warning_dialog_ = helix::ui::modal_show_confirmation(
        lv_tr("Filament Detected"),
        lv_tr("The toolhead sensor indicates filament is already loaded. "
              "Proceed with load anyway?"),
        ModalSeverity::Warning, lv_tr("Proceed"), on_load_warning_proceed, on_load_warning_cancel,
        this);

    if (!load_warning_dialog_) {
        spdlog::error("[{}] Failed to create load warning dialog", get_name());
        return;
    }

    spdlog::debug("[{}] Load warning dialog shown", get_name());
}

void FilamentPanel::show_unload_warning() {
    // Close any existing dialog first
    if (unload_warning_dialog_) {
        helix::ui::modal_hide(unload_warning_dialog_);
        unload_warning_dialog_ = nullptr;
    }

    unload_warning_dialog_ = helix::ui::modal_show_confirmation(
        lv_tr("No Filament Detected"),
        lv_tr("The toolhead sensor indicates no filament is present. "
              "Proceed with unload anyway?"),
        ModalSeverity::Warning, lv_tr("Proceed"), on_unload_warning_proceed,
        on_unload_warning_cancel, this);

    if (!unload_warning_dialog_) {
        spdlog::error("[{}] Failed to create unload warning dialog", get_name());
        return;
    }

    spdlog::debug("[{}] Unload warning dialog shown", get_name());
}

void FilamentPanel::on_load_warning_proceed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_load_warning_proceed");
    auto* self = static_cast<FilamentPanel*>(lv_event_get_user_data(e));
    if (self) {
        // Hide dialog first
        if (self->load_warning_dialog_) {
            helix::ui::modal_hide(self->load_warning_dialog_);
            self->load_warning_dialog_ = nullptr;
        }
        // Execute load
        self->execute_load();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_load_warning_cancel(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_load_warning_cancel");
    auto* self = static_cast<FilamentPanel*>(lv_event_get_user_data(e));
    if (self && self->load_warning_dialog_) {
        helix::ui::modal_hide(self->load_warning_dialog_);
        self->load_warning_dialog_ = nullptr;
        spdlog::debug("[FilamentPanel] Load cancelled by user");
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_unload_warning_proceed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_unload_warning_proceed");
    auto* self = static_cast<FilamentPanel*>(lv_event_get_user_data(e));
    if (self) {
        // Hide dialog first
        if (self->unload_warning_dialog_) {
            helix::ui::modal_hide(self->unload_warning_dialog_);
            self->unload_warning_dialog_ = nullptr;
        }
        // Execute unload
        self->execute_unload();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_unload_warning_cancel(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_unload_warning_cancel");
    auto* self = static_cast<FilamentPanel*>(lv_event_get_user_data(e));
    if (self && self->unload_warning_dialog_) {
        helix::ui::modal_hide(self->unload_warning_dialog_);
        self->unload_warning_dialog_ = nullptr;
        spdlog::debug("[FilamentPanel] Unload cancelled by user");
    }
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// GLOBAL INSTANCE (needed by main.cpp)
// ============================================================================

static std::unique_ptr<FilamentPanel> g_filament_panel;

FilamentPanel& get_global_filament_panel() {
    if (!g_filament_panel) {
        g_filament_panel = std::make_unique<FilamentPanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy("FilamentPanel",
                                                         []() { g_filament_panel.reset(); });
    }
    return *g_filament_panel;
}
