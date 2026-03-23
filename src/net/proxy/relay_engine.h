#pragma once

#include <cstddef>
#include <string>

class relay_engine {
public:
    static int read_from_fd_into_buffer(int fd, std::string& buffer, bool& read_closed);
    static int flush_buffer_to_fd(int fd, std::string& buffer, std::size_t& offset);
};