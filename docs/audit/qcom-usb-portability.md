# QcomXnuUsbDebug 可移植性审计（xnu-8796.121.2 / SM8250-AC kona / 小米 12X）

审计对象：`FFT-infra/QcomXnuUsbDebug`（main 分支 2025-10-28 快照；`README.md:5` 自述「内核补丁对应 Darwin22 / XNU 8796.141.3」）。
目标树：WSL `/root/hakios/xnu-src/xnu-8796.121.2`（`git log` = `c54f35c xnu-8796.121.2`，带 HAKIOS 改动）。
落盘：`D:/IOS/qcom-usb-debug`（`git clone` 被 GitHub 重置连接，改走 `codeload` zip，命令见第 6 节）。

结论先行：**六个 API 签名全部兼容，没有一个需要改类型**；成本不在 API，而在「补丁 hunk 对不上」和「本树已有 loader_log tee，装上去会互相打死」。按现价：**(b) 只抄核心 ≈ 0.2–0.5 人日，(a) 整模块 ≈ 3–5 人日，且带把 USB 一起搞没的风险**。

---

## 1. 六个 API 逐个核对（本树实测，路径:行号 + 签名）

| 模块调用点 | 本树声明 | 签名 | 判定 |
| --- | --- | --- | --- |
| `kernel/QcomXnuUsbDebug.c:804,806,808` `ml_io_map` | `osfmk/arm/machine_routines.h:703` | `vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size)` | 兼容 |
| `kernel/QcomXnuUsbDebug.c:710,713,715` `ml_static_vtop` | `osfmk/arm/machine_routines.h:575`；实现 `osfmk/arm64/arm_vm_init.c:411` | `vm_offset_t ml_static_vtop(vm_offset_t)` | 签名兼容，语义有坑（3.2） |
| `kernel/QcomXnuUsbDebug.c:736,759` `PE_parse_boot_argn` | `pexpert/pexpert/pexpert.h:337` | `boolean_t PE_parse_boot_argn(const char *, void *, int max_arg)` | 完全一致 |
| `kernel/QcomXnuUsbDebug.c:35,1279` `PE_halt_restart` | `iokit/IOKit/IOPlatformExpert.h:90`；定义 `iokit/Kernel/IOPlatformExpert.cpp:339` | `extern int (*PE_halt_restart)(unsigned int type)` | 完全一致（模块自带 extern，绕开 C++ 头，可行） |
| `kernel/QcomXnuUsbDebug.c:199` `master_cpu` | `osfmk/kern/cpu_number.h:62`；定义 `osfmk/kern/processor.c:143`；赋值 `osfmk/arm/arm_init.c:444` | `extern int master_cpu` | 类型一致，时序受限（3.1） |
| `patches/darwin22/0002-...worker.patch:177` `panic_active` | `osfmk/kern/debug.h:622`；实现 `osfmk/kern/debug.c:1761` | `unsigned panic_active(void)` | 兼容（返回 unsigned，当布尔用无碍） |
| （模块未列出）`cpu_number()` | `osfmk/arm/cpu_number.h:73`；宏 `#define getCpuDatap() current_thread()->machine.CpuDatap`（`osfmk/arm/cpu_data.h:123`） | `int cpu_number(void)` | 兼容，但有 NULL 风险（3.1） |

**明确的不兼容点（不是签名，是可见性与语义）：**

1. `ml_io_map` 的声明被 `osfmk/arm/machine_routines.h:692` 的 `#if defined(PEXPERT_KERNEL_PRIVATE) || defined(MACH_KERNEL_PRIVATE)` 包住，外层还有 `__APPLE_API_PRIVATE`（`:683`）。构建时 `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/Makefile:22` 有 `-DMACH_KERNEL_PRIVATE`、`pexpert/DEVELOPMENT/Makefile:22` 有 `-DPEXPERT_KERNEL_PRIVATE`，但 **bsd 单元两个都没有**——这正是仓库自己在 `patches/darwin23/0002-...worker.patch:25-27` 手写 `extern` 的原因。移植后 bsd 侧任何 `ml_io_map`/`ml_static_vtop` 调用都必须自带 `extern`。
2. `ml_static_vtop` 在本树**会 panic**：`osfmk/arm64/arm_vm_init.c:418-421` 对越界 VA 直接 `panic("... illegal VA ...")`。模块 `validate_static_pages()`（`kernel/QcomXnuUsbDebug.c:705-720`）把它当无副作用查询，逐页调三次；一旦 `usb_dma`/`usb_tables` 落在 ptov 表覆盖外，调试器自己先把内核打死。
3. `panic_active` 在 `#if XNU_KERNEL_PRIVATE` 内（`osfmk/kern/debug.h:586-624`），bsd 单元默认拿不到，同 1，需自带 `extern`。

---

## 2. 可直接抄（零改动，逐行核对过）

| 项 | 出处 | 本树对应 | 说明 |
| --- | --- | --- | --- |
| SoC profile 全套寄存器值 | `soc/Sm8250.h:33-133` | 无冲突 | GCC `0x100000`+`0x1f0000`、DWC3 `0xa600000`+`0x100000`、SMMU `0x15000000`+`0x100000`、SID `0x0`、CBCR `0xf010/18/1c/54/58`、BCR `0xf000`、GDSCR `0xf004`、QSCRATCH `0xf8810`，每项带 dts / Linux 行号出处，纯数据。 |
| 强制 profile 选择的静态断言 | `include/QcomUsbSoc.h:9-40` | 独立 | 窗口不重叠、IOVA 2 MiB 对齐等编译期校验，直接可用。 |
| CPU-only 日志环形缓冲 | `kernel/QcomUsbBuffer.h:12-77` | 独立 | 4 MiB、`__atomic_exchange_n` 非自旋、保最旧字节，零 libc 依赖。 |
| DWC3 控制器核心 | `usb/Dwc3.c:471-687` | 独立 | 只依赖 `struct dwc3_platform` 七个回调（`usb/Dwc3.h:38-48`），零 OS 头、零平台 ID。 |
| QSMMUv500 私有域 | `soc/QcomUsbDmaDomain.c:38,273,398` | 独立 | 只经 `QUSB_DOMAIN_PLATFORM`（`soc/QcomUsbDmaDomain.h:15-24`）碰硬件。 |
| 只读资格判定 | `soc/QcomUsbQualify.c:19-79` | 独立 | `QcomUsbInspectInherited` 只读 CBCR/GDSCR/GCTL；`:44-47` 已用 `QUD_GDSC_POWER_BIT_LEGACY` 处理 SM8250 的 legacy GDSCR 语义。 |
| DAIF 保存/恢复 + 非自旋互斥 | `kernel/QcomXnuUsbDebug.c:197-211` | 独立 | `mrs daif` / `msr daifset,#3`，纯内联汇编。 |
| CNTVCT 计时 | `kernel/QcomXnuUsbDebug.c:83-90` | 独立 | 与本树 `osfmk/arm64/loader_return.c:98` 的 DAIF 用法同构。 |
| 逐页验证物理连续 | `kernel/QcomXnuUsbDebug.c:705-720` | 依赖 `ml_static_vtop` | 思路可抄，外层要加范围判断。 |

---

## 3. 需改

### 3.1 `cpu_number()` / `master_cpu` 的可用时刻（本审计最硬的一条）

- 本树 `cpu_number()` = `getCpuDatap()->cpu_number`（`osfmk/arm/cpu_common.c:817-820`），`getCpuDatap()` = `current_thread()->machine.CpuDatap`（`osfmk/arm/cpu_data.h:123`）。
- `current_thread` 在 `osfmk/arm/arm_init.c:461-465` 才由 `thread_bootstrap()` + `machine_set_current_thread()` 装上。
- 模块 `acquire()` 第一行就是 `if (cpu_number() != master_cpu) return 0;`（`kernel/QcomXnuUsbDebug.c:199`）。
- **结论**：`qcom_usb_debug_init()` 不能挂在 `arm_init.c:444` 之前。仓库把挂钩点放在 `PE_init_kprintf`（`pexpert/arm/pe_kprintf.c:60-73`，由 `arm_init.c:557` 的 `kernel_startup_initialize_upto(STARTUP_SUB_KPRINTF)` 驱动）恰好安全——别为「更早」把它前移到 `loader_checkpoint(19)`（`arm_init.c:558`）之前；要更早只能用 (b)。

### 3.2 `ml_static_vtop` 外面必须加范围判断

在 `kernel/QcomXnuUsbDebug.c:705` 的 `validate_static_pages()` 前加 `if (va < gVirtBase || va - gVirtBase >= gPhysSize) return 0;`（同 `osfmk/arm64/arm_vm_init.c:403,418` 的判断）。否则一个坏地址换来一次 `panic()`。

### 3.3 补丁只能用 `darwin22/`，且有一个 hunk 必须手改

实测 `git apply --check`（本树六个文件拷进临时 git 仓，逐文件检查）：

| 补丁 | 文件 | 结果 |
| --- | --- | --- |
| `darwin22/0001-console` | `pexpert/arm/pe_kprintf.c` | OK（offset -1） |
| | `osfmk/kern/printf.c` | OK |
| | `osfmk/console/serial_console.c` | OK（offset +20） |
| | `osfmk/console/serial_general.c` | OK |
| | `osfmk/kern/debug.c` | **FAIL** |
| `darwin22/0002-worker` | `bsd/kern/bsd_init.c` | OK（offset -172） |
| `darwin23/0001-console` | `pe_kprintf.c` / `debug.c` / `serial_console.c` | **全 FAIL** |
| `darwin23/0002-worker` | `bsd_init.c` | OK |

`darwin23` 是给 10063 的，别用。唯一失败点：补丁找 `panic_spin_forever(void)` 后紧跟 `{` 再 `for (;;) {`，本树 `osfmk/kern/debug.c:1153-1156` 是 `{` 后先有 `loader_return();`。把 `qcom_usb_debug_panic_drain()` 插在 `loader_return();` **之前**（loader_return 一旦交还 loader 就不回来了）。

### 3.4 `darwin22/0001` 会**双重 tee**，只保留一处

- 它在 `console_write()` 开头加分块 tee（补丁 `:187-203`，落到本树 `osfmk/console/serial_console.c:535`），**又**在 `_cnputs()` 加 tee（补丁 `:228-241`）。正常路径 `console_write -> _cnputs` 会把每个字节 tee 两遍。
- `darwin23` 已修成只在 `_cnputs` 和 `console_write_unbuffered` 各一次。**用 darwin23 的落点、套 darwin22 的上下文**改。

### 3.5 本树已有 tee，装模块会顶掉 loader 日志

- 本树 `osfmk/console/serial_console.c:274-292` 已有 `HAKIOS_LOG_TEE` 块（调 `loader_log_line()`）。
- 本树 `pexpert/arm/pe_kprintf.c:158-165` 的 `serial_putc_crlf()` 里调 `loader_log_putc(c)`（`:32` extern，`:163` 调用）——**kprintf 现在正是靠这条进 `loader_log`**。
- 而 `darwin22/0001` 收尾是 `if (qcom_usb_debug_init()) PE_kputc = qcom_usb_debug_putc;`（补丁 `:55-58`），把 `PE_kputc` 从 `serial_putc_crlf` 换掉 → **`loader_log_putc` 从此收不到 kprintf 字节，loader 回读通道静默失效**。
- 改法二选一：在 `qcom_usb_tee()` 里同时调 `loader_log_putc(c)`；或干脆不换 `PE_kputc`，只留 `_cnputs` 那一处 tee。

### 3.6 编译单元与头文件可见性

`kernel/QcomXnuUsbDebug.c:12-14` 只 include `machine/machine_routines.h`、`kern/cpu_number.h`、`pexpert/pexpert.h`。放进 osfmk 目录编译（带 `-DMACH_KERNEL_PRIVATE`）即可拿到 `ml_io_map` 与 `master_cpu`；`PE_halt_restart` 靠 `:35` 自带 extern，**不要**改成 include `IOKit/IOPlatformExpert.h`（C++ 头，会污染 C 单元）。
---

## 4. 风险（按杀伤力排序）

1. **`ml_io_map` 只有 8 MiB io_submap 预算**：`osfmk/arm/io_map.c:65` `#define IO_MAP_SIZE (8ul << 20)`，`:76` 一次性 `kmem_suballoc` 固定；`:119-130` 走 `KMA_PERMANENT`，不可回收。模块一次要约 `0x3F0000`（≈3.9 MiB，近一半）；本树 `osfmk/arm64/machine_routines.c:1259-1273` 的 IMPL/coresight/UTTDBG/CPM 映射也在吃这块。模块自己在 `kernel/QcomXnuUsbDebug.c:120-124` 也写了「`ml_io_map` 给的是永久映射，`ml_io_unmap` 收不回」。
2. **`ml_static_vtop` 会 panic**（3.2）。调试器把自己打死，比没日志更糟。
3. **`cpu_number()` 早于 `current_thread` 是 NULL 解引用**（3.1）。症状伪装成「死得更早」，极易误判死因。
4. **SMMU 域安装动的是活表**：`soc/QcomUsbDmaDomain.c:273` 的 `QcomUsbDomainInstall` 改 SID 0 的 SMR/S2CR 与 CB 配置，`:398` 的 `Restore` 才还原。还原前挂掉 → 固件原给 USB 的映射已被换，热重启可能连 fastboot 侧的 `xnuloader` 都进不去。
5. **固件所有权判定是三重门且静默**：`soc/Sm8250.h:4-9` 假定继承 ABL 的 DEVICE 模式 PHY/电源；不过就 `fail(INHERITED_STATE_REFUSED)`（`kernel/QcomXnuUsbDebug.c:813-819`），**一个字节都出不来**。唯一线索 `boot_progress_persist`（`:336-337`，`weak_import`）本树**没有这个符号**（全树 grep 为空），所以拒绝理由也无处显示，只能靠 `qcom-usb-diag=N` 的 PSCI 复位延时码（`kernel/QcomXnuUsbDebug.c:1443-1450`）。
6. **`PE_kputc` 单点覆盖**（3.5），换错就关掉已有通道。
7. **长停顿**：`qcom_usb_debug_write()` 在 ACTIVE 路径会 poll 并可能等 1 ms（`kernel/QcomXnuUsbDebug.c:1396-1404`）；而本树 `kprintf` 全程持 `kprintf_lock` 且关中断（`pexpert/arm/pe_kprintf.c:114-127`），1 ms × 每字符 = 秒级停顿，会撞 watchdog / spin-debug。

---

## 5. (a) 整模块 vs (b) 只抄核心

### 5.1 代码量实测

| 组成 | 行数 |
| --- | --- |
| `kernel/QcomXnuUsbDebug.c` | 1534 |
| `usb/Dwc3.c` + `Dwc3.h` + `Dwc3Regs.h` | 687 + 94 + 632 |
| `soc/QcomUsbDmaDomain.c` + `.h` | 480 + 85 |
| `soc/QcomUsbQualify.c` + `.h` | 517 + 158 |
| `soc/Sm8250.h` + `include/QcomUsbSoc.h` + `include/QcomXnuUsbDebug.h` | 137 + 41 + 140 |
| **核心小计** | **4583** |
| 可选（控制/截图，`QcomUsbControl.h` + `QcomUsbScreenshot.*`） | 561 |
| 补丁新增行 `darwin22/0001`（17 hunk / 5 文件） | +102 |
| 补丁新增行 `darwin22/0002`（2 hunk / 1 文件） | +216 |

### 5.2 (a) 整模块移植 ≈ 3–5 人日

拆解：① 构建接入 3 处（osfmk / pexpert / bsd 的 `OBJS` + `CFILES` + `-I` + `-DXNU_QCOM_USB_DEBUG=1`，仿 `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/Makefile:22,46-64`）；② 6 个 hunk 手改（3.3/3.4/3.5）；③ 2 处语义补丁（vtop 范围、`PE_kputc` 保 `loader_log`）；④ 5 个 TU 交叉编译 + import 白名单校验（仓库自带 `scripts/build.py:117-129`，白名单正好是这六个符号 + `bzero/memcpy/memset`）。

耗时分布：0.5 天接入构建，1 天手改补丁，1 天实施/回滚验证，1–2 天实机靠 `qcom-usb-diag` 复位码逐位读出「资格判定死在哪一条」。命中第 4 节 1/4/5 任一，就是「USB 也没了」。

### 5.3 (b) 只抄「tee 到固定区 → loader 读回」≈ 0.2–0.5 人日

**关键发现：这条路本树已经实现，而且比模块更早可用。**

- `osfmk/arm64/loader_return.c:36-38`：`LOADER_LOG_SIZE 1536 KiB`、头 `256 KiB` 保开头 + `1 MiB` 环保结尾；`:66` 的 `char loader_log[]` 是**静态、物理可寻址**的。
- `:90 loader_log_putc(char)` / `:117 loader_log_line(const char *)`：内部关 `DAIF` + 原子锁，**无分配、无调度、无 print**，第一行 C 代码就能用。
- `:181 loader_stub_slot[4]` 与 `osfmk/arm64/start.s:334-341` 的 `XNULDRST` magic、`:620-637` 的 `LOADER_CHECKPOINT` 宏配对；`:232 loader_jump()` 经 TTBR0 1:1 跳回 loader。
- `start.s:305 Lloader_early_exception`：**MMU 关闭状态**下的异常向量会把 `loader_log` 刷回 RAM（`:330-333`）再跳 stub——这正是「死在 `machine_startup` 之前」的唯一出路。
- 本树已有 **23 个** `loader_checkpoint()` 调用点，全在 `osfmk/arm/arm_init.c`：`:364(10),373(11),377(30),379(31),381(32),385(33),401(34),439(35),441(12),445(40),459(41),462(42),466(43),475(44),477(13),480(14),484(15),492(16),542(17),556(18),558(19),600(20),679(21)`（该文件共 24 处匹配，其中 `:342` 是 extern 声明）；汇编侧 `osfmk/arm64/start.s:647` 是 checkpoint 1。`osfmk/arm64/machine_routines.c:471,479,481,487` 用的是 `loader_log_line()`，不是 checkpoint。
- `osfmk/console/serial_console.c:274-292` 的 `HAKIOS_LOG_TEE` 已在 `_cnputs` 入口 tee；`pexpert/arm/pe_kprintf.c:163` 已在 `serial_putc_crlf` 里 tee kprintf。

所以 (b) 的真实增量不是「造 tee」，是**补三个空洞**：

1. **格式缓冲前的截断**：`osfmk/console/serial_general.c:159-176` 的 `console_printbuf_putc` 在 `:172` 处按 `SERIALMODE_SYNCDRAIN` 决定是否丢弃超长行；`_cnputs` 只看到定型后的一行。仓库那招（`darwin23/0001` 对 `serial_general.c` 的 hunk，补丁 `:245-259`）就是把这个分支无条件打开，本树该 hunk **可以干净套上**。
2. **`_cnputs` 的 256 字节截断**：`osfmk/console/serial_console.c:279-289` 现在 `tee[256]`、超长补 `..`、且丢 `\r`、遇 `\0` 就断。长行会缺尾巴。改成逐字节调 `loader_log_putc()`，或搬 `console_write()` 那层的 16 KiB 分块形态。
3. **checkpoint 密度**：`arm_init.c:542`（`arm_vm_init` 之后）到 `:556` 之间只有一个 18，而 `:557` 的 `kernel_startup_initialize_upto` 内部有 20 个 STARTUP 条目却无 checkpoint。补 3–5 个 `loader_checkpoint()` 即可把「死在哪个 STARTUP 条目」二分出来。

净新增约 **120–200 行**，不改驱动、不碰 USB、不碰 SMMU、不碰 `ml_io_map`。

### 5.4 对比

| | (a) 整模块 | (b) 只抄核心 |
| --- | --- | --- |
| 净新增代码 | 4583 行核心（+561 可选）+318 行补丁 | 120–200 行 |
| 要改的本树文件 | 9（3 个 Makefile + 6 个 .c） | 2（`serial_console.c`、`serial_general.c`，可选 `arm_init.c`） |
| 手改 hunk | 6 处失败/需重排 + 2 处语义 | 0 |
| 依赖硬件判定 | 是（CBCR/GDSCR/SMMU 三重门） | 否 |
| 失败可见性 | 可能完全静默（无 `boot_progress_persist`） | 总有部分日志 |
| 变砖风险 | 有（SMMU 表未还原 + io_submap 永久占用） | 无（只写自己的 .bss） |
| 最早可用时刻 | `PE_init_kprintf`（`pe_kprintf.c:60`，即 `arm_init.c:557`） | `start.s:305`（MMU 关也能出） |
| 估时 | 3–5 人日 | 0.2–0.5 人日 |

**建议**：先做 (b) 三处空洞 + 补 checkpoint，把「死在哪」定位出来；只有当日志能稳定走到 `machine_startup` 之后、且需要在**正常运行期**持续拿日志时，(a) 才值那 3–5 人日。顺序倒过来等于在不知道死因的前提下先装一套有变砖风险的驱动。

---

## 6. 复现命令

```bash
# 仓库（git clone 会被重置，用 codeload）
curl -sSL -o /d/IOS/_qcomusb.zip \
  https://codeload.github.com/FFT-infra/QcomXnuUsbDebug/zip/refs/heads/main
wsl -d Ubuntu-24.04 -e bash -lc \
  'cd /mnt/d/IOS && rm -rf _x qcom-usb-debug && unzip -q _qcomusb.zip -d _x && \
   mv _x/QcomXnuUsbDebug-main qcom-usb-debug && cd qcom-usb-debug && \
   git init -q . && git remote add origin https://github.com/FFT-infra/QcomXnuUsbDebug'

# 六个 API 的实测位置
X=/root/hakios/xnu-src/xnu-8796.121.2
grep -rn 'ml_io_map'          $X/osfmk/arm/machine_routines.h
grep -rn 'ml_static_vtop'     $X/osfmk/arm/machine_routines.h $X/osfmk/arm64/arm_vm_init.c
grep -rn 'PE_parse_boot_argn' $X/pexpert/pexpert/pexpert.h
grep -rn 'PE_halt_restart'    $X/iokit/IOKit/IOPlatformExpert.h $X/iokit/Kernel/IOPlatformExpert.cpp
grep -rn 'master_cpu'         $X/osfmk/kern/cpu_number.h $X/osfmk/arm/arm_init.c
grep -rn 'panic_active'       $X/osfmk/kern/debug.h $X/osfmk/kern/debug.c

# 补丁适用性（逐文件；本树文件拷进临时仓）
T=/tmp/xnucopy; rm -rf $T; mkdir -p $T; cd $T; git init -q .
for f in pexpert/arm/pe_kprintf.c osfmk/kern/debug.c osfmk/kern/printf.c \
         osfmk/console/serial_console.c osfmk/console/serial_general.c bsd/kern/bsd_init.c; do
  mkdir -p $T/$(dirname $f); cp $X/$f $T/$f; done
git add -A && git commit -qm base
git apply --check /mnt/d/IOS/qcom-usb-debug/patches/darwin22/0001-qcom-usb-debug-console.patch
git apply --check /mnt/d/IOS/qcom-usb-debug/patches/darwin22/0002-qcom-usb-debug-worker.patch
# 期望：0002 干净；0001 仅 osfmk/kern/debug.c 失败（panic_spin_forever 已插 loader_return()）
```

---

## 7. 一句话给下一个人

六个 API 都能用；`darwin23` 别套，`darwin22` 只改 `debug.c` 一个 hunk 就能套上；但**先别装**——本树的 `loader_log` + `loader_checkpoint` + `start.s` 早停已是同一条思路的现成实现，把 `serial_general.c:172` 的截断打开、`serial_console.c:279` 的 256 字节上限去掉、再补 3 个 checkpoint，比移植 4583 行带 SMMU 写权限的驱动安全得多。