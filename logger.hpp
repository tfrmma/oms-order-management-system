#pragma once

// async logger. hot path writes into a SPSC ring, a background thread drains
// to disk + stderr (errors only). zero heap, zero blocking on the critical path.
//
// format: TSV with fixed-width fields so grep/awk work cleanly and tail -f
// is readable without tooling.
//
// severity:
//   ERROR, risk reject, pool exhaustion, queue full. always to stderr too.
//   WARN, partial fill, reroute triggered, stale margin data.
//   INFO, order lifecycle: new, fill, cancel, reject.
//   DEBUG, SOR slice counts, cursor state, dispatch detail.

#include "oms_types.hpp"
#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <string_view>

namespace oms {

enum class LogLevel : uint8_t { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

// fixed-size log entry, no heap, no std::string, no dynamic anything.
struct alignas(kCacheLine) LogEntry {
    static constexpr uint32_t kMsgLen = 184;

    uint64_t  ts_ns;
    LogLevel  level;
    uint8_t   _pad[7];
    char      msg[kMsgLen];
};

// ring size: 64K entries = ~13 MB. enough to absorb a burst without blocking.
inline constexpr uint32_t kLogRingSize = 1 << 16;

class AsyncLogger {
public:
    AsyncLogger() = default;

    // call once at startup. opens the log file, starts the drain thread.
    bool init(const char* filepath) noexcept {
        file_ = std::fopen(filepath, "a");
        if (!file_) {
            std::fprintf(stderr, "[LOGGER] failed to open %s\n", filepath);
            return false;
        }
        // header so you know where sessions start
        std::fprintf(file_, "# session_start ts_ns=%lu\n", now_ns());
        std::fflush(file_);

        running_.store(true, std::memory_order_release);
        drain_thread_ = std::thread([this]{ drain_loop(); });
        return true;
    }

    void shutdown() noexcept {
        running_.store(false, std::memory_order_release);
        if (drain_thread_.joinable()) drain_thread_.join();
        if (file_) { std::fflush(file_); std::fclose(file_); file_ = nullptr; }
    }

    ~AsyncLogger() noexcept { shutdown(); }

    // hot path: format into the ring entry, release to the drain thread.
    // if the ring is full we drop and count it, no blocking, ever.
    template<typename... Args>
    void log(LogLevel level, const char* fmt, Args&&... args) noexcept {
        const uint32_t h      = head_.load(std::memory_order_relaxed);
        const uint32_t next_h = (h + 1) & kMask;
        if (next_h == tail_.load(std::memory_order_acquire)) [[unlikely]] {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        LogEntry& e = ring_[h];
        e.ts_ns = now_ns();
        e.level = level;
        // safe: snprintf always null-terminates
        std::snprintf(e.msg, LogEntry::kMsgLen, fmt, std::forward<Args>(args)...);
        head_.store(next_h, std::memory_order_release);
    }

    // convenience wrappers
    template<typename... Args> void debug(const char* fmt, Args&&... args) noexcept
        { log(LogLevel::DEBUG, fmt, std::forward<Args>(args)...); }
    template<typename... Args> void info(const char* fmt, Args&&... args) noexcept
        { log(LogLevel::INFO,  fmt, std::forward<Args>(args)...); }
    template<typename... Args> void warn(const char* fmt, Args&&... args) noexcept
        { log(LogLevel::WARN,  fmt, std::forward<Args>(args)...); }
    template<typename... Args> void error(const char* fmt, Args&&... args) noexcept
        { log(LogLevel::ERROR, fmt, std::forward<Args>(args)...); }

    [[nodiscard]] uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    static constexpr uint32_t kMask = kLogRingSize - 1;
    static_assert((kLogRingSize & kMask) == 0);

    static const char* level_str(LogLevel l) noexcept {
        switch (l) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
        }
        return "?????";
    }

    static uint64_t now_ns() noexcept {
        struct timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    void drain_loop() noexcept {
        while (running_.load(std::memory_order_acquire) ||
               tail_.load(std::memory_order_relaxed) != head_.load(std::memory_order_acquire)) {

            const uint32_t t = tail_.load(std::memory_order_relaxed);
            if (t == head_.load(std::memory_order_acquire)) {
                // nothing to drain, yield so we don't eat a core
                std::this_thread::yield();
                continue;
            }

            const LogEntry& e = ring_[t];
            const char* lvl   = level_str(e.level);

            if (file_)
                std::fprintf(file_, "%lu\t%s\t%s\n", e.ts_ns, lvl, e.msg);

            // errors always go to stderr immediately
            if (e.level == LogLevel::ERROR)
                std::fprintf(stderr, "\033[1;31m[%s]\033[0m %s\n", lvl, e.msg);
            else if (e.level == LogLevel::WARN)
                std::fprintf(stderr, "\033[1;33m[%s]\033[0m %s\n", lvl, e.msg);

            tail_.store((t + 1) & kMask, std::memory_order_release);

            // flush every 256 entries to avoid losing data on crash
            if ((t & 0xFF) == 0 && file_) std::fflush(file_);
        }
        if (file_) std::fflush(file_);
    }

    alignas(kCacheLine) std::array<LogEntry, kLogRingSize> ring_{};
    alignas(kCacheLine) std::atomic<uint32_t> head_{0};
    alignas(kCacheLine) std::atomic<uint32_t> tail_{0};
    alignas(kCacheLine) std::atomic<uint64_t> dropped_{0};

    std::atomic<bool> running_{false};
    std::thread       drain_thread_;
    std::FILE*        file_{nullptr};
};

// process-wide logger instance. init() in main before anything else.
inline AsyncLogger g_log;

} // namespace oms
