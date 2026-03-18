//
// log.h - lightweight structured logger (C++20, no external deps)
//
// Provides four log levels, timestamps, and colored terminal output.
// Color is enabled automatically on Windows 10+ and any ANSI-capable terminal.
//
// ── Quick reference ----------------------------------------------------------------------------─
//
//   log_debug("Punch jitter: {}ms", jitt);          // verbose protocol detail
//   log_info ("Connected to {} at {}:{}", id, ip, port); // meaningful state changes
//   log_warn ("Unexpected source: {}", src);         // something looks wrong but recoverable
//   log_error("Socket bind failed: {}", ec.message()); // something broke
//
//   // Silence noisy levels at runtime (e.g. in production):
//   Logger::get().set_level(LogLevel::WARN);
//
// ── Output format --------------------------------------------------------------------------------
//
//   [12:34:56.789] [ INFO] Connected to abc123 at 1.2.3.4:5678
//   [12:34:56.801] [ WARN] Unexpected source: xyz
//   [12:34:56.950] [ERROR] Socket bind failed: address already in use
//
// ── Format strings ----------------------------------------------------------------------------──
//
//   Uses std::format (C++20) syntax - {} for each argument, in order.
//   Format errors (wrong number of {}) are caught at runtime via std::format_error.
//
//   For types that don't have a built-in formatter (e.g. asio addresses,
//   nlohmann::json bodies), call .to_string() / .dump() first:
//
//     log_info("Peer at {}", ep.address().to_string());
//     log_debug("Body: {}", env.body.dump());
//
#pragma once

#include <chrono>
#include <ctime>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

// ---- Log levels --------------------------------------------------------------------------------──
//
// In order from least to most severe.
// set_level(X) suppresses every level below X.
//
//   DEBUG  – noisy per-packet / per-timer detail (punch retries, jitter, SEND/RECV)
//   INFO   – normal lifecycle events (connections, registrations, circuit ready)
//   WARN   – unexpected but non-fatal conditions (unknown sender, bad token, etc.)
//   ERR    – hard failures (socket errors, parse failures, exceptions)
//
enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERR = 3 };

// ---- Logger (singleton) ------------------------------------------------------------------------
//
// A single instance is created on first use and lives for the program's lifetime.
// All log calls are thread-safe - a mutex guards the output stream so lines from
// the network thread and the CLI thread don't interleave.
//
class Logger {
public:
    // Returns the one global logger instance.
    static Logger& get() noexcept {
        static Logger inst;
        return inst;
    }

    // Change the minimum level that produces output.
    // Everything strictly below this level is silently dropped.
    void set_level(LogLevel lvl) noexcept { min_level_ = lvl; }
    LogLevel level() const noexcept { return min_level_; }

    // Core log function.
    // fmt is a std::format-style format string checked at compile time.
    // Example: logger.log(LogLevel::INFO, "Peer {} joined", peer_id);
    template <typename... Args>
    void log(LogLevel lvl, std::string_view fmt, Args&&... args) {
        if (lvl < min_level_) return;  // fast path: skip formatting entirely
        log_impl(lvl, std::vformat(fmt, std::make_format_args(args...)));
    }

private:
    LogLevel min_level_ = LogLevel::INFO;  // default: hide DEBUG noise
    std::mutex mtx_;                        // guards std::cerr
    bool colors_ = false;                   // whether the terminal supports ANSI codes

    // Constructor - tries to enable ANSI color codes.
    // On Windows, the console needs an explicit mode flag set before ANSI escape
    // sequences are interpreted. On Linux/macOS they work out of the box.
    Logger() {
#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
            colors_ = SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
        colors_ = true;
#endif
    }

    // Fixed-width label shown in every log line, e.g. " INFO" or "ERROR".
    static const char* level_str(LogLevel lvl) noexcept {
        switch (lvl) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERR:   return "ERROR";
            default:              return "?????";
        }
    }

    // ANSI escape code that sets the foreground color for a given level.
    // The reset code "\033[0m" is written after the label to restore normal color
    // so only the timestamp+level prefix is colored, keeping the message readable.
    static const char* color_code(LogLevel lvl) noexcept {
        switch (lvl) {
            case LogLevel::DEBUG: return "\033[38;5;208m";  // orange
            case LogLevel::INFO:  return "\033[36m";          // cyan
            case LogLevel::WARN:  return "\033[38;5;135m";   // violet
            case LogLevel::ERR:   return "\033[31m";          // red
            default:              return "";
        }
    }

    // Produces a "HH:MM:SS.mmm" string from the current wall clock.
    // The millisecond component makes it easy to see timing between events.
    static std::string timestamp() {
        const auto now = std::chrono::system_clock::now();
        const auto tt  = std::chrono::system_clock::to_time_t(now);
        // Extract sub-second milliseconds separately since strftime has no ms specifier.
        const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now.time_since_epoch()).count() % 1000;
        struct tm tm_info{};
#ifdef _WIN32
        localtime_s(&tm_info, &tt);   // thread-safe version on MSVC
#else
        localtime_r(&tt, &tm_info);   // thread-safe version on POSIX
#endif
        char buf[16];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_info);
        return std::format("{}.{:03d}", buf, ms);  // zero-pad milliseconds to 3 digits
    }

    // Writes the fully formatted line to stderr under the mutex.
    // We write to stderr (not stdout) so that log output and interactive CLI
    // prompts don't compete for the same stream.
    void log_impl(LogLevel lvl, const std::string& msg) {
        const std::string ts = timestamp();
        std::lock_guard lock(mtx_);
        if (colors_) {
            // Prefix is uncolored; color wraps the message text only.
            std::cerr << "\033[0m [" << ts << "] [" << level_str(lvl) << "] "
                      << color_code(lvl)
                      << msg
                      << "\033[0m" << '\n';
        } else {
            std::cerr << '[' << ts << "] [" << level_str(lvl) << "] " << msg << '\n';
        }
    }
};

// ---- Convenience free functions ------------------------------------------------------------──
//
// These forward directly to the singleton, so call sites look clean:
//
//   log_info("Circuit {} ready", circuit_id);
//
// rather than:
//
//   Logger::get().log(LogLevel::INFO, "Circuit {} ready", circuit_id);
//
template <typename... Args>
inline void log_debug(std::string_view fmt, Args&&... args) {
    Logger::get().log(LogLevel::DEBUG, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
inline void log_info(std::string_view fmt, Args&&... args) {
    Logger::get().log(LogLevel::INFO, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
inline void log_warn(std::string_view fmt, Args&&... args) {
    Logger::get().log(LogLevel::WARN, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
inline void log_error(std::string_view fmt, Args&&... args) {
    Logger::get().log(LogLevel::ERR, fmt, std::forward<Args>(args)...);
}
