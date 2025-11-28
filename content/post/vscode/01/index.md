---
title: 修复高版本vscode报错 远程主机不满足运行vs code服务器的先决条件
description: 绝了
date: 2025-11-26 20:25:00+0000

categories:
    - 技术
tags:
    - vscode
weight: 1       # You can add weight to some posts to override the default sorting (date descending)
---

# 背景

旧版本的vscode不支持各种vibe code插件，如cline, cursor, roo code等，这很难受。\
参考vscode的官方文档(https://code.visualstudio.com/docs/remote/faq#\_can-i-run-vs-code-server-on-older-linux-distributions)，自1.99版本之后vscode只支持远程连接glibc>=2.28的环境。\
但是我们可以通过构建新版vscode需要的sysroot来连接旧版linux(glibc >= 2.28 and libstdc++ >= 3.4.25)

# 编译sysroot

这里提供两种方法

1. 跟着上面的官方文档，使用交叉编译工具构建sysroot
2. 直接使用预先编译好的二进制(https://github.com/kxkxk/vscode-sysroot) 下载vscode-sysroot-x86_64-linux-gnu.tgz

# 安装sysroot并配置环境变量

1. 上传二进制到服务器
2. 解压到`\~/.vscode-server/sysroot`

```
tar zxf vscode-sysroot-x86_64-linux-gnu.tgz -C ~/.vscode-server
```

3. 创建`\~/.vscode-server/sysroot.sh`，写入以下内容

```
# Path to the dynamic linker in the sysroot (used for --set-interpreter option with patchelf)
export VSCODE_SERVER_CUSTOM_GLIBC_LINKER=$HOME/.vscode-server/sysroot/lib/ld-linux-x86-64.so.2
# Path to the library locations in the sysroot (used as --set-rpath option with patchelf)
export VSCODE_SERVER_CUSTOM_GLIBC_PATH=$HOME/.vscode-server/sysroot/usr/lib:$HOME/.vscode-server/sysroot/lib
# Path to the patchelf binary on the remote host
export VSCODE_SERVER_PATCHELF_PATH=$HOME/.vscode-server/sysroot/usr/bin/patchelf
```

4. 配置ssh config
在`~/.ssh/config`中添加以下内容
```
  Host xxxxx
    HostName xxxxx
    User xxxxx
    # 增加这一条
    RemoteCommand source ~/.vscode-server/sysroot.sh && exec $SHELL -l
```
注: 启用RemoteCommand需要在vscode的配置里开启useLocalServer和Enable Remote Command。windows系统的vscode有bug，需要在setting.json中手动添加(https://github.com/microsoft/vscode-remote-release/issues/9697) 这帮人为什么不修?
```
"remote.SSH.useLocalServer": true
```
done，现在可以使用remote-ssh连接远程了
