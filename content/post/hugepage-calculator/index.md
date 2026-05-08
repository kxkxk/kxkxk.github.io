---
title: SPDK 大页内存计算器
description: 根据内存大小、磁盘容量和数量，计算所需的2MB大页内存数量
date: 2026-05-08 12:00:00+0000

categories:
    - 技术
tags:
    - SPDK
    - NVMe
    - hugepages
---

部署 SPDK/NVMe 存储时，需要为每个磁盘分配大页内存（HugePages）。本文提供一个计算器，根据服务器内存、磁盘容量和数量，快速计算出需要配置的 2MB 大页数量。

## 参数说明

| 参数 | 说明 |
|------|------|
| 内存大小 | 服务器物理内存，单位 GB |
| 磁盘容量 | 单块 NVMe 磁盘容量 |
| 磁盘数量 | NVMe 磁盘总数 |
| pc_size | SPDK page cache 大小 |

## 计算器

<div id="hp-calc">
<style>
#hp-calc { max-width: 640px; margin: 1.5em 0; }
#hp-calc fieldset { border: 1px solid var(--card-border); border-radius: var(--card-border-radius); padding: 1.2em 1.5em; margin-bottom: 1em; background: var(--card-background); }
#hp-calc legend { font-weight: 600; padding: 0 .4em; }
.hp-row { display: flex; align-items: center; margin-bottom: .7em; gap: .6em; }
.hp-row:last-child { margin-bottom: 0; }
.hp-row label { min-width: 100px; font-size: .92em; }
.hp-row input, .hp-row select { flex: 1; padding: .4em .6em; border: 1px solid var(--card-border); border-radius: 4px; background: var(--card-background); color: var(--body-text-color); font-size: .92em; }
.hp-btn { display: inline-block; padding: .55em 1.6em; border: none; border-radius: 4px; background: var(--accent-color); color: #fff; font-size: .95em; cursor: pointer; margin-top: .5em; }
.hp-btn:hover { opacity: .85; }
.hp-results { margin-top: 1em; }
.hp-result-item { display: flex; justify-content: space-between; padding: .5em 0; border-bottom: 1px dashed var(--card-border); font-size: .92em; }
.hp-result-item:last-child { border-bottom: none; }
.hp-result-item .val { font-weight: 700; font-family: monospace; }
.hp-warn { color: #e55; font-weight: 600; margin-top: .6em; font-size: .9em; }
.hp-ok { color: #2a9d8f; font-weight: 600; margin-top: .6em; font-size: .9em; }
</style>

<fieldset>
<legend>输入参数</legend>
<div class="hp-row">
  <label>内存大小 (GB)</label>
  <input type="number" id="hp-mem" value="256" min="1">
</div>
<div class="hp-row">
  <label>磁盘容量</label>
  <select id="hp-disk">
    <option value="7.68">7.68 TB</option>
    <option value="15.36">15.36 TB</option>
  </select>
</div>
<div class="hp-row">
  <label>磁盘数量</label>
  <input type="number" id="hp-count" value="1" min="1">
</div>
<div class="hp-row">
  <label>pc_size</label>
  <select id="hp-pcsz">
    <option value="64">64 KB</option>
    <option value="128">128 KB</option>
    <option value="256">256 KB</option>
  </select>
</div>
</fieldset>

<button class="hp-btn" onclick="hpCalc()">计算</button>

<div class="hp-results" id="hp-out"></div>
</div>

<script>
const HP_TABLE = {
  '64_7.68':   { hp: 13, total: 18 },
  '64_15.36':  { hp: 17, total: 26 },
  '128_7.68':  { hp: 10, total: 13 },
  '128_15.36': { hp: 12, total: 17 },
  '256_7.68':  { hp: 7,  total: 9 },
  '256_15.36': { hp: 9,  total: 12 },
};

function hpCalc() {
  const memGB   = parseInt(document.getElementById('hp-mem').value)   || 0;
  const disk    = document.getElementById('hp-disk').value;
  const count   = parseInt(document.getElementById('hp-count').value) || 0;
  const pcsz    = document.getElementById('hp-pcsz').value;
  const key     = pcsz + '_' + disk;
  const row     = HP_TABLE[key];

  if (!row) { document.getElementById('hp-out').innerHTML = '<p class="hp-warn">未找到对应的基准数据</p>'; return; }
  if (memGB <= 0 || count <= 0) { document.getElementById('hp-out').innerHTML = '<p class="hp-warn">请输入有效的内存和磁盘数量</p>'; return; }

  const hpGB      = row.hp * count;
  const totalGB   = row.total * count;
  const remainGB  = memGB - totalGB;
  const totalHP   = hpGB * 1024 / 2;
  const nodeHP    = Math.ceil(totalHP / 2);

  const warnOrOk = remainGB >= 0
    ? `<p class="hp-ok">内存充足，剩余 ${remainGB} GB 可供系统和其它进程使用</p>`
    : `<p class="hp-warn">内存不足！还需额外 ${Math.abs(remainGB)} GB，请减少磁盘数量或增大内存</p>`;

  document.getElementById('hp-out').innerHTML = `
    <div class="hp-results">
      <div class="hp-result-item"><span>总大页内存需求</span><span class="val">${hpGB} GB</span></div>
      <div class="hp-result-item"><span>总内存需求</span><span class="val">${totalGB} GB</span></div>
      <div class="hp-result-item"><span>2MB 大页总数</span><span class="val">${totalHP.toLocaleString()}</span></div>
      <div class="hp-result-item"><span>node0 大页数</span><span class="val">${nodeHP.toLocaleString()}</span></div>
      <div class="hp-result-item"><span>node1 大页数</span><span class="val">${nodeHP.toLocaleString()}</span></div>
      ${warnOrOk}
    </div>`;
}

hpCalc();
</script>

## 配置方法

计算出大页数量后，在每个 NUMA node 上配置：

```bash
# 临时生效
echo <node0大页数> > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
echo <node1大页数> > /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages

# 永久生效（写入 /etc/sysctl.conf）
vm.nr_hugepages = <大页总数>
```
