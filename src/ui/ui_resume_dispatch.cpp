// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_resume_dispatch.h"

#include "ams_state.h"
#include "moonraker_api.h"
#include "standard_macros.h"
#include "ui_error_reporting.h"
#include "ui_update_queue.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace helix::ui {

void dispatch_prepared_resume(MoonrakerAPI* api,
                              std::string log_prefix,
                              std::function<void()> on_failure) {
    if (!api) {
        spdlog::warn("{} dispatch_prepared_resume: api is null", log_prefix);
        if (on_failure) on_failure();
        return;
    }

    // The macro-dispatch closure. The success path stays on whichever
    // thread the API delivers it — we only spdlog::info() there (thread
    // safe). The error path bounces through queue_update so the toast,
    // lv_tr() lookup, and `on_failure` body all run on the main thread
    // even though StandardMacros::execute may invoke this callback from
    // the libhv WebSocket event-loop thread on JSON-RPC failure.
    auto dispatch = [api, log_prefix, on_failure]() {
        StandardMacros::instance().execute(
            StandardMacroSlot::Resume, api,
            [log_prefix]() {
                spdlog::info("{} Resume command sent successfully", log_prefix);
            },
            [log_prefix, on_failure](const MoonrakerError& err) {
                spdlog::error("{} Failed to resume: {}", log_prefix, err.message);
                auto user_msg = err.user_message();
                helix::ui::queue_update("dispatch_prepared_resume::on_macro_error",
                                        [user_msg = std::move(user_msg), on_failure]() {
                                            NOTIFY_ERROR(lv_tr("Failed to resume: {}"), user_msg);
                                            if (on_failure) on_failure();
                                        });
            },
            /*timeout_ms=*/0, /*suppress_auto_toast=*/true);
    };

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        dispatch();
        return;
    }

    // prepare_for_resume's on_ready contract guarantees main-thread
    // invocation, so the prep-failure branch doesn't need its own
    // queue_update bounce.
    int slot = backend->get_current_slot();
    backend->prepare_for_resume(
        slot, [dispatch = std::move(dispatch), log_prefix,
               on_failure](const AmsError& err) mutable {
            if (!err.success()) {
                spdlog::error("{} prepare_for_resume failed: {}", log_prefix, err.technical_msg);
                NOTIFY_ERROR(lv_tr("Resume preparation failed: {}"), err.user_msg);
                if (on_failure) on_failure();
                return;
            }
            dispatch();
        });
}

} // namespace helix::ui
