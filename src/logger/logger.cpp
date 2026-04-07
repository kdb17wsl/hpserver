#include "logger.h"

#include <format>
#include <iostream>

void logger::init(const std::string& log_path, const size_t buffer_size) {
    log_queue = std::make_unique<ring_buffer<std::string>>(buffer_size);
    running = true;

    write_thread = std::thread([this, log_path]() {
        std::ofstream out;
        if (!log_path.empty()) {
            out.open(log_path, std::ios::app);
        }

        while (running || (log_queue && !log_queue->empty())) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this] {
                return !running || (log_queue && !log_queue->empty());
            });

            auto log_entry_opt = log_queue->pop();
            if (!log_entry_opt) {
                continue;
            }

            if (out.is_open()) {
                out << *log_entry_opt << std::endl;
            } else {
                std::cout << *log_entry_opt << std::endl;
            }
        }
    });
}

logger::~logger() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        running = false;
    }
    cv.notify_all();
    if (write_thread.joinable()) {
        write_thread.join();
    }
}