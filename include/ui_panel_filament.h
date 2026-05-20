// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_ams_edit_modal.h"
#include "ui_observer_guard.h"
#include "ui_panel_base.h"

#include "active_material_provider.h"
#include "config.h"
#include "macro_param_modal.h"
#include "operation_timeout_guard.h"
#include "subject_managed_panel.h"
#include "ui/temperature_observer_bundle.h"

// Forward declarations
class TemperatureService;

/**
 * @file ui_panel_filament.h
 * @brief Filament panel - Filament loading/unloading operations with safety checks
 *
 * Provides temperature-controlled filament operations:
 * - Material presets (PLA 210°C, PETG 240°C, ABS 250°C, Custom)
 * - Load/Unload/Purge operations with safety checks
 * - Temperature monitoring with visual feedback
 * - Safety warning when nozzle is too cold (< 170°C)
 *
 * ## Reactive Subjects:
 * - `filament_temp_display` - Temperature string (e.g., "210 / 240°C")
 * - `filament_status` - Status message (e.g., "✓ Ready to load")
 * - `filament_material_selected` - Selected material ID (-1=none, 0-3)
 * - `filament_extrusion_allowed` - Boolean: 1=hot enough, 0=too cold
 * - `filament_safety_warning_visible` - Boolean: 1=show warning, 0=hide
 * - `filament_warning_temps` - Warning card temp text
 *
 * ## Key Features:
 * - Temperature-driven safety logic (not a state machine)
 * - Imperative button enable/disable for performance
 * - Keypad integration for custom temperature input
 * - Visual preset selection feedback (LV_STATE_CHECKED)
 *
 * ## Migration Notes:
 * Phase 4 panel - demonstrates hybrid reactive/imperative state management.
 * Temperature updates are pushed externally via set_temp(), not observed.
 *
 * @see PanelBase for base class documentation
 * @see UITemperatureUtils for safety validation functions
 */

class FilamentPanel : public PanelBase {
  public:
    /**
     * @brief Construct FilamentPanel with injected dependencies
     *
     * @param printer_state Reference to helix::PrinterState
     * @param api Pointer to MoonrakerAPI (for future temp commands)
     */
    FilamentPanel(helix::PrinterState& printer_state, MoonrakerAPI* api);

    ~FilamentPanel() override;

    //
    // === PanelBase Implementation ===
    //

    /**
     * @brief Initialize filament subjects for XML binding
     *
     * Registers: filament_temp_display, filament_status, filament_material_selected,
     *            filament_extrusion_allowed, filament_safety_warning_visible,
     *            filament_warning_temps
     */
    void init_subjects() override;

    /**
     * @brief Deinitialize all subjects for clean shutdown
     *
     * Called by StaticPanelRegistry during application teardown.
     * Must be called BEFORE lv_deinit() to avoid dangling observer references.
     */
    void deinit_subjects();

    /**
     * @brief Setup button handlers and initial visual state
     *
     * - Wires preset buttons (PLA, PETG, ABS, Custom)
     * - Wires action buttons (Load, Unload, Purge)
     * - Configures safety warning visibility
     * - Initializes temperature display
     *
     * @param panel Root panel object from lv_xml_create()
     * @param parent_screen Parent screen for navigation
     */
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;

    const char* get_name() const override {
        return "Filament Panel";
    }
    const char* get_xml_component_name() const override {
        return "filament_panel";
    }

    //
    // === Public API ===
    //

    /**
     * @brief Update temperature display and safety state
     *
     * Called externally when temperature updates arrive from printer.
     * Updates subjects and triggers safety state re-evaluation.
     *
     * @param current Current nozzle temperature in °C
     * @param target Target nozzle temperature in °C
     */
    void set_temp(int current, int target);

    /**
     * @brief Get current temperature values
     *
     * @param[out] current Pointer to receive current temp (may be nullptr)
     * @param[out] target Pointer to receive target temp (may be nullptr)
     */
    void get_temp(int* current, int* target) const;

    /**
     * @brief Select a material preset
     *
     * Sets target temperature and updates visual state.
     *
     * @param material_id 0=PLA(210°C), 1=PETG(240°C), 2=ABS(250°C), 3=Custom
     */
    void set_material(int material_id);

    /**
     * @brief Get currently selected material
     * @return Material ID (-1=none, 0=PLA, 1=PETG, 2=ABS, 3=Custom)
     */
    int get_material() const {
        return selected_material_;
    }

    /**
     * @brief Check if extrusion operations are safe
     *
     * @return true if nozzle is at or above MIN_EXTRUSION_TEMP (170°C)
     */
    bool is_extrusion_allowed() const;

    /**
     * @brief Set temperature limits from Moonraker heater config
     *
     * @param min_temp Minimum allowed temperature
     * @param max_temp Maximum allowed temperature
     * @param min_extrude_temp Minimum extrusion temperature (default: 170°C)
     */
    void set_limits(int min_temp, int max_temp, int min_extrude_temp = 170);

    /**
     * @brief Set TemperatureService for combined temperature graph
     *
     * @param tcp Pointer to TemperatureService (for mini combined graph)
     */
    void set_temp_control_panel(TemperatureService* tcp) {
        temp_control_panel_ = tcp;
    }

  private:
    //
    // === Subjects (owned by this panel) ===
    //

    SubjectManager subjects_;
    lv_subject_t temp_display_subject_;
    lv_subject_t status_subject_;
    lv_subject_t material_selected_subject_;
    lv_subject_t extrusion_allowed_subject_;
    lv_subject_t safety_warning_visible_subject_;
    lv_subject_t warning_temps_subject_;
    lv_subject_t
        safety_warning_text_subject_; ///< "Heat to at least X°C to load/unload" with dynamic temp
    lv_subject_t material_nozzle_temp_subject_;
    lv_subject_t material_bed_temp_subject_;

    // Nozzle label (dynamic: "Nozzle" or "Nozzle N" for multi-tool)
    lv_subject_t nozzle_label_subject_;
    char nozzle_label_buf_[32] = {};
    ObserverGuard active_tool_observer_;
    void update_nozzle_label();

    // Left card temperature subjects (current and target for nozzle/bed)
    lv_subject_t nozzle_current_subject_;
    lv_subject_t nozzle_target_subject_;
    lv_subject_t bed_current_subject_;
    lv_subject_t bed_target_subject_;
    lv_subject_t chamber_current_subject_;
    lv_subject_t chamber_target_subject_;

    // Operation state
    OperationTimeoutGuard operation_guard_;

    // Cooldown button visibility (1 when nozzle target > 0, 0 otherwise)
    lv_subject_t nozzle_heating_subject_;

    // Purge amount button active subjects (boolean: 0=inactive, 1=active)
    // Using separate subjects because bind_style doesn't work with multiple ref_values
    lv_subject_t purge_5mm_active_subject_;
    lv_subject_t purge_10mm_active_subject_;
    lv_subject_t purge_25mm_active_subject_;

    // Purge amount state
    int purge_amount_ = 10; // Default 10mm

    // Preset button temperature label subjects (e.g., "210°C / 60°C")
    lv_subject_t preset_temps_subjects_[4];
    char preset_temps_bufs_[4][24];

    // Subject storage buffers
    char temp_display_buf_[32];
    char status_buf_[64];
    char warning_temps_buf_[64];
    char safety_warning_text_buf_[48]; ///< "Heat to at least X°C to load/unload"
    char material_nozzle_buf_[16];
    char material_bed_buf_[16];
    char nozzle_current_buf_[16];
    char nozzle_target_buf_[16];
    char bed_current_buf_[16];
    char bed_target_buf_[16];
    char chamber_current_buf_[16];
    char chamber_target_buf_[16];

    //
    // === Instance State ===
    //

    int nozzle_current_ = 25;
    int nozzle_target_ = 0;
    int bed_current_ = 25;
    int bed_target_ = 0;
    int chamber_current_ = 25; ///< Chamber current temperature (degrees, observer converts)
    int chamber_target_ = 0;   ///< Chamber target temperature (centidegrees, matches PrinterState)
    int prev_nozzle_target_ = -1; ///< Previous target for change detection in update_all_temps
    int prev_bed_target_ = -1;    ///< Previous target for change detection in update_all_temps
    int selected_material_ = -1;  // -1=none, 0=PLA, 1=PETG, 2=ABS, 3=TPU
    int nozzle_min_temp_ = 0;
    int nozzle_max_temp_ = 500;
    int bed_max_temp_ = 150;
    int chamber_max_temp_ = 150;
    int min_extrude_temp_ = 170; ///< Klipper's min_extrude_temp (default 170°C)

    // Auto-preheat state for filament operations
    enum class PreheatOp { NONE, LOAD, UNLOAD, EXTRUDE, RETRACT, PURGE };
    static const char* preheat_op_name(PreheatOp op);
    PreheatOp pending_preheat_op_ = PreheatOp::NONE;
    int pending_preheat_target_ = 0; ///< Target temp in °C for pending preheat
    int prior_nozzle_target_ = 0; ///< Nozzle target before preheat (0 = was off → cool down after)

    // Filament macros now resolved via StandardMacros singleton (load, unload, purge)

    // Child widgets (for imperative state management)
    // Action buttons (btn_load_, btn_unload_, btn_purge_) - disabled state managed by XML bindings
    lv_obj_t* safety_warning_ = nullptr;
    lv_obj_t* status_icon_ = nullptr;
    lv_obj_t* preset_buttons_[4] = {nullptr};

    // Dynamic spool preset button (shown when active material != standard preset)
    lv_obj_t* spool_preset_row_ = nullptr;
    lv_obj_t* spool_preset_button_ = nullptr;
    lv_obj_t* spool_preset_label_ = nullptr;
    lv_obj_t* spool_preset_temps_ = nullptr;
    std::optional<helix::ActiveMaterial> cached_active_material_;

    // Temperature labels for color updates (4-state heating color)
    lv_obj_t* nozzle_current_label_ = nullptr;
    lv_obj_t* bed_current_label_ = nullptr;
    lv_obj_t* chamber_current_label_ = nullptr;

    // Warning dialogs for filament sensor integration
    lv_obj_t* load_warning_dialog_ = nullptr;
    lv_obj_t* unload_warning_dialog_ = nullptr;

    // Temperature graph (managed by TemperatureService)
    TemperatureService* temp_control_panel_ = nullptr;

    // Temperature graph (for dynamic sizing when bottom card changes)
    lv_obj_t* temp_graph_card_ = nullptr;

    // Spool card widgets — serves both Multi-Filament (AMS/multi-tool) and
    // External Spool presentations; C++ swaps the visible rows.
    lv_obj_t* spool_card_ = nullptr;
    lv_obj_t* spool_card_header_row_ = nullptr;
    lv_obj_t* extruder_selector_group_ = nullptr;
    lv_obj_t* extruder_dropdown_ = nullptr;
    lv_obj_t* btn_manage_slots_ = nullptr;
    lv_obj_t* ams_manage_row_ = nullptr;
    ObserverGuard tools_version_observer_;

    void populate_extruder_dropdown();
    void update_multi_filament_card_visibility();
    void apply_left_column_sizing(bool external_spool_mode);
    void handle_extruder_changed();
    static void on_extruder_dropdown_changed(lv_event_t* e);

    // External spool display (no-AMS mode)
    lv_obj_t* external_spool_row_ = nullptr;
    lv_obj_t* external_spool_container_ = nullptr;
    lv_obj_t* external_spool_canvas_ = nullptr;
    lv_obj_t* external_spool_material_label_ = nullptr;
    lv_obj_t* external_spool_color_label_ = nullptr;
    ObserverGuard external_spool_observer_;
    lv_subject_t card_title_subject_;
    char card_title_buf_[32] = {};
    std::unique_ptr<helix::ui::AmsEditModal> edit_modal_;

    void setup_external_spool_display();
    void update_external_spool_from_state();
    void show_external_spool_edit_modal();
    static void on_external_spool_edit_clicked(lv_event_t* e);

    // Temperature observer bundle (nozzle + bed current/target)
    helix::ui::TemperatureObserverBundle<FilamentPanel> temp_observers_;
    ObserverGuard ams_type_observer_;       ///< Adjusts temp card size when AMS hidden
    ObserverGuard chamber_temp_observer_;   ///< Chamber temperature observer
    ObserverGuard chamber_target_observer_; ///< Chamber target temperature observer
    ObserverGuard ams_action_observer_; ///< Ends operation guard when AMS action returns to idle

    //
    // === Private Helpers ===
    //

    void update_temp_display();
    void update_status();
    void update_status_icon(const char* icon_name, const char* color_token);
    void update_warning_text();
    void update_safety_state();
    void update_preset_buttons_visual();
    void update_preset_button_temps();   ///< Update preset button labels from filament DB
    void check_and_auto_select_preset(); ///< Auto-select preset if targets match
    void update_all_temps();             ///< Unified handler for temp observer bundle
    void check_pending_preheat();        ///< Called from update_all_temps()
    void cancel_pending_preheat();       ///< Reset preheat state + notify
    struct PreheatTempResult {
        int temp = 0;
        std::string material_name;
    };
    PreheatTempResult
    resolve_preheat_temp() const; ///< Priority: ext spool > AMS slot > preset > fallback
    bool
    has_active_spool_material() const; ///< True if external spool or AMS slot has known material
    void start_preheat_for_op(PreheatOp op); ///< Resolve temp, heat, set pending state
    void restore_heater_after_preheat();     ///< Cool down if heater was off before preheat

    //
    // === Instance Handlers ===
    //

    void handle_preset_button(int material_id);
    void handle_spool_preset_button();
    void update_spool_preset();
    void handle_nozzle_temp_tap();
    void handle_bed_temp_tap();
    void handle_chamber_temp_tap();
    void handle_custom_chamber_confirmed(float value);
    void handle_custom_nozzle_confirmed(float value);
    void handle_custom_bed_confirmed(float value);
    void handle_load_button();
    void handle_unload_button();
    void handle_extrude_button();
    void handle_purge_button();
    void handle_retract_button();
    void handle_purge_amount_select(int amount);
    void handle_cooldown();
    void update_material_temp_display();
    void update_chamber_temp_display();
    void update_left_card_temps();
    void update_status_icon_for_state();
    static constexpr uint32_t OPERATION_TIMEOUT_MS =
        120000; // 2 min — purge/extrude at slow feedrate

    // Filament sensor warning helpers
    void show_load_warning();
    void show_unload_warning();
    void execute_load();
    void execute_unload();
    void execute_extrude();
    void execute_retract();
    void execute_purge();
    void run_filament_macro(const std::string& macro_name, const std::string& op_label,
                            const helix::MacroParamResult& params);

    int current_extruder_target() const;
    void snapshot_prior_heater_target();

    //
    // === Static Trampolines ===
    //

    // XML event_cb callbacks (global accessor pattern)
    static void on_manage_slots_clicked(lv_event_t* e);
    static void on_load_clicked(lv_event_t* e);
    static void on_unload_clicked(lv_event_t* e);
    static void on_extrude_clicked(lv_event_t* e);
    static void on_purge_clicked(lv_event_t* e);
    static void on_retract_clicked(lv_event_t* e);

    // Material preset callbacks (XML event_cb)
    static void on_preset_pla_clicked(lv_event_t* e);
    static void on_preset_petg_clicked(lv_event_t* e);
    static void on_preset_abs_clicked(lv_event_t* e);
    static void on_preset_tpu_clicked(lv_event_t* e);
    static void on_preset_spool_clicked(lv_event_t* e);

    // Temperature tap callbacks (XML event_cb)
    static void on_nozzle_temp_tap_clicked(lv_event_t* e);
    static void on_bed_temp_tap_clicked(lv_event_t* e);
    static void on_nozzle_target_tap_clicked(lv_event_t* e);
    static void on_bed_target_tap_clicked(lv_event_t* e);
    static void on_filament_chamber_target_tap(lv_event_t* e);

    // Purge amount callbacks (XML event_cb)
    static void on_purge_5mm_clicked(lv_event_t* e);
    static void on_purge_10mm_clicked(lv_event_t* e);
    static void on_purge_25mm_clicked(lv_event_t* e);

    // Cooldown callback (XML event_cb)
    static void on_cooldown_clicked(lv_event_t* e);

    // Keypad callback bridges (different signature - not LVGL events)
    static void custom_nozzle_keypad_cb(float value, void* user_data);
    static void custom_bed_keypad_cb(float value, void* user_data);

    // Filament sensor warning dialog callbacks
    static void on_load_warning_proceed(lv_event_t* e);
    static void on_load_warning_cancel(lv_event_t* e);
    static void on_unload_warning_proceed(lv_event_t* e);
    static void on_unload_warning_cancel(lv_event_t* e);
};

// Global instance accessor (needed by main.cpp)
FilamentPanel& get_global_filament_panel();
