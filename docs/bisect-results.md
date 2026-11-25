# 启动二分结果：内核一直在跑，日志通道断了

> 2026-11-26 凌晨，用 `loader_checkpoint()` 逐段二分，全部实测。
> **推翻了前面三份文档的主要结论**：内核没有死在早期，它跑完了整个启动链。

---

## 0 最重要的一句

**内核执行完全正常。坏的是「内核把日志交给 loader」这条通道。**

前面把「零输出」解释成「内核死了」，是错的。

---

## 1 二分方法与结果

`loader_return.c:405` 有现成机制：

```c
void loader_checkpoint(uint64_t n) {
    if (loader_stub_slot[2] == n) loader_jump(2, n);   // 立刻返回 loader
}
```

`fastboot oem run N` 设置 `loader_stub_slot[2] = N`，内核在每条 `loader_checkpoint(N)` 处比对命中就交还控制权。
**这条路完全绕开 printf/日志，是纯汇编层的判断。**

### 测过的检查点

| 位置 | 检查点 | 结果 |
|---|---|---|
| `start.s` 全程 | CP1、2、3、4、5、**6、7、8**（6-8 为本次新增） | ✅ 全部 `checkpoint N 0` |
| `arm_init` 全程 | CP10–21（原有） | ✅ 全部命中 |
| `machine_startup` 入口 | **CP22**（本次新增） | ✅ 命中 |
| `machine_conf` 前/后 | **CP23 / CP24**（本次新增） | ✅ 命中 |
| `kernel_bootstrap` 前 | **CP25**（本次新增） | ✅ 命中 |
| `kernel_bootstrap` 里 `printf(version)` 前 | **CP26**（本次新增） | ✅ 命中 |
| `scale_setup()` 后 | **CP27**（本次新增） | ✅ 命中 |

**内核一路跑到 `kernel_bootstrap()` 内部，`scale_setup()` 之后。**

### 新增检查点的位置

```
osfmk/arm64/start.s
  :979  LOADER_CHECKPOINT 6    APPLY_TUNABLES x12,x13,x14 之前
  :991  LOADER_CHECKPOINT 7    cbnz x21,Ltrampoline 之前
  :1012 LOADER_CHECKPOINT 8    ret 之前（最后一条）

osfmk/arm64/machine_routines.c
  :471  loader_checkpoint(22)  machine_startup 第一行
  :480  loader_checkpoint(23)  machine_conf() 之前
  :482  loader_checkpoint(24)  machine_conf() 之后
  :488  loader_checkpoint(25)  kernel_bootstrap() 之前

osfmk/kern/startup.c
  :432  loader_checkpoint(26)  printf("%s\n", version) 之前
  :436  loader_checkpoint(27)  scale_setup() 之后
```

---

## 2 核心矛盾

`kernel_bootstrap()` 第一行是：

```c
printf("%s\n", version);   /* log kernel version */
```

**CP26 就在这条语句之前，它命中了。** 所以这条 `printf` 一定被执行。

而 `printf` 的必经之路是 `serial_putc_crlf`（`pexpert/arm/pe_kprintf.c:158`）：

```c
static void serial_putc_crlf(char c) {
    if (c == '\n') uart_putc('\r');
    loader_log_putc(c);      // ← 写内核日志
    uart_putc(c);
}
```

**但 `logdump` 分区里零字节。** 说明 `loader_log_putc` 写了没用，或者写了但 loader 读不到。

### 两个候选原因

**H1：loader 找不到日志。**

loader 靠扫描内核内存找 magic `XNULIVE!`（`loader.c:3081`）来确定日志在哪：

```c
for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
    if (*q == KERNEL_LIVE_MAGIC) { kernel_live = (const struct kernel_live *)q; break; }
}
```

**如果内核还没写出这个 magic，loader 就找不到 `loader_log`，无论里面写了多少都读不到。**

**H2：`loader_log_putc` 自身卡住。**

它的锁是纯自旋（`loader_return.c:105`）：

```c
for (uint32_t tries = 0; !locked && tries < 10000000; tries++) {
    locked = os_atomic_cmpxchg(&loader_log_lock, 0, 1, acquire);
}
```

无持有者标识、无重入保护、无降级。若 `loader_log_lock` 非零，自旋 1000 万次。
上游 `arch/arm64/kernel/loader_return.c:117-144` 已修成 MPIDR 持有者 + 可重入 + 超时降级。

`_loader_log_lock` 地址实测 `0xfffffff0083cb000`（类型 `b` = BSS），在 `__DATA` 段内（`0x8138000 + 0x464000 = 0x859c000`），loader 已 `memset` 清零，所以初值应为 0。

---

## 3 前面文档里被推翻的部分

| 之前写的 | 现在实测 |
|---|---|
| 「死在 `machine_startup` 第一条 C 语句之前」 | **错**。CP22 命中，`machine_startup` 执行了 |
| 「纯汇编阶段就死了」 | **错**。`start.s` 全段 CP1-8 都命中 |
| 「`_cnputs` 零输出所以代码没执行到」 | **错**。执行到了，是日志通道问题 |

那六处 `[HKL] loader_log_line` 检查点确实零输出——但原因是**日志通道本身不通**，不是那些代码没跑。

---

## 4 复跑方法

```bash
# 手机在 loader
cd /d/IOS/repos/hakios-xnu-kernel
fastboot oem ramdisk
FB_TIMEOUT=1800 fastboot stage userland/system.apfs
FB_TIMEOUT=180  fastboot stage /d/IOS/phone/p-cp.bin
fastboot oem run 22          # 或任意检查点号
fastboot getvar result       # 期望: checkpoint 22 0
# 手机掉出 loader 说明内核真崩了；仍在 loader 且 result 是 checkpoint N 0 说明命中
```

判读：

| result | 含义 |
|---|---|
| `checkpoint N 0` | **命中**，内核执行到了 |
| `returned <len> 0` | 跑完没停（该编号不存在或内核正常结束） |
| 手机掉出 loader | 内核在那个检查点之前真崩了 |

**注意**：`oem run` 之后 `result` 不会自动清空。每次测之前先读一次确认不是上一轮的残留。
