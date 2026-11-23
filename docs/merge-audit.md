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

## psyche's touch: three links of a chain, all of them broken

The Xiaomi 12X's touch screen worked on 2 November — `logs/153459-run.log` has `touches=14955` with
coordinates spread across the panel, `last=(454,2258)`, `last=(3586,3233)` — and it was the third of
three separate things that made the merged tree unable to repeat it. None of them is a merge conflict;
each is a link that was never written down.

1. **`drivers/input/conf` had no `driver PSYCHE_TOUCH`.** `hakios.psyche_touch.c` was listed in
   `drivers/map` (the export script's index of where each file goes) but never declared to the
   configuration system, so `CONFIG_HAKIOS_PSYCHE_TOUCH` named nothing, and `hakios-config.py check`
   could not have told anyone: a symbol no `conf` declares is not an error, it is simply never built.
   The block is now in `drivers/input/conf`, word for word as the bring-up tree had it.

2. **`hakios_input_init()` never called `hakios_psyche_touch_init()`.** It called `hakios_goodix_init()`
   and `hakios_buttons_init()` and stopped. The bring-up tree's own log is the evidence, and it is
   measured rather than read: `llvm-objdump -r` on the object built from the merged
   `bsd/dev/hakios.input.c` now lists `ARM64_RELOC_BRANCH26 _hakios_psyche_touch_init` at offset
   `0x4d4`, between `_hakios_goodix_init` at `0x4d0` and `_hakios_buttons_init` at `0x4d8`. Before the
   fix that relocation was not there at all — `git show HEAD:drivers/input/hakios.input.c` has no
   occurrence of the name.

3. **`CONFIG_HAKIOS_PSYCHE_TOUCH=n`.** It is `y` now. A driver whose symbol a `.config` does not name
   is built (`hakios-config.py`: `config.get(d.symbol, d.default) == "y"`, and `Driver.default` is `y`),
   which is why the 2 November run's log says *26 drivers built in (the drivers' defaults (no .config))*
   and lists `psyche_touch` among them. A `.config` that names it `n` is the only way to leave it out,
   and this one did.

Both touch drivers read `touchscreen=1` and both return at once when their chip does not answer. On the
12X the Goodix probe finds nothing — that run's log has no line from it — and psyche_touch goes on to
read the FTS V521. The two are therefore started one after the other, not chosen between.

## On the phone, 24 November: what the three fixes did and did not do

The three were carried to the Linux build tree by `_audit/sync-touch-fix.sh`, built by
`_audit/build-touch.sh`, and run. The short answer: the driver now gets further than it has ever got on
this tree, and the machine still does not survive a run with `touchscreen=1` for long enough to count a
touch. Every line below is from a log in `logs/`.

**It is compiled, called, and reaches the chip.** `logs/021451-run.log`, `021854-run.log` and
`025451-run.log` all contain the whole sequence, the first time on this tree:

```
psyche_touch: serial engine 0x101, interface 0, params 0x20102864, source 19200000 Hz
psyche_touch: vdd already on
psyche_touch: avdd already on
psyche_touch: supplies ready
psyche_touch: system info header ok, data id 01
psyche_touch: chip id 3640, fw ver 0016, config ver 0018
psyche_touch: afe ver cfg 07 cx 07 panel 07, protocol 06
psyche_touch: controller ready (GENI)
psyche_touch: sensing started (scan mode active)
psyche_touch: 125 Hz polling, 10 touches max, raw x 1870..9300 y 1285..23065 -> 1080x2400
psyche_touch: touch the screen now
```

Those runs used `hakios-kernel.full-1119` (the 19 November kernel, 22,341,416 bytes), not the kernel
built from this commit, because that is the kernel the comparison needed.

**Where the chain breaks now: the supplies.** `touch_step=N` panics at checkpoint N of `power_on()` and
hands the machine back to the loader with the log written out, so a checkpoint that never answers is the
one that took the machine with it:

| `touch_step` | result |
|---|---|
| 1 | `returned 20283` |
| 2 | `returned 20297` |
| 3 | no answer |

Checkpoint 3 sits after `rpmh_read(0x41200 + VRM_ENABLE)`. Checkpoints 1 and 2 are the first rail and
came back; the second never did.

**Why: nothing on the normal path mapped the RPMH window.** `gcc`, `se`, `rsc` and `cmddb` are
file-scope and start at zero, and only the experimental entry points (`do_0xa7clk`, `do_probe0xa7`,
`do_powerseq` and the rest) ever set them. `TCS(reg)` is `rsc + 0xd00 + reg`, so with `rsc` zero the
sequencer command was written at `0xd00` — the vectors, not the RSC. `power_on()` did not test `rsc`
before writing; every other entry point did. Both are fixed in `58741f9`: the normal path maps the pin
tiles, GCC and the RSC before it uses them, and `power_on()` returns early when there is no window.

**What is still open.** The kernel built with that fix still does not come back with `touchscreen=1`, and
the failure is now silent rather than a null store, so the mapping was not the only thing. The same
kernel with plain `-v` returns normally (`logs/013531-run.log`, `returned 14049`), and `-v touchscreen=1`
does not; the difference is the touch driver and nothing else.

Runs that were tried, to save the next person the time:

| kernel | ramdisk | arguments | result |
|---|---|---|---|
| 19 Nov | `hello` | `-v touchscreen=1` | returned, `sensing started`, no `touches=` line |
| 19 Nov | `hello` | `-v touchscreen=1 touch_fifowatch=1` | no answer |
| 19 Nov | `root.apfs` | `rd=md0 -v touchscreen=1` | no answer |
| 19 Nov | none | `-v touchscreen=1` | no answer |
| 24 Nov | `hello` | `-v` | returned 14049 |
| 24 Nov | `hello` | `-v touchscreen=1` | no answer |
| 24 Nov | `hello` | `-v touchscreen=1 touch_step=1,2,3` | see the table above |

Only the combination that ends within five seconds — `hello`, which is process 1 and exits — comes back
at all. Every run that would have kept the driver polling (a real user space, `fifowatch`, no ramdisk)
stops the machine. That is consistent with the 2 November logs, which show `touches=14955` from a run
whose user space stayed up: the poll thread is where this dies, and the `touches=` counter is never
reached now. The next step is the poll thread itself — `fts_thread` and what it does per iteration —
not the supplies.

The kernel in this tree is 22,344,256 bytes and carries all of the above. The regression it represents
against 2 November is real and not yet explained; nothing in this document claims otherwise.

