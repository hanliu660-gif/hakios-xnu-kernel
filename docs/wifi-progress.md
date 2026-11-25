# WiFi：走到哪了，卡在哪，别再碰什么

> 这份文件把 2026-11-17 / 11-18 那两轮实机实验的结论搬进仓库 —— 它们此前只在一份本地
> 工作日志（`交接文档.md` §51/§52）里，仓库外面，随时会丢。
>
> 每条结论后面都标了它的来源：**【实测】** 有日志轮次号，**【字节】** 2026-11-25 在本机
> 对 `prebuilt/hakios-kernel` 直接验的，**【推断】** 没有直接证据。
> 与 `STATUS.md` 冲突时以 `STATUS.md` 为准；本文是它的 WiFi 部分的展开。

---

## 0 一句话

**电台、链路、QMI、固件加载全部通了，HTC 与 WMI 也已经穿过 —— 卡在
`ath11k_wmi_wait_for_unified_ready`，固件不答 `WMI_INIT_CMD` 的 unified ready 事件，
5.004 秒后超时（`-60`）。** `en0` 从未出现。

---

## 1 硬件与代码的来路

| 项 | 值 |
|---|---|
| 电台 | **QCA6390 on PCIe0**（不是 WCN6750 —— psyche 的 WCN6750 说法在 §16 已被推翻） |
| 驱动 | Linux `ath11k`，**未经改动地**编译进 XNU（一处例外，见下） |
| ath11k 版本 | Linux **v7.0.14**（`tools/mk-ath11k.sh` 从 kernel.org 取） |
| LinuxKPI 头 | FreeBSD **15.1**（`wifi/lx/patches/*.patch` 施加） |
| 产物 | 单个目标文件 `xnu-deps/build/ath11k/out/ath11k.o`；`xnu-build.sh` 在它存在且配置带 `hakios_wifi` 时才链进内核 |
| 我们对 ath11k 的全部改动 | `qmi.c` 的 **56 行**，两个意图（见 §4） |

**【字节】** 2026-11-25 核对 `prebuilt/hakios-kernel` 的 Mach-O 符号表：
`_ath11k*` **669 个**，`_lx_*` **95 个**，`_hakios*` **228 个**。
`ath11k_core_init` / `ath11k_hal_srng_init` / `ath11k_ce_alloc_pipes` / `ath11k_htc_init` /
`ath11k_htc_wait_target` / `ath11k_wmi_wait_for_unified_ready` / `ath11k_dp_alloc` /
`ath11k_qmi_wlanfw_mode_send` / `ath11k_mac_register` / `ath11k_qmi_firmware_start` 全在。
**也就是说：仓库里这颗内核已经带着完整的 ath11k，不需要"先重编"。**

---

## 2 分阶段状态

| 阶段 | 状态 | 证据 |
|---|---|---|
| PCIe 枚举 / SMMU 上下文 / QRTR 双向 | ✅ | 【实测】枚举 1–18 号设备，QRTR 收发正常 |
| QMI 握手 → 读固件版本 | ✅ | 【实测】`IND_REGISTER`→`HOST_CAP`→`RESPOND_MEM`→board→M3；读到 `WLAN.HST.1.0.1.r1-01134-QCAHSTSWPL_SILICONZ-1.586138.2.619465.3` |
| mode 7 冷启动校准（拿 `cal_done 1`） | ⚠️ 抽签 | 【实测】约 1/3 命中；命中后固件基本不再崩（6 轮里 5 轮健康） |
| 校准后 restart（第二会话继承校准） | ✅ 走通过 | 【实测】`015551`、`035159`：`cal_done 1` → mode OFF(4) 返回 14 字节 → 第二会话 `cal_done 1` → board/M3 reuse 生效 |
| mode 0 → HTC → HTT → WMI → `en0` | ⚠️ **到 WMI unified ready 为止** | 【实测】`005222`、`222902` 两轮穿过 HTC target ready、HTT connect、WMI connect、HTC start、WMI service ready、mac_allocate、REO setup、WMI init cmd |
| `en0` 出现在 Darwin 里 | ⛔ **从未到达** | 同上，卡在下一步 |

**注意这张表修正了一个流传很广的说法**：不是"停在 QMI"，也不是"停在 HTC"。
QMI 早通了，HTC 至少两次穿过。真实的墙是 **WMI 层的 unified ready**。

---

## 3 当前唯一的卡点：`wmi_wait_for_unified_ready`

【实测】`005222`（`wifi_ini_cfg=3` + `wifi_mode_delay=200`）：

```
[ 53.011743] the rings are set up and the copy engines have their buffers: the bus is started
[ 58.015828] failed to receive wmi unified ready event: -60     ← 差 5.004 s，正好 WMI 超时
```

固件收下了 `WMI_INIT_CMD`，但没有回 unified ready 事件。

**要查的是**：`ath11k_wmi_cmd_init()` 组出来的 `WMI_INIT_CMD` 与这个固件期望的是否一致 ——
QMI 之外 WMI 层的 service ID、以及 HTT 连接是否配对。这是**唯一还没被研究过的一层**。

---

## 4 深度排名与前提条件

### 4.1 全部 536 轮按"走到 `ath11k_core_start()` 第几步"排名

【实测】只有三档：

| 深度 | 轮次 | 卡在 `core_start()` 的第几步 |
|---|---|---|
| **L3** | `005222`、`222902` | 第 13 步 `wmi_wait_for_unified_ready` |
| L1 | `223743` | 第 4 步 `htc_wait_target` |

`005222`/`222902` 比 `223743` **多走 9 步**。此前长期把 `223743` 当成最深的样本，
是错的。

### 4.2 CFG 是进入深处的必要条件（不是充分条件）

【实测】按有没有 `the cfg goes out` 统计 528 轮：

```
发了 CFG :   8 轮，其中进入 core_start 深处 3 轮   (37.5%)
没发 CFG : 520 轮，其中进入 core_start 深处 0 轮   (  0% )
```

三个深轮全部带 CFG 位（`wifi_ini_cfg` = 3 / 2 / 3）。
**只发 INI 不发 CFG（`wifi_ini_cfg=1`）的 4 轮，一次都没深入过。**

⚠️ 这与"冷启动只发 mode"并不矛盾 —— 那条只对 **mode 7** 成立；
**对 mode 0（第二会话）必须发 CFG**。

### 4.3 `wifi_mac` 是硬性前提

【实测】全部 536 轮里，**没有 `wifi_mac` 的 432 轮，mode 0 一次都没到过**。

### 4.4 一组可用的参数

```
wifi_mac=02:00:00:12:34:56
wifi_ini_cfg=3
wifi_mode_delay=200
wifi_cal_delay=20        # 10 ms 必死；20 ms 命中率最高；50 ms 一成二败；100 ms 必败
pcie=1  cpus=2  bootcpu=7
```

**`EXTRA` 是整体替换，不是追加。** 少写一个键，就等于把它退回缺省值 ——
历史上至少三次实验因此作废（`wifi_cal_delay` 退回 50 ms、`pcie_bdf` 退回错板）。

---

## 5 已经写进内核、但需要 mode 7 命中才会执行的补丁

【字节】三个都在 `prebuilt/hakios-kernel` 里：

| 符号 / 字符串 | 作用 |
|---|---|
| `wifi_cal_off` | mode 4（校准后 OFF）开关，现在默认关 |
| `wifi_no_reuse_fw` | 让 BDF/M3 的 skip 失效，强制重新下载 |
| `the calibration is the last session's, so there is no handshake to make` | `cal_inherited` 生效点 |
| `wifi_cal_gap` | 校准后静默窗口 |

【实测】这三个补丁**只被执行过一次**：`083605`。

```
203: cold boot: the firmware is put into the off mode
208: cold boot: the off mode returned 14
210: cold boot: 4000 ms with the endpoint down          ← wifi_cal_gap 生效
216: host cap: cal_done 1                              ← 继承生效
231: cold boot: the calibration is the last session's, so there is no handshake to make
233: host cap: cal_done 1                              ← 值保住
```

然后 `round 2` 在跳过 BDF/M3 之后**死等**（跑满 240 s 无任何 QMI 消息）。

由那次失败导出的修复（在 `qmi.c` 里，已编进内核）：

```c
/* BDF 与 M3 的 skip 各加一个条件 */
if (lx_os_option("wifi_no_reuse_fw") == 0u && ab->qmi.cal_done != 0 &&
    ab->qmi.cal_inherited == 0) { ... return 0; }
```

理由：`cal_done` 只说明校准**发生过**，不说明字节还在 —— `power_down/up` 会清空固件内存。

---

## 6 重试路径本身是坏的（`wifi_tries` 不是"再抽一次签"）

【实测】`021930`（`wifi_tries=3`）的三次尝试：

| attempt | 死法 |
|---|---|
| 1 | `mode 7` 超时 `-60`（固件已死） |
| 2 | **连 BDF 都发不出去**：`failed to wait board file download request: -60` |
| 3 | 同上：`failed to load board data file: -60` |

`reuse_fw` 让第二实例跳过 BDF/M3 的**发送**，但 `ath11k_qmi_load_bdf_qmi()` 仍在**等一个回应**，
而那个固件实例根本没起来，于是 `-60`。

**要提高命中率，重试必须从"重新枚举 + 重新下载全部固件"开始，而不是复用。**
按 1/3 命中算，N 次独立抽签的把握是 1−0.67^N：3 次约 70%，5 次约 87%。

---

## 7 提前判废：一个省时间的指标

【实测】看 regdb 尾片（961 字节，transaction 8）的 ack：

```
qmi: in 14 bytes from 7:1: type 2, transaction 8, message 37
```

- `txn8_ack = 1` → 后面 mode 7 一定会发出去（14 轮 100% 一致）
- `txn8_ack = 0` → BDF 就卡死在 `-60`，mode 7 根本不会发

用它可以在 mode 7 之前 10 秒以上判废一轮，不必等超时。

---

## 8 负结论：这些别再碰（都是实测排除的）

| 追过的假设 | 判据 | 结论 |
|---|---|---|
| `wifi_tries` 污染第一实例 | `013430` 用成功轮逐字相同的参数仍失败 | ✗ |
| `pcie_swreset` | 已默认关，关掉后仍失败 | ✗ |
| `pcie_linkretry` | 两轮都 `=0` | ✗ |
| 电源 revote | `smpa4/5/6`、`smpc8`、`ldoa5/9` 日志成功失败完全一致 | ✗ |
| 发出的字节 | 52 B、transaction 20、completion `0x2000034` 完全一致 | ✗ |
| 发出时刻 | 成功 43.1–46.4 s，失败 43.1–44.9 s，**区间重叠** | ✗ |
| 固件进错误 EE | `BHI_EE` 字段（`dword1 & 0xff`）恒为 0 | ✗ |
| `TF:bad user tlv` | 18 轮只出现 1 次 | ✗ 噪声 |
| fw log 里的 `PHY_ON:Invalid Chip` | 是镜像内的**字符串模板**，不是运行时输出 | ✗ 误读，已撤回 |
| `MHICTRL=2` 清 SYSERR | `0xff05` 写前写后不变；第二次重试 `0x201`（完全干净）仍失败 | ✗ SYSERR 与成败无关 |
| `pcie_swreset`/`sw_reset` 整条线 | 同上 | ✗ 可以关掉 |
| antenna switch sync（`0x0047`） | 补上后 1/4 = 25%，与历史 32% 在统计噪声内 | ✗ 不是决定因素 |

**另有一条经验**：`hakios_pcie_ee_read()` 目前返回不了真值
（`before mode 7 the chip says it is in state -1` 里的 `-1` 是"窗口没映射"的哨兵）。
功能接好了但取不到数；EE 已被证明不是瓶颈，优先级压到底。

---

## 9 跑一次 WiFi 的最小步骤

```bash
# 1) 内核：仓库里这颗就够（含 AMSS + 21 个 board + M3 + 完整 ath11k）
strings -a prebuilt/hakios-kernel | grep -c "failed to receive wmi unified ready"   # 期望 1
strings -a prebuilt/hakios-kernel | grep -c "THE GPU STAYS ON"                       # 期望 1

# 2) payload：把 WiFi 的参数写进 cmdline
python3 xnuloader/mkpayload.py -o /tmp/wifi-payload.bin \
  kernel=prebuilt/hakios-kernel ramdisk=userland/root.apfs \
  --cmdline "loader_return_after=240 epoch=$(date +%s) sb_delay=8 cpus=2 bootcpu=7 \
pcie=1 wifi_mac=02:00:00:12:34:56 wifi_ini_cfg=3 wifi_mode_delay=200 wifi_cal_delay=20" \
  --kernel-version "$(cat kernel.ver)"

# 3) 上机（system.apfs 必须先 stage 成 md0，见 docs/boot-to-desktop.md）
FB_TIMEOUT=120 fastboot stage /tmp/wifi-payload.bin
fastboot oem run

# 4) 取完整日志：**不要调 oem log**（它会重组成心跳摘要并覆盖原始缓冲）
fastboot getvar result
fastboot get_staged logs/wifi.log
```

判废/判定：

```bash
grep -n "txn8_ack\|the cfg goes out\|the mode request goes out\|the bus is started\|\
wmi unified ready\|ctl_resp never came in" logs/wifi.log
```

| 看到什么 | 说明 |
|---|---|
| 没有 `qmi: in 14 bytes ... transaction 8` | BDF 就死了，本轮废，不必等 |
| 有 `the cfg goes out` | 有资格进入深处（必要条件） |
| `failed to receive wmi unified ready event: -60` | **到 L3 了 —— 这是当前的目标状态**，可用来做 WMI 层实验 |
| `ctl_resp never came in (-60)` | 只到 L1（比 L3 浅 9 步） |

---

## 10 下一步（按价值）

1. **抽到一次 mode 7 → 复现 `005222` 的 L3 → 专攻 `wmi unified ready`。**
   拿 `WMI_INIT_CMD` 的字节与固件 cap 里声明的能力逐字段对；查 HTT 连接是否配对。
2. **让重试变成真正的独立事件**（§6）：完整冷启动，不复用。
3. `hakios_pcie_ee_read()` 顺手修（要查 `pcie_map_regions()` 是否填 `va`），优先级低。

**不要再做的**：mode 4 / SYSERR / antenna switch（§8 全部实测排除）。

---

## 11 内核里到底带着什么 WiFi 固件

【字节】2026-11-25 在 `prebuilt/hakios-kernel` 里逐字节核对：

| 东西 | 值 |
|---|---|
| AMSS（`amss20.bin`，整份 ELF） | **4,352,144 B**，在核内偏移 `161416`；该区间 2,621,673 字节非零 |
| board 文件 | `hakios_wlanfw_board_count = 21`（数组 `board_0`..`board_20`），合计 **1,215,824 B**，21 份内容互不相同 |
| M3（`m3.bin`，ELF） | **266,684 B**，在核内偏移 `4513568` |
| 三类合计 | **1,482,508 B**（board + M3）+ AMSS 4,352,144 B |

**所以"本包内核不含 amss、要跑 WiFi 得先重编"这句话是错的** —— 见 [NOTES](NOTES.md) 里已更正的那段。

### 11.1 三处对不上的地方（记下来，别当成事实去用）

1. **文件头说 47，实际是 21。** `drivers/wifi/hakios.wlanfw.c` 的注释写
   "The board files are all forty-seven that this phone's package carries"，
   但同一份文件里 `hakios_wlanfw_board_count = 21u`，只声明了 `board_0`..`board_20`。
   21 份内容经 SHA-256 比对**互不相同**，所以不是生成器去重的结果 —— 是注释与数据本身不符。
   哪一个是对的，**没有证据判断**（生成脚本不在仓库里）。
   盘点 board 文件时以 `hakios_wlanfw_board_count` 为准。

   名字表在 `hakios.wlanfw.c:92750`（`hakios_wlanfw_boards[]`），21 项全列：

   ```
   board_0  bd_j11.elf      board_7  bd_j1s.elf     board_14 bd_k11ain.elf
   board_1  bd_j11_b.elf    board_8  bd_j2s.elf     board_15 bd_k81.elf
   board_2  bd_j11gl.elf    board_9  bd_j3s.elf     board_16 bd_k81a.elf
   board_3  bd_j11in.elf    board_10 bd_j3sgl.elf   board_17 bd_l3a.elf   ← psyche 的真板
   board_4  bd_j1_b.elf     board_11 bd_j3sin.elf   board_18 bd_l3agl.elf
   board_5  bd_j1gl.elf     board_12 bd_k11a.elf    board_19 bd_l81a.elf
   board_6  bd_j1in.elf     board_13 bd_k11agl.elf  board_20 bdwlan.elf
   ```

   **`bd_l3a.elf` 在里面（`board_17`，57,844 B）** —— 所以 `pcie_bdf=bd_l3a.elf`
   这条 cmdline 参数在这颗内核里仍可用，它指向的就是 psyche 自己那份板级数据。

2. **`pcie_bdf` 不是死参数**（旧工作日志 §52.6 说它只在
   `hakios.pcie.c.before-generation` 里被解析、当前代码搜不到 —— 那个文件本机不存在，
   而 `hakios.pcie.c` 里明明白白有它）：
   - `drivers/wifi/hakios.pcie.c:7581` —— `PE_parse_boot_argn("pcie_bdf", pcie_bdf_name, …)`
   - `drivers/wifi/hakios.wifi.c:4245` —— `wifi_umi_board()` 也读它

   而且**缺省值在两条路上不一样**，这会咬人：

   | 路径 | 没给 `pcie_bdf` 时 |
   |---|---|
   | `wifi_umi=1`（ath11k，我们用这条） | 退回 **`bd_j11.elf`** —— 不是 psyche 的板 |
   | 手写路线（`hakios.pcie.c` 的 stage 20/21） | 空串 → `mhi_wlfw_board()` 返回 NULL → `NO BOARD FILE IS NAMED` |

   所以 **`pcie_bdf=bd_l3a.elf` 要显式给**：`bd_l3a.elf` 是 `board_17`，才是 psyche 自己那份。

3. **生成脚本不在仓库里。** `tools/gen_wlanfw_blobs.py` 被
   `hakios.wlanfw.c` / `hakios.pcie.c` / `hakios.wifi.c` 三处注释引用，
   但 `git ls-files` 里没有它。要用自己的 `fw-wifi/` 重新生成 board 数组，
   得先按 `hakios_wlanfw_board` 结构体补出这个脚本（`{ name, size, bytes }` 三项，
   头部另有 `hakios_wlanfw_m3_bytes` / `_m3_size` / `_board_count`）。

---

## 12 这份文件与其它文档的关系

- `STATUS.md` §5 —— 摘要，指向本文。
- `docs/NOTES.md` §4 —— 摘要（已同步修正）。
- `docs/replica.md` §8 —— 已知缺口一句话（已改为指向本文）。
- `docs/driver-review/wifi-bringup.md`、`wifi-stack.md` —— **安全审计**（W1–W13），不是进度。
- 原来的 `交接文档.md` §51/§52 —— 本地工作日志，不在仓库里；本文是它的可核对摘要。
