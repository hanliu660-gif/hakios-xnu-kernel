# Three lines, and what was in each

The repository had three copies of the work that no longer agreed: the upstream branch, the tree this
phone was brought up in, and the files on the machine that builds it. Merging them meant knowing which
line held which fix, because a fix that exists in two places writes its own version and the newer one is
not always the better one.

## Where the three stood

Everything below is measured, not remembered: `git rev-list --left-right --count`, `wc -c`,
`git log -- <path>`, and `md5sum` on the copies.

| | commit | since the split | what is only there |
|---|---|---|---|
| `origin/main` | `63fc2b4` | 294 commits | Metal: `hakios-metal`, the window server through Metal, frame-rate work |
| the bring-up tree | `e9d5f47` | 33 commits | PCIe stages 11 to 23, the AMSS and WLAN firmware, QRTR, the battery's gauge, psyche's sensors |
| this machine | `a3384e0` | — | the point they split at |

The split is `a3384e0`. The bring-up tree's 33 commits were never pushed; the upstream branch went on
without them and, from the file counts, without ever touching what they change.

## What upstream did not have

The useful question was not "which is newer" but "which fix exists only here". Counted by path:

- **`drivers/wifi/hakios.pcie.c` does not exist upstream.** Neither `hakios.amss.c` nor
  `hakios.wlanfw.c`; `git ls-tree origin/main drivers/wifi/` lists seven files and none of them is one of
  these three. The whole PCIe-to-QCA6390 path — stages 11 through 23, 285,172 bytes of it — is on the
  bring-up line and nowhere else.
- **`drivers/gpu/hakios.adreno.c` upstream is 167,033 bytes**, the 31 October original, and
  `git log origin/main -- drivers/gpu/hakios.adreno.c` returns one commit: the split itself. Every fix
  after it — `gx_power_on` (0 occurrences upstream), `GPU_FW_PREFIX` (0), `adreno_measure` (0),
  `CORE_STREAM_MASK` (0) — is on the bring-up line.
- **`hakios.battery.c` upstream is unchanged since the split** (`git log` returns the split commit only),
  so the PM8150B gauge is here and not there.
- **`hakios.psyche_touch.c` is not upstream**, and `drivers/map` there does not name it.
- **`userland/src/launchd_small.c` is not upstream**, and `userland/build.sh` there does not build it.

The reverse was also true, and it is the reason a plain copy would have been wrong: upstream's
`MiniIOMobileFramebuffer.cpp` is 84,251 bytes against the bring-up tree's 65,863, and upstream's
`hakios.gpu.cpp` is 47,089 against the 43,055 on this machine. Copying the bring-up tree over upstream
would have taken Metal and the frame-rate work out.

## The six files both lines changed

45 paths on one side, 339 on the other, and 6 in both. Three of those 6 turned out not to be conflicts
at all — the bring-up line's version was already upstream, word for word:

- `configs/symbols.tsv`: upstream lines 63 and 85 hold `CAENV` and `FB_CA_HZ` with the identical
  description text the bring-up line adds.
- `docs/boot-args.md`: `fb_ca_hz` is there.
- `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`: `IOMFBDisplayRefresh` appears 4 times and
  `fb_ca_hz` twice.

So the work of merging was: one real conflict (`configs/common_defconfig`), one small addition
(`userland/build.sh`), and `docs/index.md`, which was two different edits to the same table.

## `hakios.gpu.cpp`: each side had something the other did not

Upstream's copy has the client's own pages (`kHakiosUserMemory`, `userBuffer`) and no fix for a fence
that does not return. This machine's copy has the fix and no userptr:

```c
		done = !hakios_gpu_ready() || gpuWait(fence, milliseconds);
		IOLockUnlock(gGPULock);
		if (!done) {
			/* the surface is taken as one the GPU is done with */
			surface->drawn = 0;
		}
```

The two are not alternatives. Git's three-way merge put them together without a conflict because they
touch different parts of `MiniIOSurfaceDrawn`, and the result has both: 47,628 bytes, `surface->drawn = 0`
at line 242 inside the lock, and `kHakiosUserMemory` in 5 places. That merge was checked by reading the
function, not by trusting the exit status.

## What was checked before calling it done

- `git apply --check` of `xnu/hakios.patch` against a fresh `git archive c54f35c`: **applies cleanly**.
- A full `tools/make-kernel.sh`: every driver compiled (`CC hakios.adreno.o`, `hakios.pcie.o`,
  `hakios.wifi.o`, `hakios.lx.o`, ...), the kernel linked, and `make-kernel: built: hakios-kernel`.
  The result is 22,343,864 bytes — 2,448 more than the 22,341,416 this machine had, which is the
  psyche touch driver, the QRTR generation/poll additions and `launchd_small` arriving.
- The symbols are in that kernel: `hakios_psyche_touch_init`, `hakios_pcie_radio_boot` and
  `hakios_qmi_poll` one each in `strings`, `hakios_adreno` five times, and `THE GPU STAYS ON` once —
  the line `hakios.adreno.c` prints when step 21 has run.

One build obstacle is worth writing down because it looks like a failure of the code and is not:
`makedefs/MakeInc.cmd` takes `DSYMUTIL` from `$(XCRUN) -sdk ... -find dsymutil`, and on Linux there is
no `xcrun`, so the variable is empty and the recipe runs `--minimize` as if it were a command
(`bash: line 1: --minimize: command not found`, `MakeInc.kernel:131: Error 127`). The kernel is already
linked at that point — the failure is the debug-symbol copy afterwards. `/root/theos/toolchain/linux/iphone/bin/dsymutil`
exists and runs; naming it in the environment builds the whole thing:

    export PATH=/root/theos/toolchain/linux/iphone/bin:$PATH
    export DSYMUTIL=/root/theos/toolchain/linux/iphone/bin/dsymutil
    tools/make-kernel.sh

## The rule this leaves behind

A file that two lines both changed, where one line is 84,251 bytes and the other 65,863, is decided by
count and by `git log -- <path>`, not by which checkout was opened last. The same applies to
`hakios.adreno.c` (201,834 against 176,088 against 167,033): the number says which copy it is.

Two things the export script had to learn, both found by it refusing to run:

- A driver being rewritten leaves copies beside it (`hakios.wifi.c.before-generation`,
  `hakios.qmi.c.before-qrtr-rejoin`, `*-recovered`, `*.working-1933`). `xnu/export.sh` reads
  untracked files as new drivers and stopped with "these new drivers are not in `drivers/map`" for each.
  The `JUNK` pattern now covers `.bak<anything>`, `.before-`, `.after-`, `.working-` and the like.
- `hakios.sbl.c` (2,674,605 bytes) is a firmware image that was tried and then folded into the driver
  that ships it: `bsd/conf/files` never built it. It is excluded by name rather than left to look like a
  driver nobody wired up.
