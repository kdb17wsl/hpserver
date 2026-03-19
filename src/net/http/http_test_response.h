#pragma once

#include <string>

#include "http_request_parser.h"

std::string build_test_echo_response(const http_request_parser::request_info& req);
