#pragma once

#include <string>
#include <string_view>

#include "http_request_parser.h"

namespace http_message_builder {

std::string build_plain_text_response(int status_code, std::string_view reason_phrase,
                                      std::string_view body, bool close_connection = true);

std::string build_service_unavailable_response(std::string_view detail);

std::string build_forbidden_response(std::string_view detail);

std::string build_connect_established_response(std::string_view proxy_agent = "hpserver");

std::string build_forward_proxy_request(const http_request_parser::request_info& req);

}  // namespace http_message_builder
