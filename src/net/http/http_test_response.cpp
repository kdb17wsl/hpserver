#include "http_test_response.h"

#include <algorithm>
#include <sstream>
#include <utility>
#include <vector>

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
    body << "chunked=" << bool_to_text(req.chunked) << "\n";
    body << "keep_alive=" << bool_to_text(req.keep_alive) << "\n";
    body << "is_connect=" << bool_to_text(req.is_connect) << "\n";
    body << "header_count=" << req.headers.size() << "\n";
    body << "body_size=" << req.body.size() << "\n";

    std::vector<std::pair<std::string, std::string>> sorted_headers;
    sorted_headers.reserve(req.headers.size());
    for (const auto& entry : req.headers) {
        sorted_headers.emplace_back(entry.first, entry.second);
    }
    std::sort(sorted_headers.begin(), sorted_headers.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

    body << "headers_begin\n";
    for (const auto& header : sorted_headers) {
        body << header.first << ": " << header.second << "\n";
    }
    body << "headers_end\n";
    body << "body_begin\n";
    body << req.body << "\n";
    body << "body_end\n";

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
