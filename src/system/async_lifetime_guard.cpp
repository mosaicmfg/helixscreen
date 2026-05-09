// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file async_lifetime_guard.cpp
 * @brief Bg-thread detector helpers for LifetimeToken
 *
 * The token type itself is header-only (templated defer methods), so this TU
 * only owns the bg-thread anti-pattern detector — main-thread id capture +
 * per-callsite first-fire reporting. See header for the rule the detector
 * targets and `project_l081_recurrence_post_840.md` for cluster context.
 */

#include "async_lifetime_guard.h"

#include "helix_lvgl_anomaly.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>

namespace helix::internal {

namespace {

/// Recorded main thread id. Populated by set_main_thread_id() before any
/// bg threads can spawn. on_main_thread() returns true conservatively
/// while this is unset so the detector doesn't false-positive during
/// the brief early-init window.
std::atomic<bool> g_main_thread_set{false};
pthread_t g_main_thread_id;

/// Per-thread first-fire seen-set: TLS array of LRs already reported by
/// THIS thread. 64 slots × sizeof(void*) = 512 bytes per thread, linear
/// probe with cheap `>>4` hash. Each unique (thread, LR) pair fires at
/// most one anomaly per session. Multiple bg threads hitting the same LR
/// each get one report — interesting because each is a separate race vs.
/// the main thread. Table-full is silently dropped (no spam fallback).
constexpr size_t kSeenSlots = 64;
thread_local void* tls_seen_lrs[kSeenSlots] = {};

/// Returns true if this is the first time `lr` has been reported on the
/// calling thread; false otherwise (already seen or table full).
bool record_first_fire(void* lr) noexcept {
    if (lr == nullptr) return false;
    const auto h = static_cast<size_t>(reinterpret_cast<uintptr_t>(lr) >> 4) & (kSeenSlots - 1);
    for (size_t step = 0; step < kSeenSlots; ++step) {
        const size_t slot = (h + step) & (kSeenSlots - 1);
        if (tls_seen_lrs[slot] == lr) return false;
        if (tls_seen_lrs[slot] == nullptr) {
            tls_seen_lrs[slot] = lr;
            return true;
        }
    }
    return false;
}

} // namespace

void set_main_thread_id() noexcept {
    if (g_main_thread_set.load(std::memory_order_acquire)) return;
    g_main_thread_id = pthread_self();
    g_main_thread_set.store(true, std::memory_order_release);
}

bool on_main_thread() noexcept {
    if (!g_main_thread_set.load(std::memory_order_acquire)) {
        // Init not yet run — conservative return so the detector doesn't
        // false-positive on the genuine main thread before we recorded it.
        return true;
    }
    return pthread_equal(pthread_self(), g_main_thread_id) != 0;
}

[[gnu::noinline]] void report_bg_expired_check() noexcept {
    // expired() is [[gnu::always_inline]], so when our caller F has an
    // inlined `tok.expired()`, this trampoline's __builtin_return_address(0)
    // resolves to the LR *inside F* at the point of the inlined call — the
    // user's source-level callsite. Capturing the LR up in the inline path
    // would have given F's caller's LR after inlining (wrong frame).
    void* lr = __builtin_return_address(0);
    if (!record_first_fire(lr)) return;

    // pthread_t is `unsigned long` on glibc/musl Linux but a pointer on
    // macOS — reinterpret_cast<uintptr_t> fails on 32-bit ARM where
    // sizeof(unsigned long) == sizeof(uintptr_t) but the integer ranks
    // differ. memcpy-into-uint64_t sidesteps both the cast restriction
    // and the platform-dependent representation.
    uint64_t tid_word = 0;
    pthread_t self = pthread_self();
    std::memcpy(&tid_word, &self,
                sizeof(self) < sizeof(tid_word) ? sizeof(self) : sizeof(tid_word));
    char ctx[96];
    std::snprintf(ctx, sizeof(ctx),
                  "lr=%p tid=0x%llx (likely L081 Mechanism C: tok.expired() on bg thread)",
                  lr, static_cast<unsigned long long>(tid_word));

    // Use the existing display-anomaly channel so the telemetry pipeline
    // captures backtrace + rate-limits across other LVGL anomalies.
    helix_lvgl_anomaly("bg_tok_expired_check", ctx);

    spdlog::warn("[LifetimeToken] bg-thread expired() check while alive at lr={} — "
                 "likely missing tok.defer() wrap (cluster:pstat-async-delete Mechanism C)",
                 lr);
}

} // namespace helix::internal
