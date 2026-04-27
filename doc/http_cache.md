# http_cache 模块文档

## 1. 模块职责

`http_cache` 为正向代理提供两级 HTTP 响应缓存：

1. **L1 内存缓存**：基于 `memory_cache` 的进程内 LRU，用于高频小对象的纳秒级命中。
2. **L2 磁盘缓存**：基于 RocksDB 的持久化存储，用于全量缓存和进程重启后的数据保留。

当前实现聚焦于 L1 的快速命中路径和 L2 的框架占位；完整的 HTTP 缓存语义（新鲜度验证、条件请求、TTL 淘汰）在后续迭代中逐步补齐。

## 2. 架构概览

```
客户端请求
    │
    ▼
Reactor 线程
    │
    ├──► http_cache::lookup()
    │       │
    │       ├──► L1 (memory_cache)
    │       │       命中 ──► 直接返回 kHit
    │       │
    │       └──► L2 (RocksDB)
    │               命中 ──► 返回响应（TODO: 回填 L1）
    │               未命中 ──► kMiss，走代理请求上游
    │
    └──► http_cache::store()
            │
            ├──► ≤ 64KB ──► 写入 L1 + L2
            └──► > 64KB ──► 跳过 L1，仅写入 L2
```

L1 与 L2 之间是**旁路（look-aside）**关系：`lookup` 先查 L1，miss 再查 L2；`store` 分别写入两级，大对象跳过 L1 以避免内存污染和拷贝开销。

## 3. 核心数据结构

### 3.1 `memory_cache`

位于 `src/util/memory_cache.h`，是一个**非线程安全**的泛型 LRU 缓存模板。

- **双维度淘汰**：同时受 `max_entries`（条目数上限）和 `max_bytes`（总字节上限）约束，任一条件触发即淘汰最久未使用条目。
- **可定制计重函数**：默认对具有 `.size()` 成员的类型按长度计重，其余类型回退 `sizeof(T)`；也支持传入自定义 lambda/函数对象。
- **API 风格**：提供 `get()`（返回裸指针，适合内部短生命周期访问）和 `with_value()`（回调 API，无指针逃逸风险，推荐外部调用）。

```cpp
memory_cache<std::string, std::string> cache(4096, 64 * 1024 * 1024);
cache.put("key", "value");

// 回调方式（推荐）
cache.with_value("key", [](const std::string& v) {
    // v 仅在回调内有效
});

// 指针方式（内部使用）
const std::string* v = cache.get("key");
```

### 3.2 `http_cache`

位于 `src/net/http/http_cache.h`，封装 L1 + L2 的协同逻辑。

#### `lookup_status`

| 状态 | 含义 |
|------|------|
| `kBypass` | 请求本身不可缓存（如 CONNECT、非 GET） |
| `kMiss` | 可缓存但未命中 |
| `kNotModified` | 条件请求验证为未修改（TODO） |
| `kHit` | 命中缓存 |
| `kError` | 缓存模块异常（如 RocksDB 未初始化） |

#### `lookup_result`

```cpp
struct lookup_result {
    lookup_status status = lookup_status::kBypass;
    std::string cache_key;
    std::string response;
    std::string detail;
};
```

## 4. 公开接口

### 4.1 生命周期

- `bool open(std::string_view db_path)`
  - 打开 RocksDB 数据库，路径由调用方指定。
  - 失败返回 `false`。
- `bool ready() const`
  - RocksDB 是否已成功初始化。
- `void configure_l1(std::size_t max_entries, std::size_t max_bytes)`
  - 配置 L1 内存缓存容量。默认状态为 `(0, 0)`，即 L1 完全禁用。
  - 可随时调用，会重新构造内部 `memory_cache` 实例（原有 L1 数据丢失）。

### 4.2 查询与写入

- `lookup_result lookup(const http_request_parser::request_info& req) const`
  - 入口先判断请求是否可缓存；不可缓存直接返回 `kBypass`。
  - 构造 cache key，依次查询 L1 → L2。
  - L1 命中直接返回 `kHit`；L2 命中返回完整响应（TODO：当前为占位实现，总是返回 `kMiss`）。
- `bool store(const http_request_parser::request_info& req, std::string_view upstream_response)`
  - 判断可缓存性，构造 cache key。
  - 小对象（≤ 64KB）写入 L1；所有对象（当前实现中）均保留 L2 写入占位符。
  - 不可缓存或 RocksDB 未就绪时返回 `false`。

## 5. 缓存流程

### 5.1 Lookup 路径

```
1. is_cacheable_request(req)
   └─ 否 ──► return kBypass

2. build_cache_key(req)

3. ready()?
   └─ 否 ──► return kError

4. L1 lookup (with_value)
   └─ 命中 ──► result.response = cached
               return kHit

5. L2 lookup (RocksDB::Get)
   └─ 命中 ──► result.response = value
               // TODO: 回填 L1（需考虑大小阈值）
               // TODO: 条件请求验证（If-Modified-Since / ETag）
               return kHit / kNotModified
   └─ 未命中 ──► return kMiss
```

### 5.2 Store 路径

```
1. is_cacheable_request(req) && ready()
   └─ 否 ──► return false

2. build_cache_key(req)

3. upstream_response.size() ≤ 64KB?
   └─ 是 ──► l1_cache_.put(key, response)
   └─ 否 ──► 跳过 L1

4. // TODO: 解析响应头，提取 Last-Modified / ETag / Cache-Control
5. // TODO: 序列化为 HttpCacheEntry，写入 RocksDB
6. return true
```

## 6. Cache Key 构建

`build_cache_key` 将以下字段拼接为唯一标识：

```
host:port|method|url|accept-encoding
```

- `host`、`port` 来自请求解析结果。
- `method` 当前仅对 `GET` 生成缓存（其他方法直接 `kBypass`）。
- `accept-encoding` 仅在请求头中存在时追加，用于区分压缩与非压缩响应。

**注意**：当前实现未处理 `Vary` 响应头。若上游返回 `Vary: User-Agent`，同一 URL 对不同 UA 应生成不同 cache key，此逻辑待扩展。

## 7. 可缓存性判断

当前规则极简：

```cpp
return !req.is_connect && req.method == "GET";
```

即：
- **拒绝** `CONNECT` 隧道请求。
- **拒绝** 非 `GET` 方法（`POST`、`PUT`、`DELETE` 等）。

后续需扩展为基于响应头 `Cache-Control` 的完整可缓存性分析（如 `no-store`、`private`、`max-age` 等）。

## 8. 大小分流策略

L1 只缓存**小对象**，阈值硬编码为 **64KB**（`kL1MaxBodySize`）。

**原因**：
- 大文件（视频、高分辨率图片、安装包）单次拷贝消耗大量 CPU 和内存带宽。
- 少量大对象即可挤爆 L1 的 `max_bytes`，导致大量小对象被过早淘汰，命中率骤降。
- 大对象通常不是高频热点（HTML/CSS/JS 小文件才是）。

**权衡**：64KB 是一个经验值，覆盖绝大多数网页文本资源和常规图片；若业务以视频流为主，可调低阈值甚至完全禁用 L1（`configure_l1(0, 0)`）。

## 9. 线程模型

- `http_cache` 实例由 `hpserver` 持有，所有操作（`lookup`、`store`）均在 **Reactor 主线程** 执行。
- `memory_cache` 内部无任何同步原语，设计上假设单线程访问。若未来需要多线程共享 L1，需在 `http_cache` 层加锁，而不是修改 `memory_cache`。
- RocksDB 的 `DB::Get` 在当前配置下是同步阻塞调用；由于命中 L1 时直接短路返回，L2 的磁盘 IO 仅发生在 L1 miss 场景，对热点路径影响有限。

## 10. 边界与注意事项

### 10.1 L1 数据非持久化

`configure_l1()` 或进程重启会清空 L1。L1 仅作为性能加速层，不保证数据持久性。

### 10.2 当前 L2 为占位实现

`lookup` 的 L2 路径和 `store` 的 L2 路径均为框架占位符：
- `lookup` 查询 RocksDB 后始终返回 `kMiss`。
- `store` 未实际写入 RocksDB。

后续需补齐：
1. 用 `HttpCacheEntry` protobuf 序列化 metadata + body。
2. RocksDB `Put` / `Get` 的完整调用。
3. L2 hit 后的 L1 回填逻辑（需再次判断大小阈值）。

### 10.3 无 TTL / 过期淘汰

当前 `memory_cache` 仅有容量驱动的 LRU 淘汰，没有时间维度。HTTP 语义层面的过期（`max-age`、`Expires`）和条件验证（`304 Not Modified`）均未实现。

### 10.4 大文件的 L2 存储风险

若 L2 直接以 RocksDB value 存放大文件（几 MB 以上），LSM-tree 的写放大和 Compaction 开销会显著增加。后续大文件应考虑 **RocksDB 存索引 + 文件系统存 body** 的分流方案。

## 11. 未来扩展方向

| 方向 | 优先级 | 说明 |
|------|--------|------|
| L2 完整实现 | 高 | 用 `HttpCacheEntry` protobuf 存 metadata，RocksDB 存 key → entry |
| TTL / 新鲜度 | 高 | 解析 `Cache-Control` 和 `Expires`，在 `lookup` 中做过期判断 |
| 条件请求 | 高 | 支持 `If-Modified-Since` / `If-None-Match`，返回 `kNotModified` |
| L2 → L1 回填 | 中 | L2 命中后，小对象写回 L1，加速下一次访问 |
| Vary 头支持 | 中 | 根据响应 `Vary` 头扩展 cache key 的组成字段 |
| 大文件外存 | 中 | 大对象 body 落文件系统，RocksDB 只存路径和 metadata |
| L1 指标暴露 | 低 | 命中率、当前条目数、当前字节数等统计 |

## 12. 文件位置

- L1 实现：`src/util/memory_cache.h`
- L1 测试：`tests/memory_cache_test.cpp`
- 缓存封装接口：`src/net/http/http_cache.h`
- 缓存封装实现：`src/net/http/http_cache.cpp`
- Protobuf 定义（生成文件）：`src/net/http/http_cache.pb.h`
- 本文档：`doc/http_cache.md`
