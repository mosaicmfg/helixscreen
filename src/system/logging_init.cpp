// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "logging_init.h"

#ifndef HELIX_WATCHDOG
#include "hv/hlog.h"
#endif
#include "lvgl_assert_handler.h"
#include "lvgl_log_handler.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdlib>
#include <filesystem>
#include <lvgl.h>
#include <unistd.h>
#include <vector>

// Define the global callback pointer for LVGL assert handler
helix_assert_callback_t g_helix_assert_cpp_callback = nullptr;

#ifdef __linux__
#ifdef HELIX_HAS_SYSTEMD
#include <spdlog/sinks/systemd_sink.h>
#endif
#include <spdlog/sinks/syslog_sink.h>
#endif

#ifdef HELIX_PLATFORM_ANDROID
#include <spdlog/sinks/android_sink.h>
#endif

namespace helix {
namespace logging {

namespace {

/// Check if a path is writable (for file logging location selection)
bool is_path_writable(const std::string& path) {
    // Check parent directory for new files, or file itself if exists
    std::filesystem::path p(path);
    std::filesystem::path dir = p.parent_path();

    if (dir.empty()) {
        dir = ".";
    }

    // Check if directory exists and is writable
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return false;
    }

    // Try to determine write permission
    auto perms = std::filesystem::status(dir, ec).permissions();
    if (ec) {
        return false;
    }

    // Check owner write permission (simplified check)
    return (perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none;
}

/// Get XDG_DATA_HOME or default ~/.local/share
std::string get_xdg_data_home() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] != '\0') {
        return xdg;
    }

    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.local/share";
    }

    return "/tmp"; // Last resort fallback
}

/// Resolve log file path with fallback logic
std::string resolve_log_file_path(const std::string& override_path) {
    if (!override_path.empty()) {
        return override_path;
    }

    // Try /var/log first (requires permissions, typical for system services)
    const std::string var_log = "/var/log/helix-screen.log";
    if (is_path_writable(var_log)) {
        return var_log;
    }

    // Fallback to user directory
    std::string user_dir = get_xdg_data_home() + "/helix-screen";
    std::error_code ec;
    std::filesystem::create_directories(user_dir, ec);

    return user_dir + "/helix.log";
}

/// Detect best available logging target at runtime
LogTarget detect_best_target() {
#ifdef HELIX_PLATFORM_ANDROID
    // Android: use logcat sink (stdout is invisible in adb logcat)
    return LogTarget::Android;
#elif defined(__linux__)
#ifdef HELIX_HAS_SYSTEMD
    // Check for systemd journal socket
    std::error_code ec;
    if (std::filesystem::exists("/run/systemd/journal/socket", ec)) {
        return LogTarget::Journal;
    }
#endif
    // Syslog is always available on Linux
    return LogTarget::Syslog;
#else
    // macOS/other: console only by default
    return LogTarget::Console;
#endif
}

/// Add system sink based on target
void add_system_sink(std::vector<spdlog::sink_ptr>& sinks, LogTarget target,
                     const std::string& file_path) {
    switch (target) {
#ifdef __linux__
#ifdef HELIX_HAS_SYSTEMD
    case LogTarget::Journal:
        sinks.push_back(std::make_shared<spdlog::sinks::systemd_sink_mt>("helix-screen"));
        break;
#endif
    case LogTarget::Syslog:
        sinks.push_back(std::make_shared<spdlog::sinks::syslog_sink_mt>("helix-screen", LOG_PID,
                                                                        LOG_USER, false));
        break;
#endif
    case LogTarget::File: {
        std::string path = resolve_log_file_path(file_path);
        // 5MB max size, 3 rotated files
        sinks.push_back(
            std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path, 5 * 1024 * 1024, 3));
        break;
    }
#ifdef HELIX_PLATFORM_ANDROID
    case LogTarget::Android:
        sinks.push_back(std::make_shared<spdlog::sinks::android_sink_mt>("HelixScreen"));
        break;
#endif
    case LogTarget::Console:
    case LogTarget::Auto:
        // Console-only or auto (which would have been resolved already)
        // No additional sink needed
        break;
#ifdef __linux__
    default:
        // Handle Journal case when HELIX_HAS_SYSTEMD is not defined
        // Fall back to syslog
        if (target == LogTarget::Journal) {
            sinks.push_back(std::make_shared<spdlog::sinks::syslog_sink_mt>("helix-screen", LOG_PID,
                                                                            LOG_USER, false));
        }
        break;
#else
    default:
        break;
#endif
    }
}

/// C++ assert callback that logs via spdlog and dumps backtrace.
/// IMPORTANT: Do NOT call any LVGL functions here — this callback may fire
/// during rendering or layout, and re-entrant LVGL calls cause cascading
/// assertions and SIGSEGV (crash signature 0997d072).
void lvgl_assert_spdlog_callback(const char* file, int line, const char* func) {
    // Log via spdlog for consistent logging across all outputs
    spdlog::critical("╔═══════════════════════════════════════════════════════════╗");
    spdlog::critical("║              LVGL ASSERTION FAILED                        ║");
    spdlog::critical("╠═══════════════════════════════════════════════════════════╣");
    spdlog::critical("║ File: {}", file);
    spdlog::critical("║ Line: {}", line);
    spdlog::critical("║ Func: {}()", func);
    spdlog::critical("╚═══════════════════════════════════════════════════════════╝");

    // Dump recent log messages that led up to this assertion
    spdlog::critical("=== Recent log messages (backtrace) ===");
    spdlog::dump_backtrace();
}

} // namespace

void init_early() {
    // Create minimal console-only logger at WARN level
    // This allows early startup code to log without crashing
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("helix", console);
    logger->set_level(spdlog::level::warn);
    spdlog::set_default_logger(logger);
}

void init(const LogConfig& config) {
    std::vector<spdlog::sink_ptr> sinks;

    // Resolve auto-detection first so we can decide about console
    LogTarget effective_target =
        (config.target == LogTarget::Auto) ? detect_best_target() : config.target;

    // Console sink — added when enabled AND the target benefits from it.
    //
    // Behavior by target:
    //   - Console: console is the ONLY sink, always add.
    //   - Android: stdout is invisible; android_sink handles output. Skip.
    //   - Syslog/Journal/File: structured destination already captures output.
    //     Add console ONLY when stdout is a TTY (interactive run from a shell),
    //     so dev workstations and `ssh -t` sessions still see colored output.
    //     Daemonized launches under SysV/systemd have stdout redirected to a
    //     file or the journal — adding a stdout sink there double-logs every
    //     line (was the root cause of the Snapmaker U1 tmpfs blowout where
    //     /tmp/helixscreen.log grew to 498 MB at trace level).
    bool add_console = false;
    if (config.enable_console) {
        switch (effective_target) {
        case LogTarget::Console:
            add_console = true;
            break;
        case LogTarget::Android:
            add_console = false;
            break;
        default:
            add_console = (isatty(STDOUT_FILENO) != 0);
            break;
        }
    }
    if (add_console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    // Add system sink
    add_system_sink(sinks, effective_target, config.file_path);

    // Create logger with all sinks
    auto logger = std::make_shared<spdlog::logger>("helix", sinks.begin(), sinks.end());
    logger->set_level(config.level);

    // Set as default logger
    spdlog::set_default_logger(logger);

    // Enable backtrace buffer to capture recent log messages before an assertion
    // These get dumped when spdlog::dump_backtrace() is called in the assert handler
    spdlog::enable_backtrace(32);

    // Register C++ callback for LVGL assert handler
    // This provides spdlog integration and LVGL state context
    g_helix_assert_cpp_callback = lvgl_assert_spdlog_callback;

    // NOTE: LVGL log handler is registered separately AFTER lv_init()
    // because lv_init() resets the global state and clears any callbacks.
    // See Application::init_display() which calls register_lvgl_log_handler().

    // Log what we configured (at debug level so it's not noisy)
    spdlog::debug("[Logging] Initialized: target={}, console={}, backtrace=32 messages",
                  log_target_name(effective_target), config.enable_console ? "yes" : "no");
}

LogTarget parse_log_target(const std::string& str) {
    if (str == "journal")
        return LogTarget::Journal;
    if (str == "syslog")
        return LogTarget::Syslog;
    if (str == "file")
        return LogTarget::File;
    if (str == "console")
        return LogTarget::Console;
    if (str == "android")
        return LogTarget::Android;
    return LogTarget::Auto; // Default for "auto" or unrecognized
}

const char* log_target_name(LogTarget target) {
    switch (target) {
    case LogTarget::Auto:
        return "auto";
    case LogTarget::Journal:
        return "journal";
    case LogTarget::Syslog:
        return "syslog";
    case LogTarget::File:
        return "file";
    case LogTarget::Console:
        return "console";
    case LogTarget::Android:
        return "android";
    }
    return "unknown";
}

spdlog::level::level_enum parse_level(const std::string& str,
                                      spdlog::level::level_enum default_level) {
    if (str.empty()) {
        return default_level;
    }
    if (str == "trace") {
        return spdlog::level::trace;
    }
    if (str == "debug") {
        return spdlog::level::debug;
    }
    if (str == "info") {
        return spdlog::level::info;
    }
    if (str == "warn" || str == "warning") {
        return spdlog::level::warn;
    }
    if (str == "error") {
        return spdlog::level::err;
    }
    if (str == "critical") {
        return spdlog::level::critical;
    }
    if (str == "off") {
        return spdlog::level::off;
    }
    return default_level;
}

spdlog::level::level_enum verbosity_to_level(int verbosity) {
    if (verbosity <= 0) {
        return spdlog::level::warn;
    }
    switch (verbosity) {
    case 1:
        return spdlog::level::info;
    case 2:
        return spdlog::level::debug;
    default:
        return spdlog::level::trace;
    }
}

int to_hv_level(spdlog::level::level_enum level) {
    // libhv levels: VERBOSE(0) < DEBUG(1) < INFO(2) < WARN(3) < ERROR(4) < FATAL(5) < SILENT(6)
    switch (level) {
    case spdlog::level::trace:
    case spdlog::level::debug:
        return 1; // LOG_LEVEL_DEBUG (libhv has no trace, cap at debug)
    case spdlog::level::info:
        return 2; // LOG_LEVEL_INFO
    case spdlog::level::warn:
        return 3; // LOG_LEVEL_WARN
    case spdlog::level::err:
        return 4; // LOG_LEVEL_ERROR
    case spdlog::level::critical:
        return 5; // LOG_LEVEL_FATAL
    case spdlog::level::off:
        return 6; // LOG_LEVEL_SILENT
    default:
        return 3; // LOG_LEVEL_WARN
    }
}

#ifndef HELIX_WATCHDOG
void set_runtime_level(spdlog::level::level_enum level) {
    spdlog::set_level(level);
    hlog_set_level(to_hv_level(level));
    spdlog::info("[Logging] Runtime log level changed to {}",
                 spdlog::level::to_string_view(level).data());
}
#endif

spdlog::level::level_enum resolve_log_level(int cli_verbosity, const std::string& config_level_str,
                                            bool test_mode) {
    // Precedence: CLI verbosity > config file > defaults

    // CLI verbosity takes top precedence
    if (cli_verbosity > 0) {
        return verbosity_to_level(cli_verbosity);
    }

    // Config file level (if specified)
    if (!config_level_str.empty()) {
        // Use warn as fallback for invalid config strings
        return parse_level(config_level_str, spdlog::level::warn);
    }

    // Defaults: test mode = debug, production = warn
    return test_mode ? spdlog::level::debug : spdlog::level::warn;
}

} // namespace logging
} // namespace helix
