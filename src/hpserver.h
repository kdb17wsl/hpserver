#pragma once

#include <netinet/in.h>

#include <memory>
#include <string>
#include <vector>

#include "http_conn.h"
#include "http_cache.h"
#include "ip_filter.h"
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
    hpserver(int port = DEFAULT_PORT,
             unsigned int proxy_workers = 0,
             unsigned int tunnel_workers = 0,
             std::size_t proxy_queue_size = 0,
             std::size_t tunnel_queue_size = 0);
    ~hpserver();

    int listen();

private:
    struct proxy_done_event {
        int client_fd = -1;
        bool ok = false;
        std::string response;
        bool close_after_done = false;
        bool is_connect = false;
        int err = 0;
    };

    struct sockaddr_in server_addr;
    socket_ops server_socket;
    int port;

    poller poller_;
    struct epoll_event events[MAX_EVENTS];

    std::vector<std::unique_ptr<http_conn>> connections_;
    unsigned int proxy_worker_count_ = 0;
    unsigned int tunnel_worker_count_ = 0;
    std::size_t proxy_queue_size_ = 0;
    std::size_t tunnel_queue_size_ = 0;
    thread_pool proxy_pool_;
    thread_pool tunnel_pool_;
    threadsafe_queue<proxy_done_event> proxy_done_queue_;
    std::vector<bool> proxy_inflight_;
    std::vector<bool> close_after_flush_;
    int proxy_event_fd_ = -1;
    http_cache cache_;
    timer connection_timer_;
    ip_filter ip_filter_;


    void init();
    bool init_cache_db();
    bool set_nonblocking(int fd) const;
    void close_client(int client_fd);
    int handle_client(int client_fd);
    int flush_client_output(int client_fd);
    bool init_proxy_async();
    bool submit_proxy_job(int client_fd, http_conn::request_info req);
    void drain_proxy_done_events();
    void refresh_client_timeout(int client_fd);
};