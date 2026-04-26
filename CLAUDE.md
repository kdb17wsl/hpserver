# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

```bash
# All tests
ctest --test-dir build --output-on-failure

# Single test binary
./build/tests/http_conn_test
./build/tests/http_response_parser_test
./build/tests/threadsafe_queue_test
./build/tests/thread_pool_test
./build/tests/bloom_filter_test
./build/tests/bitwise_trie_test
./build/tests/ip_filter_test

# Single test case via GTest filter
./build/tests/http_conn_test --gtest_filter=HttpConnTest.ParseRequest
```

Release build disables logging via `DISABLE_LOGGING` macro:
```bash
cmake -S . -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_release
```

## Architecture

Single Reactor main thread + dual Worker thread pools:
- **Main thread** (Reactor): socket accept, epoll event dispatch, connection lifecycle, timeout reaping. All epoll IO and `http_conn` state mutation happens here.
- **Proxy Workers**: pool for HTTP forward proxy requests (default: max(2, hw_concurrency)).
- **Tunnel Workers**: pool for HTTPS CONNECT tunnel tasks (default: max(32, hw_concurrency * 8)).
- **Completion path**: Workers post results via `threadsafe_queue` + `eventfd` notification back to the Reactor thread.

Entry point: `src/main.cpp` → `src/hpserver.h/.cpp` (event loop and connection management).

### Key modules

| Path | Role |
|------|------|
| `src/net/core/poller` | Epoll wrapper (ET mode) |
| `src/net/core/socket_ops` | RAII socket operations |
| `src/net/core/ip_filter` | IP blacklist via bitwise trie |
| `src/net/http/http_conn` | Connection state machine and buffering |
| `src/net/http/http_request_parser` | HTTP request parsing (llhttp) |
| `src/net/http/http_response_parser` | HTTP response parsing (chunked encoding) |
| `src/net/http/http_message_builder` | Response construction |
| `src/net/http/http_cache` | RocksDB-backed caching with Protobuf serialization |
| `src/net/http/http_connection_io` | I/O read/write paths |
| `src/net/proxy/http_proxy` | Forward proxy and CONNECT tunnel logic |
| `src/util/thread_pool` | Bounded thread pool (wait/non-wait stop) |
| `src/util/threadsafe_queue` | Lock-free queue for cross-thread communication |
| `src/util/timer` | Timer wheel for connection timeouts |
| `src/util/bitwise_trie` | IP prefix matching |
| `src/util/bloom_filter` | Probabilistic membership queries |

### Critical invariants

- `connections_` is indexed by client fd with MAX_FD (65536) boundary checks. Preserve direct-fd-indexing storage sizing.
- Under EPOLLET: read/write paths must drain until EAGAIN/EWOULDBLOCK. EINTR is retry, not error.
- Non-blocking accept loops: EAGAIN/EWOULDBLOCK are normal termination; retry on EINTR.
- Do not move epoll IO or http_conn state mutation off the reactor thread unless explicitly introducing synchronized ownership.

## Conventions

- C++20, Google style with 4-space indent, 100-column limit (`.clang-format`).
- Extend existing modules under `src/net/http` and `src/net/core` before introducing new abstractions.
- Prefer test-first for parser and connection behavior changes; add/update tests in `tests/http_conn_test.cpp` for protocol edge cases.

## Documentation

- `doc/loop.md` — Reactor loop and timer behavior
- `doc/http_conn.md` — HTTP connection state machine and buffer semantics
- `doc/proxy_agent_runbook.md` — Proxy implementation stages and validation commands
- `doc/agent.md` — Roadmap-level material; validate against src/ and tests/ before implementing
