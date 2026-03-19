#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "net/core/socket_ops.h"

class http_connection_io {
public:
    explicit http_connection_io(int fd = -1);

    int fd() const { return sock_.get_fd(); }
    bool valid() const { return sock_.get_fd() >= 0; }

    ssize_t read_from_socket();

    void queue_write(std::string_view data);
    ssize_t flush_to_socket();
    std::string take_write_buffer();

    const std::string& read_buffer() const { return read_buffer_; }
    void consume_read_bytes(std::size_t bytes);

private:
    socket_ops sock_;

    std::string read_buffer_;
    std::string write_buffer_;
    std::size_t write_offset_ = 0;
};
