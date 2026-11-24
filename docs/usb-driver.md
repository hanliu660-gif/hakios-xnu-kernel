# Windows 侧的 USB：loader 是怎么被认出来的（实测，附反证）

> 用途：把 Windows 侧到底需要什么讲清楚。
>
> **先给结论，分两种情况，别混为一谈：**
> 1. **没有装过 `android_winusb.inf` 的 Windows —— 一定需要装驱动。** 不装就是"未知设备"，
>    `fastboot devices` 永远空。包里已经带了可以直接装的整包（`drivers/windows/android_winusb.zip`）。
> 2. **装过之后**，`VID_18D1&PID_D00D` 会被按 **PID 点名**认成 *Android Bootloader Interface*，
>    这时才不需要任何额外动作 —— 不需要自签 `.cat`、不需要 `inf2cat`、不需要 `testsigning`。
>
> 早先本文只写了第 2 种情况，被理解成"任何机器都不用装驱动"，那是不对的，已按实测改正。

## 1. 事实：loader 绑在哪个 INF

```
Unknown   Android Bootloader Interface    oem67.inf   USB\VID_18D1&PID_D00D\23170C8B
Unknown   Android Bootloader Interface    oem67.inf   USB\VID_18D1&PID_D00D\XNULOADER
Unknown   Android ADB Interface           oem67.inf   USB\VID_18D1&PID_4E11\23170C8B
Unknown   Android ADB Interface           oem67.inf   USB\VID_18D1&PID_4EE7\23170C8B
Unknown   Android Composite ADB Interface oem67.inf   USB\VID_18D1&PID_D001\23170C8B
```

复现命令（PowerShell）：

```powershell
foreach ($d in (Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_18D1' })) {
  $inf  = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath').Data
  $desc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_DriverDesc').Data
  "{0,-9} {1,-30} {2,-14} {3}" -f $d.Status, $d.FriendlyName, $inf, $d.InstanceId
}
```

`Status = Unknown` 只是"设备当前不在场"（手机没插/没进 loader），不是 Code 28。
真出 Code 28 时 Status 会是 `Error` 且 `Problem` 为 28。

## 2. oem67.inf 是什么

```
Original Name:  android_winusb.inf
Provider Name:  OPLUS, Inc.          ← OPPO 的 android_winusb 分支，DriverVer 01/04/2023,11.0.0.0
Class Name:     AndroidUsbDeviceClass
Published Name: oem67.inf
CatalogFile.NTamd64 = androidwinusba64.cat        ← 有签名
```

它里面**按 PID 直接点名**了 loader 的 ID：

```ini
%SingleBootLoaderInterface% = USB_Install, USB\VID_18D1&PID_D00D
```

所以匹配走的是 `VID_18D1&PID_D00D`，跟接口描述符里的 `Class_FF/SubClass_42/Prot_03` 一点关系都没有。

> 顺带：Google 自家的 `android_winusb.inf`（本机 `oem96.inf`）里也有 Prot_03 的安装行
> `%SingleAdbInterface% = USB_Install, USB\Class_FF&SubClass_42&Prot_03`。
> 也就是说无论走 PID 点名还是走接口描述符，Prot_03 都有已签名驱动覆盖。

## 3. 被推翻的四条

| 报告主张 | 本机反证 |
|---|---|
| XNULOADER 是 CM_PROB_FAILED_INSTALL / Code 28 | 绑定记录 `oem67.inf` + `Android Bootloader Interface`；Status=Unknown 表示不在场，不是失败 |
| `winusb.inf` 只认 Prot_01，所以 loader 认不上 | `winusb.inf` 确实只有 `Class_ff&SubClass_42&Prot_01`（ADB），但 loader **不是**靠 winusb.inf 认的，靠的是 oem67.inf 的 PID 点名 |
| 必须自签 `hakios_fastboot.cat` / 装 WDK `inf2cat` / 开 `testsigning` | 本机从未需要；11-24 全程 `fastboot boot / stage / oem run / get_staged` 成功 |
| `oem24.inf` 是 android_winusb，D00D 被它覆盖 | `oem24.inf` = Realtek 无线网卡（`netrtwlanu6.cat`），与 Android 无关 |

## 4. 站得住的那两条

1. **`Prot_03` 是 loader 自己报的**，不是 UEFI FD：`xnuloader/loader.c` 第 3625 / 3631 行
   两处 `9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4, /* fastboot interface */`。
   真想要 Prot_01，改这里重编即可——但没有必要（见 §2）。
2. **`usb_connected` 的门槛真实存在**：`loader.c:3619` `static int usb_connected;`（默认 0），
   由 `UsbDeviceEventDeviceStateChange` 置位；`usb_command_loop()` 里
   `if (!usb_connected) { usb_wait(0xff); continue; }`。
   但实机从 Windows 冷进 loader 时事件会到（否则 11-24 的 `stage/get_staged` 全都不可能完成），
   **不要**照着"默认 0 所以卡死"的推断去改默认值。

## 5. 真出问题时的排查顺序

1. `fastboot devices` 空、但设备管理器里能看到 `Android Bootloader Interface` → `adb kill-server`（adb 占 5037）。
2. 设备管理器里是黄色感叹号（Status=Error, Problem=28）→ 换线/换口；再不行手动"从磁盘安装"
   指到 `C:\Windows\INF\oem67.inf`（或任意 `android_winusb.inf`）。
3. `fastboot getvar product` 回 `product: xnuloader` 就说明已经在 loader 里了——此时
   Windows 侧的 USB 这一层已经通了，后面任何失败都不是驱动问题。


## 6. 另一台 Windows 从零装（如果你的机器上没有 oem67.inf）

**先确认真的需要装**：手机进 bootloader 后跑 `tools/win-usb-check.ps1`，
只要 [1] 里出现任意一行 `Status=OK`，就说明已经在工作，不要再动驱动。

确需安装时，去拿**带签名的** `android_winusb.inf`（三选一，都是现成的、已签名的）：

| 来源 | 怎么拿 |
|---|---|
| Google 官方 | SDK Manager 勾 `extras;google;usb_driver`，得到 `android_winusb.inf` + `android_winusb.cat` |
| 任意 OPPO / 一加 / realme 的 Windows 驱动包 | 解压后里面同样有 `android_winusb.inf` + `androidwinusba64.cat`（本机 `oem67.inf` 就是这一类） |
| 本机已有 → 拷出来 | `C:\Windows\INF\oem67.inf` 改名回 `android_winusb.inf`，配套 `.cat` 一起带 |

装（管理员 PowerShell，**.inf 与 .cat 必须同目录**）：

```powershell
pnputil /add-driver .\android_winusb.inf /install
pnputil /enum-drivers | Select-String -Pattern 'android_winusb|18D1'
```

还认不上，就在设备管理器里对那个黄色感叹号设备 → 更新驱动程序 → 浏览 → **让我从计算机上可用的驱动列表中选取**
→ 从磁盘安装 → 指到那份 `android_winusb.inf` → 选 *Android Bootloader Interface*。

**不需要**做这些：`bcdedit /set testsigning on`、自己生成 `.cat`、装 WDK/`inf2cat`、改 loader 的描述符。
（loader 报 `Prot_03` 也无所谓：`android_winusb.inf` 里既有 PID 点名 `VID_18D1&PID_D00D`，
Google 版里也有 `USB\Class_FF&SubClass_42&Prot_03` 这条安装行。）

## 7. 实在装不上：两条绕开 Windows 的路

1. **usbipd-win + WSL**（把 USB 直通给 Linux，用 Linux 的 fastboot，驱动这层彻底不存在）：

   ```powershell
   winget install --exact dorssel.usbipd-win
   usbipd list                      # 找到 18d1:d00d 那行的 BUSID
   usbipd bind --busid <BUSID>
   usbipd attach --wsl --busid <BUSID>
   ```
   ```bash
   sudo apt install -y android-tools-adb android-tools-fastboot
   fastboot devices                  # 在 WSL 里应能看到
   ```
   注意：`usbipd attach` 之后 Windows 侧就看不到这个设备了，跑完 `usbipd detach` 还回来。

2. **换一台 Linux 机器**（树莓派都行）：`apt install android-tools-fastboot`，
   本包的 `xl` 就是按 Linux 写的（`bash xl enter` / `ramdisk` / `run`），这条路最少折腾。

## 8. 一句话

驱动这一层在本项目里**只影响"Windows 能不能把字节送进去"**，
进去了之后的一切（`oem ramdisk`、`stage`、`oem run`、`get_staged`）都与驱动无关。
所以遇到"跑不起来"，先跑 `tools/win-usb-check.ps1` 定位到层，不要默认是驱动。

