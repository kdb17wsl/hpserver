#include "http_proxy.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string_view>

namespace {
constexpr std::size_t kIoBufferSize = 8192;
}

bool http_proxy::wait_fd(int fd, short events) const {
	struct pollfd pfd {
		fd, events, 0
	};

	while (true) {
		int ret = ::poll(&pfd, 1, kIoTimeoutMs);
		if (ret > 0) {
			return true;
		}
		if (ret == 0) {
			errno = ETIMEDOUT;
			return false;
		}
		if (errno == EINTR) {
			continue;
		}
		return false;
	}
}

bool http_proxy::connect_upstream(const std::string& host, std::uint16_t port,
								  int& upstream_fd) const {
	upstream_fd = -1;

	struct addrinfo hints {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo* result = nullptr;
	const std::string port_str = std::to_string(port);
	if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
		return false;
	}

	bool connected = false;
	for (struct addrinfo* ai = result; ai != nullptr && !connected; ai = ai->ai_next) {
		int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol);
		if (fd == -1) {
			continue;
		}

		int ret = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
		if (ret == 0) {
			upstream_fd = fd;
			connected = true;
			break;
		}

		if (ret == -1 && errno == EINPROGRESS) {
			if (!wait_fd(fd, POLLOUT)) {
				::close(fd);
				continue;
			}

			int so_error = 0;
			socklen_t so_error_len = sizeof(so_error);
			if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0) {
				::close(fd);
				continue;
			}

			if (so_error == 0) {
				upstream_fd = fd;
				connected = true;
				break;
			}

			errno = so_error;
		}

		::close(fd);
	}

	::freeaddrinfo(result);
	return connected;
}

bool http_proxy::forward_request(const http_conn::request_info& req,
							 std::string& out_response, int* out_errno) {
	out_response.clear();
	if (out_errno != nullptr) {
		*out_errno = 0;
	}

	if (req.method.empty() || req.host.empty() || req.is_connect || req.port == 0) {
		if (out_errno != nullptr) {
			*out_errno = EINVAL;
		}
		errno = EINVAL;
		return false;
	}

	http_conn dummy_conn;
	http_proxy proxy(dummy_conn);

	std::string forward = req.method;
	forward.push_back(' ');
	forward.append(build_origin_form(req.url));
	forward.append(" HTTP/");
	forward.append(req.version.empty() ? "1.1" : req.version);
	forward.append("\r\n");

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

		forward.append(key);
		forward.append(": ");
		forward.append(kv.second);
		forward.append("\r\n");
	}

	if (!has_host_header) {
		forward.append("host: ");
		forward.append(req.host);
		if (req.port != 80) {
			forward.push_back(':');
			forward.append(std::to_string(req.port));
		}
		forward.append("\r\n");
	}

	if (!has_content_length_header && !req.body.empty()) {
		forward.append("content-length: ");
		forward.append(std::to_string(req.body.size()));
		forward.append("\r\n");
	}

	forward.append("connection: close\r\n\r\n");
	forward.append(req.body);

	int upstream_fd = -1;
	if (!proxy.connect_upstream(req.host, req.port, upstream_fd)) {
		if (out_errno != nullptr) {
			*out_errno = errno;
		}
		return false;
	}

	bool ok = proxy.send_all_nonblocking(upstream_fd, forward);
	if (!ok) {
		if (out_errno != nullptr) {
			*out_errno = errno;
		}
		::close(upstream_fd);
		return false;
	}

	char buf[kIoBufferSize];
	while (true) {
		ssize_t n = ::recv(upstream_fd, buf, sizeof(buf), 0);
		if (n > 0) {
			out_response.append(buf, static_cast<std::size_t>(n));
			continue;
		}

		if (n == 0) {
			break;
		}

		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			if (!proxy.wait_fd(upstream_fd, POLLIN)) {
				ok = false;
				break;
			}
			continue;
		}

		ok = false;
		break;
	}

	if (!ok && out_errno != nullptr) {
		*out_errno = errno;
	}

	::close(upstream_fd);
	return ok;
}

bool http_proxy::send_all_nonblocking(int fd, const std::string& data) const {
	std::size_t offset = 0;
	while (offset < data.size()) {
		ssize_t n = ::send(fd, data.data() + offset, data.size() - offset, 0);
		if (n > 0) {
			offset += static_cast<std::size_t>(n);
			continue;
		}

		if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (!wait_fd(fd, POLLOUT)) {
				return false;
			}
			continue;
		}

		return false;
	}

	return true;
}

bool http_proxy::relay_response_to_client(int upstream_fd) {
	char buf[kIoBufferSize];
	while (true) {
		ssize_t n = ::recv(upstream_fd, buf, sizeof(buf), 0);
		if (n > 0) {
			conn.queue_write(std::string_view(buf, static_cast<std::size_t>(n)));
			while (conn.has_pending_write()) {
				if (conn.flush_to_socket() < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						if (!wait_fd(conn.fd(), POLLOUT)) {
							return false;
						}
						continue;
					}
					return false;
				}

				if (conn.has_pending_write() && !wait_fd(conn.fd(), POLLOUT)) {
					return false;
				}
			}
			continue;
		}

		if (n == 0) {
			return true;
		}

		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			if (!wait_fd(upstream_fd, POLLIN)) {
				return false;
			}
			continue;
		}

		return false;
	}
}

std::string http_proxy::build_origin_form(const std::string& url) {
	if (url.rfind("http://", 0) == 0) {
		const std::size_t host_begin = std::strlen("http://");
		const std::size_t path_pos = url.find('/', host_begin);
		if (path_pos == std::string::npos) {
			return "/";
		}
		return url.substr(path_pos);
	}

	if (url.rfind("https://", 0) == 0) {
		const std::size_t host_begin = std::strlen("https://");
		const std::size_t path_pos = url.find('/', host_begin);
		if (path_pos == std::string::npos) {
			return "/";
		}
		return url.substr(path_pos);
	}

	if (url.empty()) {
		return "/";
	}

	if (url == "*") {
		return url;
	}

	if (!url.empty() && url.front() != '/') {
		return std::string("/") + url;
	}

	return url;
}

bool http_proxy::is_hop_by_hop_header(const std::string& key) {
	return key == "connection" || key == "proxy-connection" || key == "keep-alive" ||
		   key == "te" || key == "trailer" || key == "transfer-encoding" ||
		   key == "upgrade";
}

std::string http_proxy::build_forward_request() const {
	const auto& req = conn.request();
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

void http_proxy::queue_error_response(int status, const char* reason, const char* body) {
	std::string payload = body == nullptr ? "" : body;
	std::string response;
	response.append("HTTP/1.1 ");
	response.append(std::to_string(status));
	response.push_back(' ');
	response.append(reason == nullptr ? "Error" : reason);
	response.append("\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n");
	response.append("Content-Length: ");
	response.append(std::to_string(payload.size()));
	response.append("\r\n\r\n");
	response.append(payload);

	conn.queue_write(response);
	while (conn.has_pending_write()) {
		if (conn.flush_to_socket() < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (!wait_fd(conn.fd(), POLLOUT)) {
					break;
				}
				continue;
			}
			break;
		}
		if (conn.has_pending_write() && !wait_fd(conn.fd(), POLLOUT)) {
			break;
		}
	}
}

bool http_proxy::handle_request() {
	const auto& req = conn.request();
	if (req.is_connect) {
		queue_error_response(501, "Not Implemented", "CONNECT is not implemented yet.");
		return false;
	}

	if (req.host.empty() || req.port == 0) {
		queue_error_response(400, "Bad Request", "Missing or invalid Host header.");
		return false;
	}

	const std::string forward_request = build_forward_request();
	if (forward_request.empty()) {
		queue_error_response(400, "Bad Request", "Unable to build upstream request.");
		return false;
	}

	int upstream_fd = -1;
	if (!connect_upstream(req.host, req.port, upstream_fd)) {
		queue_error_response(502, "Bad Gateway", "Failed to connect upstream.");
		return false;
	}

	bool ok = send_all_nonblocking(upstream_fd, forward_request);
	if (ok) {
		ok = relay_response_to_client(upstream_fd);
	}

	::close(upstream_fd);
	if (!ok) {
		return false;
	}

	conn.reset_for_next_message();
	return true;
}