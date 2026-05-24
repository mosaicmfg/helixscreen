// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_error_router.h"

#include "moonraker_api.h"
#include "moonraker_client.h"
#include "moonraker_error.h"
#include "moonraker_types.h"
#include "printer_recovery_service.h"
#include "rpc_error_correlation.h"
#include "ui_modal.h"
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

/// One-tap recovery action attached to specific CFS error codes.
struct RecoveryAction {
    const char* button_label;  ///< Label like "Reset CFS"
    const char* gcode;         ///< GCode to send on tap
    const char* log_tag;       ///< For spdlog::info on tap
};

/// Context passed to the modal's confirm/cancel callbacks. Heap-
/// allocated by the call site, freed in whichever callback fires.
struct RecoveryCtx {
    MoonrakerAPI* api;
    const RecoveryAction* action;  ///< Points at a static RecoveryAction
};

/// Lookup: which key codes get an actionable button.
///
/// Conservative list — only codes where a software action is genuinely
/// curative. Most slot-level errors (key849 retract stuck, key835-839
/// extrude jams) need a physical fix; a button there would mislead the
/// user. Add codes here as we identify real one-tap recoveries.
const RecoveryAction* find_recovery(const std::string& code) {
    // key840: "box switch state error" — state machine in an inconsistent
    // state. BOX_ERROR_CLEAR resets the box driver state and lets the
    // user retry the operation. Same gcode AmsBackendCfs::reset_gcode()
    // already exposes via the AMS panel.
    static const RecoveryAction key840 = {
        "Reset CFS", "BOX_ERROR_CLEAR", "GcodeErrorRouter::key840_reset"};
    if (code == "key840") return &key840;

    return nullptr;
}

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

    // [L072] Both registrations run on the WS thread. MoonrakerClient
    // copies the callback list under lock and invokes outside it, so
    // unregister_method_callback / remove_connected_observer in our dtor
    // do NOT block in-flight invocations. lifetime_.bg_cb wraps the
    // delivery: when the WS thread fires the wrapper, it queues `fn` to
    // the main thread with a generation snapshot; on main-thread dispatch
    // the gen is re-checked, so a callback that fires after the dtor
    // invalidates `lifetime_` is silently dropped.
    client_->register_method_callback(
        "notify_gcode_response", kNotifyHandlerName,
        lifetime_.bg_cb("GcodeErrorRouter::on_notify",
                        [this](const nlohmann::json& msg) {
                            on_notify_gcode_response(msg);
                        }));

    // Reconnect replay. Fires on WS open + Klippy ready transitions.
    // bg_cb takes a 0-arg callback fine — the lambda below doesn't need
    // arguments; the wrapper just defers and gen-checks.
    client_->add_connected_observer(
        kReplayObserverName,
        lifetime_.bg_cb("GcodeErrorRouter::on_connected", [this]() { on_connected(); }));
}

GcodeErrorRouter::~GcodeErrorRouter() {
    // Erase the map entries so no NEW invocations start after this point.
    // In-flight invocations (already past the map lookup, queued for dispatch)
    // are handled by lifetime_'s generation guard — see the bg_cb usage in
    // the ctor. lifetime_ destructs after this body returns and invalidates
    // all outstanding tokens, so any deferred body that lands on main after
    // the unregister is silently dropped.
    if (client_) {
        client_->unregister_method_callback("notify_gcode_response", kNotifyHandlerName);
        client_->remove_connected_observer(kReplayObserverName);
    }
}

void GcodeErrorRouter::clean_error_text(std::string& text, std::string& out_code) {
    out_code.clear();

    // K2's Klipper builds emit errors in two shapes:
    //   1. Pure JSON:     `{"code":"key849","msg":"...","values":[...]}`
    //   2. Embedded JSON: `Internal error during connect: !{"code":"key298",...}`
    //      (observed K2 Plus 2026-05-24 when klipper_mcu shutdown)
    // Scan for the first `{"code":` anywhere in the line. If found, parse
    // from there; otherwise fall through to the heuristic rewrites.
    auto json_start = text.find("{\"code\"");
    if (json_start != std::string::npos) {
        // Brace-balance forward from json_start to find the matching close
        // brace, ignoring `{`/`}` inside string literals. nlohmann::parse
        // requires whole-input — it won't ignore trailing garbage — so we
        // extract just [json_start, obj_end) before parsing.
        size_t i = json_start;
        int depth = 0;
        bool in_string = false;
        bool escape = false;
        size_t obj_end = std::string::npos;
        for (; i < text.size(); ++i) {
            char c = text[i];
            if (in_string) {
                if (escape) {
                    escape = false;
                } else if (c == '\\') {
                    escape = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}') {
                if (--depth == 0) {
                    obj_end = i + 1;
                    break;
                }
            }
        }

        if (obj_end != std::string::npos) {
            std::string json_str = text.substr(json_start, obj_end - json_start);
            try {
                auto j = nlohmann::json::parse(json_str);
                if (j.contains("code") && j["code"].is_string()) {
                    out_code = j["code"].get<std::string>();
                    nlohmann::json values = nlohmann::json::array();
                    if (j.contains("values")) {
                        values = j["values"];
                    }
#if HELIX_HAS_CFS
                    if (auto friendly =
                            printer::CfsErrorDecoder::lookup_message_with_values(
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
                // Malformed JSON despite the {"code" prefix — leave text
                // untouched and fall through to heuristic patterns.
            }
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
            // If the code has a registered recovery action, surface as a
            // confirmation modal with a one-tap fix button; otherwise a
            // plain alert.
            if (auto* rec = find_recovery(code); rec && api_) {
                const char* title = lv_tr("Filament System Error");

                // Dedup against an already-showing modal with the same title.
                // modal_show_confirmation does NOT dedup internally (only
                // ui_notification_error does); without this, two rapid key840
                // events would stack two modals and leak two RecoveryCtxs.
                if (lv_obj_t* top = helix::ui::modal_get_top()) {
                    if (lv_obj_t* title_label =
                            lv_obj_find_by_name(top, "dialog_title")) {
                        const char* existing = lv_label_get_text(title_label);
                        if (existing && strcmp(existing, title) == 0) {
                            spdlog::debug("[GcodeError] Skipping duplicate "
                                          "recovery modal for {}", code);
                            return;
                        }
                    }
                }

                // Heap-allocate ctx for the recovery callback. Lifetime is
                // tied to the dialog widget via LV_EVENT_DELETE — fires
                // unconditionally when the dialog is destroyed (button tap,
                // backdrop dismiss, ESC, ModalStack::clear() on shutdown),
                // so the ctx is freed exactly once regardless of dismissal
                // path. confirm/cancel cbs only invoke the action; the
                // DELETE cb is the sole owner of the free.
                auto* ctx = new RecoveryCtx{api_, rec};
                lv_obj_t* dialog = helix::ui::modal_show_confirmation(
                    title, clean.c_str(), ModalSeverity::Error, rec->button_label,
                    [](lv_event_t* e) {
                        auto* c = static_cast<RecoveryCtx*>(lv_event_get_user_data(e));
                        if (!c || !c->api || !c->action) return;
                        const char* tag = c->action->log_tag;
                        spdlog::info("[GcodeError] User tapped recovery: {}", tag);
                        c->api->execute_gcode(
                            c->action->gcode,
                            [tag]() { spdlog::info("[Recovery] {} completed", tag); },
                            [tag](const MoonrakerError& err) {
                                spdlog::error("[Recovery] {} failed: {}", tag, err.message);
                                ToastManager::instance().show(
                                    ToastSeverity::ERROR,
                                    ("Recovery failed: " + err.user_message()).c_str(), 6000);
                            },
                            MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
                    },
                    /*on_cancel=*/nullptr,  // DELETE cb handles all cleanup
                    ctx);

                if (!dialog) {
                    // modal_show_confirmation logs internally on failure.
                    // ctx never reaches a callback; free directly.
                    spdlog::warn("[GcodeError] modal_show_confirmation returned null; "
                                 "discarding recovery ctx for {}", code);
                    delete ctx;
                    return;
                }

                lv_obj_add_event_cb(
                    dialog,
                    [](lv_event_t* e) {
                        delete static_cast<RecoveryCtx*>(lv_event_get_user_data(e));
                    },
                    LV_EVENT_DELETE, ctx);
                return;
            }
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
    // [L072] get_gcode_store's success callback fires on the WS thread when
    // Moonraker responds. The request tracker holds the callback for the
    // duration of the RPC, so a late response delivered after our dtor would
    // otherwise re-enter `this` on freed memory. bg_cb defers to main with
    // a generation guard.
    client_->get_gcode_store(
        kReplayFetchCount,
        lifetime_.bg_cb(
            "GcodeErrorRouter::replay_response",
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
        }),
        [](const MoonrakerError& err) {
            spdlog::debug("[GcodeError replay] gcode_store query failed: {}", err.message);
        });
}

}  // namespace helix
