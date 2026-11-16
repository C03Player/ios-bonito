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
	return 0;
}


/* ---- 8-bit high-speed mode and multi-block reads ---- */
#define XFER_MAX_BLOCKS	2048	/* 1 MiB */
static uint8_t xfer_buf[XFER_MAX_BLOCKS * 512] __attribute__((aligned(64)));
static volatile uint64_t xfer_desc[XFER_MAX_BLOCKS * 512 / 0x8000 + 1] __attribute__((aligned(8)));

/* Read n (1..2048) sectors starting at lba into xfer_buf through ADMA2 with CMD18 + auto CMD12. */
static int xfer_read(uint32_t lba, unsigned n)
{
	if (!n || n > XFER_MAX_BLOCKS)
		return -20;
	unsigned bytes = n * 512, d = 0;
	for (unsigned off = 0; off < bytes; off += 0x8000, d++) {
		unsigned len = bytes - off > 0x8000 ? 0x8000 : bytes - off;
		uint64_t attr = 0x21 | (off + len >= bytes ? 0x2 : 0);
		xfer_desc[d] = (uint64_t)(uint32_t)(uintptr_t)(xfer_buf + off) << 32 | (uint64_t)len << 16 | attr;
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

uint32_t emmc_sectors(void)
{
	return sectors;
}

int emmc_read(uint32_t lba, void *buf)
{
	return data_read(17, hc_card ? lba : lba * 512, buf, 512);
}
