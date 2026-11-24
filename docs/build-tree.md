# 怎么再把这颗内核编出来（构建树复现方法）

> 写在当事机器上，2026-11-25。下面每个数字都是这次实跑的，不是估的。
> 这里只放「方法」：命令、判定、踩过的坑、以及哪几样东西不在公开仓里。文件内容不进仓。

## 0 结论先说

- 内核只由 `xnu/`（81 个文件，含 `xnu/hakios.patch` 139,055 B）、`drivers/`（80 M）、`configs/`、`xnu-build.sh`、`xnu-env.sh`、`xnu-deps/setup.sh`、`Makefile` 决定。这些都在 `psyche` 分支上。
- `xnu-src`（586 M）和 `xnu-deps`（1.1 G）是**生成物**：`xnu/apply.sh` 和 `xnu-deps/setup.sh` 各一条命令就能重新长出来，不需要拷。
- 17 G 的构建树里，与内核有关的只有约 1.7 G，且其中 1.68 G 是可再生的；真正的「私有」部分只有 3 个文件（§3），其中只有 1 个可能进内核。
- 内核**内嵌构建日期**。`strings -a hakios-kernel | grep "Darwin Kernel Version"`：

  ```
  Darwin Kernel Version 22.5.0: Tue Nov 24 02:21:22 CST 2026; root:xnu-8796.121.2[xnu-8796.121.2-dirty]/BUILD/obj/DEVELOPMENT_ARM64_BCM2837
  ```

  包里那颗 11-19 的写的是 `Thu Nov 19 01:30:30 CST 2026`。这个日期由 XNU 自己的 `config/newvers.pl` 取：原文 `my $BUILD_DATE = `date`;`，**不认 `SOURCE_DATE_EPOCH`**；戳进生成的 `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/version.c`，实测该文件 mtime `2026-11-24 02:21:22.258567376`，与串里的时间一秒不差。
- 所以「逐字节复现某颗历史内核」= 相同的源 **加上相同的这行日期**，只对源码不够。
- 本次两次 `make kernel` 都是 md5 `f42652c8a85754de224a2af9a5243820`、sha256 `858ef2d59ab3c79364988127f9a9b6608aed6f9aa63acfb9a9d2d6ce1c513b94`、22,344,256 B。但这棵树已经编过，`mach.development.bcm2837` 与 `hakios-kernel` 的 mtime 停在 2026-11-24 02:21，make 什么都没重做 —— **第二次的「成功」只是把同一个文件又 `cp -p` 了一遍，不能当作可重现性的证据**。真验证见 §3 末。

## 1 这台机器上 17 G 的去向（`du -sh` 实测）

| 目录 | 体积 | 与内核的关系 |
|---|---|---|
| `sysimage` | 5.7 G | 无关。IPS W 解包出来的 Darwin userland，只喂 `xnuloader/mkidentity.sh` 与 userland 镜像 |
| `Mu-Silicium-new-4bd8` / `Mu-Silicium` | 5.1 G + 2.0 G | 无关。UEFI FD 的源码树；成品 FD 已进仓 `devices/redwood/prebuilt/SILICIUM_UEFI.fd` |
| `userland` | 2.4 G | 无关。`root.apfs` / `system.apfs` 的构建输入与产物，和内核各走各的 |
| `xnu-deps` | 1.1 G | **有关但可再生**：`xnu-deps/setup.sh` |
| `xnu-src` | 586 M | **有关但可再生**：`xnu/apply.sh`（Apple 的 `xnu-8796.121.2` + `hakios.patch`） |
| `venv-uefi` / `qemu` | 116 M + 80 M | 无关。UEFI 侧工具与模拟器 |
| `drivers` | 80 M | **有关，必须在**（已在仓里） |
| `tools` `docs` `configs` `xnuloader` `xnu` | 各 1–2 M | **有关，已在仓里** |

结论：要留 17 G 才能编内核，是误解。留 1.7 G 就够，而且 1.68 G 可以随时重下。

## 2 一条命令重建（只用公开源）

### 2.1 机器与依赖

实测环境：Ubuntu 24.04 LTS，`clang 11.1.0`（theos 自带，来自 `github.com/L1ghtmann/llvm-project` 的 `210562a8907c5f1ec4a02c7ac969d26d81d304a0`）、`clang-18 18.1.3`、`ld64-609`。

```sh
sudo apt install -y git curl tar xz-utils make clang clang-18 lld lld-18 \
  llvm-18 llvm-18-dev llvm-18-tools llvm-18-linker-tools \
  bison flex m4 perl python3 autoconf automake libssl-dev gawk nasm cmake
```

`bison flex m4` 是 XNU 自己的构建工具；`clang-18`/`llvm-18` 只给 `xnuloader/build.sh`（BootShim）与 `mkxnuimg-psyche.py` 用；**内核本身用的是 theos 里的 `clang-11`**（`xnu-build.sh` 的 `CC=xnu-clang CXX=xnu-clang++`）。

### 2.2 theos（cctools + Darwin SDK）

`xnu-deps/setup.sh` 只检查两样东西，检查不到就明确报错退出：

```sh
$THEOS/toolchain/linux/iphone/bin/ld      # cctools 的链接器（实测 ld64-609）
$THEOS/sdks/iPhoneOS16.5.sdk/usr/include   # Darwin SDK
```

本机 `$THEOS = ~/theos`（remote 是 `https://github.com/theos/theos.git`，HEAD `6e7b8c4`），里面既有 `sdks/iPhoneOS16.5.sdk` 也有 Linux toolchain。装法两条都可：

```sh
git clone --recursive https://github.com/theos/theos ~/theos   # 再用 theos 的 Linux toolchain 补 toolchain/linux/iphone
bash <(curl -fsSL https://theos.dev/install)                   # 官方脚本，Linux 上会带上那套 cctools
```

### 2.3 编

```sh
git clone -b psyche https://github.com/hanliu660-gif/hakios-xnu-kernel.git hakios
cd hakios
export THEOS=$HOME/theos
export DSYMUTIL=$THEOS/toolchain/linux/iphone/bin/dsymutil   # 不设就是 §5 第 1 条
JOBS=16 make kernel
```

`make kernel` 三行都写在 `Makefile:74-77` 里，一次跑完：

```make
kernel:
	[ -d xnu-src/xnu-8796.121.2 ] || xnu/apply.sh
	[ -e xnu-deps/host/bin/ld64 ] || xnu-deps/setup.sh
	tools/make-kernel.sh -j$(JOBS)
```

`JOBS ?= 8`（`Makefile:40`），一次干净构建建议给 8–16。第一次会把 Apple 的 XNU 拉下来、把 host 工具（`mig` `migcom` `unifdef` `tcsh` `unzbm`）和两个静态库编出来，占 1.7 G、花十几分钟到半小时；之后就是增量。

产物两个名字同一个文件：`./hakios-kernel`，XNU 里的原名 `xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`。

### 2.4 判据

- `tools/make-kernel.sh` 打印 `make-kernel: built: hakios-kernel (…)` 才算成；它按**失败的 target**判断，不按退出码（§4）。
- 大小量级 22.3 MB。本次实测 22,344,256 B。
- 想确认和机器上跑的那颗一致：`sha256sum hakios-kernel`，已验过的是 `b99cc5269d9583e2d09cc7285e3e02d2fcd12db85374ec208800b5b37d4509b3`（22,341,416 B）。差 2,840 B 的原因见 §3 末尾。

## 3 不在公开仓里的三个文件

`psyche` 分支的 tip 与本机这棵树逐文件比对（20 个 `git status` 里被改过的文件全查了一遍）：**17 个一样，3 个不一样**。

| 文件 | 这棵树 | 仓库 tip | 影响什么 |
|---|---|---|---|
| `xnuloader/loader.c` | 172,318 B | 170,064 B | 只影响 `loader.efi`（`xnu.img` 里那条 71,168 B 的 XNUP 载荷）。**不进内核** |
| `userland/mkjobs.py` | 25,070 B | 24,276 B | 只影响 `userland/*.apfs` 镜像的构建。**不进内核** |
| `wifi/lx/src/lx_bus.c` | 39,750 B | 29,778 B | 只有 `xnu-deps/build/ath11k/out/ath11k.o` 存在、且配置带 `hakios_wifi` 时，`xnu-build.sh`（第 8–13 行）才把这个目标文件链进内核 |

所以：**照 §2 编出来的内核，代码等价**；三处差异里唯一可能影响内核的是 `lx_bus.c`，而且只在你把 ath11k 目标文件编出来之后。

这三份的唯一完整副本在包里：`wsl/hakios-build-tree.tar.gz`（638 个 `git ls-files` 的工作区内容）+ `wsl/DIFF.md`（82 个差异文件的逐条清单与影响分析）。要逐字节回到当时状态，把这个 tar 铺成 `~/hakios`，再跑 §2.3 后面那三行。

顺带把两件容易混的事分开：

- 我先前那两次「编成功」是**空转**：`mach.development.bcm2837` 与 `hakios-kernel` 的 mtime 一直停在 2026-11-24 02:21，make 什么都没重做，`make-kernel.sh` 只是 `cp -p` 了同一个文件（md5 `f42652c8`）。删掉链接产物才是真重建：md5 `220a4893d5a8f9fe3c77150d3014815f`，仍是 22,344,256 B。**「连编两次同 md5」不能当作确定性的证据。**
- **11-19 实机验证过的那颗是 22,341,416 B**（`b99cc526`），比现在小 2,840 B。它是更早的树状态产的。想复现它得回到当时的树；想直接跑它，用包里那颗，不必编。

## 4 为什么退出码 2 算成功

这个机型配置上 XNU 自己的收尾必然失败：`config/*.symbolset` 的 kext 符号集与 `check_all_exports` 会报 `exported name not in import list`（DriverKit 的 `IOService::*`、Apple 芯片的 `_t8110dart_*` 这些这颗内核根本没有的符号）；Android 手机的 machine configuration 把那些部件去掉了，而这颗内核不加载任何 kext。于是 `-k` 之下链接成功、退出码仍是 2。

`tools/make-kernel.sh` 的处理是「只容忍这几步」，原文：

```sh
grep -E '^make(\[[0-9]+\])?: \*\*\* ' "$1" |
	grep -vE '.symbolset\]|check_all_exports|config_all|_recurse_into_config|[Makefile:[0-9]+: all]|^make: \*\*\* [all] Error'
```

任何编译、链接失败都会落在这条过滤之外 → 判失败。日志里那行 `Error 1 (ignored)` 是符号集步骤，正常。

## 5 失败对照表（都是这台机器上真踩过的）

| 现象 | 原因 | 处理 |
|---|---|---|
| `MakeInc.cmd: --minimize: command not found` / `MakeInc.kernel:131: Error 127` | `DSYMUTIL` 空，XNU 的 MakeInc 调不到 `dsymutil` | `export DSYMUTIL=$THEOS/toolchain/linux/iphone/bin/dsymutil` |
| 退出码 2，日志尾部一片 `exported name not in import list` | 符号集步骤，正常 | 什么都不用做（§4） |
| `make-kernel: a build step failed that is not one of the kext symbol-set checks:` 后面跟几行 `make: *** […]` | 真失败（编译/链接） | 看那几行 target |
| `make-kernel: no kernel was produced: …mach.development.bcm2837` | 链接没产物 | 看前面第一次报错 |
| `apply.sh: … already exists; not touching it` | 树已存在，脚本不覆盖 | 删掉或另指目录：`xnu/apply.sh /path/to/tree` |
| `apply.sh: failed; removing the incomplete …` | 拉取/打补丁失败，脚本已自清 | 重跑；网络问题先看 `XNU_URL` |
| `setup: no cctools in …: install theos with its Linux toolchain (or set THEOS)` | theos 没装或没带 Linux toolchain | §2.2 |
| `setup: no …/iPhoneOS16.5.sdk (the SDKs of github.com/theos/sdks go to $THEOS/sdks)` | SDK 缺 | §2.2 |
| 编到一半 OOM / 被 kill | 每个编译单元吃内存，`-j` 太大 | 降 `JOBS`（8 起步） |

## 6 只重打镜像、不编内核

`xnu.img` 与内核是两条独立的链：`mkxnuimg-psyche.py`（BootShim + UEFI FD + loader 载荷 + DTB）不碰内核文件。逐字节重打的完整命令见 `docs/replica.md` §5，其中 `--mtime` 决定 gzip 头那 4 字节，漏了就是「长度一样、sha 不同」。

`userland/root.apfs`（GPU 固件在它里面）与 `userland/system.apfs`（根文件系统）由 `userland/build.sh` 产出，与内核无关 —— 见 §7。

## 7 userland 的两个镜像：system.apfs 与 root.apfs

跑机时两个 APFS 都在，分工不同（尺寸与哈希都是 2026-11-25 在这台机器上实测的）：

| 镜像 | 尺寸 | sha256 | 怎么进内存 |
|---|---|---|---|
| `userland/system.apfs` | 2,476,728,320 B | `c99f6e2255559ef596778c90378e6d01a4d510d2ec95ce9b306f4a10213c6201` | `fastboot oem ramdisk` 常驻 = `md0`（根文件系统） |
| `userland/root.apfs` | 10,838,016 B | `2fc590ff650b9b798afb14ea2da02ef5ae6f8caa0a3c1a9f8b8d8fc914941a21` | payload 的 `ramdisk=` = `md1`（含 GPU 固件 `fw/a650_gmu.bin`） |

两条命令：

```sh
cd userland && SYSTEM=1 bash build.sh          # 两个镜像都重打（system 约 4 分钟）
cd userland && bash build.sh                   # 只在输入比 system.apfs 新时才重打
```

`SYSTEM=1` 只是强制重打 system.apfs。平时的重打条件是「不存在，或 `obj/stage0`、`sysimage/system.manifest`、`sysimage/cryptex`、`cryptex-app` 比它新」（`userland/build.sh` 第 653–655 行）。

### 7.1 输入链：IPSW → sysimage → 镜像

```
iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw   6,165,698,198 B（Apple 官方，自己下载）
  └─ sysimage/unpack.sh ─→ sysimage/rootfs/                     2.9 G
                          sysimage/system.manifest              5,897,479 B（逐条 mode/uid/gid/路径）
                          sysimage/cryptex/、cryptex-app/      OS 与 App 两个 cryptex
                          sysimage/20F66__iPhone10,3_6/        dyld_shared_cache_arm64* 分片共 2.6 G
                          sysimage/system.files                3,968,230 B
                          sysimage/rootfs.slt                  204,473,993 B
                          sysimage/var.skel、DeviceTree.d22ap.im4p（33,694 B → mkidentity.sh 出 identity.h）
```

### 7.2 system.apfs 的装配（`userland/build.sh` 第 652–758 行）

| 行 | 做什么 |
|---|---|
| 663–664 | `rm -rf sysroot`；`linkcopy sysimage/rootfs sysroot` —— 硬链接拷贝，不真复制 2.9 GB |
| 666–668 | 删掉 Darwin 的 LaunchDaemons/NanoLaunchDaemons/LaunchAngels/launchd.plist，换成 `ln -s /redwood/LaunchDaemons` —— 只加载我们那几十个 job，不是它全部的 500 个 |
| 669–672 | 建 `/redwood`、`/sbin`、`/System/Volumes/Darwin`（Darwin 没有 `/System/Volumes`，而镜像一旦建好就再也建不了目录） |
| 674–677 | `private/var/staged_system_apps/*` → `/Applications/`，然后删掉 `private/var`（那本身是每次开机新建的内存盘） |
| 678–679 | `sbin/launchd` 改名 `launchd.darwin`，`obj/stage0`（34,560 B）装成 `sbin/launchd` —— 我们自己的 init |
| 683 | `mkhighrate.py sysroot`：给每个 app 的 Info.plist 加 `CADisableMinimumFrameDurationOnPhone`（否则 CoreAnimation 把它们锁在 60 fps，面板是 120） |
| 686–710 | 可选固件：`fw-audio/adsp.*`（20 MB）、`aw882xx_acf.bin`（扬声器功放配置）、`fw-modem/*`（仅 `MODEM=1`） |
| 714–730 | 可选 `/jb`：Procursus + Sileo 前缀（`tools/mk-jb.sh` 拉来的），并把 `mv/cp/rm/mkdir/chmod` 软链到 `/bin` |
| 732–738 | `HakiosAudio.driver`、`big` → `usr/local/redwood`（>4 MB 的二进制住这里）、`HakiosMetal.bundle` |
| 741–745 | `cryptex/` → `System/Cryptexes/OS`，`cryptex-app/` → `System/Cryptexes/App` |
| 746–752 | dyld 共享缓存的分片硬链进 `System/Cryptexes/OS/System/Library/Caches/com.apple.dyld`（`.symbols` 跳过） |
| 755 | `python3 ../tools/apfs/mkapfs.py -z -o system.apfs System=sysroot,manifest=$DARWIN/system.manifest` |
| 756 | `CHECK=1` 时再用 `check-image.sh` 逐条对账 |

### 7.3 为什么 5.4 GB 的根能变成 2.4 GB

`mkapfs.py -z`（第 906 行）不是把整份文件压成一个流：小文件走 APFS 的 decmpfs 属性整份内联（type 3，不占块），大文件在 resource fork 里切成 64 KB 的 zlib 片（type 4）。`build.sh` 第 753–754 行的原话：

> zlib where it saves room, as Apple keeps the root (a small file whole in its attribute): **5.4 GB of files in 2.5 GB**, what fits into the phone's RAM beside Darwin

### 7.4 root.apfs 为什么只有 10 MB

第 559–568 行把 `lib/`、`bin/` 里 **大于 4 MB** 的文件搬进 `big/`，原地留一个指向 `/usr/local/redwood/…` 的符号链 —— 小镜像要和内核一起塞进 loader 给的 64 MB（带上它们就从 6.6 MB 长到 26 MB）。所以 Turnip / Mesa 那些十几 MB 的库在 **system.apfs** 的 `/usr/local/redwood` 里，root.apfs 里只有链接。

第 569 行：`[ -d big ] && SYSTEM=1` —— 只要有这种搬迁，就强制重打系统镜像（否则链接指向不存在的东西）。

最后第 572 行：`mkapfs.py -z --plain fw -o root.apfs redwood=root`，`--plain fw` 表示 `fw/` 不压缩（内核驱动直接按内存布局读，见 `drivers/storage/hakios.image.c`）。

### 7.5 两条要注意

1. **`build.sh` 就地覆盖 `userland/system.apfs`，没有备份。** 想验证「重打出来的是不是这一份」必须先 `cp -p system.apfs system.apfs.ref-<日期>`，且 WSL 里要有 5 G 以上空余（现在只剩约 14 G，`sysroot` 是硬链所以不额外吃盘，但最好别在满盘上跑）。
2. `mkapfs.py` 的 `--time` 默认 `1683900000`（2023-05-12），**所有时间戳都被写死**（第 908 行），所以同一输入理论上能逐字节一致；但本机**没有真跑过这次重打**，不能据此断言已复现。

## 8 我这台机器**打算**删什么、怎么拿回来

⚠ 这一节一度写成「已删」，实际**没有执行**：用户要求先别删，`/root/hakios`（17 G）与 `/root/hakios-build`（5.9 G）都完好无损。真要清盘的处置如下。

动的是**可再生部分**（前提：§2 的两条命令能重跑出同样的东西）：

- `/root/hakios`（17 G）整棵，其中 638 个受控文件已在包 `wsl/hakios-build-tree.tar.gz` 里，`xnu-src`/`xnu-deps` 用 §2.3 两行长回来。**注意 `userland/system.apfs`（2.4 G）与 `root.apfs` 就在这棵树里**，删之前先确认包内那两份（§7 的哈希）在，否则就从这里再拷一份。
- `/root/hakios-build`（5.9 G）：一份 IPSW 6.17 GB + 指向已删 `ROM` 的悬空软链，与内核无关。
- 压缩 `C:\WSL\hakios\ext4.vhdx`（逻辑 221,711,958,016 B）：`wsl --shutdown` 后 `Optimize-VHD` 或 `diskpart compact vdisk`。

没动的是 `~/theos`（923 M，§2.2 的 cctools 与 SDK）和 `/root/kbuild`、`/root/v28..v34_*.img`、`/root/sys_v31,v32`（Flyme 的内核构建与镜像，与本项目无关）。

再拿回整棵树的顺序：

```sh
mkdir -p ~/hakios && tar -xzf hakios-build-tree.tar.gz -C ~/hakios   # 638 个文件（包内 wsl/）
cd ~/hakios && export THEOS=$HOME/theos DSYMUTIL=$THEOS/toolchain/linux/iphone/bin/dsymutil
make kernel JOBS=16                                                  # apply.sh + setup.sh + build
```

## 9 一句话

**内核 = 公开分支 + theos + 两条生成命令；两个 userland 镜像 = `userland/build.sh` + IPSW 解出的 `sysimage/`（§7）；17 G 里 15 G 与这两样都无关。想要逐字节一致，拿包里的树快照与两个 apfs；想验证方法，就照 §2、§7 从零跑一遍。**
