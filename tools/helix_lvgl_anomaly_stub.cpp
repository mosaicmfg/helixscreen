// SPDX-License-Identifier: GPL-3.0-or-later
//
// No-op stubs for the LVGL→helixscreen C bridges, used by helix-splash and
// helix-watchdog. Both link against the patched LVGL (which references these
// bridges for telemetry / crash diagnosis) but do not link the telemetry
// manager or the crash handler. The real implementations live in
// src/system/helix_lvgl_anomaly.cpp and src/system/crash_handler.cpp.
//
// Why stubs instead of weak-undef-to-NULL: the bridges are declared
// __attribute__((weak)) extern in the LVGL patches, which on glibc/musl
// resolves to NULL when undefined and the runtime null-checks (`if (helix_*)`
// before each call) handle absence safely. However, weak-undef-resolves-to-0
// behavior is fragile under static-PIE LTO across some toolchains. Real
// no-op stubs sidestep the toolchain risk entirely.

#include "helix_lvgl_anomaly.h"

extern "C" void helix_lvgl_anomaly(const char* /*code*/, const char* /*context*/) {
    // splash/watchdog have no telemetry pipeline; drop the report on the floor.
}

// 3XNZQB2R bridges. Splash/watchdog don't run a crash handler, so widget
// identity capture and text-bounds queries are no-ops.
extern "C" void helix_crash_note_event_target_id(const char* /*class_name*/,
                                                 const char* /*obj_name*/) {}

extern "C" int helix_get_text_bounds(unsigned long* /*lo*/, unsigned long* /*hi*/) {
    return 0; // no bounds available — patched LVGL falls back to "no check"
}
