// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_spoolman_overlay.cpp
 * @brief Implementation of SpoolmanOverlay
 */

#include "ui_spoolman_overlay.h"

#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#if HELIX_HAS_LABEL_PRINTER
#include "ui_settings_label_printer.h"
#endif
#include "ui_settings_barcode_scanner.h"
#include "ui_button.h"
#include "ui_spoolman_setup.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "ams_state.h"
#include "http_executor.h"
#include "hv/requests.h"
#include "moonraker_api.h"
#include "moonraker_config_manager.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "spoolman_manager.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::ui {

// Database keys for settings persistence
static constexpr const char* DB_NAMESPACE = "helix-screen";
static constexpr const char* DB_KEY_SYNC_ENABLED = "spoolman_sync_enabled";
static constexpr const char* DB_KEY_REFRESH_INTERVAL = "spoolman_weight_refresh_interval";

// Legacy keys (pre-rename migration)
static constexpr const char* LEGACY_DB_KEY_SYNC_ENABLED = "ams_spoolman_sync_enabled";
static constexpr const char* LEGACY_DB_KEY_REFRESH_INTERVAL = "ams_weight_refresh_interval";

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<SpoolmanOverlay> g_spoolman_overlay;

SpoolmanOverlay& get_spoolman_overlay() {
    if (!g_spoolman_overlay) {
        g_spoolman_overlay = std::make_unique<SpoolmanOverlay>();
        StaticPanelRegistry::instance().register_destroy("SpoolmanOverlay",
                                                         []() { g_spoolman_overlay.reset(); });
    }
    return *g_spoolman_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

SpoolmanOverlay::SpoolmanOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

SpoolmanOverlay::~SpoolmanOverlay() {
    if (subjects_initialized_ && lv_is_initialized()) {
        lv_subject_deinit(&sync_enabled_subject_);
        lv_subject_deinit(&refresh_interval_subject_);
        lv_subject_deinit(&scanner_device_status_subject_);
    }
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void SpoolmanOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Initialize sync enabled subject (default: true/enabled)
    lv_subject_init_int(&sync_enabled_subject_, DEFAULT_SYNC_ENABLED ? 1 : 0);
    lv_xml_register_subject(nullptr, "ams_spoolman_sync_enabled", &sync_enabled_subject_);

    // Initialize refresh interval subject (default: 30 seconds)
    lv_subject_init_int(&refresh_interval_subject_, DEFAULT_REFRESH_INTERVAL_SECONDS);
    lv_xml_register_subject(nullptr, "ams_spoolman_refresh_interval", &refresh_interval_subject_);

    // Initialize scanner device status subject
    auto scanner_name = helix::SettingsManager::instance().get_scanner_device_name();
    auto scanner_id = helix::SettingsManager::instance().get_scanner_device_id();
    const char* status = scanner_id.empty() ? "Auto-detect" : scanner_name.c_str();
    snprintf(scanner_status_buf_, sizeof(scanner_status_buf_), "%s", status);
    lv_subject_init_string(&scanner_device_status_subject_, scanner_status_buf_, nullptr,
                           sizeof(scanner_status_buf_), scanner_status_buf_);
    lv_xml_register_subject(nullptr, "scanner_device_status", &scanner_device_status_subject_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void SpoolmanOverlay::register_callbacks() {
    // Register sync toggle callback
    lv_xml_register_event_cb(nullptr, "on_ams_spoolman_sync_toggled", on_sync_toggled);

    // Register interval dropdown callback
    lv_xml_register_event_cb(nullptr, "on_ams_spoolman_interval_changed", on_interval_changed);

#if HELIX_HAS_LABEL_PRINTER
    // Label printer sub-panel launcher
    lv_xml_register_event_cb(nullptr, "on_spoolman_label_printer_clicked",
                             on_label_printer_clicked);
#endif

    // Barcode scanner picker callback
    lv_xml_register_event_cb(nullptr, "on_barcode_scanner_clicked", on_barcode_scanner_clicked);

    // Server setup callbacks
    lv_xml_register_event_cb(nullptr, "on_spoolman_connect_clicked", on_connect_clicked);
    lv_xml_register_event_cb(nullptr, "on_spoolman_cancel_setup_clicked", on_cancel_setup_clicked);
    lv_xml_register_event_cb(nullptr, "on_spoolman_change_clicked", on_change_clicked);
    lv_xml_register_event_cb(nullptr, "on_spoolman_remove_clicked", on_remove_clicked);

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* SpoolmanOverlay::create(lv_obj_t* parent) {
    if (overlay_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    // Create from XML component
    overlay_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "spoolman_settings", nullptr));
    if (!overlay_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Find control widgets for programmatic access
    sync_toggle_ = lv_obj_find_by_name(overlay_, "sync_toggle");
    interval_dropdown_ = lv_obj_find_by_name(overlay_, "interval_dropdown");

    // Server setup widgets
    host_input_ = lv_obj_find_by_name(overlay_, "spoolman_host_input");
    port_input_ = lv_obj_find_by_name(overlay_, "spoolman_port_input");
    setup_status_text_ = lv_obj_find_by_name(overlay_, "setup_status_text");
    server_url_text_ = lv_obj_find_by_name(overlay_, "server_url_text");
    connect_btn_ = lv_obj_find_by_name(overlay_, "connect_btn");
    setup_card_ = lv_obj_find_by_name(overlay_, "setup_card");
    status_card_ = lv_obj_find_by_name(overlay_, "status_card");

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_;
}

void SpoolmanOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    // Ensure subjects and callbacks are initialized
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Lazy create overlay
    if (!overlay_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    // Load settings from database
    load_from_database();

    // Update UI controls to match subject values
    update_ui_from_subjects();

    // Show current server URL if connected
    update_server_url_display();

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_, this);

    // Register close callback to destroy widget tree when overlay closes
    NavigationManager::instance().register_overlay_close_callback(overlay_, [this]() {
        // overlay_ is an alias for overlay_root_, so pass it directly
        destroy_overlay_ui(overlay_);
    });

    // Push onto navigation stack
    NavigationManager::instance().push_overlay(overlay_);
}

void SpoolmanOverlay::refresh() {
    if (!overlay_) {
        return;
    }

    load_from_database();
    update_ui_from_subjects();
}

// ============================================================================
// DATABASE OPERATIONS
// ============================================================================

void SpoolmanOverlay::load_from_database() {
    if (!api_) {
        spdlog::warn("[{}] No API available, using default values", get_name());
        return;
    }

    // Helper: apply sync_enabled value and update polling
    auto apply_sync = [this](bool enabled) {
        lv_subject_set_int(&sync_enabled_subject_, enabled ? 1 : 0);
        spdlog::debug("[{}] Loaded sync_enabled={} from database", get_name(), enabled);
        if (enabled) {
            SpoolmanManager::instance().start_spoolman_polling();
        } else {
            SpoolmanManager::instance().stop_spoolman_polling();
        }
    };

    auto parse_sync = [](const nlohmann::json& value, bool default_val) {
        if (value.is_boolean())
            return value.get<bool>();
        if (value.is_number())
            return value.get<int>() != 0;
        return default_val;
    };

    // Load sync enabled — try new key, fall back to legacy key
    api_->database_get_item(
        DB_NAMESPACE, DB_KEY_SYNC_ENABLED,
        [this, apply_sync, parse_sync](const nlohmann::json& value) {
            apply_sync(parse_sync(value, DEFAULT_SYNC_ENABLED));
        },
        [this, apply_sync, parse_sync](const MoonrakerError&) {
            // New key not found — try legacy key
            api_->database_get_item(
                DB_NAMESPACE, LEGACY_DB_KEY_SYNC_ENABLED,
                [this, apply_sync, parse_sync](const nlohmann::json& value) {
                    bool enabled = parse_sync(value, DEFAULT_SYNC_ENABLED);
                    apply_sync(enabled);
                    // Migrate to new key
                    save_sync_enabled(enabled);
                    spdlog::info("[{}] Migrated {} -> {}", get_name(), LEGACY_DB_KEY_SYNC_ENABLED,
                                 DB_KEY_SYNC_ENABLED);
                },
                [this, apply_sync](const MoonrakerError&) { apply_sync(DEFAULT_SYNC_ENABLED); });
        });

    // Helper: apply refresh interval
    auto apply_interval = [this](int interval) {
        lv_subject_set_int(&refresh_interval_subject_, interval);
        spdlog::debug("[{}] Loaded refresh_interval={} from database", get_name(), interval);
    };

    auto parse_interval = [](const nlohmann::json& value, int default_val) {
        return value.is_number() ? value.get<int>() : default_val;
    };

    // Load refresh interval — try new key, fall back to legacy key
    api_->database_get_item(
        DB_NAMESPACE, DB_KEY_REFRESH_INTERVAL,
        [this, apply_interval, parse_interval](const nlohmann::json& value) {
            apply_interval(parse_interval(value, DEFAULT_REFRESH_INTERVAL_SECONDS));
        },
        [this, apply_interval, parse_interval](const MoonrakerError&) {
            // New key not found — try legacy key
            api_->database_get_item(
                DB_NAMESPACE, LEGACY_DB_KEY_REFRESH_INTERVAL,
                [this, apply_interval, parse_interval](const nlohmann::json& value) {
                    int interval = parse_interval(value, DEFAULT_REFRESH_INTERVAL_SECONDS);
                    apply_interval(interval);
                    // Migrate to new key
                    save_refresh_interval(interval);
                    spdlog::info("[{}] Migrated {} -> {}", get_name(),
                                 LEGACY_DB_KEY_REFRESH_INTERVAL, DB_KEY_REFRESH_INTERVAL);
                },
                [this, apply_interval](const MoonrakerError&) {
                    apply_interval(DEFAULT_REFRESH_INTERVAL_SECONDS);
                });
        });
}

void SpoolmanOverlay::save_sync_enabled(bool enabled) {
    if (!api_) {
        spdlog::warn("[{}] No API available, cannot save setting", get_name());
        return;
    }

    api_->database_post_item(
        DB_NAMESPACE, DB_KEY_SYNC_ENABLED, enabled,
        [this, enabled]() {
            spdlog::info("[{}] Saved sync_enabled={} to database", get_name(), enabled);
        },
        [this](const MoonrakerError& err) {
            spdlog::error("[{}] Failed to save sync_enabled: {}", get_name(), err.message);
        });
}

void SpoolmanOverlay::save_refresh_interval(int interval_seconds) {
    if (!api_) {
        spdlog::warn("[{}] No API available, cannot save setting", get_name());
        return;
    }

    api_->database_post_item(
        DB_NAMESPACE, DB_KEY_REFRESH_INTERVAL, interval_seconds,
        [this, interval_seconds]() {
            spdlog::info("[{}] Saved refresh_interval={} to database", get_name(),
                         interval_seconds);
        },
        [this](const MoonrakerError& err) {
            spdlog::error("[{}] Failed to save refresh_interval: {}", get_name(), err.message);
        });
}

// ============================================================================
// UTILITY METHODS
// ============================================================================

int SpoolmanOverlay::dropdown_index_to_seconds(int index) {
    // Dropdown options: "30s", "1 min", "2 min", "5 min"
    switch (index) {
    case 0:
        return 30;
    case 1:
        return 60;
    case 2:
        return 120;
    case 3:
        return 300;
    default:
        return 30;
    }
}

int SpoolmanOverlay::seconds_to_dropdown_index(int seconds) {
    switch (seconds) {
    case 30:
        return 0;
    case 60:
        return 1;
    case 120:
        return 2;
    case 300:
        return 3;
    default:
        return 0; // Default to 30s
    }
}

void SpoolmanOverlay::update_ui_from_subjects() {
    // Update dropdown to match current interval
    if (interval_dropdown_) {
        int interval_seconds = lv_subject_get_int(&refresh_interval_subject_);
        int dropdown_index = seconds_to_dropdown_index(interval_seconds);
        lv_dropdown_set_selected(interval_dropdown_, static_cast<uint32_t>(dropdown_index));
    }

    // Toggle state is handled by subject binding in XML
}

void SpoolmanOverlay::on_ui_destroyed() {
    sync_toggle_ = nullptr;
    interval_dropdown_ = nullptr;
    host_input_ = nullptr;
    port_input_ = nullptr;
    setup_status_text_ = nullptr;
    server_url_text_ = nullptr;
    connect_btn_ = nullptr;
    setup_card_ = nullptr;
    status_card_ = nullptr;
}

void SpoolmanOverlay::on_activate() {
    OverlayBase::on_activate();
    // Scanner selection may have changed in the child overlay; re-sync the row.
    update_scanner_status_text();
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void SpoolmanOverlay::on_sync_toggled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_sync_toggled");

    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!toggle || !lv_obj_is_valid(toggle)) {
        spdlog::warn("[SpoolmanOverlay] Stale callback - toggle no longer valid");
    } else {
        bool is_checked = lv_obj_has_state(toggle, LV_STATE_CHECKED);

        spdlog::info("[SpoolmanOverlay] Sync toggle: {}", is_checked ? "enabled" : "disabled");

        // Update subject
        auto& overlay = get_spoolman_overlay();
        lv_subject_set_int(&overlay.sync_enabled_subject_, is_checked ? 1 : 0);

        // Save to database
        overlay.save_sync_enabled(is_checked);

        // Update Spoolman polling
        if (is_checked) {
            SpoolmanManager::instance().start_spoolman_polling();
        } else {
            SpoolmanManager::instance().stop_spoolman_polling();
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::on_interval_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_interval_changed");

    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!dropdown || !lv_obj_is_valid(dropdown)) {
        spdlog::warn("[SpoolmanOverlay] Stale callback - dropdown no longer valid");
    } else {
        int selected = static_cast<int>(lv_dropdown_get_selected(dropdown));
        int interval_seconds = dropdown_index_to_seconds(selected);

        spdlog::info("[SpoolmanOverlay] Interval changed: {}s", interval_seconds);

        // Update subject
        auto& overlay = get_spoolman_overlay();
        lv_subject_set_int(&overlay.refresh_interval_subject_, interval_seconds);

        // Save to database
        overlay.save_refresh_interval(interval_seconds);

        // Note: The actual polling interval in AmsState is currently fixed at 30s.
        // This setting is stored for future use when configurable polling is implemented.
        // For now, we just persist the user's preference.
    }

    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// LABEL PRINTER SUB-PANEL
// ============================================================================

#if HELIX_HAS_LABEL_PRINTER
void SpoolmanOverlay::on_label_printer_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_label_printer_clicked");
    auto& overlay = helix::settings::get_label_printer_settings_overlay();
    auto& spoolman = get_spoolman_overlay();
    overlay.show(spoolman.parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}
#endif

// ============================================================================
// SERVER SETUP
// ============================================================================

void SpoolmanOverlay::set_setup_status(const char* text, bool is_error) {
    if (setup_status_text_) {
        lv_label_set_text(setup_status_text_, text);
        lv_obj_set_style_text_color(setup_status_text_,
                                    theme_manager_get_color(is_error ? "danger" : "text_muted"),
                                    LV_PART_MAIN);
    }
}

void SpoolmanOverlay::set_connecting(bool connecting) {
    if (!connect_btn_ || !lv_obj_is_valid(connect_btn_))
        return;
    if (connecting) {
        lv_obj_add_state(connect_btn_, LV_STATE_DISABLED);
        ui_button_set_text(connect_btn_, lv_tr("Connecting..."));
    } else {
        lv_obj_remove_state(connect_btn_, LV_STATE_DISABLED);
        ui_button_set_text(connect_btn_, lv_tr("Connect"));
    }
}

// ============================================================================
// BARCODE SCANNER PICKER
// ============================================================================

void SpoolmanOverlay::on_barcode_scanner_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_barcode_scanner_clicked");
    get_spoolman_overlay().handle_barcode_scanner_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::handle_barcode_scanner_clicked() {
    spdlog::debug("[{}] Barcode Scanner clicked - opening settings overlay", get_name());
    helix::ui::get_barcode_scanner_settings_overlay().show(parent_screen_);
}

void SpoolmanOverlay::update_scanner_status_text() {
    auto id = helix::SettingsManager::instance().get_scanner_device_id();
    auto name = helix::SettingsManager::instance().get_scanner_device_name();
    const char* status = id.empty() ? "Auto-detect" : name.c_str();
    snprintf(scanner_status_buf_, sizeof(scanner_status_buf_), "%s", status);
    lv_subject_copy_string(&scanner_device_status_subject_, scanner_status_buf_);
}

// ============================================================================
// SERVER SETUP
// ============================================================================

void SpoolmanOverlay::on_connect_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_connect_clicked");

    auto& overlay = get_spoolman_overlay();
    const char* host_raw = overlay.host_input_ ? lv_textarea_get_text(overlay.host_input_) : "";
    const char* port_raw = overlay.port_input_ ? lv_textarea_get_text(overlay.port_input_) : "";

    std::string host(host_raw ? host_raw : "");
    std::string port(port_raw ? port_raw : "");

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t");
        size_t end = s.find_last_not_of(" \t");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    };
    trim(host);
    trim(port);

    if (port.empty())
        port = DEFAULT_SPOOLMAN_PORT;

    if (!SpoolmanSetup::validate_host(host)) {
        overlay.set_setup_status(lv_tr("Please enter an IP address or hostname."), true);
        return;
    }
    if (!SpoolmanSetup::validate_port(port)) {
        overlay.set_setup_status(lv_tr("Please enter a valid port (1-65535)."), true);
        return;
    }

    overlay.set_connecting(true);
    overlay.set_setup_status(lv_tr("Checking Spoolman server..."));
    overlay.probe_spoolman_server(host, port);

    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::probe_spoolman_server(const std::string& host, const std::string& port) {
    // In mock mode, skip the real HTTP probe and simulate success
    if (get_runtime_config() && get_runtime_config()->should_mock_moonraker()) {
        spdlog::info("[{}] Mock mode — skipping HTTP probe, simulating Spoolman at {}:{}",
                     get_name(), host, port);
        set_setup_status(lv_tr("Spoolman found! Configuring..."));
        configure_spoolman(host, port);
        return;
    }

    auto token = lifetime_.token();
    std::string probe_url = SpoolmanSetup::build_probe_url(host, port);
    std::string host_copy = host;
    std::string port_copy = port;

    spdlog::info("[{}] Probing Spoolman at {}", get_name(), probe_url);

    // Route through HttpExecutor::fast() (bounded 4-worker pool) instead of
    // spawning a detached std::thread per click. Raw thread creation failed
    // with pthread EAGAIN under thread exhaustion on memory-constrained ARM
    // devices (#724) and crashed when users double-tapped the Connect button.
    helix::http::HttpExecutor::fast().submit([this, token, probe_url, host_copy, port_copy]() {
        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_GET;
        req->url = probe_url;
        req->timeout = 5;
        auto resp = requests::request(req);
        bool success = (resp && resp->status_code == 200);

        if (token.expired())
            return;
        token.defer("SpoolmanOverlay::probe_result", [this, success, host_copy, port_copy]() {
            if (success) {
                spdlog::info("[{}] Spoolman probe succeeded", get_name());
                set_setup_status(lv_tr("Spoolman found! Configuring..."));
                configure_spoolman(host_copy, port_copy);
            } else {
                spdlog::warn("[{}] Spoolman probe failed at {}:{}", get_name(), host_copy,
                             port_copy);
                auto msg = fmt::format("{} {}:{}", lv_tr("Could not reach Spoolman at"), host_copy,
                                       port_copy);
                set_setup_status(msg.c_str(), true);
                set_connecting(false);
            }
        });
    });
}

// ============================================================================
// CONFIG WRITE CHAIN
// ============================================================================

void SpoolmanOverlay::configure_spoolman(const std::string& host, const std::string& port) {
    if (!api_) {
        set_setup_status(lv_tr("Not connected to printer."), true);
        set_connecting(false);
        return;
    }
    auto token = lifetime_.token();
    auto entries = SpoolmanSetup::build_spoolman_config_entries(host, port);

    api_->transfers().download_file(
        "config", "helixscreen.conf",
        [this, token, entries](const std::string& content) {
            if (token.expired())
                return;
            // Defer to main thread — finish_configure() accesses lifetime_
            token.defer([this, content, entries]() { finish_configure(content, entries); });
        },
        [this, token, entries](const MoonrakerError& err) {
            if (token.expired())
                return;
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND) {
                token.defer([this, entries]() { finish_configure("", entries); });
            } else {
                token.defer("SpoolmanOverlay::configure_error", [this]() {
                    set_setup_status(lv_tr("Failed to read config. Check connection."), true);
                    set_connecting(false);
                });
            }
        });
}

void SpoolmanOverlay::finish_configure(
    const std::string& helix_content,
    const std::vector<std::pair<std::string, std::string>>& entries) {
    auto token = lifetime_.token();
    std::string modified = helix::MoonrakerConfigManager::add_section(
        helix_content, "spoolman", entries, "Spoolman - added by HelixScreen");

    api_->transfers().upload_file(
        "config", "helixscreen.conf", modified,
        [this, token]() {
            if (token.expired())
                return;
            // Defer to main thread — ensure_moonraker_include() accesses lifetime_
            token.defer([this]() { ensure_moonraker_include(); });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            auto msg = err.message;
            token.defer("SpoolmanOverlay::upload_error", [this, msg]() {
                spdlog::error("[{}] Failed to upload helixscreen.conf: {}", get_name(), msg);
                set_setup_status(lv_tr("Failed to save config."), true);
                set_connecting(false);
            });
        });
}

void SpoolmanOverlay::ensure_moonraker_include() {
    auto token = lifetime_.token();
    api_->transfers().download_file(
        "config", "moonraker.conf",
        [this, token](const std::string& content) {
            if (token.expired())
                return;
            // Defer to main thread — helper methods access lifetime_
            token.defer([this, content]() {
                if (helix::MoonrakerConfigManager::has_include_line(content)) {
                    restart_and_verify();
                    return;
                }
                auto token2 = lifetime_.token();
                std::string modified = helix::MoonrakerConfigManager::add_include_line(content);
                api_->transfers().upload_file(
                    "config", "moonraker.conf", modified,
                    [this, token2]() {
                        if (token2.expired())
                            return;
                        token2.defer([this]() { restart_and_verify(); });
                    },
                    [this, token2](const MoonrakerError&) {
                        if (token2.expired())
                            return;
                        token2.defer([this]() {
                            set_setup_status(lv_tr("Failed to update moonraker.conf."), true);
                            set_connecting(false);
                        });
                    });
            });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND) {
                // Defer to main thread — upload chain accesses lifetime_
                token.defer([this]() {
                    auto token2 = lifetime_.token();
                    std::string fresh = helix::MoonrakerConfigManager::add_include_line("");
                    api_->transfers().upload_file(
                        "config", "moonraker.conf", fresh,
                        [this, token2]() {
                            if (token2.expired())
                                return;
                            token2.defer([this]() { restart_and_verify(); });
                        },
                        [this, token2](const MoonrakerError&) {
                            if (token2.expired())
                                return;
                            token2.defer([this]() {
                                set_setup_status(lv_tr("Failed to update moonraker.conf."), true);
                                set_connecting(false);
                            });
                        });
                });
            } else {
                token.defer([this]() {
                    set_setup_status(lv_tr("Failed to read moonraker.conf."), true);
                    set_connecting(false);
                });
            }
        });
}

void SpoolmanOverlay::restart_and_verify() {
    lifetime_.defer("SpoolmanOverlay::restart_status",
                    [this]() { set_setup_status(lv_tr("Restarting Moonraker...")); });

    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::LONG);

    auto token = lifetime_.token();
    api_->restart_moonraker(
        [this, token]() {
            if (token.expired())
                return;
            spdlog::info("[{}] Moonraker restart initiated", get_name());
            token.defer("SpoolmanOverlay::restart_wait", [this]() {
                set_setup_status(lv_tr("Waiting for Moonraker..."));
                lv_timer_create(
                    [](lv_timer_t* timer) {
                        lv_timer_delete(timer);
                        auto& overlay = get_spoolman_overlay();
                        overlay.verify_spoolman_connected();
                    },
                    8000, nullptr);
            });
        },
        [this, token](const MoonrakerError&) {
            if (token.expired())
                return;
            token.defer("SpoolmanOverlay::restart_error", [this]() {
                set_setup_status(lv_tr("Failed to restart Moonraker."), true);
                set_connecting(false);
            });
        });
}

void SpoolmanOverlay::verify_spoolman_connected() {
    if (!api_)
        return;
    auto token = lifetime_.token();
    api_->spoolman().get_spoolman_status(
        [this, token](bool connected, int /*spool_id*/) {
            if (token.expired())
                return;
            token.defer("SpoolmanOverlay::verify_status", [this, connected]() {
                if (connected) {
                    spdlog::info("[{}] Spoolman verified connected!", get_name());
                    set_setup_status("");
                    update_server_url_display();
                    ToastManager::instance().show(ToastSeverity::SUCCESS,
                                                  lv_tr("Spoolman connected!"), 3000);
                } else {
                    set_setup_status(
                        lv_tr("Moonraker restarted but Spoolman not connected. Check server."),
                        true);
                }
                set_connecting(false);
            });
        },
        [this, token](const MoonrakerError&) {
            if (token.expired())
                return;
            token.defer("SpoolmanOverlay::verify_error", [this]() {
                set_setup_status(
                    lv_tr("Could not verify Spoolman status. Moonraker may still be restarting."),
                    true);
                set_connecting(false);
            });
        });
}

// ============================================================================
// CHANGE, REMOVE, URL DISPLAY
// ============================================================================

void SpoolmanOverlay::update_server_url_display() {
    if (!server_url_text_ || !api_)
        return;
    auto token = lifetime_.token();
    api_->transfers().download_file(
        "config", "helixscreen.conf",
        [this, token](const std::string& content) {
            auto url =
                helix::MoonrakerConfigManager::get_section_value(content, "spoolman", "server");
            token.defer("SpoolmanOverlay::url_display", [this, url]() {
                if (!server_url_text_)
                    return;
                if (!url.empty()) {
                    auto text = fmt::format("{} {}", lv_tr("Connected to"), url);
                    lv_label_set_text(server_url_text_, text.c_str());
                } else {
                    lv_label_set_text(server_url_text_, lv_tr("Connected"));
                }
            });
        },
        [this, token](const MoonrakerError&) {
            token.defer("SpoolmanOverlay::url_fallback", [this]() {
                if (!server_url_text_)
                    return;
                lv_label_set_text(server_url_text_, lv_tr("Connected"));
            });
        });
}

void SpoolmanOverlay::on_change_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_change_clicked");
    auto& overlay = get_spoolman_overlay();

    if (overlay.api_) {
        auto token = overlay.lifetime_.token();
        overlay.api_->transfers().download_file(
            "config", "helixscreen.conf",
            [&overlay, token](const std::string& content) {
                auto url =
                    helix::MoonrakerConfigManager::get_section_value(content, "spoolman", "server");
                auto parsed = SpoolmanSetup::parse_url_components(url);
                auto host = std::move(parsed.first);
                auto port = std::move(parsed.second);
                if (token.expired())
                    return;
                token.defer([&overlay, host, port]() {
                    if (overlay.host_input_)
                        lv_textarea_set_text(overlay.host_input_, host.c_str());
                    if (overlay.port_input_)
                        lv_textarea_set_text(overlay.port_input_, port.c_str());
                });
            },
            [](const MoonrakerError&) {});
    }

    if (overlay.setup_card_)
        lv_obj_remove_flag(overlay.setup_card_, LV_OBJ_FLAG_HIDDEN);
    if (overlay.status_card_)
        lv_obj_add_flag(overlay.status_card_, LV_OBJ_FLAG_HIDDEN);

    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::on_cancel_setup_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_cancel_setup_clicked");

    // Restore default visibility — let the subject bindings take over again
    auto& overlay = get_spoolman_overlay();
    if (overlay.setup_card_)
        lv_obj_add_flag(overlay.setup_card_, LV_OBJ_FLAG_HIDDEN);
    if (overlay.status_card_)
        lv_obj_remove_flag(overlay.status_card_, LV_OBJ_FLAG_HIDDEN);
    overlay.set_setup_status("");
    overlay.set_connecting(false);

    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::on_remove_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_remove_clicked");

    modal_show_confirmation(
        lv_tr("Remove Spoolman?"),
        lv_tr(
            "This will remove the Spoolman configuration from Moonraker and restart the service."),
        ModalSeverity::Warning, lv_tr("Remove"),
        [](lv_event_t*) {
            auto& overlay = get_spoolman_overlay();
            overlay.remove_spoolman_config();
        },
        nullptr, nullptr);

    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::remove_spoolman_config() {
    if (!api_)
        return;
    auto token = lifetime_.token();
    api_->transfers().download_file(
        "config", "helixscreen.conf",
        [this, token](const std::string& content) {
            // Build modified content on bg thread (pure string ops, no `this`).
            std::string modified =
                helix::MoonrakerConfigManager::remove_section(content, "spoolman");
            // Marshal upload call to main thread before touching api_.
            token.defer("SpoolmanOverlay::remove_upload",
                        [this, token, modified = std::move(modified)]() mutable {
                            api_->transfers().upload_file(
                                "config", "helixscreen.conf", modified,
                                [this, token]() {
                                    if (token.expired())
                                        return;
                                    token.defer([this]() {
                                        set_setup_status(lv_tr("Restarting Moonraker..."));
                                    });
                                    EmergencyStopOverlay::instance().suppress_recovery_dialog(
                                        RecoverySuppression::LONG);
                                    api_->restart_moonraker(
                                        [this, token]() {
                                            if (token.expired())
                                                return;
                                            token.defer([this]() {
                                                ToastManager::instance().show(
                                                    ToastSeverity::SUCCESS,
                                                    lv_tr("Spoolman removed."), 3000);
                                            });
                                        },
                                        [this, token](const MoonrakerError&) {
                                            if (token.expired())
                                                return;
                                            token.defer([this]() {
                                                set_setup_status(
                                                    lv_tr("Failed to restart Moonraker."), true);
                                            });
                                        });
                                },
                                [this, token](const MoonrakerError&) {
                                    if (token.expired())
                                        return;
                                    token.defer([this]() {
                                        set_setup_status(lv_tr("Failed to save config."), true);
                                    });
                                });
                        });
        },
        [this, token](const MoonrakerError&) {
            if (token.expired())
                return;
            token.defer([this]() { set_setup_status(lv_tr("Failed to read config."), true); });
        });
}

} // namespace helix::ui
