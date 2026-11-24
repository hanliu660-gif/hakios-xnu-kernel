# 源码与工具链接（可下载的都在这）

仓库里**已经带了**：内核（`prebuilt/hakios-kernel`）、loader 源码与 `xnu.img` 的打包器、
`root.apfs`、`run-payload.bin` 与它的 cmdline、psyche 的 UEFI FD 与 DTB。
下面这些是仓库内**没有**（体积大或需授权）但重建/改代码时要用的，按需自取。

## 一、本项目源码

| 东西 | 链接 | 说明 |
| --- | --- | --- |
| 内核 + 打包器（本设备分支） | https://github.com/hanliu660-gif/hakios-xnu-kernel （分支 `psyche`） | 就是本仓库 |
| 上游内核仓 | https://github.com/oosexgang/hakios-xnu-kernel | 只读上游；本设备分支是从它分出来的 |
| 上游用户态 | https://github.com/oosexgang/hackios-xnu-userland | `root.apfs` 里的 init/launchd 任务来源 |
| UEFI 壳 | https://github.com/oosexgang/hakios-uefi | FD 的封装与 BootShim |
| UEFI 固件上游 | https://github.com/Project-Silicium/Mu-Silicium | 我们用的 FD 是它的 psyche(SM8250) 变体编译产物，**该变体的源树已不在**，故 `devices/psyche/prebuilt/` 直接收了 FD 与 shim |

## 二、同族移植参考（读别人的做法最快）

| 项目 | 链接 | 参考点 |
| --- | --- | --- |
| apollo（小米 10T / SM8250 系） | https://github.com/enesuzun2002/hakios-apollo | 电量/显示/adreno/GPU 驱动的写法（`devices/mi-10t/xnu/files/`） |
| ios-apollo | https://github.com/aTechGeekDev/ios-apollo | xnu 补丁与 wifi 目录结构 |
| ios-sweet-umi | https://github.com/leafall903/ios-sweet-umi | GPU 单元的组织方式 |
| hackios-xnu-kernel-umi | https://github.com/JopangJovan/hakios-xnu-kernel-umi | 另一条 UMI 线 |
| hakios-sm7325 / hakios-mt6768 | https://github.com/oosexgang/hakios-sm7325 、https://github.com/oosexgang/hakios-mt6768 | 别的 SoC 的移植分层 |
| 文档站 | https://hackios-web.vercel.app 、https://atechgeekdev.github.io/ios-apollo/ | 上手说明 |

## 三、必须自己下的大件

### 3.1 iOS 16.5 IPSW（iPhone X）—— 唯一的大件，6.17 GB

**这是 `userland/system.apfs`（2.4 GB 的 Darwin 用户态）的唯一来源。**
仓库不含 Darwin 的任何部分（`LEGAL.md`），所以这一份必须你自己下：

| 直达方式 | 地址 |
| --- | --- |
| ipsw.me 的设备页 | https://ipsw.me/iPhone10,3 |
| ipsw.me 的下载页（选 16.5 / 20F66） | https://ipsw.me/download/iPhone10,3/20F66 |
| Apple 的固件目录（ipsw.me 就是从这取的） | https://mesu.apple.com/assets/com_apple_MobileAsset_SoftwareUpdate/com_apple_MobileAsset_SoftwareUpdate.xml |

文件名 `iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`，**6,165,698,198 B**。
校验：`sha256` 前 8 位 `c1091c40`，后 5 位 `d52f2`。

只取其中几样（`sysimage/unpack.sh` 会自动挑）：`dyld_shared_cache_arm64*`、
`Firmware/all_flash/DeviceTree.d22ap.im4p`、Cryptex 系统卷与 App 卷。

```sh
sysimage/unpack.sh iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw   # 约 8 GB 中转
cd userland && SYSTEM=1 bash build.sh                              # -> system.apfs 2.4 GB
```

> 为什么不用别的版本：`xnuloader` 生成的设备树、`system.apfs` 里的 dyld 共享缓存、
> Cryptex 的路径都对 16.5 / 20F66 这一版写的。换版本要一并改 `xnuloader/identity.h` 与
> `userland/build.sh` 里的 `$DARWIN` 路径。

### 3.2 工具链

| 东西 | 链接 | 备注 |
| --- | --- | --- |
| theos | https://github.com/theos/theos | iOS 工具链宿主（`xnu-deps/setup.sh` 默认 clone 到 `/root/theos`，约 923 MB） |
| theos 的 Linux 工具链 | `bash ~/theos/bin/install-theos` | clone 完还要跑它，才装 `toolchain/linux/iphone` 的 cctools |
| iPhoneOS SDK | https://github.com/theos/sdks | 用 sparse-checkout 只取 `iPhoneOS16.5.sdk` |
| platform-tools（fastboot/adb） | https://developer.android.com/tools/releases/platform-tools | Windows/Linux/macOS 都有 |
| **Windows 的 fastboot 驱动** | 本仓库 `tools/windows-usb-driver/` | **Windows 上必须先装这个**，否则 `fastboot` 看不见设备（见 `docs/usb-driver.md`） |
| Python 3 | https://www.python.org/downloads/ | 打 payload、解析 IPSW 用 |
| Ubuntu（WSL 里编译） | https://cloud-images.ubuntu.com/wsl/noble/current/ | 或 Microsoft Store 装 Ubuntu；编译需要 clang-18 / llvm-objcopy-18 |

## 四、查资料用

| 东西 | 链接 |
| --- | --- |
| Apple 开源 XNU（8796.121.2 即本代） | https://github.com/apple-oss-distributions/xnu |
| Mesa（`hakios-gl` 报的 OpenGL ES 3.2 Mesa 25.0.7） | https://gitlab.freedesktop.org/mesa/mesa |
| 高通 kona 平台内核参考（SM8250） | https://github.com/LineageOS/android_kernel_xiaomi_sm8250 |

> 链接都是各项目的官方地址；若某天失效，按项目名搜同名仓库即可（这些名字都很独特）。