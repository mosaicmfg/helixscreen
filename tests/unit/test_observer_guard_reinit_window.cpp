// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_observer_guard_reinit_window.cpp
 * @brief Regression tests for ObserverGuard removal coherence across the
 *        printer-state reinit window (ObserverGuard::invalidate_all()).
 *
 * Background (debug bundles 449TVQ82 / X3RA4252, v0.99.70 pi):
 *   A LedWidget observer on a STATIC subject (PrinterState-owned led_state /
 *   led_brightness) fired after its LambdaObserverContext had been freed,
 *   crashing in observe_int_sync<LedWidget>::_FUN (SIGSEGV in
 *   __aarch64_ldadd4_acq_rel — a shared_ptr refcount bump on freed memory).
 *
 *   Root cause: ObserverGuard::reset() used a single global boolean
 *   (s_subjects_valid). It is set false during teardown so that surviving
 *   singleton guards — whose subjects were just freed by
 *   StaticSubjectRegistry::deinit_all() — skip lv_observer_remove() on the
 *   already-freed observer. But new widgets are also created WHILE the flag is
 *   false (Application::init_printer_state() builds the home panel via
 *   finalize_setup() before revalidate_all()). An observer created in that
 *   window is attached to a LIVE subject; if it is reset() in the same window
 *   the boolean wrongly suppressed lv_observer_remove(), orphaning a live
 *   observer node on a live subject while its context was freed.
 *
 *   Fix: replace the boolean with a monotonic invalidation epoch. An observer
 *   skips removal only if it was created BEFORE the most recent
 *   invalidate_all() (its subject was genuinely freed by deinit_all());
 *   observers created during/after the window are removed normally.
 *
 * These tests FAIL against the boolean implementation and PASS after the fix.
 */

#include "ui_observer_guard.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "observer_factory.h"

#include <atomic>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

struct CountingPanel {
    int notifications = 0;
};

void drain() {
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

} // namespace

// An observer created DURING the reinit window (after invalidate_all, before
// revalidate_all) is attached to a live subject. Resetting it in the same
// window MUST remove it from the subject — otherwise it is orphaned and a
// later notify fires on the freed context (the production UAF).
TEST_CASE_METHOD(LVGLTestFixture,
                 "ObserverGuard removes window-created observer from a live subject",
                 "[observer][raii][crash_hardening]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    REQUIRE(lv_ll_get_len(&subject.subs_ll) == 0);

    CountingPanel panel;

    // Simulate teardown: deinit_all() freed old subjects' observers, then the
    // app flipped the guard state for surviving singletons.
    ObserverGuard::invalidate_all();

    // Simulate init_printer_state() building new widgets in the window: this
    // observer is registered on a freshly-(re)created, LIVE subject.
    ObserverGuard guard = helix::ui::observe_int_sync<CountingPanel>(
        &subject, &panel, [](CountingPanel* p, int /*v*/) { p->notifications++; });
    REQUIRE(lv_ll_get_len(&subject.subs_ll) == 1);

    // Still in the window, the widget rebinds/detaches and resets the guard.
    guard.reset();

    // The observer MUST be gone. Boolean impl skips removal here → len == 1.
    REQUIRE(lv_ll_get_len(&subject.subs_ll) == 0);

    // App finishes init.
    ObserverGuard::revalidate_all();

    // A later status update must not reach the freed context.
    panel.notifications = 0;
    lv_subject_set_int(&subject, 99);
    drain();
    REQUIRE(panel.notifications == 0);

    lv_subject_deinit(&subject);
}

// Guardrail: the protective behavior must be preserved. A guard created BEFORE
// invalidate_all(), whose subject was freed by deinit_all() during teardown,
// must SKIP lv_observer_remove() (the observer pointer is already dangling) —
// reset() must not crash.
TEST_CASE_METHOD(LVGLTestFixture, "ObserverGuard skips removal for observers freed by deinit_all()",
                 "[observer][raii][crash_hardening]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);

    CountingPanel panel;

    // Created during normal operation (before any teardown).
    ObserverGuard guard = helix::ui::observe_int_sync<CountingPanel>(
        &subject, &panel, [](CountingPanel* p, int /*v*/) { p->notifications++; });
    REQUIRE(lv_ll_get_len(&subject.subs_ll) == 1);

    // Teardown: invalidate_all() marks the epoch, then deinit frees the
    // subject's observers (as StaticSubjectRegistry::deinit_all() would).
    ObserverGuard::invalidate_all();
    lv_subject_deinit(&subject);

    // reset() must detect the observer predates the invalidation and skip
    // lv_observer_remove() on the now-freed pointer. No crash, no assertion.
    REQUIRE_NOTHROW(guard.reset());

    ObserverGuard::revalidate_all();
}
