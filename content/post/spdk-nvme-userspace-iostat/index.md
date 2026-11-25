---
title: "为 SPDK 裸盘实现 iostat：队列深度、忙碌时间和延迟"
description: "从队列面积和忙碌时间推导指标，并用合成输入复现 sysstat -z 的显示行为。"
slug: spdk-nvme-userspace-iostat
date: 2026-11-25
categories:
    - 技术
tags:
    - SPDK
    - NVMe
    - iostat
math: false
draft: false
---

已复核记账公式，并用合成快照实测 sysstat v12.7.6 的两个输入布局；未在真实 NVMe 上验证采集开销或告警阈值。

## 先定统计边界

当 NVMe 由 SPDK 用户态驱动直接管理、I/O 不经过 Linux 块层时，内核的 `/proc/diskstats` 不会收到这些请求的计数。使用 `O_DIRECT` 访问内核块设备不是同一个场景。若经过 SPDK bdev，先看 `bdev_get_iostat` 和队列深度采样接口；若直接调用 SPDK NVMe API，就在自己的 I/O 封装层做记账。不要把 bdev、qpair 和 SSD 内部队列的统计当成同一个量。见 [SPDK NVMe 驱动](https://spdk.io/doc/nvme.html) 和 [SPDK JSON-RPC](https://spdk.io/doc/jsonrpc.html#bdev_get_iostat)。

这里的请求从**应用确认提交成功**计到**应用处理完成回调**，因此延迟可能包含驱动排队及轮询等待，不能当成介质服务时间。业务层排队另计；提交失败不增加未完成数。已受理请求无论成功还是失败，最终只减一次；超时通知不等同于请求生命周期结束。吞吐与平均延迟在本文仅统计成功完成，错误单独计数。

## 从累计量得到区间指标

每个事件（成功受理、终结、快照）先用旧的未完成数 `Q` 更新累计量，再修改状态；时基用单调时钟：

```text
dt = now - last
queue_ns += Q * dt
busy_ns  += (Q > 0 ? dt : 0)
last = now
```

两个快照之间，令采样间隔为 `T_s` 秒或 `T_ns` 纳秒；按读写分别计算：

| 指标 | 计算 |
| --- | --- |
| IOPS | Δ成功完成数 / T_s |
| KiB/s | Δ成功字节数 / 1024 / T_s |
| await（ms） | Δ成功请求累计耗时(ns) / Δ成功完成数 / 10⁶ |
| aqu-sz | Δqueue_ns / T_ns |
| busy %（类似 %util） | 100 × Δbusy_ns / T_ns |

零完成时 await 和延迟分位数无样本；但只要还有在途请求，快照就应继续推进 queue 和 busy。读取有代际标识的累计快照，重启或设备重建时丢弃跨代差分；采集失败和快照过期要能单独识别。长时间无完成时，结合 oldest inflight age 和 poller 心跳排查，不能只等 P99 变高。

## 两个队列，每个窗口各一秒

合成序列：A 在 100 ms 受理、400 ms 成功完成一次 4 KiB 读；B 在 250 ms 受理、1100 ms 失败结束。

| 窗口 | 成功读 IOPS | 读 await | aqu-sz | busy |
| --- | ---: | ---: | ---: | ---: |
| [0, 1000) ms | 1 | 300 ms | 1.05 | 90% |
| [1000, 2000) ms | 0 | 无样本 | 0.10 | 10% |

首窗累计队列面积为 300 + 750 = 1050 请求·ms，忙碌时间取区间并集，只有 900 ms。次窗虽然没有成功完成，B 仍在途了 100 ms。上述积分已用合成事件计算复核，**不是设备跑分**。

多线程时让所属线程发布各自的快照，避免逐 I/O 争用全局锁。对齐窗口后，次数、字节和队列面积可以相加；平均延迟应以完成数加权，P99 应先合并使用同一桶边界的直方图再计算。**各 qpair 的忙碌时间不能相加**：要取各忙区间并集，或对盘级状态统一记账。若用间隔采样估算，标明“估算”，并验证是否漏掉短突发。对并行 NVMe，即使 busy 达到 100%，也只说明始终有未完成请求，不能证明盘已饱和。

### 窗口积分不等于 Linux 6.6 的完成时记账

以 Linux v6.6 的 blk-mq 路径为例，符合统计条件的请求结束时才把该请求的耗时累加到 `nsecs`，导出的第 11 字段把读、写、discard 和 flush 的累计耗时相加后换算成毫秒。见 [完成记账](https://github.com/torvalds/linux/blob/v6.6/block/blk-mq.c#L919-L935) 和 [字段导出](https://github.com/torvalds/linux/blob/v6.6/block/genhd.c#L905-L927)。因此这里的窗口积分即使通过 `iostat -f` 显示，也不承诺与原生 Linux v6.6 的短窗 `aqu-sz` 数值一致。

假设唯一请求在 0.5 s 受理、1.5 s 完成，两个采样窗口分别为 [0, 1) s 和 [1, 2) s；在相同请求计时边界且不考虑舍入的模型下，积分得到 0.5、0.5，完成记账得到 0、1.0。前者适合显示当前窗口积压；严格比较 Linux 口径时应使用单独的完成累计耗时。这是由源码推导出的用例，不是硬件测量。

## 兼容显示与待验证

支持 `-f` 的 sysstat 可以从自定义目录读取兼容的 `block/<device>/stat`。最小读写方案按顺序导出：读完成数、读合并数、读扇区数、累计读耗时 ms；写对应四项；然后是在途数、忙碌 ms、累计队列面积（请求·ms）。不计合并就填 0；扇区按 **512 字节**换算而非设备 LBA 大小；累计量在导出时再转成毫秒。使用同目录临时文件与原子 rename 防止半行读取；需要精确采样间隔时，优先让 RPC 提供实际快照时间。字段形状相同不表示语义与任一内核版本完全一致。见 [Linux I/O 统计](https://docs.kernel.org/admin-guide/iostats.html) 与 [sysstat iostat 手册源码](https://github.com/sysstat/sysstat/blob/master/man/iostat.in)。

下文验证跨窗口且无成功完成的显示；仍需单独检查重启归零和计数回绕，并用 SPDK 只读负载对比开启与关闭采集时的 IOPS、P99 和每核开销。

### sysstat v12.7.6：两种输入布局与 `-z` 实测（无设备）

从上游 v12.7.6 tag 构建 `iostat`，用 `iostat -V` 核对版本。在临时目录写两份合成累计快照，**不访问真实 NVMe**。普通模式用 `LC_ALL=C iostat -dx -f "$dir" ALL 2 2`，读 `$dir/block/spdk-test/stat`：

```text
1 0 8 300 0 0 0 0 1 900 1050
1 0 8 300 0 0 0 0 0 1000 1150
```

先写第一行作为完整文件；启动时用 `stdbuf -oL` 按行读输出，看到第一条 `spdk-test` 数据后，立刻用同目录临时文件和原子 rename 把内容换成第二行。加 `-z` 前，每次重跑都先恢复第一行。另以 `-p ALL` 单独重复：命令为 `LC_ALL=C iostat -dx -f "$dir" -p ALL 2 2`，输入改成 `$dir/diskstats`，两份快照各在对应行前加 `240 0 spdk-test `（末尾有空格），不要同时放两个布局。

| 布局 | 不加 `-z` 的第二期 | 加 `-z` 的第二期 |
| --- | --- | --- |
| `block/spdk-test/stat` | r/s = 0.00，w/s = 0.00，aqu-sz = 0.05，%util = 5.00 | 设备行不显示 |
| `diskstats` + `-p ALL` | r/s = 0.00，w/s = 0.00，aqu-sz = 0.05，%util = 5.00 | 设备行不显示 |

四组均执行通过。两个快照之间累计忙碌时间和队列面积各增 100 ms，完成数不变，显示值符合 2 秒采样的预期。**排查“还在途但没有完成”的设备，不要用 `-z` 判断设备是否空闲。** 第一份报告是用于建立差分的初值，不拿它的开机平均值作对比。

v12.7.6 的 [采样分发](https://github.com/sysstat/sysstat/blob/v12.7.6/iostat.c#L1915-L1938)切换输入解析器；[设备文件读取](https://github.com/sysstat/sysstat/blob/v12.7.6/iostat.c#L395-L453)可能把已打开但格式不合法的行记为零。因此不能直接截断旧文件再写。原子 rename 只保证单个文件完整，多设备一致快照需要在同一份聚合文件中发布同一批快照。用 `+f` 合并系统设备与导出设备时使用不同名字，避免 [同名合并](https://github.com/sysstat/sysstat/blob/v12.7.6/iostat.c#L274-L301)。[扩展输出](https://github.com/sysstat/sysstat/blob/v12.7.6/iostat.c#L1455-L1461)对累计队列计数下降有单独处理；代际重启与计数回绕仍需分开验证。本节未测试回绕、重启归零、其他 sysstat 版本或硬件上的采集开销与告警阈值。
