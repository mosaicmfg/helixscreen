// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_async_lifetime_guard.cpp
 * @brief Unit tests for AsyncLifetimeGuard — generation-counter-based async callback safety
 */

#include "../lvgl_test_fixture.h"
#include "async_lifetime_guard.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Pure token tests (no LVGL needed)
// ============================================================================

TEST_CASE("Token valid when no invalidation", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto tok = guard.token();

    REQUIRE_FALSE(tok.expired());
    REQUIRE(static_cast<bool>(tok));
}

TEST_CASE("Token expired after invalidate", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto tok = guard.token();

    guard.invalidate();

    REQUIRE(tok.expired());
    REQUIRE_FALSE(static_cast<bool>(tok));
}

TEST_CASE("Multiple tokens all expire", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto t1 = guard.token();
    auto t2 = guard.token();
    auto t3 = guard.token();

    REQUIRE_FALSE(t1.expired());
    REQUIRE_FALSE(t2.expired());
    REQUIRE_FALSE(t3.expired());

    guard.invalidate();

    REQUIRE(t1.expired());
    REQUIRE(t2.expired());
    REQUIRE(t3.expired());
}

TEST_CASE("Generation cycling — old token expired, new token valid", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto old_tok = guard.token();

    guard.invalidate();

    auto new_tok = guard.token();

    REQUIRE(old_tok.expired());
    REQUIRE_FALSE(new_tok.expired());
}

TEST_CASE("Double invalidate still works correctly", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto t1 = guard.token();

    guard.invalidate();
    REQUIRE(t1.expired());

    // Second invalidate should not break anything
    guard.invalidate();
    REQUIRE(t1.expired());

    // New token after double-invalidate should be valid
    auto t2 = guard.token();
    REQUIRE_FALSE(t2.expired());
}

// ============================================================================
// Defer tests (need LVGL for UpdateQueue)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Defer runs when valid", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    bool ran = false;

    guard.defer([&ran]() { ran = true; });

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Defer skips when invalidated", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    bool ran = false;

    guard.defer([&ran]() { ran = true; });
    guard.invalidate();

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Defer with tag skips when invalidated", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    bool ran = false;

    guard.defer("test::tagged_callback", [&ran]() { ran = true; });
    guard.invalidate();

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Defer safe after guard destroyed", "[lifetime_guard]") {
    bool ran = false;

    {
        AsyncLifetimeGuard guard;
        guard.defer([&ran]() { ran = true; });
        // guard destroyed here — invalidate() called in destructor
    }

    // Draining should not crash, and callback should NOT run
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Defer after invalidate uses new generation",
                 "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    bool first_ran = false;
    bool second_ran = false;

    guard.defer([&first_ran]() { first_ran = true; });
    guard.invalidate();
    guard.defer([&second_ran]() { second_ran = true; });

    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(first_ran);
    REQUIRE(second_ran);
}

// ============================================================================
// LifetimeToken::defer() tests (need LVGL for UpdateQueue)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Token defer runs when valid", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    bool ran = false;

    tok.defer([&ran]() { ran = true; });

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Token defer skips when guard invalidated", "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    bool ran = false;

    guard.invalidate();
    tok.defer([&ran]() { ran = true; });

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Token defer skips when guard destroyed", "[lifetime_guard]") {
    bool ran = false;

    // Use optional to control guard lifetime independently of token
    auto guard_ptr = std::make_unique<AsyncLifetimeGuard>();
    auto tok = guard_ptr->token();

    // Destroy guard — invalidate() in destructor expires tok
    guard_ptr.reset();

    // Token outlives guard — defer must not crash and must skip callback
    tok.defer([&ran]() { ran = true; });

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Token defer with tag skips when invalidated",
                 "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    bool ran = false;

    guard.invalidate();
    tok.defer("test::tagged_token_callback", [&ran]() { ran = true; });

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Token defer after invalidate — new token works",
                 "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto old_tok = guard.token();
    bool old_ran = false;
    bool new_ran = false;

    guard.invalidate();
    auto new_tok = guard.token();

    old_tok.defer([&old_ran]() { old_ran = true; });
    new_tok.defer([&new_ran]() { new_ran = true; });

    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(old_ran);
    REQUIRE(new_ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "Token defer queued before invalidate is skipped",
                 "[lifetime_guard]") {
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    bool ran = false;

    // Queue via token, then invalidate before drain
    tok.defer([&ran]() { ran = true; });
    guard.invalidate();

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ran);
}

// ============================================================================
// Bg-thread expired() detector (3XNZQB2R / cluster:pstat-async-delete)
// ============================================================================
//
// These tests verify expired()'s correctness invariant under bg-thread use.
// The *anomaly reporting* side-effect (helix_lvgl_anomaly call + spdlog
// warn) is exercised here too — the binary links the real telemetry
// implementation, so a misfire would surface as a [Warn] in the test log.
//
// What these don't test: that telemetry actually emitted the bundle field
// the field-resolver reads. That's verified manually after the AD5M deploy
// smoke + via the next field bundle in this signature.

TEST_CASE("expired() correct on main thread (alive)", "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    REQUIRE_FALSE(tok.expired());
}

TEST_CASE("expired() correct on main thread (after invalidate)",
         "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    guard.invalidate();
    REQUIRE(tok.expired());
}

TEST_CASE("expired() correct on bg thread (alive)", "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    AsyncLifetimeGuard guard;
    auto tok = guard.token();

    // Bg thread + alive token: should return false (the dangerous case the
    // detector flags via anomaly, but the boolean must still be correct).
    std::atomic<bool> bg_saw_alive{false};
    std::thread bg([&]() {
        bg_saw_alive.store(!tok.expired());
    });
    bg.join();
    REQUIRE(bg_saw_alive.load());
}

TEST_CASE("expired() correct on bg thread (after invalidate)",
         "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    AsyncLifetimeGuard guard;
    auto tok = guard.token();
    guard.invalidate();

    std::atomic<bool> bg_saw_expired{false};
    std::thread bg([&]() {
        bg_saw_expired.store(tok.expired());
    });
    bg.join();
    REQUIRE(bg_saw_expired.load());
}

TEST_CASE("expired() under tight bg-thread loop doesn't crash",
         "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    AsyncLifetimeGuard guard;
    auto tok = guard.token();

    // First-fire-only seen-set must tolerate the same callsite firing many
    // times without spamming, crashing, or breaking the boolean result.
    std::thread bg([&]() {
        for (int i = 0; i < 1000; ++i) {
            REQUIRE_FALSE(tok.expired());
        }
    });
    bg.join();
}

TEST_CASE("on_main_thread() reflects current thread", "[lifetime_guard][bg_detector]") {
    helix::internal::set_main_thread_id();
    REQUIRE(helix::internal::on_main_thread());

    std::atomic<bool> bg_saw_main{true};
    std::thread bg([&]() {
        bg_saw_main.store(helix::internal::on_main_thread());
    });
    bg.join();
    REQUIRE_FALSE(bg_saw_main.load());
}

TEST_CASE("Thread safety — concurrent token and invalidate", "[lifetime_guard][slow]") {
    AsyncLifetimeGuard guard;
    std::atomic<bool> stop{false};
    std::atomic<int> token_count{0};
    std::atomic<int> invalidate_count{0};

    // Thread 1: repeatedly create tokens and check them
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            auto tok = guard.token();
            // Just exercise expired() — result may vary due to races, but must not crash
            (void)tok.expired();
            token_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Thread 2: repeatedly invalidate
    std::thread writer([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            guard.invalidate();
            invalidate_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Let them race for a short time
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_relaxed);

    reader.join();
    writer.join();

    // If we got here without crashing, the test passed
    REQUIRE(token_count.load() > 0);
    REQUIRE(invalidate_count.load() > 0);
}
