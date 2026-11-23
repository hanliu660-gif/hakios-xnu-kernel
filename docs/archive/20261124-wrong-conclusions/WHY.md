# 这批结论为什么被移出 docs/

2026-11-24 04:16 与 04:22 两次实机跑机（`logs/041623-run.log`、`logs/042229-run.log`）之后，
`docs/` 里 11-24 凌晨那批结论有多条被**同一批二进制**直接推翻。原文完整保留在本目录的
`merge-audit.md`（未改一字），下面逐条给反证与行号。

## 逐条

### 1. 「22,341,416 字节那个内核 GPU 在 submission 101 就死，`worked 0 us`」

- 出处：`docs/gpu-regression.md`（11-24 00:13）
- 反证：22,341,416 B / md5 `0b7957f0b463e9dd30a5db808c97a477` 的同一个文件，今天 04:22 跑出
  `logs/042229-run.log:7881`：
  `HakiosGPU: 1667 submissions in 5008 ms: ... the GPU worked 573 us on each of 1667 ... 0 longer; 955 ms in all`
  并且 `0 over before they began`（没有一次等待超时）。
- 结论：这个文件今天的 GPU 是好的。`101` 那次故障真实发生过，但不能用「文件字节数」归因；
  真正变量是当时的 ramdisk/根结构（见第 2 条）。

### 2. 「19 Nov 内核 + `root.apfs` + `touchscreen=1` 必然无响应；`touches` 计数器再也到不了」

- 出处：`merge-audit.md` 的 run 表（11-24 03:40）
- 反证：今天跑的就是这个组合 —— `full-1119` 内核 + `ramdisk=userland/root.apfs`，
  cmdline 见 `logs/042229-run.log:9-11`（`rd=md0 ... touchscreen=1 ... touchscreen=1`），
  结果 `psyche_touch: ... touches=166 bad=2 irq_low=100 last=(1079,2074)`（`:7871`、`:7879`）。
- 结论：组合本身能跑。差的不是内核，是**大镜像怎么进 md0、小根怎么进 md1**。
  正确的两段结构见 `STATUS.md` §2。

### 3. 「死因 poll 线程 `fts_thread`，不在供电」

- 出处：`merge-audit.md` 末段
- 反证：同一个字节的内核，换回正确 ramdisk 结构即可正常轮询（第 2 条），
  期间 `poll=25625 reads=25625`，没有一次读失败。
- 结论：把一个 payload 配置问题归因到驱动的线程模型，方向错了。

### 4. 「`soc_name=unknown`、设备树 len 从 4438 变 3874，是有人动过工作区」

- 出处：上一轮口述推断
- 反证：设备树由 loader 自己生成 —— `xnuloader/loader.c` 第 8-9 行注释：
  `copies the Mach-O segments there, builds an Apple-format device tree`。
  4438 对应 11-19 的完整 boot-args，3874 对应「只带 `cpus=1 touchscreen=1`」那轮；
  长度差异是参数差异的函数，不是文件被改的痕迹。
- 结论：这条推断作废。备份镜像的 ctime/mtime 自洽，也没有批量回填时间戳的证据。

### 5. 「`touch_step=3` 卡死，说明第二路供电（`rpmh_read(0x41200 + VRM_ENABLE)`）不回」

- 出处：`merge-audit.md` 的 checkpoint 表
- 评价：那张表只证明「进入 checkpoint 3 之后没有返回」，不能推出供电有问题。
  今天没带 `touch_step`，跑通全程，`vdd already on` / `avdd already on` / `supplies ready` 三行都在。
- 结论：作为排查记录保留，作为结论作废。

## 这批材料里仍然有效的部分

`merge-audit.md` 的**前半段**（三条线的 `git rev-list --left-right --count`、`wc -c`、`md5sum`、
`git log -- <path>` 对比）是实测数据，依然有效：`drivers/wifi/hakios.pcie.c` 确实只在本线、
上游 `hakios.adreno.c` 确实是 167,033 B、`hakios.battery.c` 自 split 起没动过。
作废的是它对**手机行为**的那些结论。

## 时间线（防止再混淆）

| 时间 | 事件 | 结果 |
|---|---|---|
| 11-24 00:13 | `docs/gpu-regression.md` 写完 | 「22,341,416 坏的」← 今天被推翻 |
| 11-24 00:28 | `3279de9` 三条线合并 | 内核 22,343,864 B |
| 11-24 01:06 | 构建出 22,344,256 B 内核（md5 `bc09f836...`） | **已被我覆盖，只能重编找回** |
| 11-24 01:08 / 02:27 | `dda81eb` / `58741f9` 改 psyche_touch | 未在本次跑机中验证 |
| 11-24 03:40 | `252e6c7` 写 `merge-audit.md` 手机段 | 「poll 线程是死因」← 今天被推翻 |
| 11-24 04:16 / 04:22 | 两次实机跑机 | **三合一跑通**（166 touches / 1667 submissions / 58 fps） |

## 归档位置

- 本目录：`merge-audit.md`（原文）、`WHY.md`（本文件）
- 历史内核：_archive/20261124/kernels/（23 个，文件名与 md5 全保留）
- 当前唯一可信结论：`../../../STATUS.md`
