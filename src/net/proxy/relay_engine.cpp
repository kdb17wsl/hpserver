#include "relay_engine.h"

#include <sys/socket.h>

#include <cerrno>

namespace {
constexpr std::size_t kRelayChunk = 8192;
}

int relay_engine::read_from_fd_into_buffer(int fd, std::string& buffer, bool& read_closed) {
    char chunk[kRelayChunk];
    while (true) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n > 0) {
            buffer.append(chunk, static_cast<std::size_t>(n));
            continue;
        }

        if (n == 0) {
            read_closed = true;
            return 0;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

int relay_engine::flush_buffer_to_fd(int fd, std::string& buffer, std::size_t& offset) {
    while (offset < buffer.size()) {
        const ssize_t n = ::send(fd, buffer.data() + offset, buffer.size() - offset, 0);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        return -1;
    }

    buffer.clear();
    offset = 0;
    return 0;
}