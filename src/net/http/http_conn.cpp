#include "http_conn.h"

http_conn::http_conn(int fd) : io_(fd) {}

ssize_t http_conn::read_from_socket() {
    return io_.read_from_socket();
}

void http_conn::queue_write(std::string_view data) { io_.queue_write(data); }

ssize_t http_conn::flush_to_socket() { return io_.flush_to_socket(); }

std::string http_conn::take_write_buffer() { return io_.take_write_buffer(); }

bool http_conn::parse_available_data() {
    if (has_parse_error() || parse_offset_ >= io_.read_buffer().size()) {
        return !has_parse_error();
    }

    std::string_view remain(io_.read_buffer().data() + parse_offset_,
                            io_.read_buffer().size() - parse_offset_);
    if (!parser_.parse(remain)) {
        return false;
    }

    parse_offset_ = io_.read_buffer().size();
    return true;
}

void http_conn::reset_for_next_message() {
    if (parse_offset_ > 0) {
        io_.consume_read_bytes(parse_offset_);
    }
    parse_offset_ = 0;

    parser_.reset_for_next_message();
}