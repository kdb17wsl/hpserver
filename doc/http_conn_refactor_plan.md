# HTTP Connection 与 Parser 解耦规划

## 目标
- 将连接收发与协议解析解耦，减少单类职责。
- 保持现有 `http_conn` 对外接口稳定，避免影响上层 `hpserver` 和现有测试。
- 为后续代理能力扩展预留清晰边界：访问控制、缓存、转发、审计。

## 已落地的拆分
- `src/net/http/http_connection_io.h/.cpp`
- `src/net/http/http_request_parser.h/.cpp`
- `src/net/http/http_conn.h/.cpp` 改为 Facade/编排层

## 新架构职责
- `http_connection_io`
  - 负责 socket 读写、读缓冲、写缓冲、非阻塞发送。
  - 不关心 HTTP 协议语义。
- `http_request_parser`
  - 负责 llhttp 状态机与回调。
  - 生成 `request_info`，维护解析状态与错误信息。
- `http_conn`
  - 负责编排：读取数据、将未解析片段喂给 parser、消息完成后 reset。
  - 保持兼容旧接口，作为迁移过渡层。

## 对项目目标的价值
- 高并发目标：连接层可独立优化（比如 zero-copy、分段发送策略）而不触碰解析逻辑。
- 协议扩展目标：可以新增 `https_tunnel_parser` 或 `socks5_parser`，复用同一连接层。
- 安全管控目标：可在 parser 输出后串接独立策略链（IP/CIDR、URL 规则、WAF）。
- 工程化目标：模块级单测更聚焦，定位问题更快。

## 下一阶段建议
1. 引入统一会话对象 `proxy_session`，聚合 client/server 两端连接。
2. 新建 `request_router`，按 method/host/url 决定缓存、转发、拦截路径。
3. 抽象 `policy_engine`（白名单、URL 规则、速率限制）。
4. 抽象 `forwarder`（目标连接池、超时与重试、隧道透传）。
5. 为 `http_connection_io` 与 `http_request_parser` 增加独立单测文件，降低回归风险。
