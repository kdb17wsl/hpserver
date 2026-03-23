#pragma once

#include <cstdint>
#include <string>

class upstream_connector {
public:
    static int open_nonblocking(const std::string& host, std::uint16_t port,
                                int& upstream_fd, bool& connected_immediately);
};