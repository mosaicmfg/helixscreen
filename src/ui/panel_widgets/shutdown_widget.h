// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "ui_shutdown_modal.h"

#include "panel_widget.h"

class MoonrakerAPI;

namespace helix {

class ShutdownWidget : public PanelWidget {
  public:
    explicit ShutdownWidget(MoonrakerAPI* api);
    ~ShutdownWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "shutdown";
    }

    // XML event callback (public for early registration)
    static void shutdown_clicked_cb(lv_event_t* e);

  private:
    MoonrakerAPI* api_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* shutdown_btn_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    ShutdownModal shutdown_modal_;

    // Lifetime guard for async callback safety
    helix::AsyncLifetimeGuard lifetime_;

    void handle_click();
};

void register_shutdown_widget();

/// Configure @p modal with single-/dual-scope callbacks for @p api (matching
/// the home-panel widget's behavior — including local-fallback when Moonraker
/// is disconnected) and show it as a child of @p parent_screen.
///
/// Caller owns @p modal and @p lifetime; both must outlive the modal. The
/// lifetime guard is required for the "shutdown both" / "reboot both" flows
/// that defer the local SystemPower call until the printer-side ack.
void show_shutdown_dialog(MoonrakerAPI* api,
                          ShutdownModal& modal,
                          AsyncLifetimeGuard& lifetime,
                          lv_obj_t* parent_screen);

} // namespace helix
