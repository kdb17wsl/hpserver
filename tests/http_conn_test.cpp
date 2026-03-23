#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "src/net/http/http_conn.h"
#include "src/net/http/http_test_response.h"
#include "src/net/proxy/http_proxy.h"

namespace {

class SocketPair {
public:
    SocketPair() {
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            throw std::runtime_error(std::strerror(errno));
        }
        ends_[0] = fds[0];
        ends_[1] = fds[1];
    }

    ~SocketPair() {
        if (ends_[0] != -1) {
            ::close(ends_[0]);
        }
        if (ends_[1] != -1) {
            ::close(ends_[1]);
        }
    }

    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    int server_fd() const { return ends_[0]; }
    int peer_fd() const { return ends_[1]; }

private:
    int ends_[2] = {-1, -1};
};

void set_nonblocking_or_fail(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ASSERT_NE(flags, -1) << std::strerror(errno);
    ASSERT_NE(::fcntl(fd, F_SETFL, flags | O_NONBLOCK), -1) << std::strerror(errno);
}

void write_all_or_fail(int fd, std::string_view data) {
    const char* ptr = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const ssize_t written = ::send(fd, ptr, remaining, 0);
        ASSERT_GT(written, 0) << std::strerror(errno);
        ptr += written;
        remaining -= static_cast<std::size_t>(written);
    }
}

std::string read_exact_or_fail(int fd, std::size_t size) {
    std::string out(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t n = ::recv(fd, out.data() + offset, size - offset, 0);
        if (n <= 0) {
            throw std::runtime_error(std::strerror(errno));
        }
        offset += static_cast<std::size_t>(n);
    }
    return out;
}

std::size_t parse_content_length(std::string_view headers) {
    const std::string key = "content-length:";
    std::string lower(headers);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    const std::size_t pos = lower.find(key);
    if (pos == std::string::npos) {
        return 0;
    }

    std::size_t value_begin = pos + key.size();
    while (value_begin < lower.size() &&
           (lower[value_begin] == ' ' || lower[value_begin] == '\t')) {
        ++value_begin;
    }

    std::size_t value_end = value_begin;
    while (value_end < lower.size() &&
           std::isdigit(static_cast<unsigned char>(lower[value_end])) != 0) {
        ++value_end;
    }

    if (value_end == value_begin) {
        return 0;
    }

    return static_cast<std::size_t>(std::strtoull(lower.substr(value_begin, value_end - value_begin).c_str(),
                                                  nullptr, 10));
}

std::string read_http_message_with_timeout(int fd, int timeout_ms) {
    std::string out;
    std::size_t expected_total = 0;

    while (true) {
        struct pollfd pfd {
            fd, POLLIN, 0
        };

        const int poll_ret = ::poll(&pfd, 1, timeout_ms);
        if (poll_ret <= 0) {
            break;
        }

        char buf[4096];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));

            if (expected_total == 0) {
                const std::size_t header_end = out.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    const std::size_t body_length =
                        parse_content_length(std::string_view(out.data(), header_end + 4));
                    expected_total = header_end + 4 + body_length;
                }
            }

            if (expected_total > 0 && out.size() >= expected_total) {
                break;
            }
            continue;
        }

        if (n == 0) {
            break;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }

        break;
    }

    return out;
}

std::string read_until_contains_with_timeout(int fd, std::string_view token,
                                             int timeout_ms) {
    std::string out;
    while (true) {
        if (!token.empty() && out.find(token) != std::string::npos) {
            break;
        }

        struct pollfd pfd {
            fd, POLLIN, 0
        };
        const int poll_ret = ::poll(&pfd, 1, timeout_ms);
        if (poll_ret <= 0) {
            break;
        }

        char buf[4096];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            continue;
        }
        break;
    }
    return out;
}

class LocalUpstreamServer {
public:
    explicit LocalUpstreamServer(std::string response) : response_(std::move(response)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ == -1) {
            throw std::runtime_error(std::strerror(errno));
        }

        int reuse = 1;
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            const std::string err = std::strerror(errno);
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error(err);
        }

        if (::listen(listen_fd_, 1) != 0) {
            const std::string err = std::strerror(errno);
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error(err);
        }

        sockaddr_in bound_addr {};
        socklen_t bound_len = sizeof(bound_addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound_addr), &bound_len) != 0) {
            const std::string err = std::strerror(errno);
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error(err);
        }
        port_ = ntohs(bound_addr.sin_port);

        worker_ = std::thread([this]() { this->serve_once(); });
    }

    ~LocalUpstreamServer() {
        if (listen_fd_ != -1) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    LocalUpstreamServer(const LocalUpstreamServer&) = delete;
    LocalUpstreamServer& operator=(const LocalUpstreamServer&) = delete;

    std::uint16_t port() const { return port_; }

    std::string take_received_request() {
        if (worker_.joinable()) {
            worker_.join();
        }
        return received_request_;
    }

private:
    void serve_once() {
        int conn_fd = -1;
        while (true) {
            conn_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (conn_fd >= 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            return;
        }

        received_request_ = read_http_message_with_timeout(conn_fd, 2000);

        std::size_t sent = 0;
        while (sent < response_.size()) {
            const ssize_t n = ::send(conn_fd, response_.data() + sent, response_.size() - sent, 0);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n == -1 && errno == EINTR) {
                continue;
            }
            break;
        }

        ::shutdown(conn_fd, SHUT_WR);
        ::close(conn_fd);
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread worker_;
    std::string response_;
    std::string received_request_;
};

class LocalTunnelUpstreamServer {
public:
    LocalTunnelUpstreamServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ == -1) {
            throw std::runtime_error(std::strerror(errno));
        }

        int reuse = 1;
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            const std::string err = std::strerror(errno);
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error(err);
        }

        if (::listen(listen_fd_, 1) != 0) {
            const std::string err = std::strerror(errno);
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error(err);
        }

        sockaddr_in bound_addr {};
        socklen_t bound_len = sizeof(bound_addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound_addr), &bound_len) != 0) {
            const std::string err = std::strerror(errno);
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error(err);
        }
        port_ = ntohs(bound_addr.sin_port);

        worker_ = std::thread([this]() { this->serve_once(); });
    }

    ~LocalTunnelUpstreamServer() {
        if (listen_fd_ != -1) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    LocalTunnelUpstreamServer(const LocalTunnelUpstreamServer&) = delete;
    LocalTunnelUpstreamServer& operator=(const LocalTunnelUpstreamServer&) = delete;

    std::uint16_t port() const { return port_; }

    std::string take_client_bytes() {
        if (worker_.joinable()) {
            worker_.join();
        }
        return client_bytes_;
    }

private:
    void serve_once() {
        int conn_fd = -1;
        while (true) {
            conn_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (conn_fd >= 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            return;
        }

        const std::string hello = "UP_HELLO";
        (void)::send(conn_fd, hello.data(), hello.size(), 0);

        client_bytes_ = read_until_contains_with_timeout(conn_fd, "PING", 2000);
        const std::string reply = "UP_ECHO:" + client_bytes_;
        (void)::send(conn_fd, reply.data(), reply.size(), 0);

        ::shutdown(conn_fd, SHUT_WR);
        ::close(conn_fd);
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread worker_;
    std::string client_bytes_;
};

}  // namespace

TEST(HttpConnTest, ParsesSimpleGetRequest) {
    SocketPair sockets;
    set_nonblocking_or_fail(sockets.server_fd());

    http_conn conn(sockets.server_fd());

    write_all_or_fail(
        sockets.peer_fd(),
        "GET /hello HTTP/1.1\r\n"
        "Host: Example.COM:8081\r\n"
        "Connection: keep-alive\r\n"
        "\r\n");

    ASSERT_GT(conn.read_from_socket(), 0);
    ASSERT_TRUE(conn.parse_available_data());
    ASSERT_TRUE(conn.is_message_complete());

    const auto& req = conn.request();
    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.url, "/hello");
    EXPECT_EQ(req.version, "1.1");
    EXPECT_EQ(req.host, "Example.COM");
    EXPECT_EQ(req.port, 8081);
    EXPECT_TRUE(req.keep_alive);
    EXPECT_FALSE(req.chunked);
    EXPECT_EQ(req.content_length, 0U);
    EXPECT_TRUE(req.body.empty());
    ASSERT_EQ(req.headers.count("host"), 1U);
    EXPECT_EQ(req.headers.at("host"), "Example.COM:8081");
}

TEST(HttpConnTest, ResetsAndParsesNextKeepAliveRequest) {
    SocketPair sockets;
    set_nonblocking_or_fail(sockets.server_fd());

    http_conn conn(sockets.server_fd());

    write_all_or_fail(
        sockets.peer_fd(),
        "POST /first HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello");

    ASSERT_GT(conn.read_from_socket(), 0);
    ASSERT_TRUE(conn.parse_available_data());
    ASSERT_TRUE(conn.is_message_complete());
    EXPECT_EQ(conn.request().method, "POST");
    EXPECT_EQ(conn.request().body, "hello");
    EXPECT_EQ(conn.request().port, 80);

    conn.reset_for_next_message();
    EXPECT_EQ(conn.state(), http_conn::parse_state::kReadingRequest);
    EXPECT_TRUE(conn.request().method.empty());
    EXPECT_TRUE(conn.read_buffer().empty());

    write_all_or_fail(
        sockets.peer_fd(),
        "GET /second HTTP/1.1\r\n"
        "Host: second.example:9090\r\n"
        "\r\n");

    ASSERT_GT(conn.read_from_socket(), 0);
    ASSERT_TRUE(conn.parse_available_data());
    ASSERT_TRUE(conn.is_message_complete());

    const auto& req = conn.request();
    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.url, "/second");
    EXPECT_EQ(req.host, "second.example");
    EXPECT_EQ(req.port, 9090);
    EXPECT_TRUE(req.body.empty());
}

TEST(HttpConnTest, ParsesConnectAuthorityFromRequestTarget) {
    SocketPair sockets;
    set_nonblocking_or_fail(sockets.server_fd());

    http_conn conn(sockets.server_fd());

    write_all_or_fail(
        sockets.peer_fd(),
        "CONNECT upstream.example:443 HTTP/1.1\r\n"
        "Host: ignored.example:8443\r\n"
        "\r\n");

    ASSERT_GT(conn.read_from_socket(), 0);
    ASSERT_TRUE(conn.parse_available_data());
    ASSERT_TRUE(conn.is_message_complete());

    const auto& req = conn.request();
    EXPECT_TRUE(req.is_connect);
    EXPECT_EQ(req.method, "CONNECT");
    EXPECT_EQ(req.url, "upstream.example:443");
    EXPECT_EQ(req.host, "upstream.example");
    EXPECT_EQ(req.port, 443);
}

TEST(HttpConnTest, FlushesQueuedWriteBufferToSocket) {
    SocketPair sockets;

    http_conn conn(sockets.server_fd());
    conn.queue_write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");

    const std::string expected = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
    ASSERT_EQ(conn.flush_to_socket(), static_cast<ssize_t>(expected.size()));
    EXPECT_TRUE(conn.take_write_buffer().empty());
    EXPECT_EQ(read_exact_or_fail(sockets.peer_fd(), expected.size()), expected);
}

TEST(HttpConnTest, ReportsParseErrorForInvalidRequest) {
    SocketPair sockets;
    set_nonblocking_or_fail(sockets.server_fd());

    http_conn conn(sockets.server_fd());

    write_all_or_fail(sockets.peer_fd(), "BAD REQUEST\r\n\r\n");

    ASSERT_GT(conn.read_from_socket(), 0);
    EXPECT_FALSE(conn.parse_available_data());
    EXPECT_TRUE(conn.has_parse_error());
    EXPECT_FALSE(conn.parse_error().empty());
}

TEST(HttpConnTest, BuildsEchoResponseFromRequestInfo) {
    http_request_parser::request_info req;
    req.method = "POST";
    req.url = "/echo";
    req.version = "1.1";
    req.host = "example.org";
    req.port = 8088;
    req.content_length = 7;
    req.keep_alive = true;
    req.is_connect = false;

    const std::string response = build_test_echo_response(req);

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(response.find("Content-Type: text/plain; charset=utf-8\r\n"), std::string::npos);
    EXPECT_NE(response.find("Connection: keep-alive\r\n"), std::string::npos);
    EXPECT_NE(response.find("\r\n\r\nmethod=POST\n"), std::string::npos);
    EXPECT_NE(response.find("url=/echo\n"), std::string::npos);
    EXPECT_NE(response.find("version=1.1\n"), std::string::npos);
    EXPECT_NE(response.find("host=example.org\n"), std::string::npos);
    EXPECT_NE(response.find("port=8088\n"), std::string::npos);
    EXPECT_NE(response.find("content_length=7\n"), std::string::npos);
    EXPECT_NE(response.find("keep_alive=true\n"), std::string::npos);
    EXPECT_NE(response.find("is_connect=false\n"), std::string::npos);
}

TEST(HttpConnTest, ProxyForwardsGetAsOriginFormAndFiltersHopHeaders) {
    SocketPair sockets;
    set_nonblocking_or_fail(sockets.server_fd());

    LocalUpstreamServer upstream(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 6\r\n"
        "Connection: close\r\n"
        "\r\n"
        "GET_OK");

    http_conn conn(sockets.server_fd());

    const std::string request =
        "GET http://127.0.0.1:" + std::to_string(upstream.port()) +
        "/hello?q=1 HTTP/1.1\r\n"
        "Host: 127.0.0.1:" + std::to_string(upstream.port()) +
        "\r\n"
        "Proxy-Connection: keep-alive\r\n"
        "Connection: keep-alive\r\n"
        "Keep-Alive: timeout=5\r\n"
        "\r\n";

    write_all_or_fail(sockets.peer_fd(), request);
    ASSERT_GT(conn.read_from_socket(), 0);
    ASSERT_TRUE(conn.parse_available_data());
    ASSERT_TRUE(conn.is_message_complete());

    http_proxy proxy(conn);
    ASSERT_TRUE(proxy.handle_request());

    const std::string response = read_http_message_with_timeout(sockets.peer_fd(), 2000);
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(response.find("GET_OK"), std::string::npos);

    const std::string forwarded = upstream.take_received_request();
    EXPECT_NE(forwarded.find("GET /hello?q=1 HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(forwarded.find("host: 127.0.0.1:" + std::to_string(upstream.port()) + "\r\n"),
              std::string::npos);
    EXPECT_EQ(forwarded.find("proxy-connection:"), std::string::npos);
    EXPECT_EQ(forwarded.find("keep-alive:"), std::string::npos);
    EXPECT_NE(forwarded.find("connection: close\r\n"), std::string::npos);
}

TEST(HttpConnTest, ProxyForwardsPostBodyAndReturnsUpstreamResponse) {
    SocketPair sockets;
    set_nonblocking_or_fail(sockets.server_fd());

    LocalUpstreamServer upstream(
        "HTTP/1.1 201 Created\r\n"
        "Content-Length: 7\r\n"
        "Connection: close\r\n"
        "\r\n"
        "POST_OK");

    http_conn conn(sockets.server_fd());

    const std::string body = "a=1&b=2";
    const std::string request =
        "POST http://127.0.0.1:" + std::to_string(upstream.port()) +
        "/submit HTTP/1.1\r\n"
        "Host: 127.0.0.1:" + std::to_string(upstream.port()) +
        "\r\n"
        "Content-Length: " + std::to_string(body.size()) +
        "\r\n"
        "Proxy-Connection: keep-alive\r\n"
        "\r\n" + body;

    write_all_or_fail(sockets.peer_fd(), request);
    ASSERT_GT(conn.read_from_socket(), 0);
    ASSERT_TRUE(conn.parse_available_data());
    ASSERT_TRUE(conn.is_message_complete());

    http_proxy proxy(conn);
    ASSERT_TRUE(proxy.handle_request());

    const std::string response = read_http_message_with_timeout(sockets.peer_fd(), 2000);
    EXPECT_NE(response.find("HTTP/1.1 201 Created\r\n"), std::string::npos);
    EXPECT_NE(response.find("POST_OK"), std::string::npos);

    const std::string forwarded = upstream.take_received_request();
    EXPECT_NE(forwarded.find("POST /submit HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(forwarded.find("content-length: 7\r\n"), std::string::npos);
    EXPECT_EQ(forwarded.find("proxy-connection:"), std::string::npos);
    EXPECT_NE(forwarded.rfind("\r\n\r\na=1&b=2"), std::string::npos);
}

TEST(HttpConnTest, ProxyConnectEstablishesTunnelAndRelaysBidirectionalBytes) {
    SocketPair sockets;
    set_nonblocking_or_fail(sockets.server_fd());

    LocalTunnelUpstreamServer upstream;
    http_conn conn(sockets.server_fd());

    const std::string request =
        "CONNECT 127.0.0.1:" + std::to_string(upstream.port()) +
        " HTTP/1.1\r\n"
        "Host: ignored.example\r\n"
        "\r\n";
    write_all_or_fail(sockets.peer_fd(), request);

    ASSERT_GT(conn.read_from_socket(), 0);
    ASSERT_TRUE(conn.parse_available_data());
    ASSERT_TRUE(conn.is_message_complete());
    ASSERT_TRUE(conn.request().is_connect);

    bool proxy_ok = false;
    std::thread proxy_thread([&]() {
        http_proxy proxy(conn);
        proxy_ok = proxy.handle_request();
    });

    std::string client_read = read_until_contains_with_timeout(sockets.peer_fd(), "\r\n\r\n", 2000);
    EXPECT_NE(client_read.find("HTTP/1.1 200 Connection Established\r\n"), std::string::npos);

    write_all_or_fail(sockets.peer_fd(), "PING");

    client_read += read_until_contains_with_timeout(sockets.peer_fd(), "UP_ECHO:PING", 2000);
    EXPECT_NE(client_read.find("UP_HELLO"), std::string::npos);
    EXPECT_NE(client_read.find("UP_ECHO:PING"), std::string::npos);

    if (proxy_thread.joinable()) {
        proxy_thread.join();
    }
    EXPECT_TRUE(proxy_ok);

    const std::string tunnel_payload = upstream.take_client_bytes();
    EXPECT_NE(tunnel_payload.find("PING"), std::string::npos);
}