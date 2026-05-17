// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h" // SubjectLifetime

#include "subject_managed_panel.h"

#include <lvgl.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class Config;
}

namespace helix {

/// Fan type classification for display and control
enum class FanType {
    PART_COOLING,    ///< Main part cooling fan ("fan" or configured part fan)
    HEATER_FAN,      ///< Hotend cooling fan (auto-controlled, not user-adjustable)
    CONTROLLER_FAN,  ///< Electronics cooling (auto-controlled)
    TEMPERATURE_FAN, ///< Thermostatically controlled fan (auto-controlled)
    GENERIC_FAN,     ///< User-controllable generic fan (fan_generic)
    OUTPUT_PIN_FAN   ///< Creality-style output_pin fan (controllable via M106 P<index>)
};

/**
 * @brief Wizard-configured fan role assignments
 *
 * Maps fan roles to Moonraker object names. Used to:
 * - Correctly classify the configured part fan (even if it's a fan_generic)
 * - Override display names with role-based names for configured fans
 */
struct FanRoleConfig {
    std::string part_fan;    ///< Configured part cooling fan object name
    std::string hotend_fan;  ///< Configured hotend fan object name
    std::string chamber_fan; ///< Configured chamber fan object name
    std::string exhaust_fan; ///< Configured exhaust fan object name

    /// Build from wizard config. Returns empty roles if config is null.
    static FanRoleConfig from_config(Config* config);
};

/**
 * @brief Fan information for multi-fan display
 *
 * Holds display name, current speed, and controllability for each fan
 * discovered from Moonraker.
 */
struct FanInfo {
    std::string object_name;  ///< Full Moonraker object name (e.g., "heater_fan hotend_fan")
    std::string display_name; ///< Human-readable name (e.g., "Hotend Fan")
    FanType type = FanType::GENERIC_FAN;
    int speed_percent = 0;        ///< Current speed 0-100%
    bool is_controllable = false; ///< true for fan_generic, false for heater_fan/controller_fan
    std::optional<int> rpm;       ///< RPM from fan_feedback or Klipper rpm field
};

/**
 * @brief Picked primary fan object names for the standard 3-fan display.
 *
 * Each member is the object_name of the first fan of that role found in
 * the discovered fan list (in fan-discovery order), or empty if none.
 */
struct PrimaryFans {
    std::string part;   ///< First PART_COOLING fan, or empty
    std::string hotend; ///< First HEATER_FAN, or empty
    std::string aux;    ///< First of CONTROLLER_FAN, TEMPERATURE_FAN,
                        ///< GENERIC_FAN, OUTPUT_PIN_FAN — or empty
};

/**
 * @brief Manages fan-related subjects for printer state
 *
 * Handles both static fan subjects (main fan speed, version) and
 * dynamic per-fan subjects created during printer discovery.
 * Extracted from PrinterState as part of god class decomposition.
 */
class PrinterFanState {
  public:
    PrinterFanState() = default;
    ~PrinterFanState() = default;

    // Non-copyable
    PrinterFanState(const PrinterFanState&) = delete;
    PrinterFanState& operator=(const PrinterFanState&) = delete;

    /**
     * @brief Initialize fan subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    /**
     * @brief Update fan state from Moonraker status JSON
     * @param status JSON object containing fan data
     */
    void update_from_status(const nlohmann::json& status);

    /**
     * @brief Initialize fan tracking from discovered fan objects
     * @param fan_objects List of Moonraker fan object names
     * @param roles Wizard-configured fan role assignments (for naming and classification)
     */
    void init_fans(const std::vector<std::string>& fan_objects, const FanRoleConfig& roles = {});

    /**
     * @brief Update speed for a specific fan (called during status updates)
     * @param object_name Moonraker object name (e.g., "heater_fan hotend_fan")
     * @param speed Speed as 0.0-1.0 (Moonraker format)
     */
    void update_fan_speed(const std::string& object_name, double speed);

    /// Update RPM reading for a fan (from fan_feedback or Klipper rpm field)
    void update_fan_rpm(const std::string& object_name, int rpm);

    /// Rename a fan: saves custom name to Config, updates display_name, bumps fans_version
    void rename_fan(const std::string& object_name, const std::string& new_name);

    // Subject accessors
    lv_subject_t* get_fan_speed_subject() {
        return &fan_speed_;
    }
    lv_subject_t* get_fans_version_subject() {
        return &fans_version_;
    }

    /**
     * @brief Get speed subject for a specific fan (dynamic — requires lifetime token!)
     *
     * Returns the per-fan speed subject for reactive UI updates.
     * Each fan discovered via init_fans() has its own subject.
     *
     * IMPORTANT: These are dynamic subjects that may be destroyed during reconnection.
     * Always pass the returned lifetime token to your observer factory function
     * to prevent use-after-free crashes. See ui_observer_guard.h for details.
     *
     * @param object_name Moonraker object name (e.g., "fan", "heater_fan hotend_fan")
     * @param[out] lifetime Receives the subject's lifetime token (empty if not found)
     * @return Pointer to subject, or nullptr if fan not found
     */
    lv_subject_t* get_fan_speed_subject(const std::string& object_name, SubjectLifetime& lifetime);

    /// @deprecated Use the overload that returns a SubjectLifetime token.
    /// This overload exists only for call sites that don't create observers.
    lv_subject_t* get_fan_speed_subject(const std::string& object_name);

    /**
     * @brief Get all tracked fans
     * @return Const reference to fan info vector
     */
    const std::vector<FanInfo>& get_fans() const {
        return fans_;
    }

    /// Pick first fan of each primary role from the discovered list.
    /// Returns empty strings for roles with no detected fan.
    PrimaryFans classify_primary_fans() const;

  private:
    friend class PrinterFanStateTestAccess;

    /// Classify fan type from object name (considers configured part fan)
    FanType classify_fan_type(const std::string& object_name) const;

    /// Check if fan type is user-controllable
    static bool is_fan_controllable(FanType type);

    /// Get role-based display name override, or empty string if none
    std::string get_role_display_name(const std::string& object_name) const;

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    // Static fan subjects
    lv_subject_t fan_speed_{};    ///< Main part-cooling fan, 0-100%
    lv_subject_t fans_version_{}; ///< Increments on fan list changes

    // Dynamic per-fan subjects (unique_ptr prevents invalidation on rehash)
    std::unordered_map<std::string, std::unique_ptr<lv_subject_t>> fan_speed_subjects_;
    // Lifetime tokens for dynamic fan subjects — destroyed when subject is deinited,
    // expiring weak_ptrs in ObserverGuards to prevent use-after-free.
    std::unordered_map<std::string, SubjectLifetime> fan_speed_lifetimes_;

    // Fan metadata
    std::vector<FanInfo> fans_;

    // Configured fan roles from wizard config
    FanRoleConfig roles_;
    /// Maps configured fan object names to role display names
    std::unordered_map<std::string, std::string> role_display_names_;
};

} // namespace helix
