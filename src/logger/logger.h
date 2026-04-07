#pragma once

#include <format>
#include <string>
#include <thread>
#include <fstream>
#include <atomic>
#include <memory>

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
    void write_log(log_level level, std::format_string<Args...> fmt, Args&&... args) {
        std::string formatted_message = std::format(fmt, std::forward<Args>(args)...);

        std::string log_entry;
        switch (level) {
            case log_level::info:
                log_entry = "[INFO] " + formatted_message;
                break;
            case log_level::warning:
                log_entry = "[WARNING] " + formatted_message;
                break;
            case log_level::error:
                log_entry = "[ERROR] " + formatted_message;
                break;
            case log_level::debug:
                log_entry = "[DEBUG] " + formatted_message;
                break;
        }

        if (log_queue) {
            log_queue->push(std::move(log_entry));
        }
    }

private:
    logger() = default;

    std::unique_ptr<ring_buffer<std::string>> log_queue;
    std::thread write_thread;
    std::atomic<bool> running{false};
};

#define LOG_INFO(fmt, ...) logger::instance().write_log(log_level::info, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) logger::instance().write_log(log_level::warning, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) logger::instance().write_log(log_level::error, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) logger::instance().write_log(log_level::debug, fmt, ##__VA_ARGS__)