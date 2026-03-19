#include "poller.h"

#include <cerrno>
#include <cstdio>
#include <utility>

poller::poller() : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (epoll_fd_ == -1) {
        perror("epoll_create1");
    }
}

poller::~poller() {
    if (epoll_fd_ != -1) {
        if (::close(epoll_fd_) == -1) {
            perror("close(epoll_fd)");
        }
    }
}

poller::poller(poller&& other) noexcept : epoll_fd_(other.epoll_fd_) {
    other.epoll_fd_ = -1;
}

poller& poller::operator=(poller&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (epoll_fd_ != -1 && ::close(epoll_fd_) == -1) {
        perror("close(epoll_fd)");
    }

    epoll_fd_ = other.epoll_fd_;
    other.epoll_fd_ = -1;
    return *this;
}

int poller::control(int op, int fd, std::uint32_t events) const {
    if (epoll_fd_ == -1) {
        errno = EBADF;
        return -1;
    }

    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    struct epoll_event* ev_ptr = (op == EPOLL_CTL_DEL) ? nullptr : &ev;
    int ret = ::epoll_ctl(epoll_fd_, op, fd, ev_ptr);
    if (ret == -1) {
        switch (op) {
            case EPOLL_CTL_ADD:
                perror("epoll_ctl(ADD)");
                break;
            case EPOLL_CTL_MOD:
                perror("epoll_ctl(MOD)");
                break;
            case EPOLL_CTL_DEL:
                perror("epoll_ctl(DEL)");
                break;
            default:
                perror("epoll_ctl");
                break;
        }
    }
    return ret;
}

int poller::add(int fd, std::uint32_t events) const {
    return control(EPOLL_CTL_ADD, fd, events);
}

int poller::modify(int fd, std::uint32_t events) const {
    return control(EPOLL_CTL_MOD, fd, events);
}

int poller::remove(int fd) const { return control(EPOLL_CTL_DEL, fd, 0); }

int poller::wait(struct epoll_event* events, int max_events, int timeout_ms) const {
    if (epoll_fd_ == -1) {
        errno = EBADF;
        return -1;
    }

    if (events == nullptr || max_events <= 0) {
        errno = EINVAL;
        return -1;
    }

    int ret = ::epoll_wait(epoll_fd_, events, max_events, timeout_ms);
    if (ret == -1 && errno != EINTR) {
        perror("epoll_wait");
    }
    return ret;
}
