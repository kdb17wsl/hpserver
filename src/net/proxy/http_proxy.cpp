#include "http_proxy.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "poller.h"
#include "logger/logger.h"

namespace {
constexpr std::size_t kIoBufferSize = 8192;
constexpr char kConnectEstablishedResponse[] =
    "HTTP/1.1 200 Connection Established\r\n"
    "Proxy-Agent: hpserver\r\n"
    "\r\n";

bool append_nonblocking_read(int fd, std::string& out, bool& eof) {
	char buf[kIoBufferSize];
	while (true) {
		ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
		if (n > 0) {
			out.append(buf, static_cast<std::size_t>(n));
			continue;
		}

		if (n == 0) {
			eof = true;
			return true;
		}

		if (errno == EINTR) {
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return true;
		}
		return false;
	}
}

bool flush_nonblocking(int fd, std::string& buf, std::size_t& offset) {
	while (offset < buf.size()) {
		ssize_t n = ::send(fd, buf.data() + offset, buf.size() - offset, 0);
		if (n > 0) {
			offset += static_cast<std::size_t>(n);
			continue;
		}

		if (n == -1 && errno == EINTR) {
			continue;
		}
		if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return true;
		}
		return false;
	}

	buf.clear();
	offset = 0;
	return true;
}
}

bool http_proxy::wait_fd(int fd, std::uint32_t events) {
	poller io_poller;
	if (!io_poller.valid()) {
		return false;
	}

	if (io_poller.add(fd, events) != 0) {
		return false;
	}

	struct epoll_event ready_event {};
	while (true) {
		const int ret = io_poller.wait(&ready_event, 1, kIoTimeoutMs);
		if (ret > 0) {
			break;
		}
		if (ret == 0) {
			errno = ETIMEDOUT;
			break;
		}
		if (errno == EINTR) {
			continue;
		}
		break;
	}

	const int wait_errno = errno;
	(void)io_poller.remove(fd);
	errno = wait_errno;

	return ready_event.events != 0;
}

bool http_proxy::connect_upstream(const std::string& host, std::uint16_t port,
								  int& upstream_fd) {
	upstream_fd = -1;

	struct addrinfo hints {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo* result = nullptr;
	const std::string port_str = std::to_string(port);
	LOG_DEBUG("Connecting to upstream {}:{}", host, port);
	if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
		LOG_ERROR("DNS lookup failed for {}:{}", host, port);
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
			LOG_DEBUG("Directly connected to upstream {}:{} (fd: {})", host, port, upstream_fd);
			break;
		}

		if (ret == -1 && errno == EINPROGRESS) {
			if (!wait_fd(fd, EPOLLOUT)) {
				LOG_WARNING("Connection timeout/refused by wait_fd for {}:{}", host, port);
				::close(fd);
				continue;
			}

			int so_error = 0;
			socklen_t so_error_len = sizeof(so_error);
			if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0) {
				LOG_ERROR("getsockopt failed: {}", strerror(errno));
				::close(fd);
				continue;
			}

			if (so_error == 0) {
				upstream_fd = fd;
				connected = true;
				LOG_DEBUG("Async connected to upstream {}:{} (fd: {})", host, port, upstream_fd);
				break;
			}

			LOG_WARNING("Async connection failed for {}:{}: {}", host, port, strerror(so_error));
			errno = so_error;
		}

		::close(fd);
	}

	if (!connected) {
		LOG_ERROR("Failed to connect to any address for {}:{}", host, port);
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

	const std::string forward = build_forward_request(req);
	if (forward.empty()) {
		if (out_errno != nullptr) {
			*out_errno = EINVAL;
		}
		errno = EINVAL;
		return false;
	}

	int upstream_fd = -1;
	if (!connect_upstream(req.host, req.port, upstream_fd)) {
		LOG_ERROR("Failed to connect to upstream {}:{}: {}", req.host, req.port, strerror(errno));
		if (out_errno != nullptr) {
			*out_errno = errno;
		}
		return false;
	}

	LOG_DEBUG("Forwarding request to upstream fd {} ({})", upstream_fd, req.host);
	bool ok = send_all_nonblocking(upstream_fd, forward);
	if (!ok) {
		LOG_ERROR("Failed to send request to upstream fd {}: {}", upstream_fd, strerror(errno));
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
			LOG_DEBUG("Upstream closed connection (fd: {})", upstream_fd);
			break;
		}

		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			if (!wait_fd(upstream_fd, EPOLLIN)) {
				LOG_WARNING("Wait for upstream response timeout/error (fd: {})", upstream_fd);
				ok = false;
				break;
			}
			continue;
		}

		LOG_ERROR("Error receiving from upstream fd {}: {}", upstream_fd, strerror(errno));
		ok = false;
		break;
	}

	if (!ok && out_errno != nullptr) {
		*out_errno = errno;
	}

	::close(upstream_fd);
	return ok;
}

bool http_proxy::send_all_nonblocking(int fd, const std::string& data) {
	std::size_t offset = 0;
	while (offset < data.size()) {
		ssize_t n = ::send(fd, data.data() + offset, data.size() - offset, 0);
		if (n > 0) {
			offset += static_cast<std::size_t>(n);
			continue;
		}

		if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (!wait_fd(fd, EPOLLOUT)) {
				return false;
			}
			continue;
		}

		return false;
	}

	return true;
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

std::string http_proxy::build_forward_request(const http_conn::request_info& req) {
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

bool http_proxy::forward_connect_tunnel(int client_fd, const http_conn::request_info& req,
									int* out_errno) {
	LOG_DEBUG("Establishing CONNECT tunnel for {}:{} on client fd {}", req.host, req.port, client_fd);
	if (out_errno != nullptr) {
		*out_errno = 0;
	}

	if (client_fd < 0 || req.host.empty()) {
		LOG_ERROR("Invalid arguments for CONNECT tunnel: client_fd={}, host={}", client_fd, req.host);
		errno = EINVAL;
		if (out_errno != nullptr) {
			*out_errno = errno;
		}
		return false;
	}

	const std::uint16_t upstream_port = req.port == 0 ? 443 : req.port;
	int upstream_fd = -1;
	if (!connect_upstream(req.host, upstream_port, upstream_fd)) {
		LOG_ERROR("CONNECT tunnel: failed to connect to upstream {}:{}", req.host, upstream_port);
		if (out_errno != nullptr) {
			*out_errno = errno;
		}
		return false;
	}

	LOG_DEBUG("CONNECT tunnel: sending 200 Connection Established to client fd {}", client_fd);
	if (!send_all_nonblocking(client_fd, kConnectEstablishedResponse)) {
		LOG_ERROR("CONNECT tunnel: failed to send response to client fd {}: {}", client_fd, strerror(errno));
		if (out_errno != nullptr) {
			*out_errno = errno;
		}
		::close(upstream_fd);
		return false;
	}

	LOG_DEBUG("CONNECT tunnel entering data forwarding loop: client fd {}, upstream fd {}", client_fd, upstream_fd);
	std::string c2u_buf;
	std::string u2c_buf;
	std::size_t c2u_offset = 0;
	std::size_t u2c_offset = 0;
	bool client_eof = false;
	bool upstream_eof = false;
	bool upstream_write_shutdown = false;
	bool client_write_shutdown = false;

	while (true) {
		if (!flush_nonblocking(upstream_fd, c2u_buf, c2u_offset)) {
			break;
		}
		if (!flush_nonblocking(client_fd, u2c_buf, u2c_offset)) {
			break;
		}

		if (client_eof && c2u_buf.empty() && !upstream_write_shutdown) {
			::shutdown(upstream_fd, SHUT_WR);
			upstream_write_shutdown = true;
		}
		if (upstream_eof && u2c_buf.empty() && !client_write_shutdown) {
			::shutdown(client_fd, SHUT_WR);
			client_write_shutdown = true;
		}

		if (client_eof && upstream_eof && c2u_buf.empty() && u2c_buf.empty()) {
			::close(upstream_fd);
			return true;
		}

		struct pollfd fds[2] = {};
		fds[0].fd = client_fd;
		fds[0].events = 0;
		if (!client_eof && c2u_buf.empty()) {
			fds[0].events |= POLLIN;
		}
		if (!u2c_buf.empty()) {
			fds[0].events |= POLLOUT;
		}

		fds[1].fd = upstream_fd;
		fds[1].events = 0;
		if (!upstream_eof && u2c_buf.empty()) {
			fds[1].events |= POLLIN;
		}
		if (!c2u_buf.empty()) {
			fds[1].events |= POLLOUT;
		}

		if (fds[0].events == 0 && fds[1].events == 0) {
			continue;
		}

		int n = ::poll(fds, 2, kIoTimeoutMs);
		if (n == 0) {
			errno = ETIMEDOUT;
			break;
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}

		if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
			(fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			errno = ECONNRESET;
			break;
		}

		if ((fds[0].revents & POLLIN) != 0) {
			if (!append_nonblocking_read(client_fd, c2u_buf, client_eof)) {
				break;
			}
		}
		if ((fds[1].revents & POLLIN) != 0) {
			if (!append_nonblocking_read(upstream_fd, u2c_buf, upstream_eof)) {
				break;
			}
		}
	}

	if (out_errno != nullptr) {
		*out_errno = errno;
	}
	::close(upstream_fd);
	return false;
}
