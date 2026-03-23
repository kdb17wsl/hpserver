#include "upstream_connector.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>

int upstream_connector::open_nonblocking(const std::string& host, std::uint16_t port,
                                         int& upstream_fd, bool& connected_immediately) {
    upstream_fd = -1;
    connected_immediately = false;

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        return -1;
    }

    int ret = -1;
    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol);
        if (fd == -1) {
            continue;
        }

        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            upstream_fd = fd;
            connected_immediately = true;
            ret = 0;
            break;
        }

        if (errno == EINPROGRESS) {
            upstream_fd = fd;
            ret = 0;
            break;
        }

        ::close(fd);
    }

    ::freeaddrinfo(result);
    return ret;
}