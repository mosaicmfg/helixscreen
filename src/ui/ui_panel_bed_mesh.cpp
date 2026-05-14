// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_panel_bed_mesh.cpp
 * @brief Bed mesh visualization panel with 3D preview and profile management
 *
 * @pattern GOLD STANDARD - declarative XML + subject bindings, no imperative widget manipulation
 * @threading Destruction flag guards async callbacks
 *
 * @see Referenced in CLAUDE.md as exemplar
 */

#include "ui_panel_bed_mesh.h"

#include "ui_bed_mesh.h"
#include "ui_callback_helpers.h"
#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_global_panel_helper.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

using namespace helix;

#include <cstdio>
#include <cstring>
#include <limits>

// ============================================================================
// Forward declarations for static event callbacks
// ============================================================================
static void on_profile_clicked_cb(lv_event_t* e);
static void on_profile_rename_cb(lv_event_t* e);
static void on_profile_delete_cb(lv_event_t* e);
static void on_calibrate_header_clicked_cb(lv_event_t* e);
static void on_calibrate_cancel_cb(lv_event_t* e);
static void on_calibrate_start_cb(lv_event_t* e);
static void on_rename_cancel_cb(lv_event_t* e);
static void on_rename_confirm_cb(lv_event_t* e);
static void on_delete_cancel_cb(lv_event_t* e);
static void on_delete_confirm_cb(lv_event_t* e);
static void on_save_config_no_cb(lv_event_t* e);
static void on_save_config_yes_cb(lv_event_t* e);
static void on_emergency_stop_cb(lv_event_t* e);
static void on_save_profile_cb(lv_event_t* e);

// ============================================================================
// Constructor / Destructor
// ============================================================================

BedMeshPanel::BedMeshPanel() {
    // Initialize buffer contents
    std::memset(profile_name_buf_, 0, sizeof(profile_name_buf_));
    std::strncpy(dimensions_buf_, "No mesh data", sizeof(dimensions_buf_) - 1);
    std::memset(max_label_buf_, 0, sizeof(max_label_buf_));
    std::memset(max_value_buf_, 0, sizeof(max_value_buf_));
    std::memset(min_label_buf_, 0, sizeof(min_label_buf_));
    std::memset(min_value_buf_, 0, sizeof(min_value_buf_));
    std::memset(variance_buf_, 0, sizeof(variance_buf_));
    std::memset(rename_old_name_buf_, 0, sizeof(rename_old_name_buf_));
    std::memset(probe_text_buf_, 0, sizeof(probe_text_buf_));
    std::memset(error_message_buf_, 0, sizeof(error_message_buf_));

    // Initialize profile buffers
    for (int i = 0; i < BED_MESH_MAX_PROFILES; i++) {
        std::memset(profile_name_bufs_[static_cast<size_t>(i)].data(), 0, 64);
        std::memset(profile_range_bufs_[static_cast<size_t>(i)].data(), 0, 32);
    }

    spdlog::trace("[BedMeshPanel] Instance created");
}

BedMeshPanel::~BedMeshPanel() {
    deinit_subjects();

    // During shutdown, MoonrakerClient may already be destroyed - release subscription
    // guard WITHOUT trying to unsubscribe (matches pattern in ams_backend_*.cpp)
    subscription_.release();

    // CRITICAL: Check if LVGL is still initialized before calling LVGL functions.
    // During static destruction, LVGL may already be torn down.
    if (lv_is_initialized()) {
        // Modal dialogs: use helix::ui::modal_hide() - NOT lv_obj_del()!
        // See docs/DEVELOPER_QUICK_REFERENCE.md "Modal Dialog Lifecycle"
        if (calibrate_modal_widget_) {
            helix::ui::modal_hide(calibrate_modal_widget_);
            calibrate_modal_widget_ = nullptr;
        }
        if (rename_modal_widget_) {
            helix::ui::modal_hide(rename_modal_widget_);
            rename_modal_widget_ = nullptr;
        }
        if (save_config_modal_widget_) {
            helix::ui::modal_hide(save_config_modal_widget_);
            save_config_modal_widget_ = nullptr;
        }
        if (delete_modal_widget_) {
            helix::ui::modal_hide(delete_modal_widget_);
            delete_modal_widget_ = nullptr;
        }
    }

    // Clear widget pointers (LVGL owns the objects)
    canvas_ = nullptr;
    calibrate_name_input_ = nullptr;
    rename_name_input_ = nullptr;
}

// ============================================================================
// Subject Initialization
// ============================================================================

void BedMeshPanel::init_subjects() {
    init_subjects_guarded([this]() {
        // Current mesh stats subjects
        UI_MANAGED_SUBJECT_INT(bed_mesh_available_, 0, "bed_mesh_available", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_profile_name_, profile_name_buf_, "",
                                  "bed_mesh_profile_name", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_dimensions_, dimensions_buf_, "No mesh data",
                                  "bed_mesh_dimensions", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_max_label_, max_label_buf_, "Max", "bed_mesh_max_label",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_max_value_, max_value_buf_, "--", "bed_mesh_max_value",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_min_label_, min_label_buf_, "Min", "bed_mesh_min_label",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_min_value_, min_value_buf_, "--", "bed_mesh_min_value",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_variance_, variance_buf_, "", "bed_mesh_variance",
                                  subjects_);

        // Profile count
        UI_MANAGED_SUBJECT_INT(bed_mesh_profile_count_, 0, "bed_mesh_profile_count", subjects_);

        // Profile list subjects (5 profiles)
        for (int i = 0; i < BED_MESH_MAX_PROFILES; i++) {
            std::string name_key = "bed_mesh_profile_" + std::to_string(i) + "_name";
            std::string range_key = "bed_mesh_profile_" + std::to_string(i) + "_range";
            std::string active_key = "bed_mesh_profile_" + std::to_string(i) + "_active";

            auto idx = static_cast<size_t>(i);
            char* name_buf = profile_name_bufs_[idx].data();
            char* range_buf = profile_range_bufs_[idx].data();

            // Initialize with 5-arg form: (subject, buf, prev_buf, size, initial_value)
            lv_subject_init_string(&profile_name_subjects_[idx], name_buf, nullptr,
                                   profile_name_bufs_[idx].size(), "");
            lv_xml_register_subject(nullptr, name_key.c_str(), &profile_name_subjects_[idx]);
            subjects_.register_subject(&profile_name_subjects_[idx]);

            lv_subject_init_string(&profile_range_subjects_[idx], range_buf, nullptr,
                                   profile_range_bufs_[idx].size(), "");
            lv_xml_register_subject(nullptr, range_key.c_str(), &profile_range_subjects_[idx]);
            subjects_.register_subject(&profile_range_subjects_[idx]);

            lv_subject_init_int(&profile_active_subjects_[idx], 0);
            lv_xml_register_subject(nullptr, active_key.c_str(), &profile_active_subjects_[idx]);
            subjects_.register_subject(&profile_active_subjects_[idx]);
        }

        // Modal state subjects (NOT visibility - internal state only)
        UI_MANAGED_SUBJECT_INT(bed_mesh_calibrating_, 0, "bed_mesh_calibrating", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_rename_old_name_, rename_old_name_buf_, "",
                                  "bed_mesh_rename_old_name", subjects_);
        // Note: All modals now use helix::ui::modal_show() pattern instead of visibility subjects

        // Calibration state machine subjects
        UI_MANAGED_SUBJECT_INT(bed_mesh_calibrate_state_, 0, "bed_mesh_calibrate_state", subjects_);
        UI_MANAGED_SUBJECT_INT(bed_mesh_probe_progress_, 0, "bed_mesh_probe_progress", subjects_);
        UI_MANAGED_SUBJECT_INT(bed_mesh_probe_indeterminate_, 0, "bed_mesh_probe_indeterminate",
                               subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_probe_text_, probe_text_buf_, "Preparing...",
                                  "bed_mesh_probe_text", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_mesh_error_message_, error_message_buf_, "",
                                  "bed_mesh_error_message", subjects_);

        // Self-register cleanup — ensures deinit runs before lv_deinit()
        StaticPanelRegistry::instance().register_destroy(
            "BedMeshPanelSubjects", []() { get_global_bed_mesh_panel().deinit_subjects(); });

        spdlog::debug("[{}] Subjects registered", get_name());
    });
}

void BedMeshPanel::deinit_subjects() {
    deinit_subjects_base(subjects_);
}

// ============================================================================
// Create
// ============================================================================

lv_obj_t* BedMeshPanel::create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[{}] Cannot create: null parent", get_name());
        return nullptr;
    }

    spdlog::debug("[{}] Creating overlay from XML", get_name());

    parent_screen_ = parent;

    // Reset cleanup flag when (re)creating
    cleanup_called_ = false;

    // Create overlay from XML
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "bed_mesh_panel", nullptr));

    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create from XML", get_name());
        return nullptr;
    }

    // Use standard overlay panel setup
    // Note: Back button is wired via header_bar.xml default callback (on_header_back_clicked)
    ui_overlay_panel_setup_standard(overlay_root_, parent_screen_, "overlay_header",
                                    "overlay_content");

    lv_obj_t* overlay_content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (!overlay_content) {
        spdlog::error("[{}] overlay_content not found!", get_name());
        return overlay_root_;
    }

    // Find canvas widget
    canvas_ = lv_obj_find_by_name(overlay_content, "bed_mesh_canvas");
    if (!canvas_) {
        spdlog::error("[{}] Canvas widget 'bed_mesh_canvas' not found in XML", get_name());
        return overlay_root_;
    }
    spdlog::debug("[{}] Found canvas widget - rotation controlled by touch drag", get_name());

    // Setup Moonraker subscription for mesh updates
    setup_moonraker_subscription();

    // Setup observer for build_volume changes (to refresh bounds when stepper config loads)
    setup_build_volume_observer();

    // Load initial mesh data from MoonrakerAPI
    MoonrakerAPI* api = get_moonraker_api();
    if (api && api->advanced().has_bed_mesh()) {
        const BedMeshProfile* mesh = api->advanced().get_active_bed_mesh();
        if (mesh) {
            spdlog::info("[{}] Active mesh: profile='{}', size={}x{}", get_name(), mesh->name,
                         mesh->x_count, mesh->y_count);
            on_mesh_update_internal(*mesh);
        }
    } else {
        spdlog::info("[{}] No mesh data available from Moonraker", get_name());
    }

    // Always update profile list — saved profiles exist even without an active mesh
    if (api) {
        update_profile_list_subjects();
    }

    // Apply saved render mode preference from settings
    int saved_mode = DisplaySettingsManager::instance().get_bed_mesh_render_mode();
    auto render_mode = static_cast<BedMeshRenderMode>(saved_mode);
    ui_bed_mesh_set_render_mode(canvas_, render_mode);
    spdlog::debug("[{}] Render mode set from settings: {} ({})", get_name(), saved_mode,
                  saved_mode == 0 ? "Auto" : (saved_mode == 1 ? "3D" : "2D"));

    // Apply zero plane visibility from settings
    bool show_zero_plane = DisplaySettingsManager::instance().get_bed_mesh_show_zero_plane();
    ui_bed_mesh_set_zero_plane_visible(canvas_, show_zero_plane);
    spdlog::debug("[{}] Zero plane visibility set from settings: {}", get_name(), show_zero_plane);

    // Evaluate render mode based on FPS history from previous sessions
    // This decides whether to use 3D or 2D fallback mode for AUTO mode
    ui_bed_mesh_evaluate_render_mode(canvas_);

    // Initially hidden
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created successfully", get_name());
    return overlay_root_;
}

// ============================================================================
// Callback Registration
// ============================================================================

void BedMeshPanel::register_callbacks() {
    if (callbacks_registered_) {
        spdlog::debug("[{}] Callbacks already registered", get_name());
        return;
    }

    spdlog::debug("[{}] Registering event callbacks", get_name());

    register_xml_callbacks({
        // Header calibrate button
        {"on_bed_mesh_calibrate_clicked", on_calibrate_header_clicked_cb},

        // Profile row callbacks (5 profiles)
        {"on_profile_0_clicked", on_profile_clicked_cb},
        {"on_profile_1_clicked", on_profile_clicked_cb},
        {"on_profile_2_clicked", on_profile_clicked_cb},
        {"on_profile_3_clicked", on_profile_clicked_cb},
        {"on_profile_4_clicked", on_profile_clicked_cb},

        {"on_profile_0_rename", on_profile_rename_cb},
        {"on_profile_1_rename", on_profile_rename_cb},
        {"on_profile_2_rename", on_profile_rename_cb},
        {"on_profile_3_rename", on_profile_rename_cb},
        {"on_profile_4_rename", on_profile_rename_cb},

        {"on_profile_0_delete", on_profile_delete_cb},
        {"on_profile_1_delete", on_profile_delete_cb},
        {"on_profile_2_delete", on_profile_delete_cb},
        {"on_profile_3_delete", on_profile_delete_cb},
        {"on_profile_4_delete", on_profile_delete_cb},

        // Calibrate modal
        {"on_bed_mesh_calibrate_cancel", on_calibrate_cancel_cb},
        {"on_bed_mesh_calibrate_start", on_calibrate_start_cb},

        // Rename modal
        {"on_bed_mesh_rename_cancel", on_rename_cancel_cb},
        {"on_bed_mesh_rename_confirm", on_rename_confirm_cb},

        // Delete modal
        {"on_bed_mesh_delete_cancel", on_delete_cancel_cb},
        {"on_bed_mesh_delete_confirm", on_delete_confirm_cb},

        // Save config modal
        {"on_bed_mesh_save_config_no", on_save_config_no_cb},
        {"on_bed_mesh_save_config_yes", on_save_config_yes_cb},

        // Calibration modal - emergency stop and save profile
        {"on_bed_mesh_emergency_stop", on_emergency_stop_cb},
        {"on_bed_mesh_save_profile", on_save_profile_cb},
    });

    callbacks_registered_ = true;
    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void BedMeshPanel::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    spdlog::debug("[{}] on_activate()", get_name());

    // Defer layout computation and data loading to after the current timer cycle.
    // on_activate() runs inside push_overlay()'s queue_update lambda, which fires
    // during lv_timer_handler(). Calling lv_obj_update_layout() here can crash in
    // layout_update_core() because lv_obj_move_foreground() just modified the
    // children array in the same cycle (#417, #419).
    lifetime_.defer("BedMeshPanel::activate_layout", [this]() {
        if (canvas_) {
            lv_obj_update_layout(canvas_);
        }

        MoonrakerAPI* api = get_moonraker_api();
        if (api && api->advanced().has_bed_mesh()) {
            const BedMeshProfile* mesh = api->advanced().get_active_bed_mesh();
            if (mesh) {
                on_mesh_update_internal(*mesh);
            }
        }

        if (api) {
            update_profile_list_subjects();
        }

        ensure_async_rendering();
    });
}

void BedMeshPanel::ensure_async_rendering() {
    if (!canvas_) {
        return;
    }

    // Don't start the render thread if there's no mesh data — it would just
    // fail and show "Rendering..." on top of the "No mesh loaded" overlay.
    if (!ui_bed_mesh_has_data(canvas_)) {
        return;
    }

    if (!ui_bed_mesh_is_async_mode(canvas_)) {
        lv_obj_update_layout(canvas_);
        ui_bed_mesh_set_async_mode(canvas_, true);
    }

    lv_obj_invalidate(canvas_);
    ui_bed_mesh_request_async_render(canvas_);
}

void BedMeshPanel::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());

    // Stop the render thread when panel is not visible to avoid wasting CPU
    if (canvas_) {
        ui_bed_mesh_set_async_mode(canvas_, false);
    }

    // Call base class
    OverlayBase::on_deactivate();
}

void BedMeshPanel::on_ui_destroyed() {
    canvas_ = nullptr;
    profile_dropdown_ = nullptr;
    calibrate_name_input_ = nullptr;
    rename_name_input_ = nullptr;
    calibrate_modal_widget_ = nullptr;
    rename_modal_widget_ = nullptr;
    save_config_modal_widget_ = nullptr;
    delete_modal_widget_ = nullptr;
    build_volume_observer_.reset();
}

// ============================================================================
// Profile List Update
// ============================================================================

void BedMeshPanel::update_profile_list_subjects() {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        lv_subject_set_int(&bed_mesh_profile_count_, 0);
        return;
    }

    const auto raw_profiles = api->advanced().get_bed_mesh_profiles();
    const BedMeshProfile* active_mesh = api->advanced().get_active_bed_mesh();
    std::string active_name = active_mesh ? active_mesh->name : "";

    // Filter out the temporary calibration profile used internally
    std::vector<std::string> profiles;
    profiles.reserve(raw_profiles.size());
    for (const auto& p : raw_profiles) {
        if (p != "_hs_temp")
            profiles.push_back(p);
    }

    spdlog::debug("[{}] update_profile_list_subjects: {} profiles, active='{}'", get_name(),
                  profiles.size(), active_name);

    int count = std::min(static_cast<int>(profiles.size()), BED_MESH_MAX_PROFILES);
    lv_subject_set_int(&bed_mesh_profile_count_, count);

    for (int i = 0; i < BED_MESH_MAX_PROFILES; i++) {
        size_t idx = static_cast<size_t>(i);
        if (i < count) {
            profile_names_[idx] = profiles[idx];

            // Set name
            lv_subject_copy_string(&profile_name_subjects_[idx], profiles[idx].c_str());

            // Calculate and set range (mm without suffix for profile lists)
            float range = calculate_profile_range(profiles[idx]);
            std::snprintf(profile_range_bufs_[idx].data(), 32, "%.3f", range);
            lv_subject_copy_string(&profile_range_subjects_[idx], profile_range_bufs_[idx].data());

            // Set active state
            int is_active = (profiles[idx] == active_name) ? 1 : 0;
            lv_subject_set_int(&profile_active_subjects_[idx], is_active);
        } else {
            profile_names_[idx].clear();
            lv_subject_copy_string(&profile_name_subjects_[idx], "");
            lv_subject_copy_string(&profile_range_subjects_[idx], "");
            lv_subject_set_int(&profile_active_subjects_[idx], 0);
        }
    }

    spdlog::debug("[{}] Profile list updated: {} profiles, active='{}'", get_name(), count,
                  active_name);
}

float BedMeshPanel::calculate_profile_range(const std::string& profile_name) {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return 0.0f;

    // Get mesh data for this profile (supports both active and stored profiles)
    const BedMeshProfile* mesh = api->advanced().get_bed_mesh_profile(profile_name);
    if (!mesh || mesh->probed_matrix.empty()) {
        return 0.0f;
    }

    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();

    for (const auto& row : mesh->probed_matrix) {
        for (float z : row) {
            min_z = std::min(min_z, z);
            max_z = std::max(max_z, z);
        }
    }

    return max_z - min_z;
}

// ============================================================================
// Mesh Data Update
// ============================================================================

void BedMeshPanel::set_mesh_data(const std::vector<std::vector<float>>& mesh_data) {
    if (!canvas_) {
        spdlog::error("[{}] Cannot set mesh data - canvas not initialized", get_name());
        return;
    }

    if (mesh_data.empty() || mesh_data[0].empty()) {
        spdlog::error("[{}] Invalid mesh data - empty rows or columns", get_name());
        return;
    }

    int rows = static_cast<int>(mesh_data.size());
    int cols = static_cast<int>(mesh_data[0].size());

    std::vector<const float*> row_pointers(static_cast<size_t>(rows));
    for (int i = 0; i < rows; i++) {
        row_pointers[static_cast<size_t>(i)] = mesh_data[static_cast<size_t>(i)].data();
    }

    if (!ui_bed_mesh_set_data(canvas_, row_pointers.data(), rows, cols)) {
        spdlog::error("[{}] Failed to set mesh data in widget", get_name());
        return;
    }

    update_info_subjects(mesh_data, cols, rows);
}

void BedMeshPanel::redraw() {
    if (!canvas_) {
        spdlog::warn("[{}] Cannot redraw - canvas not initialized", get_name());
        return;
    }
    ui_bed_mesh_redraw(canvas_);
}

void BedMeshPanel::setup_moonraker_subscription() {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] Cannot subscribe to Moonraker - API is null", get_name());
        return;
    }

    auto token = lifetime_.token();

    SubscriptionId id =
        api->subscribe_notifications([this, api, token](nlohmann::json notification) {
            // Check if this notification contains bed_mesh data BEFORE deferring to main thread
            if (!notification.contains("params") || !notification["params"].is_array() ||
                notification["params"].empty()) {
                return;
            }
            const nlohmann::json& params = notification["params"][0];
            if (!params.contains("bed_mesh") || !params["bed_mesh"].is_object()) {
                return;
            }

            // Defer LVGL modifications to main thread
            token.defer("BedMeshPanel::mesh_update", [this, api]() {
                const BedMeshProfile* mesh = api->advanced().get_active_bed_mesh();
                if (mesh) {
                    on_mesh_update_internal(*mesh);
                }
                update_profile_list_subjects();
            });
        });

    // Store in RAII guard for automatic cleanup on destruction
    subscription_ = SubscriptionGuard(api, id);
    spdlog::debug("[{}] Registered Moonraker callback for mesh updates", get_name());
}

void BedMeshPanel::setup_build_volume_observer() {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] Cannot observe build_volume - API is null", get_name());
        return;
    }

    // Observe build_volume_version subject to refresh bounds when stepper config loads
    build_volume_observer_ = helix::ui::observe_int_sync<BedMeshPanel>(
        api->get_build_volume_version_subject(), this, [](BedMeshPanel* self, int /*version*/) {
            spdlog::debug("[{}] build_volume changed, refreshing bed bounds", self->get_name());
            self->refresh_bed_bounds();
        });
}

void BedMeshPanel::refresh_bed_bounds() {
    if (!canvas_ || !has_cached_mesh_bounds_) {
        return;
    }

    MoonrakerAPI* api = get_moonraker_api();
    const auto& bed = api ? api->hardware().build_volume() : BuildVolume{};
    double bed_x_min = bed.x_min;
    double bed_x_max = bed.x_max;
    double bed_y_min = bed.y_min;
    double bed_y_max = bed.y_max;

    // Wait for valid build_volume - do NOT use fallback to avoid flash
    if (bed_x_max <= bed_x_min || bed_y_max <= bed_y_min) {
        spdlog::debug("[{}] Deferring render until build_volume is available", get_name());
        return;
    }

    spdlog::debug("[{}] Using build_volume for bed bounds: X[{:.0f},{:.0f}] Y[{:.0f},{:.0f}]",
                  get_name(), bed_x_min, bed_x_max, bed_y_min, bed_y_max);

    ui_bed_mesh_set_bounds(canvas_, bed_x_min, bed_x_max, bed_y_min, bed_y_max, cached_mesh_min_x_,
                           cached_mesh_max_x_, cached_mesh_min_y_, cached_mesh_max_y_);

    // If we have pending mesh data, render it now that bounds are valid
    if (has_pending_mesh_data_) {
        spdlog::debug("[{}] Rendering deferred mesh data", get_name());
        set_mesh_data(pending_mesh_data_);
        pending_mesh_data_.clear();
        has_pending_mesh_data_ = false;
        // Start async rendering now that data is available
        ensure_async_rendering();
    }
}

void BedMeshPanel::on_mesh_update_internal(const BedMeshProfile& mesh) {
    spdlog::debug("[{}] on_mesh_update_internal called, probed_matrix.size={}", get_name(),
                  mesh.probed_matrix.size());

    if (mesh.probed_matrix.empty()) {
        lv_subject_set_int(&bed_mesh_available_, 0);
        lv_subject_copy_string(&bed_mesh_dimensions_, lv_tr("No mesh data"));
        lv_subject_copy_string(&bed_mesh_max_label_, lv_tr("Max"));
        lv_subject_copy_string(&bed_mesh_max_value_, "--");
        lv_subject_copy_string(&bed_mesh_min_label_, lv_tr("Min"));
        lv_subject_copy_string(&bed_mesh_min_value_, "--");
        lv_subject_copy_string(&bed_mesh_variance_, "");
        spdlog::warn("[{}] No mesh data available", get_name());
        return;
    }

    lv_subject_set_int(&bed_mesh_available_, 1);
    lv_subject_copy_string(&bed_mesh_profile_name_, mesh.name.c_str());

    std::snprintf(dimensions_buf_, sizeof(dimensions_buf_), "%dx%d", mesh.x_count, mesh.y_count);
    lv_subject_copy_string(&bed_mesh_dimensions_, dimensions_buf_);

    // Calculate Z range, mean, and coordinates of min/max points
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
    double z_sum = 0.0;
    int z_count = 0;
    int min_row = 0, min_col = 0;
    int max_row = 0, max_col = 0;

    for (size_t row = 0; row < mesh.probed_matrix.size(); row++) {
        for (size_t col = 0; col < mesh.probed_matrix[row].size(); col++) {
            float z = mesh.probed_matrix[row][col];
            z_sum += z;
            z_count++;
            if (z < min_z) {
                min_z = z;
                min_row = static_cast<int>(row);
                min_col = static_cast<int>(col);
            }
            if (z > max_z) {
                max_z = z;
                max_row = static_cast<int>(row);
                max_col = static_cast<int>(col);
            }
        }
    }

    // Normalize mesh data: subtract mean so deviations are centered around zero.
    // This makes the 3D visualization show bed flatness (what users care about)
    // rather than absolute probe heights (which depend on Z calibration).
    float z_mean = (z_count > 0) ? static_cast<float>(z_sum / z_count) : 0.0f;
    float norm_min_z = min_z - z_mean;
    float norm_max_z = max_z - z_mean;

    std::vector<std::vector<float>> normalized_matrix;
    normalized_matrix.reserve(mesh.probed_matrix.size());
    for (const auto& row : mesh.probed_matrix) {
        std::vector<float> norm_row;
        norm_row.reserve(row.size());
        for (float z : row) {
            norm_row.push_back(z - z_mean);
        }
        normalized_matrix.push_back(std::move(norm_row));
    }

    spdlog::debug(
        "[{}] Normalized mesh: mean={:.4f}, raw range [{:.3f}, {:.3f}] -> [{:.3f}, {:.3f}]",
        get_name(), z_mean, min_z, max_z, norm_min_z, norm_max_z);

    // Convert mesh indices to actual printer coordinates using mesh_min/mesh_max
    // Klipper's probed_matrix: row 0 = mesh_min[1], row N-1 = mesh_max[1]
    float x_step =
        (mesh.x_count > 1) ? (mesh.mesh_max[0] - mesh.mesh_min[0]) / (mesh.x_count - 1) : 0.0f;
    float y_step =
        (mesh.y_count > 1) ? (mesh.mesh_max[1] - mesh.mesh_min[1]) / (mesh.y_count - 1) : 0.0f;
    float min_x = mesh.mesh_min[0] + min_col * x_step;
    float min_y = mesh.mesh_min[1] + min_row * y_step;
    float max_x = mesh.mesh_min[0] + max_col * x_step;
    float max_y = mesh.mesh_min[1] + max_row * y_step;

    // Display raw Z values in stats (what Klipper actually measured)
    std::snprintf(max_label_buf_, sizeof(max_label_buf_), "Max [%.1f, %.1f]", max_x, max_y);
    lv_subject_copy_string(&bed_mesh_max_label_, max_label_buf_);
    helix::format::format_distance_mm(max_z, 3, max_value_buf_, sizeof(max_value_buf_));
    lv_subject_copy_string(&bed_mesh_max_value_, max_value_buf_);

    std::snprintf(min_label_buf_, sizeof(min_label_buf_), "Min [%.1f, %.1f]", min_x, min_y);
    lv_subject_copy_string(&bed_mesh_min_label_, min_label_buf_);
    helix::format::format_distance_mm(min_z, 3, min_value_buf_, sizeof(min_value_buf_));
    lv_subject_copy_string(&bed_mesh_min_value_, min_value_buf_);

    // Variance (range) is the same whether normalized or not
    float variance = max_z - min_z;
    helix::format::format_distance_mm(variance, 3, variance_buf_, sizeof(variance_buf_));
    lv_subject_copy_string(&bed_mesh_variance_, variance_buf_);

    // Cache mesh bounds
    if ((mesh.mesh_max[0] > mesh.mesh_min[0]) && (mesh.mesh_max[1] > mesh.mesh_min[1])) {
        cached_mesh_min_x_ = mesh.mesh_min[0];
        cached_mesh_max_x_ = mesh.mesh_max[0];
        cached_mesh_min_y_ = mesh.mesh_min[1];
        cached_mesh_max_y_ = mesh.mesh_max[1];
        has_cached_mesh_bounds_ = true;
    }

    // Tell the renderer to add back the mean when displaying Z values
    // so axis labels and tooltips show original probe heights
    if (canvas_) {
        ui_bed_mesh_set_z_display_offset(canvas_, static_cast<double>(z_mean));
    }

    // Check if build_volume is available
    MoonrakerAPI* api = get_moonraker_api();
    const auto& bed = api ? api->hardware().build_volume() : BuildVolume{};
    bool has_valid_build_volume = (bed.x_max > bed.x_min && bed.y_max > bed.y_min);

    spdlog::debug("[{}] BuildVolume check: x=[{:.0f},{:.0f}] y=[{:.0f},{:.0f}] valid={}, "
                  "mesh_bounds_cached={}",
                  get_name(), bed.x_min, bed.x_max, bed.y_min, bed.y_max, has_valid_build_volume,
                  has_cached_mesh_bounds_);

    if (has_valid_build_volume) {
        // Build volume available - set bounds and render immediately
        refresh_bed_bounds();
        set_mesh_data(normalized_matrix);
        // Start async rendering now that renderer has data (may be first data arrival
        // while panel is already visible — e.g. profile load or WebSocket update)
        ensure_async_rendering();
    } else {
        // Build volume not yet available - defer rendering until it arrives
        pending_mesh_data_ = std::move(normalized_matrix);
        has_pending_mesh_data_ = true;
        spdlog::debug("[{}] Deferring mesh render until build_volume is available", get_name());
    }

    spdlog::info("[{}] Mesh updated: {} ({}x{}, raw Z: [{:.3f}, {:.3f}], normalized: [{:.3f}, "
                 "{:.3f}])",
                 get_name(), mesh.name, mesh.x_count, mesh.y_count, min_z, max_z, norm_min_z,
                 norm_max_z);
}

void BedMeshPanel::update_info_subjects(const std::vector<std::vector<float>>& mesh_data, int cols,
                                        int rows) {
    std::snprintf(dimensions_buf_, sizeof(dimensions_buf_), "%dx%d points", cols, rows);
    lv_subject_copy_string(&bed_mesh_dimensions_, dimensions_buf_);

    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
    for (const auto& row : mesh_data) {
        for (float val : row) {
            min_z = std::min(min_z, val);
            max_z = std::max(max_z, val);
        }
    }

    float variance = max_z - min_z;
    std::snprintf(variance_buf_, sizeof(variance_buf_), "%.3f mm", variance);
    lv_subject_copy_string(&bed_mesh_variance_, variance_buf_);
}

// ============================================================================
// Profile Operations
// ============================================================================

void BedMeshPanel::load_profile(int index) {
    if (index < 0 || index >= BED_MESH_MAX_PROFILES)
        return;
    if (profile_names_[static_cast<size_t>(index)].empty())
        return;
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }

    const std::string& name = profile_names_[static_cast<size_t>(index)];
    spdlog::info("[{}] Loading profile: {}", get_name(), name);

    MoonrakerAPI* api = get_moonraker_api();
    if (api) {
        operation_guard_.begin(SLOW_OPERATION_TIMEOUT_MS, [this] {
            hide_all_modals();
            pending_operation_ = PendingOperation::None;
            NOTIFY_WARNING(lv_tr("Bed mesh operation timed out"));
        });
        auto token = lifetime_.token();
        std::string cmd = "BED_MESH_PROFILE LOAD=" + name;
        api->execute_gcode(
            cmd,
            [this, token, api, name]() {
                if (token.expired())
                    return;
                token.defer("BedMeshPanel::load_refresh", [this, api]() {
                    operation_guard_.end();
                    spdlog::debug("[{}] Profile loaded", get_name());
                    const BedMeshProfile* mesh = api->advanced().get_active_bed_mesh();
                    if (mesh) {
                        on_mesh_update_internal(*mesh);
                    }
                    update_profile_list_subjects();
                });
            },
            [this, token](const MoonrakerError& err) {
                if (token.expired())
                    return;
                auto msg = err.message;
                token.defer("BedMeshPanel::load_error", [this, msg]() {
                    operation_guard_.end();
                    spdlog::error("[{}] Failed to load profile: {}", get_name(), msg);
                    NOTIFY_ERROR(lv_tr("Failed to load profile"));
                });
            });
    }
}

void BedMeshPanel::delete_profile(int index) {
    if (index < 0 || index >= BED_MESH_MAX_PROFILES)
        return;
    if (profile_names_[static_cast<size_t>(index)].empty())
        return;

    const std::string& name = profile_names_[static_cast<size_t>(index)];
    show_delete_confirm_modal(name);
}

void BedMeshPanel::rename_profile(int index) {
    if (index < 0 || index >= BED_MESH_MAX_PROFILES)
        return;
    if (profile_names_[static_cast<size_t>(index)].empty())
        return;

    const std::string& name = profile_names_[static_cast<size_t>(index)];
    show_rename_modal(name);
}

void BedMeshPanel::preheat_for_probing() {
    preheat_turned_on_nozzle_ = false;
    preheat_turned_on_bed_ = false;

    MoonrakerAPI* api = get_moonraker_api();
    if (!api) return;

    auto& state = get_printer_state();

    // Subject values are centidegrees (value * 10) — target of 0 means heater is off
    int nozzle_target = lv_subject_get_int(state.get_active_extruder_target_subject());
    int bed_target = lv_subject_get_int(state.get_bed_target_subject());

    auto set_temp = [api](const std::string& heater, double temp, const char* label) {
        api->set_temperature(
            heater, temp, []() {},
            [label](const MoonrakerError& err) {
                spdlog::warn("[BedMeshPanel] Failed to preheat {}: {}", label, err.message);
            });
    };

    if (nozzle_target == 0) {
        preheat_turned_on_nozzle_ = true;
        spdlog::info("[BedMeshPanel] Preheating nozzle to {}°C for probing", PROBE_NOZZLE_TEMP);
        set_temp(state.active_extruder_name(), PROBE_NOZZLE_TEMP, "nozzle");
    } else {
        spdlog::info("[BedMeshPanel] Nozzle already heating (target={}°C), skipping",
                     nozzle_target / 10.0);
    }

    if (bed_target == 0) {
        preheat_turned_on_bed_ = true;
        spdlog::info("[BedMeshPanel] Preheating bed to {}°C for probing", PROBE_BED_TEMP);
        set_temp("heater_bed", PROBE_BED_TEMP, "bed");
    } else {
        spdlog::info("[BedMeshPanel] Bed already heating (target={}°C), skipping",
                     bed_target / 10.0);
    }
}

void BedMeshPanel::cooldown_after_probing() {
    if (!preheat_turned_on_nozzle_ && !preheat_turned_on_bed_) return;

    MoonrakerAPI* api = get_moonraker_api();
    if (!api) return;

    auto turn_off = [api](const std::string& heater, const char* label) {
        spdlog::info("[BedMeshPanel] Turning off {} (was off before probing)", label);
        api->set_temperature(
            heater, 0.0, []() {},
            [label](const MoonrakerError& err) {
                spdlog::warn("[BedMeshPanel] Failed to turn off {}: {}", label, err.message);
            });
    };

    if (preheat_turned_on_nozzle_) {
        turn_off(get_printer_state().active_extruder_name(), "nozzle");
        preheat_turned_on_nozzle_ = false;
    }

    if (preheat_turned_on_bed_) {
        turn_off("heater_bed", "bed");
        preheat_turned_on_bed_ = false;
    }
}

void BedMeshPanel::start_calibration() {
    // Prevent double-tap while already probing
    auto current_state = static_cast<BedMeshCalibrationState>(
        lv_subject_get_int(&bed_mesh_calibrate_state_));
    if (current_state == BedMeshCalibrationState::PROBING) {
        spdlog::debug("[BedMeshPanel] Calibration already in progress, ignoring");
        return;
    }

    // Reset state to PROBING
    lv_subject_set_int(&bed_mesh_calibrate_state_,
                       static_cast<int>(BedMeshCalibrationState::PROBING));
    lv_subject_set_int(&bed_mesh_probe_progress_, 0);
    lv_subject_set_int(&bed_mesh_probe_indeterminate_, 0);
    lv_subject_copy_string(&bed_mesh_probe_text_, lv_tr("Preparing..."));

    // Show modal immediately
    calibrate_modal_widget_ = helix::ui::modal_show("bed_mesh_calibrate_modal");
    spdlog::debug("[BedMeshPanel] Starting calibration, modal shown");

    // Get API
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_calibration_error("API not available");
        return;
    }

    // Preheat nozzle and bed for probing (respects existing targets)
    preheat_for_probing();

    // If we turned on any heaters, wait for them to reach target before proceeding
    if (preheat_turned_on_nozzle_ || preheat_turned_on_bed_) {
        lv_subject_set_int(&bed_mesh_probe_indeterminate_, 1);
        lv_subject_copy_string(&bed_mesh_probe_text_, lv_tr("Heating..."));

        // Build TEMPERATURE_WAIT command for heaters we turned on
        std::string wait_cmd;
        if (preheat_turned_on_nozzle_) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "TEMPERATURE_WAIT SENSOR=%s MINIMUM=%.0f\n",
                          get_printer_state().active_extruder_name().c_str(), PROBE_NOZZLE_TEMP);
            wait_cmd += buf;
        }
        if (preheat_turned_on_bed_) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "TEMPERATURE_WAIT SENSOR=heater_bed MINIMUM=%.0f",
                          PROBE_BED_TEMP);
            wait_cmd += buf;
        }

        spdlog::info("[BedMeshPanel] Waiting for preheat: {}", wait_cmd);

        auto token = lifetime_.token();
        api->execute_gcode(
            wait_cmd,
            [this, token]() {
                if (token.expired()) return;
                token.defer("BedMeshPanel::preheat_done", [this]() {
                    spdlog::info("[BedMeshPanel] Preheat complete, proceeding to home/probe");
                    start_home_and_probe();
                });
            },
            [this, token](const MoonrakerError& err) {
                if (token.expired()) return;
                std::string msg = "Preheat failed: " + err.message;
                token.defer("BedMeshPanel::preheat_error",
                            [this, msg]() { on_calibration_error(msg); });
            },
            CALIBRATION_TIMEOUT_MS);
    } else {
        // Heaters already on — go straight to home/probe
        start_home_and_probe();
    }
}

void BedMeshPanel::start_home_and_probe() {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_calibration_error("API not available");
        return;
    }

    // Check homing state — auto-home if needed before probing
    const char* homed = lv_subject_get_string(get_printer_state().get_homed_axes_subject());
    bool all_homed = homed && std::string(homed).find("xyz") != std::string::npos;

    if (all_homed) {
        start_calibration_probing();
    } else {
        spdlog::info("[BedMeshPanel] Not fully homed (axes={}), sending G28 first",
                     homed ? homed : "none");
        lv_subject_set_int(&bed_mesh_probe_indeterminate_, 1);
        lv_subject_copy_string(&bed_mesh_probe_text_, lv_tr("Homing..."));

        auto token = lifetime_.token();
        api->execute_gcode(
            "G28",
            [this, token]() {
                if (token.expired()) return;
                token.defer("BedMeshPanel::g28_done", [this]() {
                    spdlog::info("[BedMeshPanel] G28 complete, starting calibration");
                    start_calibration_probing();
                });
            },
            [this, token](const MoonrakerError& err) {
                if (token.expired()) return;
                std::string msg = (err.type == MoonrakerErrorType::TIMEOUT)
                                      ? "Homing timed out — printer may still be homing"
                                      : "Homing failed: " + err.message;
                token.defer("BedMeshPanel::g28_error",
                            [this, msg]() { on_calibration_error(msg); });
            },
            MoonrakerAPI::HOMING_TIMEOUT_MS);
    }
}

void BedMeshPanel::start_calibration_probing() {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_calibration_error("API not available");
        return;
    }

    lv_subject_copy_string(&bed_mesh_probe_text_, lv_tr("Preparing..."));

    auto token = lifetime_.token();

    // Query configfile for bed_mesh probe_count before starting calibration.
    // This gives us the expected total for firmware that only emits
    // "probe at X,Y" lines without a "Probing point X/Y" progress line.
    json params = {{"objects", json::object({{"configfile", json::array({"settings"})}})}};
    api->get_client().send_jsonrpc(
        "printer.objects.query", params,
        [this, api, token](json response) {
            // BG: parse JSON without touching `this`. Member call (launch_calibration)
            // happens inside the defer below.
            int expected = 0;
            int samples = 1;
            try {
                const auto& settings = response["result"]["status"]["configfile"]["settings"];
                if (settings.contains("bed_mesh") && settings["bed_mesh"].contains("probe_count")) {
                    const auto& pc = settings["bed_mesh"]["probe_count"];
                    if (pc.is_array() && pc.size() >= 2) {
                        expected = pc[0].template get<int>() * pc[1].template get<int>();
                    } else if (pc.is_number_integer()) {
                        int n = pc.template get<int>();
                        expected = n * n; // square grid
                    }
                }
                // Probe samples: each mesh point may be probed multiple times.
                // Check known probe sections — bltouch inherits from probe;
                // load_cell_probe (Centauri Carbon's mainline-Klipper strain
                // gauge module) has its own samples key.
                for (const char* section : {"probe", "bltouch", "load_cell_probe"}) {
                    if (settings.contains(section) && settings[section].contains("samples")) {
                        int s = settings[section]["samples"].template get<int>();
                        if (s > 1) {
                            samples = s;
                            break;
                        }
                    }
                }
            } catch (...) {
                // Non-fatal — proceed with 0 (indeterminate)
            }

            spdlog::info("[BedMeshPanel] Expected probe count from config: {} (samples={})",
                         expected, samples);
            // L081 Mechanism C: launch_calibration is a member fn; marshal to main.
            token.defer("BedMeshPanel::launch_after_query",
                        [this, api, token, expected, samples]() {
                            launch_calibration(api, token, expected, samples);
                        });
        },
        [this, api, token](const MoonrakerError& /*err*/) {
            // L081 Mechanism C: launch_calibration is a member fn; marshal to main.
            token.defer("BedMeshPanel::launch_after_query_err", [this, api, token]() {
                spdlog::warn("[BedMeshPanel] Failed to query bed_mesh config, "
                             "proceeding without expected probe count");
                launch_calibration(api, token, 0);
            });
        });
}

void BedMeshPanel::launch_calibration(MoonrakerAPI* api, helix::LifetimeToken token,
                                      int expected_probes, int probe_samples) {
    // Start calibration with progress tracking
    api->advanced().start_bed_mesh_calibrate(
        // Progress callback (from WebSocket thread)
        [this, token](int current, int total) {
            if (token.expired())
                return;
            token.defer("BedMeshPanel::probe_progress",
                        [this, current, total]() { on_probe_progress(current, total); });
        },
        // Complete callback (from WebSocket thread)
        [this, token]() {
            if (token.expired())
                return;
            token.defer("BedMeshPanel::calibrate_done", [this]() { on_calibration_complete(); });
        },
        // Error callback (from WebSocket thread)
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            auto msg = err.message;
            token.defer("BedMeshPanel::calibrate_error",
                        [this, msg]() { on_calibration_error(msg); });
        },
        expected_probes, probe_samples);
}

// ============================================================================
// Modal Management
// ============================================================================

void BedMeshPanel::show_calibrate_modal() {
    lv_subject_set_int(&bed_mesh_calibrating_, 0);

    calibrate_modal_widget_ = helix::ui::modal_show("bed_mesh_calibrate_modal");
    spdlog::debug("[{}] Showing calibrate modal", get_name());
}

void BedMeshPanel::show_rename_modal(const std::string& profile_name) {
    pending_rename_old_ = profile_name;
    lv_subject_copy_string(&bed_mesh_rename_old_name_, profile_name.c_str());

    rename_modal_widget_ = helix::ui::modal_show("bed_mesh_rename_modal");
    spdlog::debug("[{}] Showing rename modal for: {}", get_name(), profile_name);
}

void BedMeshPanel::show_delete_confirm_modal(const std::string& profile_name) {
    pending_delete_profile_ = profile_name;

    // Create message with profile name
    char msg_buf[256];
    snprintf(msg_buf, sizeof(msg_buf), "Delete profile '%s'? This action cannot be undone.",
             profile_name.c_str());

    delete_modal_widget_ = helix::ui::modal_show_confirmation(
        lv_tr("Delete Profile?"), msg_buf, ModalSeverity::Warning, lv_tr("Delete"),
        on_delete_confirm_cb, on_delete_cancel_cb,
        nullptr); // Uses global panel reference

    if (!delete_modal_widget_) {
        spdlog::error("[{}] Failed to create delete confirmation modal", get_name());
        return;
    }

    spdlog::debug("[{}] Showing delete confirm modal for: {}", get_name(), profile_name);
}

void BedMeshPanel::show_save_config_modal() {
    save_config_modal_widget_ = helix::ui::modal_show("bed_mesh_save_config_modal");
    spdlog::debug("[{}] Showing save config modal", get_name());
}

void BedMeshPanel::hide_all_modals() {
    // Cancel any pending operation timeout
    operation_guard_.end();

    // Reset calibration state machine
    lv_subject_set_int(&bed_mesh_calibrating_, 0);
    lv_subject_set_int(&bed_mesh_calibrate_state_, static_cast<int>(BedMeshCalibrationState::IDLE));

    // Hide all modals (all use ui_modal_hide pattern now)
    if (calibrate_modal_widget_) {
        helix::ui::modal_hide(calibrate_modal_widget_);
        calibrate_modal_widget_ = nullptr;
    }
    if (rename_modal_widget_) {
        helix::ui::modal_hide(rename_modal_widget_);
        rename_modal_widget_ = nullptr;
    }
    if (save_config_modal_widget_) {
        helix::ui::modal_hide(save_config_modal_widget_);
        save_config_modal_widget_ = nullptr;
    }
    if (delete_modal_widget_) {
        helix::ui::modal_hide(delete_modal_widget_);
        delete_modal_widget_ = nullptr;
    }
}

void BedMeshPanel::confirm_delete_profile() {
    std::string name = pending_delete_profile_;
    hide_all_modals();
    execute_delete_profile(name);
}

void BedMeshPanel::decline_save_config() {
    hide_all_modals();
    pending_operation_ = PendingOperation::None;
}

void BedMeshPanel::confirm_save_config() {
    hide_all_modals();
    execute_save_config();
    pending_operation_ = PendingOperation::None;
}

void BedMeshPanel::start_calibration_with_name(const std::string& profile_name) {
    hide_all_modals();
    execute_calibration(profile_name);
}

void BedMeshPanel::confirm_rename(const std::string& new_name) {
    std::string old_name = pending_rename_old_;
    hide_all_modals();
    execute_rename_profile(old_name, new_name);
}

// ============================================================================
// Profile Operation Implementations
// ============================================================================

void BedMeshPanel::execute_delete_profile(const std::string& name) {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    spdlog::info("[{}] Deleting profile: {}", get_name(), name);

    operation_guard_.begin(OPERATION_TIMEOUT_MS, [this] {
        hide_all_modals();
        pending_operation_ = PendingOperation::None;
        NOTIFY_WARNING(lv_tr("Bed mesh operation timed out"));
    });

    auto token = lifetime_.token();
    std::string cmd = "BED_MESH_PROFILE REMOVE=" + name;
    api->execute_gcode(
        cmd,
        [this, token, name]() {
            if (token.expired())
                return;
            token.defer("BedMeshPanel::delete_done", [this, name]() {
                operation_guard_.end();
                spdlog::info("[{}] Profile deleted: {}", get_name(), name);
                NOTIFY_SUCCESS(lv_tr("Profile deleted"));
                pending_operation_ = PendingOperation::Delete;
                show_save_config_modal();
            });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            auto msg = err.message;
            token.defer("BedMeshPanel::delete_error", [this, msg]() {
                operation_guard_.end();
                spdlog::error("[{}] Failed to delete profile: {}", get_name(), msg);
                NOTIFY_ERROR(lv_tr("Failed to delete profile"));
            });
        });
}

void BedMeshPanel::execute_rename_profile(const std::string& old_name,
                                          const std::string& new_name) {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    spdlog::info("[{}] Renaming profile: {} -> {}", get_name(), old_name, new_name);

    operation_guard_.begin(OPERATION_TIMEOUT_MS, [this] {
        hide_all_modals();
        pending_operation_ = PendingOperation::None;
        NOTIFY_WARNING(lv_tr("Bed mesh operation timed out"));
    });

    auto token = lifetime_.token();

    // Step 1: Load the profile
    std::string load_cmd = "BED_MESH_PROFILE LOAD=" + old_name;
    api->execute_gcode(
        load_cmd,
        [this, token, old_name, new_name]() {
            if (token.expired())
                return;
            // Step 2: Save with new name
            MoonrakerAPI* api2 = get_moonraker_api();
            if (!api2) {
                token.defer("BedMeshPanel::rename_no_api", [this]() { operation_guard_.end(); });
                return;
            }
            std::string save_cmd = "BED_MESH_PROFILE SAVE=" + new_name;
            api2->execute_gcode(
                save_cmd,
                [this, token, old_name, new_name]() {
                    if (token.expired())
                        return;
                    // Step 3: Remove old name
                    MoonrakerAPI* api3 = get_moonraker_api();
                    if (!api3) {
                        token.defer("BedMeshPanel::rename_no_api2",
                                    [this]() { operation_guard_.end(); });
                        return;
                    }
                    std::string remove_cmd = "BED_MESH_PROFILE REMOVE=" + old_name;
                    api3->execute_gcode(
                        remove_cmd,
                        [this, token, old_name, new_name]() {
                            if (token.expired())
                                return;
                            token.defer("BedMeshPanel::rename_done", [this, old_name, new_name]() {
                                operation_guard_.end();
                                spdlog::info("[{}] Profile renamed: {} -> {}", get_name(), old_name,
                                             new_name);
                                NOTIFY_SUCCESS(lv_tr("Profile renamed"));
                                pending_operation_ = PendingOperation::Rename;
                                show_save_config_modal();
                            });
                        },
                        [this, token](const MoonrakerError& err) {
                            if (token.expired())
                                return;
                            auto msg = err.message;
                            token.defer("BedMeshPanel::rename_remove_error", [this, msg]() {
                                operation_guard_.end();
                                spdlog::error("[{}] Failed to remove old profile: {}", get_name(),
                                              msg);
                                NOTIFY_ERROR(lv_tr("Rename failed at remove step"));
                            });
                        });
                },
                [this, token](const MoonrakerError& err) {
                    if (token.expired())
                        return;
                    auto msg = err.message;
                    token.defer("BedMeshPanel::rename_save_error", [this, msg]() {
                        operation_guard_.end();
                        spdlog::error("[{}] Failed to save new profile: {}", get_name(), msg);
                        NOTIFY_ERROR(lv_tr("Rename failed at save step"));
                    });
                });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            auto msg = err.message;
            token.defer("BedMeshPanel::rename_load_error", [this, msg]() {
                operation_guard_.end();
                spdlog::error("[{}] Failed to load profile for rename: {}", get_name(), msg);
                NOTIFY_ERROR(lv_tr("Rename failed at load step"));
            });
        });
}

void BedMeshPanel::execute_calibration(const std::string& /*profile_name*/) {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    // Probe into a temporary profile so we don't clobber an existing "default"
    // mesh. save_profile_with_name() renames this to the user's chosen name
    // after probing completes.
    static constexpr const char* TEMP_PROFILE = "_hs_temp";

    // A printer may declare a multi-line gcode template in the database
    // (calibration.bed_mesh_gcode). Used today by the Elegoo Centauri Carbon,
    // whose mainline-Klipper [load_cell_probe] aborts on stale tare; the
    // template tares first, runs the vendor's wipe wrapper, then saves the
    // current mesh under {profile}.
    const std::string& printer_name = get_printer_state().get_printer_type();
    std::string cmd = PrinterDetector::get_bed_mesh_calibrate_gcode(printer_name);

    std::string macro_name;
    if (!cmd.empty()) {
        const std::string placeholder = "{profile}";
        for (size_t pos = cmd.find(placeholder); pos != std::string::npos;
             pos = cmd.find(placeholder, pos + 1)) {
            cmd.replace(pos, placeholder.size(), TEMP_PROFILE);
        }
        macro_name = "<calibration.bed_mesh_gcode>";
    } else {
        // Resolve the calibration macro via StandardMacros (user-configurable,
        // defaults to auto-detected BED_MESH_CALIBRATE or G29)
        const auto& macro_info = StandardMacros::instance().get(StandardMacroSlot::BedMesh);
        macro_name = macro_info.get_macro();
        if (macro_name.empty()) {
            macro_name = "BED_MESH_CALIBRATE"; // Fallback if nothing configured/detected
        }
        cmd = macro_name + " PROFILE=" + std::string(TEMP_PROFILE);
    }

    spdlog::info("[{}] Starting calibration (temp profile: {}, macro: {})", get_name(),
                 TEMP_PROFILE, macro_name);
    lv_subject_set_int(&bed_mesh_calibrating_, 1);

    auto token = lifetime_.token();
    api->execute_gcode(
        cmd,
        [this, token]() {
            if (token.expired())
                return;
            token.defer("BedMeshPanel::calibrate_gcode_done", [this]() {
                spdlog::info("[{}] Calibration started", get_name());
                NOTIFY_INFO(lv_tr("Calibration started"));
            });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            auto msg = err.message;
            auto type = err.type;
            token.defer("BedMeshPanel::calibrate_gcode_error", [this, msg, type]() {
                if (type == MoonrakerErrorType::TIMEOUT) {
                    spdlog::warn("[{}] Calibration response timed out (may still be running)",
                                 get_name());
                    NOTIFY_WARNING(lv_tr("Calibration may still be running — response timed out"));
                } else {
                    spdlog::error("[{}] Failed to start calibration: {}", get_name(), msg);
                    NOTIFY_ERROR(lv_tr("Failed to start calibration"));
                    lv_subject_set_int(&bed_mesh_calibrating_, 0);
                }
            });
        },
        CALIBRATION_TIMEOUT_MS);
}

void BedMeshPanel::execute_save_config() {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    spdlog::info("[{}] Saving config (will restart Klipper)", get_name());

    // Suppress recovery dialog — SAVE_CONFIG triggers an expected Klipper restart
    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::LONG);

    operation_guard_.begin(SLOW_OPERATION_TIMEOUT_MS, [this] {
        hide_all_modals();
        pending_operation_ = PendingOperation::None;
        NOTIFY_WARNING(lv_tr("Bed mesh operation timed out"));
    });

    auto token = lifetime_.token();
    api->execute_gcode(
        "SAVE_CONFIG",
        [this, token]() {
            if (token.expired())
                return;
            token.defer("BedMeshPanel::save_config_done", [this]() {
                operation_guard_.end();
                spdlog::info("[{}] SAVE_CONFIG sent - Klipper will restart", get_name());
                NOTIFY_INFO(lv_tr("Configuration saved - restarting"));
            });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            auto msg = err.message;
            token.defer("BedMeshPanel::save_config_error", [this, msg]() {
                operation_guard_.end();
                spdlog::error("[{}] Failed to save config: {}", get_name(), msg);
                NOTIFY_ERROR(lv_tr("Failed to save configuration"));
            });
        });
}

// ============================================================================
// Calibration Progress Handlers
// ============================================================================

void BedMeshPanel::on_probe_progress(int current, int total) {
    if (total > 0) {
        lv_subject_set_int(&bed_mesh_probe_indeterminate_, 0);
        // Cap at 99% — completion signal handles the final transition.
        // Adaptive mesh may produce more probes than the config hint.
        int progress = std::min(current * 100 / total, 99);
        lv_subject_set_int(&bed_mesh_probe_progress_, progress);

        std::snprintf(probe_text_buf_, sizeof(probe_text_buf_), "Probing point %d of %d", current,
                      total);
        spdlog::debug("[BedMeshPanel] Probe progress: {}/{} ({}%)", current, total, progress);
    } else {
        // Fallback: firmware didn't report total — show spinner instead of bar
        lv_subject_set_int(&bed_mesh_probe_indeterminate_, 1);
        std::snprintf(probe_text_buf_, sizeof(probe_text_buf_), "Probing... (%d points)", current);
        spdlog::debug("[BedMeshPanel] Probe progress: {} points (total unknown)", current);
    }
    lv_subject_copy_string(&bed_mesh_probe_text_, probe_text_buf_);
}

void BedMeshPanel::on_calibration_complete() {
    spdlog::info("[BedMeshPanel] Calibration complete, transitioning to naming state");
    cooldown_after_probing();
    lv_subject_set_int(&bed_mesh_calibrate_state_,
                       static_cast<int>(BedMeshCalibrationState::NAMING));
}

/// Strip Klipper error prefixes and make messages user-friendly
static std::string sanitize_error_message(const std::string& raw) {
    std::string msg = raw;

    // Strip Klipper emergency/error prefixes
    if (msg.rfind("!! ", 0) == 0)
        msg = msg.substr(3);
    if (msg.rfind("Error:", 0) == 0)
        msg = msg.substr(6);
    if (msg.rfind("error:", 0) == 0)
        msg = msg.substr(6);

    // Trim leading whitespace
    auto start = msg.find_first_not_of(" \t");
    if (start != std::string::npos && start > 0)
        msg = msg.substr(start);

    // Strip JSON-looking content (e.g. {"message": "Must home axis..."} → extract message)
    if (!msg.empty() && msg.front() == '{') {
        auto mpos = msg.find("\"message\"");
        if (mpos != std::string::npos) {
            // Find the value after "message":
            auto colon = msg.find(':', mpos + 9);
            if (colon != std::string::npos) {
                auto qstart = msg.find('"', colon + 1);
                auto qend =
                    (qstart != std::string::npos) ? msg.find('"', qstart + 1) : std::string::npos;
                if (qstart != std::string::npos && qend != std::string::npos) {
                    msg = msg.substr(qstart + 1, qend - qstart - 1);
                }
            }
        }
    }

    return msg;
}

void BedMeshPanel::on_calibration_error(const std::string& message) {
    cooldown_after_probing();
    spdlog::error("[BedMeshPanel] Calibration error: {}", message);
    std::string clean = sanitize_error_message(message);
    std::strncpy(error_message_buf_, clean.c_str(), sizeof(error_message_buf_) - 1);
    error_message_buf_[sizeof(error_message_buf_) - 1] = '\0';
    lv_subject_copy_string(&bed_mesh_error_message_, error_message_buf_);
    lv_subject_set_int(&bed_mesh_calibrate_state_,
                       static_cast<int>(BedMeshCalibrationState::ERROR));
}

void BedMeshPanel::handle_emergency_stop() {
    spdlog::warn("[BedMeshPanel] Emergency stop during bed mesh calibration");

    // E-stop kills all heaters — just reset preheat tracking flags
    preheat_turned_on_nozzle_ = false;
    preheat_turned_on_bed_ = false;

    // Suppress recovery dialog — user intentionally triggered E-Stop from this modal
    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::LONG);

    MoonrakerAPI* api = get_moonraker_api();
    if (api) {
        api->emergency_stop([]() { spdlog::info("[BedMeshPanel] Emergency stop sent"); },
                            [](const MoonrakerError& err) {
                                spdlog::error("[BedMeshPanel] Emergency stop failed: {}",
                                              err.message);
                            });
    }

    // Close modal and reset state
    hide_all_modals();
    lv_subject_set_int(&bed_mesh_calibrate_state_, static_cast<int>(BedMeshCalibrationState::IDLE));
}

void BedMeshPanel::save_profile_with_name(const std::string& name) {
    spdlog::info("[BedMeshPanel] Saving mesh profile: {}", name);

    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        hide_all_modals();
        return;
    }

    // The calibration probed into "_hs_temp" to avoid clobbering existing
    // profiles. Now save the mesh under the user's chosen name and remove
    // the temporary profile.
    static constexpr const char* TEMP_PROFILE = "_hs_temp";

    auto token = lifetime_.token();
    std::string cmd = "BED_MESH_PROFILE SAVE=" + name;
    api->execute_gcode(
        cmd,
        [this, token, name, api]() {
            if (token.expired())
                return;

            // Clean up the temporary calibration profile
            api->execute_gcode(std::string("BED_MESH_PROFILE REMOVE=") + TEMP_PROFILE, nullptr,
                               nullptr);

            token.defer("BedMeshPanel::save_profile_done", [this, name]() {
                spdlog::info("[BedMeshPanel] Profile saved: {}", name);
                NOTIFY_SUCCESS(lv_tr("Mesh saved as '{}'"), name);
                hide_all_modals();
                lv_subject_set_int(&bed_mesh_calibrate_state_,
                                   static_cast<int>(BedMeshCalibrationState::IDLE));
                pending_operation_ = PendingOperation::Calibrate;
                show_save_config_modal();
            });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            auto msg = err.message;
            token.defer("BedMeshPanel::save_profile_error", [this, msg]() {
                spdlog::error("[BedMeshPanel] Failed to save profile: {}", msg);
                NOTIFY_ERROR(lv_tr("Failed to save profile"));
                hide_all_modals();
            });
        });
}

// ============================================================================
// Static Event Callbacks
// ============================================================================

// Helper to extract profile index from callback name
static int get_profile_index_from_event(lv_event_t* e) {
    // The callback name contains the index (e.g., "on_profile_2_clicked")
    // We use user_data to pass the panel, not the index
    // Instead, look at the target object's name
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!target)
        return -1;

    // Walk from target upward looking for profile_row_N
    // Start from target itself (handles clicking the row card directly)
    lv_obj_t* obj = target;
    while (obj) {
        const char* name = lv_obj_get_name(obj);
        if (name && std::strncmp(name, "profile_row_", 12) == 0) {
            return name[12] - '0'; // Extract digit
        }
        obj = lv_obj_get_parent(obj);
    }
    return -1;
}

static void on_profile_clicked_cb(lv_event_t* e) {
    int index = get_profile_index_from_event(e);
    if (index >= 0) {
        get_global_bed_mesh_panel().load_profile(index);
    }
}

static void on_profile_rename_cb(lv_event_t* e) {
    int index = get_profile_index_from_event(e);
    if (index >= 0) {
        get_global_bed_mesh_panel().rename_profile(index);
    }
}

static void on_profile_delete_cb(lv_event_t* e) {
    int index = get_profile_index_from_event(e);
    if (index >= 0) {
        get_global_bed_mesh_panel().delete_profile(index);
    }
}

static void on_calibrate_header_clicked_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().start_calibration();
}

static void on_calibrate_cancel_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().hide_all_modals();
}

static void on_calibrate_start_cb(lv_event_t* /*e*/) {
    // Find the textarea
    lv_obj_t* input = lv_obj_find_by_name(lv_layer_top(), "calibrate_profile_name_input");
    if (!input) {
        // Try from parent screen
        input = lv_obj_find_by_name(lv_screen_active(), "calibrate_profile_name_input");
    }

    std::string profile_name = "default";
    if (input) {
        const char* text = lv_textarea_get_text(input);
        if (text && std::strlen(text) > 0) {
            profile_name = text;
        }
    }

    get_global_bed_mesh_panel().start_calibration_with_name(profile_name);
}

static void on_rename_cancel_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().hide_all_modals();
}

static void on_rename_confirm_cb(lv_event_t* /*e*/) {
    // Get the new name from the input field
    lv_obj_t* input = lv_obj_find_by_name(lv_layer_top(), "rename_new_name_input");
    if (!input) {
        input = lv_obj_find_by_name(lv_screen_active(), "rename_new_name_input");
    }

    if (input) {
        const char* text = lv_textarea_get_text(input);
        if (text && std::strlen(text) > 0) {
            get_global_bed_mesh_panel().confirm_rename(std::string(text));
        }
    }
}

static void on_delete_cancel_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().hide_all_modals();
}

static void on_delete_confirm_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().confirm_delete_profile();
}

static void on_save_config_no_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().decline_save_config();
}

static void on_save_config_yes_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().confirm_save_config();
}

static void on_emergency_stop_cb(lv_event_t* /*e*/) {
    get_global_bed_mesh_panel().handle_emergency_stop();
}

static void on_save_profile_cb(lv_event_t* /*e*/) {
    // Find the input field in the modal
    lv_obj_t* input = lv_obj_find_by_name(lv_layer_top(), "calibrate_profile_name_input");
    if (!input) {
        input = lv_obj_find_by_name(lv_screen_active(), "calibrate_profile_name_input");
    }

    std::string profile_name = "default";
    if (input) {
        const char* text = lv_textarea_get_text(input);
        if (text && std::strlen(text) > 0) {
            profile_name = text;
        }
    }

    get_global_bed_mesh_panel().save_profile_with_name(profile_name);
}

// ============================================================================
// Global Instance
// ============================================================================

DEFINE_GLOBAL_PANEL(BedMeshPanel, g_bed_mesh_panel, get_global_bed_mesh_panel)
