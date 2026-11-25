---
title: "FoundationDB：提交结果未知时，怎样避免重复加一"
description: "用稳定请求 ID 和事务内去重标记，区分普通冲突重试与提交结果未知。"
slug: foundationdb-retry-idempotency
date: 2026-11-25
categories:
    - 技术
tags:
    - FoundationDB
    - 事务
    - 幂等
math: false
draft: false
---

扩展[博客文章](https://kxkxk.github.io/p/foundationdb-transactions-resolvers-logging/)中对重试和原子操作的讨论。本文是设计笔记，未在实际集群注入故障。

## 先区分两类重试

- 热点读改写：两个事务都读取旧值，再据此更新。能表达成盲 `atomic ADD` 时，可以移除计数器的普通读依赖；物理热点和其他读依赖仍在。
- `commit_unknown_result`：第一次事务可能已经提交，但客户端没收到确定结果。盲加法不是幂等的，重跑同一个逻辑请求可能再加一次。

## 把应当冲突的请求放在同一个键上

在重试循环之外为每个逻辑操作选定稳定 ID。键可以按 `(namespace, request_id)` 编码。应用在同一个 FDB 事务中：

1. **普通读**该 ID 的标记键。若存在，核对它对应同一个不可变请求（例如目标计数器和增量），然后返回已保存的结果或已执行状态。同 ID、不同请求应报错。
2. 若标记不存在，盲加计数器，再写入标记。业务变更和标记一起提交。

如果两个尝试都普通读到标记不存在，先提交者写入的标记会与后提交者的读冲突范围相交。后者重试便能看到标记。不能把这个普通读换成单独的 snapshot read，也不能禁用标记写入的冲突范围。不同请求 ID 之间没有这条标记依赖，但仍可能因其他读取或业务约束冲突。

## 客户端重试和请求重投递不是一回事

调用方如果可能重新发起整个请求，或者消息系统会重投递，必须沿用相同的 ID；只在一次 SDK 调用内生成随机 ID 不够。标记要保留到约定的重试窗口结束，并考虑延迟到达和仍在执行的尝试。超过保留期限要定义行为，不能声称无限期去重。此方案只覆盖同一事务里的 FDB 修改，不能直接给发消息、外部 RPC 等副作用提供同样的保证。

## 如何验证

假设计数器从 0 开始：A 提交加 1，但响应丢失；B 提交加 1；随后并发重试 A。只要 A 沿用 ID、参数一致且标记仍在，预期最终值为 2。再加三个对照：为 A 换新 ID、过早清理标记、同 ID 改增量。分别验证重复执行风险和参数不一致的拒绝行为。以上是建议用例，不是实测结果。

## 参考资料

- [FoundationDB Developer Guide](https://apple.github.io/foundationdb/developer-guide.html#transactions-with-unknown-results)；[Automatic Idempotency 文档的固定版本](https://github.com/apple/foundationdb/blob/04c62ec94138a5e831620fff4f9f8a0a63ef23b0/documentation/sphinx/source/automatic-idempotency.rst)。后者在该版本将自动幂等标为实验特性，不能替代应用对跨调用的 ID、参数及保留期的定义。
