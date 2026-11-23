/*
 * Battery monitor for Xiaomi psyche (小米 12X).
 *
 * This file began as redwood's (POCO X5 Pro): it read a page that xnuloader filled from the UEFI's
 * Qualcomm charger drivers, and that path is kept here as a fallback (battery_shared_pa, below).
 * On psyche it does not work: the loader's page stays at "0 reads, 1 failed".
 *
 * The primary path now reads the PMIC itself, the way enesuzun2002/hakios-apollo's
 * hakios.apollo_battery.c does for the Mi 10T (same SoC, same PM8150B):
 *
 *   PM8150B is SPMI slave 2. Its fuel gauge is qpnp-fg-gen4, which runs its own algorithm
 *   (coulomb counting blended with the OCV of the battery profile) inside the PMIC, so its
 *   monotonic SOC, the battery's voltage and current are plain registers:
 *
 *       FG_SOC_MONOTONIC   0x4009   one byte, followed by a copy of itself
 *       FG_INFO_VBATT_IBATT   0x41a0   [1:0] voltage, [3:2] current, each followed by a copy at 0x41a6
 *       FG_RR_BATT_TEMP    0x4288   [9:0] whole degrees Celsius
 *
 *   The charger's state is qpnp-smb5 on the same slave: CHGR_STATUS_1 at 0x1006 (low 3 bits: the
 *   charge state) and USBIN_INT_RT_STS at 0x1310 (bit 4: a cable is in).
 *
 * Verified against the phone's own device tree (2026-11-03, _assets/fdt_pmic.py):
 *
 *   /soc/qcom,spmi@c440000/qcom,pm8150b@2   reg=0x2 0x0
 *     qpnp,fg  [qcom,fg-gen4]   fg-batt-soc@4000  fg-batt-info@4100  fg-rradc@4200  fg-memif@4300
 *     qpnp-smb5                 chgr@1000  batif@1200  usb@1300
 *
 * Nothing of the loader is needed on this path, and nothing is written: reads only.
 *
 * Note: drivers/power/hakios.pmic.c says the chips are PMK8350 / PM7325 / PM8350C — that is
 * redwood's. psyche's are PM8150@0, PM8150@1, PM8150B@2, PM8150B@3. The slave numbers happen to
 * be the same, the silicon does not; never address a PMIC by slave number alone without the revid.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void hakios_spmi_lock(void);      /* drivers/power/hakios.pmic.c: one arbiter transfer at a time */
extern void hakios_spmi_unlock(void);
extern void HakiosBatteryReport(const void *state, unsigned size);     /* drivers/power/hakios.battery.cpp */

/* ---- the loader's cache: the fallback, unchanged --------------------------------------------------------- */

#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)
#define LOADER_PAGE_SIZE                0x4000UL

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent, voltage_mv, current_ma, temperature_c;
	uint32_t charger_action, polls, failures, reserved;
};

static volatile struct loader_battery *shared_battery;
static int battery_soc_percent = -1, battery_voltage_mv = -1, battery_current_ma;
static bool pmic_diagnosed;
static int battery_temperature_c = -273, battery_present, battery_charger_present;
static int battery_charging, battery_soc_estimated, battery_charger_action = -1;
static int battery_from_pmic;
static uint64_t battery_updates, battery_failures;

/* for others in the kernel (the flashlight's thermal guard): -273 while it is not known */
int
hakios_battery_temperature(void)
{
	return battery_temperature_c;
}

SYSCTL_DECL(_kern_hakios);              /* the node is drivers/core/hakios.platform.c's: every driver hangs its sysctls there */
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_percent, 0, "battery state of charge, 0..100 percent, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_voltage_mv, 0, "battery voltage in millivolts, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_current_ma, 0, "battery current in milliamps: more than 0 while discharging");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_temperature_c, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_temperature_c, 0, "battery temperature in Celsius, or -273 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_present, 0, "1 if there is a battery");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_present, 0, "1 if a cable is in the USB port");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charging, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charging, 0, "1 while the charger is charging a non-full battery");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc_estimated, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_estimated, 0, "1 only when SOC had to be estimated from voltage");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_action, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_action, 0, "last charger state: 1 charging, 4 full, 3 not charging, 2 discharging");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_from_pmic, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_from_pmic, 0, "1 when the values come from the PMIC's fuel gauge, 0 from the loader's cache");

/* ---- the SPMI arbiter: reads through this system's observer channels ------------------------------------- */

#define ARB_CORE_PHYS           0x0c440000UL     /* dt: qcom,spmi@c440000 */
#define ARB_CORE_SIZE           0x1100UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_SYSTEM              0
#define ARB_VERSION             0x000
#define ARB_FEATURES            0x004
#define ARB_MAX_CHANNELS        512
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))               /* [19:8] slave and peripheral */

#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)
#define READ_TIMEOUT_US         2000

#define SPMI_SLAVE_PM8150B      2               /* the DTB's qcom,pm8150b@2: reg = <0x2 0x0> */

/* qpnp-fg-gen4 and qpnp-smb5 registers, all on slave 2 (apollo_battery.c's values, which the DTB confirms) */
#define FG_REVID                0x0100          /* pm8150b@2/qcom,revid@100 */
#define FG_SOC_MONOTONIC        0x4009
#define FG_SOC_EN_CTL           0x4046          /* bit 7: the gauge's own algorithm is running */
#define FG_ALGORITHM_EN         0x80
#define FG_RR_INT_RT_STS        0x4210
#define FG_RR_BATT_MISSING      0x01
#define FG_RR_BATT_TEMP         0x4288
#define FG_INFO_VBATT_IBATT     0x41a0
#define FG_INFO_VBATT_IBATT_CP  0x41a6
#define CHGR_STATUS_1           0x1006
#define USBIN_INT_RT_STS        0x1310
#define USBIN_PLUGIN            0x10
#define FG_READ_TRIES           5

enum { CHG_STATE_TRICKLE = 1, CHG_STATE_TAPER = 4, CHG_STATE_TERMINATED = 5 };

static vm_offset_t arb_core, arb_observer;
static uint32_t arb_channels;
static uint16_t channel_cache[256];
static bool channel_seen[256];
static bool pmic_ready;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)(uintptr_t)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)address = value;
}

/* The channel that reaches `peripheral` of slave 2, or -1. The arbiter's table is walked once per peripheral. */
static int
channel_of(uint8_t peripheral)
{
	uint16_t ppid = (uint16_t)((SPMI_SLAVE_PM8150B << 8) | peripheral);

	if (channel_seen[peripheral]) {
		return channel_cache[peripheral] == 0xffff ? -1 : (int)channel_cache[peripheral];
	}
	channel_seen[peripheral] = true;
	channel_cache[peripheral] = 0xffff;
	for (uint32_t n = 0; n < arb_channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));

		if (map != 0 && (uint16_t)((map >> 8) & 0xfff) == ppid) {
			channel_cache[peripheral] = (uint16_t)n;
			return (int)n;
		}
	}
	return -1;
}

/* Read `count` (1 to 8) registers of slave 2 at `address` (peripheral << 8 | register); NULL, or what went wrong. */
static const char *
spmi_read(uint16_t address, uint8_t *bytes, uint32_t count)
{
	uint8_t peripheral = (uint8_t)(address >> 8);
	uint8_t reg = (uint8_t)(address & 0xff);
	int channel = channel_of(peripheral);
	vm_offset_t ch;
	uint64_t start, now, interval;
	uint32_t status, data[2];

	if (channel < 0) {
		return "no channel to that peripheral";
	}
	ch = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * (uint32_t)channel;
	start = mach_absolute_time();
	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	hakios_spmi_lock();
	wr(ch + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = rd(ch + OBSERVER_STATUS);
		now = mach_absolute_time();
	} while (!(status & STATUS_DONE) && now - start < interval);
	data[0] = rd(ch + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(ch + OBSERVER_DATA1) : 0;
	hakios_spmi_unlock();

	if (!(status & STATUS_DONE)) {
		return "no answer";
	} else if (status & STATUS_DENIED) {
		return "denied";
	} else if (status & STATUS_FAILURE) {
		return "failed";
	} else if (status & STATUS_DROPPED) {
		return "dropped";
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

/* ---- the fuel gauge ------------------------------------------------------------------------------------- */

struct gauge {
	bool soc_ok, vbat_ok, temp_ok, chg_ok;
	bool en_ok, missing_ok;
	uint8_t soc, soc_copy, en_ctl, rr_rt, status1, usbin;
	int voltage_mv, current_ma, temperature_c;
	const char *problem;
};

static void
gauge_read(struct gauge *g)
{
	uint8_t a[4], b[4], t[2];
	const char *problem = NULL;

	memset(g, 0, sizeof(*g));
	g->temperature_c = -273;

	/* voltage and current, each with a copy at 0x41a6: taken only when the two agree */
	for (int i = 0; i < FG_READ_TRIES && !g->vbat_ok; i++) {
		if ((problem = spmi_read(FG_INFO_VBATT_IBATT, a, 4)) != NULL ||
		    (problem = spmi_read(FG_INFO_VBATT_IBATT_CP, b, 4)) != NULL) {
			break;
		}
		g->vbat_ok = memcmp(a, b, sizeof(a)) == 0;
	}
	if (g->vbat_ok) {
		uint32_t v = (uint32_t)a[1] << 8 | a[0];
		int32_t i = (int16_t)((uint16_t)a[3] << 8 | a[2]);

		g->voltage_mv = (int32_t)((uint64_t)v * 122070 / 1000000);      /* 0.122070 mV a count */
		g->current_ma = (int32_t)((int64_t)i * 488281 / 1000000);       /* 0.488281 mA a count */
	} else {
		g->problem = problem != NULL ? problem : "the voltage and current do not agree with their copies";
	}

	/*
	 * SOC: BATT_SOC_FG_MONOTONIC_SOC (0x4009) and its copy _CP (0x400a), one byte each.
	 * fg_get_msoc_raw() believes the value when the two agree, and only then; there is no upper
	 * bound in that check, because the raw value is 0..255, not a percentage (FULL_SOC_RAW is 255,
	 * and fg_get_msoc() scales it: 0 -> 0, 255 -> 100, 1..254 -> 1..99). A bound of 100 here, as
	 * this file had, rejected every reading above about 39% and reported it as a gauge that did
	 * not answer.
	 */
	for (int i = 0; i < FG_READ_TRIES; i++) {
		if (spmi_read(FG_SOC_MONOTONIC, t, 2) != NULL) {
			break;
		}
		g->soc = t[0];
		g->soc_copy = t[1];
		if (t[0] == t[1]) {
			g->soc_ok = true;
			break;
		}
	}

	/*
	 * The two bytes fg_gen4's own driver uses to decide whether that value means anything:
	 * BATT_SOC_EN_CTL bit 7 (the gauge's algorithm runs) and ADC_RR_INT_RT_STS bit 0 (the battery
	 * is there). psyche's own device tree has the fg-gen4 node disabled -- see the note at the top
	 * of this file -- so whether its algorithm is running is exactly what this has to show.
	 */
	g->en_ok = spmi_read(FG_SOC_EN_CTL, &g->en_ctl, 1) == NULL;
	g->missing_ok = spmi_read(FG_RR_INT_RT_STS, &g->rr_rt, 1) == NULL;

	/* temperature: whole degrees, not quarters (apollo's note: 0x21 read 33 C where Android said 35.5) */
	if (spmi_read(FG_RR_BATT_TEMP, t, 2) == NULL) {
		int32_t raw = (int32_t)((t[1] & 0x03) << 8 | t[0]);

		g->temperature_c = raw >= 0x200 ? raw - 0x400 : raw;
		g->temp_ok = g->temperature_c > -40 && g->temperature_c < 100;
	}

	g->chg_ok = spmi_read(CHGR_STATUS_1, &g->status1, 1) == NULL &&
	    spmi_read(USBIN_INT_RT_STS, &g->usbin, 1) == NULL;
}

/*
 * What fg-gen4 itself reports, once. psyche's device tree disables this gauge, so the point of
 * the line is to show what a disabled one answers: whether BATT_SOC_EN_CTL bit 7 says its
 * algorithm runs, whether ADC_RR_INT_RT_STS says a battery is there, and the two raw SOC bytes.
 */
static void
battery_pmic_diagnose(void)
{
	struct gauge g;

	memset(&g, 0, sizeof(g));
	gauge_read(&g);
	printf("battery: pmic: fg-gen4 says SOC %02x (copy %02x, %s), EN_CTL %02x%s, RR_INT_RT_STS %02x%s, "
	    "V %d mV, T %d C%s%s\n", g.soc, g.soc_copy, g.soc_ok ? "they agree" : "they disagree",
	    g.en_ctl, !g.en_ok ? " ??" : (g.en_ctl & FG_ALGORITHM_EN) ? " algorithm running" : " ALGORITHM OFF",
	    g.rr_rt, !g.missing_ok ? " ??" : (g.rr_rt & FG_RR_BATT_MISSING) ? " BATTERY MISSING" : " battery there",
	    g.voltage_mv, g.temperature_c, g.vbat_ok ? "" : " (voltage refused)",
	    g.temp_ok ? "" : " (temperature refused)");
}

/*
 * fg_get_msoc()'s scaling of the gauge's raw 0..255 into the 0..100 that Android shows:
 * DIV_ROUND_CLOSEST((raw - 1) * (100 - 2), 255 - 2) + 1, with 0 and 255 as the endpoints.
 */
static int
fg_soc_percent(uint8_t raw)
{
	if (raw == 0xff) {
		return 100;
	}
	if (raw == 0) {
		return 0;
	}
	return ((raw - 1) * 98 + 253 / 2) / 253 + 1;
}

static void
battery_pmic_poll(void)
{
	struct gauge g;
	struct hakios_battery_state state;
	uint8_t chg_state;
	bool external, charging, full;

	gauge_read(&g);
	if (!g.soc_ok) {
		if (battery_failures < 3) {
			printf("battery: pmic: no SOC: %s (SOC bytes %02x %02x, EN_CTL %02x%s, RR_INT_RT_STS %02x%s, "
			    "V %d mV, T %d C)\n",
			    g.problem != NULL ? g.problem : "the SOC and its copy disagree", g.soc, g.soc_copy, g.en_ctl,
			    !g.en_ok ? " ??" : (g.en_ctl & FG_ALGORITHM_EN) ? " algorithm running" : " ALGORITHM OFF",
			    g.rr_rt, !g.missing_ok ? " ??" : (g.rr_rt & FG_RR_BATT_MISSING) ? " BATTERY MISSING" : " battery there",
			    g.voltage_mv, g.temperature_c);
		}
		battery_failures++;
		return;
	}
	chg_state = g.chg_ok ? (uint8_t)(g.status1 & 7) : 0;
	external = g.chg_ok && (g.usbin & USBIN_PLUGIN) != 0;
	charging = g.chg_ok && chg_state >= CHG_STATE_TRICKLE && chg_state <= CHG_STATE_TAPER;
	full = g.chg_ok && chg_state == CHG_STATE_TERMINATED;

	battery_soc_percent = fg_soc_percent(g.soc);
	battery_voltage_mv = g.vbat_ok ? g.voltage_mv : -1;
	battery_current_ma = g.vbat_ok ? g.current_ma : 0;
	battery_temperature_c = g.temp_ok ? g.temperature_c : -273;
	battery_present = 1;
	battery_charger_present = external ? 1 : 0;
	battery_charging = charging ? 1 : 0;
	battery_soc_estimated = 0;
	battery_charger_action = external ? (full ? 4 : charging ? 1 : 3) : 2;
	battery_from_pmic = 1;
	battery_updates++;

	memset(&state, 0, sizeof(state));
	state.size = sizeof(state);
	state.percent_hundredths = fg_soc_percent(g.soc) * 100;
	state.voltage_uv = g.vbat_ok ? g.voltage_mv * 1000 : 0;
	/* the gauge counts a positive current while discharging; this structure counts it while charging */
	state.current_ua = g.vbat_ok ? -g.current_ma * 1000 : 0;
	state.temperature_hundredths = g.temp_ok ? g.temperature_c * 100 : 0;
	state.present = 1;
	state.external = external ? 1 : 0;
	state.status = battery_charger_action;
	HakiosBatteryReport(&state, sizeof(state));
}

/* ---- the loader's cache: the source on psyche, and the fallback everywhere the PMIC's gauge works ------ */

static bool
battery_snapshot(struct loader_battery *out)
{
	uint32_t before, after;

	if (shared_battery == NULL) {
		return false;
	}
	for (unsigned retry = 0; retry < 4; retry++) {
		before = shared_battery->sequence;
		if (before & 1) continue;
		__asm__ volatile ("dmb ishld" ::: "memory");
		memcpy(out, (const void *)(uintptr_t)shared_battery, sizeof(*out));
		__asm__ volatile ("dmb ishld" ::: "memory");
		after = shared_battery->sequence;
		if (before == after && !(after & 1)) return true;
	}
	return false;
}

static bool
battery_loader_poll(void)
{
	struct loader_battery sample;

	if (!battery_snapshot(&sample) || sample.magic != LOADER_BATTERY_MAGIC ||
	    sample.version != LOADER_BATTERY_VERSION || sample.size < sizeof(sample) ||
	    !(sample.flags & LOADER_BATTERY_VALID) || sample.soc_percent < 0 || sample.soc_percent > 100) {
		battery_failures++;
		return false;
	}
	battery_soc_percent = sample.soc_percent;
	battery_voltage_mv = (sample.flags & LOADER_BATTERY_VOLTAGE_VALID) ? sample.voltage_mv : -1;
	battery_current_ma = sample.current_ma;
	battery_temperature_c = sample.temperature_c;
	battery_present = (sample.flags & LOADER_BATTERY_PRESENT) != 0;
	battery_charger_present = (sample.flags & LOADER_BATTERY_CHARGER_PRESENT) != 0;
	battery_charging = (sample.flags & LOADER_BATTERY_CHARGING) != 0;
	battery_soc_estimated = (sample.flags & LOADER_BATTERY_SOC_ESTIMATED) != 0;
	battery_charger_action = (int)sample.charger_action;
	battery_from_pmic = 0;
	battery_updates++;
	return true;
}

/*
 * Where the numbers come from, and why not from the PMIC.
 *
 * psyche's device tree disables PM8150B's fg-gen4 (status = "disabled" on
 * /soc/qcom,spmi@c440000/qcom,pm8150b@2/qpnp,fg) and puts the pack's real fuel gauge on I2C
 * instead (bq27z561@0x55 at /soc/i2c@884000). Nothing on this board loads a profile into
 * fg-gen4, so its algorithm never runs and BATT_SOC_FG_MONOTONIC_SOC and its copy disagree
 * for good. The firmware does talk to the real gauge, and the loader keeps its answer in the
 * shared page -- that is the `battery: flags=f SOC=100% V=4414 mV` line in the run logs.
 *
 * So the cache is the source here. The SPMI gauge is still read, once, so the log says what it
 * reports (EN_CTL and ADC_RR_INT_RT_STS in particular); its readings never reach the sysctls.
 */
static void
battery_poll(void)
{
	if (shared_battery != NULL) {
		if (pmic_ready && !pmic_diagnosed) {
			pmic_diagnosed = true;
			battery_pmic_diagnose();
		}
		if (battery_loader_poll()) {
			return;
		}
	}
	if (pmic_ready) {
		battery_pmic_poll();
	}
}

/*
 * What the audio DSP's battery manager says (drivers/audio/hakios.adsp.c, with adsp=1 unless battmgr=0):
 * the values that Android shows, from where Android takes them. They are kept here for the sysctls
 * and passed on to the power source that Darwin reads (drivers/power/hakios.battery.cpp).
 */
static int battery_full_uah, battery_design_uah, battery_cycles, battery_source_dsp;

SYSCTL_INT(_kern_hakios, OID_AUTO, battery_full_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_full_uah, 0, "the battery's full charge as the fuel gauge has learnt it, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_design_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_design_uah, 0, "the battery's charge as designed, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_cycles, 0, "the battery's charge cycles (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_from_dsp, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_source_dsp, 0, "1 when the values come from the audio DSP's battery manager");

void
hakios_battery_report(const struct hakios_battery_state *b)
{
	/* (rounded in 64 bits: the DSP's value near INT32_MAX plus 50 would overflow an int, and a full battery would read 0%) */
	int percent = (int)(((int64_t)b->percent_hundredths + 50) / 100);

	battery_soc_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	battery_voltage_mv = b->voltage_uv / 1000;
	battery_current_ma = b->current_ua / 1000;
	/* (in 64 bits for the same reason: an overflow here would make a hot battery read as cold) */
	battery_temperature_c = (int)(((int64_t)b->temperature_hundredths + 50) / 100);
	battery_present = b->present != 0;
	battery_charger_present = b->external != 0;
	battery_charging = b->status == 1;      /* POWER_SUPPLY_STATUS_CHARGING */
	battery_soc_estimated = 0;
	battery_charger_action = b->status;
	if (b->full_uah > 0) {
		battery_full_uah = b->full_uah;
	}
	if (b->design_uah > 0) {
		battery_design_uah = b->design_uah;
	}
	if (b->cycles > 0) {
		battery_cycles = b->cycles;
	}
	battery_source_dsp = 1;
	battery_updates++;
	HakiosBatteryReport(b, sizeof(*b));
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (unsigned tick = 0;; tick++) {
		battery_poll();
		if (tick % 12 == 0) {
			printf("battery: SOC=%d%%%s, V=%d mV, I=%d mA, T=%d C, battery=%d charger=%d charging=%d "
			    "action=%d (%llu reads, %llu failed, from %s)\n", battery_soc_percent,
			    battery_soc_estimated ? " (voltage estimate)" : "", battery_voltage_mv,
			    battery_current_ma, battery_temperature_c, battery_present, battery_charger_present,
			    battery_charging, battery_charger_action, battery_updates, battery_failures,
			    battery_from_pmic ? "the PMIC's gauge" : "the loader's cache");
		}
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}

/* Map the arbiter and prove the gauge answers: the revid of slave 2. */
static bool
battery_pmic_init(void)
{
	uint32_t version;
	uint8_t revid[4];
	const char *problem;

	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	if (arb_core == 0 || arb_observer == 0) {
		printf("battery: pmic: could not map the SPMI arbiter\n");
		return false;
	}
	version = rd(arb_core + ARB_VERSION);
	arb_channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (arb_channels == 0 || arb_channels > ARB_MAX_CHANNELS) {
		arb_channels = ARB_MAX_CHANNELS;
	}
	printf("battery: pmic: SPMI arbiter version %#x, %u channels\n", version, arb_channels);

	problem = spmi_read(FG_REVID, revid, 4);
	if (problem != NULL) {
		printf("battery: pmic: slave %u's revid: %s\n", SPMI_SLAVE_PM8150B, problem);
		return false;
	}
	printf("battery: pmic: slave %u (PM8150B) answers; revid %02x %02x %02x %02x\n", SPMI_SLAVE_PM8150B,
	    revid[0], revid[1], revid[2], revid[3]);
	printf("battery: pmic: channels: FG 0x40 -> %d, FG 0x41 -> %d, RRADC 0x42 -> %d, CHGR 0x10 -> %d, USBIN 0x13 -> %d\n",
	    channel_of(0x40), channel_of(0x41), channel_of(0x42), channel_of(0x10), channel_of(0x13));
	/* Nothing more is read here: an SPMI read of the gauge at this point in the start-up kept the
	 * kernel from starting at all (20-line log, no Darwin banner). The gauge is read from the
	 * battery thread instead, five seconds in, where the same reads work. */
	return true;
}

void
hakios_battery_init(void)
{
	uint32_t enabled = 0, use_pmic = 1;
	uint64_t shared_pa = 0;
	vm_offset_t page;
	thread_t thread;

	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) return;
	(void)PE_parse_boot_argn("battery_pmic", &use_pmic, sizeof(use_pmic));

	/*
	 * The loader's cache first, and whether or not the PMIC probe works: on psyche the cache is
	 * the only source that reads the real gauge (see battery_poll()). It used to be mapped only in
	 * the else branch below, which on this board never ran, because the PMIC probe succeeds.
	 */
	if (PE_parse_boot_argn("battery_shared_pa", &shared_pa, sizeof(shared_pa)) && shared_pa != 0) {
		/* The cache must lie inside the one page that is mapped: an address near its end would be read past the mapping. */
		if ((shared_pa & (LOADER_PAGE_SIZE - 1)) + sizeof(struct loader_battery) > LOADER_PAGE_SIZE) {
			printf("battery: battery_shared_pa %#llx: the cache would cross the end of its page\n", shared_pa);
		} else {
			page = ml_io_map((vm_offset_t)(shared_pa & ~(LOADER_PAGE_SIZE - 1)), LOADER_PAGE_SIZE);
			shared_battery = (volatile struct loader_battery *)(page + (shared_pa & (LOADER_PAGE_SIZE - 1)));
			if (shared_battery->magic != LOADER_BATTERY_MAGIC ||
			    shared_battery->version != LOADER_BATTERY_VERSION ||
			    shared_battery->size < sizeof(struct loader_battery)) {
				printf("battery: bad xnuloader cache at %#llx (magic %#llx version %u size %u)\n", shared_pa,
				    shared_battery->magic, shared_battery->version, shared_battery->size);
				shared_battery = NULL;
			} else {
				printf("battery: Qualcomm UEFI cache at pa %#llx: the source\n", shared_pa);
			}
		}
	}

	/* the PMIC next: it needs nothing of the loader, and its registers are worth printing either way */
	if (use_pmic != 0 && battery_pmic_init()) {
		pmic_ready = true;
		printf("battery: reading PM8150B's fuel gauge (fg-gen4) too, for its diagnostic line\n");
	}
	if (shared_battery == NULL && !pmic_ready) {
		printf("battery: neither the PMIC's gauge nor the loader's cache is available\n");
		return;
	}
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
