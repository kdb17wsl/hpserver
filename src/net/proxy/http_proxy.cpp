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
constexpr std::size_t kTunnelBufferLimit = 1 << 20;
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

bool http_proxy::flush_pending_nonblocking(int fd, std::string& buffer,
										   std::size_t& offset) const {
	while (offset < buffer.size()) {
		ssize_t n = ::send(fd, buffer.data() + offset, buffer.size() - offset, 0);
		if (n > 0) {
			offset += static_cast<std::size_t>(n);
			continue;
		}

		if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return true;
		}

		return false;
	}

	buffer.clear();
	offset = 0;
	return true;
}

bool http_proxy::relay_tunnel_bidirectional(int upstream_fd) {
	std::string client_to_upstream;
	std::string upstream_to_client;
	std::size_t c2u_offset = 0;
	std::size_t u2c_offset = 0;
	bool client_closed = false;
	bool upstream_closed = false;

	while (true) {
		if ((client_closed || upstream_closed) && c2u_offset == client_to_upstream.size() &&
			u2c_offset == upstream_to_client.size()) {
			return true;
		}

		struct pollfd pfds[2];
		pfds[0].fd = conn.fd();
		pfds[0].events = 0;
		pfds[0].revents = 0;
		pfds[1].fd = upstream_fd;
		pfds[1].events = 0;
		pfds[1].revents = 0;

		if (!client_closed && client_to_upstream.size() < kTunnelBufferLimit) {
			pfds[0].events |= POLLIN;
		}
		if (!upstream_closed && upstream_to_client.size() < kTunnelBufferLimit) {
			pfds[1].events |= POLLIN;
		}
		if (u2c_offset < upstream_to_client.size()) {
			pfds[0].events |= POLLOUT;
		}
		if (c2u_offset < client_to_upstream.size()) {
			pfds[1].events |= POLLOUT;
		}

		const int ret = ::poll(pfds, 2, kIoTimeoutMs);
		if (ret == 0) {
			errno = ETIMEDOUT;
			return false;
		}
		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}

		if ((pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			client_closed = true;
		}
		if ((pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			upstream_closed = true;
		}

		if (!client_closed && (pfds[0].revents & POLLIN) != 0) {
			char buf[kIoBufferSize];
			ssize_t n = ::recv(conn.fd(), buf, sizeof(buf), 0);
			if (n > 0) {
				client_to_upstream.append(buf, static_cast<std::size_t>(n));
				if (client_to_upstream.size() > kTunnelBufferLimit) {
					errno = ENOBUFS;
					return false;
				}
			} else if (n == 0) {
				client_closed = true;
			} else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
				return false;
			}
		}

		if (!upstream_closed && (pfds[1].revents & POLLIN) != 0) {
			char buf[kIoBufferSize];
			ssize_t n = ::recv(upstream_fd, buf, sizeof(buf), 0);
			if (n > 0) {
				upstream_to_client.append(buf, static_cast<std::size_t>(n));
				if (upstream_to_client.size() > kTunnelBufferLimit) {
					errno = ENOBUFS;
					return false;
				}
			} else if (n == 0) {
				upstream_closed = true;
			} else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
				return false;
			}
		}

		if (c2u_offset < client_to_upstream.size() &&
			!flush_pending_nonblocking(upstream_fd, client_to_upstream, c2u_offset)) {
			return false;
		}
		if (u2c_offset < upstream_to_client.size() &&
			!flush_pending_nonblocking(conn.fd(), upstream_to_client, u2c_offset)) {
			return false;
		}
	}
}

bool http_proxy::handle_connect_tunnel(const std::string& host, std::uint16_t port) {
	int upstream_fd = -1;
	if (!connect_upstream(host, port, upstream_fd)) {
		queue_error_response(502, "Bad Gateway", "Failed to connect upstream.");
		return false;
	}

	const std::string response = "HTTP/1.1 200 Connection Established\r\n"
								 "Proxy-Agent: hpserver\r\n"
								 "\r\n";
	bool ok = send_all_nonblocking(conn.fd(), response);
	if (ok) {
		ok = relay_tunnel_bidirectional(upstream_fd);
	}

	::close(upstream_fd);
	if (!ok) {
		return false;
	}

	conn.reset_for_next_message();
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
		if (req.host.empty() || req.port == 0) {
			queue_error_response(400, "Bad Request", "Missing or invalid CONNECT target.");
			return false;
		}
		return handle_connect_tunnel(req.host, req.port);
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