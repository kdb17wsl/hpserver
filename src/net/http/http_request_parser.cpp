#include "http_request_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace {
http_request_parser* self_from_parser(llhttp_t* parser) {
    return static_cast<http_request_parser*>(parser->data);
}
}  // namespace

http_request_parser::http_request_parser() { init_parser(); }

bool http_request_parser::parse(std::string_view data) {
    if (has_parse_error() || data.empty()) {
        return !has_parse_error();
    }

    llhttp_errno_t err = llhttp_execute(&parser_, data.data(), data.size());

    if (err == HPE_OK) {
        return true;
    }

    if (err == HPE_PAUSED_UPGRADE) {
        parse_state_ = parse_state::kUpgradedTunnel;
        llhttp_resume_after_upgrade(&parser_);
        return true;
    }

    parse_state_ = parse_state::kParseError;
    parse_error_ = llhttp_errno_name(err);
    const char* reason = llhttp_get_error_reason(&parser_);
    if (reason != nullptr && reason[0] != '\0') {
        parse_error_.append(": ");
        parse_error_.append(reason);
    }
    return false;
}

void http_request_parser::reset_for_next_message() {
    clear_request_runtime();
    parse_state_ = parse_state::kReadingRequest;
    parse_error_.clear();

    llhttp_reset(&parser_);
    parser_.data = this;
}

void http_request_parser::init_parser() {
    llhttp_settings_init(&settings_);
    settings_.on_message_begin = &http_request_parser::on_message_begin_cb;
    settings_.on_url = &http_request_parser::on_url_cb;
    settings_.on_version = &http_request_parser::on_version_cb;
    settings_.on_header_field = &http_request_parser::on_header_field_cb;
    settings_.on_header_value = &http_request_parser::on_header_value_cb;
    settings_.on_headers_complete = &http_request_parser::on_headers_complete_cb;
    settings_.on_body = &http_request_parser::on_body_cb;
    settings_.on_message_complete = &http_request_parser::on_message_complete_cb;

    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

void http_request_parser::clear_request_runtime() {
    request_ = request_info{};
    current_header_field_.clear();
    current_header_value_.clear();
    parsing_last_was_value_ = false;
}

int http_request_parser::on_message_begin_cb(llhttp_t* parser) {
    auto* self = self_from_parser(parser);
    self->clear_request_runtime();
    self->parse_state_ = parse_state::kReadingRequest;
    return 0;
}

int http_request_parser::on_url_cb(llhttp_t* parser, const char* at, size_t length) {
    auto* self = self_from_parser(parser);
    self->request_.url.append(at, length);
    return 0;
}

int http_request_parser::on_version_cb(llhttp_t* parser, const char* at, size_t length) {
    auto* self = self_from_parser(parser);
    self->request_.version.append(at, length);
    return 0;
}

int http_request_parser::on_header_field_cb(llhttp_t* parser, const char* at,
                                            size_t length) {
    auto* self = self_from_parser(parser);
    if (self->parsing_last_was_value_) {
        self->finish_pending_header();
        self->current_header_field_.clear();
        self->current_header_value_.clear();
        self->parsing_last_was_value_ = false;
    }

    self->current_header_field_.append(at, length);
    return 0;
}

int http_request_parser::on_header_value_cb(llhttp_t* parser, const char* at,
                                            size_t length) {
    auto* self = self_from_parser(parser);
    self->current_header_value_.append(at, length);
    self->parsing_last_was_value_ = true;
    return 0;
}

int http_request_parser::on_headers_complete_cb(llhttp_t* parser) {
    auto* self = self_from_parser(parser);
    self->finish_pending_header();
    self->request_.method =
        llhttp_method_name(static_cast<llhttp_method_t>(parser->method));
    self->request_.keep_alive = llhttp_should_keep_alive(parser) != 0;
    self->request_.content_length = parser->content_length;
    self->request_.is_connect = parser->method == HTTP_CONNECT;
    self->post_process_headers();
    return 0;
}

int http_request_parser::on_body_cb(llhttp_t* parser, const char* at, size_t length) {
    auto* self = self_from_parser(parser);
    self->request_.body.append(at, length);
    return 0;
}

int http_request_parser::on_message_complete_cb(llhttp_t* parser) {
    auto* self = self_from_parser(parser);
    self->parse_state_ = parse_state::kMessageComplete;
    return 0;
}

void http_request_parser::finish_pending_header() {
    if (current_header_field_.empty()) {
        return;
    }

    std::string key = to_lower_ascii(current_header_field_);
    request_.headers[key] = current_header_value_;
}

void http_request_parser::post_process_headers() {
    auto te_it = request_.headers.find("transfer-encoding");
    if (te_it != request_.headers.end()) {
        std::string te = to_lower_ascii(te_it->second);
        request_.chunked = te.find("chunked") != std::string::npos;
    }

    if (request_.is_connect) {
        parse_host_port(request_.url, request_.host, request_.port);
        return;
    }

    auto host_it = request_.headers.find("host");
    if (host_it != request_.headers.end()) {
        parse_host_port(host_it->second, request_.host, request_.port);
        if (request_.port == 0) {
            request_.port = 80;
        }
    }
}

std::string http_request_parser::to_lower_ascii(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void http_request_parser::parse_host_port(std::string_view host_port, std::string& host,
                                          std::uint16_t& port) {
    host.clear();
    port = 0;

    while (!host_port.empty() &&
           std::isspace(static_cast<unsigned char>(host_port.front())) != 0) {
        host_port.remove_prefix(1);
    }
    while (!host_port.empty() &&
           std::isspace(static_cast<unsigned char>(host_port.back())) != 0) {
        host_port.remove_suffix(1);
    }
    if (host_port.empty()) {
        return;
    }

    if (host_port.front() == '[') {
        std::size_t end = host_port.find(']');
        if (end == std::string_view::npos) {
            host.assign(host_port);
            return;
        }
        host.assign(host_port.substr(1, end - 1));

        if (end + 2 <= host_port.size() && host_port[end + 1] == ':') {
            std::string port_str(host_port.substr(end + 2));
            port = static_cast<std::uint16_t>(
                std::strtoul(port_str.c_str(), nullptr, 10));
        }
        return;
    }

    std::size_t colon_pos = host_port.rfind(':');
    if (colon_pos == std::string_view::npos || host_port.find(':') != colon_pos) {
        host.assign(host_port);
        return;
    }

    host.assign(host_port.substr(0, colon_pos));
    std::string port_str(host_port.substr(colon_pos + 1));
    port = static_cast<std::uint16_t>(std::strtoul(port_str.c_str(), nullptr, 10));
}
