---
name: summarize-project-run-flow
description: "总结 hpserver 运行流程。用于说明启动顺序、Reactor 循环、请求生命周期、代理分流、CONNECT 隧道、定时器回收与线程所有权。关键词: 运行流程, 请求链路, event loop, reactor, proxy, CONNECT, shutdown。"
argument-hint: "范围: 启动流程 | 请求生命周期 | 代理链路 | 全链路(默认)"
user-invocable: true
---

# 总结项目运行流程

## 产出目标
输出一份基于源码证据的 hpserver 运行流程说明，覆盖从进程启动到连接关闭的全路径，并默认包含完整代理链路（普通 HTTP 代理与 CONNECT 隧道），同时标明 Reactor 与 Worker 线程的所有权边界。

## 适用场景
- 需要讲清项目运行主线与请求完整生命周期。
- 需要定位请求从接入、解析、代理到回写和关闭的路径。
- 需要解释 event loop、epoll、定时器与连接清理如何协作。
- 需要说明线程边界与并发安全约束。

## 输入参数
- 范围：启动流程、请求生命周期、代理链路、全链路（默认）。
- 深度：快速概览或详细追踪。

## 执行步骤
1. 确认范围与深度，未指定时默认输出全链路并包含完整代理路径。
2. 按以下顺序采集证据：
   - 文档：doc/loop.md、doc/http_conn.md
   - 入口与主控：src/main.cpp、src/hpserver.cpp、src/hpserver.h
   - HTTP 编排：src/net/http/http_conn.cpp、src/net/http/http_conn.h
   - 代理路径：src/net/proxy/*、doc/proxy_agent_runbook.md
3. 先构建主生命周期阶段：
   - 进程启动与监听初始化
   - accept 与连接注册
   - 读入与协议解析
   - 路由与代理分流
   - 回写与刷出
   - 超时与清理
4. 明确分支逻辑：
   - 普通 HTTP 代理：解析请求 -> 上游转发 -> 响应回写 -> 连接策略收尾
   - CONNECT 隧道：握手建立 -> 双向转发 -> 半关闭/异常收尾
   - 错误与超时：重试条件、关闭条件、关闭责任归属
5. 提取关键判定点：
   - EPOLLET 下读写是否持续到 EAGAIN/EWOULDBLOCK
   - EINTR 的重试语义与边界
   - keep-alive 与 close-after-flush 的切换条件
   - 仅 Reactor 线程可变更的连接状态
6. 输出两层结果：
   - A 层：5-8 步的中文主线
   - B 层：每步对应文件与关键符号
7. 依据验收清单自检后再输出。

## 完成标准
- 包含入口点与首次事件循环接管。
- 明确主要所有权切换（Reactor 与 Worker）。
- 全链路场景下必须同时覆盖普通 HTTP 代理与 CONNECT 隧道。
- 说明超时、清理、关闭触发条件与执行位置。
- 结论不与源码和测试行为冲突。

## 输出格式
1. 一段式高层总览。
2. 5-10 步编号流程。
3. 条件 -> 行为 的决策表。
4. 证据不足时列出风险与未知项。

## 质量要求
- 优先采用源码可证实行为，路线图文档仅作补充。
- 时间顺序严格，避免把初始化与收尾混写。
- 分支条件必须显式表达，不做隐含推断。
- 说明关键不变量（fd 边界、线程所有权、ET drain 语义）。
