#include "http_message_builder.h"

#include <cctype>
#include <string>
#include <string_view>

namespace {

std::string sanitize_header_token(std::string_view value, std::string_view fallback) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\r' || c == '\n') {
            continue;
        }
        out.push_back(c);
    }
    if (out.empty()) {
        out.assign(fallback.data(), fallback.size());
    }
    return out;
}

std::string sanitize_body(std::string_view body) {
    std::string out;
    out.reserve(body.size());
    for (char c : body) {
        out.push_back(c == '\r' ? ' ' : c);
    }
    return out;
}

std::string build_origin_form(std::string_view url) {
    if (url.rfind("http://", 0) == 0) {
        const std::size_t host_begin = sizeof("http://") - 1;
        const std::size_t path_pos = url.find('/', host_begin);
        if (path_pos == std::string::npos) {
            return "/";
        }
        return std::string(url.substr(path_pos));
    }

    if (url.rfind("https://", 0) == 0) {
        const std::size_t host_begin = sizeof("https://") - 1;
        const std::size_t path_pos = url.find('/', host_begin);
        if (path_pos == std::string::npos) {
            return "/";
        }
        return std::string(url.substr(path_pos));
    }

    if (url.empty()) {
        return "/";
    }

    if (url == "*") {
        return std::string(url);
    }

    if (url.front() != '/') {
        std::string out;
        out.reserve(url.size() + 1);
        out.push_back('/');
        out.append(url.data(), url.size());
        return out;
    }

    return std::string(url);
}

bool is_hop_by_hop_header(std::string_view key) {
    return key == "connection" || key == "proxy-connection" || key == "keep-alive" ||
           key == "te" || key == "trailer" || key == "transfer-encoding" ||
           key == "upgrade";
}

}  // namespace

namespace http_message_builder {

std::string build_plain_text_response(int status_code, std::string_view reason_phrase,
                                      std::string_view body, bool close_connection) {
    const std::string reason = sanitize_header_token(reason_phrase, "Unknown");
    const std::string payload = sanitize_body(body);

    std::string response;
    response.reserve(128 + payload.size());

    response.append("HTTP/1.1 ");
    response.append(std::to_string(status_code));
    response.push_back(' ');
    response.append(reason);
    response.append("\r\n");
    response.append("Content-Type: text/plain; charset=utf-8\r\n");
    response.append("Content-Length: ");
    response.append(std::to_string(payload.size()));
    response.append("\r\n");
    response.append("Connection: ");
    response.append(close_connection ? "close" : "keep-alive");
    response.append("\r\n");
    response.append("\r\n");
    response.append(payload);
    return response;
}

std::string build_service_unavailable_response(std::string_view detail) {
    std::string message = "proxy upstream unavailable";
    if (!detail.empty()) {
        message.append(": ");
        message.append(detail.data(), detail.size());
    }
    return build_plain_text_response(503, "Service Unavailable", message, true);
}

std::string build_connect_established_response(std::string_view proxy_agent) {
    const std::string safe_agent = sanitize_header_token(proxy_agent, "hpserver");

    std::string response;
    response.reserve(96 + safe_agent.size());
    response.append("HTTP/1.1 200 Connection Established\r\n");
    response.append("Proxy-Agent: ");
    response.append(safe_agent);
    response.append("\r\n");
    response.append("\r\n");
    return response;
}

std::string build_forward_proxy_request(const http_request_parser::request_info& req) {
    if (req.method.empty() || req.host.empty() || req.is_connect) {
        return {};
    }

    const std::string target = build_origin_form(req.url);
    const std::string version = req.version.empty() ? "1.1" : req.version;

    std::string out;
    out.reserve(1024 + req.body.size());
    out.append(req.method);
    out.push_back(' ');
    out.append(target);
    out.append(" HTTP/");
    out.append(version);
    out.append("\r\n");

    bool has_host_header = false;
    bool has_content_length_header = false;
    for (const auto& kv : req.headers) {
        const std::string& key = kv.first;
        if (is_hop_by_hop_header(key)) {
            continue;
        }

        if (key == "host") {
            has_host_header = true;
        }
        if (key == "content-length") {
            has_content_length_header = true;
        }

        out.append(key);
        out.append(": ");
        out.append(kv.second);
        out.append("\r\n");
    }

    if (!has_host_header) {
        out.append("host: ");
        out.append(req.host);
        if (req.port != 0 && req.port != 80) {
            out.push_back(':');
            out.append(std::to_string(req.port));
        }
        out.append("\r\n");
    }

    if (!has_content_length_header && !req.body.empty()) {
        out.append("content-length: ");
        out.append(std::to_string(req.body.size()));
        out.append("\r\n");
    }

    out.append("connection: close\r\n");
    out.append("\r\n");
    out.append(req.body);
    return out;
}

}  // namespace http_message_builder
