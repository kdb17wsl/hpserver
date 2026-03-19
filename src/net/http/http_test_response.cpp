#include "http_test_response.h"

#include <sstream>

namespace {

const char* bool_to_text(bool value) { return value ? "true" : "false"; }

}  // namespace

std::string build_test_echo_response(const http_request_parser::request_info& req) {
    std::ostringstream body;
    body << "method=" << req.method << "\n";
    body << "url=" << req.url << "\n";
    body << "version=" << req.version << "\n";
    body << "host=" << req.host << "\n";
    body << "port=" << req.port << "\n";
    body << "content_length=" << req.content_length << "\n";
    body << "keep_alive=" << bool_to_text(req.keep_alive) << "\n";
    body << "is_connect=" << bool_to_text(req.is_connect) << "\n";

    const std::string body_text = body.str();

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: text/plain; charset=utf-8\r\n";
    response << "Content-Length: " << body_text.size() << "\r\n";
    response << "Connection: " << (req.keep_alive ? "keep-alive" : "close") << "\r\n";
    response << "\r\n";
    response << body_text;

    return response.str();
}
