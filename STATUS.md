# STATUS — 这个工作区目前唯一可信的结论

> 落盘时间：2026-11-24 04:45（Asia/Hong_Kong）。
> 本文件只写**有日志证据**的结论。凡是没有日志/字节/实机回读支撑的说法，一律不写。
> 与之冲突的旧文档（尤其是 2026-11-24 凌晨那批结论）已移入 `docs/archive/20261124-wrong-conclusions/`，逐条反证在 `WHY.md`。

## 1. 一句话现状

三重驱动（**GPU + 触摸 + 电池**）的 XNU 内核**已经在 psyche 上实机跑通**，2026-11-24 04:16 与 04:22 各跑一次，两次都成功：
GPU 持续出图（活动窗口 58 fps，全程 `0 gave up`）、触摸真实报点 166 次、电池读数 37 次 0 失败、桌面渲染到屏幕并可触摸翻页。

| 项 | 值 |
|---|---|
| 可用内核 | `ROM/hakios-kernel` = `ROM/hakios-kernel.full-1119` |
| md5 | `0b7957f0b463e9dd30a5db808c97a477` |
| 大小 | 22,341,416 B |
| 构建时间（镜像内） | Tue Nov 19 01:30:30 CST 2026 |
| 验证日志 | `logs/041623-run.log`（904,336 B）、`logs/042229-run.log`（909,533 B） |
| 验证截图 | `logs/run-055.png`（4:23 首屏）、`logs/run-135.png`（4:24 第二屏） |

## 2. 复现配方（照抄即可，别再试别的组合）

三个必须同时成立的条件，缺一个就黑屏/软渲染/`bad size`：

1. **大镜像 `userland/system.apfs`（2,476,728,320 B）走 loader 的 ramdisk 通道，成为 resident `md0`**；
2. **小根 `userland/root.apfs`（10,838,016 B）作为 payload 的 `ramdisk=`，成为 `md1`** —— GPU 固件 `fw/a650_gmu.bin`（41,548 B）、`a650_sqe.fw`（31,964 B）、`a650_zap.*` 都在这份小根里；
3. **cmdline 必须把 `gpu=1 battery=1 battery_pmic=1 touchscreen=1 fb_ca_hz=120` 全带上**。

```bash
cd /d/DSH/IOS/ROM
python3 xnuloader/mkpayload.py -o run-payload.bin \
  kernel=hakios-kernel ramdisk=userland/root.apfs \
  --cmdline "loader_return_after=240 epoch=$(date +%s) sb_delay=8 touchscreen=1 bootcpu=7 cpus=8 cpufreq=1 gpu=1 battery=1 battery_pmic=1 fb_ca_hz=120"

# 先让手机进 loader（同一 session 不能跑第二个内核，必须先 xl reboot 离开）
fastboot boot /d/DSH/IOS/xnu.img

FB_TIMEOUT=60 fastboot stage run-payload.bin
fastboot oem run
fastboot getvar result          # 轮询到非 running
fastboot get_staged logs/$(date +%H%M%S)-run.log
```

实际跑通那次的 cmdline 原文（`logs/042229-run.log`）：

```
debug=0x8 rd=md0 battery_shared_pa=0x00000000a1013000 hakios_kver=2.3.4-hakios-darwin22.5-g252e6c7b
loader_return_after=240 epoch=1795465349 sb_delay=8 touchscreen=1 bootcpu=7 cpus=8
cpufreq=1 gpu=1 battery=1 battery_pmic=1 fb_ca_hz=120
```

### 三个已排除的坑（别再踩）

- `bash ./xl run` 内部直接执行 `xnuloader/mkpayload.py`，本机 MSYS bash 会把它当 shell 脚本跑，报 `line 10: Build: command not found`。**显式用 `python3` 调用**，不要改仓库里的 `xl`。
- 把 `ramdisk=userland/system.apfs` 塞进 payload → 2,499,070,064 B → `fastboot stage` 报 `remote: 'bad size'`。
- payload 完全不带 ramdisk → 内核把 resident 的 `system.apfs` 当根，adreno 立刻报
  `THE GPU IS NOT STARTED: the small image has no fw/a650_gmu.bin`，退回软件渲染。

### 环境前提

- platform-tools：`C:/Users/23239/Desktop/刷机与设备/platform-tools`，序列号 `23170c8b`。
- **跑之前 `adb kill-server`**：adb 抢 5037 会 `fastboot boot` 报 `AdbWriteEndpointSync failed`（信号灯超时 121）。
- 同一个 session 里跑第二个内核必死；换内核前先 `xl reboot`。

## 3. 三合一实测证据（原文 + 行号）

GPU 服务起来了（`logs/042229-run.log:634`）：

```
adreno: THE GPU STAYS ON for the system to draw with: the service HakiosGPU
```

触摸翻页那 5 秒的渲染量（`logs/042229-run.log:7881`，时间戳 1742.28）：

```
HakiosGPU: 1667 submissions in 5008 ms: 6430 command buffers and 79412 buffers named,
  1921 KB out of the cache in 60 us a submission;
  the GPU worked 573 us on each of 1667
  (0 under 0.1 ms, 630 under 0.25, 205 under 0.5, 538 under 1, 287 under 2, 7 under 4, 0 longer;
   955 ms in all); 5 waits of 861 us, 0 over before they began
```

同一段的出帧（`logs/042229-run.log:7882`）：

```
MiniIOMobileFramebuffer: 293 frames in 5018 ms (58 a second), 5412 us each in the driver,
  after 15 us waiting for the GPU and, 292 of them, 10707 us for the next refresh;
  289 swaps waited for the frame before, 0 gave up; 1 swaps came a refresh late themselves
```

**帧率不是恒定 58。** 同一个日志里 5 秒窗口依次是 13 → 29 → 25 → 19 → 58 fps，对应
「静态桌面（没什么要重画）→ 手指在屏幕上滑动翻页」。全程 **`0 gave up`**，这是驱动按需出帧，
不是掉帧故障。`logs/041623-run.log:7858/7897` 两处独立复现 58 a second、`0 gave up`。

屏幕与图形栈：

```
backboardd StartWindowServer main screen {1125,2436} scale:3
hakios-gl: OpenGL ES 3.2 Mesa 25.0.7; FD650
```

触摸（真报点，不是假活；`logs/042229-run.log:1737 / 7871 / 7879`）：

```
hakios: input: touch screen service 0x100f3bb40, buttons 0x100f0a1a0
psyche_touch: poll=25000 reads=25000 nonempty=100 touches=166 bad=2 irq_low=100 last=(1079,2074)
psyche_touch: poll=25625 reads=25625 nonempty=100 touches=166 bad=2 irq_low=100 last=(1079,2074)
```

`touches` 从 0 一路涨到 166 并停在那里，`last=(1079,2074)` 是最后一次落点的原始坐标
（屏幕 1080×2400，2074 在屏幕下缘 —— 就是「上滑翻页」那个手势）。
对照：11-19 那轮是 `touches=0`，只探测到芯片、一个报点都没有。

电池（`logs/042229-run.log:2428 / 7702 / 7750 / 7809`）：

```
hakios: battery: SOC=71% battery=1 charger=1 charging=1
battery: SOC=71%, V=4069 mV, I=369 mA, T=29 C, battery=1 charger=1 charging=1 action=3
  (25 reads, 0 failed, from the loader's cache)
battery: SOC=71%, V=4045 mV, I=472 mA, T=30 C, battery=1 charger=1 charging=1 action=3
  (37 reads, 0 failed, from the loader's cache)
```

**一句必须说清的限定**：这 37 次读取的数据源写的是 `from the loader's cache` —— 
数值是真的（SOC / 电压 / 电流 / 温度 / 充电状态齐全，且随负载变化：I 从 355 → 369 → 472 mA），
但走的是 loader 缓存的快照路径，**不是内核经 PMIC/SPMI 实时读表**。
整份日志里 SPMI 只在 `buttons: step 3: the first transfers on the SPMI bus` 出现一次。
要做「真·PMIC 电量计」，得跑 `hakios.battery.c` 的非 cache-first 分支，那是另一件事。

## 4. 已知缺口（不是故障，别当回归查）

- 桌面上 **Weather / Calendar 两个 widget 是空白白块**（截图 `run-055.png` / `run-135.png` 可见）。缺的是 widget 的数据源进程，与 GPU/触摸/电池三条驱动无关。
- 桌面翻页正常（`run-135.png` 是触摸翻到第二页后截的），说明触摸事件真的进了 WindowServer。

## 5. WiFi —— 只跑通了一部分（用户 11-24 04:33 的原话是对的）

以 `docs/交接文档.md` §47.2（11-06 定论）为准，分阶段：

| 阶段 | 状态 | 证据 |
|---|---|---|
| PCIe / QRTR | ✅ | 枚举 1–18、SMMU 上下文、QRTR 双向通信 |
| QMI | ✅ | `IND_REGISTER`→`HOST_CAP`→`RESPOND_MEM`→board→M3；读到 `WLAN.HST.1.0.1.r1-01134-QCAHSTSWPL_SILICONZ-1.586138.2.619465.3` |
| 冷启动校准 mode 7 | ⚠️ | 12 次跑机只有 1 次被接受（`061152` CAL DONE + READY） |
| 校准后 restart | ⚠️ | SMMU 幂等 bug 已修（`fc7d35c`），修完还没跑到这一步 |
| mode 0 → HTC → `en0` | ⛔ | **从未到达** |

固件自断言原文：`fw log 0x2ffe80: Asserted in0x1570b70s:1979`（`0x1570b70` = 返回地址，`1979` = 行号）。
同一份输入三种失败形态随机出现 → 是**竞态**，不是参数错。

### 跑 WiFi 的条件

- 内核必须**重编**（当前 `hakios-kernel.full-1119` 不含 AMSS/WLAN 固件）。11-24 新增 `drivers/wifi/hakios.amss.c`（内嵌 `amss20.bin` 4,352,144 B，9 段 × 524,288，首段 SBL 走 BHI、整份走 BHIe）与 `drivers/wifi/hakios.wlanfw.c`（47 个 board 文件 + M3）。
- cmdline 要带 `pcie=1`，`cpus=2`，以及 `wifi_mac=02:00:00:12:34:56`。
- 现成脚本：`_work/run-wifi-ath11k.sh`（`CPUS=2 SECS=240 EXTRA="wifi_mac=02:00:00:12:34:56"`）。

今天这两轮**WiFi 一动没动**：日志里只有 launchd 的
`pending spawn, domain in on-demand-only mode: com.hakios.wifi / com.apple.wifid`，`pcie:` 行数为 0 —— 因为 cmdline 没带 `pcie=1`。

## 6. 被推翻的旧结论（这就是要清掉的那批）

| 旧说法 | 出处 | 反证 |
|---|---|---|
| 「22,341,416 字节那个内核 GPU 在 submission 101 就死，`worked 0 us`」 | `docs/gpu-regression.md`（11-24 00:13，已加冲突提示） | 同一个 22,341,416 B 文件今天跑出 **1667 submissions / 573 µs each**，无 `no answer` |
| 「19 Nov 内核 + `root.apfs` + `touchscreen=1` 必然无响应，`touches` 计数器再也到不了」 | `merge-audit.md`（11-24 03:40，已归档） | 今天正是这个组合（`full-1119` + `root.apfs`）跑通，`touches=166` |
| 「死因在 poll 线程 `fts_thread`，不在供电」 | `merge-audit.md`（已归档） | 同一二进制 + 正确 ramdisk 结构即可正常跑，说明之前是 payload/根结构配错，不是线程 |
| 「`soc_name=unknown`、设备树 len 从 4438 变 3874，是有人动过工作区」 | 上一轮口述推断 | 设备树由 `xnuloader/loader.c` 自己生成（第 8-9 行注释：`builds an Apple-format device tree`），len 差异由 boot-args 决定 |
| 「工作区被破坏性读取过，今天的结论文档不可信」 | 用户 11-24 04:0x | 方向对：不可信的正是凌晨那批**结论文档**；但备份镜像的 ctime/mtime 自洽，未见批量回填痕迹 |

这五条已经写进 `docs/archive/20261124-wrong-conclusions/WHY.md`，与归档的原文放在一起，便于日后对照。

## 7. 归档说明

两侧分开放，因为一边要进 git、一边 418 MB 不能进：

**进仓库的文本**（`docs/archive/20261124-wrong-conclusions/`）：

- `merge-audit.md` —— 从 `docs/` 移出的当天结论文档（原文未改一字）。
- `WHY.md` —— 上面第 6 节那张表的详细版，逐条给日志行号与时间线。

**留在本地、被 `.gitignore` 挡在仓库外的二进制**（`_archive/20261124/`，随时可以拿回来）：

- `kernels/` —— 23 个非当前使用的历史内核镜像（pcie 阶段、wifi 阶段、gpu 试验、thermal 对照等），418 MB，文件名与 md5 全保留。
- `stubs/` —— `_amss_stub.c`、`_pcie_stub.c`、`_wlanfw_stub.c`、`_fix_makefile*.py`（11-19 那次「把驱动换成桩」试验的残留）。

留在 `ROM/` 根目录的内核只有两个：`hakios-kernel`（当前可用）与 `hakios-kernel.full-1119`（同内容备份，md5 都是 `0b7957f0b463e9dd30a5db808c97a477`）。

`.gitignore` 是白名单制（`/*` 全忽略，逐条 `!` 放行），这轮新增了 `!/STATUS.md`。

## 8. 下一步（按优先级）

1. **找回被我覆盖的那个 11-24 01:06 内核**（md5 `bc09f836e9fc7c48a48fb647452f57df`，22,344,256 B）：仓库里没有备份，只能重编 —— `wsl -d hakios -u root -e bash -lc "cd /root/hakios && bash tools/make-kernel.sh -j12"`。注意 `makedefs/MakeInc.cmd` 在 Linux 上因为 `xcrun` 不存在会让 `DSYMUTIL` 为空，报 `--minimize: command not found` / `MakeInc.kernel:131: Error 127`，需要 `export DSYMUTIL=/root/theos/toolchain/linux/iphone/bin/dsymutil`。
2. **WiFi 重编内核 + `pcie=1` 复跑**，看模式 7 校准的竞态能不能稳定命中。
3. 补 Weather / Calendar widget 的数据源（与驱动无关）。

## 9. 工作区其它材料的定位

| 位置 | 是什么 |
|---|---|
| `ROM/drivers/` | 驱动源码；`power/`（battery/battguard/pmic/cpufreq/thermal）、`input/`（psyche_touch/goodix/input）、`gpu/`（adreno/gpu.cpp）、`wifi/`（pcie/amss/wlanfw/wifi/wlfw/lx/sta/apple80211） |
| `ROM/userland/` | `system.apfs`（2.47 GB 根）、`root.apfs`（10.8 MB 小根，含 GPU 固件） |
| `ROM/logs/` | 608 个实机日志，128 MB。**是证据，不清理**；按 `HHMMSS-run.log` 命名，日期看 mtime |
| `D:/DSH/IOS/docs/交接文档.md` | 365 KB 总账，§47.2 是 WiFi 定论 |
| `_work/` | 跑机脚本（`run-wifi-ath11k.sh` 等）与临时产物 |
