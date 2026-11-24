# Windows：把已经在跑的驱动装到另一台机器

> 目录：`drivers/windows/`
> - `android_winusb.zip`（2.5 MB，整包，直接发过去）
> - `android_winusb/`（解压后的原件，`.inf` 与 `.cat` 必须同目录）

## 这份是什么

从本机 `C:\Windows\System32\DriverStore\FileRepository\android_winusb.inf_amd64_8068221a8ddf326a`
原样拷出来的驱动包（本机就是用它把 `USB\VID_18D1&PID_D00D` 认成 *Android Bootloader Interface* 的）：

| 文件 | 说明 |
|---|---|
| `android_winusb.inf` | Provider = **OPLUS, Inc.**，DriverVer = `01/04/2023,11.0.0.0`，46,886 B，sha256 `1a8f6e5d…6f30b9` |
| `androidwinusba64.cat` | 目录签名 `CN=Microsoft Windows Hardware Compatibility Publisher`，签名状态 **Valid** |
| `amd64/WinUSBCoInstaller2.dll` | WinUSB 协安装器（`AddService = WinUSB` 需要） |
| `amd64/WdfCoInstaller01009.dll` | WDF 协安装器 |

**它是要装的** —— 目标机器没装过 `android_winusb.inf` 的话，loader 就是未知设备（Code 28），`fastboot devices` 永远空。
它只是**已经签好名**，所以装的时候不需要 `testsigning`、不需要 `inf2cat`、不需要自己生成 `.cat`。
（本机之所以看起来"不用装"，是因为这份驱动早在装 OPPO/一加的 USB 驱动时就落进 DriverStore 并绑好了。）

INF 里点名的设备 ID（这是关键，跟接口描述符无关）：

```ini
%SingleBootLoaderInterface% = USB_Install, USB\VID_18D1&PID_D00D
```

## 在那台机器上怎么装（两条命令）

把 `android_winusb.zip` 拷过去解压，然后**管理员 PowerShell**：

```powershell
cd <解压出来的 android_winusb 目录>          # 里面有 android_winusb.inf 和 androidwinusba64.cat
pnputil /add-driver .\android_winusb.inf /install
```

看结果：

```powershell
pnputil /enum-drivers | Select-String -Pattern 'android_winusb' -Context 0,3
# 应出现一行 Provider = OPLUS, Inc.、Driver Version = 01/04/2023 11.0.0.0
```

设备管理器里若设备还带黄色感叹号：右键 → 更新驱动程序 → 浏览 → **让我从计算机上可用的驱动列表中选取**
→ 从磁盘安装 → 选中 `android_winusb.inf` → 选 **Android Bootloader Interface**。

## 装完怎么验（三步，一步比一步硬）

1. `tools/win-usb-check.ps1` —— 手机插好、进 bootloader 后跑一次；[1] 里应出现
   `Status=OK  Inf=oem??.inf  FriendlyName=Android Bootloader Interface`。
2. `fastboot devices` —— 应列出 `<序列号>  fastboot`。空的话先 `adb kill-server`。
3. `fastboot getvar product` —— 回了 `product: xnuloader` 就说明驱动这条线彻底通了，
   后面 `oem ramdisk` / `stage` / `oem run` / `get_staged` 的失败都与驱动无关。

## 如果那台机器死活装不上

- 换线、换 USB 口（避开前置口和 hub）；很多"驱动问题"是线只供电。
- 用 `usbipd-win + usbip-win2` 把手机 USB 搬到另一台机器（见 `docs/usb-driver.md` §7）。
- 或者干脆把手机插到已经正常工作的机器上跑。
