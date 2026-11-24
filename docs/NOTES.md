# 移植笔记（脱敏）

> 这份笔记只讲**为什么这么做**和**怎么验证**。每一步的实测数字都能在 `docs/STATUS.md` 与 `proof/` 的日志里
> 找到对应行号。人名、设备序列号、账号信息都已去掉；路径一律用包内相对路径。

---

## 0. 一句话

把 Apple 开源的 **XNU（Darwin 22.5 / xnu-8796.121.2，即 iOS 16.5 世代）** 跑在
**小米 12X（psyche / SM8250-AC kona）** 上，用 iOS 16.5 的真机用户态（从 iPhone X 的 IPSW 里取）
作为根文件系统，显卡/触摸/电量/显示由我们写的驱动接管——**不刷任何分区，全部在内存里跑**。

---

## 1. 启动链为什么长这样

```
fastboot boot xnu.img
  └─ Android bootloader 把 xnu.img 的 kernel 段（gzip 流）塞进内存并跳进去
      └─ BootShim（208 B 汇编）：把 XNU 的 Mach-O 约定翻译成 UEFI 能接的入口
          └─ UEFI 固件（Mu-Silicium 的 psyche 变体，3 MB FD）
              └─ loader.efi（我们自己写，跑在 UEFI 里）
                  ├─ 收 payload：Mach-O 内核 + ramdisk + 命令行
                  ├─ 建 Apple 设备树（按命令行里的 cpus/touchscreen/gpu…）
                  └─ 跳 XNU
                      └─ XNU 起 md0（常驻 2.3 GB iOS 用户态）+ md1（payload 里的 root.apfs）
                          └─ launchd → SpringBoard → 桌面
```

三段各自存在的理由：

1. **为什么要有 UEFI 壳**：Android 的 bootloader 只会 `boot kernel+ramdisk`，不会跑 Mach-O。
   用一个已开源的 UEFI 固件当"内核"最省事——它自带 USB/内存/定时器，我们只要在上面写一个 loader。
   自己从零造一个能跑在 kona 上的裸机环境，工作量是数量级的差别。
2. **为什么要有 BootShim**：固件期望的是 ELF/PE，XNU 是 Mach-O。208 字节的汇编跳板把
   `FD_BASE=0x9FC00000` 处的镜像搬到 XNU 期望的位置再跳。**这个常量是每台机器不同的**
   （同族 redwood 是 `0x9FD00000`），搞错就是直接黑屏无输出。
3. **为什么大根镜像进内存而不是刷分区**：2.3 GB 的 iOS 用户态没有合适的分区可写，
   而 loader 可以在 `oem ramdisk` 时预留一大块 RAM 当 `md0`。代价是每次冷启动要传 2.3 GB，
   好处是**一次上传、多次 run**，且完全不动手机上的任何分区。

---

## 2. 五个关键决策（以及不这么做会怎样）

### D1 大镜像走 loader 常驻，payload 只带小盘
payload 若把 `system.apfs`（2.3 GB）一起塞进去，`fastboot stage` 直接回 `bad size`。
**正确结构**：`system.apfs` → `oem ramdisk` 常驻 = `md0`；`root.apfs`（10 MB）→ payload 的 ramdisk = `md1`。

### D2 payload 必须带 root.apfs，不能空
空了之后内核会把常驻的 `md0` 当根——那是 iOS 的根，**里面没有我们的 GPU 固件**。
adreno 的第一句话就是 `THE GPU IS NOT STARTED: the small image has no fw/a650_gmu.bin`，
整个系统退回软件渲染（桌面还能出来，所以极容易误判"GPU 没问题"）。
判据：日志里必须出现 `HakiosGPU: … submissions …`。

### D3 一次 loader 会话只跑一个内核
loader 的内存窗口/页表状态只能用一次。想再跑必须先 `fastboot reboot` 回 Android 重新进。
（否则现象是：第二次 run 立刻死在早期，看起来像"内核坏了"。）

### D4 驱动只做 SpringBoard 真正走的那几条路径
不是把 Linux 驱动整个搬过来，而是"够桌面用"的最小闭集：

| 子系统 | 我们的实现 | 验收数字（`proof/042229-run.log`） |
| --- | --- | --- |
| GPU | Adreno 642L，自管 submit/ring | `1667 submissions in 5008 ms, 573 us each, 6430 command buffers` |
| 显示 | 把 DRM/MSM 的 framebuffer 包成 iOS 的 IOFB | `293 frames in 5018 ms (58 a second), 289 swaps waited, 0 gave up` |
| 触摸 | ST FTS V521 走 SPI，转成 IOHIDEvent | `touches=166`、`last=(1079,2074)` |
| 电量 | 读 loader 留下的 PMIC 快照 | `SOC=71%, V=4045 mV, I=472 mA, T=30 C, battery=1 charger=1` |
| 控制台 | Mesa 软件/硬件 GL | `OpenGL ES 3.2 Mesa 25.0.7; FD650` |

诚实标注：**电量那条是从 loader 的缓存快照读的，不是内核经 PMIC/SPMI 实时读表**。
整份日志里 SPMI 只在按键驱动初始化时出现过一次。要做真电量计得把 SPMI 传输补上。

### D5 结论必须能指到日志行号
这条是踩了坑之后立的规矩：中途出现过一份"结论文档"断言 GPU 死了、触摸再也报不了点，
而同一天同一个 md5 的内核在实机上跑出 1667 次 submission、166 次报点。
**当文档和镜像/日志冲突时，以运行时为准**。包里的 `docs/STATUS.md` 就是按这条写的：
每条结论后面跟 `logs/xxx-run.log:行号`。

---

## 3. 踩过的坑（现象 → 原因 → 正确做法）

| 现象 | 原因 | 正确做法 |
| --- | --- | --- |
| `fastboot boot` 报 `AdbWriteEndpointSync failed (121)` | adb 服务占着 5037 | 先 `adb kill-server`，立刻正常 |
| `stage` 报 `bad size` | payload 里塞了 2.3 GB | 大镜像只走 `oem ramdisk` |
| 桌面出来但动画卡成幻灯片 | payload 没带 root.apfs，GPU 固件找不到 | payload 带 `ramdisk=userland/root.apfs` |
| 第二次 run 立刻死 | 同一 loader 会话 | 回 Android 重进 |
| `./xnuloader/mkpayload.py` 报 `line 10: Build: command not found` | Windows/MSYS 下按 shebang 当 shell 脚本执行 | 显式 `python3 xnuloader/mkpayload.py …` |
| 设备树长度从 4438 变 3874、`soc_name=unknown` | 以为是"工作区被动过" | 其实设备树是 loader **按命令行现造**的（`xnuloader/loader.c` 开头注释写明），长度随参数变 |
| `git` 里加了 `!/path/file` 白名单却不生效 | 父目录先被 `/devices/*` 排除，子项白名单无效 | 先 `!/devices/` + `!/devices/redwood/`，再放子项白名单 |
| 解析 `xnu.img` 时 `gzip.decompress` 报错 | kernel 段 gzip 流后面**紧贴着 842,746 B 的 DTB** | 用 `zlib.decompressobj(31)` 只解流，DTB 按 `page_size+kernel_size-842746` 切片 |
| `fastboot stage` 传 2.3 GB 中途超时 | 默认超时 10 秒 | `FB_TIMEOUT=1800` |
| 重打 `xnu.img` 大小对得上、字节对不上（差 24 字节） | `gzip.compress` 默认把当前时间写进 gzip 头（偏移 2052 的 4 字节 mtime）→ ANDROID 头里的 id（SHA1）跟着变 | 传 `--mtime 1793694715`（11-03 16:31:55，原镜像的值）；不传则用 0，可复现但不等同这一份 |
| 重打 `usb-payload.bin` 短 8 字节 | 原镜像那份末尾多了 8 个零字节（XNUP 终止填充） | 功能无差别，XNUP 读到空名条目就停；要字节级一致就用包内那份 |
| 没有 clang 就想打 `xnu.img` | `mkxnuimg-psyche.py` 默认要现编 BootShim | 脚本已会回落到 `devices/psyche/prebuilt/BootShim.bin`（208 B，就是实机那份）；两条命令照跑 |

---

## 3.5 USB 身份：三个容易被误判成"驱动坏了"的点

1. **Prot_03 是 loader 自己报的，不是 UEFI FD。** `xnuloader/loader.c` 第 3625 / 3631 行两处描述符都写着
   `0xff, 0x42, 0x03`；想改成 `Prot_01` 改这里就行，但没必要。
2. **需要装 `android_winusb.inf`，但不需要自签。** 没装过的机器上 loader 就是"未知设备"（Code 28），
   `fastboot devices` 永远空 —— 这一步是真的要装。装完（包里 `drivers/windows/android_winusb.zip`，
   已签名，`pnputil /add-driver` 一条命令）之后，本机 `VID_18D1&PID_D00D` 绑的是 `oem67.inf`（`android_winusb.inf` 的 OPPO 分支），
   它按 PID 直接点名 `%SingleBootLoaderInterface% = USB_Install, USB\VID_18D1&PID_D00D`——
   匹配跟接口描述符无关。Google 自家的 `android_winusb.inf`（本机 `oem96.inf`）里也另有
   `USB\Class_FF&SubClass_42&Prot_03` 那条安装行。所以"缺 `.cat` / 要 `inf2cat` / 要 `testsigning`"都不成立，
   11-24 全程 fastboot 正常。详见 `docs/usb-driver.md`。
3. **`usb_connected` 的门槛确实存在，但正常路径会满足。** loader.c 3619 行 `static int usb_connected;`（默认 0），
   只由 `UsbDeviceEventDeviceStateChange` 置位；`usb_command_loop()` 里 `if (!usb_connected) { usb_wait(0xff); continue; }`。
   实机从 Windows 冷进 loader 时事件会到（否则 11-24 那次 `stage/get_staged` 全部无法完成），所以不要凭这段代码就改默认值。

---

## 4. WiFi 走到哪了（诚实版）

| 阶段 | 状态 | 证据 |
| --- | --- | --- |
| PCIe 枚举 / SMMU / QRTR 双向 | ✅ | 1–18 号设备枚举，QRTR 收发正常 |
| QMI 握手 → 读固件版本 | ✅ | `WLAN.HST.1.0.1.r1-01134-QCAHSTSWPL_SILICONZ-1.586138.2.619465.3` |
| `mode 7` 冷启动校准 | ⚠️ 12 次跑机成功 1 次 | 唯一一次 `CAL DONE + READY` |
| 校准后 restart | ⚠️ | SMMU 幂等 bug 已修（`fc7d35c`），修完还没跑到 |
| `mode 0` → HTC → `en0` | ⛔ 从未到达 | 固件自断言 `fw log 0x2ffe80: Asserted in 0x1570b70s:1979` |

判断是**竞态**而不是参数错的依据：同样的输入，三种失败形态随机出现。
另外这套 WiFi 需要重编内核（`drivers/wifi/hakios.amss.c` 把 4.3 MB 的 `amss20.bin` 编进去了，
因为 radio 比任何 ramdisk 都早启动），本包的内核是 11-19 那份，**不含** amss——
要跑 WiFi 得先按 `docs/replica.md` 重编。

---

## 5. 还没做（不是打包漏了）

1. 桌面 Weather / Calendar 两个 widget 是空白白块（截图可见）——缺对应的 iOS 框架依赖。
2. WiFi 的 HTC/`en0`（见上）。
3. 电量走真 SPMI 而不是 loader 缓存。
4. 键盘/音量键、摄像头、音频都还没接。

---

## 6. 文档纪律（这次学到的）

- **归档 ≠ 删除**：被推翻的结论不删，移到 `docs/archive/` 并附一条"为什么错"。
  这样下次再看到那份文档的人不会重新踩一遍。
- **一个入口**：`docs/STATUS.md` 是唯一权威页，其它文档要么指向它，要么明确标"历史"。
- **别信时间戳，信字节**：判断某个镜像"是不是被换过"，比 md5、比内嵌构建时间字符串，而不是比文件时间。
