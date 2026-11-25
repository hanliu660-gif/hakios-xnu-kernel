# replica.md —— 换一台电脑，从零复现 psyche 的最好状态

> 目标状态：GPU 出画面 + 触摸翻页 + 电量读数（logs/042229-run.log 那次）。
> 配方唯一权威：STATUS.md（跑机）与本文（从零建环境）。冲突以实测日志行号为准。

## 0. 什么可再生、什么必须随身带

| 资产 | 再生方式 |
|---|---|
| 内核 hakios-kernel | tools/make-kernel.sh（本仓库源码） |
| loader.efi / xnu.img | xnuloader/build.sh + mkxnuimg.py |
| root.apfs | userland/build.sh（每次重建） |
| system.apfs | userland/build.sh ← sysimage/ ← IPSW |
| sysimage 提取脚本 | 本仓库 sysimage/（已 track） |
| theos + iPhoneOS16.5.sdk + Linux 工具链 | xnu-deps/setup.sh 全自动（SDK 走 github.com/theos/sdks sparse-checkout） |
| IPSW iPhone10,3,iPhone10,6_16.5_20F66 | 自己下载，6 165 698 198 B（Apple 官方） |
| **SILICIUM_UEFI_psyche.fd** | 本仓库 `devices/psyche/prebuilt/` 已收（3,145,728 B，md5 `513fa373`） |
| **SILICIUM_UEFI.fd**（thyme 变体，实机成功款） | 本仓库 `devices/redwood/prebuilt/` 已收（md5 `2acd6396`） |
| **device-tree.dtb** | psyche 自己的设备树，本仓库 `devices/psyche/prebuilt/` 已收（842,746 B，md5 `9daef336`）；也可 `adb pull /sys/firmware/fdt` 重取 |
| **BootShim.bin** | 由 `device/redwood/prebuilt/BootShim-src/` 现编（`Makefile` 在仓库里，`FD_BASE` 传参） |

## 1. 环境一台新机器

- Windows + WSL2 Ubuntu 24.04（发行版名任意，本文用 hakios）。
- WSL 里进仓库根跑一次：bash xnu-deps/setup.sh（两个 stage；需要先有 theos：
  git clone --recursive https://github.com/theos/theos.git 到 $HOME/theos，
  工具链 toolchain/linux/iphone 由 its setup 装）。
- 自检：bash tools/doctor.sh —— 要求 "theos with the iPhoneOS 16.5 SDK and the cctools linker"。
- theos 的 Linux 工具链不是 `git clone --recursive` 自带的：clone 完还要跑 `bash ~/theos/bin/install-theos`，
  它按平台装 `toolchain/linux/iphone`（cctools）并把 SDK 收进 `sdks/`。install-theos 拒绝以 root 运行，要用普通用户；
  装完 `/root/theos/toolchain/linux/iphone/bin/ld` 必须存在，否则 `xnu-deps/setup.sh` 第 104 行会 die。
- 编内核前：export DSYMUTIL=/root/theos/toolchain/linux/iphone/bin/dsymutil
  （缺它 MakeInc.cmd 报 --minimize: command not found / MakeInc.kernel:131: Error 127）。
- Windows 侧：platform-tools（fastboot/adb）；跑机时 adb kill-server 先（抢 5037 会让 fastboot boot 报 AdbWriteEndpointSync failed）。

## 2. 源码

    git clone https://github.com/hanliu660-gif/hakios-xnu-kernel.git hakios
    cd hakios && git checkout psyche        # psyche 的全部改动在这条分支（fe39db5）

## 3. IPSW → sysimage（一次性，约 8 GB 中转盘）

    IPSW=<你下载的 iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw 的路径> sysimage/unpack.sh

产出（sysimage/ 下）：rootfs/、system.files、system.manifest、var.skel、cryptex/、cryptex-app.dmg、
20F66__iPhone10,3_6/（dyld 共享缓存）、DeviceTree.d22ap.im4p。
 psyche 身份：xnuloader/mkidentity.sh 从 DeviceTree 生成 identity.h（loader 编译时用）。

## 4. userland（root.apfs 10 MB + system.apfs 2.4 GB）

    cd userland && SYSTEM=1 bash build.sh

（root.apfs 每次重建；system.apfs 只在输入变新时重打。约 4 分钟。）

## 5. 内核 + loader + xnu.img

WSL 内（仓库在 /root/hakios，或自行挂载路径）：

    export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:/root/theos/toolchain/linux/iphone/bin
    bash tools/make-kernel.sh -j12          # -> hakios-kernel

WSL（BootShim 现编要 clang/llvm-objcopy；实机验证过的默认值不用改）：

    cd xnuloader && bash build.sh           # -> loader.efi（identity.h 已在则直接编）
    cd .. && export CC=/usr/bin/clang-18 OBJCOPY=/usr/bin/llvm-objcopy-18
    python3 mkxnuimg-psyche.py -o xnu.img   # psyche 专用打包器（FD_BASE=0x9FC00000）
    # 默认 FD = devices/redwood/prebuilt/SILICIUM_UEFI.fd（thyme，11-03/11-24 实机成功款，md5 2acd6396）；
    # psyche 变体 = devices/psyche/prebuilt/SILICIUM_UEFI_psyche.fd（md5 513fa373），用 --fd 换上做诊断。
    # DTB 默认 devices/psyche/prebuilt/device-tree.dtb（psyche 自己的，md5 9daef336）。
    # 已逐段对比黄金参照（顶层 11-03 xnu.img）：shim/FD/DTB/ramdisk 全等。
    # mkxnuimg.py 与 mkxnuimg-psyche.py 的 FD_BASE 都是 0x9FC00000（redwood 的 0x9FD00000 是过时说法，
    # 见 mkxnuimg.py:20）；两者的差别在 DTB 的来源与 FD 的默认值，不在基址。

## 6. 跑机（= STATUS.md 配方，此处为完整抄录）

进 loader（手机在 fastboot，序列号 23170c8b）：

    fastboot boot <路径>/xnu.img

打包并运行三合一（GPU+触摸+电池；battery=1 走 loader 缓存读数）：

    python3 xnuloader/mkpayload.py -o run-payload.bin kernel=hakios-kernel ramdisk=userland/root.apfs \
      --cmdline "loader_return_after=240 epoch=<now> sb_delay=8 touchscreen=1 bootcpu=7 cpus=8 cpufreq=1 gpu=1 battery=1 battery_pmic=1 fb_ca_hz=120"
    FB_TIMEOUT=60 fastboot stage run-payload.bin
    fastboot oem run
    # 轮询 fastboot getvar result 直到非 running；fastboot get_staged <path> 取日志

大镜像进 resident：xl ramdisk userland/system.apfs（md0 是根，GPU 固件 root.apfs=md1 里有）。
同 session 跑第二个内核必死：先 xl reboot 离开 loader 再 fastboot boot。

## 7. 验收（对 STATUS.md 的行号）

- adreno: THE GPU STAYS ON …（042229-run.log:7881，1667 submissions / 573 us）
- MiniIOMobileFramebuffer: 293 frames … 0 gave up（:7882）
- psyche_touch: … touches=166 bad=2 last=(1079,2074)（:7871）
- battery: SOC=… (37 reads, 0 failed, from the loader's cache)（:7809）
- 截图桌面翻页 = run-055.png / run-135.png

## 8. 已知缺口

- Weather / Calendar widget 空白。
- WiFi 停在 **WMI unified ready**（不是 QMI）：固件收下 `WMI_INIT_CMD` 却不回事件，
  5.004 s 超时。跑 WiFi 线要 `pcie=1 cpus=2 wifi_mac=…(硬性) wifi_ini_cfg=2|3`；
  内核**不用重编** —— `prebuilt/hakios-kernel` 已含 AMSS / M3 / 21 个 board / 完整 ath11k。
  全部细节与复跑命令：[wifi-progress.md](wifi-progress.md)。