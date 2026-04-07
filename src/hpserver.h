#pragma once

#include <netinet/in.h>

#include <memory>
#include <string>
#include <vector>

#include "http_conn.h"
#include "poller.h"
#include "socket_ops.h"
#include "timer.h"
#include "thread_pool.h"
#include "threadsafe_queue.h"

const int MAX_FD = 65536;
const int MAX_EVENTS = 1024;
const int DEFAULT_PORT = 8080;

class hpserver {
public:
    hpserver(int port = DEFAULT_PORT) : port(port) {};
    ~hpserver();

    int listen();

private:
    struct proxy_done_event {
        int client_fd = -1;
        bool ok = false;
        std::string response;
        bool close_after_done = false;
        int err = 0;
    };

    struct sockaddr_in server_addr;
    socket_ops server_socket;
    int port;

    poller poller_;
    struct epoll_event events[MAX_EVENTS];

    std::vector<std::unique_ptr<http_conn>> connections_;
    thread_pool proxy_pool_;
    threadsafe_queue<proxy_done_event> proxy_done_queue_;
    std::vector<bool> proxy_inflight_;
    int proxy_event_fd_ = -1;
    timer connection_timer_;

    void init();
    bool set_nonblocking(int fd) const;
    void close_client(int client_fd);
    int handle_client(int client_fd);
    int flush_client_output(int client_fd);
    bool init_proxy_async();
    void submit_proxy_job(int client_fd, http_conn::request_info req);
    void drain_proxy_done_events();
    void refresh_client_timeout(int client_fd);
};