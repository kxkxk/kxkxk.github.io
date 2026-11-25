---
title: "为什么磁盘停顿 500 ms，读 P99 却只有 1 ms？"
description: "用可运行的 FIFO 模型演示未完成请求与 coordinated omission 如何掩盖读尾延迟。"
slug: linux-io-tail-latency-blind-spots
date: 2026-11-25
categories:
    - 技术
tags:
    - Linux
    - fio
    - 性能测试
math: false
draft: false
---

例子是可复现的合成模型，不是 fio 或设备实测。用于分析「后台写入时，前台随机读的尾延迟测准了吗」。

## 两种不同的漏样

**第一种：请求已发出，还没完成。** 完成直方图只记录结束的请求。窗口内如果只有健康的 I/O 完成，P99 看起来正常；如果什么都没完成，此窗口没有 P99 样本。补看当前在途数、完成数增量和最老在途请求的年龄。Linux 6.6 原生 blk-mq 下，磁盘统计第 9 项是在途快照，第 11 项在完成时累计已记账请求的耗时；一个 410 ms 请求若在最后一个 100 ms 窗口完成，单它就能让该窗口的 `aqu-sz` 约为 4.10，即使采样时已经没有在途 I/O。这不是该窗口内实际同时排了 4 个请求。

**第二种：压测端根本没发出本应到达的请求。** 如果客户端必须等前一个请求完成，停顿期间它自己就降了负载。即使等所有已提交的 I/O 都完成，也找不回未生成的请求。当真实需求会独立于完成持续到来时，这种漏样叫 *coordinated omission*。如果实际业务本来就是一问一答，完成驱动仍是有意义的负载模型，不需要假装每种压测都应该开环。

## 一个可复现的反例

同一个 FIFO 服务端，每个请求服务 1 ms，t=2000 到 2500 ms 之间不开始服务。两种客户端在正常时都每 10 ms 产生一个请求：第一个最多一个在途，下一次产生时间是「上次到达 + 10 ms」和「上次完成」的较晚者；第二个按固定时间表产生需求，可以排队。统计 10 s 内产生的请求，按到达至完成算延迟，P99 取排序后的第 `ceil(0.99 × N)` 个样本。

| 客户端 | 样本数 | P99 | 最大值 | 超过 100 ms |
| --- | ---: | ---: | ---: | ---: |
| 等完成后再发 | 951 | 1 ms | 501 ms | 1 |
| 到达独立于完成 | 1000 | 411 ms | 501 ms | 45 |

无停顿对照：两者都是 1000 个样本，P99 和最大值均为 1 ms。相同最大值说明两个客户端都看见了暂停；P99 差异来自后续请求有没有进入样本，而不是磁盘突然恢复得更快。

### 最小复现

保存为 `repro.cjs`，运行 `node repro.cjs`。

```js
const assert = require("node:assert/strict");

function sample({ gated, stall }) {
  let done = 0;
  const lat = [];
  for (let arrival = 0; arrival < 10_000;) {
    let start = Math.max(arrival, done);
    if (stall && start >= 2_000 && start < 2_500) start = 2_500;
    done = start + 1;
    lat.push(done - arrival);
    arrival = gated ? Math.max(arrival + 10, done) : arrival + 10;
  }
  lat.sort((a, b) => a - b);
  return [lat.length, lat[Math.ceil(0.99 * lat.length) - 1],
    lat.at(-1), lat.filter(x => x > 100).length];
}

assert.deepEqual(sample({ gated: true,  stall: true  }), [951, 1, 501, 1]);
assert.deepEqual(sample({ gated: false, stall: true  }), [1000, 411, 501, 45]);
for (const gated of [true, false])
  assert.deepEqual(sample({ gated, stall: false }), [1000, 1, 1, 0]);
console.log("4 model checks passed");
```

## 用在读写混合压测里

1. 先明确前台是固定并发，还是独立到达。把读独跑和读写混跑的前台计划负载、后台写入阶段、采样窗口对齐。fio 的配置深度不是实测深度，也不是到达率。fio 3.38 文档明确提示 `io_submit_mode=offload` 不能可靠地与异步引擎搭配，不能把它直接加到 libaio 当成已验证的解法。
2. 独立到达时，记录计划到达、实际提交和终态时间；同时报计划量、已发、已完成、发晚、掉弃与超时、实际深度。尾延迟从计划到达算起，可以再把提交前排队和提交后完成时间拆开。没完成或未提交的请求不要悄悄算进「成功样本」；用全部到达作为分母单报截止时间内成功比例。
3. 分层定位：如果应用 P99 上升而盘层完成直方图没变，先查完成量是否下降及最老在途年龄，再区分应用排队、块层等待与设备服务。Linux 的 `block_rq_complete` 可能只是部分完成，不宜无条件移除在途记录；重排队时根据要测的是单次派发还是整个请求选择是否保留首次时间。SPDK 裸盘则在进程自己的请求边界记录年龄，不用内核块层统计代替。

来源：[Linux 6.6 块请求记账](https://github.com/torvalds/linux/blob/v6.6/block/blk-mq.c)、[统计导出](https://github.com/torvalds/linux/blob/v6.6/block/genhd.c)、[块层 tracepoint](https://docs.kernel.org/6.6/core-api/tracepoint.html)、[fio 3.38 提交模式](https://kernel.googlesource.com/pub/scm/linux/kernel/git/axboe/fio/%2B/refs/tags/fio-3.38/HOWTO.rst#3325)。相关笔记：[SPDK 裸盘 iostat](/p/spdk-nvme-userspace-iostat/)。
