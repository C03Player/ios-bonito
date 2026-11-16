/*
 * bonito (Pixel 3a XL, SDM670) boot shim: hardware probe, stage 1.
 *
 * ABL enters here at EL1 with the MMU and caches off and x0 = DTB. This
 * build only LOOKS at the hardware and prints what it finds on the screen
 * (and keeps it in a RAM log). It writes to no storage, no PMIC and no
 * clock/regulator register; the only hardware write is the display's
 * autorefresh bit so command-mode frames reach the panel. It never resets
 * for 30 s: then it requests a reboot into the bootloader and resets.
 *
 * Derived from ios-flame's shim (MIT); see ../../ATTRIBUTION.md.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "log.h"
#include "sde.h"
#include "spmi.h"
#include "util.h"

#define IMEM_RESTART_REASON	0x146bf65cUL	/* imem 0x146bf000 + restart_reason@65c */
#define WDT_BASE		0x17980000UL	/* qcom,wdt@17980000 (stock SDM670 DTB); flame's is 0x17c10000 */
#define WDT_RST			0x04
#define WDT_EN			0x08
#define WDT_STS			0x0c
#define WDT_BARK		0x10
#define WDT_BITE		0x14
#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define SDHCI_BASE		0x007c4000UL	/* sdhci@7c4000, eMMC (sdhci-msm-v5) */
#define SDHCI_CQ_BASE		0x007c5000UL

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

static int screen_ok;
static volatile int in_exception;

/* Redraw the whole log; the newest text is the end, so show the tail that fits. */
static void show(void)
{
	if (!screen_ok)
		return;
	con_clear(0xff102030);
	uint32_t start = 0;
	/* skip leading lines until the rest fits (measure with fg == 0) */
	while (start < log_len) {
		con_home();
		if (con_text(log_buf + start, log_len - start, 0) == log_len - start)
			break;
		while (start < log_len && log_buf[start] != '\n')
			start++;
		start++;
	}
	con_home();
	con_text(log_buf + start, log_len - start, WHITE);
}

#define STEP() show()

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (in_exception++)
		for (;;)
			__asm__ volatile("wfe");
	log_printf("\n*** EXCEPTION slot %lu esr %lx elr %lx far %lx x0 %lx lr %lx\n", slot,
		   rsr("esr_el1"), rsr("elr_el1"), rsr("far_el1"), regs[0], regs[30]);
	show();
	uint64_t end = ticks_now() + 19200000ULL * 20;
	while (ticks_now() < end)
		;
	for (;;)
		smc_call(PSCI_SYSTEM_RESET, 0, 0, 0);
}

void secondary_main(uint64_t idx)
{
	(void)idx;
	for (;;)
		__asm__ volatile("wfe");
}

static void mem_cb(const char *name, const uint32_t *w, unsigned n)
{
	log_printf("dtb %s reg:", name);
	for (unsigned i = 0; i < n; i++)
		log_printf(" %x", w[i]);
	log_printf("\n");
}

#ifndef RESET_AFTER_S
#define RESET_AFTER_S	30
#endif
/* Bisect aid: SHIM_STAGE=n stops after stage n and resets (0: reset right away, nothing touched). */
#ifndef SHIM_STAGE
#define SHIM_STAGE	99
#endif
#define PON_SOFT_RB_SPARE	0x88f	/* PM660 PON (SID 0), reason in [7:1]; unverified on bonito */
#define PON_REASON_BOOTLOADER	0x02
#define RESTART_BOOTLOADER	0x77665500U

static void set_reboot_bootloader(void)
{
	uint8_t v = 0xff, after = 0xff;
	int rc = spmi_read(0, PON_SOFT_RB_SPARE, &v);
	if (!rc)
		rc = spmi_write(0, PON_SOFT_RB_SPARE, (v & 1) | PON_REASON_BOOTLOADER << 1);
	spmi_read(0, PON_SOFT_RB_SPARE, &after);
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	log_printf("reboot reason: pon spare %x -> %x (rc %d)\n", v, after, rc);
}

static void log_cpu(uintptr_t dtb)
{
	log_printf("BONITOSHIM probe v1 t=%lu\n", ticks_now());
	log_printf("cpu: el %lu midr %lx mpidr %lx\n", rsr("currentel") >> 2, rsr("midr_el1"),
		   rsr("mpidr_el1"));
	log_printf("cpu: mmfr0 %lx pfr0 %lx isar0 %lx\n", rsr("id_aa64mmfr0_el1"),
		   rsr("id_aa64pfr0_el1"), rsr("id_aa64isar0_el1"));
	log_printf("cpu: sctlr %lx ctr %lx cntfrq %lu\n", rsr("sctlr_el1"), rsr("ctr_el0"),
		   rsr("cntfrq_el0"));
	uint64_t lo = 0, hi = 0;
	fdt_chosen_u64(dtb, "linux,initrd-start", &lo);
	fdt_chosen_u64(dtb, "linux,initrd-end", &hi);
	log_printf("boot: dtb %lx magic %x initrd %lx-%lx\n", (uint64_t)dtb,
		   dtb ? __builtin_bswap32(rd32(dtb)) : 0, lo, hi);
	fdt_memory_regs(dtb, mem_cb);
	log_printf("psci: version %lx smccc %lx\n", smc_call(PSCI_VERSION, 0, 0, 0),
		   smc_call(SMCCC_VERSION, 0, 0, 0));
	log_printf("psci: feat cpu_on %lx reset %lx reset2 %lx susp %lx\n",
		   smc_call(PSCI_FEATURES, PSCI_CPU_ON, 0, 0),
		   smc_call(PSCI_FEATURES, PSCI_SYSTEM_RESET, 0, 0),
		   smc_call(PSCI_FEATURES, PSCI_SYSTEM_RESET2, 0, 0),
		   smc_call(PSCI_FEATURES, PSCI_SYSTEM_SUSPEND, 0, 0));
}

/* Read-only PON peripheral registers of the PMIC at SID 0 (PM660 on bonito). */
static void log_pmic(void)
{
	static const uint16_t regs[] = { 0x100, 0x101, 0x104, 0x105, 0x808, 0x809, 0x80a, 0x80c,
					 0x810, 0x85a, 0x85b, 0x88c, 0x88f };
	log_printf("pmic sid0:");
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint8_t v = 0;
		int rc = spmi_read(0, regs[i], &v);
		log_printf(" %x=%x%s", regs[i], v, rc ? "!" : "");
	}
	log_printf("\n");
}

static void log_gic(void)
{
	log_printf("gic: ctlr %x typer %x iidr %x\n", rd32(GICD_BASE), rd32(GICD_BASE + 4),
		   rd32(GICD_BASE + 8));
	for (int i = 0; i < 8; i++) {
		uintptr_t r = GICR_BASE + 0x20000UL * i;
		log_printf("gicr%d: typer %x:%x waker %x\n", i, rd32(r + 0xc), rd32(r + 8),
			   rd32(r + 0x14));
	}
}

/* Read-only look at the eMMC host controller ABL used to load us. */
static void log_sdhci(void)
{
	log_printf("sdhci: caps %x:%x ver %x pres %x hctl %x clk %x\n", rd32(SDHCI_BASE + 0x40),
		   rd32(SDHCI_BASE + 0x44), rd32(SDHCI_BASE + 0xfc) >> 16, rd32(SDHCI_BASE + 0x24),
		   rd32(SDHCI_BASE + 0x28), rd32(SDHCI_BASE + 0x2c));
	log_printf("sdhci: cq ver %x cfg %x\n", rd32(SDHCI_CQ_BASE), rd32(SDHCI_CQ_BASE + 4));
}

static void __attribute__((noreturn)) stage_reset(void)
{
	for (;;) {
		smc_call(PSCI_SYSTEM_RESET, 0, 0, 0);
		__asm__ volatile("wfe");
	}
}

#ifndef STAGE_WAIT_S
#define STAGE_WAIT_S	5
#endif
/* Wait STAGE_WAIT_S seconds petting the watchdog, then reset. */
#define STAGE(n) do { if (SHIM_STAGE <= (n)) { uint64_t t_ = ticks_now(); \
	while (ticks_now() - t_ < 19200000ULL * STAGE_WAIT_S) { delay_ms(500); } \
	stage_reset(); } } while (0)

/* The bootloader leaves the APSS watchdog running (bark 11 s in the DTB): look, disable, pet. */
__attribute__((unused)) static void wdt_quiet(void)
{
	log_printf("wdt: en %x sts %x bark %x bite %x\n", rd32(WDT_BASE + WDT_EN),
		   rd32(WDT_BASE + WDT_STS), rd32(WDT_BASE + WDT_BARK), rd32(WDT_BASE + WDT_BITE));
	wr32(WDT_BASE + WDT_RST, 1);
	wr32(WDT_BASE + WDT_EN, 0);
	log_printf("wdt: after disable en %x sts %x\n", rd32(WDT_BASE + WDT_EN),
		   rd32(WDT_BASE + WDT_STS));
}

void shim_main(uintptr_t dtb)
{
#ifdef SHIM_WDT_QUIET
	wdt_quiet();	/* experimental: builds with this never reached the display */
#endif
	STAGE(0);
	log_cpu(dtb);
	STAGE(1);
	log_printf("imem restart_reason %x\n", rd32(IMEM_RESTART_REASON));

	sde_probe();
	STAGE(2);
	uintptr_t fb = sde.pipe ? rd32(sde.pipe + 0x14) : 0;
	uint32_t stride = sde.pipe ? rd32(sde.pipe + 0x24) & 0xffff : 0;
	log_printf("fb %lx stride %u\n", (uint64_t)fb, stride);
	screen_ok = !con_init(fb, stride);
	if (!screen_ok) {
		/* No usable pipe found: draw into the start of the splash region anyway. */
		log_printf("con_init(%lx,%u) failed; fallback %lx\n", (uint64_t)fb, stride,
			   (uint64_t)FB_SPLASH_BASE);
		screen_ok = !con_init(FB_SPLASH_BASE, FB_W * 4);
	}
	sde_autorefresh(1);
	STEP();
	STAGE(3);

	if (!spmi_init()) {
		log_pmic();
		STEP();
	}
	STAGE(4);
	log_gic();
	STEP();
	STAGE(5);
	log_sdhci();
	log_printf("probe done at %lu ms\n", ticks_now() / 19200);
	STEP();
	/* Proof of life without a readable screen: after RESET_AFTER_S the shim asks for a
	 * reboot into the bootloader (PMIC PON reason + IMEM cookie, as flame does) and resets. */
	uint64_t t0 = ticks_now(), end = t0 + 19200000ULL * RESET_AFTER_S;
	uint32_t last = 0;
	while (ticks_now() < end) {
		uint32_t sec = (uint32_t)((ticks_now() - t0) / 19200000ULL);
		if (sec != last && sec % 3 == 0) {	/* keep the panel fed and the text on top */
			last = sec;
#ifdef SHIM_WDT_QUIET
			wr32(WDT_BASE + WDT_RST, 1);
#endif
			sde_autorefresh(1);
			log_printf("alive %us / %us\n", sec, (unsigned)RESET_AFTER_S);
			show();
		}
	}
	set_reboot_bootloader();
	for (;;) {
		smc_call(PSCI_SYSTEM_RESET, 0, 0, 0);
		__asm__ volatile("wfe");
	}
}
