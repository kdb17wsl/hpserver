#include "http_response_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace {
http_response_parser* self_from_parser(llhttp_t* parser) {
    return static_cast<http_response_parser*>(parser->data);
}
}  // namespace

http_response_parser::http_response_parser() { init_parser(); }

bool http_response_parser::parse(std::string_view data) {
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

void http_response_parser::reset_for_next_message() {
    clear_response_runtime();
    parse_state_ = parse_state::kReadingResponse;
    parse_error_.clear();

    llhttp_reset(&parser_);
    parser_.data = this;
}

void http_response_parser::init_parser() {
    llhttp_settings_init(&settings_);
    settings_.on_message_begin = &http_response_parser::on_message_begin_cb;
    settings_.on_status = &http_response_parser::on_status_cb;
    settings_.on_version = &http_response_parser::on_version_cb;
    settings_.on_header_field = &http_response_parser::on_header_field_cb;
    settings_.on_header_value = &http_response_parser::on_header_value_cb;
    settings_.on_headers_complete = &http_response_parser::on_headers_complete_cb;
    settings_.on_body = &http_response_parser::on_body_cb;
    settings_.on_message_complete = &http_response_parser::on_message_complete_cb;

    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
}

void http_response_parser::clear_response_runtime() {
    response_ = response_info{};
    current_header_field_.clear();
    current_header_value_.clear();
    parsing_last_was_value_ = false;
}

int http_response_parser::on_message_begin_cb(llhttp_t* parser) {
    auto* self = self_from_parser(parser);
    self->clear_response_runtime();
    self->parse_state_ = parse_state::kReadingResponse;
    return 0;
}

int http_response_parser::on_status_cb(llhttp_t* parser, const char* at, size_t length) {
    auto* self = self_from_parser(parser);
    self->response_.reason_phrase.append(at, length);
    return 0;
}

int http_response_parser::on_version_cb(llhttp_t* parser, const char* at, size_t length) {
    auto* self = self_from_parser(parser);
    self->response_.version.append(at, length);
    return 0;
}

int http_response_parser::on_header_field_cb(llhttp_t* parser, const char* at,
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

int http_response_parser::on_header_value_cb(llhttp_t* parser, const char* at,
                                              size_t length) {
    auto* self = self_from_parser(parser);
    self->current_header_value_.append(at, length);
    self->parsing_last_was_value_ = true;
    return 0;
}

int http_response_parser::on_headers_complete_cb(llhttp_t* parser) {
    auto* self = self_from_parser(parser);
    self->finish_pending_header();
    self->response_.status_code = llhttp_get_status_code(parser);
    self->response_.keep_alive = llhttp_should_keep_alive(parser) != 0;
    self->response_.content_length = parser->content_length;
    self->post_process_headers();
    return 0;
}

int http_response_parser::on_body_cb(llhttp_t* parser, const char* at, size_t length) {
    auto* self = self_from_parser(parser);
    self->response_.body.append(at, length);
    return 0;
}

int http_response_parser::on_message_complete_cb(llhttp_t* parser) {
    auto* self = self_from_parser(parser);
    self->parse_state_ = parse_state::kMessageComplete;
    return 0;
}

void http_response_parser::finish_pending_header() {
    if (current_header_field_.empty()) {
        return;
    }

    std::string key = to_lower_ascii(current_header_field_);
    response_.headers[key] = current_header_value_;
}

void http_response_parser::post_process_headers() {
    auto te_it = response_.headers.find("transfer-encoding");
    if (te_it != response_.headers.end()) {
        std::string te = to_lower_ascii(te_it->second);
        response_.chunked = te.find("chunked") != std::string::npos;
    }
}

std::string http_response_parser::to_lower_ascii(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
