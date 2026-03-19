#pragma once

#include <netinet/in.h>

#include <memory>
#include <vector>

#include "http_conn.h"
#include "poller.h"
#include "socket_ops.h"

const int MAX_FD = 65536;
const int MAX_EVENTS = 1024;
const int DEFAULT_PORT = 8080;

class hpserver {
public:
    hpserver(int port = DEFAULT_PORT) : port(port) {};
    ~hpserver() = default;

    int listen();

private:
    struct sockaddr_in server_addr;
    socket_ops server_socket;
    int port;

    poller poller_;
    struct epoll_event events[MAX_EVENTS];

    std::vector<std::unique_ptr<http_conn>> connections_;

    void init();
    bool set_nonblocking(int fd) const;
    void close_client(int client_fd);
    int handle_client(int client_fd);
};