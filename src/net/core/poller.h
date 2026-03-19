#pragma once

#include <sys/epoll.h>
#include <unistd.h>

#include <cstdint>

class poller {
private:
    int epoll_fd_;

    /**
     * @brief Wrapper around epoll_ctl for add/modify/delete operations.
     *
     * Uses the current epoll instance to apply control operations to a target fd.
     * For EPOLL_CTL_ADD and EPOLL_CTL_MOD, @p events is written to epoll_event::events.
     * For EPOLL_CTL_DEL, the event pointer is passed as nullptr and @p events is ignored.
     *
     * @param op epoll control operation: EPOLL_CTL_ADD, EPOLL_CTL_MOD, or EPOLL_CTL_DEL.
     * @param fd Target file descriptor to control.
     * @param events Interested event mask (e.g. EPOLLIN | EPOLLET).
     * @return 0 on success, -1 on failure with errno set by epoll_ctl.
     */
    int control(int op, int fd, std::uint32_t events) const;

public:
    poller();
    ~poller();

    poller(const poller&) = delete;
    poller& operator=(const poller&) = delete;

    poller(poller&& other) noexcept;
    poller& operator=(poller&& other) noexcept;

    int add(int fd, std::uint32_t events) const;
    int modify(int fd, std::uint32_t events) const;
    int remove(int fd) const;

    int wait(struct epoll_event* events, int max_events, int timeout_ms) const;

    int get_fd() const { return epoll_fd_; }
    bool valid() const { return epoll_fd_ != -1; }
};
