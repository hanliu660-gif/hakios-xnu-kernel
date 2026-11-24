# 从 fastboot 到 iOS 桌面（本次实测流程）

2026-11-24 04:22 的那次成功启动，用的就是本目录里的 `xnu.img` + `hakios-kernel` + `userland/system.apfs` + `userland/root.apfs`。
下面按顺序写清楚每一步、每个坑、以及怎么判断成功。

---

## 0. 前置条件

| 项 | 要求 |
| --- | --- |
| 手机 | 小米 12X（psyche），**Bootloader 已解**，当前在 Android 或 fastboot |
| 电脑 | Windows / Linux 均可；`fastboot` 在 PATH（platform-tools） |
| 其它 | Python 3（只有重打 payload 时才用）；USB 线能传 2.3 GB |
| 注意 | 全程**不刷任何分区**，全部在内存里跑；断电/拔线最多丢掉这次运行 |

判断手机现在在哪：

```bash
adb devices          # 有 device = 在 Android
fastboot devices     # 有 <sn>  fastboot = 在 bootloader 或 loader
fastboot getvar product   # product: xnuloader = 已经在 loader 里
```

### Windows 上的 USB 驱动（一次说清，别再折腾自签 cat）

**先分清两件事：这台机器装没装过 `android_winusb.inf`。**

- **没装过** → loader 就是"未知设备"，设备管理器里带黄色感叹号，`fastboot devices` 永远空。
  这一步是真的要装驱动：包里 `drivers/windows/android_winusb.zip`（2.5 MB，已签名），
  `pnputil /add-driver .\android_winusb.inf /install` 一条命令，详见 `drivers/README.md`。
- **装过之后** → `USB\VID_18D1&PID_D00D` 被**按 PID 点名**（`%SingleBootLoaderInterface%`）认成
  *Android Bootloader Interface*，这时才"不需要额外动作"：不需要自签 `.cat`、不需要 `inf2cat`、
  不需要 `bcdedit /set testsigning on`。

loader 报的接口描述符是 `Class_FF&SubClass_42&Prot_03`（`xnuloader/loader.c` 第 3625/3631 行，loader 自己报的，
不是 UEFI FD）。Google 的 `android_winusb.inf` 里有对应的 `%SingleAdbInterface%` 那条，
但本机实际绑定走的是 PID 点名，两条路径都不需要自签。

自检（手机插好、进 bootloader）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\win-usb-check.ps1
```

[1] 里出现 `Status=OK` + `Android Bootloader Interface` 才算驱动这层通了；
`Status=Unknown` 只是设备当前不在场，`Status=Error` + `Problem=28` 才是真没驱动（去装上面那个包）。
其余两种失败不是驱动问题：`fastboot devices` 空但设备正常 → `adb kill-server`（§1）；手机还在 Android → 进 bootloader。

---

## 1. 关掉 adb（必须）

adb 会占住 5037 端口，`fastboot boot` 会报：

```
fastboot: error: AdbWriteEndpointSync failed: 信号灯超时时间已到 (121)
```

```bash
adb kill-server
```

（本机经验：kill 之后立刻正常，不用重启手机。）

---

## 2. 进 bootloader，再进 loader

```bash
adb reboot bootloader     # 已在 fastboot 就跳过
fastboot boot xnu.img     # 把 loader 送进内存并启动，不写分区
sleep 8
fastboot devices          # 期望：<序列号>  fastboot
fastboot getvar product   # 期望：product: xnuloader
```

`xnu.img` 结构（想自己重打看 `docs/replica.md` §5）：

```
ANDROID! header (v1, page 2048)
kernel = gzip(BootShim 208B + UEFI FD 3 MB + usb-payload 71 KB: loader.efi) + DTB 842,746B
ramdisk = 5 B
```

BootShim 的 `FD_BASE=0x9FC00000`（psyche 版）+ thyme FD + psyche DTB，三段都与 11-03 的黄金镜像逐字节相同。

---

## 3. 上传常驻根镜像（2.31 GiB，一次就够）

loader 会把这 2.3 GB 放进手机 RAM，作为内核的 `/dev/md0`（root 文件系统），之后**跨多次 run 不丢**，直到 `fastboot oem ramdisk none` 或断电重启。

```bash
fastboot oem ramdisk                        # 让 loader 预留内存窗口
FB_TIMEOUT=1800 fastboot stage userland/system.apfs
```

- 传 2.3 GB 视 USB 速度 2–10 分钟。
- **默认 `FB_TIMEOUT` 只有 10 秒**，不设大一定失败。
- 失败重来时先 `fastboot oem ramdisk none` 清掉再传。

日志里对应的成功标志（`proof/042229-run.log` 开头）：

```
resident image at pa fc000000 size 93a00000: it is the root (md0), the payload ramdisk is md1
```

---

## 4. 跑内核

```bash
FB_TIMEOUT=120 fastboot stage run-payload.bin
fastboot oem run
```

`run-payload.bin` 是 `hakios-kernel` + `userland/root.apfs` + 启动参数打成的 payload（31.6 MB）。要重打：

```bash
# 与包里的 run-payload.bin 逐字节相同（实测 cmp 无差异，33,179,760 B）
python3 xnuloader/mkpayload.py -o run-payload.bin \
  kernel=hakios-kernel ramdisk=userland/root.apfs \
  --cmdline "$(cat payload-cmdline.txt)" --kernel-version "$(cat kernel.ver)"
```

原始命令行（`payload-cmdline.txt`，其中 `epoch` 只是打 payload 那一刻的时间戳，不参与任何判断）：

```
loader_return_after=240 epoch=1795465349 sb_delay=8 touchscreen=1 bootcpu=7 cpus=8 cpufreq=1 gpu=1 battery=1 battery_pmic=1 fb_ca_hz=120
```

内核版本串（`kernel.ver`）：`2.3.4-hakios-darwin22.5-g252e6c7b`。
少了这个条目，payload 会短 72 字节，loader banner 里没有 `kernel.ver=`，内核也拿不到 `hakios_kver` 这个 boot-arg。

**三条不能省的结构**（都是实测踩出来的）：

1. **payload 里绝不能带 system.apfs** —— 曾把 2.3 GB 直接塞进 payload，`fastboot stage` 报 `bad size`；
   大镜像只能走第 3 步的 `oem ramdisk`。
2. **`ramdisk=userland/root.apfs` 必须有** —— 少了它，内核会把常驻的 system.apfs 当根，
   GPU 固件（`fw/a650_gmu.bin` 在 root.apfs 里）找不到，adreno 报
   `THE GPU IS NOT STARTED: the small image has no fw/a650_gmu.bin`，画面退回软件渲染。
3. `cpus=8`、`bootcpu=7`、`gpu=1`、`touchscreen=1`、`battery=1`、`battery_pmic=1`
   决定这次启动带哪些驱动 —— 本包的 `run-payload.bin` 已经带全。

跑起来后：

- 默认 `loader_return_after=240`，即 4 分钟后内核自己交还 loader。
- 想提前结束：`fastboot oem stop`。
- 想看它跑到哪：`fastboot getvar result`（`running` → 结束状态）。

---

## 5. 收日志与截图

```bash
mkdir -p logs
fastboot oem log        >/dev/null && fastboot get_staged logs/boot.log
fastboot getvar result                       # 结束状态
fastboot oem screenshot >/dev/null && fastboot get_staged logs/screen.raw   # loader 侧抓屏（原始 RGB）
```

`logs/screen.raw` 用 `tools/` 里的脚本或 `xl screenshot` 转 PNG（`xl` 内部就是这么做的）。

---

## 6. 成功的判定（五条，缺一条就是没起全）

在 `logs/boot.log` 里搜：

| 关键行 | 含义 |
| --- | --- |
| `HakiosGPU: … submissions in … ms, … us each, … command buffers` | GPU 在真跑（不是软件渲染） |
| `MiniIOMobileFramebuffer: … frames in … ms (… a second), … swaps waited …, 0 gave up` | 显示链路正常出帧 |
| `psyche_touch: poll=… reads=… nonempty=… touches=N`（N>0） | 触摸真报点（不是"假活"的 touches=0） |
| `battery: SOC=…, V=… mV, I=… mA, T=… C, battery=1 charger=1` | 电量计在读 |
| `StartWindowServer: initial main screen size:{1125, 2436} scale:3` | 桌面窗口服务起来了 |

对照样本：`proof/042229-run.log`（1667 submissions / 573 µs、293 frames / 0 gave up、touches=166、37 reads 0 failed）。

---

## 7. 回 Android

```bash
fastboot reboot          # 或者 xl reboot
```

注意：**同一个 loader 会话里跑第二个内核必死**（loader 的内部状态只能用一次）。
要再跑一遍，先 `fastboot reboot` 回 Android，再从头做第 2 步。

---

## 8. 故障对照

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `AdbWriteEndpointSync failed` | adb 占着 5037 | `adb kill-server` |
| `fastboot boot` 后 `fastboot devices` 空 | loader 没起来，或手机不是 fastboot | 看手机屏幕；`fastboot reboot` 重来 |
| `stage` 报 `bad size` | payload 里塞了 2.3 GB 的 system.apfs | 大镜像只走 `oem ramdisk` |
| `stage` 卡住/超时 | `FB_TIMEOUT` 太小 | `FB_TIMEOUT=1800` |
| 画面是软件渲染、`THE GPU IS NOT STARTED` | payload 少了 root.apfs | 用本包的 `run-payload.bin` |
| 内核跑到一半死 | 同一 loader 会话第二次 run | 回 Android 重来 |
| 黑屏但日志正常 | framebuffer 未刷新/亮度 | `fastboot oem screenshot` 看 loader 侧画面 |
