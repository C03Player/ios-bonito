#ifndef MEMSHARE_H
#define MEMSHARE_H
#include "link.h"
#define MEMSHARE_SERVICE	0x34
#define MEMSHARE_PORT		0x13
int memshare_init(void);
void memshare_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n);
#endif
