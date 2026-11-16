#pragma once
#include <stdint.h>

/* Bring the eMMC up (SDHCI 0x7c4000, SDCC1) from the state ABL leaves it in and read sectors. */
int emmc_init(void);
int emmc_read(uint32_t lba, void *buf);	/* one 512-byte sector, 1-bit PIO, read-only */
uint32_t emmc_sectors(void);
