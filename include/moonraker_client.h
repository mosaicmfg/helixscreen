// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_client.h
 * @brief MoonrakerClient - WebSocket Transport Layer
 *
 * ## Responsibilities
 *
 * - WebSocket connection lifecycle (connect, reconnect, disconnect)
 * - JSON-RPC 2.0 protocol handling (delegates to MoonrakerRequestTracker)
 * - Subscription management for status updates (notify_status_update)
 * - Printer discovery orchestration (delegates to MoonrakerDiscoverySequence)
 * - Event system and modal suppression
 *
 * ## NOT Responsible For
 *
 * - Domain-specific operations (use MoonrakerAPI instead)
 * - Input validation (done by MoonrakerAPI)
 * - HTTP file transfers (done by MoonrakerAPI)
 * - High-level printer commands like start_print, home_axes (use MoonrakerAPI)
 *
 * ## Architecture Notes
 *
 * MoonrakerClient composes:
 * - MoonrakerRequestTracker: pending request lifecycle, timeout, response routing
 * - MoonrakerDiscoverySequence: multi-step async printer discovery
 *
 * MoonrakerAPI is the domain layer that builds on top of MoonrakerClient to provide
 * high-level operations like printing, motion control, and file management.
 *
 * @see MoonrakerAPI for domain-specific operations
 * @see MoonrakerRequestTracker for request/response tracking
 * @see MoonrakerDiscoverySequence for printer discovery
 * @see PrinterDiscovery for hardware capabilities
 */

#pragma once

#include "hv/Event.h" // TimerID
#include "hv/WebSocketClient.h"
#include "i_moonraker_client.h"
#include "json_fwd.h"
#include "moonraker_discovery_sequence.h"
#include "moonraker_error.h"
#include "moonraker_events.h"
#include "moonraker_request.h"
#include "moonraker_request_tracker.h"
#include "moonraker_types.h"
#include "printer_detector.h" // For BuildVolume struct
#include "printer_discovery.h"
#include "spdlog/spdlog.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace helix {
/**
 * @brief Unique identifier for notification subscriptions
 *
 * Used to track and remove subscriptions registered via register_notify_update().
 * Valid IDs are always > 0; ID 0 indicates invalid/unsubscribed.
 */
using SubscriptionId = uint64_t;

/** @brief Invalid subscription ID constant */
inline constexpr SubscriptionId INVALID_SUBSCRIPTION_ID = 0;

// RequestId and INVALID_REQUEST_ID are defined in moonraker_request_tracker.h

} // namespace helix

namespace helix {
using ::json; // Make global json alias visible in this namespace

/**
 * @brief Connection state for Moonraker WebSocket
 */
enum class ConnectionState {
    DISCONNECTED, // Not connected
    CONNECTING,   // Connection in progress
    CONNECTED,    // Connected and ready
    RECONNECTING, // Automatic reconnection in progress
    FAILED        // Connection failed (max retries exceeded)
};

/**
 * @brief WebSocket client for Moonraker API communication
 *
 * Implements JSON-RPC 2.0 protocol for Klipper/Moonraker integration.
 * Handles connection lifecycle, automatic reconnection, and message routing.
 * Delegates request tracking to MoonrakerRequestTracker and discovery to
 * MoonrakerDiscoverySequence.
 */
class MoonrakerClient : public hv::WebSocketClient, public IMoonrakerClient {
  public:
    MoonrakerClient(hv::EventLoopPtr loop = nullptr);
    ~MoonrakerClient();

    // Prevent copying (WebSocket client should not be copied)
    MoonrakerClient(const MoonrakerClient&) = delete;
    MoonrakerClient& operator=(const MoonrakerClient&) = delete;

    // GcodeStoreEntry is defined in moonraker_types.h
    // Kept as a type alias for backward compatibility
    using GcodeStoreEntry = ::GcodeStoreEntry;

    // ========== Connection Lifecycle ==========

    /**
     * @brief Connect to Moonraker WebSocket server
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * @param url WebSocket URL (e.g., "ws://127.0.0.1:7125/websocket")
     * @param on_connected Callback invoked when connection opens
     * @param on_disconnected Callback invoked when connection closes
     * @return 0 on success, non-zero on error
     */
    int connect(const char* url, std::function<void()> on_connected,
                std::function<void()> on_disconnected) override;

    /**
     * @brief Disconnect from Moonraker WebSocket server
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * Closes the WebSocket connection and resets internal state.
     * Also clears cached discovery data (hostname, sensors, fans, etc.)
     * to prevent stale data when reconnecting to a different printer.
     * Safe to call multiple times (idempotent).
     */
    void disconnect() override;

    /**
     * @brief Clear all cached discovery data
     *
     * Resets hostname, heaters, sensors, fans, LEDs to empty/default values.
     * Called automatically by disconnect() to prevent stale data when
     * switching between printers.
     */
    void clear_discovery_cache() {
        discovery_.clear_cache();
    }

    /**
     * @brief Force full reconnection with complete state reset
     *
     * Unlike automatic reconnection (which preserves callbacks and pending requests),
     * force_reconnect() performs a complete teardown and rebuild:
     *   1. Disconnects cleanly (if connected)
     *   2. Clears all pending requests (callbacks not invoked)
     *   3. Resets retry counter to 0
     *   4. Reconnects to the same URL
     *   5. Runs discover_printer() to re-subscribe to Moonraker events
     *
     * Use this when:
     *   - User manually requests reconnection (e.g., settings panel "Reconnect" button)
     *   - Recovering from persistent error states
     *   - After printer firmware restart
     *
     * Thread-safe. Can be called from any thread.
     */
    void force_reconnect();

    // ========== Subscription Management ==========

    /**
     * @brief Register callback for status update notifications
     *
     * Invoked when Moonraker sends "notify_status_update" messages
     * (triggered by printer.objects.subscribe subscriptions).
     *
     * @param cb Callback function receiving parsed JSON notification
     * @return Subscription ID for later unsubscription (0 = invalid/failed)
     */
    SubscriptionId register_notify_update(std::function<void(const json&)> cb);

    /**
     * @brief Unsubscribe from status update notifications
     *
     * Removes a previously registered notification callback.
     * Safe to call with invalid IDs (no-op).
     *
     * @param id Subscription ID returned by register_notify_update()
     * @return true if subscription was found and removed, false otherwise
     */
    bool unsubscribe_notify_update(SubscriptionId id);

    /**
     * @brief Register persistent callback for specific notification methods
     *
     * Unlike one-time request callbacks, these persist across multiple messages.
     * Useful for console output, prompt notifications, etc.
     *
     * @param method Notification method name (e.g., "notify_gcode_response")
     * @param handler_name Unique identifier for this handler (for unregistration)
     * @param cb Callback function receiving parsed JSON notification
     */
    void register_method_callback(const std::string& method, const std::string& handler_name,
                                  std::function<void(const json&)> cb);

    /**
     * @brief Unregister a method callback by handler name
     *
     * Removes a previously registered method-specific callback.
     * Safe to call with non-existent method/handler combinations (no-op).
     *
     * @param method Notification method name (e.g., "notify_gcode_response")
     * @param handler_name Handler name used during registration
     * @return true if handler was found and removed, false otherwise
     */
    bool unregister_method_callback(const std::string& method, const std::string& handler_name);

    // ========== JSON-RPC Protocol ==========

    /**
     * @brief Send JSON-RPC request without parameters
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * @param method RPC method name (e.g., "printer.info")
     * @return 0 on success, non-zero on error
     */
    int send_jsonrpc(const std::string& method) override;

    /**
     * @brief Send JSON-RPC request with parameters
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * @param method RPC method name
     * @param params JSON parameters object
     * @return 0 on success, non-zero on error
     */
    int send_jsonrpc(const std::string& method, const json& params) override;

    /**
     * @brief Send JSON-RPC request with one-time response callback
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * Callback is invoked once when response arrives, then removed.
     *
     * @param method RPC method name
     * @param params JSON parameters object
     * @param cb Callback function receiving response JSON
     * @return Request ID for cancellation, or INVALID_REQUEST_ID on error
     */
    RequestId send_jsonrpc(const std::string& method, const json& params,
                           std::function<void(const json&)> cb) override;

    /**
     * @brief Send JSON-RPC request with success and error callbacks
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * @param method RPC method name
     * @param params JSON parameters object
     * @param success_cb Callback for successful response
     * @param error_cb Callback for errors (timeout, JSON-RPC error, etc.)
     * @param timeout_ms Optional timeout override (0 = use default)
     * @param silent If true, don't emit RPC_ERROR events (for internal probes)
     * @return Request ID for cancellation, or INVALID_REQUEST_ID on error
     */
    RequestId send_jsonrpc(const std::string& method, const json& params,
                           std::function<void(const json&)> success_cb,
                           std::function<void(const MoonrakerError&)> error_cb,
                           uint32_t timeout_ms = 0, bool silent = false) override;

    /**
     * @brief Cancel a pending JSON-RPC request
     *
     * Removes the request from the pending queue without invoking callbacks.
     * Safe to call with invalid IDs or already-completed requests (no-op).
     * The actual WebSocket message cannot be recalled once sent; this only
     * prevents callback invocation when the response arrives.
     *
     * @param id Request ID returned by send_jsonrpc()
     * @return true if request was found and cancelled, false otherwise
     */
    bool cancel_request(RequestId id) {
        return tracker_.cancel(id);
    }

    /**
     * @brief Send G-code script command
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * Convenience wrapper for printer.gcode.script method.
     *
     * @param gcode G-code string (e.g., "G28", "M104 S210")
     * @return 0 on success, non-zero on error
     */
    int gcode_script(const std::string& gcode) override;

    /**
     * @brief Fetch G-code command history from Moonraker
     *
     * Retrieves the most recent G-code commands and responses from
     * Moonraker's gcode_store endpoint (server.gcode_store).
     *
     * @param count Maximum number of entries to retrieve
     * @param on_success Callback with vector of GcodeStoreEntry (oldest first)
     * @param on_error Callback for errors
     */
    void
    get_gcode_store(int count, std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
                    std::function<void(const MoonrakerError&)> on_error) override;

    // ========== Discovery (delegates to MoonrakerDiscoverySequence) ==========

    /**
     * @brief Perform printer auto-discovery sequence
     *
     * Calls printer.objects.list → server.info → printer.info → printer.objects.subscribe
     * in sequence, parsing discovered objects and populating PrinterState.
     *
     * Virtual to allow mock override for testing without real printer connection.
     *
     * @param on_complete Callback invoked when discovery completes successfully
     * @param on_error Optional callback invoked if discovery fails (e.g., Klippy not connected)
     */
    void
    discover_printer(std::function<void()> on_complete,
                     std::function<void(const std::string& reason)> on_error = nullptr) override;

    /**
     * @brief Parse object list from printer.objects.list response
     *
     * Categorizes Klipper objects into typed arrays (extruders, heaters, sensors, fans).
     *
     * @param objects JSON array of object names
     */
    void parse_objects(const json& objects) {
        discovery_.parse_objects(objects);
    }

    /**
     * @brief Parse bed mesh data from Moonraker notification
     *
     * Forwards bed_mesh JSON to registered callback (MoonrakerAPI).
     * The API layer owns bed mesh data storage; Client is just the transport.
     *
     * @param bed_mesh JSON object from bed_mesh subscription
     */
    void parse_bed_mesh(const json& bed_mesh) {
        discovery_.parse_bed_mesh(bed_mesh);
    }

    /**
     * @brief Get discovered hardware data
     * @return Reference to PrinterDiscovery containing all discovered hardware
     */
    [[nodiscard]] helix::PrinterDiscovery hardware() const {
        return discovery_.hardware();
    }

    /**
     * @brief Check if client has been identified to Moonraker
     *
     * After a successful server.connection.identify call, this returns true.
     * The flag is reset on disconnect to allow re-identification on reconnect.
     *
     * @return True if already identified to Moonraker on current connection
     */
    bool is_identified() const {
        return discovery_.is_identified();
    }

    /**
     * @brief Reset identification state (for testing)
     *
     * Clears the identified flag. In production, this is done automatically
     * on disconnect. Exposed for unit tests to verify state transitions.
     */
    void reset_identified() {
        discovery_.reset_identified();
    }

    /**
     * @brief Get current connection state
     */
    ConnectionState get_connection_state() const {
        return connection_state_;
    }

    /**
     * @brief Get URL from last connect() call
     *
     * Returns the WebSocket URL used in the most recent connect() call.
     * Empty string if never connected.
     */
    const std::string& get_last_url() const {
        return last_url_;
    }

    /**
     * @brief Get connection generation counter
     *
     * Increments on each connect() call. Can be used to detect stale
     * callbacks from previous connections.
     */
    uint64_t connection_generation() const {
        return connection_generation_.load();
    }

    /**
     * @brief Set callback for connection state changes
     *
     * @param cb Callback invoked when state changes (old_state, new_state)
     */
    void set_state_change_callback(std::function<void(ConnectionState, ConnectionState)> cb) {
        state_change_callback_ = cb;
    }

    /**
     * @brief Set callback for hardware discovery (early phase)
     *
     * Called immediately after printer.objects.list is parsed, BEFORE the main
     * subscription response arrives. This allows hardware-dependent subsystems
     * (like AMS/MMU backends) to be initialized early enough to receive the
     * initial state from the subscription.
     *
     * @param cb Callback invoked with discovered hardware (early)
     */
    void set_on_hardware_discovered(std::function<void(const helix::PrinterDiscovery&)> cb) {
        discovery_.set_on_hardware_discovered(std::move(cb));
    }

    /**
     * @brief Set callback for printer discovery completion
     *
     * Called after discover_printer() successfully completes auto-discovery.
     * Provides the discovered PrinterDiscovery for reactive UI updates.
     *
     * @param cb Callback invoked with discovered hardware
     */
    void set_on_discovery_complete(
        std::function<void(const helix::PrinterDiscovery&, const nlohmann::json& initial_status)>
            cb) {
        discovery_.set_on_discovery_complete(std::move(cb));
    }

    /**
     * @brief Set callback for bed mesh updates
     *
     * Called when bed mesh data is received from Moonraker (via notify_status_update
     * or initial subscription response). The callback receives the raw JSON bed_mesh
     * object for independent parsing by MoonrakerAPI.
     *
     * @param callback Callback receiving raw bed_mesh JSON, or nullptr to disable
     */
    void set_bed_mesh_callback(std::function<void(const json&)> callback) {
        discovery_.set_bed_mesh_callback(std::move(callback));
    }

    // ========== Events ==========

    /**
     * @brief Register callback for transport events
     *
     * Only one handler can be registered at a time. Registering a new
     * handler replaces the previous one.
     *
     * @param cb Callback function, or nullptr to unregister
     */
    void register_event_handler(MoonrakerEventCallback cb);

    /**
     * @brief Temporarily suppress disconnect modal notifications
     *
     * Call this before intentionally triggering a Klipper restart to prevent
     * the "Printer Firmware Disconnected" error modal from appearing.
     * The suppression automatically expires after the specified duration.
     *
     * @param duration_ms How long to suppress disconnect modals (default 10000ms)
     */
    void suppress_disconnect_modal(uint32_t duration_ms = 10000);

    /**
     * @brief Check if disconnect modal is currently suppressed
     *
     * @return true if suppress_disconnect_modal() was called recently
     */
    [[nodiscard]] bool is_disconnect_modal_suppressed() const;

    // ========== Configuration ==========

    /**
     * @brief Set connection timeout in milliseconds
     *
     * @param timeout_ms Connection timeout (default 10000ms)
     */
    void set_connection_timeout(uint32_t timeout_ms) {
        connection_timeout_ms_ = timeout_ms;
    }

    /**
     * @brief Set default request timeout in milliseconds
     *
     * @param timeout_ms Request timeout
     */
    void set_default_request_timeout(uint32_t timeout_ms) {
        tracker_.set_default_timeout(timeout_ms);
    }

    /**
     * @brief Configure timeout and reconnection parameters
     *
     * Sets all timeout and reconnection parameters from config values.
     *
     * @param connection_timeout_ms Connection timeout in milliseconds
     * @param request_timeout_ms Default request timeout in milliseconds
     * @param keepalive_interval_ms WebSocket keepalive ping interval
     * @param reconnect_min_delay_ms Minimum reconnection delay
     * @param reconnect_max_delay_ms Maximum reconnection delay
     */
    void configure_timeouts(uint32_t connection_timeout_ms, uint32_t request_timeout_ms,
                            uint32_t keepalive_interval_ms, uint32_t reconnect_min_delay_ms,
                            uint32_t reconnect_max_delay_ms) {
        connection_timeout_ms_ = connection_timeout_ms;
        tracker_.set_default_timeout(request_timeout_ms);
        keepalive_interval_ms_ = keepalive_interval_ms;
        reconnect_min_delay_ms_ = reconnect_min_delay_ms;
        reconnect_max_delay_ms_ = reconnect_max_delay_ms;
    }

    /**
     * @brief Process timeout checks for pending requests
     *
     * Should be called periodically (e.g., from main loop) to check for timed out requests.
     * Typically called every 1-5 seconds.
     */
    void process_timeouts() {
        tracker_.check_timeouts(
            [this](MoonrakerEventType type, const std::string& msg, bool is_error,
                   const std::string& details) { emit_event(type, msg, is_error, details); });
    }

    // ========== Simulation Methods (for testing) ==========

    /**
     * @brief Toggle filament runout simulation (for testing)
     *
     * No-op in real client. Mock client overrides to simulate filament runout
     * sensor triggering, allowing F-key toggling during development.
     *
     * This abstraction allows Application to call through the base class
     * without needing to know about or cast to MoonrakerClientMock.
     */
    void toggle_filament_runout_simulation() override {
        // No-op in real client - only mock client implements
    }

    /**
     * @brief Get lifetime guard for safe destructor-aware captures
     *
     * Callers capture a weak_ptr from this. When the client is destroyed,
     * the shared_ptr is reset first, so weak_ptr::lock() returns nullptr.
     * Used by SubscriptionGuard to avoid calling into a destroyed client.
     *
     * This is distinct from the internal WebSocket-callback guard (which is
     * also reset on disconnect to cancel in-flight libhv callbacks). Clients
     * of SubscriptionGuard only care about *object* destruction, not
     * disconnect/reconnect cycles.
     */
    std::weak_ptr<bool> lifetime_weak() const {
        return destruction_guard_;
    }

    // ========== Methods used by composed classes ==========

    /**
     * @brief Emit event to registered handler
     *
     * Thread-safe. If no handler is registered, the event is logged and dropped.
     * Public to allow MoonrakerDiscoverySequence and MoonrakerRequestTracker access.
     *
     * @param type Event type
     * @param message Human-readable message
     * @param is_error true for error events, false for warnings
     * @param details Additional details (optional)
     */
    void emit_event(MoonrakerEventType type, const std::string& message, bool is_error = false,
                    const std::string& details = "");

    /**
     * @brief Dispatch printer status to all registered notify callbacks
     *
     * Wraps raw status data (e.g., from subscription response) into a
     * notify_status_update notification format and dispatches to callbacks.
     * Used for both initial subscription state and incremental updates.
     * Public to allow MoonrakerDiscoverySequence access.
     *
     * @param status Raw printer status object
     */
    void dispatch_status_update(const json& status);

    /**
     * @brief Invoke an on_connected-style callback with exception safety.
     *
     * Shared by the onopen path and the notify_klippy_ready / notify_klippy_shutdown
     * retry paths so they all produce consistent logging and can't propagate
     * exceptions back into libhv's event loop.
     *
     * @param cb The callback to invoke; no-op if empty.
     * @param cause Short human-readable label for log messages.
     */
    static void invoke_connected_callback(const std::function<void()>& cb, const char* cause);

  protected:
    /**
     * @brief Transition to new connection state
     *
     * @param new_state The new state to transition to
     */
    void set_connection_state(ConnectionState new_state);

    // Notification callbacks (protected to allow mock to trigger notifications)
    // Map of subscription ID -> callback for O(1) unsubscription
    std::map<SubscriptionId, std::function<void(const json&)>> notify_callbacks_;
    std::atomic<SubscriptionId> next_subscription_id_{1}; // Start at 1 (0 = invalid)
    std::mutex callbacks_mutex_; // Protect notify_callbacks_ and method_callbacks_

    // Persistent method-specific callbacks (protected to allow mock to dispatch)
    // method_name : { handler_name : callback }
    std::map<std::string, std::map<std::string, std::function<void(const json&)>>>
        method_callbacks_;

    // Discovery sequence (protected to allow mock access to hardware vectors)
    MoonrakerDiscoverySequence discovery_;

    /**
     * @brief Gate for outbound sends — true only when connection_state_ ==
     *        CONNECTED.
     *
     * libhv's WebSocketClient::send only checks `channel != NULL`, NOT the WS
     * protocol state. Sends issued during CONNECTING / WS_UPGRADING /
     * RECONNECTING write WS frame bytes onto a stream that's not in WS-frame
     * phase — Moonraker silently drops the malformed bytes and the request
     * stalls indefinitely (#909).
     *
     * Virtual so MoonrakerClientMock can return true unconditionally without
     * having to drive connection_state_ through tests.
     *
     * @param method RPC method name (used for diagnostic logging only)
     * @return true if it is safe to issue a send right now
     */
    virtual bool ready_to_send(const char* method) const;

  private:
    // Request tracker (pending requests, timeouts, response routing)
    MoonrakerRequestTracker tracker_;

    // Connection state tracking
    std::atomic_bool was_connected_;
    std::atomic<ConnectionState> connection_state_;
    std::atomic_bool is_destroying_{false}; // Prevent callbacks during destruction
    std::atomic<uint64_t> connection_generation_{
        0}; // Increments on each connect(), used to invalidate stale discovery callbacks
    std::function<void(ConnectionState, ConnectionState)> state_change_callback_;
    mutable std::mutex state_callback_mutex_; // Protect state_change_callback_ during destruction
    uint32_t connection_timeout_ms_;
    uint32_t reconnect_attempts_ = 0;
    uint32_t max_reconnect_attempts_ = 0; // 0 = infinite

    // Connection parameters (from config)
    uint32_t keepalive_interval_ms_;
    uint32_t reconnect_min_delay_ms_;
    uint32_t reconnect_max_delay_ms_;

    // Stored connection info for force_reconnect()
    std::string last_url_;                          // URL used in last connect()
    std::function<void()> last_on_connected_;       // Callback from last connect()
    std::function<void()> last_on_disconnected_;    // Callback from last connect()
    std::function<void()> last_discovery_complete_; // Callback from last discover_printer()
    mutable std::mutex reconnect_mutex_;            // Protect stored connection info

    // Event handler for transport events (decouples from UI layer)
    MoonrakerEventCallback event_handler_;
    mutable std::mutex event_handler_mutex_;

    // Serialize connect()/disconnect() calls — close/callback-setup/open is not
    // atomic and concurrent callers race on libhv internal state and callback
    // assignment.  Recursive because force_reconnect() calls both.
    mutable std::recursive_mutex connect_mutex_;

    // Disconnect modal suppression (for intentional restarts)
    std::chrono::steady_clock::time_point suppress_disconnect_modal_until_{};
    mutable std::mutex suppress_mutex_;

    // Callback synchronization mutex
    // Callbacks take a shared (read) lock; the destructor takes an exclusive (write) lock.
    // This ensures all in-flight callbacks complete before destruction proceeds.
    mutable std::shared_mutex callback_lifecycle_mutex_;

    // Periodic health-check timer (runs on libhv event loop thread)
    // Checks request timeouts and reconnection staleness independently of message flow
    hv::TimerID health_timer_id_{0};
    static constexpr int HEALTH_CHECK_INTERVAL_MS = 5000;

    void start_health_timer();
    void stop_health_timer();

    // Reconnection staleness detection.
    // Written/read only from the libhv event loop thread (onclose + health timer).
    std::chrono::steady_clock::time_point reconnect_started_at_{};
    static constexpr int MAX_RECONNECT_STALL_MS = 60000;

    // WebSocket callback cancellation guard. Reset on BOTH disconnect and
    // destruction so in-flight libhv callbacks (onopen/onmessage/onclose)
    // early-return on either event. Internal use only.
    std::shared_ptr<bool> lifetime_guard_ = std::make_shared<bool>(true);

    // Object-destruction guard. Reset ONLY in the destructor. Exposed via
    // lifetime_weak() for SubscriptionGuard and other holders that need to
    // detect that the MoonrakerClient instance itself is gone — not that it
    // merely dropped a WebSocket connection.
    std::shared_ptr<bool> destruction_guard_ = std::make_shared<bool>(true);
};

} // namespace helix
