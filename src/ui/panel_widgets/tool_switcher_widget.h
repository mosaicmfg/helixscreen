// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "panel_widget.h"
#include "ui_observer_guard.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace helix {

class PrinterState;

class ToolSwitcherWidget : public PanelWidget {
  public:
    explicit ToolSwitcherWidget(PrinterState& printer_state);
    ~ToolSwitcherWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override { return "tool_switcher"; }
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool has_overlay_open() const override { return picker_backdrop_ != nullptr; }

    // Static instance tracker for callbacks from static event handlers
    static ToolSwitcherWidget* s_active_instance;

  private:
    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* picker_backdrop_ = nullptr;

    int current_colspan_ = 1;
    int current_rowspan_ = 1;

    ObserverGuard active_tool_observer_;
    ObserverGuard tool_count_observer_;

    std::vector<lv_obj_t*> pill_buttons_;

    // Grid layout descriptors for multi-row pill layout. LVGL stores these
    // pointers (no copy), so the backing arrays must outlive the layout.
    std::vector<int32_t> grid_col_dsc_;
    std::vector<int32_t> grid_row_dsc_;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer destructs. Without this, queued observer callbacks captured
    // via tok.defer() see token.expired() == false after the observers are
    // already gone and dereference a half-destroyed widget. See temp_stack_widget.h
    // (commit 45abc8c2a, bundle AX3CKAKB).
    helix::AsyncLifetimeGuard lifetime_;

    void rebuild_pills();
    void rebuild_compact();
    void show_tool_picker();
    void dismiss_tool_picker();
    void handle_tool_selected(int tool_index);
    void on_active_tool_changed(int tool_index);

  public:
    static void tool_pill_cb(lv_event_t* e);
    static void tool_compact_cb(lv_event_t* e);
};

void register_tool_switcher_widget();

} // namespace helix
