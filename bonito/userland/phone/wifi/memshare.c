/*
 * memshare: the modem's heap extension (QMI service 0x34, downstream drivers/soc/qcom/memshare/msm_memshare.c). The modem's dynamic heap
 * (dynamic_heap.c, "get_heap_memory: HLOS returned error" without this server) and its diag/GPS clients ask the apps side for DDR blocks:
 * a block is allocated, handed to the modem's VM (hyp_assign, as rmtfs does) and its physical address sent back. The clients and sizes are the
 * stock device tree's qcom,memshare nodes of this phone: client 0 (GPS) and client 2: size 0, client 1: 5 MiB, allocated at boot; a request
 * for a client whose size is 0 fails with NO_MEMORY, as the downstream driver does.
 */
#include <stdio.h>
#include <string.h>
#include "link.h"
#include "memshare.h"
#include "pmem.h"
#include "qmi.h"
#include "scm.h"

#define MEM_ALLOC		0x20
#define MEM_FREE		0x21
#define MEM_ALLOC_GENERIC	0x22
#define MEM_FREE_GENERIC	0x23
#define MEM_QUERY_SIZE		0x24
#define GUARD			0x1000u

static struct {
	uint32_t id, init_size;
	uint64_t pa;		/* the block (without the guard) once allocated */
	uint32_t size;
} cl[] = {
	{ .id = 0 }, { .id = 1, .init_size = 0x500000 }, { .id = 2 },
};
#define NCL (sizeof(cl) / sizeof(cl[0]))

static int find(uint32_t id)
{
	for (unsigned i = 0; i < NCL; i++)
		if (cl[i].id == id)
			return (int)i;
	return -1;
}

/* One block per client per phone boot, kept for the modem's whole life (a fresh hyp_assign after each restart would never return, see rmtfs.c). */
static int block_for(int i, uint32_t want)
{
	if (cl[i].pa)
		return 0;
	uint32_t size = want < cl[i].init_size ? want : cl[i].init_size;
	if (!size)
		return -1;
	uint64_t a = pm_alloc((uint64_t)size + 2 * GUARD, 0x1000);
	if (!a)
		return -1;
	static const uint32_t src[] = { VMID_HLOS }, dst[] = { VMID_HLOS, VMID_MSS_MSA };
	if (hyp_assign(a + GUARD, size, src, 1, dst, 2))
		return -1;
	cl[i].pa = a + GUARD;
	cl[i].size = size;
	return 0;
}

int memshare_init(void)
{
	if (qrtr_new_server(MEMSHARE_SERVICE, 1, 1, MEMSHARE_PORT))
		return -1;
	printf("memshare: serving at %u:%x\n", QRTR_NODE_APPS, MEMSHARE_PORT);
	return 0;
}

static void reply(const struct qrtr_hdr *h, struct qmi_buf *q)
{
	qrtr_send(QRTR_TYPE_DATA, MEMSHARE_PORT, h->src_node, h->src_port, q->b, (uint32_t)qmi_finish(q));
}

void memshare_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	struct qmi_buf q;
	if (qmi_parse(d, n, &m) || m.type != QMI_REQUEST)
		return;
	qmi_start(&q, QMI_RESPONSE, m.txn, m.id);
	uint32_t bytes = 0, client = 0, proc = 0, seq = 0;
	switch (m.id) {
	case MEM_ALLOC_GENERIC: {
		qmi_get_u32(&m, 1, &bytes);
		qmi_get_u32(&m, 2, &client);
		qmi_get_u32(&m, 3, &proc);
		qmi_get_u32(&m, 4, &seq);
		int i = find(client), rc = -1;
		if (i >= 0)
			rc = block_for(i, bytes);
		printf("memshare: alloc client %u proc %u seq %u bytes %u -> %s", client, proc, seq, bytes,
		       i < 0 ? "unknown client" : rc ? "no memory" : "ok");
		if (!rc)
			printf(" (%llx, %u bytes)", (unsigned long long)cl[i].pa, cl[i].size);
		printf("\n");
		uint8_t info[13] = { 1 };	/* one entry: u64 address, u32 size; the downstream driver sends the client's block, or zeros, either way */
		uint64_t pa = (i >= 0 && !rc) ? cl[i].pa : 0;
		uint32_t sz = (i >= 0 && !rc) ? cl[i].size : 0;
		memcpy(info + 1, &pa, 8);
		memcpy(info + 9, &sz, 4);
		qmi_tlv_result(&q, rc ? 1 : 0, rc ? 2 : 0);
		qmi_tlv_u32(&q, 0x10, seq);
		qmi_tlv(&q, 0x11, info, sizeof(info));
		break;
	}
	case MEM_QUERY_SIZE: {
		qmi_get_u32(&m, 1, &client);
		qmi_get_u32(&m, 0x10, &proc);
		int i = find(client);
		printf("memshare: query size client %u proc %u -> %u\n", client, proc, i >= 0 ? cl[i].init_size : 0);
		qmi_tlv_result(&q, 0, 0);
		qmi_tlv_u32(&q, 0x10, i >= 0 ? cl[i].init_size : 0);
		break;
	}
	case MEM_FREE_GENERIC:
		printf("memshare: free (generic), kept\n");
		qmi_tlv_result(&q, 0, 0);
		break;
	case MEM_ALLOC: {
		/* the old GPS-only request: a result enum (TLV 1, 16 bit) and no block */
		qmi_get_u32(&m, 1, &bytes);
		printf("memshare: legacy alloc %u bytes -> refused\n", bytes);
		uint16_t r = 1;
		qmi_tlv(&q, 1, &r, 2);
		break;
	}
	case MEM_FREE: {
		printf("memshare: legacy free\n");
		uint16_t r = 0;
		qmi_tlv(&q, 1, &r, 2);
		break;
	}
	default:
		printf("memshare: unknown message %#x\n", m.id);
		qmi_dump(&m);
		qmi_tlv_result(&q, 1, 0x47);
		break;
	}
	reply(h, &q);
}
