// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "hv/json.hpp"

#include <mutex>
#include <string>

class MoonrakerAPI;

namespace helix {
class MoonrakerClient;

/// Centralizes Klipper/Moonraker gcode-error surfacing for HelixScreen.
///
/// Two input paths feed in:
///   1. Live `notify_gcode_response` broadcast — fires once per event.
///   2. `server.gcode_store` replay on (re)connect — catches errors that
///      fired while HelixScreen was offline (broken boot autostart, crash
///      recovery, WebSocket bounce). Without this, reconnecting to a
///      paused printer shows no error context; the chinglish stays buried
///      in klippy.log.
///
/// Translation: `!! {"code":"key849",...}` → `CfsErrorDecoder` →
/// "Retract failed — filament stuck in connector in unit 1 slot A.
///  Manually pull the filament back through the connector".
///
/// Routing:
///   - `key8xx` (CFS hardware) → modal "Filament System Error"
///   - `key298` (MCU bridge)   → toast with "Recover" action
///   - other `!!`              → deferred toast (RPC-correlation dedup)
///   - `Error:` lines          → toast
///   - replay path             → modal (the user was disconnected; a
///                                transient toast they can miss is not
///                                enough on first reconnect)
///
/// Lifetime: owned by `Application`. Registers callbacks in the ctor and
/// unregisters them in the dtor; the MoonrakerClient pointer is not
/// owned and must outlive this router.
class GcodeErrorRouter {
  public:
    GcodeErrorRouter(MoonrakerAPI* api, MoonrakerClient* client);
    ~GcodeErrorRouter();

    GcodeErrorRouter(const GcodeErrorRouter&) = delete;
    GcodeErrorRouter& operator=(const GcodeErrorRouter&) = delete;

  private:
    /// Live `notify_gcode_response` handler — runs on the WS thread.
    void on_notify_gcode_response(const nlohmann::json& msg);

    /// Fires on every WS connect / Klippy ready transition. Queries
    /// `server.gcode_store` and replays the most recent `!!` line that
    /// passes age + dedup gates.
    void on_connected();

    /// Walks a single response line through translate + emit. Used by
    /// both the live path and the replay path (replay only feeds `!!`
    /// lines; this still handles `Error:` for the live caller).
    void process_line(const std::string& line);

    /// Splits a raw response line into translated `text` plus extracted
    /// `out_code`. Static + side-effect free so the replay path can
    /// reuse it without holding any router state.
    static void clean_error_text(std::string& text, std::string& out_code);

    /// Bytes-only truncation for transient toasts. Modals always get the
    /// full text — they wrap to multiple lines.
    static std::string truncate_for_toast(std::string text);

    MoonrakerAPI* api_;
    MoonrakerClient* client_;

    /// Dedup state for the replay path. Multiple WS events can fire the
    /// connected observer in quick succession (WS open → Klippy ready);
    /// without this we'd modal the same error twice.
    std::mutex replay_mutex_;
    double last_replayed_time_ = 0.0;
};

}  // namespace helix
