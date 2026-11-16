#pragma once
#include <stdint.h>

/* Bring the eMMC up (SDHCI 0x7c4000, SDCC1) from the state ABL leaves it in and read sectors. */
int emmc_init(void);
int emmc_read(uint32_t lba, void *buf);	/* one 512-byte sector, 1-bit PIO, read-only */
uint32_t emmc_sectors(void);
int emmc_fast(void);	/* HS timing, 8-bit bus, 50 MHz */
int emmc_bench(uint32_t lba, unsigned total_mib);	/* kB/s or negative error */
const uint8_t *emmc_xfer_buf(void);
int emmc_read_multi(uint32_t lba, unsigned n);	/* n <= 2048 sectors into emmc_xfer_buf() */
