---
title: "RDMA 实验手册：OFED 安装、工具使用、打流与监控"
description: "安装 NVIDIA MLNX_OFED / DOCA-OFED，使用 show_gids、ib_write_bw 和时延测试工具，并通过 sysfs、ethtool、RDMA counters 定位真实流量与拥塞。"
slug: rdma-ofed-tools-monitoring
date: 2026-09-12T15:41:36+08:00
image: cover.png
categories:
    - 技术
tags:
    - RDMA
    - OFED
    - RoCE
    - InfiniBand
    - perftest
    - 网络监控
draft: false
---

这篇文章把 RDMA 实验的操作顺序串起来：**选驱动栈 → 安装工具 → 找准设备、端口与 GID → 跑通双机 → 测带宽和时延 → 用硬件计数器解释结果**。默认讨论两台 Linux 主机上的 NVIDIA/Mellanox ConnectX 网卡，使用主机内存和 RC 传输。

基础概念、verbs 和拥塞算法原理见上一篇：[RDMA 从入门到设计](/p/rdma-verbs-congestion-demo/)。这里主要回答“装哪个包、执行什么命令、输出是什么意思”。

先纠正一个容易混用的名字：perftest 的写带宽工具是 **`ib_write_bw`**，不是 `ibv_write_bw`；`ibv_devinfo`、`ibv_devices` 才带 `ibv_` 前缀。[perftest 上游](https://github.com/linux-rdma/perftest)

命令中的网卡名、IP、GID 索引和下载文件名都需要换成本机值。示例输出是格式说明，不是特定硬件的实测成绩。封面为参考拼贴设计生成的筱泽广主题 AI 插画。

## 选择 MLNX_OFED、DOCA-OFED 还是发行版驱动

| 路线 | 包含什么 | 适用场景 |
|---|---|---|
| 发行版 inbox 驱动 + rdma-core | 内核自带 RDMA 驱动、发行版维护的用户态库和工具 | 功能已满足，需要沿用系统维护周期 |
| MLNX_OFED | NVIDIA/Mellanox 提供的内核驱动、用户态库、工具，传统 ISO/TGZ 安装方式 | 维护已锁定 MLNX_OFED 版本的环境 |
| DOCA-OFED | DOCA-Host 中提供 MLNX_OFED 类似内容的安装 profile | 需要 NVIDIA OFED 驱动与工具的新部署或迁移 |

NVIDIA 已将 MLNX_OFED 延续到 DOCA-Host；希望保留传统 OFED 使用方式时，对应 profile 是 `doca-ofed`。这不表示必须购买 BlueField，ConnectX 主机也可使用。`doca-networking`、`doca-all` 覆盖更多功能，不能把所有 profile 当成同一个安装包。[NVIDIA 迁移指南](https://docs.nvidia.com/doca/sdk/nvidia-mlnx-ofed-to-doca-ofed-transition-guide.pdf)、[DOCA Profiles](https://networking-docs.nvidia.com/doca/archive/3-5-0/doca-profiles)

**先选一条驱动/库维护路线，再安装测试工具。** 在已装厂商 OFED 的主机上，直接从另一个源替换 `libibverbs`、provider 或内核 RDMA 包，可能形成版本混装。选择软件包时要同时检查网卡型号、固件、操作系统、运行内核和 CPU 架构，不能只看“Ubuntu”或“x86”。[DOCA 安装与支持矩阵入口](https://networking-docs.nvidia.com/doca/archive/3-5-0/doca-installation-guide-for-linux)

## 安装前确认机器与软件状态

```bash
cat /etc/os-release
uname -r
uname -m
lspci -nn | grep -Ei 'Mellanox|NVIDIA.*(Ethernet|InfiniBand)'
ip -br link
ip -br address

# 已存在这些命令时再执行
ofed_info -s
ibv_devices
rdma link show
```

对已识别的 Ethernet 网口查询驱动与固件，例如：

```bash
ethtool -i enp65s0f0
modinfo -n mlx5_core
modinfo -n mlx5_ib
dkms status
```

`modinfo -n` 指向磁盘上的模块文件，单独它不能证明内核当前已经加载这份新模块。软件安装完成后还需要按该版本说明重新加载或重启，再核对设备状态。

安装 OFED 会替换网络相关组件，重新加载 `openibd` 会影响该网卡上的连接。应在可使用本地控制台或带外管理的维护窗口完成，提前保存网口、地址、路由与原软件包版本。本文给出安装命令，不把安装动作夹在打流脚本中。

## 安装 NVIDIA DOCA-OFED

### 获取匹配的官方包

从 [NVIDIA DOCA 下载页](https://developer.nvidia.com/doca-downloads) 选择 **Host** 软件、发行版、版本与架构，下载相应的 DEB 或 RPM 仓库包。普通 ConnectX 主机安装 Host 软件，不需要照着 BlueField 的 BFB 刷机流程操作。

以下是本地仓库包方式：先安装仓库，再通过系统包管理器安装 `doca-ofed`。它与“直接装完一个 DEB 就拥有全部驱动”不同；完全离线部署还需准备系统依赖及签名材料。[DOCA-Host 安装说明](https://networking-docs.nvidia.com/doca/archive/3-5-0/doca-host-installation-and-upgrade)

### Ubuntu / Debian 系发行版

只有目标发行版与内核在所选版本的支持范围内，才采用对应安装包。文件名中的占位符需要替换。

```bash
# 准备与当前运行内核匹配的构建依赖
sudo apt-get update
sudo apt-get install build-essential dkms "linux-headers-$(uname -r)"

# 安装已经下载的 NVIDIA Host 仓库包
sudo dpkg -i './doca-host_<匹配版本与平台>_amd64.deb'
sudo apt-get update

# 先看候选版本与包来源，再安装 OFED profile
apt-cache policy doca-ofed
sudo apt-get install doca-ofed
```

ARM64 需要对应架构的包；不能只把 x86 文件名的后缀改掉。若包仓库报签名错误，应按该发布版本更新官方 keyring，保留签名验证。

### RHEL / Rocky 等 RPM 系发行版

```bash
sudo dnf install gcc make "kernel-devel-$(uname -r)" dkms
dkms --version

sudo rpm -Uvh './doca-host-<匹配版本与平台>.x86_64.rpm'
sudo dnf makecache
dnf info doca-ofed
sudo dnf install doca-ofed
```

包管理命令相似，不代表所有 RHEL 衍生发行版自动得到相同支持。本文核对的 DOCA 3.5 安装文档要求 RPM 系统具备 **DKMS ≥ 3.2**；若发行版默认仓库不满足，先按该版本支持说明补齐依赖，不要无视版本要求继续安装。[DOCA-Host 前置要求](https://networking-docs.nvidia.com/doca/archive/3-5-0/doca-host-installation-and-upgrade)

### 固件、Secure Boot 和驱动生效

官方安装流程还包括 `mlnx-fw-updater` 等固件工具。固件升级需匹配网卡型号、PSID/OEM 和软件矩阵，按安装器提示执行所需的驱动重载、重启或冷启动；仅更新软件包不会自动证明运行固件已切换。

```bash
# 仅在本次维护计划包含匹配固件升级时执行
sudo apt-get install mlnx-fw-updater
# RPM 系使用 sudo dnf install mlnx-fw-updater
```

需要重新加载驱动、且已停止依赖这些设备的业务时：

```bash
sudo /etc/init.d/openibd restart
```

启用 Secure Boot 时，还要处理 DKMS 模块签名和 MOK 注册。当前 DOCA 文档给出的典型公钥位置是 `/var/lib/dkms/mok.pub`；以本机 DKMS 配置为准，完成导入和重启时的注册流程。不要把关闭 Secure Boot 当成缺失签名的固定解决步骤。[官方签名与生效流程](https://networking-docs.nvidia.com/doca/archive/3-5-0/doca-host-installation-and-upgrade)

已有 MLNX_OFED 时，按原版本卸载说明与迁移指南先清理旧栈，再安装新 profile；不要将网上的通配符删除命令直接用于生产主机。

## 安装传统 MLNX_OFED

### TGZ 安装包

从 [MLNX_OFED 官方下载页](https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/) 选择目标版本、OS 和架构。下载包通常名为 `MLNX_OFED_LINUX-<版本>-<OS>-<架构>.tgz`。

```bash
tar -xzf 'MLNX_OFED_LINUX-<版本>-<OS>-<架构>.tgz'
cd 'MLNX_OFED_LINUX-<版本>-<OS>-<架构>'
./mlnxofedinstall --help

# 示例：此次先安装软件，明确不自动更新固件
sudo ./mlnxofedinstall --without-fw-update
```

默认安装器可能尝试更新固件；`--without-fw-update` 明确将这件事排除在本次软件安装之外。安装器会报告冲突包、日志路径及后续操作，应先理解这些输出。[MLNX_OFED 官方安装说明](https://docs.nvidia.com/networking/display/mlnxofedv23102131201lts/installing-mlnx-ofed.pdf)

### ISO 安装包

```bash
sudo mkdir -p /mnt/mlnx-ofed
sudo mount -o ro,loop \
    'MLNX_OFED_LINUX-<版本>-<OS>-<架构>.iso' /mnt/mlnx-ofed
sudo /mnt/mlnx-ofed/mlnxofedinstall --without-fw-update
sudo umount /mnt/mlnx-ofed
```

### 内核不匹配时怎么办

先检查当前内核是否在该发布版本支持范围内，以及 headers/devel 是否匹配。传统 MLNX_OFED 的 RPM 路径可能需要 `--add-kernel-support` / `mlnx_add_kernel_support.sh`；Ubuntu/Debian 常走 DKMS，不能机械套用同一组参数。新的 DOCA 路径也已更多依赖 DKMS，应读对应版本说明。

“能编出模块”不等于“任意内核受厂商支持”。不要把 `--force`、`--skip-distro-check`、`--skip-unsupported-devices-check` 拼成默认安装命令。[MLNX_OFED 内核适配说明](https://docs.nvidia.com/networking/display/mlnxofedv23102131201lts/installing-mlnx-ofed.pdf)

### 安装后看什么

```bash
ofed_info -s
ibv_devinfo
rdma link show
lsmod | grep -E 'mlx5_core|mlx5_ib|ib_core|rdma_cm'
dmesg -T | grep -Ei 'mlx5|infiniband|rdma|dkms' | tail -80
```

DOCA 版本若提供 `doca-info`，还可运行：

```bash
/opt/mellanox/doca/tools/doca-info
```

查看的是一条链：**软件包已安装 → 驱动已加载 → 设备可枚举 → 端口状态正确 → 双机路径可通信**。`ofed_info -s` 只覆盖这条链的一部分。

## 工具分别由什么包提供

| 工具 | 常见来源 | 用途 |
|---|---|---|
| `ib_write_bw`、`ib_read_bw`、`ib_send_bw` | `perftest` | WRITE / READ / SEND 吞吐 |
| `ib_write_lat`、`ib_read_lat`、`ib_send_lat` | `perftest` | 相应操作的延迟基准 |
| `ibv_devices`、`ibv_devinfo`、`ibv_rc_pingpong` | rdma-core 的 utilities；DEB 常见 `ibverbs-utils`，RPM 常见 `libibverbs-utils` | 枚举设备、能力和基本通信 |
| `rping` | rdma-core / librdmacm utilities | CM 建连与 RDMA ping-pong |
| `show_gids` | NVIDIA/Mellanox `mlnx-tools` | GID、类型、索引与网口映射 |
| `ibdev2netdev` | OFED/发行版相关工具包，随发行版打包变化 | RDMA 设备到 netdev 映射 |
| `ibstat`、`perfquery`、`iblinkinfo` | `infiniband-diags` | IB 端口与 Fabric 诊断 |
| `rdma` | iproute2；部分 OFED 另带厂商版本 | RDMA 链路、资源和统计 |
| `ethtool`、`ip`、`numactl` | 对应发行版工具包 | 网卡信息、网络配置、NUMA 绑定 |
| `mst`、`mlxlink`、`mlxconfig` | NVIDIA MFT | 设备管理、物理链路、配置查询 |

可用 `dpkg -S /实际/命令路径` 或 `rpm -qf /实际/命令路径` 确认已安装文件的归属，不要仅凭博客里的包名判断来源。[rdma-core](https://github.com/linux-rdma/rdma-core)、[perftest](https://github.com/linux-rdma/perftest)、[mlnx-tools 包清单](https://github.com/Mellanox/mlnx-tools/blob/8fbfd9cfff34e2d32f1dd5a115c42eb8bd3b66c8/mlnx-tools.spec)

### 使用发行版 RDMA 栈安装工具

以下是一组常见 Ubuntu/Debian 包名，适用于选择发行版维护路线的主机：

```bash
sudo apt-get update
sudo apt-get install rdma-core ibverbs-providers ibverbs-utils \
    rdmacm-utils infiniband-diags perftest iproute2 ethtool numactl
```

RHEL 系常见对应组合：

```bash
sudo dnf install rdma-core libibverbs-utils librdmacm-utils \
    infiniband-diags perftest iproute ethtool numactl
```

已使用 NVIDIA OFED/DOCA 时，优先使用该栈自带或同源的工具。补装前查看候选包与依赖解析，避免为了一个测试程序替换整套 verbs 库。

### 安装与使用 show_gids

先查找现有命令，包括普通用户 PATH 可能未包含的 sbin：

```bash
command -v show_gids
ls /usr/sbin/show_gids /sbin/show_gids 2>/dev/null
```

若已配置提供它的 NVIDIA 仓库，可查询并安装 `mlnx-tools`：

```bash
apt-cache policy mlnx-tools
sudo apt-get install mlnx-tools
# RPM 系：dnf info mlnx-tools；sudo dnf install mlnx-tools
```

如果只缺这个只读脚本，也可以从 NVIDIA/Mellanox 官方仓库取固定版本，查看内容后单独使用，不需要因此安装全部 OFED：

```bash
curl -fL \
  'https://raw.githubusercontent.com/Mellanox/mlnx-tools/8fbfd9cfff34e2d32f1dd5a115c42eb8bd3b66c8/sbin/show_gids' \
  -o show_gids
less show_gids
bash ./show_gids
bash ./show_gids mlx5_0

# 需要放入 PATH 时再安装，保留脚本中的版权说明
sudo install -m 0755 ./show_gids /usr/local/bin/show_gids
```

`show_gids` 主要读取 sysfs；**它不会创建 GID，也不会替你配置 IP、RoCE v2 或路由**。[官方脚本源码](https://github.com/Mellanox/mlnx-tools/blob/8fbfd9cfff34e2d32f1dd5a115c42eb8bd3b66c8/sbin/show_gids)

### 需要更新的 perftest 时从源码安装

两端使用相同版本或同一提交。下面固定了本文核对的上游提交，也可换成团队选定的 release tag：

```bash
# 发行版库路线的开发依赖示例；OFED 用户选择当前栈的对应开发包
sudo apt-get install build-essential autoconf automake libtool pkg-config \
    libibverbs-dev librdmacm-dev libibumad-dev libpci-dev

git clone https://github.com/linux-rdma/perftest.git
cd perftest
git checkout b513a77278c8061ca6c4dcd1a95d08801c6e7623
./autogen.sh
./configure --prefix=/opt/rdma-perftest
make -j4
sudo make install

/opt/rdma-perftest/bin/ib_write_bw --version
/opt/rdma-perftest/bin/ib_write_bw --help
```

这套构建只安装用户态测试程序，不替换宿主机内核驱动。若在 Docker x86 环境构建，发行版、依赖库与目标主机 ABI 要兼容；构建时不需要 RDMA 网卡，运行时才需要正确设备和 provider。使用独立前缀能避免无意覆盖系统的 `ib_write_bw`。[构建说明与依赖检查](https://github.com/linux-rdma/perftest/blob/b513a77278c8061ca6c4dcd1a95d08801c6e7623/configure.ac)

```bash
type -a ib_write_bw
ib_write_bw --version
ldd "$(command -v ib_write_bw)"
```

不同发行版、厂商版本和上游版本的选项会有差别。先以实际执行的二进制 `--help` 为准，特别留意是否调用了另一套安装目录里的工具。

## 找准设备、端口、网口和 GID

### 这几个“端口”不是同一回事

| 名称 | 示例 | 对应参数 |
|---|---|---|
| RDMA 设备 | `mlx5_0` | perftest `-d` |
| 设备端口 | `1` | `-i` |
| Ethernet netdev | `enp65s0f0` | IP、MTU、ethtool 操作对象 |
| GID 表下标 | `3` | 手工 verbs 建连时的 `-x` |
| 测试控制连接端口 | `18515` | `-p` |
| RoCE v2 UDP 目的端口 | `4791` | 网络封装使用，**不是**把 perftest `-p` 改成它 |

RoCE v2 的封装与网口/GID 类型关系见 [NVIDIA RoCE 说明](https://docs.nvidia.com/networking-ethernet-software/cumulus-linux-510/Layer-1-and-Switch-Ports/Quality-of-Service/RDMA-over-Converged-Ethernet-RoCE/)。

```bash
ibv_devices
ibv_devinfo -d mlx5_0 -i 1
rdma link show
ibdev2netdev
show_gids mlx5_0
```

`show_gids` 输出常包含两列 DEV：前一列是 RDMA 设备，后一列是关联 netdev。如下仅演示字段对应：

```text
DEV      PORT  INDEX  GID                               IPv4             VER  DEV
mlx5_0   1     3      0000:...:ffff:c0a8:640b             192.168.100.11   v2   enp65s0f0
```

部分发行版已经不再提供 `ibdev2netdev`，这时用 `rdma link show` 和上面的 GID/netdev 映射即可，不必为了旧命令换驱动栈。[RHEL 9 迁移说明](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/9/pdf/considerations_in_adopting_rhel_9/considerations-in-adopting-rhel-9.pdf)

RoCE v2 应选择**对应目标网口、IP/VLAN 且类型为 v2**的有效 GID。两台机器的索引可以不同；相同 IP 可能有不同 RoCE 类型的 GID。不要固定认为 `-x 3` 在所有机器上都正确。

不用脚本也可直接核对某个索引：

```bash
cat /sys/class/infiniband/mlx5_0/ports/1/gids/3
cat /sys/class/infiniband/mlx5_0/ports/1/gid_attrs/types/3
cat /sys/class/infiniband/mlx5_0/ports/1/gid_attrs/ndevs/3
```

GID 表会随地址/网口配置发生变化，记录实验时的实际映射。[Linux GID sysfs ABI](https://github.com/torvalds/linux/blob/master/Documentation/ABI/stable/sysfs-class-infiniband)

### IB 和 RoCE 的前提分别是什么

- **IB**：链路与端口达到可通信状态，Fabric 有正确运行的 Subnet Manager，LID、P_Key、路径与 MTU 合适。已有 SM 时先核实，不要给每个实验端点都启动一套新的管理进程。
- **RoCE v2**：Ethernet 链路、IP/VLAN/GID 和双向路径正确；全路径 MTU 相容，交换机 QoS/PFC/ECN 与端点配置匹配。RoCE 不需要 IB 的 SM。

检查示例：

```bash
ibstat
ip -br address show enp65s0f0
ip route get 192.168.100.11 from 192.168.100.12
ping -I 192.168.100.12 -c 3 192.168.100.11
```

`ping` 成功不证明 RDMA 已通，端口 ACTIVE 也不证明 GID、权限或队列连接正确。MTU 也有两层：Ethernet 网口 MTU 与 verbs 的 path MTU；不能把 Ethernet 的 `9000` 直接填进普通 verbs 的 `-m`。先读 `ibv_devinfo` 的 `active_mtu`，再决定是否显式指定。

## 先跑通双机，再打满带宽

### 确认进程的 memlock 限额

```bash
ulimit -Sl
ulimit -Hl
```

普通内存注册需要满足进程可锁定内存的限制。若硬限额已经允许，可以在**实际启动测试的同一个 shell**中用 `ulimit -l unlimited` 提高软限额；硬限额不足时需通过登录会话/服务的资源限制配置调整。systemd 服务和容器有各自的限制来源，改一个交互 shell 不会自动修改它们。具体额度要覆盖当前实验的 MR，注册失败时同时保留 errno。[Linux verbs 内存锁定说明](https://docs.kernel.org/infiniband/user_verbs.html)

### 建连模式：默认模式、-R 和 -z

| 模式 | 连接参数怎样交换 | 实际数据怎样传输 |
|---|---|---|
| 默认 | 普通 TCP 控制连接交换 QP/内存信息，工具配置 verbs QP | 仍是 RDMA，不是 TCP 搬运 payload |
| `-R` / `--rdma_cm` | RDMA CM 建立测试 QP | 在 CM 建立的 QP 上跑测试 |
| `-z` / `--comm_rdma_cm` | 用 CM 交换连接信息 | 测试使用常规方式创建的 QP；与 `-R` 不同 |

默认模式下，客户端末尾的 IP 是控制连接目标，甚至可以是管理网地址；测试数据则由 `-d/-i/-x` 等选出的 RDMA 路径决定。`-R` 模式要求地址能正确解析到 RDMA 设备与路径，不能拿一个仅管理网可达的 IP 来替代。

第一次实验选择一种模式，两端保持一致。本篇 RoCE 主线用默认模式，显式选择 GID；CM 单独给出示例。[perftest 连接参数](https://github.com/linux-rdma/perftest/blob/b513a77278c8061ca6c4dcd1a95d08801c6e7623/README)

### 用 rping 检查 CM 和数据传输

```bash
# 服务端
rping -s -a 192.168.100.11 -p 7471 -v -V

# 客户端
rping -c -a 192.168.100.11 -I 192.168.100.12 \
    -p 7471 -C 100 -S 4096 -v -V
```

`-V` 校验数据，`-C` 限制次数，`-I` 指定客户端源地址。多网卡环境中，明确源地址有助于排除路由选择偏差。rping 跑通后仍需分别测目标操作、消息大小和负载。[rping 上游手册](https://github.com/linux-rdma/rdma-core/blob/36f7ed8b9ce39c73444616c1dcee3ae734b138fb/librdmacm/man/rping.1)

### 为两台机器设置明确的参数

下面的索引 **3 和 5 仅用于演示两端可以不同**，应先从 `show_gids` 得到实际值。所有代码块按 Bash 使用。

服务端终端：

```bash
RDMA_DEV=mlx5_0
RDMA_PORT=1
RDMA_GID=3
SERVER_IP=192.168.100.11
TEST_PORT=18515
```

客户端终端：

```bash
RDMA_DEV=mlx5_0
RDMA_PORT=1
RDMA_GID=5
SERVER_IP=192.168.100.11
TEST_PORT=18515
```

两端均先执行 `ib_write_bw --version`，确认测试版本一致。`-s/-D/-n/-q/-b/-c` 等影响测试协议和负载的参数保持一致；设备名、设备端口、GID 和绑定 CPU 使用各自主机的值。

## 带宽测试：WRITE、READ、SEND

### WRITE 单向打流

先在服务端执行并保持等待：

```bash
ib_write_bw -d "$RDMA_DEV" -i "$RDMA_PORT" -x "$RDMA_GID" \
    -p "$TEST_PORT" -c RC -s 65536 -D 15 --report_gbits
```

再在客户端执行：

```bash
ib_write_bw -d "$RDMA_DEV" -i "$RDMA_PORT" -x "$RDMA_GID" \
    -p "$TEST_PORT" -c RC -s 65536 -D 15 --report_gbits "$SERVER_IP"
```

客户端发起 WRITE，把本地缓冲区写进服务端注册内存。大流量方向通常是 **客户端 TX → 服务端 RX**，反向还有协议控制流量。`-D 15` 是工具的测量时长，建连与收尾可能使进程总运行时间更长。

### READ 单向打流

服务端：

```bash
ib_read_bw -d "$RDMA_DEV" -i "$RDMA_PORT" -x "$RDMA_GID" \
    -p "$TEST_PORT" -c RC -s 65536 -D 15 --report_gbits
```

客户端：

```bash
ib_read_bw -d "$RDMA_DEV" -i "$RDMA_PORT" -x "$RDMA_GID" \
    -p "$TEST_PORT" -c RC -s 65536 -D 15 --report_gbits "$SERVER_IP"
```

**客户端仍是操作发起方，但大块 payload 从服务端返回客户端。** 所以监控时应主要看到服务端 TX、客户端 RX 增长。只盯客户端 TX，容易误判“READ 没有打满”。

### SEND 打流

将两端命令中的工具改为 `ib_send_bw`，先使用相同的大小和时长：

```bash
# 服务端
ib_send_bw -d "$RDMA_DEV" -i "$RDMA_PORT" -x "$RDMA_GID" \
    -p "$TEST_PORT" -c RC -s 4096 -D 15 --report_gbits

# 客户端
ib_send_bw -d "$RDMA_DEV" -i "$RDMA_PORT" -x "$RDMA_GID" \
    -p "$TEST_PORT" -c RC -s 4096 -D 15 --report_gbits "$SERVER_IP"
```

SEND 会消耗接收 WR。接收深度、补接收速度和双方 CPU 调度可能限制结果，与普通 WRITE 的接收路径不同。

### 双向、持续打流与大小扫描

| 目的 | 两端如何修改 |
|---|---|
| 双向 WRITE | 给 `ib_write_bw` 两端都加 `-b` |
| 多 QP | 两端都加相同的 `-q 4`，并记录总 QP 数 |
| 调整发送深度 | 两端按测试要求设置 `-t 128` 等值 |
| READ 在途数 | 查询 `ib_read_bw --help` 后使用 `-o 8` 等值，受设备能力约束 |
| 持续观察计数器 | 普通 WRITE/READ BW 使用 `--run_infinitely`，完成后结束两端进程 |
| 扫描消息大小 | 使用 `-a -n 10000`，或逐个大小重新启动两端；不要只在一端变参数 |

`--run_infinitely` 的输出间隔由工具版本和 duration 参数决定，常见默认是 5 秒。它与部分操作/选项存在限制，不要把普通 WRITE 的无限模式命令直接套到所有 WRITE_WITH_IMM/SEND 变种。

双向模式常报告两个方向的合计带宽。100 Gbit/s 全双工链路的双向汇总超过 100 Gbit/s，并不能说明单方向超过线速。[perftest 测量方法与模式约束](https://github.com/linux-rdma/perftest/blob/b513a77278c8061ca6c4dcd1a95d08801c6e7623/README)、[选项实现](https://github.com/linux-rdma/perftest/blob/b513a77278c8061ca6c4dcd1a95d08801c6e7623/src/perftest_parameters.c)

### 原生 IB 和 RDMA CM 命令怎样调整

原生 IB 的普通同子网 RC 测试一般不需要 RoCE 的 GID 索引，先省略 `-x`，保留实际设备和端口：

```bash
# 服务端
ib_write_bw -d mlx5_0 -i 1 -s 65536 -D 15 --report_gbits
# 客户端；末尾可以是双方可达的管理网地址或 IPoIB 地址
ib_write_bw -d mlx5_0 -i 1 -s 65536 -D 15 --report_gbits '<服务端控制地址>'
```

需要用 RDMA CM 建连时，两端加 `-R`，并使用能够正确选择 RDMA 路径的地址：

```bash
# 服务端
ib_write_bw -R -d mlx5_0 -i 1 -s 65536 -D 15 --report_gbits
# 客户端
ib_write_bw -R -d mlx5_0 -i 1 -s 65536 -D 15 --report_gbits '<服务端RDMA地址>'
```

这些是不同建连方式的模板；CM 路径仍需核对路由、地址族和实际 RoCE 类型。不要假设 `-R` 下一个手工 GID 参数能覆盖 CM 的所有路径选择。多地址环境中的源地址选项以本机版本帮助为准。

## 时延测试：先弄清测的是什么

### 小消息 WRITE 延迟

```bash
# 服务端
ib_write_lat -d "$RDMA_DEV" -i "$RDMA_PORT" -x "$RDMA_GID" \
    -p "$TEST_PORT" -c RC -s 64 -n 10000

# 客户端
ib_write_lat -d "$RDMA_DEV" -i "$RDMA_PORT" -x "$RDMA_GID" \
    -p "$TEST_PORT" -c RC -s 64 -n 10000 "$SERVER_IP"
```

要观察 WRITE_WITH_IMM 变体，先确认该版本的 `ib_write_lat --help` 提供 `--write_with_imm`，再在两端同时加上。它也不等于你的业务程序“收到通知后处理数据，再回复”的完整耗时。

### READ 与 SEND 延迟

相同方式分别在两端启动 `ib_read_lat` 或 `ib_send_lat`。例如 4 KiB READ：

```bash
# 服务端
ib_read_lat -d "$RDMA_DEV" -i "$RDMA_PORT" -x "$RDMA_GID" \
    -p "$TEST_PORT" -c RC -s 4096 -n 10000

# 客户端
ib_read_lat -d "$RDMA_DEV" -i "$RDMA_PORT" -x "$RDMA_GID" \
    -p "$TEST_PORT" -c RC -s 4096 -n 10000 "$SERVER_IP"
```

### 为什么 READ 和 WRITE 不能按同一个“单向时延”比较

本文核对的上游 `print_report_lat()` / `print_report_lat_duration()` 对 READ/Atomic 使用 `rtt_factor=1`，对 SEND/WRITE 使用 `rtt_factor=2`：

| 工具 | 常规测试口径 |
|---|---|
| `ib_send_lat` / `ib_write_lat` | ping-pong 周期折算的一半，依赖双向近似对称 |
| `ib_read_lat` | 一次 READ 请求与数据返回完成所需的操作周期，不额外除以二 |
| 自定义 WRITE_WITH_IMM RPC | 取决于业务打点，可包含 CQ、排队、处理和回复 |

所以 `ib_read_lat` 不能被解释为单程网络传播时间。一般性 README 对“RTT 除二”的说明较粗，具体操作应核对所用版本实现。[perftest 延迟计算源码](https://github.com/linux-rdma/perftest/blob/b513a77278c8061ca6c4dcd1a95d08801c6e7623/src/perftest_parameters.c#L5092)

### 看结果和保存样本

输出常包含消息大小、迭代数、最小值、最大值、典型值/中位数、平均值以及部分分位数；具体列以版本为准，单位通常为 μs。`t_typical` 不应直接当平均值。

```bash
# 按本机 help 选择原始输出方式，两端按要求保持一致
ib_read_lat --help
# -H：排序后的样本输出；-U：按测量顺序的样本输出
```

不要只记录最低时延。对稳定性同时看 median、P99/P99.9、最大值与 CPU；先单独预热，再重复同一组测试。超长迭代也会改变样本存储和缓存行为。需要比较业务效果时，额外测请求发出到业务响应的时间。

## perftest 常用参数速查

| 参数 | 含义 | 使用注意 |
|---|---|---|
| `-d` / `-i` | RDMA 设备 / 设备端口 | 两端使用各自实际值 |
| `-p` | 控制连接或 CM 服务端口 | 两端一致，并行实验使用不同端口 |
| `-x` | 本地 GID 索引 | RoCE 类型、地址与 netdev 必须正确 |
| `-s` | 每条操作的字节数 | 不是网口 MTU |
| `-n` / `-D` | 次数 / duration 模式 | 选择一种主测试口径 |
| `-a` | 扫描消息大小 | 时间可能很长，先看扫描范围 |
| `-b` | 双向带宽 | 注意结果是否汇总两个方向 |
| `-q` / `-t` | QP 数 / 发送队列深度 | 越大不一定越快，也影响排队 |
| `-r` | 接收深度 | SEND 等接收路径尤为相关 |
| `-o` | READ/Atomic 在途操作数 | 不要超过支持能力 |
| `-I` | inline 大小 | 仅适合支持的发送/写操作 |
| `-l` | 一次提交的 WR 链表长度 | 批量提交会改变吞吐、CPU 与突发 |
| `-Q` | CQ moderation / 完成生成间隔 | 不等同于修改网卡所有中断策略 |
| `-e` | 事件等待 | 并非每种测试均支持，不直接套用普通 WRITE |
| `--report_gbits` | 以 Gbit/s 输出带宽 | 比较结果时统一单位 |
| `--tclass` | GRH Traffic Class | RoCE 的 QoS 分类值应与网络方案一致 |
| `-T` | CM 模式相关 TOS 设置 | 与普通 verbs 的 `--tclass` 适用模式不同 |
| `-F` | 忽略/抑制 CPU 频率相关检查或提示 | **不会**把 CPU 自动设成 performance 模式 |

如需指定 DSCP，先理解 Traffic Class/TOS 的字段编码及 ECN 位，再检查 NIC/交换机映射。不要只因某篇命令写了 `--tclass 106` 就把它当通用性能参数。[perftest 参数手册](https://github.com/linux-rdma/perftest/blob/b513a77278c8061ca6c4dcd1a95d08801c6e7623/man/perftest.1)

## RDMA 流量监控：先选对观测位置

**硬件 RDMA 可以绕过普通内核网络数据路径。** `sar -n DEV`、`iftop`、`/proc/net/dev` 或普通 `tcpdump` 的结果，可能没有完整覆盖实际 RDMA payload。先区分软件 netdev、RDMA/vPort 和物理端口计数器，再判断是否真的“没流量”。[Linux mlx5 计数器分层说明](https://docs.kernel.org/networking/device_drivers/ethernet/mellanox/mlx5/counters.html)

| 观测层 | 工具 / 位置 | 能回答的问题 |
|---|---|---|
| 应用 | perftest 输出 | 该测试完成了多少有效操作/数据 |
| RDMA 资源 | `rdma resource` | 哪些进程在使用 QP/MR/CQ |
| RDMA 统计 | `rdma statistic`、sysfs `hw_counters` | 请求、错误、重试、ECN/CNP |
| 端口数据量 | sysfs `counters/port_*_data` | 对应设备报告范围内的传输增量 |
| NIC vPort / 物理口 | `ethtool -S` | RDMA 字节、线口字节、PFC 与链路问题 |
| 网络 | 交换机端口/队列计数、遥测 | 热点、丢包、ECN 标记、PFC、排队 |

两层计数器可能记录同一批数据，不要相加。物理端口总流量也不等于某个进程的 RDMA payload。

### sysfs：读取端口计数器

```bash
RDMA_BASE=/sys/class/infiniband/mlx5_0/ports/1
cat "$RDMA_BASE/counters/port_xmit_data"
cat "$RDMA_BASE/counters/port_rcv_data"
cat "$RDMA_BASE/counters/port_xmit_packets"
cat "$RDMA_BASE/counters/port_rcv_packets"
```

标准 `port_xmit_data` / `port_rcv_data` 的单位是 **4 字节**，所以必须先换算。这个 4 是计数单位，不是当前网卡协商的 lane 数。对于 `ethtool` 已明确以 bytes 为单位的计数器，则不能再乘 4。

```text
delta = 当前计数 - 上次计数
标准 port_*_data 对应字节增量 = delta × 4
Gbit/s = delta × 4 × 8 / 实际采样间隔秒数 / 1,000,000,000
```

应使用单调时钟计算真实间隔。驱动重载、设备复位、计数器清零或回绕时，发现当前值小于前值应重建基线，避免算出负带宽或巨大假峰值。计数器范围由驱动实现决定，SR-IOV 等场景还要区分 PF/VF/vPort。[Linux InfiniBand sysfs ABI](https://github.com/torvalds/linux/blob/master/Documentation/ABI/stable/sysfs-class-infiniband)

配套提供一个只读采样脚本：[rdma_rate.py](rdma_rate.py)。它读取标准端口计数器，输出 TX/RX Gbit/s，并记录部分拥塞/错误计数的增量：

```bash
python3 rdma_rate.py --device mlx5_0 --port 1 \
    --interval 1 --samples 30 > rdma-rate.csv
```

不支持的可选计数输出 `NA`，不会当成 0；计数回退会标记 reset。1 秒采样适合看持续趋势，无法还原微秒级 microburst，也不证明某个请求的时延。`hw_counters/lifespan` 还可能使硬件统计读取有缓存周期。

### rdma statistic 与进程/QP 归属

```bash
rdma link show
rdma resource show qp link mlx5_0/1
rdma statistic show link mlx5_0/1
rdma statistic qp show link mlx5_0/1
```

`resource show` 是资源视图，不自动给出每个进程的 GB/s。按 QP/PID 的统计需要内核、驱动和设备支持，某些配置还只影响随后创建的 QP。

在独立实验环境中，可先记下现有模式，再为随后创建的测试 QP 开启自动绑定：

```bash
rdma statistic qp mode link mlx5_0/1
sudo rdma statistic qp set link mlx5_0/1 auto pid,type on

# 此时启动 perftest，再查询
rdma statistic qp show link mlx5_0/1

# 原模式为 off 时，实验结束后恢复
sudo rdma statistic qp set link mlx5_0/1 auto off
```

这些 `set` 命令会修改统计模式；共享主机上应保留并恢复原模式。查询结果能提供哪些计数由驱动决定，不能预设一定包含字节率。[iproute2 rdma-statistic 手册](https://github.com/iproute2/iproute2/blob/main/man/man8/rdma-statistic.8)

### ethtool：区分 RDMA 字节和物理总字节

```bash
ethtool -S enp65s0f0 | grep -Ei \
  'rdma.*(bytes|packets)|bytes_phy|prio.*(pause|bytes)|discard|error|ecn|cnp'
```

mlx5 设备可能提供 `tx_vport_rdma_unicast_bytes`、`rx_vport_rdma_unicast_bytes` 等 RDMA/vPort 计数，以及物理端口、优先级的计数。名称随版本与设备而变，先保存完整 `ethtool -S` 输出再过滤。

按同一计数器取两次采样做差。只有确认计数器单位是字节时才使用 `delta × 8 / 秒数`；带 `_phy` 的口级计数可能包含其他流量、协议开销或其他功能的流量，不应与 perftest payload 数字逐字节强求相等。[mlx5 vPort 与物理计数器定义](https://docs.kernel.org/networking/device_drivers/ethernet/mellanox/mlx5/counters.html)

如果安装了 `mlnx-tools`，可试用它提供的速率观察工具：

```bash
mlnx_perf --help
mlnx_perf -i enp65s0f0
```

它是计数器观察入口，仍要查看当前版本选择了哪些计数器和单位。[NVIDIA mlnx-tools](https://github.com/Mellanox/mlnx-tools)

### ECN / CNP / PFC 和重试计数怎么看

```bash
ls /sys/class/infiniband/mlx5_0/ports/1/hw_counters
rdma statistic show link mlx5_0/1
```

| 计数器例子 | 含义 / 观察方向 |
|---|---|
| `np_ecn_marked_roce_packets` | 本端作为接收端，收到带 CE 标记的 RoCE v2 包 |
| `np_cnp_sent` | 本端作为 NP，发出拥塞反馈 CNP |
| `rp_cnp_handled` | 本端作为发送端，处理 CNP 并进入相应速率控制 |
| `rp_cnp_ignored` | 收到但未处理的 CNP，需结合配置解释 |
| `local_ack_timeout_err` | 本地 ACK 定时器过期事件，不等于每次都成为最终 QP 失败 |
| `rnr_nak_retry_err` | RNR NAK 相关重试，检查接收 WR 与消费能力 |
| `out_of_buffer` | 相关 QP 缺少 WQE 等接收资源导致的丢弃，按设备定义解释 |
| `out_of_sequence` / `packet_seq_err` | 乱序 / 序列问题及相关恢复信号 |
| `req_cqe_error` / `resp_cqe_error` | requester/responder 完成错误，结合应用 WC 查首个失败 |

WRITE 压测时，服务端通常承担大流量接收端角色，客户端处理回来的拥塞反馈；READ 压测的大流量方向相反。CNP 有抑制/合并机制，两个计数不一定一一相等。计数增长说明发生了对应事件，**不直接证明 DCQCN 参数最优或已经定位到根因**。[NVIDIA RDMA 计数器资料](https://docs.nvidia.com/doca/sdk/doca-sdk-v2-8-0.0.pdf)、[Oracle RoCE v2 计数器说明](https://blogs.oracle.com/linux/rocev2-congestion-counters-explained)

PFC 要结合 NIC 的每优先级 pause 计数和交换机两侧计数观察。`rx_pause` 与 `tx_pause` 的意义是该端口收到/发送暂停，不能脱离方向解释。pause 持续时间的单位也要查对应设备文档。

原生 IB 还应查看 `port_xmit_wait`、FECN/BECN、链路错误与信用压力。`port_xmit_wait` 是与发送等待相关的 tick 计数，不能直接当成微秒或丢包数。[标准端口计数器](https://github.com/torvalds/linux/blob/master/Documentation/ABI/stable/sysfs-class-infiniband)

### MFT、mlxlink 与 IB Fabric 工具

需要查物理链路、模块或 FEC 信息时，使用匹配网卡的 [NVIDIA MFT](https://network.nvidia.com/products/adapter-software/firmware-tools/)。若当前 OFED/DOCA 已带这些工具，无需重复安装；独立安装应使用该版本官方 Linux 包及安装说明。

下载的是官方 Linux TGZ 时，安装流程如下；`<文件名>`、`<解压目录>` 使用真实值：

```bash
tar -xzf '<MFT安装包文件名>.tgz'
cd '<MFT解压目录>'
./install.sh --help
sudo ./install.sh
```

安装 MFT 不等于刷入固件。它先提供查询与管理工具，固件变更是另外的操作。[MFT 官方安装说明](https://docs.nvidia.com/networking/display/mftv4350/Compilation-and-Installation)

```bash
# 已安装 MFT 后：启动本地管理驱动并列设备
sudo mst start
sudo mst status -v

# 使用上一步识别出的设备节点，只查询
sudo mlxlink -d '/dev/mst/<本机设备节点>'
sudo mlxconfig -d '/dev/mst/<本机设备节点>' query
```

`mlxlink` 用于物理层信息，不能替代应用带宽测试；查询命令与设置速率、端口模式、清计数的命令也要区分。[mlxlink 官方用法](https://networking-docs.nvidia.com/mftswum/4350/mlxlink-utility)

IB Fabric 可用 `iblinkinfo` 查看链路信息，用 `perfquery` 查指定端口的计数；先通过本机手册确定目标 LID/端口和计数宽度。采样阶段使用查询，不清零共享计数器。

### 抓包与长期监控

```bash
# RoCE v2 端点抓包的尝试；硬件卸载可能使它看不到 payload
sudo tcpdump -i enp65s0f0 -nn 'udp port 4791'
```

抓不到包并不能证明没有 RDMA 流量。需要包级证据时，可结合网卡支持的抓包功能或交换机 SPAN/镜像，并说明观测点与抓包开销。默认 perftest 的 TCP 控制连接与 UDP 4791 的 RoCE 数据路径也不要混淆。

长期监控可把 InfiniBand/RDMA 计数器送入 Prometheus/Grafana。node_exporter 提供 InfiniBand collector；先看 `/metrics` 实际导出的字段和 HELP 单位，再对累计 counter 使用 `rate()`。如果 exporter 已转换为 bytes，就不要再次乘 4。[node_exporter collector 说明](https://github.com/prometheus/node_exporter)

看板至少分开显示：TX/RX 速率、错误/重试增量、ECN/CNP、PFC，以及应用 P99。标签明确主机、RDMA 设备、端口和 netdev；读取频率、缓存周期及 exporter 自身开销也应记录。

## 一组实验怎样把打流和监控放在一起

在每台机器上准备三个终端：

1. **打流终端**：先服务端，后客户端，记录完整命令和 perftest 输出。
2. **RDMA 统计终端**：运行 `rdma_rate.py`，记录实际 TX/RX 和拥塞计数增量。
3. **系统/链路终端**：保存测试前后的 `ethtool -S`、`rdma statistic` 和 CPU/NUMA 信息。

```bash
mkdir -p rdma-results
ethtool -S enp65s0f0 > rdma-results/ethtool-before.txt
rdma statistic show link mlx5_0/1 > rdma-results/rdma-before.txt

# 在独立终端启动采样，再在打流终端执行测试
python3 rdma_rate.py --device mlx5_0 --port 1 \
    --interval 1 --samples 30 > rdma-results/rate.csv

# 测试结束后
ethtool -S enp65s0f0 > rdma-results/ethtool-after.txt
rdma statistic show link mlx5_0/1 > rdma-results/rdma-after.txt
```

日志通过管道传给 `tee` 时，在 Bash 中启用 `set -o pipefail`，避免工具失败却只看见 tee 成功。记录 perftest 退出状态、两端版本、网卡/固件、GID、消息大小、QP/depth、CPU 绑定、测试方向和时间窗口。

想看 NUMA 影响时，先查询实际拓扑，再绑定到对应节点；不要默认所有机器的网卡都在 node 0：

```bash
cat /sys/class/net/enp65s0f0/device/numa_node
cat /sys/class/net/enp65s0f0/device/local_cpulist
numactl --hardware
# 例如确认网卡位于 node 0 后，再用：
# numactl --cpunodebind=0 --membind=0 ib_write_bw ...
```

## 常见问题按阶段排查

| 问题 | 优先检查 |
|---|---|
| `ib_write_bw: command not found` | `perftest` 是否安装，PATH 是否指向预期前缀 |
| `show_gids` 找不到 | `mlnx-tools`、sbin PATH 或官方独立脚本 |
| `ibv_devices` 为空 | 驱动、provider、设备节点、容器设备映射 |
| MR 注册失败 | memlock、可锁定内存、库/provider 来源、权限 |
| TCP 控制连接失败 | 服务端是否等待、`-p` 是否一致、控制 IP/路由/防火墙 |
| 默认模式能跑，`-R` 不行 | CM 地址解析、RDMA 网口路由、地址族 |
| 修改 QP / GID 报错 | 索引、v1/v2、关联 IP/netdev、路径与 MTU |
| RNR 重试 | 对端接收 WR、接收深度、线程是否及时消费 |
| retry exceeded | 对端状态、路径、丢包、超时；结合首个 WC 错误 |
| 带宽只有预期一小部分 | 先看链路速率/PCIe/NUMA，再看大小、深度与网络拥塞 |
| READ 时客户端 TX 很低 | 检查客户端 RX 和服务端 TX，确认监控方向 |
| perftest 很高、系统网络图很低 | 可能监控的是普通 netdev 软件统计，换 RDMA/硬件计数器 |
| ECN/PFC 增长且尾延迟变差 | 关联负载、双方计数、交换机队列与应用接收能力 |

设备、端口、版本和观测范围对齐后，数字才有可比性。调参方法与拥塞机制可继续阅读上一篇的 [性能优化章节](/p/rdma-verbs-congestion-demo/#congestion)。

## 官方资料入口

- [NVIDIA DOCA 下载](https://developer.nvidia.com/doca-downloads)、[DOCA-Host 安装](https://networking-docs.nvidia.com/doca/archive/3-5-0/doca-host-installation-and-upgrade)、[DOCA Profiles](https://networking-docs.nvidia.com/doca/archive/3-5-0/doca-profiles)。
- [MLNX_OFED 下载](https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/)、[传统安装说明](https://docs.nvidia.com/networking/display/mlnxofedv23102131201lts/installing-mlnx-ofed.pdf)。
- [perftest README](https://github.com/linux-rdma/perftest/blob/b513a77278c8061ca6c4dcd1a95d08801c6e7623/README)、[工具参数](https://github.com/linux-rdma/perftest/blob/b513a77278c8061ca6c4dcd1a95d08801c6e7623/man/perftest.1)、[延迟计算实现](https://github.com/linux-rdma/perftest/blob/b513a77278c8061ca6c4dcd1a95d08801c6e7623/src/perftest_parameters.c)。
- [show_gids 源码](https://github.com/Mellanox/mlnx-tools/blob/8fbfd9cfff34e2d32f1dd5a115c42eb8bd3b66c8/sbin/show_gids)。
- [Linux InfiniBand sysfs](https://github.com/torvalds/linux/blob/master/Documentation/ABI/stable/sysfs-class-infiniband)、[mlx5 ethtool 计数器](https://docs.kernel.org/networking/device_drivers/ethernet/mellanox/mlx5/counters.html)、[rdma statistic](https://github.com/iproute2/iproute2/blob/main/man/man8/rdma-statistic.8)。

资料核对日期：2026-09-12。示例绑定了明确的源码版本；安装、测试参数和硬件计数器仍应以目标机器实际版本为准。
