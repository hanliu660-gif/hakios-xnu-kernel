# psyche 的依赖链：哪些是我们写的，哪些是别人的

> 用途：一份**可核对的来源表**。上面每一行都指到本仓库里引入它的脚本或目录，
> 或者说明它为什么不在仓库里、该由谁从哪取。
> 与 `CHANGELOG.md` 的 "Sources" 一节一致；那份是全项目的，这份专说 psyche 这条路。

---

## 一、一张图

```
Apple 开源 XNU (xnu-8796.121.2)
  ├─ xnu/hakios.patch          我们改的（ARM64 / 嵌入式 / 驱动接入）
  ├─ xnu/files/                我们新加的文件
  └─ drivers/                  我们的驱动（GPU / 触摸 / 电池 / PCIe / WiFi …）
        ↓ 编出
  prebuilt/hakios-kernel       22,341,416 B  md5 0b7957f0   ← 实机验证过的那份
        ↓ 由 loader 装进内存
BootShim (208 B 汇编)  +  Mu-Silicium UEFI FD  +  psyche DTB  +  usr-payload
        ↓ 打成 boot.img，fastboot boot
ABL (手机自带的 bootloader)
        ↓
UEFI 固件 (Mu-Silicium 的 psyche 变体)
        ↓
xnuloader (我们写的 loader，跑在 UEFI 里)
        ↓ 建立 Apple 设备树，跳 XNU
XNU 内核
        ↓ 挂两块盘
md0 = system.apfs  ← 你自己的 IPSW 生成的 Darwin 16.5 用户态（2.4 GB，仓库不含）
md1 = root.apfs    ← 我们写的 init / etc/rc / launchd plist（10.8 MB）
        ↓
launchd → SpringBoard → 桌面
```

---

## 二、我们写的

| 组件 | 位置 | 说明 |
| --- | --- | --- |
| 内核补丁 | `xnu/hakios.patch`、`xnu/files/` | 施加在 Apple 的 `xnu-8796.121.2` 上 |
| 驱动 | `drivers/`（按子系统） | Adreno GPU、ST FTS V521 触摸、PMIC 电池、PCIe/QCA6390、thermal … |
| loader | `xnuloader/loader.c`、`build.sh` | 跑在 UEFI 里，读 payload、建设备树、跳内核 |
| BootShim | `devices/psyche/prebuilt/BootShim-src/` | 208 字节汇编，把 Mach-O 约定翻译成 UEFI 入口 |
| 打包器 | `mkxnuimg-psyche.py`、`xnuloader/mkpayload.py` | payload → boot.img |
| 用户态 | `userland/` | `init`（stage0）、`etc/rc`、launchd 任务、注入库 |
| 工具 | `tools/`、`xl` | 构建、跑机、APFS 打包、驱动检查 |

编出来的内核：`prebuilt/hakios-kernel`，`md5 0b7957f0b463e9dd30a5db808c97a477`，
内嵌 19 个 `hakios.*` 驱动（含 `psyche_touch`、`adreno`、`pcie`、`amss`、`wlanfw`）。

---

## 三、别人的（按项目源引入）

| 组件 | 来源 | 版本 | 仓库里怎么引入 | 许可证 |
| --- | --- | --- | --- | --- |
| **XNU 内核** | [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu) | tag `xnu-8796.121.2` | `xnu/apply.sh` | APSL 2.0 |
| **HFS Plus** | [apple-oss-distributions/hfs](https://github.com/apple-oss-distributions/hfs) | tag `hfs-627.100.6` | `xnu/hfs/hfs.sh` | APSL 2.0 |
| **UEFI 固件** | [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) | `e51cd877e31c` 的 psyche(SM8250) 变体 | **源码树已不在**，只有编译产物 `devices/psyche/prebuilt/SILICIUM_UEFI*.fd` | BSD-2-Clause-Patent |
| **U-Boot**（另一条启动路） | [u-boot](https://source.denx.de/u-boot/u-boot) | `v2026.07` | `hakios-uefi/u-boot/apply.sh` | GPL-2.0+ |
| **OpenGL ES** | [Mesa](https://archive.mesa3d.org/) | 25.0.7（freedreno） | `tools/mk-mesa.sh` | MIT |
| **Vulkan** | 同一份 Mesa 的 Turnip | 25.0.7 | `tools/mk-turnip.sh` | MIT |
| **Metal on Vulkan** | [UniXNU/AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) + [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan) | `243d63c` / `624bab3` | `tools/mk-m2v.sh` | MIT / Apache-2.0 |
| **mkbootimg.py** | Android Open Source Project | — | 随仓库携带 | Apache-2.0 |
| **libdeflate** | Eric Biggers | — | `xnu/files/bsd/miscfs/apfs/libdeflate/` | MIT |
| **ath11k**（WiFi） | Linux | — | `tools/mk-ath11k.sh` 现取 | BSD-3-Clause-Clear |
| **FreeBSD LinuxKPI 头** | FreeBSD | — | `tools/mk-ath11k.sh` | BSD-2-Clause |
| **构建工具** | libdispatch / libpthread / AvailabilityVersions / bootstrap_cmds / unifdef / macOS 13.3 SDK / iPhoneOS 16.5 SDK | — | `xnu-deps/setup.sh` | 各自 |
| **fastboot 驱动（Windows）** | Google 的 `android_winusb.inf` | — | `tools/windows-usb-driver/` | 见该目录 `HOWTO.md` |

---

## 四、不在仓库里、必须你自己准备的

这两样是 `LEGAL.md` 明确排除的（`No part of Darwin`、`No device firmware`），
不是打包遗漏：

| 东西 | 为什么不在 | 从哪来 |
| --- | --- | --- |
| **Darwin 16.5 用户态**（`system.apfs`，2.4 GB） | Apple 的许可证把它绑在 Apple 硬件上；仓库不分发 Apple 的任何二进制 | 你自己的 IPSW：`https://ipsw.me/download/iPhone10,3/20F66`（6.17 GB）→ `sysimage/unpack.sh` → `cd userland && SYSTEM=1 bash build.sh` |
| **手机固件**（`fw-gpu/`、`fw-wifi/` 等） | 厂商的（Qualcomm / Xiaomi） | 从你自己的手机拉：`make firmware` |

**另一样：构建这些镜像的那棵工作树**。本仓库的 `psyche` 分支是它的较早版本；
要逐字节重建 11-24 那份产物，需要那份树（若日后归档，会放在 `wsl/` 下并附差异清单）。

---

## 五、怎么自己核对

```sh
# 内核是不是跑通那份
md5sum prebuilt/hakios-kernel          # 期望 0b7957f0b463e9dd30a5db808c97a477
strings -a prebuilt/hakios-kernel | grep -c 'hakios\.'   # 期望 19

# 触摸驱动在不在（老内核缺这个：0；这颗 404）
strings -a prebuilt/hakios-kernel | grep -c psyche_touch  # 期望 404

# UEFI 素材
md5sum devices/psyche/prebuilt/SILICIUM_UEFI.fd         # 2acd6396（thyme）
md5sum devices/psyche/prebuilt/SILICIUM_UEFI_psyche.fd  # 513fa373
md5sum devices/psyche/prebuilt/device-tree.dtb          # 9daef336

# payload 能不能逐字节重建
python3 xnuloader/mkpayload.py -o /tmp/x.bin \
  kernel=prebuilt/hakios-kernel ramdisk=userland/root.apfs \
  --cmdline "$(cat payload-cmdline.txt)" --kernel-version "$(cat kernel.ver)"
cmp /tmp/x.bin run-payload.bin && echo identical
```

全部对得上，说明这套东西是自洽的；对不上，`docs/boot-to-desktop.md` 第 8 节有故障对照。
