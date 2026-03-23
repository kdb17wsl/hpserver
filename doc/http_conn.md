# http_conn 模块文档

## 1. 模块职责

`http_conn` 是一个面向单个客户端连接的 HTTP/1.x 收发与请求解析封装，位于 `src/net/http/`。它把三类能力收敛到一个对象里：

1. 从套接字读取数据并缓存。
2. 使用 `llhttp` 按增量方式解析 HTTP 请求。
3. 维护待发送写缓冲，并把响应数据刷回套接字。

这个类当前工作在 `HTTP_REQUEST` 模式，适合正向代理或网关场景中的“客户端请求入口”。它不负责建立上游连接，也不负责生成 HTTP 响应，只负责把一个连接上的输入输出和请求解析状态管理好。

## 2. 公开接口概览

### 构造与连接状态

- `explicit http_conn(int fd = -1)`
  - 绑定一个已有文件描述符，并初始化 `llhttp` 解析器。
- `int fd() const`
  - 返回底层连接的文件描述符。
- `bool valid() const`
  - 判断当前对象是否持有有效 fd。

### 读写相关

- `ssize_t read_from_socket()`
  - 循环调用 `recv()` 读取数据，直到遇到 `EAGAIN`、`EWOULDBLOCK`、对端关闭或真实错误。
  - 成功时返回本次累计读取字节数。
  - 返回 `0` 既可能表示“对端已关闭且本次没有再读到数据”，也可能表示“当前无更多数据可读”。调用方需要结合连接上下文判断。
- `void queue_write(std::string_view data)`
  - 追加待发送数据到内部写缓冲。
- `ssize_t flush_to_socket()`
  - 循环调用 `send()` 尝试发送内部写缓冲中尚未发送的部分。
  - 遇到 `EAGAIN` 或 `EWOULDBLOCK` 时停止并保留未发送尾部，等待下次可写事件。
- `std::string take_write_buffer()`
  - 提取当前尚未发送的数据。
  - 如果此前已经部分发送，只返回未发送的尾部。

### 解析相关

- `bool parse_available_data()`
  - 从 `read_buffer_` 中尚未解析的位置开始调用 `llhttp_execute()`。
  - 成功返回 `true`，失败返回 `false` 并设置解析错误状态。
- `bool is_message_complete() const`
  - 当普通 HTTP 请求完整结束，或者 CONNECT 已升级为隧道时返回 `true`。
- `bool is_connect_tunnel() const`
  - 当解析状态为 `kUpgradedTunnel` 时返回 `true`。
- `bool has_parse_error() const`
  - 当前连接是否已经进入解析失败状态。
- `const request_info& request() const`
  - 获取已经解析出的请求结构。
- `parse_state state() const`
  - 返回当前解析状态。
- `const std::string& parse_error() const`
  - 返回由 `llhttp` 生成的错误描述。
- `const std::string& read_buffer() const`
  - 返回原始读缓冲内容，主要用于调试或上层自行处理数据。
- `void reset_for_next_message()`
  - 在 keep-alive 场景下，为下一条请求重置解析器与运行时状态。

## 3. 核心数据结构

### `parse_state`

模块通过一个小状态机表达连接解析阶段：

- `kReadingRequest`
  - 正在读取或解析请求。
- `kMessageComplete`
  - 一个完整 HTTP 请求已解析完毕。
- `kUpgradedTunnel`
  - `llhttp` 识别到升级完成，当前连接后续应按隧道透传处理。
- `kParseError`
  - 解析失败，`parse_error()` 可获取错误原因。

### `request_info`

`request_info` 保存当前这一次请求的解析结果：

- `method`
  - HTTP 方法名，例如 `GET`、`POST`、`CONNECT`。
- `url`
  - 原始 URL 字段。对普通代理请求，这通常是绝对 URI；对 CONNECT，请求目标通常表现为 `host:port`。
- `version`
  - HTTP 版本字符串，例如 `1.1`。
- `host`
  - 从 `Host` 首部或 CONNECT 目标中提取出的主机名。
- `port`
  - 解析得到的端口。普通 HTTP 在 `Host` 中未带端口时默认补为 `80`。
- `content_length`
  - `llhttp` 给出的内容长度。
- `chunked`
  - `Transfer-Encoding` 包含 `chunked` 时为 `true`。
- `keep_alive`
  - 基于 `llhttp_should_keep_alive()` 计算出的连接复用标志。
- `is_connect`
  - 当前请求是否为 CONNECT。
- `headers`
  - 请求头表，键统一转成小写，值保留原始大小写。
- `body`
  - 请求体内容，按解析回调持续追加。

## 4. 解析流程

### 4.1 初始化

构造函数会调用 `init_parser()` 完成以下工作：

1. 初始化 `llhttp_settings_t`。
2. 绑定 URL、版本、首部、Body、消息完成等回调。
3. 以 `HTTP_REQUEST` 模式初始化 `llhttp_t`。
4. 把 `parser_.data` 指回当前 `http_conn` 对象，供静态回调取回实例。

### 4.2 读取输入

`read_from_socket()` 每次最多按 `8192` 字节块读取，并把结果追加到 `read_buffer_`。该函数不做协议判断，也不会自动触发解析；典型用法是上层在可读事件中先读取，再调用 `parse_available_data()`。

### 4.3 增量解析

`parse_available_data()` 从 `parse_offset_` 开始，把当前未解析的尾部数据一次性交给 `llhttp_execute()`：

- 返回 `HPE_OK`：
  - 说明本次喂给解析器的数据都被正常消费，`parse_offset_` 更新到 `read_buffer_` 末尾。
- 返回 `HPE_PAUSED_UPGRADE`：
  - 说明请求已经完成并进入升级态，模块把状态切为 `kUpgradedTunnel`。
- 返回其他错误：
  - 模块把状态切为 `kParseError`，并将错误名与详细原因拼接到 `parse_error_`。

### 4.4 请求字段填充

解析器回调的职责如下：

- `on_message_begin_cb`
  - 清空上一条请求的运行时字段。
- `on_url_cb`
  - 追加 URL 片段。
- `on_version_cb`
  - 追加版本号片段。
- `on_header_field_cb` / `on_header_value_cb`
  - 以流式方式收集首部名和值。
- `on_headers_complete_cb`
  - 收尾最后一个首部。
  - 从 `llhttp` 获取方法、keep-alive、content-length。
  - 判断是否为 CONNECT。
  - 进一步根据请求头推导 `host`、`port` 和 `chunked`。
- `on_body_cb`
  - 追加请求体。
- `on_message_complete_cb`
  - 把状态更新为 `kMessageComplete`。

## 5. Host 与端口解析规则

`post_process_headers()` 和 `parse_host_port()` 负责补齐目标地址信息，规则如下：

1. 若方法为 CONNECT，则直接把 `url` 当作 `host:port` 解析。
2. 否则从 `Host` 首部读取主机和端口。
3. 普通 HTTP 请求在 `Host` 未显式带端口时，默认端口补为 `80`。
4. 支持 `[::1]:8080` 这种带方括号的 IPv6 写法。
5. 对没有方括号、但包含多个冒号的字符串，当前实现会把它整体视为主机名，不尝试拆出端口。

## 6. 缓冲区语义

### 读缓冲

- `read_buffer_` 保存从连接读取到的原始字节流。
- `parse_offset_` 标识“已交给 llhttp 处理到哪里”。
- 只有在调用 `reset_for_next_message()` 时，模块才会把已经消费过的前缀从 `read_buffer_` 中擦掉。

这意味着：

1. 完成一条请求解析后，读缓冲里可能还保留已解析数据，直到显式重置。
2. keep-alive 场景下，调用方在处理完当前请求后应该调用 `reset_for_next_message()`，否则下一条请求不会从干净状态开始。

### 写缓冲

- `queue_write()` 只做追加，不尝试立即发送。
- `flush_to_socket()` 根据 `write_offset_` 支持部分发送。
- 若已经全部发完，内部会清空缓冲并把偏移重置为 `0`。
- `take_write_buffer()` 适合把尚未发出的数据移交给其他发送路径或在连接切换阶段取走尾部数据。

## 7. 典型调用顺序

下面是一种常见的事件驱动调用方式：

```cpp
http_conn conn(client_fd);

// 可读事件
ssize_t n = conn.read_from_socket();
if (n < 0) {
    // 处理读错误
}

if (!conn.parse_available_data()) {
    // 记录 conn.parse_error() 并关闭连接
}

if (conn.is_message_complete()) {
    const auto& req = conn.request();
    // 根据 req.method / req.url / req.headers 执行业务

    if (!conn.is_connect_tunnel() && req.keep_alive) {
        conn.reset_for_next_message();
    }
}

// 可写事件
conn.queue_write(response_bytes);
ssize_t written = conn.flush_to_socket();
if (written < 0) {
    // 处理写错误
}
```

## 8. 适用边界与注意事项

### 线程模型

`http_conn` 没有任何内部同步原语，默认假设一个连接对象只被单线程拥有。若要跨线程读写同一个实例，需要由上层自行保证串行化。

### 首部覆盖行为

当前 `headers` 使用 `std::unordered_map<std::string, std::string>` 保存首部；若同名首部重复出现，后写入的值会覆盖先前值，不保留多值语义。

### Body 内存占用

请求体会完整累计到 `request_.body`，没有流式消费接口。对大请求体或长上传场景，上层需要额外关注内存占用。

### 升级后剩余字节

CONNECT 或其他 upgrade 场景下，`llhttp` 返回 `HPE_PAUSED_UPGRADE` 后，模块当前直接把 `parse_offset_` 推到 `read_buffer_` 末尾，并调用 `llhttp_resume_after_upgrade()`。这意味着如果“升级完成后的隧道字节”与请求头一起到达，模块目前没有单独暴露这段剩余字节的接口；上层若需要精确保留 upgrade 后的同包数据，需要进一步扩展这里的处理逻辑。

### 连接关闭判定

`read_from_socket()` 在 `recv()` 返回 `0` 时直接返回累计读取量，因此“读到 EOF”与“本次刚好没读到新字节”的区分要由上层结合事件来源、连接状态和返回值共同判断。

## 9. 与 llhttp 的关系

本模块不自行分配 `llhttp_t`，而是把解析器作为成员对象内嵌在 `http_conn` 里，再用 `llhttp_init()` 初始化。这一点与仓库内的 `llhttp` 集成方式保持一致，也避免依赖仅在特定构建目标下才存在的动态分配接口。

## 10. 文件位置

- 接口声明：`src/net/http/http_conn.h`
- 具体实现：`src/net/http/http_conn.cpp`
- 本文档：`doc/http_conn.md`