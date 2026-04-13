#include "hpserver.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
void print_usage(const char* program) {
    std::fprintf(stderr,
                 "Usage: %s [--port N] [--proxy-workers N] [--tunnel-workers N] "
                 "[--proxy-queue N] [--tunnel-queue N]\n",
                 program);
}

bool parse_unsigned(const char* text, unsigned long long& out) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }

    out = value;
    return true;
}
}

int main(int argc, char* argv[]) {
    int port = DEFAULT_PORT;
    unsigned int proxy_workers = 0;
    unsigned int tunnel_workers = 0;
    std::size_t proxy_queue_size = 0;
    std::size_t tunnel_queue_size = 0;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (i + 1 >= argc) {
            std::fprintf(stderr, "Missing value for argument: %s\n", arg);
            print_usage(argv[0]);
            return 1;
        }

        unsigned long long parsed = 0;
        if (!parse_unsigned(argv[i + 1], parsed)) {
            std::fprintf(stderr, "Invalid numeric value for %s: %s\n", arg, argv[i + 1]);
            return 1;
        }

        if (std::strcmp(arg, "--port") == 0) {
            if (parsed == 0 || parsed > 65535ULL) {
                std::fprintf(stderr, "Port out of range: %s\n", argv[i + 1]);
                return 1;
            }
            port = static_cast<int>(parsed);
        } else if (std::strcmp(arg, "--proxy-workers") == 0) {
            if (parsed > std::numeric_limits<unsigned int>::max()) {
                std::fprintf(stderr, "proxy-workers out of range: %s\n", argv[i + 1]);
                return 1;
            }
            proxy_workers = static_cast<unsigned int>(parsed);
        } else if (std::strcmp(arg, "--tunnel-workers") == 0) {
            if (parsed > std::numeric_limits<unsigned int>::max()) {
                std::fprintf(stderr, "tunnel-workers out of range: %s\n", argv[i + 1]);
                return 1;
            }
            tunnel_workers = static_cast<unsigned int>(parsed);
        } else if (std::strcmp(arg, "--proxy-queue") == 0) {
            if (parsed > std::numeric_limits<std::size_t>::max()) {
                std::fprintf(stderr, "proxy-queue out of range: %s\n", argv[i + 1]);
                return 1;
            }
            proxy_queue_size = static_cast<std::size_t>(parsed);
        } else if (std::strcmp(arg, "--tunnel-queue") == 0) {
            if (parsed > std::numeric_limits<std::size_t>::max()) {
                std::fprintf(stderr, "tunnel-queue out of range: %s\n", argv[i + 1]);
                return 1;
            }
            tunnel_queue_size = static_cast<std::size_t>(parsed);
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", arg);
            print_usage(argv[0]);
            return 1;
        }

        ++i;
    }

    hpserver server(port, proxy_workers, tunnel_workers, proxy_queue_size, tunnel_queue_size);
    if (server.listen() == -1) {
        perror("Failed to start server");
        return 1;
    }
    return 0;
}
