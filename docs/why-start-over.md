# 交接 — 2026-11-26 06:45

> 上一份是 `docs/交接-20261125.md`（11-25 07:10）。这一份接在它后面。

---

## 0 一句话

**`psyche` 分支（`xnu-8796.121.2`）已经编不出能启动的内核了。
五次编译全部失败，包括从 Apple 源码全新 `apply.sh` + 全新编译。
决定迁移到上游 `oosexgang/hakios-xnu-kernel` 的 `common` 分支从零重建。**

能跑的只剩两样：`prebuilt/hakios-kernel`（`0b7957f0`，11-19 原版）和
`D:/IOS/phone/system-work.apfs`（`c99f6e22`，2.4 GB 用户态）。**别动这两个。**

---

## 1 前因后果

### 1.1 起点

上一份交接说：真机已经跑到 iOS 桌面（11-25 06:24），公开仓已推送 `87626fb`。
本次会话的目标是**验证公开仓能不能复现那次成功**，以及修掉文档里的错误结论。

### 1.2 做了什么

**第一阶段：修文档**（已推送）

| 提交 | 内容 |
|---|---|
| `917cdf85` | 补 `docs/wifi-progress.md`（把 536 轮 WiFi 实验的结论搬进仓库）；修 4 处错误结论 |
| `c6d82f30` | 补三份审计报告（`docs/audit/`） |
| `c3f52fe9` | 补 `docs/bisect-results.md`（启动二分） |
| `2bfe1e25` | 补 `docs/selfbuild.md`（构建链怎么搭起来） |
| `385e4cbc` | 补 `docs/kernel-death-and-usb-debug.md` |

**第二阶段：搭构建链**（成功）

```bash
# 三个必须的坑，全都踩过了
ln -s /usr/bin/python3 /usr/local/bin/python   # XNU 的 lldbmacros 用 #!/usr/bin/env python
export DSYMUTIL=/root/theos/toolchain/linux/iphone/bin/dsymutil
bash ./xnu-build.sh -k -j12                    # -k 不可省；rc=2 是正常的（符号集那步）
```

编出 963 个 `.o`，73 秒。`ath11k.o` 也用 `tools/mk-ath11k.sh` 补上了
（FreeBSD 源用 `mirrors.163.com`，997 KB/s，比官网 256 KB/s 快 4 倍）。

**第三阶段：内核起不来**（失败，本轮核心）

### 1.3 关键发现：日志开关默认关着

`pexpert/arm/pe_kprintf.c:21-23`：

```c
SECURITY_READ_ONLY_LATE(bool) disable_serial_output = true;    // 默认 true
SECURITY_READ_ONLY_LATE(bool) disable_kprintf_output = true;   // 默认 true
```

两个都为 false 才打印。关掉的方法：

| 变量 | boot-arg |
|---|---|
| `disable_serial_output` | `debug=0x8` |
| `disable_kprintf_output` | `-enable_kprintf_spam`（DEVELOPMENT 构建） |

**还有第二个坑**：`_doprnt_log` 是**逐字符**调 `PE_kputc → _cnputc`，
不是批量走 `_cnputs`。**tee 必须挂在 `_cnputc` 上**，挂 `_cnputs` 一条日志都出不来。

改对之后（`osfmk/console/serial_console.c` 的 `_cnputc` 加 `loader_log_line`），
内核日志完整出来了 —— **它跑到了 iOS 桌面**：

```
hakios-gl: frame 4220: 942 GL calls, 187 draws, 730 triangles
backboardd[11:514] [TouchEvents] contact 1 presence: touching.
launchd (pid/33 [Spotlight]) <Notice>: uncorking domain.
```

### 1.4 但自编内核还是起不来

用 `loader_checkpoint` 二分（`fastboot oem run N`），CP1–CP27 全部命中：

```
start.s (CP1-8) -> arm_init (CP10-21) -> machine_startup (CP22-25) -> kernel_bootstrap (CP26-27)
```

**内核执行完全正常，不是死在早期。**

但真正上机对比后，事实变了：

| 内核 | 来源 | 大小 | 结果 |
|---|---|---|---|
| **`0b7957f0`** | **prebuilt，11-19 编** | 22,341,416 B | ✅ **到 iOS 桌面，能滑动，60+ 秒稳定** |
| `d5dfdc53` | 11-25 编，无插桩 | 22,330,568 B | ❌ 8 秒 USB 消失 |
| `495c0fe1` | 11-26 编，带插桩 | 22,330,568 B | ❌ 8 秒 USB 消失 |
| `29e27bb8` | 11-26 编，不链 ath11k | 21,835,608 B | ❌ 8 秒 USB 消失 |
| **`71b1e414`** | **Apple 源码全新 apply + 全新编译** | 22,330,568 B | ❌ **8 秒 USB 消失** |

**对照实验**：同一个 `mkpayload.py`、同一条 cmdline、同一块 `system.apfs`、
同一个 loader session 流程，**只换 kernel**：

```
run-payload.bin (0b7957f0):  t=8s running → t=60s running，framebuffer 持续刷新
p-onlyk.bin     (d5dfdc53):  t=8s phone=none，USB 直接消失
```

### 1.5 为什么会这样

`docs/build-tree.md` §3 自己写着：

> 想复现它得回到当时的树；想直接跑它，用包里那颗，不必编。

而且 XNU 的 `config/newvers.pl` 把构建时间 `date` 戳进内核，**不认 `SOURCE_DATE_EPOCH`**：

```
prebuilt:  Thu Nov 19 01:30:30 CST 2026
k-genuine: Wed Nov 25 09:03:48 CST 2026
```

**结论：`psyche` 分支的构建不可复现。** 11-19 那次成功用的树状态已经无从还原。

---

## 2 已经排除的（别再试）

这一轮排查很長，这些都实测排除了：

| 假设 | 怎么排除的 |
|---|---|
| 日志关了所以像死了 | 见 §1.3，打开后确认内核在跑 |
| 死在 `machine_startup` 之前 | **错**。`loader_checkpoint` 二分，CP1–CP27 全命中 |
| `fb_beam`（防撕裂）卡显示 | 排除。`fBeamIntf==0` 时代码跳过 beam 逻辑 |
| 缺背光驱动 | 排除。prebuilt 也没背光驱动，照样显示 |
| ath11k 初始化崩 | 排除。不链 `ath11k.o` 一样 8 秒掉线 |
| 同一 loader session 跑第二个内核 | 排除。每次都完整 reboot → bootloader → boot xnu.img |
| 增量构建残留 | 排除。删整个 `BUILD/obj`，甚至删整个 `xnu-src` 重跑 `apply.sh` |
| `set_pcpu` / BSS / `MAX_MEM_SIZE` / 段重叠 | 逐项核对，全正常 |
| 启动路径函数被改坏 | 27,921 对函数逐一对齐，启动路径 0 处真实差异 |
| 编译器版本（clang-18 vs theos clang-11） | **没排完**。`xnu-clang` 硬编码 `/usr/bin/clang`（18.1.3），改指 clang-11 后它不认 `--ld-path`，编译失败 |

**最后一行的意思是**：即使编译器是原因，也不改变结论 —— 那套组合编不出 `0b7957f0` 那样的内核。

---

## 3 要做什么：迁到上游从零重建

### 3.1 目标

```
旧：hanliu660-gif/hakios-xnu-kernel  分支 psyche   xnu-8796.121.2
新：oosexgang/hakios-xnu-kernel       分支 common   xnu-12377.121.6
```

上游已克隆在 `D:/IOS/upstream-hakios/`（33 MB，分支 `common`）。
**新项目地址会变**，代码和文档一起搬。

### 3.2 为什么上游能解决问题

| | `psyche` 分支 | 上游 `common` |
|---|---|---|
| 可复现 | ❌ 编出来跑不了 | ✅ `kernel/assemble.sh` + `verify.sh` |
| 语法检查 | ❌ 没有 | ✅ `kernel/syntax-check.sh`（不上机） |
| 驱动 | 25 个目录 | **50 个 `.c`**，含我们没有的 `backlight.c` |
| 板级配置 | 散在 C 代码 | ✅ 纯数据 `board.dts`(325 行) + `soc.dtsi`(217 行) |
| 已知能跑 | — | 骁龙 780G，Darwin 27.2 桌面（`gpu=0` 软绘） |

**上游的 50 个驱动覆盖**：`adreno.c`(GPU)、`fts.c`/`goodix.c`(触摸)、`backlight.c`、
`battery.c`、`wifi.c`/`wlfw.c`/`lx.c`/`sta.c`/`apple80211.c`、`audio.c`/`sound.c`/`speaker.c`/`mic.c`、
`camera.c`/`torch.c`、`ufs.c`/`ufshc.c`、`thermal.c`/`ddr.c`/`cpufreq.c`/`cputop.c`、`bt_host.c`/`bt_uart.c`。

**迁 psyche 不需要写驱动，只需要填板级参数。**

### 3.3 代价（说清楚）

**1. 要换用户态。** 内核和用户态必须配套：

| 层 | 现在 | 上游要的 |
|---|---|---|
| 内核 | `xnu-8796.121.2`（Darwin 22） | `xnu-12377.121.6`（Darwin 26/27） |
| 用户态 | iOS 16.5（`iPhone10,3`/20F66），2.4 GB | **iPadOS 17.7.11（`iPad7,1`/21H461）**，约 7 GB IPSW |

IPSW 直链（`build.config.darwin27` 里写的）：

```
https://updates.cdn-apple.com/2025SpringFCS/fullrestores/122-77166/
DCA6CFE8-90E9-4B92-880D-3DE171798E40/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw
```

**2. 要写 psyche 的 `board.dts`。** 上游 `arch/arm64/boot/dts/` 只有 `_template` 和 `qemu-virt`。

**3. GPU 要重新验证。** 上游 README 诚实说明：Darwin 27.2 在骁龙 780G 上是**软件绘制**（`gpu=0`），
GPU 加速上游没跑通 —— **而我们是跑通的**。

---

## 4 迁移步骤

| 步 | 做什么 | 需要 |
|---|---|---|
| 1 | `adb pull /sys/firmware/fdt` 拿 psyche 的 DTB | 手机（Android 下） |
| 2 | 从 DTB 提板级参数（面板、触摸、PMIC、时钟） | 步 1 |
| 3 | 下载 iPadOS 17.7.11 IPSW（约 7 GB） | 网络 |
| 4 | `darwin/tools/unpack.sh` 解开 | 磁盘约 20 GB |
| 5 | 按 `_template/board.dts` 填 psyche 版 | 步 2 |
| 6 | `kernel/assemble.sh` + `kernel/syntax-check.sh` | — |
| 7 | `kernel/build.sh` 编内核 | — |
| 8 | `arch/arm64/boot/build.sh` 编 loader | — |
| 9 | `darwin/userland/root26-darwin.sh` 建 userland 镜像 | — |
| 10 | `bench enter` / `bench ramdisk` / `BENCH_DEFCONFIG=phone_first ./bench run` | 手机 |
| 11 | 逐项验证：显示、触摸、GPU、电池、WiFi | 手机 |

---

## 5 已经拿到的材料

### 5.1 psyche 的硬件参数（本次从 Flyme 12.6 / Android 16 的设备树提取）

**面板型号：`l3a`** —— 和 `bd_l3a.elf`（WiFi 板级数据）同一命名体系，`board_17`。

屏幕供电（`/proc/device-tree/soc/dsi_panel_pwr_supply_vci_l3a`，三条 entry）：

| entry | supply-name | 电压 | enable-load | post-on-sleep |
|---|---|---|---|---|
| 0 | `vddio` | 1,800,000 µV (1.8 V) | 62,000 | 3 |
| 1 | `dvdd` | 1,200,000 µV (1.2 V) | 62,000 | 3 |
| 2 | `vci` | 3,000,000 µV (3.0 V) | 100,000 | 10 |

其他已实测（来自内核日志）：

```
cpufreq:  group 0 = 1804 MHz   group 1 = 2419 MHz   group 2 = 3187 MHz
adreno:   RPMh "gfx.lvl" @ 0x30050, "cx.lvl" @ 0x30000, "mx.lvl" @ 0x30010
adreno:   gcc pll votes (0x152000) = 0x1206d7f
buttons:  PMIC arbiter version 0x50010000, 512 channels
thermal:  25 sensors (cpuss-0/1, gpuss-0/1, aoss0/1, video, ddr, q6-hvx, camera, cmpss, npu)
display:  1080 x 2400 @ 0x9c000000
SoC:      561 个设备树节点，15+ 条 i2c 总线
```

### 5.2 本机文件

| 路径 | 内容 |
|---|---|
| `D:/IOS/upstream-hakios/` | **上游源码**（33 MB，分支 `common`） |
| `D:/IOS/psyche2.dtb` | psyche 的 DTB（930,810 B，结构完整） |
| `D:/IOS/psyche2-fix.dtb` | 截到 `totalsize` 的版本（`dtc` 需要） |
| `D:/IOS/ref-qcomusb/` | QcomXnuUsbDebug 的参考文档 + `Sm8250.h` |
| `D:/IOS/qcom-usb-debug/` | 同 SoC 的 USB 调试模块源码 |

---

## 6 硬约束

### 6.1 上一份的（仍然有效）

- 手机：小米 12X（psyche / SM8250-AC kona），序列号 `23170c8b`
- `???????????? fastboot` = USB 已死 → **长按电源 15 秒**
- **platform-tools 在 `D:/IOS/tools/platform-tools`**，不在 PATH 里
- `userland/build.sh` 必须在 ext4 上跑（NTFS 会丢执行位）
- `payload-cmdline.txt` / `kernel.ver` 必须是 LF

### 6.2 本次新加

- **同一个 loader session 里跑第二个内核必死**（`docs/boot-to-desktop.md:189`）。
  每次跑内核前必须完整走一遍：`fastboot reboot` → `adb reboot bootloader` → `fastboot boot xnu.img`
- **`fastboot oem run` 不会清 `result`**。每次测之前先读一次，否则会读到上一轮的残留值
- **`make` 可能空转**。增量构建报 `rc=0` 但 md5 没变，必须 `rm -rf BUILD/obj/...` 才算真重建
- **`xnu-clang` 硬编码 `/usr/bin/clang`**（Ubuntu 18.1.3），不是文档说的 theos clang-11
- **判读 `result`**：`checkpoint N 0` = 命中检查点；手机掉出 loader = 那个点之前真崩了

---

## 7 必须保留的东西

| 项 | 为什么 |
|---|---|
| `D:/IOS/repos/hakios-xnu-kernel/prebuilt/hakios-kernel`（`0b7957f0`） | **唯一能跑 psyche 的内核**，任何回退都要它 |
| `D:/IOS/phone/system-work.apfs`（2,476,728,320 B，`c99f6e22`） | **唯一能到桌面的用户态** |
| `D:/IOS/logs/` 全部 | 内核日志、framebuffer 截图，实测证据 |
| `D:/IOS/phone/k-*.bin`、`p-*.bin` | 全部实验内核与 payload |
| `D:/IOS/repos/hakios-xnu-kernel/docs/` | 排查过程与结论，**新项目要搬过去** |

**`0b7957f0` 和 `c99f6e22` 丢了就再也没了。**

---

## 8 一句话给下一个会话

> `psyche` 分支编不出能跑的内核，五次全败（含从 Apple 源码全新编译）。
> 能跑的只有 `0b7957f0` 和 `c99f6e22`，别动。
> 走上游 `common`（`xnu-12377`）：能复现、有语法检查、50 个驱动现成、板级配置是数据。
> 代价是换用户态（要下 7 GB iPadOS 17.7.11 IPSW）和写 psyche 的 `board.dts`。
> **别再在 `psyche` 分支上编译了。**
