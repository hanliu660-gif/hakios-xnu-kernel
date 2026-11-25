# 结论反转：自编内核跑到了桌面，之前「起不来」是日志开关的问题

> 2026-11-26 02:30。用 `loader_checkpoint` 二分 + 把 tee 挂到正确的位置，拿到了内核日志。
> **内核一直是好的。** 前面四份文档的「死在早期」结论全部作废。

---

## 0 一句话

**自编内核跑到了 iOS 桌面，`SpringBoard`、`backboardd`、`hakios-gl` 都在工作，触摸也收得到。**

之前看到「零输出 → 2 秒掉回 Android」，是因为内核日志被两个默认关闭的开关挡住了。

---

## 1 根因：两个 flag 默认关闭

`pexpert/arm/pe_kprintf.c:21-23`：

```c
SECURITY_READ_ONLY_LATE(bool) disable_serial_output = true;    // 默认 true
SECURITY_READ_ONLY_LATE(bool) disable_kprintf_output = true;   // 默认 true
```

`kprintf()` 的最外层（`pe_kprintf.c:104`）：

```c
if (!disable_serial_output && !disable_kprintf_output) {
    ... _doprnt_log(fmt, &listp, PE_kputc, 16);   // 真正的打印
} else {
    os_log_with_args(...);                        // 只进 os_log，不打印
}
```

**两个都为 false 才会打印。** 怎么关掉（`pe_kprintf.c:37-58`）：

| 变量 | 关掉的条件 |
|---|---|
| `disable_serial_output` | `debug_boot_arg & DB_KPRT`，即 boot-arg `debug=0x8` |
| `disable_kprintf_output` | `DEBUG` 构建，**或 DEVELOPMENT 构建 + boot-arg `-enable_kprintf_spam`** |

我们的 cmdline 原来只有 loader 自己加的 `debug=0x8`，**没有 `-enable_kprintf_spam`**，
所以 `disable_kprintf_output` 一直是 true，整个打印路径被跳过。

### 修正后的 cmdline

```
loader_return_after=240 epoch=1795465349 sb_delay=8 touchscreen=1 bootcpu=7 cpus=8
cpufreq=1 gpu=1 battery=1 battery_pmic=1 fb_ca_hz=120
debug=0x8 -enable_kprintf_spam
```

---

## 2 tee 必须挂在 `_cnputc`，不是 `_cnputs`

这是第二个坑。`_doprnt_log` 是**逐字符**调用 `PE_kputc(c)`：

```
_doprnt_log -> PE_kputc(c) -> serial_putc_crlf(c)  或  console_write_unbuffered(c)
                                |                          |
                                +--------------------------+
                                            v
                                       _cnputc(c, poll)     <- 单个字符
```

而 `_cnputs(c, size)`（复数）只在批量路径上用，`printf` 根本不经过它。

**所以第一版把 tee 加在 `_cnputs` 上，一条日志都没出来。**
改到 `_cnputc` 之后立刻全出来了。

```c
// osfmk/console/serial_console.c  _cnputc() 开头
extern void loader_log_line(const char *line);
if (c != '\r' && c != 0) {
    char one[2];
    one[0] = c; one[1] = 0;
    loader_log_line(one);
}
```

---

## 3 日志证明内核到了桌面

从 `logdump` 分区（`/dev/block/sda25`，Magisk root）读回，`== KERNEL LOG ==` 之后 **134 行有内容**，末尾：

```
hakios-gl: frame 4220: 942 GL calls, 187 draws, 730 triangles, 25 clears;
           0 textures refilled from IOSurfaces; 8010 us
2026-11-24 04:26:17 backboardd[11:514] [TouchEvents] contact 1 presence: touching.
com.apple.xpc.launchd (pid/33 [Spotlight]) <Notice>: uncorking domain.
com.apple.xpc.launchd (pid/33/com.apple.WebKit.GPU) <Notice>: created the implicit endpoint upfront.
```

| 证据 | 说明 |
|---|---|
| `SpringBoard[7]` | 桌面进程在跑 |
| `backboardd[11]` + `[TouchEvents] contact 1 presence: touching` | **触摸在工作** |
| `hakios-gl: frame 4220` | GL 渲染到第 4220 帧 |
| `Spotlight[33] uncorking domain` | 系统服务在启动 |
| `WebKit.GPU` / `WebKit.WebContent` | WebKit 子系统起来了 |
| `cpu 4/5/6 is a slower core` | 大小核拓扑正确 |
| `cpufreq: group 0/1/2 ... MHz` | 三组核心频率都读到了 |
| `adreno: step 5 ... CX is on` | **GPU 初始化到 step 5** |
| `gic: v3, distributor 0x...` | GICv3 起来了 |

---

## 4 被推翻的结论（四份文档）

| 文档 | 错误结论 | 实际 |
|---|---|---|
| `kernel-death-and-usb-debug.md` | 死在 `machine_startup` 之前 | **跑到了桌面** |
| `bisect-results.md` | 死在 `kernel_bootstrap` 之后某处 | **没有死，是日志被关** |
| `audit/README.md` | 需要 USB 调试通道才能看到日志 | **不需要，开两个 boot-arg 就行** |
| `selfbuild.md` | 编出的内核起不来 | **能起来** |

共同的方法学错误：**把「零输出」当成「没执行」。**
`loader_checkpoint` 二分证明了代码一直在跑；tee 挂对位置后日志立刻出来。

---

## 5 复跑

```bash
cd /d/IOS/repos/hakios-xnu-kernel
CMDLINE="loader_return_after=240 epoch=1795465349 sb_delay=8 touchscreen=1 bootcpu=7 cpus=8 \
cpufreq=1 gpu=1 battery=1 battery_pmic=1 fb_ca_hz=120 debug=0x8 -enable_kprintf_spam"

python3 xnuloader/mkpayload.py -o /d/IOS/phone/p.bin \
  kernel=D:/IOS/phone/k-tee ramdisk=userland/root.apfs \
  --cmdline "$CMDLINE" --kernel-version "$(cat kernel.ver)"

fastboot oem ramdisk
FB_TIMEOUT=1800 fastboot stage userland/system.apfs
FB_TIMEOUT=180  fastboot stage /d/IOS/phone/p.bin
fastboot oem run
# 等 240 秒自返，然后：
fastboot get_staged /tmp/boot.log     # 或从 logdump 分区读
```

**读日志的两种方式**：

1. `fastboot get_staged` —— 内核返回 loader 后可用
2. `adb shell "su -c 'dd if=/dev/block/sda25 bs=4096 count=200'"` —— 手机已回 Android 时可读

---

## 6 现在可用的东西

| 项 | 值 |
|---|---|
| 能跑到桌面的自编内核 | `495c0fe1bb7103a0eec39b654e7a30aa`（22,330,568 B） |
| 与它配套的 payload | `/d/IOS/phone/p-putc.bin`（33,168,944 B） |
| 必需 boot-arg | `debug=0x8 -enable_kprintf_spam` |
| 日志通道 | `_cnputc` 里的 tee（已改在 `serial_console.c`） |
| 检查点工具 | `fastboot oem run N`，N 见 `bisect-results.md` |
