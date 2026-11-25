# 审计报告：自编内核为什么起不来

> 2026-11-25，三路并行审计。每份报告都标了文件路径:行号。
> 主线已排除的假设、以及三家各自查到的东西，都在这里，避免下一个会话重新走一遍。

---

## 0 状态与主线结论

**构建链通了，内核起不来。死点在 `__start` 之后、`machine_startup` 之前。**

### 已实测排除的假设

| 假设 | 验证方法 | 结果 |
|---|---|---|
| `__start_set_pcpu` / `__stop_set_pcpu` 段边界不同 | 解析 Mach-O 符号表 | **排除**。只是 8 字节标记，在 `__DATA` 末尾（`0x83a7970`），A 多两个是链接布局差异 |
| loader 不清 BSS | 读 `xnuloader/loader.c:3006` | **排除**。`memset(vmsize)` 后才 `memcpy(filesize)` |
| 内核超出 loader 的 `MAX_MEM_SIZE` | 算 `vm_hi - KERNEL_VIRT_BASE` | **排除**。两颗都是 25 MB，限制 6 GB（`loader.c:27`） |
| `__BOOTDATA` 各节重叠/越界 | 逐节算地址区间 | **排除**。两颗都干净，无重叠、无越界 |
| 内核入口被改坏 | 反汇编 `__start` | **排除**。两颗都是 `b _start_first_cpu`，**入口字节完全相同** |
| `__TEXT_EXEC` 启动路径函数被改 | 符号名配对 + 指令归一化对比 | **排除**。见 [function-diff.md](function-diff.md)：启动路径 0 处真实差异 |

### 死点的确证方式

六处 `loader_log_line()` 检查点 + `_cnputs` 全局 tee，**全部零输出**：

```
osfmk/arm64/machine_routines.c   machine_startup 入口 / machine_conf 前后 / kernel_bootstrap 前
pexpert/arm/pe_init.c            PE_init_platform 入口 / vm_initialized 分支
osfmk/console/serial_console.c   _cnputs（所有 printf 必经之路）
```

从 `logdump` 分区（`/dev/block/sda25`，Magisk root）读回，`jumping to f7a164a8` 之后：

```
jumping to f7a164a8.
log: 4096 bytes written to logdump (0), read back the same (0).   <- loader 活着
core 700: the firmware answers 0 to the request to start it.      <- PSCI 正常
[700 字节全零]                                                     <- 内核一个字都没写
```

**loader 完好，PSCI 正常，内核零输出 → 死在 `__start` 跳过去之后的纯汇编/极早期 C 阶段。**

---

## 1 上游项目现状（`D:/IOS/upstream-hakios`）

`oosexgang/hakios-xnu-kernel` 默认分支 `common`，已重构，与我们分叉的 `psyche` 差两代：

| | 上游 | 我们 |
|---|---|---|
| XNU | `12377.121.6`（Darwin 26/27） | `8796.121.2`（Darwin 22 / iOS 16.5） |
| 结构 | `arch/` `darwin/` `components/` `fs/` `crypto/` | `xnu/` `drivers/` `userland/` |
| 跑到哪 | 骁龙 780G 上 Darwin 27.2 桌面（`gpu=0` 软绘） | psyche 上 GPU+触摸+电池 |

上游的 `arch/arm64/kernel/loader_return.c` 有一处**明显更完善**的实现，值得抄：

```c
// 上游：MPIDR 持有者标识 + 可重入 + 失败降级
uint32_t me = (uint32_t)(MPIDR_EL1 & 0xffffff) + 1;
if (os_atomic_load(&loader_log_lock, relaxed) == me) return false;  // 重入
...
if (!os_atomic_xchg(&loader_log_lock_broken, true, relaxed)) { ... }  // 降级为无锁写
```

```c
// 我们：无标识、无重入、无降级，纯自旋 1000 万次
for (uint32_t tries = 0; !locked && tries < 10000000; tries++) {
    locked = os_atomic_cmpxchg(&loader_log_lock, 0, 1, acquire);
}
```

**风险**：早期启动若有嵌套调用，我们版本自旋 1000 万次 = 死机且无输出。
**但**：`_cnputs` 的 tee 也没输出，说明根本没执行到那里，所以这不是根因（只是潜在隐患）。

---

## 2 两份详细审计

| 报告 | 内容 |
|---|---|
| [qcom-usb-portability.md](qcom-usb-portability.md) | `FFT-infra/QcomXnuUsbDebug`（同 SoC 的 USB 调试通道）能否移植。**六个 XNU API 签名全兼容**；结论：只抄核心 0.2–0.5 人日，整模块 3–5 人日且有变砖风险 |
| [function-diff.md](function-diff.md) | 两颗内核 27,921 对函数逐一对比（含指令归一化 + 符号语义）。**启动路径 0 处真实差异**；真实改动 548 处集中在 ath11k(381)、GPU(29)、lx(16) |

---

## 3 下一步可走的路

### A. 在 `start.s` 里插裸写的检查点（最靠前，不依赖任何 C 运行时）

`__start` 只有一条 `b _start_first_cpu`。可以在 `_start_first_cpu` 开头插一条写固定物理地址的指令，不改任何状态。

已知可用目标：`loader_return.c:66` 的 `loader_log`（1.5 MiB 静态数组，在 `__DATA`，loader 清零过）。

### B. 用 `loader_checkpoint()` 二分

`loader_return.c:405` 有现成机制：

```c
void loader_checkpoint(uint64_t n) {
    if (loader_stub_slot[2] == n) loader_jump(2, n);   // 立即返回 loader
}
```

`fastboot oem run N` 会让内核在每个 `loader_checkpoint(n)` 处比对。**但我们的树里一个调用点都没有**（上游有 23 个）。要自己插。

这比日志更可靠：在汇编层判断，不依赖任何初始化。

### C. 装 `QcomXnuUsbDebug` 的核心部分

见 [qcom-usb-portability.md](qcom-usb-portability.md) §2「可直接抄」。但**先做 A 或 B**，不要在不知道死在哪的前提下先装一套有变砖风险的驱动。

---

## 4 参考材料位置

| 路径 | 内容 |
|---|---|
| `D:/IOS/upstream-hakios/` | 上游完整代码（33 MB，分支 `common`） |
| `D:/IOS/qcom-usb-debug/` | QcomXnuUsbDebug 源码 |
| `D:/IOS/ref-qcomusb/` | 四份参考文档 + `Sm8250.h` |
| `D:/IOS/reports/` | 原始审计报告 |
| `D:/IOS/logs/logdump.bin` / `logdump-tee.bin` | 从手机 `logdump` 分区读回的原始数据 |
