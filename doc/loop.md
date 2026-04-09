# hpserver 核心架构与异步逻辑深度解析

`hpserver` 采用 **单线程 Reactor + 动态 Worker 线程池** 的混合架构。其设计的精髓在于通过 `proxy_inflight_` 标志位实现了文件描述符（FD）在 **事件驱动流** 与 **阻塞任务流** 之间的原子级所有权切换。

## 1. 核心组件与交互模型 (Architecture)

### 1.1 线程分工
*   **Reactor 线程 (Main Thread)**:
    *   **职责**: 处理 `epoll_wait`、`accept`、`llhttp` 协议解析、定时器管理、以及最终的 `close_client`。
    *   **原则**: 绝对不执行任何可能导致阻塞的操作（如远程 DNS 查询、连接上游服务器）。
*   **Worker 线程池 (Thread Pool)**:
    *   **职责**: 执行具体的代理逻辑（`forward_request` 或 `forward_connect_tunnel`）。
    *   **原则**: 模拟同步阻塞流程以简化业务开发，完成后通过 `eventfd` 异步通知 Reactor。

### 1.2 FD 所有权移交 (The In-flight Token)
这是系统最关键的设计细节：
1.  **Reactor 拥有权**: 初始状态下，客户端 FD 在 `epoll` 中受 Reactor 监控。
2.  **移交**: 当解析出完整的 HTTP 请求时，Reactor 设置 `proxy_inflight_[client_fd] = true`，并提交任务给线程池。
3.  **屏蔽**: 主循环在处理 `client_fd` 的 `EPOLLIN` 事件时，会检查 `proxy_inflight_`。如果是 `true`，则 **直接跳过** 对该 FD 的读取和解析，防止 Reactor 与 Worker 同时操作同一个 Socket。
4.  **归还**: Worker 完成任务后，通过 `proxy_event_fd_` 唤醒 Reactor。Reactor 在 `drain_proxy_done_events` 中重置标志位，重新接管 FD。

---

## 2. CONNECT 隧道：双向数据泵 (Data Pump)

与普通 HTTP 转发不同，`CONNECT` 方法要求建立一个透明的 TCP 管道。

### 2.1 建立流程 (http_proxy.cpp)
1.  **识别**: `llhttp` 回调检测到 `CONNECT` 方法。
2.  **推派**: Reactor 将任务丢给 Worker 线程。
3.  **上游连接**: Worker 调用 `socket_ops::connect_at_idx` 同步连接目标服务器。
4.  **握手确认**: Worker 手动构造 `HTTP/1.1 200 Connection Established\r\n\r\n` 并通过 `socket_ops::send` 发回给客户端。

### 2.2 循环泵 (The Loop)
Worker 线程进入一个 `while(true)` 循环，使用 **独立的 `::poll`** 系统调用：
*   **多路监听**: 同时监听 `client_fd` 和 `upstream_fd`。
*   **字节透传**: 
    *   `client` 有数据 -> `recv` -> `upstream` -> `send`。
    *   `upstream` 有数据 -> `recv` -> `client` -> `send`。
*   **退出条件**: 任何一方关闭连接（`recv` 返回 0）或出现致命错误。

---

## 3. 延迟关闭：安全着陆 (Lazy Close)

在代理模式下，Worker 线程可能在瞬间生成大量响应数据，而客户端 Socket 缓存已满。

### 3.1 `close_after_flush_` 机制
当 Worker 任务完成时，它可能带有 `close_after_done` 标志（例如 CONNECT 隧道结束或 `Connection: close` 请求）。

1.  **进入队列**: Reactor 从完成队列读取事件。
2.  **状态判定**:
    *   若 `client_fd` 的输出缓冲区为空，直接调用 `close_client`。
    *   若缓冲区尚有残留，设置 `close_after_flush_[client_fd] = true`。
3.  **渐进写回**: 在后续的 `EPOLLOUT` 事件中，`flush_client_output` 会持续尝试清空数据。
4.  **最终清理**: 只有当缓冲区彻底归零 **且** `close_after_flush_` 为真时，连接才会被物理释放 (hpserver.cpp)。这保证了响应数据的“完整落袋”。

---

## 4. 定时器与超时管理 (Timer System)

服务器使用一个 **基于最小堆 (Min-Heap)** 的定时器轮，确保大量连接下的扫描效率。

### 4.1 惰性更新 (Lazy Update)
为了避免频繁调整堆结构：
*   **Version 机制**: 每个连接在定时器中关联一个 `version_id`。
*   **调整**: 当连接有活动需要重置超时时，只需在 `unordered_map` 中更新版本号并插入一个新节点到堆中。
*   **清理**: 当堆顶元素由于时间到期弹出时，校验其版本号。如果不是当前最新版本，直接丢弃（该节点已被更新的操作“作废”）。

---

## 5. 请求全生命周期溯源 (Sequence)

1.  **[Reactor] Accept**: 接受连接，初始化 `http_conn`，注册 `epoll`，开启 `connection_timer_`。
2.  **[Reactor] Decode**: 累积 `queue_read` 数据，`llhttp` 增量流式解析。
3.  **[Reactor] Hand-off**:
    *   普通 HTTP: 提取 `url`, `headers`, `body` 到 `proxy_job` 对象。
    *   CONNECT: 捕获目标 `host` 和 `port`。
    *   **设置 `proxy_inflight_ = true`** 并提交。
4.  **[Worker] Proxy Execute**:
    *   执行阻塞式连接与 IO。
    *   普通 HTTP 响应存入线程局部的 `std::string` 缓冲区。
5.  **[Worker] Notify**: 向 `proxy_done_queue_` 压入结果并写 `eventfd`。
6.  **[Reactor] Drain Notification**:
    *   **重置 `proxy_inflight_ = false`**。
    *   将结果写入 `queue_write`。
    *   根据需要触发 `close_after_flush_`。
7.  **[Reactor] Finalize**: `flush_client_output` 清空缓冲，移除定时器，回收资源。

---

### 技术亮点提示
*   **零拷贝趋势**: 虽然目前使用 `std::string` 作为中转，但 `socket_ops::send` 结合 `queue_write` 的流式处理为未来引入 `sendfile` 或 `splice` 留下了空间。
*   **边缘触发 (ET)**: `kClientEvents` 使用了 `EPOLLET`，要求读写必须循环至 `EAGAIN`，代码在 `socket_ops::recv` 和 `socket_ops::send` 中严格遵循了这一规范。