# The GPU that stopped after a hundred submissions

> **Conflict, 24 November 04:22.** This page says the 22,341,416-byte kernel fails at
> submission 101 with `worked 0 us`. That exact file (md5 0b7957f0b463e9dd30a5db808c97a477)
> ran 1667 submissions at 573 us each on the phone, no `no answer` — see STATUS.md and
> logs/042229-run.log:7881. The experiment record below stands; its attribution does not.

On 19 November the Adreno 642L of `redwood` worked, then stopped: `gpu: no answer: submission 101 is
the last, the core has done 33`, with `status 0xd70d85` — CP, RB, CCU, SP and LRZ all busy — and
`HakiosGPU` reporting `the GPU worked 0 us on each of 0`. The window server kept drawing, because
`swap` gives up after `SWAP_GPU_MS` and shows the surface as it is, so the symptom was not a black
screen but a frame rate of ten a second and a `no answer` every few seconds.

The kernel that worked was `hakios-kernel.before-thermal` (4 November 04:01, 15,858,624 bytes),
1448 submissions over 150 seconds, no `no answer` at all. The kernel that did not was the current
one, 22,341,416 bytes. Between them: Wi-Fi for the WCN6750, the PCIe link that carries it, the modem,
the QMI router, the battery's PMIC gauge, and a 7.6 MB firmware array compiled into the image.

## What it was not

Every one of those was ruled out by building and running, not by reading:

- **The size of the image.** With `hakios.wlanfw.c` and `hakios.amss.c` reduced to stubs the kernel
  was 15,736,048 bytes — smaller than the one that worked — and the GPU still stopped at submission
  101, `worked 0 us`.
- **Wi-Fi.** `HAKIOS_WIFI` off, `ath11k.o` moved out of `xnu-deps/build/ath11k/out/`, the whole group
  out of the link, 15,735,976 bytes: same.
- **PCIe.** `hakios.pcie.c` has twelve exported entry points; all twelve were replaced by stubs that
  answer `false`/`-1`/`NULL`, and GPU came up (`step 21: done`, `THE GPU STAYS ON`) and then failed
  the same way.
- **The battery.** `hakios.battery.c.cache-first` — the version that reads the loader's cache instead
  of the PMIC's gauge over SPMI, the one the working kernel used — changed nothing.
- **`hakios.gpu.cpp`.** The 31 October original (43,055 bytes) and the version with the `drawn` field
  both failed.
- **The GPU's own settings.** The 94 register writes of step 20 are byte-for-byte what the working
  kernel writes; the SMMU's `cbar`, `cba2r` and `idr0/1/2/7` are identical; the clock is 550 MHz in
  both; CPU groups are 1804/2419/3187 MHz in both.

## What it was

`hakios.adreno.c` was 176,088 bytes. The version the working kernel was built from —
`hakios.adreno.c.before-psyche-tsens`, 4 November 04:01 — is 201,834. **25,746 bytes of it, 12.8%,
were gone.**

What had been noticed and restored by hand were the loud ones: the three `adreno_measure` routines
that fall back to the frequency table when a clock reports zero, `GMU_CHIP`, `ahb_fence_range_0`,
`GPU_FW_PREFIX`, `CORE_STREAM_MASK`, `stream1`, `zap_unload`, the magic and clock-gating tables, the
UBWC parameters. Those are eleven places. The file had lost an eighth of itself, and no amount of
adding back the places that announce themselves covers the places that do not.

With the 201,834-byte file in place the same image (22.3 MB, Wi-Fi driver and firmware and all)
ran 443 submissions in five seconds at 669 µs each, with no `no answer`.

## How this is checked from now on

Not by reading the code and listing what is missing. **By byte count.** A driver that worked in a
kernel that still boots is a file whose length is a known number; when it stops working, compare the
two lengths before anything else. The difference is not a hint, it is the size of what to look for.

    wc -c drivers/gpu/hakios.adreno.c
    wc -c drivers/gpu/hakios.adreno.c.before-psyche-tsens

The same applies to `hakios.gpu.cpp`, `hakios.pcie.c` and the rest: every one of them has a
`.before-*` copy in the driver's directory, made by the scripts that wrote them.

## The frame rate, once it worked

`MiniIOMobileFramebuffer` counts a frame that arrives 90 ms or more after the last as a pause rather
than a slow frame (`FRAME_PAUSE`), and the count tells the two apart: a still screen gives ten frames
a second with fifty pauses, which is CoreAnimation saving power, not the GPU failing. Under load the
median interval is 17 ms — 59 a second — and the summary reads `half under 17 ms, 95 in 100 under
17 ms`. A `no answer` is what it looks like when it is the GPU: `swap` waits `SWAP_GPU_MS`, gives up,
and the pause count climbs with the frame count staying flat.

## What is still not right

Wi-Fi. The link trains, the endpoint answers as `17cb:1101`, and `pcie_upto=6` leaves it in its cold
PBL exactly as it should. Raising `pcie_upto` past 6 makes the endpoint vanish: the next read of
vendor:device returns `0xffffffff` and enumeration fails with `0x13`. That is the hybrid-bus design
showing through — on the WCN6750 the PCIe device is enumerated by the WPSS's Q6, not by APSS, and
BAR and BAR size are fetched by a QMI device-info request, not read from a device tree. See
`docs/net-review.md` for the review of the rest of the Wi-Fi path, and `hakios.battery.c`'s
experience with SPMI for why a bus shared with the power management is worth suspecting early.

## The kernels in the repository

`hakios-kernel` is the one to boot: 22,341,416 bytes, `hakios.adreno.c` at its 4 November length,
Wi-Fi driver and firmware compiled in, GPU working.

The 4 November kernel that this file is about — 15.9 MB, the one that ran 1448 submissions — is kept
as `hakios-kernel.gpu-ok-1104.deprecated`. It still boots and still works, and it is no longer what
anything should boot from: it predates the Wi-Fi, modem and battery work, so it has the GPU and not
the rest. It stays in the tree because it is the reference for "this is what the GPU looked like when
it was right", which is worth more than the disk it takes.

The other `hakios-kernel.before-*`, `pcie-*` and dated files are earlier experiments; none of them
supersedes the two above.

