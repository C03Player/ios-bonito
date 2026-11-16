/*
 * eMMC on bonito: Qualcomm SDCC5 (qcom,sdhci-msm-v5) at 0x7c4000, polled PIO, 1-bit, legacy timing.
 *
 * ABL leaves the controller shut down (bus power off, SD clock off, DLL in reset) and the SDCC1 apps
 * clock parked on GPLL6/384 MHz (live probe, see docs/BONITO-HARDWARE.md). This brings it up like
 * Linux's sdhci-msm: GCC clock -> HC_MODE -> reset -> power IRQ handshake (polled) -> SDHCI clock ->
 * CMD0/1/2/3/7/8 -> CMD17. READ ONLY: no write command is ever issued.
 *
 * References: drivers/mmc/host/sdhci-msm.c (offsets for the v5 "mci removed" layout, pwr_irq handler),
 * drivers/clk/qcom/gcc-sdm845.c (gcc_sdcc1_apps_clk_src at 0x26028, parent map 7: 1 = GPLL0_OUT_MAIN),
 * the SDHCI spec, JESD84. Regulators (VCC/VCCQ) are not touched: assumed left on by ABL.
 */
#include <stdint.h>
#include "emmc.h"
#include "log.h"
#include "util.h"

#define SDHCI		0x007c4000UL
#define GCC		0x00100000UL
#define SDCC1_APPS_CBCR	(GCC + 0x26004)
#define SDCC1_AHB_CBCR	(GCC + 0x26008)
#define SDCC1_CMD_RCGR	(GCC + 0x26028)
#define SDCC1_CFG_RCGR	(GCC + 0x2602c)

#define HC_MODE		0x78
#define VENDOR_SPEC	0x20c
#define PWRCTL_STATUS	0x240
#define PWRCTL_CLEAR	0x248
#define PWRCTL_CTL	0x24c
#define IO_PAD_PWR_SWITCH_EN	(1u << 15)
#define IO_PAD_PWR_SWITCH	(1u << 16)

static inline uint8_t rd8(uintptr_t a) { return *(volatile uint8_t *)a; }
static inline uint16_t rd16(uintptr_t a) { return *(volatile uint16_t *)a; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline void wr16(uintptr_t a, uint16_t v) { *(volatile uint16_t *)a = v; }

#define S_ARG		0x08
#define S_BLK		0x04
#define S_CMD		0x0c	/* 32-bit: [15:0] transfer mode, [31:16] command */
#define S_RESP		0x10
#define S_BUF		0x20
#define S_PRES		0x24
#define S_HCTL		0x28
#define S_PWR		0x29
#define S_CLK		0x2c
#define S_TOUT		0x2e
#define S_RST		0x2f
#define S_INT		0x30
#define S_INTEN		0x34
#define S_SIGEN		0x38

static uint32_t sectors;
static int emmc_up;
static uint32_t rca = 1;
static int hc_card;	/* sector addressing */

static int wait_bit8(uintptr_t a, uint8_t mask, uint8_t val, unsigned ms)
{
	uint64_t end = ticks_now() + 19200ULL * ms;
	while ((rd8(a) & mask) != val)
		if (ticks_now() > end)
			return -1;
	return 0;
}

static int wait_clear32(uintptr_t a, uint32_t mask, unsigned ms)
{
	uint64_t end = ticks_now() + 19200ULL * ms;
	while (rd32(a) & mask)
		if (ticks_now() > end)
			return -1;
	return 0;
}

/* GCC: SDCC1 apps clock = GPLL0_OUT_MAIN (src 1) / 6 = 100 MHz, bypass M/N. */
static int gcc_clock(void)
{
	wr32(SDCC1_APPS_CBCR, rd32(SDCC1_APPS_CBCR) | 1);
	wr32(SDCC1_AHB_CBCR, rd32(SDCC1_AHB_CBCR) | 1);
	wr32(SDCC1_CFG_RCGR, (1u << 8) | (2 * 6 - 1));
	wr32(SDCC1_CMD_RCGR, 0x3);	/* UPDATE | ROOT_EN */
	if (wait_clear32(SDCC1_CMD_RCGR, 1, 10))
		return -1;
	uint64_t end = ticks_now() + 19200ULL * 10;
	while ((rd32(SDCC1_APPS_CBCR) >> 31) && ticks_now() < end)
		;
	log_printf("emmc: gcc cmd %x cfg %x apps %x\n", rd32(SDCC1_CMD_RCGR), rd32(SDCC1_CFG_RCGR),
		   rd32(SDCC1_APPS_CBCR));
	return rd32(SDCC1_APPS_CBCR) >> 31 ? -2 : 0;
}

/* The controller raises "power IRQ" status on every power/IO-voltage write; Linux acks it from an ISR. */
static void pwr_ack(void)
{
	uint64_t end = ticks_now() + 19200ULL * 50;
	uint8_t st;
	while (!(st = rd8(SDHCI + PWRCTL_STATUS)) && ticks_now() < end)
		;
	if (!st)
		return;
	wr32(SDHCI + PWRCTL_CLEAR, st);
	uint8_t ack = 0;
	if (st & 3)
		ack |= 1;	/* BUS_SUCCESS */
	if (st & 0xc)
		ack |= 4;	/* IO_SUCCESS */
	wr8(SDHCI + PWRCTL_CTL, ack);
	log_printf("emmc: pwrctl st %x ack %x\n", st, ack);
}

static int cmd(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *resp)
{
	if (wait_clear32(SDHCI + S_PRES, 3, 100))	/* CMD + DAT inhibit */
		return -10;
	wr32(SDHCI + S_INT, 0xffffffff);
	wr32(SDHCI + S_ARG, arg);
	wr32(SDHCI + S_CMD, (index << 24) | (flags << 16) | ((flags >> 16) & 0xffff));
	uint64_t end = ticks_now() + 19200ULL * 200;
	uint32_t st;
	for (;;) {
		st = rd32(SDHCI + S_INT);
		if (st & 0x8000)
			break;	/* error */
		if (st & 1)
			break;	/* command complete */
		if (ticks_now() > end)
			return -11;
	}
	if (st & 0x8000) {
		wr32(SDHCI + S_INT, st);
		wr8(SDHCI + S_RST, 6);	/* reset CMD and DAT lines */
		wait_bit8(SDHCI + S_RST, 6, 0, 10);
		return -(int)(st >> 16) - 100;
	}
	wr32(SDHCI + S_INT, 1);
	if (resp)
		for (int i = 0; i < 4; i++)
			resp[i] = rd32(SDHCI + S_RESP + 4 * i);
	return 0;
}

/* flags encoding: (command-register low byte) in bits 7:0 of the 16-bit command field; transfer mode in the high half. */
#define CF(resp, crc, idx, data)	(((resp) | ((crc) << 3) | ((idx) << 4) | ((data) << 5)) & 0xff)
#define R_NONE	0
#define R_136	1
#define R_48	2
#define R_48B	3

static int wait_xfer_done(unsigned ms)
{
	uint64_t end = ticks_now() + 19200ULL * ms;
	for (;;) {
		uint32_t st = rd32(SDHCI + S_INT);
		if (st & 0x8000)
			return -1;
		if (st & 2) {
			wr32(SDHCI + S_INT, 2);
			return 0;
		}
		if (ticks_now() > end)
			return -2;
	}
}

/*
 * One-block read through ADMA2 (32-bit descriptors). The PIO buffer port does not advance on this controller
 * (live test 2026-11-16: every read returned the first word), and caps bit 22 (SDMA) is clear while bit 19
 * (ADMA2) is set. MMU and caches are off, so the shim's own RAM is coherent for DMA.
 */
static volatile uint64_t adma_desc[2] __attribute__((aligned(8)));
static uint8_t dma_buf[512] __attribute__((aligned(64)));

#define S_ADMA_ERR	0x54
#define S_ADMA_ADDR	0x58

static int data_read(uint32_t index, uint32_t arg, void *buf, unsigned len)
{
	if (len != 512)
		return -20;
	uint32_t pa = (uint32_t)(uintptr_t)dma_buf;
	for (unsigned i = 0; i < 512; i++)
		dma_buf[i] = 0xa5;	/* so an untouched buffer is recognisable */
	/* attr: VALID | END | ACT=tran(0x20); length; 32-bit address */
	adma_desc[0] = (uint64_t)pa << 32 | (uint64_t)512 << 16 | 0x23;
	wr32(SDHCI + S_ADMA_ADDR, (uint32_t)(uintptr_t)adma_desc);
	wr8(SDHCI + S_HCTL, (rd8(SDHCI + S_HCTL) & ~0x18) | 0x10);	/* DMA select = ADMA2 32-bit */
	wr16(SDHCI + S_BLK, len);
	wr16(SDHCI + S_BLK + 2, 1);
	wr16(SDHCI + S_TOUT, 0xe);
	if (wait_clear32(SDHCI + S_PRES, 3, 100))
		return -10;
	wr32(SDHCI + S_INT, 0xffffffff);
	wr32(SDHCI + S_ARG, arg);
	/* transfer mode: DMA enable (bit 0) + read (bit 4); command: R1 + CRC + index + data */
	wr32(SDHCI + S_CMD, (index << 24) | (CF(R_48, 1, 1, 1) << 16) | 0x0011);
	uint64_t end = ticks_now() + 19200ULL * 500;
	uint32_t st;
	for (;;) {
		st = rd32(SDHCI + S_INT);
		if (st & 0x8000)
			break;
		if (st & 2)
			break;	/* transfer complete */
		if (ticks_now() > end) {
			log_printf("emmc: dma timeout int %x pres %x adma_err %x\n", st, rd32(SDHCI + S_PRES),
				   rd32(SDHCI + S_ADMA_ERR));
			wr8(SDHCI + S_RST, 6);
			wait_bit8(SDHCI + S_RST, 6, 0, 10);
			return -11;
		}
	}
	if (st & 0x8000) {
		log_printf("emmc: dma error int %x adma_err %x\n", st, rd32(SDHCI + S_ADMA_ERR));
		wr32(SDHCI + S_INT, st);
		wr8(SDHCI + S_RST, 6);
		wait_bit8(SDHCI + S_RST, 6, 0, 10);
		return -12;
	}
	wr32(SDHCI + S_INT, 0xffffffff);
	for (unsigned i = 0; i < 512; i++)
		((uint8_t *)buf)[i] = dma_buf[i];
	return 0;
}

static uint8_t extcsd[512];

int emmc_init(void)
{
	int rc;
	if ((rc = gcc_clock())) {
		log_printf("emmc: gcc clock failed %d\n", rc);
		return rc;
	}
	wr32(SDHCI + HC_MODE, 1);
	wr8(SDHCI + S_RST, 1);
	if (wait_bit8(SDHCI + S_RST, 1, 0, 100)) {
		log_printf("emmc: reset stuck\n");
		return -2;
	}
	wr32(SDHCI + HC_MODE, 1);
	wr32(SDHCI + VENDOR_SPEC, rd32(SDHCI + VENDOR_SPEC) | IO_PAD_PWR_SWITCH_EN);
	pwr_ack();	/* anything pending from the reset */
	/* 1.8 V, power on */
	wr8(SDHCI + S_PWR, 0x0b);
	pwr_ack();
	wr32(SDHCI + VENDOR_SPEC, rd32(SDHCI + VENDOR_SPEC) | IO_PAD_PWR_SWITCH);
	log_printf("emmc: pwr %x pres %x vendor %x\n", rd8(SDHCI + S_PWR), rd32(SDHCI + S_PRES),
		   rd32(SDHCI + VENDOR_SPEC));
	/* SDHCI clock: 100 MHz / (2 * 125) = 400 kHz */
	wr16(SDHCI + S_CLK, 0);
	wr16(SDHCI + S_CLK, (125u << 8) | 1);
	uint64_t end = ticks_now() + 19200ULL * 20;
	while (!(rd16(SDHCI + S_CLK) & 2) && ticks_now() < end)
		;
	if (!(rd16(SDHCI + S_CLK) & 2)) {
		log_printf("emmc: internal clock not stable (%x)\n", rd16(SDHCI + S_CLK));
		return -3;
	}
	wr16(SDHCI + S_CLK, rd16(SDHCI + S_CLK) | 4);
	wr8(SDHCI + S_TOUT, 0xe);
	wr32(SDHCI + S_INTEN, 0x01ff00ff);
	wr32(SDHCI + S_SIGEN, 0);
	delay_ms(2);

	uint32_t r[4] = { 0 };
	rc = cmd(0, 0, CF(R_NONE, 0, 0, 0), 0);
	log_printf("emmc: CMD0 %d\n", rc);
	delay_ms(2);
	for (int i = 0; i < 200; i++) {
		rc = cmd(1, 0x40ff8080, CF(R_48, 0, 0, 0), r);
		if (!rc && (r[0] & 0x80000000u))
			break;
		delay_ms(10);
	}
	log_printf("emmc: CMD1 %d ocr %x\n", rc, r[0]);
	if (rc || !(r[0] & 0x80000000u))
		return -4;
	hc_card = !!(r[0] & 0x40000000u);
	rc = cmd(2, 0, CF(R_136, 1, 0, 0), r);
	log_printf("emmc: CMD2 %d cid %x %x %x %x\n", rc, r[3], r[2], r[1], r[0]);
	if (rc)
		return -5;
	rca = 1;
	rc = cmd(3, rca << 16, CF(R_48, 1, 1, 0), r);
	log_printf("emmc: CMD3 %d\n", rc);
	if (rc)
		return -6;
	rc = cmd(7, rca << 16, CF(R_48B, 1, 1, 0), r);
	log_printf("emmc: CMD7 %d st %x\n", rc, r[0]);
	if (rc)
		return -7;
	if (wait_xfer_done(500))	/* R1b busy */
		log_printf("emmc: CMD7 busy wait timed out\n");
	rc = data_read(8, 0, extcsd, 512);
	log_printf("emmc: EXT_CSD read %d\n", rc);
	if (rc)
		return -8;
	sectors = extcsd[212] | extcsd[213] << 8 | extcsd[214] << 16 | (uint32_t)extcsd[215] << 24;
	log_printf("emmc: sec_count %u (%u MiB) rev %u part_cfg %x bus_w %x hs %x boot_size %u KiB\n", sectors,
		   sectors / 2048, extcsd[192], extcsd[179], extcsd[183], extcsd[185], extcsd[226] * 128);
	emmc_up = 1;
	return 0;
}

int emmc_is_up(void)
{
	return emmc_up;
}


/* ---- 8-bit high-speed mode and multi-block reads ---- */
#define XFER_MAX_BLOCKS	2048	/* 1 MiB */
static uint8_t xfer_buf[XFER_MAX_BLOCKS * 512] __attribute__((aligned(64)));
static volatile uint64_t xfer_desc[XFER_MAX_BLOCKS * 512 / 0x8000 + 2] __attribute__((aligned(8)));

/*
 * Read n (1..2048) sectors at lba to physical address dst through ADMA2, CMD18 + auto CMD12.
 * use64: 64-bit ADMA2 (DMA select 3, 12-byte descriptors) so dst and the descriptor table may lie above 4 GiB
 * (XNU's RAM on bonito is entirely above 4 GiB); otherwise 32-bit ADMA2 (8-byte descriptors).
 */
static int xfer_read_x(uint32_t lba, unsigned n, uint64_t dst, uint64_t desc, int use64)
{
	if (!n || n > XFER_MAX_BLOCKS)
		return -20;
	unsigned bytes = n * 512;
	volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)desc;
	for (unsigned off = 0; off < bytes; off += 0x8000) {
		unsigned len = bytes - off > 0x8000 ? 0x8000 : bytes - off;
		uint32_t attr = 0x21 | (off + len >= bytes ? 0x2 : 0);
		uint64_t a = dst + off;
		*d++ = attr | (uint32_t)len << 16;
		*d++ = (uint32_t)a;
		if (use64)
			*d++ = (uint32_t)(a >> 32);
	}
	wr32(SDHCI + S_ADMA_ADDR, (uint32_t)desc);
	if (use64)
		wr32(SDHCI + S_ADMA_ADDR + 4, (uint32_t)(desc >> 32));
	wr8(SDHCI + S_HCTL, (rd8(SDHCI + S_HCTL) & ~0x18) | (use64 ? 0x18 : 0x10));
	wr16(SDHCI + S_BLK, 512);
	wr16(SDHCI + S_BLK + 2, n);
	wr16(SDHCI + S_TOUT, 0xe);
	if (wait_clear32(SDHCI + S_PRES, 3, 100))
		return -10;
	wr32(SDHCI + S_INT, 0xffffffff);
	wr32(SDHCI + S_ARG, hc_card ? lba : lba * 512);
	/* DMA | block count | auto CMD12 | read | multi-block */
	wr32(SDHCI + S_CMD, (18u << 24) | (CF(R_48, 1, 1, 1) << 16) | 0x37);
	uint64_t end = ticks_now() + 19200ULL * 2000;
	uint32_t st;
	for (;;) {
		st = rd32(SDHCI + S_INT);
		if (st & 0x8000)
			break;
		if (st & 2)
			break;
		if (ticks_now() > end) {
			log_printf("emmc: xfer timeout int %x pres %x\n", st, rd32(SDHCI + S_PRES));
			wr8(SDHCI + S_RST, 6);
			wait_bit8(SDHCI + S_RST, 6, 0, 10);
			return -11;
		}
	}
	if (st & 0x8000) {
		log_printf("emmc: xfer error int %x adma_err %x\n", st, rd32(SDHCI + S_ADMA_ERR));
		wr32(SDHCI + S_INT, st);
		wr8(SDHCI + S_RST, 6);
		wait_bit8(SDHCI + S_RST, 6, 0, 10);
		return -12;
	}
	wr32(SDHCI + S_INT, 0xffffffff);
	return 0;
}

static int xfer_read(uint32_t lba, unsigned n)
{
	return xfer_read_x(lba, n, (uint64_t)(uintptr_t)xfer_buf, (uint64_t)(uintptr_t)xfer_desc, 0);
}

/* 64-bit ADMA2 into/through RAM above 4 GiB (descriptor table and data), compared with a low-memory read. */
int emmc_test_high(void)
{
	const uint64_t desc = 0x110000000ULL, dst = 0x110100000ULL;
	volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)dst;
	for (unsigned i = 0; i < 4096; i++)
		p[i] = 0xa5;
	int rc = xfer_read_x(1, 8, dst, desc, 1);	/* GPT header + first entries, 4 KiB */
	if (rc)
		return rc;
	rc = xfer_read(1, 8);
	if (rc)
		return rc;
	unsigned bad = 0, a5 = 0;
	for (unsigned i = 0; i < 4096; i++) {
		if (p[i] != xfer_buf[i])
			bad++;
		if (p[i] == 0xa5)
			a5++;
	}
	log_printf("emmc: ADMA64 high RAM read: %u bytes differ, %u untouched, first %x %x\n", bad, a5, p[0], p[1]);
	return bad ? -40 : 0;
}

static int switch_ext(uint32_t index, uint32_t value)
{
	uint32_t r[4];
	int rc = cmd(6, (3u << 24) | (index << 16) | (value << 8) | 1, CF(R_48B, 1, 1, 0), r);
	if (rc)
		return rc;
	if (wait_xfer_done(500))
		return -30;
	rc = cmd(13, rca << 16, CF(R_48, 1, 1, 0), r);
	log_printf("emmc: SWITCH[%u]=%u status %x rc %d\n", index, value, r[0], rc);
	return rc ? rc : (r[0] & 0x80 ? -31 : 0);	/* bit 7 = SWITCH_ERROR */
}

/* High speed timing, 8-bit bus, 50 MHz. */
int emmc_fast(void)
{
	int rc = switch_ext(185, 1);	/* HS_TIMING = high speed */
	if (rc)
		return rc;
	wr16(SDHCI + S_CLK, 0);
	wr16(SDHCI + S_CLK, (1u << 8) | 1);	/* 100 MHz / (2 * 1) = 50 MHz */
	uint64_t end = ticks_now() + 19200ULL * 20;
	while (!(rd16(SDHCI + S_CLK) & 2) && ticks_now() < end)
		;
	wr16(SDHCI + S_CLK, rd16(SDHCI + S_CLK) | 4);
	wr8(SDHCI + S_HCTL, rd8(SDHCI + S_HCTL) | 0x04);	/* high speed enable */
	delay_ms(2);
	rc = switch_ext(183, 2);	/* BUS_WIDTH = 8-bit SDR */
	if (rc)
		return rc;
	wr8(SDHCI + S_HCTL, rd8(SDHCI + S_HCTL) | 0x20);	/* host 8-bit */
	delay_ms(2);
	return 0;
}

/* Speed test: read total_mib MiB in 1 MiB requests; returns kB/s, or negative error. */
int emmc_bench(uint32_t lba, unsigned total_mib)
{
	uint64_t t0 = ticks_now();
	for (unsigned i = 0; i < total_mib; i++) {
		int rc = xfer_read(lba + i * XFER_MAX_BLOCKS, XFER_MAX_BLOCKS);
		if (rc)
			return rc;
	}
	uint64_t us = (ticks_now() - t0) * 1000000ULL / 19200000ULL;
	return (int)((uint64_t)total_mib * 1024 * 1000000ULL / (us ? us : 1));
}

const uint8_t *emmc_xfer_buf(void)
{
	return xfer_buf;
}

int emmc_read_multi(uint32_t lba, unsigned n)
{
	return xfer_read(lba, n);
}

/* ---- writes (scratch log partition only) and GPT lookup ---- */
static uint32_t wr_lo, wr_n;	/* the only sectors emmc_write() will touch */

/* Find a GPT partition by (ASCII) name. Returns 0 and its first LBA / sector count. */
int emmc_find_partition(const char *want, uint32_t *first, uint32_t *count)
{
	static uint8_t sec[512], ent[512];
	int rc = emmc_read(1, sec);
	if (rc || sec[0] != 'E')
		return -50;
	uint32_t elba = sec[72] | sec[73] << 8 | sec[74] << 16 | (uint32_t)sec[75] << 24;
	uint32_t n = sec[80] | sec[81] << 8 | sec[82] << 16 | (uint32_t)sec[83] << 24;
	for (uint32_t i = 0; i < n; i++) {
		if (!(i & 3) && emmc_read(elba + i / 4, ent))
			return -51;
		const uint8_t *e = ent + (i & 3) * 128;
		unsigned k = 0;
		for (; want[k] && e[56 + 2 * k] == (uint8_t)want[k] && !e[57 + 2 * k]; k++)
			;
		if (want[k] || e[56 + 2 * k])
			continue;
		uint32_t f = e[32] | e[33] << 8 | e[34] << 16 | (uint32_t)e[35] << 24;
		uint32_t l = e[40] | e[41] << 8 | e[42] << 16 | (uint32_t)e[43] << 24;
		*first = f;
		*count = l - f + 1;
		return 0;
	}
	return -52;
}

/* CMD25 multi-block write of n sectors from xfer_buf through 32-bit ADMA2. Bounds are checked here, every time. */
static int xfer_write(uint32_t lba, unsigned n)
{
	if (!n || n > XFER_MAX_BLOCKS || !wr_n || lba < wr_lo || lba - wr_lo >= wr_n || n > wr_n - (lba - wr_lo)) {
		log_printf("emmc: refusing write of %u at %u\n", n, lba);
		return -60;
	}
	unsigned bytes = n * 512;
	volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)xfer_desc;
	for (unsigned off = 0; off < bytes; off += 0x8000) {
		unsigned len = bytes - off > 0x8000 ? 0x8000 : bytes - off;
		*d++ = (0x21 | (off + len >= bytes ? 0x2 : 0)) | (uint32_t)len << 16;
		*d++ = (uint32_t)(uintptr_t)(xfer_buf + off);
	}
	wr32(SDHCI + S_ADMA_ADDR, (uint32_t)(uintptr_t)xfer_desc);
	wr8(SDHCI + S_HCTL, (rd8(SDHCI + S_HCTL) & ~0x18) | 0x10);
	wr16(SDHCI + S_BLK, 512);
	wr16(SDHCI + S_BLK + 2, n);
	wr16(SDHCI + S_TOUT, 0xe);
	if (wait_clear32(SDHCI + S_PRES, 3, 100))
		return -10;
	wr32(SDHCI + S_INT, 0xffffffff);
	wr32(SDHCI + S_ARG, hc_card ? lba : lba * 512);
	/* DMA | block count | auto CMD12 | multi-block, host -> card (bit 4 clear) */
	wr32(SDHCI + S_CMD, (25u << 24) | (CF(R_48, 1, 1, 1) << 16) | 0x27);
	uint64_t end = ticks_now() + 19200ULL * 3000;
	uint32_t st;
	for (;;) {
		st = rd32(SDHCI + S_INT);
		if (st & 0x8000)
			break;
		if (st & 2)
			break;
		if (ticks_now() > end) {
			log_printf("emmc: write timeout int %x pres %x\n", st, rd32(SDHCI + S_PRES));
			wr8(SDHCI + S_RST, 6);
			wait_bit8(SDHCI + S_RST, 6, 0, 10);
			return -11;
		}
	}
	if (st & 0x8000) {
		log_printf("emmc: write error int %x adma_err %x\n", st, rd32(SDHCI + S_ADMA_ERR));
		wr32(SDHCI + S_INT, st);
		wr8(SDHCI + S_RST, 6);
		wait_bit8(SDHCI + S_RST, 6, 0, 10);
		return -12;
	}
	wr32(SDHCI + S_INT, 0xffffffff);
	/* wait until the card has finished programming: CMD13 state == transfer (4), READY_FOR_DATA */
	end = ticks_now() + 19200ULL * 3000;
	for (;;) {
		uint32_t r[4] = { 0 };
		if (!cmd(13, rca << 16, CF(R_48, 1, 1, 0), r) && (r[0] & 0x100) && ((r[0] >> 9) & 0xf) == 4)
			return 0;
		if (ticks_now() > end)
			return -13;
	}
}

/* Save a text log into the named scratch partition: sector 0 header "BNTLOG01", len, seq; text from sector 1. */
int emmc_save_log(const char *part, const char *text, uint32_t len)
{
	uint32_t first, count;
	int rc = emmc_find_partition(part, &first, &count);
	if (rc)
		return rc;
	if (count < 8)
		return -61;
	wr_lo = first;
	wr_n = count;
	uint32_t room = (count - 1) * 512;
	if (len > room)
		len = room;
	for (unsigned i = 0; i < 512; i++)
		xfer_buf[i] = 0;
	static const char magic[8] = "BNTLOG01";
	for (unsigned i = 0; i < 8; i++)
		xfer_buf[i] = (uint8_t)magic[i];
	xfer_buf[8] = len;
	xfer_buf[9] = len >> 8;
	xfer_buf[10] = len >> 16;
	xfer_buf[11] = len >> 24;
	rc = xfer_write(first, 1);
	for (uint32_t done = 0; !rc && done < len; ) {
		uint32_t chunk = len - done > XFER_MAX_BLOCKS * 512 ? XFER_MAX_BLOCKS * 512 : len - done;
		uint32_t secs = (chunk + 511) / 512;
		for (uint32_t i = 0; i < secs * 512; i++)
			xfer_buf[i] = i < chunk ? (uint8_t)text[done + i] : 0;
		rc = xfer_write(first + 1 + done / 512, secs);
		done += chunk;
	}
	return rc;
}

uint32_t emmc_sectors(void)
{
	return sectors;
}

int emmc_read(uint32_t lba, void *buf)
{
	return data_read(17, hc_card ? lba : lba * 512, buf, 512);
}
