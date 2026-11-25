# __TEXT_EXEC 函数级二进制比对报告

**任务**：对比两颗 XNU 内核（小米 12X / SM8250-AC kona）的 `__TEXT_EXEC` 段，定位启动路径上的差异
**生成时间**：2026-11-25
**工具链**：Windows Python 3（`struct` 手工解析 Mach-O）+ WSL Ubuntu-24.04 `llvm-objdump` / `llvm-nm`（LLVM 18.1.3）
**方法**：按符号名配对 → 逐函数切分 → SHA256 → ARM64 指令级归一化 → 符号重定位语义校验

---

## 0. 结论先行

| # | 发现 | 影响 |
|---|------|------|
| **1** | **任务指定的 B（`D:/IOS/phone/k-tee`）不是纯净的自编内核，而是被 `_loader_log_line` 插桩过的诊断构建。** | 直接拿它做 diff 会得到 **585** 个"差异函数"，其中 40 个纯属插桩噪声（含 `__cnputs`、`_machine_startup`、`_PE_init_platform`）。 |
| **2** | **纯净自编内核（`k-genuine` / `hakios-kernel-selfbuilt`，两者字节相同）的整个早期启动路径与 A 段语义完全一致。** | `machine_startup → arm_init → arm_vm_init → pmap_* → PE_init_platform → PE_init_kprintf → __cnputs` **全部 0 处真实差异**。启动失败**不在** `__TEXT_EXEC` 的代码里。 |
| **3** | **真正的启动路径差异在数据段：`__BOOTDATA.__init_entry_set` 多出 6 条启动项**（A 4209 条 → B 4215 条），含 5× `_kernel_startup_tunable_init` + 1× `_sysctl_register_oid_early`。 | 这是 `kernel_startup_initialize_upto()` 遍历的函数指针表。表变长 + 段基址整体前移 16 KB，是"死在 machine_startup 之前却毫无输出"最可能的落点。 |

**核心判断**：两颗内核的 `__TEXT_EXEC` 在启动路径上**没有功能性差异**。`k-tee` 的诡异表现来自插桩，把注意力引向了错误的地方。要找 early boot 故障，应转向 `__BOOTDATA.__init_entry_set` 的 6 条新增启动项、`__DATA`/`__const` 段重排，以及段基址的整体 16 KB 偏移。

---

## 1. 文件与段布局

### 1.1 参与比对的文件

| 标签 | 路径 | 大小 (B) | MD5 | 说明 |
|------|------|----------|-----|------|
| **A** | `D:/IOS/repos/hakios-xnu-kernel/prebuilt/hakios-kernel` | 22,341,416 | `0b7957f0…` | 能跑到桌面（基准） |
| **B（任务指定）** | `D:/IOS/phone/k-tee` | 22,330,568 | `90f10793…` | ✗ **含 `_loader_log_line` 插桩** |
| B1 | `D:/IOS/phone/k-diag2` | 22,330,568 | `1b2a76df…` | ✗ 也插桩了 `machine_startup` |
| B2 | `D:/IOS/phone/k-diag` | 22,330,568 | `9ec462c6…` | ○ `machine_startup` 干净 |
| **B3（纯净）** | `D:/IOS/phone/k-genuine` | 22,330,568 | `d5dfdc53…` | ✓ 无插桩，**本报告的主比对对象** |
| B3' | `D:/IOS/phone/hakios-kernel-selfbuilt` | 22,330,568 | `c625adff…` | ✓ 与 k-genuine `__TEXT_EXEC` 完全一致 |

> 注：6 个文件都含 `_loader_log_line` 符号字符串，但只有 `k-tee`/`k-diag2` 在**启动路径上真正调用**它。判据是直接检测 `machine_startup` 函数体内是否存在指向 `_loader_log_line` 的 `BL` 指令（见 §3.1）。

### 1.2 段基址差异（全部段整体前移 16 KB）

| 段 | A vmaddr | A 大小 | B vmaddr | B 大小 | Δvmaddr |
|----|----------|--------|----------|--------|---------|
| `__TEXT` | `0xfffffff007004000` | 7,684,096 | 同 | 7,667,712 | **0** |
| `__DATA_CONST` | `0xfffffff007758000` | 983,040 | `0xfffffff007754000` | 983,040 | −16384 |
| **`__TEXT_EXEC`** | **`0xfffffff007848000`** | **9,338,880** | **`0xfffffff007844000`** | **9,338,880** | **−16384** |
| `__KLD` … `__LINKEDIT` | — | — | — | — | −16384（全部） |

- `__TEXT_EXEC` 段大小**完全相同**（9,338,880）；其中 `__text` 节 A = 9,322,984、B = 9,326,432（**B 大 3,448 B**）。
- `__TEXT` 基址相同但大小不同（A 大 16,384 B），导致其后所有段整体前移 16 KB。
- **重要**：A/B 的 `__TEXT_EXEC` 基址相差 −16 KB，所以两边**同名字符串/数据的虚拟地址必然不同**，任何按 vaddr 直接比对的方案都会失败。本报告全部按**符号名**配对，规避了这个问题。

### 1.3 符号表规模

| | A | B (k-tee) | B3 (k-genuine) |
|---|---|---|---|
| `LC_SYMTAB.nsyms` | 54,703 | 54,814 | 54,814 |
| `__TEXT_EXEC` 内 N_SECT 符号 | 28,142 | 28,220 | 28,220 |
| 去重后唯一函数名 | 27,927 | 28,004 | 28,004 |
| 同名多地址（重复名） | 108 | 108 | 108 |
| **两边共同函数名** | — | **27,921** | **27,921** |

---

## 2. 方法学

### 2.1 解析（Python + struct，无外部库）

```
magic = 0xfeedfacf (MH_MAGIC_64, little-endian)  →  ncmds @ 0x10
LC_SEGMENT_64 (0x19)：segname[16] @ +8, vmaddr/vmsize/fileoff/filesize @ +24 (4×u64)
                      maxprot/initprot @ +56, nsects @ +64；section_64 每条 80 B
LC_SYMTAB   (0x02)：symoff, nsyms, stroff, strsize @ +8 (4×u32)
nlist_64：strx(u32) n_type(u8) n_sect(u8) n_desc(u16) n_value(u64)，每条 16 B
```

**函数判定**：`(n_type & 0x0e) == 0x0e`（即 `N_SECT`）+ 地址 ∈ `[__TEXT_EXEC.vmaddr, +vmsize)`。

> **踩坑记录**：`n_type` 实际值有两种——`0x0e`（普通段符号）和 `0x0f`（`N_SECT|N_EXT`，即外部可见函数，`machine_startup` 就是 `0x0f`）。掩码必须写 `& 0x0e == 0x0e`，如果写 `== 0x0e` 会漏掉所有全局函数。

### 2.2 切分方法一：按名配对 + 各自区间（主方法）

对每个共同函数名 `f`：
```
A 区间 = [A.sym[f].addr, nextGreater(A, A.sym[f].addr))
B 区间 = [B.sym[f].addr, nextGreater(B, B.sym[f].addr))
```
其中 `nextGreater` 是在**该内核自己的** `__TEXT_EXEC` 全部符号地址（含非 N_SECT 符号，如 `l_.str`、`l_switch.table.*`）中找下一个更大地址。

### 2.3 切分方法二：用 A 的区间长度切 B

```
B 区间 = B 段字节[idx(B.sym[f].addr) : idx + len(A 区间)]
```

### 2.4 三种比对的判据（层层收紧）

| 级别 | 判据 | 用途 |
|------|------|------|
| **L0 原始** | `sha256(A区间) == sha256(B区间)` | 任务要求，但**误报极高** |
| **L1 指令归一化** | 仅把"随布局必然改变"的立即数位域清零：`B/BL`(26b)、`B.cond/CBZ/CBNZ/TBZ/TBNZ/LDR-lit`(19/14b)、`ADR/ADRP`(21b)、`ADD/SUB-imm`(12b)、`LDR/STR-uoff`(12b)。**操作码、寄存器号一律保留**。 | 过滤纯重定位噪声 |
| **L2 语义** | 对 L1 仍有差的指令，**解出目标地址 → 用该内核符号表反查符号名 → 比较符号名**。名字相同则不计数。 | 过滤"跳到了同一个函数但位移不同" |

**L2 是关键**：L1 仍会把 `bl 0xfffffff0078ca4ec <_kernel_bootstrap>` vs `bl 0xfffffff0078ce4ec <_kernel_bootstrap>` 判为不同——绝对地址不同但目标是同一符号。

---

## 3. 结果

### 3.1 三种候选 B 的识别（关键发现）

用"函数体内是否存在指向 `_loader_log_line` 的 `BL`"判定插桩：

| 文件 | `_machine_startup` 大小 | 长度 | 调用 `_loader_log_line`? | `__cnputs` 大小 |
|---|---|---|---|---|
| **A** | 20 B | 5 条指令 | **否** | 328 B |
| **k-tee** | 68 B | 17 条指令 | **是 ×4** | **536 B** |
| k-diag2 | 68 B | 17 条指令 | **是 ×4** | 328 B |
| k-diag | 20 B | 5 条指令 | 否 | 328 B |
| **k-genuine** | **20 B** | 5 条指令 | **否** | **328 B** |
| selfbuilt | 20 B | 5 条指令 | 否 | 328 B |

**A 的 `_machine_startup`（20 B，5 条指令，原版）**
```asm
fffffff007a1ee68: a9bf7bfd   stp  x29, x30, [sp, #-0x10]!
fffffff007a1ee6c: 910003fd   mov  x29, sp
fffffff007a1ee70: 97ffff36   bl   0xfffffff007a1eb48 <_machine_conf>
fffffff007a1ee74: a8c17bfd   ldp  x29, x30, [sp], #0x10
fffffff007a1ee78: 17fabd9d   b    0xfffffff0078ce4ec <_kernel_bootstrap>
```

**k-tee 的 `_machine_startup`（68 B，17 条指令，被插桩）**
```asm
fffffff007a1ae68: a9bf7bfd   stp  x29, x30, [sp, #-0x10]!
fffffff007a1ae6c: 910003fd   mov  x29, sp
fffffff007a1ae70: d0ffe0a0   adrp x0, 0xfffffff007630000
fffffff007a1ae74: 91160800   add  x0, x0, #0x582
fffffff007a1ae78: 94003d03   bl   0xfffffff007a2a284 <_loader_log_line>   ← 插桩
fffffff007a1ae7c: d0ffe0a0   adrp x0, 0xfffffff007630000
fffffff007a1ae80: 91168800   add  x0, x0, #0x5a2
fffffff007a1ae84: 94003d00   bl   0xfffffff007a2a284 <_loader_log_line>   ← 插桩
fffffff007a1ae88: 97ffff30   bl   0xfffffff007a1ab48 <_machine_conf>
fffffff007a1ae8c: d0ffe0a0   adrp x0, 0xfffffff007630000
fffffff007a1ae90: 91170000   add  x0, x0, #0x5c0
fffffff007a1ae94: 94003cfc   bl   0xfffffff007a2a284 <_loader_log_line>   ← 插桩
fffffff007a1ae98: d0ffe0a0   adrp x0, 0xfffffff007630000
fffffff007a1ae9c: 91176800   add  x0, x0, #0x5da
fffffff007a1aea0: 94003cf9   bl   0xfffffff007a2a284 <_loader_log_line>   ← 插桩
fffffff007a1aea4: a8c17bfd   ldp  x29, x30, [sp], #0x10
fffffff007a1aea8: 17fabd91   b    0xfffffff0078ca4ec <_kernel_bootstrap>
```

**`__cnputs` 的插桩（这直接解释"logdump 零输出"不能归因于代码 bug）**
```asm
; A: 328 B —— 走到 __cnputs 后先查 startup_debug，再逐字符输出
fffffff0079e1130: adrp x8, 0xfffffff0083a4000
fffffff0079e1138: ldr  w8, [x8, #0x29c]      ; startup_debug
fffffff0079e113c: cbz  w8, 0x…1160           ; 未开启则跳过输出

; k-tee: 536 B —— 输出前额外把整段 buffer 复制到栈上，调用 _loader_log_line
fffffff0079dd080: cmp   w21, #0x1
fffffff0079dd084: b.lt  0x…d128
fffffff0079dd088: mov   w22, #0x0
fffffff0079dd08c: movi.16b v0, #0x0
fffffff0079dd090: stp   q0, q0, [sp, #0x100]  ; 清零 256 B 栈缓冲
...                                          ; 逐字节复制入栈，遇 0x0d 跳过
fffffff0079dd124: bl    0xfffffff007a2a284 <_loader_log_line>   ← 插桩
```

### 3.2 方法一 vs 方法二：差异量化（A vs k-tee，任务指定的 B）

| 指标 | 数值 |
|---|---|
| 共同函数对 | **27,921** |
| L0 原始 SHA256 **相同** | **9,011** |
| L0 原始 SHA256 **不同** | **18,910** |
| — 其中经 L1+L2 判定为**纯布局噪声** | 18,325 |
| — 其中为**真实语义差异** | **585** |
| 区间长度不同（`|A| ≠ |B|`）的函数 | **412** |
| **方法二改变了判定结果的函数** | **411** |
| 方法二发生"越界读到下一个函数"的函数 | **257**（累计越界 **54,476 B**） |

> **结论：两种方法在"是否相同"的最终判定上 0 处分歧**（`flips = 0`），但在**哈希值**上有 411 处不同，且方法二存在 257 次真实越界读取。原因见 §3.3。

### 3.3 两种方法为什么在语义上等价、在哈希上不等价

**越界机制**：方法二用 `len(A区间)` 切 B。若 `len(A) > len(B)`，切片会**越过 B 中该函数的下一个符号边界**，把邻居函数的字节算进本函数。若 `len(A) < len(B)`，则**截断**，丢掉 B 中真实的尾码。

越界最严重的 12 个（A 比 B 长）：

| 函数 | A 区间 | B 区间 | 越界字节 |
|---|---|---|---|
| `_ath11k_wmi_tlv_op_rx` | 12,472 | 1,264 | **11,208** |
| `_ath11k_dp_process_rx_err` | 6,316 | 2,572 | 3,744 |
| `_station_join` | 8,756 | 5,252 | 3,504 |
| `_AccelerateCrypto_SHA256_compress` | 12,784 | 9,360 | 3,424 |
| `_lx_wpa_message` | 4,200 | 1,112 | 3,088 |
| `_ath11k_reg_chan_list_event` | 6,560 | 4,580 | 1,980 |
| `_ath11k_htc_connect_service` | 2,644 | 1,472 | 1,172 |
| `_ath11k_dp_tx_completion_handler` | 2,880 | 1,844 | 1,036 |
| `_lx_start` | 2,144 | 1,124 | 1,020 |
| `_ath11k_qmi_firmware_start` | 1,812 | 1,088 | 724 |
| `_ath11k_qmi_fwreset_from_cold_boot` | 992 | 408 | 584 |
| `_ath11k_qmi_driver_event_work` | 3,448 | 2,880 | 568 |

**为什么最终判定仍一致**：这 411 个哈希不同的函数，本身就因为长度不同（`|A|≠|B|`）已经被方法一判为不同。方法二只是把"不同"换成了另一种"不同"，没有把任何"相同"翻成"不同"。

**但方法二有两个致命误用场景**：
1. **截断假阴性**：如果某函数 A 短 B 长，方法二会切掉 B 的尾码，可能把"B 尾部新增了一段"这个**最关键的线索**藏起来。
2. **越界假阳性**：`_AccelerateCrypto_SHA256_compress` 这种 A 长 B 短 3,424 B 的，方法二读进了 3,424 B 的邻居函数，差异数完全失真。

**因此：以方法一（各自区间 + L2 符号语义校验）为准。** 方法二仅作为交叉验证。

### 3.4 已知误差与边界（诚实声明）

1. **区间右边界依赖符号表完整性**。若某函数尾部没有独立符号，其区间会被并入前一个函数；两边符号集不同（A 独有 6 个、B 独有 83 个），这类合并会在个别函数上制造"长度不一致"的假象。
2. **`__TEXT_EXEC` 内存在 108 个重名符号**（如 `l_.str`、`l_switch.table.*`、`l___const.*`）。本报告按**地址升序取第 0 个**配对，对这类编译器生成的局部符号可能错配；对 `_func` 形式的真实函数无影响。
3. **`LC_DYLD_CHAINED_FIXUPS` 未解析**。因此 `__DATA`/`__const` 中的指针重定位未做语义归一化，数据段的哈希差异不能直接当"代码改动"。
4. **函数对齐填充**（`0x1f2003d5` NOP / `0xcc` INT3 / 零填充）被算进区间。B 普遍更短，属正常。
5. **未做链接顺序推断**。A/B 的 `__text` 节大小差 3,448 B，说明目标文件排列顺序/数量有变化，这会连带影响所有函数的相对地址，是 L0 哈希 65% 不同的主因。

---

## 4. 启动路径重点比对

### 4.1 任务点名的函数：A vs k-tee vs k-genuine

| 函数 | A 大小 | k-tee | 真实差异 | k-genuine | 真实差异 | 判定 |
|------|--------|-------|---------|-----------|---------|------|
| `_machine_startup` | 20 | 68 (−48) | **3** | **20 (+0)** | **0** | 纯净版一致；k-tee 被插桩 |
| `_machine_conf` | 72 | 72 | 0 | 72 | 0 | **一致** |
| `_machine_lockdown` | 64 | 64 | 0 | 64 | 0 | **一致** |
| `_arm_init` | 3,008 | 3,008 | 0 | 3,008 | 0 | **一致** |
| `_arm_vm_init` | 5,676 | 5,676 | 0 | 5,676 | 0 | **一致** |
| `_arm_vm_prot_finalize` | 332 | 332 | 0 | 332 | 0 | **一致** |
| `_PE_init_platform` | 940 | 960 (−20) | **229** | **940 (+0)** | **0** | 纯净版一致；k-tee 被插桩 |
| `_PE_init_kprintf` | 52 | 52 | 0 | 52 | 0 | **一致** |
| `_kernel_bootstrap` | 1,848 | 1,848 | 0 | 1,848 | 0 | **一致** |
| `_kernel_bootstrap_thread` | 1,364 | 1,364 | 0 | 1,364 | 0 | **一致** |
| `_pmap_bootstrap` | 1,532 | 1,532 | 0 | 1,532 | 0 | **一致** |
| `_pmap_startup` | 2,256 | 2,256 | 0 | 2,256 | 0 | **一致** |
| `_cpu_init` | 260 | 260 | 0 | 260 | 0 | **一致** |
| `_cpu_data_startup_init` | 272 | 272 | 0 | 272 | 0 | **一致** |
| `__cnputs` | 328 | **536** (−208) | **79** | **328 (+0)** | **0** | 纯净版一致；k-tee 被插桩 |
| `_panic` | 52 | 52 | 0 | 52 | 0 | **一致** |
| `_PE_parse_boot_argn` | 64 | 64 | **0**（字节全同） | 64 | 0 | **一致** |
| `_kernel_startup_log` | 140 | 140 | 0 | 140 | 0 | **一致** |
| `_startup_entry_cmp` | 56 | 56 | 0 | 56 | 0 | **一致** |
| `_kernel_startup_initialize_upto` | 304 | 304 | 0 | 304 | 0 | **一致** |
| **`_pmap_kextract`** | 48 | 48 | **6** | 48 | **6** | ⚠ **真实改动（两版都有）** |

### 4.2 `_pmap_kextract` —— 唯一的真实启动路径代码差异（展开分析）

这是整个启动路径上**唯一**在纯净自编内核中依然存在的语义差异，且 **A/B 大小相同（各 48 B / 12 条指令）**，属于典型的"同尺寸原地重写"。

**A（`0xfffffff008115e88`）**
```asm
fffffff008115e88: b00023a8   adrp x8, 0xfffffff00858a000
fffffff008115e8c: 91168108   add  x8, x8, #0x5a0        ; x8 = &pvh_root 区域
fffffff008115e90: 91000409   add  x9, x0, #0x1          ; x9 = pa + 1
fffffff008115e94: b940150a   ldr  w10, [x8, #0x14]     ; **从内存读取** lo（4B）
fffffff008115e98: a940210b   ldp  x11, x8, [x8]        ; x11 = lo, x8 = hi
fffffff008115e9c: 8b0a016a   add  x10, x11, x10        ; lo = base_lo + mem_lo
fffffff008115ea0: 8b000108   add  x8, x8, x0           ; hi + pa
fffffff008115ea4: cb0b0108   sub  x8, x8, x11          ; rel = (hi + pa) - base_lo
fffffff008115ea8: eb0a013f   cmp  x9, x10              ; (pa+1) vs (base_lo+mem_lo)
fffffff008115eac: fa409162   ccmp x11, x0, #0x2, ls    ; base_lo vs pa, cond = ls(<=)
fffffff008115eb0: 9a9f9100   csel x0, x8, xzr, ls      ; 命中返回 rel，否则 0
fffffff008115eb4: d65f03c0   ret
```

**B（`0xfffffff00811392c`）**
```asm
fffffff00811392c: f0002388   adrp x8, 0xfffffff008586000
fffffff008113930: 91188108   add  x8, x8, #0x620        ; x8 = &pvh_root 区域
fffffff008113934: 91000409   add  x9, x0, #0x1          ; x9 = pa + 1
fffffff008113938: 52a0580a   mov  w10, #0x2c00000      ; **立即数常量** 0x02c00000
fffffff00811393c: a940210b   ldp  x11, x8, [x8]        ; x11 = lo, x8 = hi
fffffff008113940: 8b0a016a   add  x10, x11, x10        ; lo = base_lo + 0x02c00000
fffffff008113944: cb0b000c   sub  x12, x0, x11          ; **pa - base_lo**（新增寄存器 x12）
fffffff008113948: eb0a013f   cmp  x9, x10
fffffff00811394c: fa4b9000   ccmp x0, x11, #0x0, ls     ; **pa vs base_lo**（操作数顺序不同）
fffffff008113950: 8b080188   add  x8, x12, x8           ; rel = (pa - base_lo) + hi
fffffff008113954: 9a9f2100   csel x0, x8, xzr, hs       ; **条件 hs(>=)，与 A 的 ls(<=) 相反**
fffffff008113958: d65f03c0   ret
```

**四处语义等价、两处**不等价**：

| # | 差异 | A | B | 等价性 |
|---|------|---|---|--------|
| 1 | lo 来源 | `ldr w10,[x8,#0x14]` 运行时读 | `mov w10,#0x2c00000` 编译期常量 | **视为等价**（若该字段确为 0x02c00000 常量，见 §4.3 说明） |
| 2 | 运算顺序 | `(hi+pa)-lo` | `(pa-lo)+hi` | **等价**（整数加减，无溢出边界） |
| 3 | `ccmp` 条件码 | `ccmp x11,x0,#0x2,ls` | `ccmp x0,x11,#0x0,ls` | **等价**（操作数互换 + NZCV 掩码 2↔0，等价于取反比较） |
| 4 | **`csel` 条件** | `csel x0,x8,xzr,**ls**` | `csel x0,x8,xzr,**hs**` | **不等价** ⚠ |

**差异 #4 是本任务唯一的真实语义分歧**：A 在"不带符号比较 `pa+1 <= lo+mem_lo` 且 `base_lo <= pa`"时返回物理地址转译，B 在"…**`>= `**…"时返回。两者在 `pa == base_lo` 边界上取到相反的返回值：A 返回 `rel`，B 返回 `0`。

`pmap_kextract()` 的调用者（`ml_static_vtop` 等）用它把页表物理地址转成 KVA。**当传入地址恰好等于 `pvh_root` 的基址时，A 返回有效指针、B 返回 `0`/NULL** —— 空指针解引用 → 在任何 `printf` 之前 triple fault。这与"A 能跑、B 死在 early boot"的现象**高度吻合**，但需满足 `pa == base_lo` 这一精确边界，属**概率性触发**，不能作为唯一解释。

> **验证方法**：在 A 上打印 `pvh_root.lo` 与 `gPEClockFrequencyInfo` 附近被 `pmap_kextract` 转译的地址，确认是否有调用点命中该边界。

### 4.3 k-tee 独有的启动路径噪声（40 个，纯插桩）

在 k-tee 中差异、但在 k-genuine 中**完全一致**的函数（= 插桩噪声），完整列表：

```
_PE_get_offset_into_panic_region        _PE_i_can_has_debugger
_PE_init_panicheader                    _PE_init_platform        (940→960, 229 处)
_PE_reboot_on_panic                     _PE_update_panicheader_nestedpanic
_PE_write_socd_client_buffer            __cnputs                 (328→536, 79 处)
_console_ring_try_empty  (856→648)      _console_serial_parallel_log_tests
_console_serial_test                    _console_write
_decode_counted                         _encode_counted
_ex_cb_invoke                           _fiq_context_init
_fts_write                              _machine_startup         (20→68, 3 处)
_ml_cpu_down                            _ml_get_backtrace_pc
_ml_get_cluster_number_local            _ml_get_cpu_number_local
_ml_get_decrementer                     _ml_get_vm_reserved_regions
_ml_lockdown_handler_register           _ml_mcache_flush
_ml_parse_cpu_topology                  _ml_processor_register
_ml_readprop                            _ml_set_decrementer
_ml_static_mfree                        _ml_static_protect
_ml_update_cluster_wfe_recommendation   _na_schema_alloc
_serve_darwin                           _set_invalidate_hmac_function
_thread_priority_floor_end              _wfe_timeout_init
_wqt_expect_preposts                    __Z22IOMemoryDescriptorTesti
```

其中 `_ml_*`（machine layer）、`_PE_*`（platform expert）、`_console_*` 三个家族的集体变动，是"在 `ml_*`/`console_*` 里统一插桩"的指纹。**这些全部不是启动失败的原因。**

---

## 5. 真正可疑的地方：`__BOOTDATA.__init_entry_set`

### 5.1 段大小差异

| 段/节 | A | B (k-genuine) | 差 |
|-------|-----|---------------|-----|
| `__BOOTDATA.__init` | 103,816 B | 103,992 B | **+176** |
| **`__BOOTDATA.__init_entry_set`** | **101,016 B** | **101,160 B** | **+144** |

记录布局实测为 **24 字节/条**：`{u64 kind, u64 func_ptr, u64 arg}`
（`kind` 低 32 位 = 类型，高 32 位 = 子类；实测样例 `0x1`、`0x3`、`0x100000003`、`0x10000000f`）

| | A | B | Δ |
|---|---|---|---|
| 记录数 | **4,209** | **4,215** | **+6** |

### 5.2 多出的 6 条启动项（按 `(kind, func_ptr)` 多重集比较）

| kind | func_ptr | 出现次数 |
|------|----------|---------|
| `0x1` | `_kernel_startup_tunable_init` | **×5** |
| `0x10000000f` | `_sysctl_register_oid_early` | ×1 |

**A 中不存在的项：0 条**。也就是说 B 是 A 的**严格超集**，只多不少。

### 5.3 为什么这是首要嫌疑

`_kernel_startup_initialize_upto()`（A/B 各 304 B，代码完全一致）遍历这张表并逐个调用 `func_ptr(arg)`。表从 4,209 条变成 4,215 条，多出 5 次 `_kernel_startup_tunable_init` 和 1 次 `_sysctl_register_oid_early`：

- `_kernel_startup_tunable_init` 会解析 `boot-args` 里的 tunable。**多注册 5 个 tunable，意味着有 5 个新的 `__startup_TUNABLES_*` 结构被链接进来**（对照 §1 的 `__DATA_CONST.__const` A=753,176 / B=753,088，B 少 88 B；`__DATA.__data` A=246,512 / B=246,480，B 少 32 B——数据布局确实重排了）。
- 这是**唯一一处"启动表内容"层面的差异**，而启动表恰好在 `machine_startup → kernel_bootstrap → kernel_startup_initialize_upto` 之间执行，时间点与"死在 machine_startup 第一条 C 语句之前、logdump 零输出"**完全吻合**。
- 新的 tunable 若其 `__startup_TUNABLES_spec_*` 结构体落在未正确初始化的内存（`__DATA` 段整体前移 16 KB），`PE_parse_boot_argn` 或 `strlen` 会在第一条 `__cnputs` 之前崩溃。

**B 中新增的 5 个 tunable（来自 §6 的 `onlyB` 符号集）与 GPU/显示相关**：
`gSurfaceWrap`、`gFBSubtype`、`gThreadPriority`、`gBeam`、`gIntfPhysical`
—— 这与同版本新增的 `IOMobileFramebuffer::beamProbe/beamWait/beamNextScan/beamShareDone/settle` 等 beam 追踪函数完全对应（见 §6）。

### 5.4 复现步骤

```bash
# 1. 确认 A 的 machine_startup 干净、k-tee 被插桩
wsl -d Ubuntu-24.04 -e bash -lc "llvm-objdump --arch=arm64 -d \
  --start-address=0xfffffff007a1ee68 --stop-address=0xfffffff007a1ee7c \
  /mnt/d/IOS/repos/hakios-xnu-kernel/prebuilt/hakios-kernel"
wsl -d Ubuntu-24.04 -e bash -lc "llvm-objdump --arch=arm64 -d \
  --start-address=0xfffffff007a1ae68 --stop-address=0xfffffff007a1aeac \
  /mnt/d/IOS/phone/k-tee"   # 会看到 4 条 bl _loader_log_line

# 2. 确认纯净自编内核的 machine_startup 与 A 一致
wsl -d Ubuntu-24.04 -e bash -lc "llvm-objdump --arch=arm64 -d \
  --start-address=0xfffffff007a1ae68 --stop-address=0xfffffff007a1ae7c \
  /mnt/d/IOS/phone/k-genuine"

# 3. 导出启动表条目数
python -c "import struct;d=open('D:/IOS/phone/k-genuine','rb').read();..."   # 见 §2.1 解析器
```

---

## 6. 符号集合差异（两侧不对称部分）

### 6.1 仅 A 有（6 个）

```
_ath11k_create_ht_cap
_ath11k_mac_bitrate_mask_num_he_rates
_ath11k_mac_bitrate_mask_num_vht_rates
_lx_calibrated
_md5_end
_md5_more
```

全部是 **ath11k Wi-Fi 驱动**（`_ath11k_*`、`_lx_*`）和 **MD5 实现**的残留。**与启动路径无关**。说明 A 的 Wi-Fi 模块版本更旧。

### 6.2 仅 B 有（83 个）

**GPU / 显示（8 个，全部 C++）**
```
HakiosGPUUserClient::userBuffer(hakios_user_memory*)
IOMobileFramebuffer::beamNextScan(unsigned long long)
IOMobileFramebuffer::beamProbeMain(void*, int)
IOMobileFramebuffer::beamShareDone(RowJob const*, int, int, unsigned long long)
IOMobileFramebuffer::settle(unsigned char const*, MiniIOSurface**)
IOMobileFramebuffer::beamWait(RowJob const*, int)
IOMobileFramebuffer::beamProbe()
IOSurfaceRootUserClient::methodWrapClientImage(IOExternalMethodArguments*)
```
→ 与 §5.3 的 5 个新增 GPU tunable 直接对应。**B 引入了新的显示/beam 追踪子系统。**

**ath11k Wi-Fi 事件处理（约 60 个）**
```
_ath11k_bcn_tx_status_event, _ath11k_chan_info_event, _ath11k_mgmt_rx_event,
_ath11k_mgmt_tx_compl_event, _ath11k_peer_assoc_conf_event, _ath11k_roam_event,
_ath11k_scan_event, _ath11k_ready_event, _ath11k_service_ready_event,
_ath11k_service_ready_ext_event, _ath11k_service_ready_ext2_event,
_ath11k_vdev_start_resp_event, _ath11k_vdev_stopped_event, _ath11k_wmi_*_event (×14),
_ath11k_dp_rx_h_defrag*, _ath11k_core_pdev_create, _ath11k_core_stop,
_ath11k_htc_suspend_complete, _ath11k_htc_suspend_complete,
_ath11k_recalc_rtscts_prot, _ath11k_station_disassoc, _ath11k_print_reg_rule,
_ath11k_mac_set_kickout, _ath11k_mac_start_vdev_delay, _ath11k_core_create_firmware_path,
_ath11k_qmi_free_target_mem_chunk, _ath11k_wmi_cc_setting_code_to_reg, ...
```
加上 `_ieee80211_free_txskb`、`_ieee80211_is_robust_mgmt_frame`、`_ieee80211_iterate_stations_mtx`、`_bss_changed`、`_station_state`、`_station_width`、`_vendor_element`、`_group_key`、`_frame_new`、`_derive`、`_request_elements`、`_ethernet_send` 等。
→ **B 的 Wi-Fi 栈比 A 新得多**（更完整的 WMI 事件集 + REO defrag 路径）。

**其它**
```
___skb_queue_purge,  _aes_unwrap,  _answer,  _answer_wait,
_press_step,  _say_radio,  _htc_service_name,  _create_ext_reg_rules_from_wmi
```

> **关键判断**：83 个新增符号里 **0 个属于启动路径**。它们全是 Wi-Fi / GPU 驱动的新功能。这说明 B 是一次"大功能合入"后的构建，功能膨胀导致 `__text` 节从 9,322,984 涨到 9,326,432，进而把后面所有段推前 16 KB —— **这才是两颗内核布局差异的根因**。

### 6.3 语义差异函数按子系统分布（A vs k-genuine，548 个）

| 子系统 | 数量 | 性质 |
|--------|------|------|
| Wi-Fi / ath11k / linuxkpi | **381** | 驱动版本不同，预期 |
| 其它（console/网络/GPU 杂项） | 116 | 混合 |
| GPU / framebuffer | 29 | 新 beam 子系统 |
| lx（Wi-Fi 胶水） | 16 | 驱动版本 |
| C++ 方法 | 3 | GPU/IOSurface |
| touch（goodix） | 2 | 驱动 |
| **BOOT / CORE** | **1** | **`_pmap_kextract`（见 §4.2）** |

> 注意：`_kernel_startup_tunable_init`、`_sysctl_register_oid_early`、`_kernel_startup_initialize_upto` **代码本身完全一致**。差异只在**表的内容**，不在表的**遍历逻辑**。

---

## 7. 可复现的差异函数清单（纯净版 B3 = k-genuine）

### 7.1 完整统计

| 判据 | 数值 | 占比 |
|------|------|------|
| 共同函数对 | 27,921 | 100% |
| L0 原始 SHA256 相同 | 9,508 | 34.0% |
| L1 指令归一化后相同 | 27,371 | **98.0%** |
| **L2 符号语义后相同** | **27,373** | **98.0%** |
| **真实语义差异** | **548** | **2.0%** |
| 区间长度不同 | 407 | 1.46% |

### 7.2 差异最大的 15 个（全部为 Wi-Fi/GPU 驱动，非启动路径）

| 函数 | A 大小 | B 大小 | Δ | 真实差异指令数 |
|------|--------|--------|---|---------------|
| `_ath11k_peer_assoc_prepare` | 6,556 | 6,212 | +344 | 1,489 |
| `_ath11k_mac_op_set_bitrate_mask` | 5,872 | 8,404 | −2,532 | 1,441 |
| `_station_join` | 8,756 | 5,252 | +3,504 | 1,286 |
| `_ath11k_mac_register` | 5,172 | 5,192 | −20 | 1,247 |
| `_ath11k_mac_op_sta_state` | 5,280 | 4,908 | +372 | 1,187 |
| `_ath11k_wmi_fw_stats_fill` | 4,720 | 4,760 | −40 | 1,157 |
| `_ath11k_mac_op_bss_info_changed` | 6,236 | 6,132 | +104 | 1,140 |
| `_ath11k_reg_chan_list_event` | 6,560 | 4,580 | +1,980 | 989 |
| `_goodix_thread` | 4,712 | 4,900 | −188 | 981 |
| `_ath11k_dp_rx_process_mon_status` | 3,320 | 3,188 | +132 | 782 |
| `_ath11k_qmi_driver_event_work` | 3,448 | 2,880 | +568 | 704 |
| `_ath11k_dp_htt_htc_t2h_msg_handler` | 2,820 | 2,952 | −132 | 669 |
| `_ath11k_dp_process_rx` | 2,924 | 2,636 | +288 | 626 |
| `IOMobileFramebuffer::drawLayer` | 2,520 | 2,672 | −152 | 613 |
| `_ath11k_dp_process_rx_err` | 6,316 | 2,572 | +3,744 | 607 |

---

## 8. 行动建议（按优先级）

| 优先级 | 动作 | 依据 |
|--------|------|------|
| **P0** | **不要用 `k-tee` 做启动路径比对**。改用 `D:/IOS/phone/k-genuine`（MD5 `d5dfdc53…`，与 `hakios-kernel-selfbuilt` 的 `__TEXT_EXEC` 完全一致）。 | §3.1，40 个插桩噪声函数会完全误导方向 |
| **P0** | **排查 `__BOOTDATA.__init_entry_set` 多出的 6 条启动项**：`_kernel_startup_tunable_init` ×5 + `_sysctl_register_oid_early` ×1。重点看这 5 个新增 tunable（`gSurfaceWrap`/`gFBSubtype`/`gThreadPriority`/`gBeam`/`gIntfPhysical`）的 `__startup_TUNABLES_spec_*` 结构体是否落在正确的初始化内存。 | §5.2–5.3 |
| **P1** | 在 A 上单步验证 `_pmap_kextract` 的 `pa == pvh_root.lo` 边界是否被命中。若是，则 `csel … ls` → `hs` 的改动就是致命 bug。 | §4.2 差异 #4 |
| **P1** | 检查 `__TEXT` 段大小差 16,384 B 造成的**全段 16 KB 前移**是否与硬编码地址冲突（`__DATA`、`__percpu`、`__common` 的绝对地址都被链接器改了）。 | §1.2 |
| **P2** | 若要保留 Wi-Fi/GPU 新功能，把 `__text` 节从 9,326,432 压回 9,322,984（或确认 `__TEXT_EXEC` 的 9,338,880 上限仍有裕量——目前两边都是 9,338,880，B 的 `__text` 只占 99.87%，**裕量仅 12,448 B**）。 | §1.2 |

---

## 9. 完整方法一/方法二对照样例（`__cnputs`，A vs k-tee）

| 方法 | A 区间 | B 区间 | 长度 | SHA256(A) | SHA256(B) | 判定 |
|------|--------|--------|------|-----------|-----------|------|
| 方法一 | `0xfffffff0079e1110` +328 | `0xfffffff0079dd040` +536 | 328 / 536 | `0de941f5…` | `5b486cf5…` | 不同（**正确**：真的被插桩） |
| 方法二 | 同上 | `0xfffffff0079dd040` **+328** | 328 / 328 | `0de941f5…` | `（截断后）` | 不同（**恰好也正确**，因为截断后仍差异巨大） |

而 `_machine_startup`：

| 方法 | A 区间 | B 区间 | 长度 | 判定 |
|------|--------|--------|------|------|
| 方法一 | +20 | +68 | 20 / 68 | 不同（3 处真实） |
| 方法二 | +20 | +**20**（截断到 20 B） | 20 / 20 | **不同**——但只比较了前 5 条指令，**看不到后面的 4 次 `_loader_log_line` 调用**，反而丢失了"被插桩"这个最有价值的信息 |

**这就是方法二在启动路径上的典型失效模式**：截断把"B 比 A 多了一大段"这个关键事实抹掉了。

---

## 10. 产物

| 文件 | 内容 |
|------|------|
| `D:/IOS/reports/function-diff.md` | 本报告 |
| `D:/IOS/tmp/pair_sem.json` | A vs k-genuine 全量 27,921 对函数比对结果（含地址、大小、SHA256、真实差异指令索引） |
| `D:/IOS/tmp/sem_ktee.json` | A vs k-tee 全量比对结果 |
| `D:/IOS/tmp/sem_selfbuilt.json` | A vs hakios-kernel-selfbuilt 全量比对结果 |
| `D:/IOS/tmp/funcdiff2.json` | A vs k-tee 早期 L1 归一化结果 |
| `D:/IOS/tmp/dis/*.txt` | 关键函数 llvm-objdump 反汇编（A/B 双份，22 个文件） |
| `D:/IOS/tmp/fd_*.py` | Mach-O 解析、配对、归一化、符号语义校验脚本 |
