// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_error_router.h"

#include "moonraker_api.h"
#include "moonraker_client.h"
#include "moonraker_error.h"
#include "moonraker_types.h"
#include "printer_recovery_service.h"
#include "rpc_error_correlation.h"
#include "ui_notification.h"
#include "ui_toast_manager.h"

#if HELIX_HAS_CFS
#include "ams_backend_cfs.h"
#endif

#include "lvgl.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <vector>

namespace helix {

namespace {

constexpr const char* kNotifyHandlerName = "gcode_error_notifier";
constexpr const char* kReplayObserverName = "gcode_store_replay";

/// Replay age gate: errors older than this in the gcode_store are
/// considered stale (probably already acknowledged by the user) and
/// not re-surfaced on reconnect.
constexpr double kReplayMaxAgeSeconds = 600.0;

/// gcode_store fetch depth on reconnect. The K2's box driver is chatty
/// (status polls every ~3s) so we need headroom to find a `!!` line.
constexpr int kReplayFetchCount = 50;

double now_unix_seconds() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

GcodeErrorRouter::GcodeErrorRouter(MoonrakerAPI* api, MoonrakerClient* client)
    : api_(api), client_(client) {
    if (!client_) {
        spdlog::warn("[GcodeErrorRouter] Null client — handlers not registered");
        return;
    }

    // Live broadcast handler. Captures `this`; the dtor unregisters
    // before MoonrakerClient is destroyed, so the WS thread can't fire
    // on a dangling pointer.
    client_->register_method_callback(
        "notify_gcode_response", kNotifyHandlerName,
        [this](const nlohmann::json& msg) { on_notify_gcode_response(msg); });

    // Reconnect replay. Fires on WS open + Klippy ready transitions.
    client_->add_connected_observer(kReplayObserverName, [this]() { on_connected(); });
}

GcodeErrorRouter::~GcodeErrorRouter() {
    if (client_) {
        client_->unregister_method_callback("notify_gcode_response", kNotifyHandlerName);
        client_->remove_connected_observer(kReplayObserverName);
    }
}

void GcodeErrorRouter::clean_error_text(std::string& text, std::string& out_code) {
    out_code.clear();

    // JSON-shape errors carry a key code we can translate via the CFS
    // decoder. Falls back to the raw `msg` when the code isn't in the
    // table; falls through entirely on parse failure.
    if (!text.empty() && text[0] == '{') {
        try {
            auto j = nlohmann::json::parse(text);
            if (j.contains("code") && j["code"].is_string()) {
                out_code = j["code"].get<std::string>();
                nlohmann::json values = nlohmann::json::array();
                if (j.contains("values")) {
                    values = j["values"];
                }
#if HELIX_HAS_CFS
                if (auto friendly = printer::CfsErrorDecoder::lookup_message_with_values(
                        out_code, values)) {
                    text = friendly->first + ". " + friendly->second;
                    return;
                }
#else
                (void)values;
#endif
            }
            if (j.contains("msg") && j["msg"].is_string()) {
                text = j["msg"].get<std::string>();
            }
        } catch (...) {
            // Not valid JSON — use as-is
        }
    }

    // Heuristic friendlier-text rewrites for common non-coded patterns.
    if (text.find("Must home axis") != std::string::npos ||
        text.find("must home") != std::string::npos) {
        text = lv_tr("Must home axes first");
        return;
    }
    if (text.find("spi_transfer_response") != std::string::npos) {
        text = lv_tr("Accelerometer communication failed. Try again.");
        return;
    }
}

std::string GcodeErrorRouter::truncate_for_toast(std::string text) {
    // UTF-8 byte truncation is not strictly correct (could land mid-codepoint),
    // but matches prior behavior. The right long-term fix is wrapping text in
    // ToastManager; at that point this goes away.
    constexpr size_t MAX_LEN = 80;
    if (text.size() > MAX_LEN) {
        text = text.substr(0, MAX_LEN - 3) + "...";
    }
    return text;
}

void GcodeErrorRouter::process_line(const std::string& line) {
    if (line.empty()) return;

    // Klipper emergency errors: "!! MCU shutdown", "!! Timer too close",
    // or "!! {json}" from CFS / motor_control wrappers.
    if (line.size() >= 2 && line[0] == '!' && line[1] == '!') {
        std::string clean =
            (line.size() > 3 && line[2] == ' ') ? line.substr(3) : line.substr(2);
        std::string code;
        clean_error_text(clean, code);
        spdlog::error("[GcodeError] Emergency: {} (code={})", clean,
                      code.empty() ? "-" : code);

        // Cross-source dedup: when an RPC caller triggered the gcode that
        // emitted this `!!`, the caller's error_cb already surfaced a
        // contextual toast ("Failed to resume print: ..."). Skipping our
        // generic toast avoids double-notification for the same root cause.
        //
        // The broadcast `!!` and the JSON-RPC error response can arrive in
        // either order — the broadcast often beats the RPC response on
        // slow devices, so the sync check below can miss late arrivals.
        // We re-check after a 150ms timer below.
        if (rpc_error_correlation::was_recently_handled(clean)) {
            spdlog::info("[GcodeError] Suppressing duplicate `!!` toast "
                         "(caller-handled RPC error already recorded): {}",
                         clean);
            return;
        }

        // key298 — rpi MCU bridge daemon shutdown. firmware_restart alone
        // can't recover; PrinterRecoveryService bounces klipper_mcu via
        // the platform recovery script.
        if (code == "key298" && api_) {
            MoonrakerAPI* api = api_;
            ToastManager::instance().show_with_action(
                ToastSeverity::ERROR, truncate_for_toast(clean).c_str(), lv_tr("Recover"),
                [](void* ud) {
                    auto* a = static_cast<MoonrakerAPI*>(ud);
                    if (!a) return;
                    spdlog::info("[GcodeError] User tapped Recover for key298");
                    PrinterRecoveryService recovery(a);
                    recovery.recover(
                        []() { spdlog::info("[Recovery] Auto-recovery initiated"); },
                        [](const MoonrakerError& err) {
                            spdlog::error("[Recovery] Auto-recovery failed: {}", err.message);
                            ToastManager::instance().show(
                                ToastSeverity::ERROR,
                                ("Recovery failed: " + err.user_message()).c_str(), 6000);
                        });
                },
                api, /*duration_ms=*/15000);
            return;
        }

        // key8xx — CFS / motor hardware faults. Modal because the AMS step
        // indicator otherwise hides the failure (box driver emits via
        // respond_raw and the dispatch script still returns success).
        // ui_notification's modal dedup-by-title collapses bursts.
        if (code.size() >= 4 && code.compare(0, 4, "key8") == 0) {
            ui_notification_error(lv_tr("Filament System Error"), clean.c_str(),
                                  /*modal=*/true);
            return;
        }

        // Deferred toast for unclassified `!!` — gives the late-arrival
        // RPC error response a chance to populate the correlation buffer
        // before we re-check at fire time.
        struct DeferredCtx {
            std::string clean;
            std::string short_form;
        };
        auto* dctx = new DeferredCtx{clean, truncate_for_toast(clean)};
        auto* dt = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* c = static_cast<DeferredCtx*>(lv_timer_get_user_data(timer));
                if (c) {
                    if (rpc_error_correlation::was_recently_handled(c->clean)) {
                        spdlog::info(
                            "[GcodeError] Suppressing deferred `!!` toast "
                            "(caller-handled RPC error arrived after): {}",
                            c->clean);
                    } else {
                        ui_notification_error("Klipper Error", c->short_form.c_str(),
                                              /*modal=*/false);
                    }
                    delete c;
                }
                lv_timer_delete(timer);
            },
            150, dctx);
        lv_timer_set_repeat_count(dt, 1);
        return;
    }

    // Command errors: "Error: Must home before probe", etc.
    if (line.size() >= 5) {
        std::string prefix = line.substr(0, 5);
        for (auto& c : prefix) c = static_cast<char>(std::tolower(c));
        if (prefix == "error") {
            std::string clean = line;
            if (line.size() > 7 && line[5] == ':' && line[6] == ' ') {
                clean = line.substr(7);
            } else if (line.size() > 6 && line[5] == ':') {
                clean = line.substr(6);
            }
            std::string code;
            clean_error_text(clean, code);
            spdlog::error("[GcodeError] {}", clean);
            ui_notification_error(nullptr, truncate_for_toast(clean).c_str(),
                                  /*modal=*/false);
        }
    }
}

void GcodeErrorRouter::on_notify_gcode_response(const nlohmann::json& msg) {
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }

    const auto& params = msg["params"];
    if (params[0].is_array()) {
        for (const auto& line : params[0]) {
            if (line.is_string()) {
                process_line(line.get<std::string>());
            }
        }
    } else if (params[0].is_string()) {
        for (const auto& line : params) {
            if (line.is_string()) {
                process_line(line.get<std::string>());
            }
        }
    }
}

void GcodeErrorRouter::on_connected() {
    if (!client_) return;
    client_->get_gcode_store(
        kReplayFetchCount,
        [this](const std::vector<GcodeStoreEntry>& entries) {
            // gcode_store is oldest-first; walk reverse for newest.
            for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                if (it->type != "response") continue;
                const std::string& raw = it->message;
                if (raw.size() < 3 || raw[0] != '!' || raw[1] != '!') continue;

                const double age = now_unix_seconds() - it->time;
                if (age > kReplayMaxAgeSeconds) {
                    spdlog::debug("[GcodeError replay] Skipping stale `!!` (age {:.0f}s)",
                                  age);
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(replay_mutex_);
                    if (it->time == last_replayed_time_) {
                        spdlog::debug("[GcodeError replay] Already replayed t={}",
                                      it->time);
                        return;
                    }
                    last_replayed_time_ = it->time;
                }

                std::string clean =
                    (raw.size() > 3 && raw[2] == ' ') ? raw.substr(3) : raw.substr(2);
                std::string code;
                clean_error_text(clean, code);

                spdlog::info(
                    "[GcodeError replay] Surfacing prior `!!` (age {:.0f}s, code={}): {}",
                    age, code.empty() ? "-" : code, clean);

                // Replay is always modal — the user was disconnected; a
                // transient toast they can miss isn't enough on first
                // reconnect. Modal dedup-by-title prevents the live
                // notify_gcode_response (if Klippy re-emits) from
                // duplicating.
                const char* title =
                    (code.size() >= 4 && code.compare(0, 4, "key8") == 0)
                        ? lv_tr("Filament System Error")
                        : lv_tr("Printer Error");
                ui_notification_error(title, clean.c_str(), /*modal=*/true);
                return;
            }
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[GcodeError replay] gcode_store query failed: {}", err.message);
        });
}

}  // namespace helix
