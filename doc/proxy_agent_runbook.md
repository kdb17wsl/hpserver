# Proxy 实现 Agent 执行手册

## 1. 文档用途

本手册用于让自动化 coding agent 在本仓库内持续完成以下目标：

1. 建立上游连接并转发普通 HTTP 请求。
2. 支持 CONNECT 隧道模式。
3. 扩展 epoll 统一管理客户端与上游双端 fd。

本手册强调可执行性，包含阶段任务、代码边界、验收标准、回归命令和可复制提示词模板。

## 2. 目标与约束

### 2.1 目标

1. HTTP 正向代理链路可用。
2. HTTPS CONNECT 可建立隧道并双向透传。
3. 在高并发下保持非阻塞、无明显资源泄漏。

### 2.2 约束

1. 保持 C++17 与现有 CMake 结构兼容。
2. 连接状态与 epoll I/O 变更优先留在 reactor 线程。
3. connections_ 继续按 client fd 直接索引，遵守 MAX_FD 边界。
4. accept 非阻塞循环保持 EINTR 重试，EAGAIN/EWOULDBLOCK 视为正常结束。

## 3. 代码边界

优先在以下文件内改动：

1. src/hpserver.h
2. src/hpserver.cpp
3. src/net/proxy/http_proxy.h
4. src/net/proxy/http_proxy.cpp
5. src/net/http/http_conn.h
6. src/net/http/http_conn.cpp
7. src/net/http/http_request_parser.h
8. src/net/http/http_request_parser.cpp
9. CMakeLists.txt

测试优先补充到：

1. tests/http_conn_test.cpp

## 4. 分阶段执行计划

### 阶段 A：普通 HTTP 转发最小可用

任务：

1. 将 hpserver 中当前测试回显逻辑替换为代理入口调用。
2. 基于 host/port 建立非阻塞上游连接。
3. 将客户端请求转发到上游，并将上游响应回传客户端。

关键实现点：

1. 发起 connect 后处理 EINPROGRESS。
2. 上游可写事件中用 SO_ERROR 确认连接是否成功。
3. 请求行改为 origin-form，过滤 hop-by-hop 头。

完成标准：

1. curl 通过代理访问 HTTP 站点成功。
2. GET 与 POST 均可工作。

### 阶段 B：CONNECT 隧道

任务：

1. CONNECT 请求上游连接成功后回 200 Connection Established。
2. 会话状态切换为 tunneling。
3. 双端按字节流透传，不再进行 HTTP 语义解析。

关键实现点：

1. 双向读写都走非阻塞缓冲。
2. 任一端关闭时，触发成对收尾与资源回收。

完成标准：

1. curl 通过代理访问 HTTPS 成功。
2. 连续请求无明显丢包或卡住。

### 阶段 C：双端 epoll 统一管理

任务：

1. 为 fd 增加角色信息，区分 listener、client、upstream。
2. 建立 client_fd 与 upstream_fd 双向映射。
3. 在同一 epoll 循环中分派双端读写事件。

关键实现点：

1. EPOLLIN 负责搬运数据到对端写缓冲。
2. EPOLLOUT 负责 flush 写缓冲与连接完成确认。
3. EPOLLERR/EPOLLHUP/EPOLLRDHUP 统一错误路径。

完成标准：

1. 双端事件处理路径统一且可追踪。
2. 没有明显 fd 泄漏和僵尸会话。

### 阶段 D：边界与稳定性

任务：

1. 处理 CONNECT 升级后同包残留字节透传。
2. 完善背压策略和半关闭语义。
3. 增加错误码映射和日志。

完成标准：

1. 压测下无明显 busy loop。
2. 异常断连后可恢复。

## 5. 建议状态机

建议每个会话至少维护以下状态：

1. reading_request
2. connecting_upstream
3. forwarding_http
4. tunneling
5. closing

状态迁移：

1. reading_request -> connecting_upstream：请求完整且策略允许。
2. connecting_upstream -> forwarding_http：普通 HTTP connect 成功。
3. connecting_upstream -> tunneling：CONNECT connect 成功并回 200。
4. 任意状态 -> closing：对端关闭、系统错误或协议错误。

## 6. Agent 执行规则

1. 每次只推进一个阶段，避免一次性大改。
2. 每个阶段结束前必须编译并运行相关测试。
3. 改动后同步更新文档中的阶段状态和遗留问题。
4. 遇到不确定设计点先给出最小可运行方案，再列后续优化点。

## 7. 阶段验收命令

在仓库根目录执行：

1. cmake -S . -B build -G Ninja
2. cmake --build build
3. ctest --test-dir build --output-on-failure

可选：

1. ./build/hpserver
2. curl -x http://127.0.0.1:8080 http://example.com -v
3. curl -x http://127.0.0.1:8080 https://example.com -v

## 8. 风险清单

1. CONNECT 升级后剩余字节可能丢失。
2. ET 模式下未完全读空或写空会导致事件丢触发。
3. 上游慢写引发写缓冲膨胀。
4. 双端关闭顺序处理不一致导致重复 close。

## 9. 任务看板模板

每次 agent 执行建议产出如下看板：

1. 目标阶段：A/B/C/D
2. 本次改动文件：逐文件列出
3. 已完成项：明确行为变化
4. 未完成项：下一步最小任务
5. 验证结果：编译、测试、手工验证
6. 风险与回滚点：是否可安全回退

## 10. 可复制的 Agent 提示词模板

模板 1：推进单阶段

请按 doc/proxy_agent_runbook.md 执行阶段 A。
要求：
1. 仅改动手册允许的文件。
2. 保持 reactor 线程持有连接状态。
3. 实现后运行 cmake --build build 与 ctest --test-dir build --output-on-failure。
4. 输出内容按“任务看板模板”给出。

模板 2：修复阶段回归

请按 doc/proxy_agent_runbook.md 对阶段 B 做回归修复。
要求：
1. 先定位 CONNECT 失败点并给出根因。
2. 最小化改动修复。
3. 提供复现命令与修复后验证结果。

模板 3：做收尾稳定性

请按 doc/proxy_agent_runbook.md 执行阶段 D。
要求：
1. 优先修复 ET 模式下的读写边界问题。
2. 增加必要日志与错误路径保护。
3. 给出仍未覆盖的风险清单。

## 11. 变更记录

1. 2026-03-19：初版创建。
2. 2026-03-23：阶段 A 首次落地，完成普通 HTTP 转发最小可用实现并通过编译与现有测试。
3. 2026-03-23：阶段 B 最小实现完成，CONNECT 建连后返回 200 并支持双向字节流透传。

## 12. 阶段状态（最新）

1. 目标阶段：B（CONNECT 隧道）
2. 本次改动文件：
	- src/net/proxy/http_proxy.h
	- src/net/proxy/http_proxy.cpp
	- tests/http_conn_test.cpp
3. 已完成项：
	- CONNECT 请求建立上游连接成功后返回 `200 Connection Established`。
	- CONNECT 会话进入隧道模式后，双端按字节流双向非阻塞透传。
	- 任一端关闭后触发会话收尾，避免隧道僵挂。
	- 新增阶段 B 自动化回归用例，覆盖 CONNECT 建连与双向透传。
4. 未完成项：
	- 双端 fd 统一纳入同一 epoll 事件循环（阶段 C）。
	- CONNECT 升级后同包残留字节透传边界（阶段 D）。
5. 验证结果：
	- 已通过 `cmake --build build`。
	- 已通过 `ctest --test-dir build --output-on-failure`（包含阶段 A 与阶段 B 新增自动化回归）。
6. 风险与回滚点：
	- 当前阶段 B 采用代理内 poll 驱动的最小隧道实现，后续阶段 C 将迁移到统一 epoll 双端分派。
	- 若需快速回退，可回退 `src/net/proxy/http_proxy.*` 中 CONNECT 分支与隧道透传逻辑。