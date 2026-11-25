# 自编内核为什么起不来：2026-11-25 调查结论

> 这一轮的目标是「在这台机器上重建内核构建链，并复现能跑的 `0b7957f0`」。
> 构建链**通了**，复现**没成**。这份文件只写实测到的，分为「已确认的事实」和
> 「仍然不知道的」，不要让下一个会话再从零猜一遍。

---

## 1 结论先行

**构建链已通，但编出来的内核起不来。**

| | prebuilt `0b7957f0` | 自编 `5d9c94c4` | 自编 `d5dfdc53` |
|---|---|---|---|
| 大小 | **22,341,416 B** | 22,330,568 B | 22,330,568 B |
| 上机 | ✅ 到桌面 | ❌ 退回 Android | ❌ 退回 Android |

同一个环境、同一条 payload 流程、同一块 `system.apfs`（`c99f6e22`，2,476,728,320 B），
唯一变量是内核。**对照实验做过，可复现。**

---

## 2 构建链：怎么编（已确认可用）

```bash
# 一次性
ln -s /usr/bin/python3 /usr/local/bin/python     # XNU 的 lldbmacros 用 #!/usr/bin/env python

# 每次
cd /root/hakios
export THEOS=/root/theos
source ./xnu-env.sh
export DSYMUTIL=$THEOS/toolchain/linux/iphone/bin/dsymutil

xnu/sync-drivers.sh xnu-src/xnu-8796.121.2        # drivers/ 的改动同步进内核树
python3 tools/hakios-config.py kernel --tree xnu-src/xnu-8796.121.2
bash ./xnu-build.sh -k -j12                        # -k 不可省；rc=2 是正常的
```

产物：`xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`

**三个必须知道的坑**（都是实测踩到的）：

1. **缺 `python`**：`tools/lldbmacros/Makefile` 的 `INSTALLPY` 规则用 `#!/usr/bin/env python`，
   而 Ubuntu 只有 `python3`。不补软链，config bootstrap 阶段报
   `/usr/bin/env: 'python': No such file or directory`（10 处），整条链崩。
2. **必须设 `DSYMUTIL`**：否则 `--minimize: command not found` / `MakeInc.kernel:131: Error 127`。
3. **必须走 `make kernel` 那条路**（`sync-drivers.sh` + `hakios-config.py` + `-k`）。
   直接 `bash xnu-build.sh` 会跳过驱动同步；`-k` 则保证符号集那步失败后链接仍完成。

**耗时**：干净构建 963 个 `.o` 约 **73 秒**（`-j12`）。

### 2.1 ath11k 目标文件

`xnu-build.sh:7` 的 `LX=$XNUDEPS/build/ath11k/out/ath11k.o` 不存在时，内核里**没有 WiFi**。
补它：

```bash
bash tools/mk-ath11k.sh          # 下载 400 MB，编译约 20 秒
```

- Linux 源：`cdn.kernel.org`（150 MB，**快**）
- FreeBSD 源：`download.freebsd.org` **只有 256 KB/s**（253 MB 要 16 分钟）；
  换 `mirrors.163.com/freebsd/releases/amd64/15.1-RELEASE/src.txz` → **997 KB/s，2 分 28 秒**
- 产物 756,220 B，**`out/stubs.txt` 为空**（18 个驱动文件的外部调用全部有真实实现，没有假桩）

**注意**：`mk-ath11k.sh` 有缓存判定，改了 `wifi/lx/src/*.c` 之后必须
`rm -rf xnu-deps/build/ath11k/obj` 才会重编。

---

## 3 两颗内核到底差在哪（反汇编实测）

用 `llvm-nm` 导出符号表（各 54,703 / 54,814 行），按段归类对比：

### 3.1 段级

| 段 | prebuilt | 自编 | 判定 |
|---|---|---|---|
| `__TEXT_EXEC`（代码） | 9,338,880 | 9,338,880 | **完全相同大小**，代码量没少 |
| `__TEXT`（字符串/跳表） | 7,684,096 | 7,667,712 | **少 16,384 B**（正好一页） |
| `__DATA` | 4,603,904 | 4,603,904 | 相同 |
| `__BOOTDATA` | 311,296 | 311,296 | 相同大小，81,254 字节内容不同（指针/重定位） |
| `__LINKINFO` | 589,824 | 589,824 | **逐字节完全相同** |

### 3.2 那 16 KB 是什么

`__TEXT` 里只属于 prebuilt 的符号（12 个），**全部与 WiFi 有关**：

```
_md5_block.k            _md5_block.shifts          ← lx_wpa.c 的 MD5 查找表
_qmi_wlanfw_antenna_switch_req_msg_v01_ei          ← QMI 天线开关消息编码
l_switch.table.ath11k_dp_rx_h_undecap
l_switch.table.ath11k_dp_tx
l_switch.table.ath11k_mac_op_set_bitrate_mask.413
l_switch.table.ath11k_peer_assoc_prepare
l_.str.68 / .98 / .100 / .101
```

**没有一条在启动路径上。** 所以 16 KB 的差不是「内核起不来」的原因。

### 3.3 只属于 prebuilt 的 7 个函数

| 符号 | 是什么 |
|---|---|
| `_md5_end` / `_md5_more` | `wifi/lx/src/lx_wpa.c` 的 MD5（WPA2 握手） |
| `_lx_calibrated` | `hakios.lx.c` 的校准状态 |
| `_qmi_wlanfw_antenna_switch_req_msg_v01_ei` | QMI 天线开关 |
| `_ath11k_create_ht_cap` / `_mac_bitrate_mask_num_he_rates` / `_num_vht_rates` | ath11k HT 能力 |

**同样全在 WiFi 侧。**

---

## 4 源码树能不能恢复（结论：不能）

Z 盘 `DSH/ios打包/wsl/hakios-build-tree.tar.gz`（12.5 MB，638 个 tracked 文件）号称是
「产出 11-24 镜像的那棵树」，`DIFF.md` 说它和仓库 psyche 分支有 82 个文件不同。
**实测它本身不自洽。**

把 tar 里的 `wifi/lx/src/` 五个文件全换上，重编 `ath11k.o`：

```
lx_bus.c:427: error: call to undeclared function 'lx_os_radio_firmware_memory'
lx_bus.c:697: error: call to undeclared function 'lx_os_rproc_shutdown'
lx_bus.c:913: error: no member named 'cal_inherited' in 'struct ath11k_qmi'
4 errors generated.
```

而 tar 自己的 `xnu/files/bsd/sys/hakios.lx.h` 和 `hakios.qmi.h`，与当前树的对应文件
**md5 完全一致**（`d22bd8c3` / `b429dc7a`），里面**根本没有**这三个接口。

**也就是说：那份 `lx_bus.c`（1073 行）配的头文件已经不存在了。**

同理，prebuilt 独有的 `_md5_end`/`_md5_more` 说明它来自一棵 **`lx_wpa.c` 还带 MD5、
且 `xnu/files` 还没有 `beam`** 的树（当前树 `beam` 字符串 10 处，prebuilt 0 处）。
那棵树不在任何现存副本里：Z 盘 tar 是 11-24 的，比 prebuilt 的 11-19 新。

---

## 5 试过但无效的

| 尝试 | 结果 |
|---|---|
| `fb_beam=0`（关掉画面防撕裂的新特性，默认开） | 仍然起不来 |
| 用 11-24 构建树的 `drivers/`、`configs/`、`xnu/` 覆盖 | 编出来大小不变，仍起不来 |
| 用 11-24 的 `wifi/lx/src/` | **编译失败**（§4），且与本问题无关 |

---

## 6 仍然不知道的

**自编内核在哪一行崩的，没有证据。**

- 内核在打印任何日志前就死了：`oem run` 后 `getvar result` 一直 `running`，
  直到手机自己重启回 Android，`get_staged` 拿不到内容。
- loader 的 `oem log` 也拿不到（内核没跑到能写 logdump 的阶段）。
- 因此**无法判断**是启动早期（`PE_init_platform` / `PE_init_cpu`）还是驱动 attach 阶段。

**下一步唯一可能有用的方向**：

1. 用 `loader_return_after` 之外的 **`wfi`/`panic` 类 boot-arg** 让内核在特定点停住，
   但要看 `drivers/core/` 和 `xnu/files/` 里有哪些可控点。
2. 或者接受「自编内核不可用」，**只用 prebuilt `0b7957f0`** 跑机，
   把改动做成 diff 让人在有 11-19 树时合并。

---

## 7 现在这台机器的状态

| 项 | 值 |
|---|---|
| 构建树 | WSL `/root/hakios`，11 GB，能编（§2） |
| 真构建树快照 | WSL `/root/hakios-build`（从 Z 盘 tar 解包，**不自洽，见 §4**） |
| 备份 | `/root/hakios-src-backup-090243`（drivers+configs）、`/root/lx-backup-195604`（lx/src） |
| ath11k.o | 756,220 B，已恢复成能编的版本 |
| 手机 | 小米 12X `23170c8b`，跑机用 `D:/IOS/scripts/phone-run.sh`（检测到 Android 立即退出） |
| prebuilt 内核 | `D:/IOS/repos/hakios-xnu-kernel/prebuilt/hakios-kernel`，**能跑，别动** |
