// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_qidi.h"

#include "ams_error.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>

// Stub backend for the QIDI Box filament changer. Read-path mirrors
// save_variables onto AmsSystemInfo; write-path (load/unload/change_tool)
// is still not implemented pending field-test access (issue #954 brought
// the protocol reference; hardware validation still gated on Sib6019).
//
// TODO(qidi-box): drop a `qidi_box_64.png` (and matching .svg / `_512.png`
// if other backends carry them) into assets/images/ams/ to match the logo
// convention used by afc_64.png, box_turtle_64.png, happy_hare_64.png, etc.
// The QIDI wordmark / box silhouette is fine — no in-app scaling required.

namespace {
// Parse `"slot<N>"` into N when valid and within [0, slot_count).
// Returns nullopt for the box_extras.py sentinel `"slot-1"` (nothing
// loaded) and for any other malformed input. Used to decode the
// `value_t<T>` and `last_load_slot` save_variables, both of which carry
// slot references in this format.
std::optional<int> parse_slot_name(const std::string& val, int slot_count) {
    if (val.rfind("slot", 0) != 0) {
        return std::nullopt;
    }
    try {
        int idx = std::stoi(val.substr(4));
        if (idx >= 0 && idx < slot_count) {
            return idx;
        }
    } catch (const std::exception&) {
        // Bad slot string — fall through to nullopt
    }
    return std::nullopt;
}
} // namespace

AmsBackendQidi::AmsBackendQidi(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Populate system_info_ so get_system_info() returns a self-consistent
    // empty-but-initialised snapshot even before any status update arrives.
    system_info_.type = AmsType::QIDI_BOX;
    system_info_.type_name = "QIDI Box"; // i18n: do not translate - product name
    system_info_.total_slots = NUM_SLOTS;
    system_info_.supports_bypass = false;
    system_info_.supports_tool_mapping = true;
    system_info_.supports_endless_spool = false;
    system_info_.supports_purge = false;
    system_info_.tip_method = TipMethod::CUT;

    // Single unit with NUM_SLOTS empty slots, PARALLEL-less HUB topology.
    AmsUnit unit;
    unit.unit_index = 0;
    unit.name = "QIDI Box";
    unit.display_name = "QIDI Box";
    unit.slot_count = NUM_SLOTS;
    unit.first_slot_global_index = 0;
    unit.connected = false; // flip once protocol is implemented
    unit.topology = PathTopology::HUB;

    for (int i = 0; i < NUM_SLOTS; ++i) {
        SlotInfo slot;
        slot.slot_index = i;
        slot.global_index = i;
        slot.status = SlotStatus::UNKNOWN;
        slot.mapped_tool = i;
        unit.slots.push_back(slot);
    }

    system_info_.units.push_back(std::move(unit));
    slot_rfid_.resize(NUM_SLOTS);

    // Field-testing gate. Default off; set HELIX_QIDI_BOX_WRITE=1 (or any
    // non-empty value) to enable the write-path. See header for rationale.
    if (const char* env = std::getenv("HELIX_QIDI_BOX_WRITE");
        env != nullptr && *env != '\0' && *env != '0') {
        write_enabled_ = true;
        spdlog::info("{} Write-path ENABLED via HELIX_QIDI_BOX_WRITE", backend_log_tag());
    }

    spdlog::debug("{} Backend constructed ({} slots, write_enabled={})",
                  backend_log_tag(), NUM_SLOTS, write_enabled_);
}

AmsBackendQidi::~AmsBackendQidi() = default;

// --- Lifecycle hooks ---

void AmsBackendQidi::on_started() {
    if (!client_) {
        return;
    }
    // Bootstrap: notify_status_update only carries deltas, so we need an
    // initial snapshot to populate save_variables. Subscribe to the QIDI
    // objects too — Moonraker won't push notifications for anything we
    // haven't subscribed to.
    //
    // For now we query the entire save_variables namespace + box_extras
    // status; heater_box<N> and aht20_f heater_box<N> are wildcarded by
    // box_count so we issue follow-up subscribes lazily as box_count is
    // observed (TODO once we have field data).
    nlohmann::json params = {
        {"objects", nlohmann::json::object({
                        {"save_variables", nullptr},
                        {"box_extras", nullptr},
                    })},
    };

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token](nlohmann::json response) {
            // [L081] Mechanism C: defer member access to main thread.
            token.defer("AmsBackendQidi::on_started_apply",
                        [this, response = std::move(response)]() {
                            apply_query_response(response);
                        });
        });
    spdlog::info("{} Bootstrap query issued for save_variables + box_extras",
                 backend_log_tag());

    // Also fetch officiall_filas_list.cfg so the temperature profile cache
    // is ready by the time filament_slot<N> entries arrive. The path is the
    // canonical Klipper config location used by box_extras.py.
    if (api_) {
        auto fila_token = lifetime_.token();
        api_->transfers().download_file(
            "config", "officiall_filas_list.cfg",
            [this, fila_token](const std::string& body) {
                // [L081] Defer apply onto main thread — apply_filas_list
                // touches member state under mutex_.
                fila_token.defer("AmsBackendQidi::filas_list_apply",
                                 [this, body]() { apply_filas_list(body); });
            },
            [this](const MoonrakerError& err) {
                spdlog::debug("{} officiall_filas_list.cfg fetch failed: {} "
                              "(non-fatal — temps stay at defaults)",
                              backend_log_tag(), err.message);
            });
    }
}

void AmsBackendQidi::apply_query_response(const nlohmann::json& response) {
    if (!response.is_object()) {
        return;
    }
    auto result_it = response.find("result");
    if (result_it == response.end() || !result_it->is_object()) {
        return;
    }
    auto status_it = result_it->find("status");
    if (status_it == result_it->end() || !status_it->is_object()) {
        return;
    }
    // The status object has the same shape as a notify_status_update
    // payload — both are `{<object_name>: <fields>, ...}` — so reuse
    // the notification handler verbatim.
    handle_status_update(*status_it);
}

void AmsBackendQidi::handle_status_update(const nlohmann::json& notification) {
    if (!notification.is_object()) {
        return;
    }
    // Moonraker delivers save_variables changes as
    // `{"save_variables": {"variables": {...}}}`. Unwrap and feed the inner
    // variables payload to parse_save_variables.
    auto sv_it = notification.find("save_variables");
    if (sv_it != notification.end() && sv_it->is_object()) {
        auto vars_it = sv_it->find("variables");
        if (vars_it != sv_it->end() && vars_it->is_object()) {
            parse_save_variables(*vars_it);
        }
    }

    // Per-box drying state arrives as separate top-level objects:
    //   "heater_generic heater_box<N>" → {temperature, target, power}
    //   "aht20_f heater_box<N>"        → {temperature, humidity}
    // We surface the maximum temperature and maximum humidity across all
    // boxes onto AmsUnit::environment so the UI can show "drying" when
    // ANY box is active.
    apply_heater_status(notification);
}

void AmsBackendQidi::apply_heater_status(const nlohmann::json& notification) {
    // NOTE: heater_generic heater_box<N> rides on the heaters loop in
    // moonraker_discovery_sequence.cpp (subscribed with temperature+target).
    // aht20_f heater_box<N> is NOT subscribed today — the classifier in
    // discovery only recognises "temperature_sensor <name>" / "temperature_fan
    // <name>" / "tmc2240"/"tmc5160" as sensors, so the aht20_f object falls
    // through and Moonraker never pushes its humidity. Until that classifier
    // gains an aht20_* branch (and the sensor fields gain "humidity"), the
    // humidity path here only fires when the unit also publishes via some
    // other subscribed object — keep the parser tolerant either way.
    constexpr std::string_view kHeaterPrefix = "heater_generic heater_box";
    constexpr std::string_view kAht20Prefix = "aht20_f heater_box";

    std::optional<float> max_temp;
    std::optional<float> max_humidity;

    for (auto it = notification.begin(); it != notification.end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        const std::string& key = it.key();
        const bool is_heater = key.rfind(kHeaterPrefix, 0) == 0;
        const bool is_aht = key.rfind(kAht20Prefix, 0) == 0;
        if (!is_heater && !is_aht) {
            continue;
        }
        if (auto t_it = it->find("temperature");
            t_it != it->end() && t_it->is_number()) {
            const float v = t_it->get<float>();
            if (!max_temp || v > *max_temp) {
                max_temp = v;
            }
        }
        if (is_aht) {
            if (auto h_it = it->find("humidity");
                h_it != it->end() && h_it->is_number()) {
                const float v = h_it->get<float>();
                if (!max_humidity || v > *max_humidity) {
                    max_humidity = v;
                }
            }
        }
    }

    if (!max_temp && !max_humidity) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (system_info_.units.empty()) {
        return;
    }
    auto& env = system_info_.units[0].environment;
    if (!env) {
        env = EnvironmentData{};
    }
    if (max_temp) {
        env->temperature_c = *max_temp;
    }
    if (max_humidity) {
        env->humidity_pct = *max_humidity;
        env->has_humidity = true;
    }
}

void AmsBackendQidi::parse_save_variables(const nlohmann::json& variables) {
    if (!variables.is_object() || system_info_.units.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);

    // enable_box: master gate set by box_extras. 1 = active, 0/missing =
    // installed-but-disabled. Treat as the unit's "connected" state.
    auto enable_it = variables.find("enable_box");
    if (enable_it != variables.end() && enable_it->is_number_integer()) {
        system_info_.units[0].connected = (enable_it->get<int>() != 0);
    }

    // box_count: number of physical boxes detected by box_detect.py via USB
    // enumeration. Each box has 4 slots; chainable up to 4 boxes / 16 slots.
    // Resize the unit's slot vector to match, preserving any existing data
    // for slots that remain valid.
    auto box_count_it = variables.find("box_count");
    if (box_count_it != variables.end() && box_count_it->is_number_integer()) {
        int box_count = box_count_it->get<int>();
        if (box_count >= 1 && box_count <= 4) {
            const int desired_slots = box_count * NUM_SLOTS;
            AmsUnit& unit = system_info_.units[0];
            if (static_cast<int>(unit.slots.size()) != desired_slots) {
                unit.slots.resize(static_cast<size_t>(desired_slots));
                slot_rfid_.resize(static_cast<size_t>(desired_slots));
                for (size_t i = 0; i < unit.slots.size(); ++i) {
                    unit.slots[i].slot_index = static_cast<int>(i);
                    unit.slots[i].global_index = static_cast<int>(i);
                    if (unit.slots[i].mapped_tool < 0) {
                        unit.slots[i].mapped_tool = static_cast<int>(i);
                    }
                }
                unit.slot_count = desired_slots;
                system_info_.total_slots = desired_slots;
            }
        }
    }

    AmsUnit& unit_ref = system_info_.units[0];

    // value_t<N> = "slot<M>" — tool N prints from slot M. Apply over the
    // default tool=slot mapping established when the unit was sized.
    const int slot_count = static_cast<int>(unit_ref.slots.size());
    for (size_t t = 0; t < unit_ref.slots.size(); ++t) {
        const std::string key = "value_t" + std::to_string(t);
        auto vt_it = variables.find(key);
        if (vt_it == variables.end() || !vt_it->is_string()) {
            continue;
        }
        if (auto idx = parse_slot_name(vt_it->get<std::string>(), slot_count)) {
            unit_ref.slots[*idx].mapped_tool = static_cast<int>(t);
        }
    }

    // Per-slot state from `slot<N>` save_variables. box_stepper.py state
    // machine values:
    //   0  = empty
    //   1  = available (parked in box)
    //   2  = loaded all the way to extruder
    //   3  = mid-transition (treat as available; action belongs on system_info_.action)
    //   -1 = slot load failed
    //   -2 = extruder load failed
    //   -3 = runout-during-print
    // Negative values all map to BLOCKED so the UI surfaces an error chip.
    for (size_t i = 0; i < unit_ref.slots.size(); ++i) {
        const std::string key = "slot" + std::to_string(i);
        auto slot_it = variables.find(key);
        if (slot_it == variables.end() || !slot_it->is_number_integer()) {
            continue;
        }
        const int state = slot_it->get<int>();
        SlotStatus mapped;
        switch (state) {
        case 0:
            mapped = SlotStatus::EMPTY;
            break;
        case 1:
        case 3:
            mapped = SlotStatus::AVAILABLE;
            break;
        case 2:
            mapped = SlotStatus::LOADED;
            break;
        default:
            // -1, -2, -3 — all error states
            mapped = (state < 0) ? SlotStatus::BLOCKED : SlotStatus::UNKNOWN;
            break;
        }
        unit_ref.slots[i].status = mapped;
    }

    // last_load_slot is box_extras.py's authoritative "which slot is in the
    // extruder right now" signal. Two outcomes:
    //   "slot<N>"  → promote slot N to LOADED (covers the case where the
    //                per-slot signal hasn't caught up, e.g. recovery paths)
    //   "slot-1"   → demote any slot still claiming LOADED to AVAILABLE
    //                (nothing is in the extruder anymore)
    // is_tool_change: box_extras.py sets this to 1 while _BOX_CHANGE_FILAMENT
    // is mid-flight, clears to 0 on completion. Surface as AmsAction::LOADING
    // so the UI can show an in-flight indicator. (LOADING is the closest
    // existing action; there's no TOOL_CHANGING enum value yet.)
    auto tool_change_it = variables.find("is_tool_change");
    if (tool_change_it != variables.end() && tool_change_it->is_number_integer()) {
        system_info_.action =
            tool_change_it->get<int>() != 0 ? AmsAction::LOADING : AmsAction::IDLE;
    }

    auto load_it = variables.find("last_load_slot");
    if (load_it != variables.end() && load_it->is_string()) {
        const std::string val = load_it->get<std::string>();
        if (val == "slot-1") {
            for (auto& slot : unit_ref.slots) {
                if (slot.status == SlotStatus::LOADED) {
                    slot.status = SlotStatus::AVAILABLE;
                }
            }
            // Nothing loaded — clear the system-level cursors so
            // get_current_slot() / get_current_tool() / is_filament_loaded()
            // report the truth instead of stale -1 defaults.
            system_info_.current_slot = -1;
            system_info_.current_tool = -1;
            system_info_.filament_loaded = false;
        } else if (auto idx = parse_slot_name(val, slot_count)) {
            unit_ref.slots[*idx].status = SlotStatus::LOADED;
            system_info_.current_slot = *idx;
            system_info_.current_tool = unit_ref.slots[*idx].mapped_tool;
            system_info_.filament_loaded = true;
        }
    }

    // Per-slot RFID indices written by box_extras.py:
    //   filament_slot<N> = material index 1-99 (officiall_filas_list.cfg)
    //   color_slot<N>    = palette index 1-24
    //   vendor_slot<N>   = vendor index (always 1 observed so far)
    // Captured raw into slot_rfid_; resolved to material/color/brand by a
    // separate cfg-file lookup (not yet implemented).
    if (slot_rfid_.size() < unit_ref.slots.size()) {
        slot_rfid_.resize(unit_ref.slots.size());
    }
    for (size_t i = 0; i < unit_ref.slots.size(); ++i) {
        const std::string suffix = std::to_string(i);
        if (auto it = variables.find("filament_slot" + suffix);
            it != variables.end() && it->is_number_integer()) {
            slot_rfid_[i].filament_id = it->get<int>();
        }
        if (auto it = variables.find("color_slot" + suffix);
            it != variables.end() && it->is_number_integer()) {
            slot_rfid_[i].color_id = it->get<int>();
        }
        if (auto it = variables.find("vendor_slot" + suffix);
            it != variables.end() && it->is_number_integer()) {
            slot_rfid_[i].vendor_id = it->get<int>();
        }
        // Apply cached fila profile (from officiall_filas_list.cfg) when
        // we've seen a filament_id and have the matching section cached.
        if (slot_rfid_[i].filament_id > 0) {
            auto p = fila_profiles_.find(slot_rfid_[i].filament_id);
            if (p != fila_profiles_.end()) {
                unit_ref.slots[i].nozzle_temp_min = p->second.nozzle_min;
                unit_ref.slots[i].nozzle_temp_max = p->second.nozzle_max;
            }
        }
    }
}

void AmsBackendQidi::apply_filas_list(const std::string& content) {
    // Minimal ConfigParser-compatible INI: `[section]` headers, `key = value`
    // lines, `#` or `;` comments. Only sections matching `fila<N>` populate
    // the cache; everything else is silently ignored. Trailing tail content
    // after the value is dropped at the first whitespace+`#`/`;`.
    auto trim = [](std::string s) {
        const auto not_space = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        return s;
    };
    auto parse_int_field = [&](const std::string& v, int& out) {
        // Drop inline `;` / `#` tail if present.
        std::string body = v;
        for (char ch : {';', '#'}) {
            auto pos = body.find(ch);
            if (pos != std::string::npos) {
                body.erase(pos);
            }
        }
        try {
            out = std::stoi(trim(body));
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };

    std::map<int, FilaProfile> next;
    std::optional<int> current_id;
    FilaProfile current;

    auto flush = [&]() {
        if (current_id) {
            next[*current_id] = current;
            current_id.reset();
            current = FilaProfile{};
        }
    };

    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        std::string t = trim(line);
        if (t.empty() || t.front() == '#' || t.front() == ';') {
            continue;
        }
        if (t.front() == '[' && t.back() == ']') {
            flush();
            std::string section = trim(t.substr(1, t.size() - 2));
            // Only `fila<digits>` sections.
            if (section.rfind("fila", 0) == 0) {
                try {
                    int id = std::stoi(section.substr(4));
                    if (id > 0) {
                        current_id = id;
                    }
                } catch (const std::exception&) {
                    // Malformed section name — fall through to ignore.
                }
            }
            continue;
        }
        if (!current_id) {
            continue;
        }
        auto eq = t.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(t.substr(0, eq));
        std::string val = t.substr(eq + 1);
        if (key == "min_temp") {
            parse_int_field(val, current.nozzle_min);
        } else if (key == "max_temp") {
            parse_int_field(val, current.nozzle_max);
        } else if (key == "box_min_temp") {
            parse_int_field(val, current.box_min);
        } else if (key == "box_max_temp") {
            parse_int_field(val, current.box_max);
        }
    }
    flush();

    std::lock_guard<std::mutex> lock(mutex_);
    fila_profiles_ = std::move(next);
    spdlog::info("{} Loaded {} fila profile(s) from officiall_filas_list.cfg",
                 backend_log_tag(), fila_profiles_.size());
}

// --- State queries ---

AmsSystemInfo AmsBackendQidi::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

SlotInfo AmsBackendQidi::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= system_info_.total_slots) {
        return SlotInfo{};
    }
    const auto* slot = system_info_.get_slot_global(slot_index);
    return slot ? *slot : SlotInfo{};
}

bool AmsBackendQidi::is_bypass_active() const {
    return false;
}

// --- Path visualisation ---

PathSegment AmsBackendQidi::get_filament_segment() const {
    return PathSegment::NONE;
}

PathSegment AmsBackendQidi::get_slot_filament_segment(int /*slot_index*/) const {
    return PathSegment::NONE;
}

PathSegment AmsBackendQidi::infer_error_segment() const {
    return PathSegment::NONE;
}

// --- Filament operations ---
//
// All write-path methods are gated behind write_enabled_ (HELIX_QIDI_BOX_WRITE).
// Default disabled: every operation returns not_supported so the read-only
// state mirror can ship without risking unvalidated gcode on production
// hardware. Sib6019 (issue #954 author) opts in via env var for field tests;
// flip the gate to default-on once the gcode protocol is validated.

AmsError AmsBackendQidi::load_filament(int slot_index) {
    if (!write_enabled_) {
        return AmsErrorHelper::not_supported(
            "QIDI Box write-path disabled (set HELIX_QIDI_BOX_WRITE=1 for field testing)");
    }
    int tool = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (system_info_.units.empty()) {
            return AmsErrorHelper::not_supported("QIDI Box: no unit configured");
        }
        const auto& slots = system_info_.units[0].slots;
        if (slot_index < 0 || static_cast<size_t>(slot_index) >= slots.size()) {
            return AmsErrorHelper::not_supported("QIDI Box: slot index out of range");
        }
        tool = slots[slot_index].mapped_tool;
        if (tool < 0) {
            tool = slot_index;
        }
    }
    return execute_gcode("T" + std::to_string(tool));
}

AmsError AmsBackendQidi::unload_filament(int slot_index) {
    if (!write_enabled_) {
        return AmsErrorHelper::not_supported(
            "QIDI Box write-path disabled (set HELIX_QIDI_BOX_WRITE=1 for field testing)");
    }
    int tool = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (system_info_.units.empty()) {
            return AmsErrorHelper::not_supported("QIDI Box: no unit configured");
        }
        const auto& slots = system_info_.units[0].slots;
        if (slot_index == -1) {
            // Active slot: find the LOADED one.
            for (const auto& s : slots) {
                if (s.status == SlotStatus::LOADED) {
                    tool = (s.mapped_tool >= 0) ? s.mapped_tool : s.slot_index;
                    break;
                }
            }
            if (tool < 0) {
                return AmsErrorHelper::not_supported("QIDI Box: no slot currently loaded");
            }
        } else if (slot_index < 0 || static_cast<size_t>(slot_index) >= slots.size()) {
            return AmsErrorHelper::not_supported("QIDI Box: slot index out of range");
        } else {
            tool = slots[slot_index].mapped_tool;
            if (tool < 0) {
                tool = slot_index;
            }
        }
    }
    return execute_gcode("UNLOAD_T" + std::to_string(tool));
}

AmsError AmsBackendQidi::select_slot(int /*slot_index*/) {
    // QIDI Box doesn't have a "select without loading" operation — load_filament
    // is the only path. Reasonable callers should use load_filament directly.
    return AmsErrorHelper::not_supported("QIDI Box: select_slot not supported (use load_filament)");
}

AmsError AmsBackendQidi::change_tool(int tool_number) {
    if (!write_enabled_) {
        return AmsErrorHelper::not_supported(
            "QIDI Box write-path disabled (set HELIX_QIDI_BOX_WRITE=1 for field testing)");
    }
    if (tool_number < 0) {
        return AmsErrorHelper::not_supported("QIDI Box: tool number out of range");
    }
    return execute_gcode("T" + std::to_string(tool_number));
}

// --- Recovery ---

AmsError AmsBackendQidi::recover() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box recover");
}

AmsError AmsBackendQidi::reset() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box reset");
}

AmsError AmsBackendQidi::cancel() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box cancel");
}

// --- Configuration ---

AmsError AmsBackendQidi::set_slot_info(int /*slot_index*/, const SlotInfo& /*info*/,
                                       bool /*persist*/) {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box set_slot_info");
}

AmsError AmsBackendQidi::set_tool_mapping(int tool_number, int slot_index) {
    if (!write_enabled_) {
        return AmsErrorHelper::not_supported(
            "QIDI Box write-path disabled (set HELIX_QIDI_BOX_WRITE=1 for field testing)");
    }
    if (tool_number < 0) {
        return AmsErrorHelper::not_supported("QIDI Box: tool number out of range");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (system_info_.units.empty()) {
            return AmsErrorHelper::not_supported("QIDI Box: no unit configured");
        }
        const auto& slots = system_info_.units[0].slots;
        if (slot_index < 0 || static_cast<size_t>(slot_index) >= slots.size()) {
            return AmsErrorHelper::not_supported("QIDI Box: slot index out of range");
        }
    }
    // box_extras.py stores `value_t<N> = "slot<M>"` — same shape we parse on
    // the read-path. Quote the value to match Klipper's SAVE_VARIABLE syntax
    // for string values.
    return execute_gcode("SAVE_VARIABLE VARIABLE=value_t" + std::to_string(tool_number) +
                         " VALUE=\"slot" + std::to_string(slot_index) + "\"");
}

void AmsBackendQidi::clear_slot_override(int /*slot_index*/) {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
}

// --- Bypass ---

AmsError AmsBackendQidi::enable_bypass() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box enable_bypass");
}

AmsError AmsBackendQidi::disable_bypass() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box disable_bypass");
}
