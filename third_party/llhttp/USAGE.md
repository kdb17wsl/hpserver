# llhttp 使用文档

本文档面向当前仓库中内嵌的 llhttp 库，说明它的接入方式、核心 API、典型解析流程和常见注意事项。

## 1. 库定位

llhttp 是一个基于回调的 HTTP/1.x 解析器，适合以下场景：

- 从 socket 读取到一段字节后进行增量解析
- 按事件回调提取 URL、Header、Body 等字段
- 在请求完成后继续复用同一个解析器处理后续报文
- 处理 keep-alive、chunked body、upgrade 等 HTTP 连接语义

当前仓库将 llhttp 以静态库方式编译，源码位于：

- `third_party/llhttp/include/llhttp.h`
- `third_party/llhttp/src/api.c`
- `third_party/llhttp/src/http.c`
- `third_party/llhttp/src/llhttp.c`

## 2. 在本仓库中的接入方式

顶层 CMake 已经定义了 `llhttp` 静态库，并把头文件目录导出出去。

### 直接链接 llhttp

```cmake
target_link_libraries(your_target PRIVATE llhttp)
```

### 通过项目公共头目标间接链接

当前仓库里 `hpserver_headers` 已经把 `llhttp` 作为接口依赖暴露出去，因此也可以这样使用：

```cmake
target_link_libraries(your_target PRIVATE hpserver_headers)
```

如果你的目标只需要当前仓库统一导出的头文件和 llhttp，这种方式更贴合现有工程结构。

## 3. 核心对象

llhttp 的核心对象有两个：

- `llhttp_t`：解析器实例，保存当前解析状态
- `llhttp_settings_t`：回调集合，定义各阶段事件处理逻辑

典型初始化顺序如下：

1. 定义并清零 `llhttp_settings_t`
2. 填充你需要的回调函数
3. 定义 `llhttp_t`
4. 调用 `llhttp_init(&parser, HTTP_REQUEST 或 HTTP_RESPONSE, &settings)`
5. 可选地通过 `parser.data` 挂接业务上下文

## 4. 最小可运行示例

下面的示例展示如何解析一个 HTTP 请求，并在回调中收集 URL、Header 和 Body。

```cpp
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "llhttp.h"
}

struct RequestContext {
    std::string url;
    std::string current_header_field;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

static int on_message_begin(llhttp_t* parser) {
    auto* ctx = static_cast<RequestContext*>(parser->data);
    ctx->url.clear();
    ctx->current_header_field.clear();
    ctx->headers.clear();
    ctx->body.clear();
    return 0;
}

static int on_url(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<RequestContext*>(parser->data);
    ctx->url.append(at, length);
    return 0;
}

static int on_header_field(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<RequestContext*>(parser->data);
    ctx->current_header_field.assign(at, length);
    return 0;
}

static int on_header_value(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<RequestContext*>(parser->data);
    ctx->headers.emplace_back(ctx->current_header_field, std::string(at, length));
    return 0;
}

static int on_body(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<RequestContext*>(parser->data);
    ctx->body.append(at, length);
    return 0;
}

static int on_headers_complete(llhttp_t* parser) {
    std::cout << "method="
              << llhttp_method_name(static_cast<llhttp_method_t>(llhttp_get_method(parser)))
              << ", version=HTTP/"
              << static_cast<int>(llhttp_get_http_major(parser))
              << "."
              << static_cast<int>(llhttp_get_http_minor(parser))
              << std::endl;
    return 0;
}

static int on_message_complete(llhttp_t* parser) {
    auto* ctx = static_cast<RequestContext*>(parser->data);
    std::cout << "url=" << ctx->url << std::endl;
    std::cout << "header count=" << ctx->headers.size() << std::endl;
    std::cout << "body=" << ctx->body << std::endl;
    return 0;
}

int main() {
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_message_begin = on_message_begin;
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_value = on_header_value;
    settings.on_headers_complete = on_headers_complete;
    settings.on_body = on_body;
    settings.on_message_complete = on_message_complete;

    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);

    RequestContext context;
    parser.data = &context;

    const char* request =
        "POST /hello HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "world";

    llhttp_errno_t err = llhttp_execute(&parser, request, std::strlen(request));
    if (err != HPE_OK) {
        std::cerr << "parse error: " << llhttp_errno_name(err)
                  << ", reason=" << (llhttp_get_error_reason(&parser) ? llhttp_get_error_reason(&parser) : "")
                  << std::endl;
        return 1;
    }

    if (llhttp_message_needs_eof(&parser)) {
        err = llhttp_finish(&parser);
        if (err != HPE_OK) {
            std::cerr << "finish error: " << llhttp_errno_name(err) << std::endl;
            return 1;
        }
    }

    return 0;
}
```

## 5. 标准使用流程

### 5.1 初始化 settings

务必先调用：

```c
llhttp_settings_t settings;
llhttp_settings_init(&settings);
```

这个函数会把所有回调置空，未设置的回调会被直接跳过。

### 5.2 初始化 parser

```c
llhttp_t parser;
llhttp_init(&parser, HTTP_REQUEST, &settings);
parser.data = your_context_ptr;
```

第二个参数指定当前解析器的报文方向：

- `HTTP_REQUEST`：解析请求
- `HTTP_RESPONSE`：解析响应
- `HTTP_BOTH`：同时兼容两种类型，通常不建议在业务代码里滥用，明确方向更安全

### 5.3 增量喂数据

```c
llhttp_errno_t err = llhttp_execute(&parser, buf, len);
```

这个调用可以重复进行，适合每次 `recv()` 到一段数据后继续解析。llhttp 会维护内部状态，不要求一次拿到完整 HTTP 报文。

### 5.4 必要时调用 finish

如果报文长度依赖 EOF 才能确定，连接读端关闭后需要调用：

```c
if (llhttp_message_needs_eof(&parser)) {
    llhttp_finish(&parser);
}
```

否则，某些响应可能会因为缺少 EOF 收尾而无法正确结束。

### 5.5 复用解析器

处理完一条消息后，如果还要解析下一条消息，可以调用：

```c
llhttp_reset(&parser);
```

`llhttp_reset()` 会保留以下内容：

- parser 类型
- settings 指针
- `parser.data`
- 已启用的 lenient flags

这很适合长连接场景。

## 6. 常用回调说明

### 数据类回调

以下回调签名为：

```c
int (*llhttp_data_cb)(llhttp_t*, const char* at, size_t length);
```

常见回调包括：

- `on_url`
- `on_status`
- `on_method`
- `on_version`
- `on_header_field`
- `on_header_value`
- `on_body`
- `on_protocol`

注意两点：

1. `at` 指向本次输入缓冲区中的一段切片，不保证以 `\0` 结尾。
2. 同一字段可能被分多次回调送达，业务代码应使用 `append` 而不是假设一次收齐。

### 状态类回调

以下回调签名为：

```c
int (*llhttp_cb)(llhttp_t*);
```

常用回调包括：

- `on_message_begin`
- `on_headers_complete`
- `on_message_complete`
- `on_chunk_header`
- `on_chunk_complete`

### 回调返回值语义

- 返回 `0`：正常继续解析
- 数据类回调返回 `-1`：llhttp 会转换为 `HPE_USER`，并设置错误原因
- 某些状态回调可返回 `HPE_PAUSED`：让解析暂停，之后通过 `llhttp_resume()` 恢复
- `on_headers_complete` 有额外语义

`on_headers_complete` 支持以下返回值：

- `0`：按正常流程解析 body
- `1`：认为没有 body，直接进入下一条消息
- `2`：认为发生 upgrade，让 `llhttp_execute()` 返回 `HPE_PAUSED_UPGRADE`
- `-1`：错误
- `HPE_PAUSED`：暂停解析

## 7. 解析结果如何获取

llhttp 本身不帮你构造完整请求对象，它只负责解析并通过回调暴露事件。常用获取方式如下：

### 请求方法

```c
llhttp_method_t method = (llhttp_method_t) llhttp_get_method(&parser);
const char* name = llhttp_method_name(method);
```

### HTTP 版本

```c
uint8_t major = llhttp_get_http_major(&parser);
uint8_t minor = llhttp_get_http_minor(&parser);
```

### 响应状态码

```c
int status = llhttp_get_status_code(&parser);
const char* status_name = llhttp_status_name((llhttp_status_t) status);
```

### Upgrade 状态

```c
uint8_t upgraded = llhttp_get_upgrade(&parser);
```

### Keep-Alive 判断

```c
int keep_alive = llhttp_should_keep_alive(&parser);
```

## 8. 错误处理

推荐在每次 `llhttp_execute()` 或 `llhttp_finish()` 后检查返回值。

```c
llhttp_errno_t err = llhttp_execute(&parser, buf, len);
if (err != HPE_OK) {
    fprintf(stderr, "errno=%s\n", llhttp_errno_name(err));
    fprintf(stderr, "reason=%s\n", llhttp_get_error_reason(&parser));
    fprintf(stderr, "error_pos_offset=%td\n",
            llhttp_get_error_pos(&parser) - buf);
}
```

常用诊断接口：

- `llhttp_get_errno()`：最近一次错误码
- `llhttp_errno_name()`：错误码名字
- `llhttp_get_error_reason()`：错误原因
- `llhttp_get_error_pos()`：出错位置，指向当前输入 buffer 中最后一个成功解析字节附近

如果在用户回调中主动报错，建议同时调用：

```c
llhttp_set_error_reason(&parser, "custom reason");
return HPE_USER;
```

## 9. 暂停与恢复

如果你的业务需要在解析中途等待外部条件，可以在回调里返回 `HPE_PAUSED`，此时：

- `llhttp_execute()` 会返回 `HPE_PAUSED`
- 后续调用 `llhttp_resume(&parser)` 后可继续解析

如果是协议升级场景，恢复方式是：

```c
llhttp_resume_after_upgrade(&parser);
```

不要在用户回调里直接调用 `llhttp_pause()`；头文件也明确说明了，这种场景应通过回调返回 `HPE_PAUSED` 来完成暂停。

## 10. Lenient 模式

llhttp 提供一组宽松解析开关，例如：

- `llhttp_set_lenient_headers()`
- `llhttp_set_lenient_chunked_length()`
- `llhttp_set_lenient_keep_alive()`
- `llhttp_set_lenient_transfer_encoding()`
- `llhttp_set_lenient_version()`
- `llhttp_set_lenient_data_after_close()`
- `llhttp_set_lenient_optional_lf_after_cr()`
- `llhttp_set_lenient_optional_cr_before_lf()`
- `llhttp_set_lenient_optional_crlf_after_chunk()`
- `llhttp_set_lenient_spaces_after_chunk_size()`
- `llhttp_set_lenient_header_value_relaxed()`

这些开关大多会降低协议严格性。头文件里已经明确提示，多数选项可能带来请求走私或缓存投毒等安全风险。除非你非常清楚兼容性需求，否则建议保持默认关闭。

## 11. 本仓库中的特殊注意事项

### 11.1 原生构建下不要依赖 llhttp_alloc 和 llhttp_free

虽然头文件声明了：

- `llhttp_alloc()`
- `llhttp_free()`

但当前仓库中的实现只在 `__wasm__` 条件下编译。也就是说，在普通 Linux 原生构建里，应当使用下面这种方式：

```c
llhttp_t parser;
llhttp_init(&parser, HTTP_REQUEST, &settings);
```

如果你在当前工程中直接调用 `llhttp_alloc()` 或 `llhttp_free()`，大概率会在链接阶段失败。

### 11.2 parser.data 是最直接的上下文挂载点

llhttp 不管理业务对象生命周期。实际工程里通常把连接对象、请求构建器或状态机上下文挂到 `parser.data` 上，在回调里再转换回来。

### 11.3 Header 和 Body 可能是分段到达的

不要假设：

- 一个 header field 只触发一次 `on_header_field`
- 一个 header value 只触发一次 `on_header_value`
- `on_body` 只触发一次

网络读取和解析边界并不等价，业务层应该按流式方式拼接。

## 12. 一个典型的 socket 集成流程

下面是服务端处理中较常见的流程：

1. 连接建立时创建业务上下文和 `llhttp_t`
2. 调用 `llhttp_settings_init()` 和 `llhttp_init()` 完成初始化
3. 把连接上下文地址写入 `parser.data`
4. 每次 `recv()` 到字节后调用 `llhttp_execute()`
5. 在 `on_url`、`on_header_field`、`on_header_value`、`on_body` 中累积请求数据
6. 在 `on_message_complete` 中将请求交给上层路由或业务处理器
7. 若连接保持活跃，则 `llhttp_reset()` 后继续处理下一条消息
8. 若读到 EOF 且 `llhttp_message_needs_eof()` 为真，则调用 `llhttp_finish()` 完成收尾

## 13. 适合先记住的 API

如果你只想先快速用起来，优先掌握下面这些接口：

- `llhttp_settings_init()`
- `llhttp_init()`
- `llhttp_execute()`
- `llhttp_finish()`
- `llhttp_reset()`
- `llhttp_get_method()`
- `llhttp_get_status_code()`
- `llhttp_should_keep_alive()`
- `llhttp_get_error_reason()`
- `llhttp_get_error_pos()`

## 14. 建议的封装方式

在业务代码里，通常不直接裸用 `llhttp_t`，而是封装一层更稳定的请求解析器对象，例如：

- 一个连接对应一个 parser
- 一个 parser 对应一个请求构建上下文
- 在回调里只做轻量拼接和状态记录
- 在 `on_message_complete` 之后再构造高层 HTTP 请求对象

这样可以把网络 IO、协议解析和业务对象构造解耦，后续也更容易测试。

## 15. 总结

在当前仓库中使用 llhttp，最重要的是把握三点：

1. 用 `llhttp_settings_t + llhttp_t + llhttp_init()` 建立解析器
2. 用 `llhttp_execute()` 做增量解析，并通过回调收集字段
3. 正确处理 EOF、暂停恢复、分段回调和解析器复用

如果你后续准备在这个仓库里继续封装 HTTP 连接或请求对象，建议下一步直接围绕 `parser.data` 建一个连接上下文和请求构建器。