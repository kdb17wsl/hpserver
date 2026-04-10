# hpserver 核心架构与异步流程深度解析

`hpserver` 采用 **单线程 Reactor (主线程) + 动态 Worker 线程池** 的混合架构。其设计的核心在于通过 `proxy_inflight_` 标志位实现了文件描述符 (FD) 在 **事件驱动流** 与 **阻塞式业务流** 之间的原子级所有权切换。

---

## 1. 架构总览：线程分工与隔离 (Architecture)

### 1.1 角色定义 (Roles)
*   **Reactor 线程 (Main Thread)**:
    *   **核心逻辑**: 处理 `epoll_wait`、`accept`、`llhttp` 协议增量解析、定时器管理、以及最终的 `close_client`。
    *   **绝对禁止**: 执行任何可能导致阻塞的操作（如 DNS 查询、同步 Connect、或耗时的代理转发）。
*   **Worker 线程池 (Thread Pool)**:
    *   **核心逻辑**: 执行具体的代理业务（普通 HTTP 转发或 CONNECT 隧道透传）。
    *   **操作方式**: 模拟同步阻塞流程以简化业务逻辑，完成后通过 `eventfd` 异步通知 Reactor。

### 1.2 所有权移交：`proxy_inflight_` (The Token)
这是系统最关键的设计细节，用于在无锁情况下保证线程安全：
1.  **Reactor 拥有权**: 初始状态下，客户端 FD 在 `epoll` 中受 Reactor 监控。
2.  **移交**: 当解析出完整的 HTTP 请求时，Reactor 设置 `proxy_inflight_[fd] = true`，并将 FD 相关的 `proxy_job` 任务提交给线程池。
3.  **屏蔽风险**: 主循环在处理 `client_fd` 的 `EPOLLIN` 事件时，会检查 `proxy_inflight_`。如果是 `true`，则 **直接跳过** 对该 FD 的读取和协议解析，防止主、从线程同时操作同一个 Socket 引发竞争。
4.  **归还**: Worker 完成任务后，将结果压入队列并写 `eventfd`。Reactor 在 `drain_proxy_done_events` 中将其重置为 `false`，重新接管 FD。

---

## 2. 代理转发全链路追踪 (Proxy Path)

### 2.1 普通 HTTP 转发 (Request-Response)
- **Worker 阶段**: 线程池获取任务 -> `connect_upstream` -> 发送请求 -> 接收完整响应 -> 将响应数据块缓存进 `std::string`。
- **回流阶段**: 通过 `eventfd` 唤醒 Reactor。Reactor 将响应数据写入 `http_conn::queue_write_`。
- **刷出阶段**: Reactor 依赖 `flush_client_output` 异步发送数据至客户端。

### 2.2 CONNECT 隧道：双向数据泵 (Bidirectional Pump)
当解析到 `CONNECT` 方法时，逻辑发生显著变化：
1.  **握手**: Worker 连接目标服务器成功后，手动向客户端发回 `HTTP/1.1 200 Connection Established\r\n\r\n`。
2.  **独立 Poll**: Worker 线程进入死循环，调用 **私有 `::poll()`** 同时监听 `client_fd` 和 `upstream_fd`。
3.  **字节透传**: 
    - `client` 有数据 -> `recv` -> `upstream` -> `send` (阻塞/非阻塞重试)。
    - `upstream` 有数据 -> `recv` -> `client` -> `send`。
4.  **收尾**: 任意一方关闭 (FIN) 时，Worker 退出循环，标记 `close_after_done = true` 并唤醒 Reactor。

---

## 3. 安全退出：`close_after_flush_` (Lazy Close)

为了保证响应数据“完整落袋”，不因 Worker 提前通知结束而丢失尚未发完的缓冲区数据：

1.  **条件触发**: Worker 完成任务后可能带回关闭指令（如隧道结束或 `Connection: close`）。
2.  **标记延迟**: Reactor 检查 `write_buffer`。若不为空，不立即 `close`，而是设置 `close_after_flush_[fd] = true` ([src/hpserver.cpp](../src/hpserver.cpp#L173))。
3.  **感知关闭**: 在 `flush_client_output` 中，每次成功写回数据后都会检查：若 `buffer.empty()` 且 `close_after_flush_` 为真，则执行物理销毁 `close_client(fd)`。

---

## 4. 定时器轮与超时管理 (Timer & Timeout)

基于最小堆 (Min-Heap) 实现的高效定时器，用于清理僵死连接。

- **惰性更新 (Lazy Update)**: 当 FD 有数据交互时，不实时删除旧的定时器节点，而是分配一个递增的 `version_id` 并在堆中插入新节点 ([src/util/timer.h](../src/util/timer.h))。
- **校验清理**: 当堆顶过期时，对比节点版本。若版本已过期（不是最新版本），直接丢弃，仅对匹配最新版本的连接执行 `close_client`。
- **休眠对齐**: `poll.wait()` 的超时参数取自 `connection_timer_.get_next_timeout_ms()`，确保 Reactor 在最闲时也能准时醒来清理资源。

---

## 5. 全链路时序表 (Sequence)

1.  **Accept**: `accept4` 接入，初始化 `http_conn`，注册 `EPOLLET`。
2.  **Decode**: `llhttp` 流式解析，存储请求头、URL。
3.  **In-flight**: 解析完成，锁定 FD 所有权，移交 Worker。
4.  **Execution**: Worker 阻塞式完成网络 IO 或隧道透传。
5.  **Return**: `eventfd` 通知 Reactor 业务结果。
6.  **Notify**: 剥离 In-flight 状态，数据注入 `queue_write_`。
7.  **Finalize**: `close_after_flush` 确保缓冲区刷干，物理销毁 FD。
