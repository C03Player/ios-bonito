/*
 * RFSA (QMI service 0x1c, version 1, instance 1): the downstream kernel's msm_sharedmem driver (drivers/uio/msm_sharedmem/sharedmem_qmi.c). The modem
 * asks for the address of a shared DDR buffer by client id and size (QMI_RFSA_GET_BUFF_ADDR, message 0x23: TLV 1 client_id, TLV 2 size) right after it
 * starts, and the kernel answers with the buffer registered for that client: the stock device tree has qcom,rmtfs_sharedmem, client id 1, the 2 MB
 * buffer that rmt_storage also uses (rmtfs.c). The reply is the result (TLV 2) and the address (TLV 0x10, u64). A healthy Android boot serves it at
 * +0.0 s; without it the modem's RFSA client (mcfg_remote_rfsa_*) gets no buffer.
 */
#include <stdio.h>
#include <string.h>
#include "link.h"
#include "qmi.h"
#include "rfsa.h"
#include "rmtfs.h"

#define RFSA_GET_BUFF_ADDR	0x23
#define CLIENT_RMTFS		1

int rfsa_init(void)
{
	if (qrtr_new_server(RFSA_SERVICE, 1, 1, RFSA_PORT))
		return -1;
	printf("rfsa: serving at %u:%x\n", QRTR_NODE_APPS, RFSA_PORT);
	return 0;
}

void rfsa_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	struct qmi_buf q;
	if (qmi_parse(d, n, &m) || m.type != QMI_REQUEST)
		return;
	qmi_start(&q, QMI_RESPONSE, m.txn, m.id);
	if (m.id != RFSA_GET_BUFF_ADDR) {
		printf("rfsa: unknown message %#x\n", m.id);
		qmi_dump(&m);
		qmi_tlv_result(&q, 1, 0x47);
	} else {
		uint32_t client = 0, size = 0, bsize = 0;
		uint64_t pa = 0;
		qmi_get_u32(&m, 1, &client);
		qmi_get_u32(&m, 2, &size);
		int ok = client == CLIENT_RMTFS && size && !rmtfs_buffer(&pa, &bsize) && size <= bsize;
		printf("rfsa: get buffer client %u size %u -> %s", client, size, ok ? "ok" : "refused");
		if (ok)
			printf(" (%llx)", (unsigned long long)pa);
		printf("\n");
		qmi_tlv_result(&q, ok ? 0 : 1, ok ? 0 : 3);
		if (ok)
			qmi_tlv_u64(&q, 0x10, pa);
	}
	qrtr_send(QRTR_TYPE_DATA, RFSA_PORT, h->src_node, h->src_port, q.b, (uint32_t)qmi_finish(&q));
}
