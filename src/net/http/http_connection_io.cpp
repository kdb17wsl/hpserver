#include "http_connection_io.h"

#include <errno.h>
#include <sys/socket.h>

namespace {
constexpr std::size_t kReadChunkSize = 8192;
}

http_connection_io::http_connection_io(int fd) : sock_(fd) {}

ssize_t http_connection_io::read_from_socket() {
    if (!valid()) {
        errno = EBADF;
        return -1;
    }

    char tmp[kReadChunkSize];
    ssize_t total = 0;
    while (true) {
        ssize_t n = ::recv(fd(), tmp, sizeof(tmp), 0);
        if (n > 0) {
            read_buffer_.append(tmp, static_cast<std::size_t>(n));
            total += n;
            continue;
        }

        if (n == 0) {
            return total;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return total;
        }

        return -1;
    }
}

void http_connection_io::queue_write(std::string_view data) {
    if (data.empty()) {
        return;
    }
    write_buffer_.append(data.data(), data.size());
}

ssize_t http_connection_io::flush_to_socket() {
    if (!valid()) {
        errno = EBADF;
        return -1;
    }

    ssize_t total = 0;
    while (write_offset_ < write_buffer_.size()) {
        const char* data = write_buffer_.data() + write_offset_;
        const std::size_t remain = write_buffer_.size() - write_offset_;
        ssize_t n = ::send(fd(), data, remain, 0);
        if (n > 0) {
            write_offset_ += static_cast<std::size_t>(n);
            total += n;
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }

        return -1;
    }

    if (write_offset_ == write_buffer_.size()) {
        write_buffer_.clear();
        write_offset_ = 0;
    }

    return total;
}

std::string http_connection_io::take_write_buffer() {
    if (write_offset_ == 0) {
        std::string out = std::move(write_buffer_);
        write_buffer_.clear();
        write_offset_ = 0;
        return out;
    }

    std::string out = write_buffer_.substr(write_offset_);
    write_buffer_.clear();
    write_offset_ = 0;
    return out;
}

bool http_connection_io::has_pending_write() const {
    return write_offset_ < write_buffer_.size();
}

void http_connection_io::consume_read_bytes(std::size_t bytes) {
    if (bytes == 0) {
        return;
    }

    if (bytes >= read_buffer_.size()) {
        read_buffer_.clear();
        return;
    }

    read_buffer_.erase(0, bytes);
}
