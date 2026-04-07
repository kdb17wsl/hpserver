#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <thread>

#include "util/ring_buffer.h"

enum class log_level { info, warning, error, debug };

class logger {
public:
    void init(const std::string& log_path = "", size_t buffer_size = 1024);
    ~logger();

    static logger& instance() {
        static logger instance;
        return instance;
    }

    template <typename... Args>
    void write_log(log_level level, const std::source_location location,
                   std::format_string<Args...> fmt, Args&&... args) {
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        auto now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm_now;
        localtime_r(&now_time_t, &tm_now);

        std::string level_str;
        switch (level) {
            case log_level::info:
                level_str = "INFO";
                break;
            case log_level::warning:
                level_str = "WARN";
                break;
            case log_level::error:
                level_str = "ERROR";
                break;
            case log_level::debug:
                level_str = "DEBUG";
                break;
        }

        std::string filename = std::filesystem::path(location.file_name()).filename().string();
        std::string user_msg = std::format(fmt, std::forward<Args>(args)...);

        // 格式: [2026-3-27 10:00:00.000] [INFO] [main.cpp:42] message
        std::string log_entry =
            std::format("[{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}] [{}] [{}:{}] {}",
                        tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour,
                        tm_now.tm_min, tm_now.tm_sec, static_cast<int>(now_ms.count()), level_str,
                        filename, location.line(), user_msg);

        if (log_queue) {
            if (log_queue->push(std::move(log_entry))) {
                cv.notify_one();
            }
        }
    }

private:
    logger() = default;

    std::unique_ptr<ring_buffer<std::string>> log_queue;
    std::thread write_thread;
    std::atomic<bool> running{false};

    std::mutex mtx;
    std::condition_variable cv;
};

#define LOG_INFO(fmt, ...)                                                              \
    logger::instance().write_log(log_level::info, std::source_location::current(), fmt, \
                                 ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...)                                                              \
    logger::instance().write_log(log_level::warning, std::source_location::current(), fmt, \
                                 ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)                                                              \
    logger::instance().write_log(log_level::error, std::source_location::current(), fmt, \
                                 ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)                                                              \
    logger::instance().write_log(log_level::debug, std::source_location::current(), fmt, \
                                 ##__VA_ARGS__)