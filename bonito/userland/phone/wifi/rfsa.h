#ifndef RFSA_H
#define RFSA_H
#include "link.h"
#define RFSA_SERVICE	0x1c
#define RFSA_PORT	0x14
int rfsa_init(void);
void rfsa_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n);
#endif
