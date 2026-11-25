# 内核死在 machine_startup 之前：证据、以及同 SoC 已有的解法

> 2026-11-25 的调查。上一份 `selfbuild.md` 只说「构建链通了但内核起不来」，
> 这份把**死在哪**确证了，并找到了 SM8250 上已经跑通的同类方案。

---

## 1 死在哪：确证到 machine_startup 之前

### 1.1 方法

在启动最早期的几个 C 入口插 `loader_log_line()` —— 这是内核把日志交给 loader
的现成通道（`osfmk/arm64/loader_return.c`，只依赖静态变量与原子操作，
**不需要内存分配器或锁初始化**，启动极早期可用）：

```c
osfmk/arm64/machine_routines.c    machine_startup 入口 / machine_conf 前后 / kernel_bootstrap 前
pexpert/arm/pe_init.c             PE_init_platform 入口 / vm_initialized 分支
```

六处检查点全部编进了内核（`strings` 数出 6 个 `[HKL]`）。

### 1.2 结果：一条都没打印

从 **`logdump` 分区（`/dev/block/sda25`）**直接读回（手机有 Magisk root）：

```
payload entry 'kernel' size 154bcc8      <- 这次自编内核的载荷
kernel loaded at pa f7004000..f8958000
jumping to f7a164a8                      <- 跳进内核
```

**`jumping` 之后一行新的都没有。** 紧接的是 `2026-11-24 04:22:42` 那次成功运行的
旧残留（`launchd`、`heartbeat 2`、194,612 字节）。

loader 的心跳是每 2 秒写一次 logdump，**连这第一次写都没发生**。所以：

> **内核在 `machine_startup` 的第一条 C 语句之前就死了，并且 loader 的 2 秒心跳线程
> 也一起停了。**

`machine_startup` 之前是汇编：`start` / `vstart` / `arm_init` /
`arm_vm_init`（建页表）/ `PE_init_kprintf`。

### 1.3 一条可疑的符号差异

对比两颗内核时，只在 prebuilt 里出现、且不属于 WiFi 的符号只有两个：

```
__start_set_pcpu
__stop_set_pcpu
```

这是链接器为 `set_pcpu` 段（per-CPU 数据）生成的边界。**段布局不同**，
在启动最早期 per-CPU 寻址出错就会立即崩、且不留日志 —— 与观察到的现象吻合。
（未验证：这只是目前唯一指向启动基础设施而非 WiFi 的差异。）

---

## 2 同 SoC 已有的解法：QcomXnuUsbDebug

### 2.1 相关项目

| 项目 | 是什么 |
|---|---|
| [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU) | 主项目：通过 UEFI 在非 Apple ARM64 硬件上跑 iOS/macOS |
| [shinyquagsire23/UniXNU-pdx203](https://github.com/shinyquagsire23/UniXNU-pdx203) | **SM8250**（索尼 Xperia 1 II）设备支持，含 `board.toml`、`dts/`、`patches/` |
| **`FFT-infra/QcomXnuUsbDebug`** | **高通 XNU 的 USB 调试通道**（DWC3 CDC），有专门的 `soc/Sm8250.h` 和 `docs/port-sm8250.md` |
| [atTechGeekDev/ios-apollo](https://github.com/aTechGeekDev/ios-apollo) | 小米 10T（SM8250 / QCA6390），WiFi 通 |

### 2.2 它解决什么

`QcomXnuUsbDebug` 在 `_cnputs` 里把**每个格式化字节直接旁路到 USB**，注释写得很明确：

> `qcom_usb_debug_write` is **bounded, non-blocking and safe in any context,
> including before the scheduler exists**; it copies into a CPU-only backlog
> and never prints, allocates or waits.

这正是我们缺的东西：内核死在打印之前，但只要这个旁路在，字节就能出来。

### 2.3 SM8250 的硬件参数（`soc/Sm8250.h`，7,681 B）

每个值都注明了来源（`sm8250.dtsi` 行号 / `alioth.dts` 行号）：

```c
QUD_GCC_BASE     0x00100000   QUD_GCC_BYTES    0x1f0000
QUD_DWC3_BASE    0x0a600000   QUD_DWC3_BYTES   0x100000
QUD_SMMU_BASE    0x15000000   QUD_SMMU_BYTES   0x100000
QUD_STREAM_ID    0x0          (不是 SM8550 的 0x40)
QUD_DMA_IOVA     0x90000000
五个 CBCR         0xf010..0xf058
USB3 block reset 0xf000
GDSC control     0xf004
QSCRATCH HS PHY  0xf8810
```

**与我们的 psyche（SM8250-AC）同一 SoC**，寄存器布局可直接参考。

### 2.4 它锁定的 XNU 版本与我们不同

| | 它 | 我们 |
|---|---|---|
| XNU | `1b191cb5` / `xnu-8796.141.3` / `xnu-10063.141.1` | `8796.121.2`（iOS 16.5） |

**补丁不能直接 apply**，但：

- `soc/Sm8250.h` 的寄存器值可以照抄
- `kernel/QcomXnuUsbDebug.c` 用的 XNU API（`ml_io_map`、`PE_parse_boot_argn`、
  `PE_halt_restart`、`master_cpu`、`panic_active`）在 `8796.121.2` 里**全部存在**
- 两份 console/worker 补丁的**思路**可以直接照做（改 `serial_console.c` 的
  `_cnputs` 加一条旁路）

### 2.5 它自己声明的证据等级（诚实）

| 项 | 状态 |
|---|---|
| host 模型测试 | 24/24 通过 |
| 补丁 apply 到真实树 | `git apply --check` rc=0 |
| 六个单元用真实 flags 交叉编译 | 通过（hook 开/关） |
| **完整内核链接** | **未做** |
| **硬件枚举** | **未做** |

所以它也不是「拿来就能跑」，但它是**目前最接近的完整参考实现**。

---

## 3 两条可行的下一步

### A. 抄它的思路，自己做一个最小 USB 日志旁路

不引整个模块，只在 `osfmk/console/serial_console.c` 的 `_cnputs` 里加一条
「把字节写到某个固定的 loader 可见区域」。风险低，代码量小（约 50 行）。

依据：我们的内核**已经有** `loader_log_line()` 这条通道，只是它只在特定位置被调用。
把它挂到 `_cnputs` 上，所有 `printf` 就都能被 loader 看见。

### B. 先查 `__start_set_pcpu`

在做 A 之前，先花 20 分钟确认这个符号差异是不是根因。
如果是，可能不需要 USB 日志就能修好。

---

## 4 参考材料（本机）

`D:/IOS/ref-qcomusb/`：

| 文件 | 内容 |
|---|---|
| `port-sm8250.md` (19,670 B) | SM8250 端口全过程、寄存器对照表、证据等级 |
| `Sm8250.h` (7,681 B) | SoC 配置，每个值的出处 |
| `architecture.md` (10,213 B) | 模块分层 |
| `usage-and-validation.md` / `control-and-screenshot.md` | 用法与协议 |

上游：`https://github.com/FFT-infra/QcomXnuUsbDebug`（默认分支 `main`）