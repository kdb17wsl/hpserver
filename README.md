# hpserver

hpserver 是一个基于 C++20 的高性能 HTTP 代理服务器实验项目，采用单 Reactor 主线程 + 双 Worker 线程池的架构。

- 主线程负责接入、epoll 事件分发、连接状态和超时回收
- Worker 线程负责上游代理任务（普通 HTTP 转发与 CONNECT 隧道）
- 使用 eventfd + 完成队列把 Worker 结果回传到 Reactor 线程

## 功能特性

- 支持 HTTP 正向代理（转发请求并回写响应）
- 支持 HTTPS CONNECT 隧道
- ET 模式非阻塞 I/O（epoll + drain-until-would-block）
- 基于文件描述符直接索引的连接管理（MAX_FD = 65536）
- 空闲连接超时回收
- IP 黑名单拦截（命中时返回 403）
- 双层 HTTP 响应缓存：L1 内存 LRU + L2 RocksDB 持久化
  - 支持条件请求（If-None-Match / If-Modified-Since）返回 304
  - 支持 Cache-Control max-age 与 Expires 新鲜度判断
  - Vary 头元数据存储

## 依赖环境

- Linux
- CMake 3.14+
- 支持 C++20 的编译器（GCC/Clang）
- Ninja（推荐）
- GTest（用于测试）
- RocksDB（C++ 静态库）
- Protocol Buffers（protoc + libprotobuf）
- ZLIB、BZip2、snappy、lz4、zstd、liburing（RocksDB 静态链接依赖）

## Ubuntu 部署环境准备

以下以 Ubuntu 22.04/24.04 为例。

### 1. 安装基础工具

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  git \
  curl
```

### 2. 安装 RocksDB 与 Protocol Buffers

```bash
sudo apt install -y \
  librocksdb-dev \
  protobuf-compiler \
  libprotobuf-dev \
  zlib1g-dev \
  libbz2-dev \
  libsnappy-dev \
  liblz4-dev \
  libzstd-dev \
  liburing-dev
```

说明：RocksDB 及其压缩依赖（snappy、lz4、zstd）和 io_uring（liburing）为项目硬依赖；CMake 通过 `find_package` 自动探测。如果系统包版本过旧，也可通过 `CMAKE_PREFIX_PATH` 指向自定义编译的 RocksDB 路径。

### 3. 安装测试依赖（可选但推荐）

```bash
sudo apt install -y libgtest-dev googletest
```

说明：项目测试通过 CMake 的 `find_package(GTest REQUIRED)` 发现 GTest。

### 4. 拉取代码并构建

```bash
git clone <your-repo-url> hpserver
cd hpserver
cmake -S . -B build -G Ninja
cmake --build build
```

### 5. 运行与验证

```bash
./build/hpserver --port 8080
```

新开一个终端验证代理：

```bash
curl -x http://127.0.0.1:8080 http://example.com -v
curl -x http://127.0.0.1:8080 https://example.com -v
```

## 构建

在仓库根目录执行：

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

生成可执行文件：

- `build/hpserver`

## 运行

### 基础启动

```bash
./build/hpserver
```

默认监听端口：8080

### 启动参数

```text
--port N
--proxy-workers N
--tunnel-workers N
--proxy-queue N
--tunnel-queue N
```

### 参数默认值规则

当不传对应参数或传 0 时：

- proxy-workers = max(2, hardware_concurrency)，若硬件并发不可用则回退到 8
- tunnel-workers = max(32, hardware_concurrency * 8)，若硬件并发不可用则回退到 64
- proxy-queue = proxy-workers * 512
- tunnel-queue = tunnel-workers * 128

### 示例

```bash
./build/hpserver --port 8080
./build/hpserver --proxy-workers 8 --tunnel-workers 64 --proxy-queue 4096 --tunnel-queue 8192
```

## 代理验证

### HTTP 转发

```bash
curl -x http://127.0.0.1:8080 http://example.com -v
```

### HTTPS CONNECT 隧道

```bash
curl -x http://127.0.0.1:8080 https://example.com -v
```

## 测试

运行全部测试：

```bash
ctest --test-dir build --output-on-failure
```

运行单测二进制示例：

```bash
./build/tests/http_conn_test
./build/tests/threadsafe_queue_test
./build/tests/thread_pool_test
```

## 项目结构

```text
src/
  main.cpp                    程序入口与命令行参数解析
  hpserver.h/.cpp             服务器主循环、连接管理、异步代理协作
  net/core/                   poller、socket 操作、IP 过滤等核心组件
  net/http/                   HTTP 连接编排、协议解析、缓存与 I/O
  net/proxy/                  上游转发与 CONNECT 隧道实现
  util/                       线程池、队列、定时器等工具模块

tests/                        GTest 单元测试
third_party/llhttp/           HTTP 解析器

doc/
  loop.md                     Reactor 循环与定时器行为说明
  http_conn.md                HTTP 连接状态机与缓冲语义
  proxy_agent_runbook.md      代理链路实现与验证手册
```

## 运行流程（简版）

1. main.cpp 解析命令行参数并创建 hpserver 实例
2. listen() 初始化 socket、epoll、eventfd 与连接容器
3. accept 新连接并注册到 epoll
4. 可读事件触发后读取并解析 HTTP 请求
5. 请求完成后投递到对应 Worker 池执行代理逻辑
6. Worker 完成后通过 eventfd 通知主线程回写响应并按策略关闭连接

## 当前行为说明

- 普通 HTTP 代理路径当前采用单请求语义，任务完成后会关闭连接（close-after-flush）
- CONNECT 隧道在任务完成后同样按收尾策略关闭连接
- 在 ET 模式下，读写路径会持续处理直到 EAGAIN/EWOULDBLOCK

## 参考文档

- `doc/loop.md`
- `doc/http_conn.md`
- `doc/proxy_agent_runbook.md`
- `doc/http_conn_refactor_plan.md`（历史方案）
- `doc/agent.md`（路线图）
