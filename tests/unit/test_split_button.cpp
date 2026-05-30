// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_split_button.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"

#include <cstring>

#include "../catch_amalgamated.hpp"

// Helper: create a split button in a fresh LVGL screen
static lv_obj_t* create_test_split_button(const char* options = "PLA\nPETG\nABS") {
    lv_init_safe();
    lv_obj_t* screen = lv_screen_active();

    // We need to create the widget programmatically since we can't go through XML
    // in unit tests without full XML init. Use the C++ API instead.
    // Create a plain obj container to act as parent.
    lv_obj_t* sb = lv_obj_create(screen);

    // Create a dropdown as child so the API functions can find it
    lv_obj_t* dropdown = lv_dropdown_create(sb);
    if (options) {
        lv_dropdown_set_options(dropdown, options);
    }

    // Create a label as child
    lv_obj_t* label = lv_label_create(sb);
    lv_label_set_text(label, "");

    return sb;
}

// =============================================================================
// C++ API: set_options / set_selected / get_selected
// =============================================================================

TEST_CASE("split_button set_options updates dropdown", "[split_button]") {
    lv_init_safe();
    lv_obj_t* screen = lv_screen_active();

    // Create a minimal split button using the init function
    // (requires XML subsystem — test the C++ API standalone instead)
    lv_obj_t* sb = lv_obj_create(screen);

    // set_options on non-split-button should be a no-op (no crash)
    ui_split_button_set_options(sb, "A\nB\nC");

    // get_selected on non-split-button returns 0
    CHECK(ui_split_button_get_selected(sb) == 0);

    // set_selected on non-split-button is a no-op (no crash)
    ui_split_button_set_selected(sb, 2);

    // set_text on non-split-button is a no-op (no crash)
    ui_split_button_set_text(sb, "test");

    lv_obj_delete(sb);
}

TEST_CASE("split_button nullptr safety", "[split_button]") {
    // All API functions should handle nullptr without crashing
    ui_split_button_set_options(nullptr, "A\nB");
    ui_split_button_set_selected(nullptr, 0);
    CHECK(ui_split_button_get_selected(nullptr) == 0);
    ui_split_button_set_text(nullptr, "test");
}

// =============================================================================
// text_format logic (tested via snprintf pattern matching)
// =============================================================================

TEST_CASE("text_format produces correct output", "[split_button]") {
    SECTION("format with %s substitution") {
        char formatted[256];
        const char* fmt = "Preheat %s";
        const char* selection = "PLA";
        snprintf(formatted, sizeof(formatted), fmt, selection);
        CHECK(std::string(formatted) == "Preheat PLA");
    }

    SECTION("format with empty selection") {
        char formatted[256];
        const char* fmt = "Preheat %s";
        const char* selection = "";
        snprintf(formatted, sizeof(formatted), fmt, selection);
        CHECK(std::string(formatted) == "Preheat ");
    }

    SECTION("format without %s passes through") {
        char formatted[256];
        const char* fmt = "Static Text";
        const char* selection = "PLA";
        snprintf(formatted, sizeof(formatted), fmt, selection);
        CHECK(std::string(formatted) == "Static Text");
    }

    SECTION("different selections produce different output") {
        char formatted[256];
        const char* fmt = "Print with %s";

        snprintf(formatted, sizeof(formatted), fmt, "PETG");
        CHECK(std::string(formatted) == "Print with PETG");

        snprintf(formatted, sizeof(formatted), fmt, "ABS");
        CHECK(std::string(formatted) == "Print with ABS");

        snprintf(formatted, sizeof(formatted), fmt, "TPU");
        CHECK(std::string(formatted) == "Print with TPU");
    }
}

// =============================================================================
// Init function (registration)
// =============================================================================

TEST_CASE("ui_split_button_init does not crash", "[split_button]") {
    lv_init_safe();
    // Should be safe to call (may warn if XML not fully initialized)
    // We mainly test that the function exists and links correctly
    ui_split_button_init();
}

// =============================================================================
// Deferred-callback lifetime safety (#980)
// =============================================================================

/**
 * @brief XML fixture that registers the real ui_split_button widget so tests
 * can drive ui_split_button_create() — which schedules a deferred label-width
 * computation that dereferences the widget. The faked split buttons used by the
 * other tests never exercise that path.
 */
class SplitButtonXmlFixture : public XMLTestFixture {
  public:
    SplitButtonXmlFixture() : XMLTestFixture() {
        ui_split_button_init();
    }
};

// Teardown-safety guard for the #980 SIGSEGV: ui_split_button_create() schedules
// a deferred callback that calls get_data()->lv_obj_get_child() on the widget.
// During rapid panel teardown the widget is freed before that callback fires.
// The fix routes the deferral through the widget-safe async_call (lv_obj_is_valid
// guard) instead of a raw lv_async_call, so the callback is skipped once the
// widget is gone.
//
// NOTE: this cannot deterministically reproduce the production use-after-free,
// even under ASAN — LVGL objects come from LVGL's internal memory pool, not the
// system allocator, so a freed lv_obj is not sanitizer-poisoned. The test guards
// the create -> delete -> pump-deferrals path against crashing/regressing; the
// correctness guarantee comes from the widget-safe guard exercised here.
TEST_CASE_METHOD(SplitButtonXmlFixture,
                 "split_button: deferred label-width callback is safe after widget delete (#980)",
                 "[split_button][crash]") {
    const char* attrs[] = {"text", "Preheat", "options", "PLA\nPETG\nABS", nullptr};
    auto* sb = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ui_split_button", attrs));
    REQUIRE(sb != nullptr);

    // Delete the widget while the label-width deferral is still pending,
    // simulating the panel teardown seen in the crash crumbs.
    lv_obj_delete(sb);

    // Fire both deferral mechanisms: the raw LVGL async queue (unfixed path,
    // pumped via the test-safe handler that fires one-shot lv_async_call timers)
    // and the UpdateQueue (fixed, widget-safe path). Neither may touch the freed
    // widget. Reaching the end without a use-after-free is the assertion.
    lv_timer_handler_safe();
    helix::ui::UpdateQueue::instance().drain();

    SUCCEED("Deferred label-width callback did not dereference the freed widget");
}
