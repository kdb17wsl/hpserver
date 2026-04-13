# hpserver 运行流程说明


当前实现采用 **单 Reactor 主线程 + 固定大小 Worker 线程池（有界队列）** 的结构：
- 主线程负责 `epoll` 事件分发、连接状态、超时回收与最终关闭。
- Worker 线程负责上游代理逻辑（普通 HTTP 转发和 CONNECT 隧道）。
- 两边通过 `proxy_inflight_`、完成队列、`eventfd` 协作。

> 术语提示
> - “协作切换”是准确说法：同一时刻尽量只有一侧在处理某个客户端 fd。
> - 这不是严格意义上的“原子所有权协议”，而是工程上可控的门控方案。

---

## 0. 先建立一个最小心智模型（30 秒版）

把 hpserver 看成 4 个子系统：
1. **接入与事件循环**：监听 socket、`epoll_wait`、事件分发。
2. **连接状态容器**：`connections_`、`proxy_inflight_`、`close_after_flush_` 三组数组按 fd 直接索引。
3. **异步代理协作**：主线程投递任务到 worker，worker 通过完成队列 + `eventfd` 回传结果。
4. **生命周期管理**：定时器刷新/过期、错误分支、延迟关闭与最终回收。

只要抓住一点，后面就不容易迷路：
- **主线程决定“连接状态”**。
- **worker 负责“代理执行”**。
- **`proxy_inflight_` 决定“当前谁能碰这个 fd”**。

---

## 1. 先看全局：线程职责与边界

### 1.1 Reactor 主线程做什么
- 监听与接入：`socket/bind/listen/accept`。
- 事件循环：`epoll_wait` 分发 `EPOLLIN/EPOLLOUT/ERR/HUP/RDHUP`。
- 协议入口：读取客户端数据并推进 HTTP 解析。
- 状态管理：更新 `proxy_inflight_`、`close_after_flush_`、定时器。
- 生命周期收尾：`close_client` 统一回收连接。

### 1.2 Worker 线程做什么
- 普通 HTTP 代理：连上游、发请求、读完整响应。
- CONNECT 隧道：建立 200 握手后做双向透传，直到一侧结束或异常。
- 结果回传：把结果写入完成队列，并写 `eventfd` 唤醒主线程。

### 1.3 三个核心状态数组各管什么
- `connections_[fd]`：fd 对应的 `http_conn` 对象，包含读缓冲、写缓冲、解析器状态。
- `proxy_inflight_[fd]`：该连接是否已交给 worker 处理中；`true` 时主线程跳过 `handle_client`。
- `close_after_flush_[fd]`：是否需要“写完再关”；用于避免响应截断。

### 1.4 连接生命周期中的“单向原则”
同一时刻尽量只允许一侧推进连接：
- 主线程阶段：读客户端、解析、投递任务、回写客户端、关闭。
- worker 阶段：执行上游代理，结束后仅回传结果，不直接改主线程内部状态数组。

这样做的好处是：定位问题时只需先判断“当前连接在哪一侧”。

### 1.5 为什么 `handle_client` 会“跳过连接”
当连接已经被提交给 worker 后，主线程如果继续读/解析同一个 fd，就会与 worker 并发操作 socket，导致竞态。为此：
1. 请求完整后主线程置 `proxy_inflight_[fd] = true` 并投递任务。
2. 后续该 fd 再触发 `EPOLLIN`，`handle_client` 直接返回。
3. worker 完成后主线程在 `drain_proxy_done_events` 把该标志清零，重新接管。

这个门控是本项目并发安全的核心。

---

## 2. 启动到事件循环：系统如何“跑起来”

### 2.1 初始化阶段
`listen()` 里依次完成：
1. `init()`：初始化日志、IP 过滤器、连接容器、标志数组。
2. 监听 socket 设为非阻塞。
3. `bind + listen` 开始对外服务。
4. 监听 fd 注册到 `epoll`（`EPOLLIN | EPOLLET`）。
5. 初始化 `eventfd` 并注册到同一个 `epoll`，用于 worker -> reactor 异步唤醒。

初始化后的关键不变量：
- `connections_.size() == MAX_FD`。
- `proxy_inflight_` 与 `close_after_flush_` 长度均为 `MAX_FD`，默认 false。
- server fd 和 eventfd 都已加入 epoll 监控。

### 2.2 主循环框架
主循环每轮做三件事：
1. 计算本轮 `epoll_wait` 超时（来自定时器最近过期时间）。
2. 处理本轮所有就绪事件。
3. 调用 `connection_timer_.tick()` 触发过期回调。

### 2.3 主循环每个分支的执行顺序
对每个就绪事件，主线程按以下优先级处理：
1. **监听 fd 分支**：进入 accept 循环，直到 `EAGAIN/EWOULDBLOCK`。
2. **eventfd 分支**：批量处理 worker 完成事件。
3. **客户端 fd 分支**：
	- 先看 `EPOLLERR/HUP/RDHUP`，有就直接关闭。
	- 再根据 `EPOLLIN/EPOLLOUT` 刷新超时。
	- 若有 `EPOLLOUT` 先尝试 flush（可能触发延迟关闭）。
	- 若有 `EPOLLIN` 再走 `handle_client`。

这个顺序保证了两个目标：
- 先处理致命错误，快速回收坏连接。
- 先处理写出再处理读入，避免写缓冲长期堆积。

---

## 3. 一条普通 HTTP 请求的完整路径

### 3.1 接入与读入
1. `accept` 新连接，设置非阻塞，注册客户端事件。
2. 创建 `http_conn`，并为该 fd 添加空闲超时定时器。
3. 客户端 `EPOLLIN` 到来后，进入 `handle_client`。

细节补充：
- accept 循环会把 `EINTR` 当作可重试。
- 黑名单命中会直接回 403 并关闭，不进入常规连接管理。

### 3.2 解析与投递
1. `read_from_socket()` 在 ET 模式下持续读到 `EAGAIN/EWOULDBLOCK`。
2. `parse_available_data()` 做增量 HTTP 解析。
3. 若消息未完整，返回等待下次可读事件。
4. 若消息完整，提取请求，`reset_for_next_message()`，置 `proxy_inflight_ = true`，提交 worker。

`handle_client` 的返回语义：
- 返回 `0`：这轮没有致命错误（包括“数据不完整，继续等”）。
- 返回 `-1`：本连接出现错误，调用方会关闭连接。

### 3.3 worker 执行代理
1. 连接上游。
2. 发送转发后的请求。
3. 接收上游响应（按头部与 body 边界判定完整性）。
4. 将结果塞入完成队列并写 `eventfd`。

异常语义：
- 若任务失败，会携带 errno 并回流。
- 若是 CONNECT 的“对端正常收尾型错误”（如 reset/broken pipe），会按预期结束处理，避免误报。

### 3.4 回流与回写
1. 主线程收到 `eventfd` 事件，调用 `drain_proxy_done_events()`。
2. 清理 `proxy_inflight_[fd]`，把响应写入连接发送缓冲。
3. 调用 `flush_client_output()` 尽量立即发送；若暂时写不完则关注 `EPOLLOUT`。

成功/失败回流分叉：
- **成功**：写响应 -> flush -> 根据 `close_after_done` 决定立即关或延迟关。
- **失败**：通常回 503（含错误信息）-> flush -> 关闭（或延迟关闭）。

---

## 4. CONNECT 隧道路径（与普通 HTTP 的关键差异）

CONNECT 不走“请求-响应一次性返回”，而是进入“持续双向透传”：
1. worker 连上目标主机后，先向客户端写 `200 Connection Established`。
2. worker 使用 `poll` 同时观察 `client_fd` 与 `upstream_fd`。
3. 两方向数据分别做 `recv -> send`，处理 `EINTR/EAGAIN`。
4. 任一方向 EOF/异常后，进行半关闭或收尾，返回完成事件。

回到主线程后通常会设置 `close_after_done` 路径，让连接按收尾策略关闭。

再细一点的行为：
- 如果客户端先 EOF，worker 会对上游执行 `shutdown(SHUT_WR)`，让上游感知“我不再发送”。
- 如果上游先 EOF，worker 会对客户端执行 `shutdown(SHUT_WR)`，对称收尾。
- 两方向缓冲都清空后才算隧道完整结束。

这使 CONNECT 不只是“能通”，而是具备基本的半关闭传播能力。

---

## 5. http_proxy 模块细节（按函数拆解）

这一章专门解释 worker 线程里真正执行代理的代码，入口在：
- `forward_request(...)`：普通 HTTP 转发。
- `forward_connect_tunnel(...)`：CONNECT 隧道透传。

### 5.1 `connect_upstream(host, port, upstream)`
作用：建立到上游服务的非阻塞连接，并把可用 socket 返回给调用方。

执行步骤：
1. `getaddrinfo` 解析域名，可能得到多个地址。
2. 逐个地址尝试 `connect`：
	- 立即成功：直接返回。
	- `EINPROGRESS`：进入可写等待。
3. 对 `EINPROGRESS` 场景，调用 `wait_fd(fd, EPOLLOUT)` 等待连接完成。
4. 使用 `getsockopt(SO_ERROR)` 判断是否真的连通。
5. 所有地址都失败则返回 false。

你可以把它理解为“带超时控制的多地址非阻塞拨号器”。

### 5.2 `wait_fd(fd, events)`
作用：在 worker 里等待某个 fd 变为可读/可写。

实现要点：
- 内部临时创建一个 poller（epoll 包装），把目标 fd 加入并等待 `kIoTimeoutMs`。
- `EINTR` 会重试。
- 超时会设置 `errno = ETIMEDOUT`。

这让 `forward_request` 和 `send_all_nonblocking` 可以统一用“等待可读/可写 + 继续推进”的风格。

### 5.3 `forward_request(req, out_response, out_errno)`
作用：执行一次完整 HTTP 上游转发并把响应拼成字符串。

关键流程：
1. 通过 `build_forward_request` 构造转发报文（来自 HTTP message builder）。
2. `connect_upstream` 建连。
3. `send_all_nonblocking_socket` 把请求完整发给上游。
4. 循环接收响应，并在读取过程中解析响应边界：
	- 识别 header 结束位置。
	- 识别 `Content-Length`。
	- 识别 `Transfer-Encoding: chunked`。
5. 命中“响应完整”条件后返回成功。

响应完整判定规则：
- 有 `Content-Length`：收到字节数达到 header + body 目标长度。
- `chunked`：`chunked_body_complete(...)` 判定最后 0 块及尾部结束。
- 其他情况：通常等待对端 EOF。

重试策略：
- 首次失败若 errno 是 `ETIMEDOUT` 或 `ECONNRESET`，会自动重试 1 次。
- 其余错误直接失败回流。

部分读取超时策略：
- 若已收到“部分有效响应”（例如 Content-Length 未收满、chunked 未结束），遇到读超时会允许有限次数重试（`kMaxPartialReadTimeoutRetries`），减少误判。

### 5.4 `forward_connect_tunnel(client_fd, req, out_errno)`
作用：执行 CONNECT 建隧道与双向字节透传。

关键流程：
1. 校验参数，默认端口回退到 443（若请求未带端口）。
2. 建立到目标服务器的上游连接。
3. 向客户端发送 `200 Connection Established`。
4. 进入双向循环：
	- 先 flush 两个方向的待发送缓冲。
	- 再用 `poll` 同时监听 client 与 upstream 的可读/可写。
	- 读到数据后追加到对应方向缓冲，下一轮继续 flush。
5. 处理 EOF 半关闭传播：
	- client EOF -> upstream `shutdown(SHUT_WR)`。
	- upstream EOF -> client `shutdown(SHUT_WR)`。
6. 双向都 EOF 且缓冲清空时，返回成功。

失败条件示例：
- `poll` 超时。
- `POLLERR/POLLHUP/POLLNVAL`。
- 任一方向 `recv/send` 硬错误。

### 5.5 `send_all_nonblocking(...)` 与 `flush_nonblocking(...)`
- `send_all_nonblocking`：用于“必须一次逻辑上发完”的报文（如 CONNECT 200 响应）。
- `flush_nonblocking`：用于隧道循环中的“缓冲区增量刷出”，支持部分发送与 offset 续传。

二者都遵循：
- `EINTR` 重试。
- `EAGAIN/EWOULDBLOCK` 视为暂不可写，等待下一次推进。

### 5.6 为什么要在 http_proxy 内做响应边界解析
如果不解析上游响应边界，普通代理容易出现两类问题：
1. **过早返回**：body 还没收完就结束，导致截断。
2. **无限等待**：明明已收满却继续等 EOF，导致延迟增大。

当前实现通过 `Content-Length/chunked` 双路径判定，显著降低这两类问题。

---

## 6. 为什么需要 `close_after_flush_`

很多任务完成时“业务上可关闭”，但内核发送缓冲未必已经发完。若立即关闭，客户端可能只收到半包。

因此实现采用延迟关闭：
1. 如果有待发送数据，先标记 `close_after_flush_[fd] = true`。
2. 保留连接直到 `flush_client_output()` 把缓冲区清空。
3. 缓冲清空后再执行 `close_client(fd)`。

这保证“先发完，再关闭”。

`flush_client_output()` 的关键行为：
- 循环调用 `flush_to_socket()` 直到写空或写不动。
- 若遇到 `EAGAIN/EWOULDBLOCK`，把该 fd 改为监听 `EPOLLOUT`，等待下次可写继续。
- 写空后把事件兴趣恢复为默认客户端事件集合（不再持续监听 `EPOLLOUT`）。

---

## 7. 超时管理：版本化最小堆定时器

定时器目标是清理长时间空闲连接，同时避免大规模连接下的高删除成本。

核心策略：
- 每次 `add/adjust` 都推入新节点并递增版本号（惰性更新）。
- 堆顶弹出时校验版本；旧版本视为陈旧节点直接丢弃。
- `get_next_timeout_ms()` 与 `epoll_wait` 对齐，保证最迟在最近超时点被唤醒。

这个方案在高并发下更稳定，避免频繁“堆中间删除”。

连接与定时器的交互规则：
- 新连接创建后 `add`。
- 收到 `EPOLLIN/EPOLLOUT` 活动后刷新（`adjust`）。
- 连接关闭时 `remove`。
- 到期回调中再次校验连接仍有效，再执行关闭。

---

## 8. 事件分支速查（条件 -> 行为）

| 条件 | 行为 |
|---|---|
| 监听 fd 可读 | 循环 `accept` 到 `EAGAIN`，创建并注册新连接 |
| `eventfd` 可读 | 批量 `drain_proxy_done_events`，处理 worker 结果 |
| 客户端 `EPOLLERR/HUP/RDHUP` | 直接 `close_client` |
| 客户端 `EPOLLOUT` | `flush_client_output`，必要时触发延迟关闭 |
| 客户端 `EPOLLIN` 且 `proxy_inflight_==false` | `handle_client` 读 + 解析 + 可能投递 worker |
| 客户端 `EPOLLIN` 且 `proxy_inflight_==true` | 跳过处理，等待 worker 回流 |

---

## 9. 关键函数速查（输入、输出、副作用）

### 9.1 `handle_client(fd)`
- 输入：一个已存在连接 fd。
- 输出：`0/-1`。
- 副作用：可能读取 socket、推进解析、重置解析状态、置 `proxy_inflight_`、投递 worker。

### 9.2 `submit_proxy_job(fd, req)`
- 输入：客户端 fd 与已解析请求。
- 输出：是否投递成功。
- 副作用：向线程池入队；失败时也会构造失败完成事件并唤醒主线程。

### 9.3 `drain_proxy_done_events()`
- 输入：完成队列中的任务结果。
- 输出：无。
- 副作用：清理 in-flight、写入响应、flush、设置延迟关闭或直接关闭。

### 9.4 `flush_client_output(fd)`
- 输入：客户端 fd。
- 输出：`0/-1`。
- 副作用：修改 epoll 关注事件（是否关注 `EPOLLOUT`）。

### 9.5 `close_client(fd)`
- 输入：客户端 fd。
- 输出：无。
- 副作用：移除定时器、清理状态位、从 epoll 移除、销毁连接对象。

---

## 10. 错误与收尾：最常见 6 类场景

1. **客户端解析错误**：`handle_client` 返回 `-1`，主循环关闭连接。
2. **线程池饱和**：`try_push` 失败，回流失败事件，主线程返回 503 并收尾。
3. **上游连接失败**：worker 回流错误，主线程返回 503。
4. **客户端写阻塞**：挂 `EPOLLOUT`，下一轮继续 flush。
5. **CONNECT 对端断开**：按预期结束隧道并关闭。
6. **空闲超时**：定时器回调触发关闭。

这些场景覆盖了线上最常见的关闭路径来源。

---

## 11. 新同学建议的阅读顺序

建议按这个顺序看代码，最快建立心智模型：
1. `src/main.cpp`：程序入口。
2. `src/hpserver.h`：核心成员变量（特别是三个数组/队列/定时器）。
3. `src/hpserver.cpp`：主循环、`handle_client`、`submit_proxy_job`、`drain_proxy_done_events`。
4. `src/net/http/http_conn.*`：连接读写与解析粘合。
5. `src/net/proxy/http_proxy.*`：普通代理与 CONNECT 细节。
6. `src/util/timer.h`：版本化最小堆实现。

---

## 12. 当前实现边界（避免误解）

- 线程池是“固定线程数 + 有界队列”，不是运行时自动扩缩容。
- 主循环使用 `epoll` ET 模式；CONNECT 透传在 worker 中使用 `poll`。
- `proxy_inflight_` 是门控协作机制，目标是降低竞态，不应过度表述为严格原子所有权协议。

理解这些边界后，你会更容易区分“现有能力”与“后续可演进方向”。
