这份实现的核心可以概括成一句话：

主线程只负责 epoll 驱动的连接接入、HTTP 读写和连接状态维护；真正的上游转发交给线程池执行，完成后再通过 eventfd 唤醒主线程回写响应。

它可以拆成 3 条协作链路：

1. 监听与连接管理链路，负责 accept、close 和超时回收。
2. 客户端读写链路，负责 EPOLLIN 和 EPOLLOUT 下的数据收发。
3. 代理异步链路，负责线程池转发、完成队列和 eventfd 通知。

## 1. 关键状态与约束

相关常量定义在 [src/hpserver.cpp](../src/hpserver.cpp#L14) 到 [src/hpserver.cpp](../src/hpserver.cpp#L16)。其中：

- kClientEvents = EPOLLIN | EPOLLET | EPOLLRDHUP，表示客户端连接采用边沿触发，并关注对端半关闭。
- kClientIdleTimeoutMs = 15000，表示空闲 15 秒后回收连接。

资源释放也很明确：析构函数只负责关闭 proxy_event_fd_，避免 eventfd 泄漏，见 [src/hpserver.cpp](../src/hpserver.cpp#L19) 到 [src/hpserver.cpp](../src/hpserver.cpp#L24)。

## 2. 启动流程

入口是 [src/hpserver.cpp](../src/hpserver.cpp#L190) 的 listen()。

启动顺序是固定的：

1. 调用 init()，初始化连接数组、代理 inflight 标记和监听 socket，见 [src/hpserver.cpp](../src/hpserver.cpp#L74) 到 [src/hpserver.cpp](../src/hpserver.cpp#L84)。
2. 把监听 socket 设为非阻塞，见 [src/hpserver.cpp](../src/hpserver.cpp#L193) 到 [src/hpserver.cpp](../src/hpserver.cpp#L195) 和 [src/hpserver.cpp](../src/hpserver.cpp#L26) 到 [src/hpserver.cpp](../src/hpserver.cpp#L39)。
3. 执行 bind() 和 listen()，见 [src/hpserver.cpp](../src/hpserver.cpp#L197) 到 [src/hpserver.cpp](../src/hpserver.cpp#L202)。
4. 把监听 fd 加入 epoll，监听 EPOLLIN | EPOLLET，见 [src/hpserver.cpp](../src/hpserver.cpp#L204) 到 [src/hpserver.cpp](../src/hpserver.cpp#L206)。
5. 初始化 eventfd，并把它也注册到 epoll 中，用于代理任务完成后的唤醒，见 [src/hpserver.cpp](../src/hpserver.cpp#L86) 到 [src/hpserver.cpp](../src/hpserver.cpp#L99)。

任一步失败都会直接返回 -1，因此 listen() 是一个严格的失败即退出入口。

## 3. 主循环怎么跑

主循环在 [src/hpserver.cpp](../src/hpserver.cpp#L214) 到 [src/hpserver.cpp](../src/hpserver.cpp#L282)。每一轮做四件事：

1. 根据 connection_timer_.get_next_timeout_ms() 计算 epoll_wait 的超时时间，见 [src/hpserver.cpp](../src/hpserver.cpp#L215) 到 [src/hpserver.cpp](../src/hpserver.cpp#L217)。
2. 调 poller_.wait() 获取当前就绪事件，见 [src/hpserver.cpp](../src/hpserver.cpp#L216) 到 [src/hpserver.cpp](../src/hpserver.cpp#L219)。
3. 逐个分发事件：监听 fd、eventfd 或客户端 fd，见 [src/hpserver.cpp](../src/hpserver.cpp#L221) 到 [src/hpserver.cpp](../src/hpserver.cpp#L279)。
4. 最后执行 connection_timer_.tick()，让超时任务真正触发，见 [src/hpserver.cpp](../src/hpserver.cpp#L281) 到 [src/hpserver.cpp](../src/hpserver.cpp#L281)。

这个循环的本质是一个单线程 reactor：所有 fd 状态切换、epoll 关注项变化和 http_conn 对象操作都在这里完成。

## 4. 新连接是怎么接进来的

当监听 fd 可读时，进入 accept 分支，见 [src/hpserver.cpp](../src/hpserver.cpp#L222) 到 [src/hpserver.cpp](../src/hpserver.cpp#L250)。

这里用了 while(true) 连续 accept，原因是 EPOLLET 要求把内核里当前可接受的连接尽量一次取空。每次 accept 的处理规则是：

- 如果返回 EINTR，就继续重试。
- 如果返回 EAGAIN 或 EWOULDBLOCK，说明这轮已经没有新连接了，退出循环。
- 其他错误则停止接收这一轮连接。

对每个新 client_fd，会按顺序做四件事：

1. 设置非阻塞。
2. 加入 epoll，关注 kClientEvents。
3. 检查 fd 是否落在 [0, MAX_FD) 内。
4. 创建 http_conn 并放入 connections_[client_fd]，然后刷新超时。

这保证了每个新连接一进入系统，就同时具备“非阻塞 IO、epoll 监听和超时回收”三个基础能力。

## 5. 客户端事件的处理顺序

普通客户端 fd 的处理在 [src/hpserver.cpp](../src/hpserver.cpp#L253) 到 [src/hpserver.cpp](../src/hpserver.cpp#L277)。它的顺序很重要：

1. 先检查 EPOLLERR、EPOLLHUP、EPOLLRDHUP。只要出现这些事件，就直接 close_client()，见 [src/hpserver.cpp](../src/hpserver.cpp#L257) 到 [src/hpserver.cpp](../src/hpserver.cpp#L260)。
2. 如果出现 EPOLLIN 或 EPOLLOUT，先刷新超时，见 [src/hpserver.cpp](../src/hpserver.cpp#L262) 到 [src/hpserver.cpp](../src/hpserver.cpp#L264)。
3. 如果有 EPOLLOUT，先 flush_client_output()，见 [src/hpserver.cpp](../src/hpserver.cpp#L266) 到 [src/hpserver.cpp](../src/hpserver.cpp#L272)。
4. 最后调用 handle_client() 处理读路径，见 [src/hpserver.cpp](../src/hpserver.cpp#L274) 到 [src/hpserver.cpp](../src/hpserver.cpp#L277)。

这个顺序的含义是：写优先于读路径中的后续处理。即使这一轮只想写响应，代码也仍然会顺手跑一次 handle_client()，因此读写逻辑是串行地在同一轮事件里完成的，而不是拆成两个独立状态机。

## 6. close 和超时是怎么收尾的

close_client() 在 [src/hpserver.cpp](../src/hpserver.cpp#L41) 到 [src/hpserver.cpp](../src/hpserver.cpp#L52)。它做的事情按顺序是：

1. 从定时器里移除该 fd。
2. 把 proxy_inflight_[fd] 复位为 false。
3. 从 epoll 中删除该 fd。
4. 释放 connections_[fd] 持有的 http_conn。

refresh_client_timeout() 在 [src/hpserver.cpp](../src/hpserver.cpp#L54) 到 [src/hpserver.cpp](../src/hpserver.cpp#L72)。它有两个分支：

- 如果定时器里已经有这个 fd，就直接 adjust 到 15 秒。
- 如果还没有，就注册一个新的超时任务，回调里再次检查连接是否仍有效，然后调用 close_client()。

这种“双重检查”很关键，因为定时器回调触发时，连接可能已经被 epoll 路径提前关闭了。

## 7. HTTP 请求是怎么读出来的

handle_client() 在 [src/hpserver.cpp](../src/hpserver.cpp#L287) 到 [src/hpserver.cpp](../src/hpserver.cpp#L326)。它的逻辑可以理解为“先读、再解析、完整后提交异步任务”。

具体步骤如下：

1. 先检查 fd 和 connections_[fd] 是否有效，无效则返回 -1，并设置 errno=ENOENT。
2. 如果 proxy_inflight_[fd] 为 true，直接返回 0，表示这个连接已经有一个请求在异步处理中，不继续读新请求，见 [src/hpserver.cpp](../src/hpserver.cpp#L295) 到 [src/hpserver.cpp](../src/hpserver.cpp#L298)。
3. 调用 conn.read_from_socket() 读取数据；如果是 EAGAIN 或 EWOULDBLOCK，返回 0，说明只是暂时没数据可读。
4. 调用 conn.parse_available_data() 解析已有缓冲；如果失败，打印 parse_error() 并返回 -1。
5. 如果请求还没完整到达，返回 0，继续等下一轮 EPOLLIN。
6. 当请求完整时，取出 req，打印日志，调用 conn.reset_for_next_message()，把 proxy_inflight_[fd] 置 true，然后 submit_proxy_job() 把请求交给线程池。

这里有一个很明确的边界：HTTP 解析是在线程池之外完成的，解析后的业务转发才异步出去。这让 reactor 线程只处理 IO 和状态，不承担上游耗时。

## 8. 异步代理链路怎么闭环

submit_proxy_job() 在 [src/hpserver.cpp](../src/hpserver.cpp#L102) 到 [src/hpserver.cpp](../src/hpserver.cpp#L119)。它把“转发请求”封装成一个线程池任务，执行步骤是：

1. 把任务丢进 proxy_pool_。
2. 在线程池线程里调用 http_proxy::forward_request(req, response, err)。
3. 生成 proxy_done_event 并推入 proxy_done_queue_。
4. 往 eventfd 写入 1，唤醒主线程。

主线程收到 eventfd 可读后，进入 drain_proxy_done_events()，见 [src/hpserver.cpp](../src/hpserver.cpp#L121) 到 [src/hpserver.cpp](../src/hpserver.cpp#L156)。它会先把 eventfd 读空，再不断 try_pop 完成队列。对每个完成事件，它会：

1. 校验 client_fd 仍然有效，失效则直接丢弃。
2. 把 proxy_inflight_[fd] 置 false，表示这个连接可以接收下一个请求了。
3. 如果 event.ok 为 false，就打印错误并关闭连接。
4. 如果成功，就把 response 放进连接发送缓冲，然后调用 flush_client_output() 尽快写回客户端。
5. 如果写成功，再刷新超时。

这个闭环的重点是线程边界清晰：工作线程只负责生成结果，不直接修改 epoll 状态，也不直接碰连接的 IO 轮转；真正的写回和状态推进仍然由主线程统一完成。

## 9. 响应写回是怎么做的

flush_client_output() 在 [src/hpserver.cpp](../src/hpserver.cpp#L158) 到 [src/hpserver.cpp](../src/hpserver.cpp#L188)。它是标准的非阻塞写回压处理：

1. 先检查 fd 和连接是否有效。
2. 只要 conn.has_pending_write()，就循环调用 conn.flush_to_socket()。
3. 如果 flush_to_socket() 写出了一部分数据，就继续写，直到写不动为止。
4. 如果返回 EAGAIN 或 EWOULDBLOCK，说明内核发送缓冲满了，于是把 epoll 关注项改成 kClientEvents | EPOLLOUT，等下次可写再续写。
5. 如果全部写完，就把 epoll 关注项改回 kClientEvents，去掉 EPOLLOUT。

这个函数的职责很纯粹：只负责把“待发缓冲”和“epoll 关注项”同步起来。

## 10. 一次完整请求的时间线

按时间顺序看，一次请求从进入到回写，大致是这样的：

1. 客户端连接到来，创建 http_conn 并注册超时。
2. 客户端发送请求，触发 EPOLLIN。
3. handle_client() 读取 socket 并增量解析。
4. 请求完整后提交代理任务，并标记该连接 inflight。
5. 工作线程把请求转发到上游并拿到响应。
6. 工作线程把完成事件放入队列，再写 eventfd。
7. 主线程被唤醒，drain_proxy_done_events() 把响应塞回连接写缓冲。
8. 如果一次没写完，就开启 EPOLLOUT 等下一轮继续写。
9. 写完后关闭 EPOLLOUT，连接继续等待下一次请求或空闲超时。

如果后续你想继续优化这份文档，我建议下一步把它再拆成两份：一份讲“事件循环和连接状态机”，另一份讲“代理异步通路和线程边界”，这样更方便和源码逐段对照。