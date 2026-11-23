/*
 * wland: WLAN bring-up for the Pixel 4 from userland (/dev/pmem), the way
 * the GPU was brought up: the modem DSP hosts the WLAN firmware (icnss /
 * WCN3990 family), so: power votes, PAS-boot the modem, SMP2P, then GLINK +
 * QRTR + QMI to the WLFW service, then the WLAN copy engines for WMI.
 *
 *   wland smem              SMEM partitions and the items we care about
 *   wland smp2p             modem SMP2P state (in: 435, out: 428)
 *   wland vote RES LVL      RPMh ARC level vote (index) for RES (e.g. mss.lvl)
 *   wland levels RES        cmd-db levels of an ARC resource
 *   wland bcm RES X [Y]     RPMh BCM (bus bandwidth) vote, e.g. MC0 (DDR) / SH0 (memory NoC); 0x3fff = top
 *   wland smp2p-init        our outbound SMP2P item + doorbell; shows the modem's entries
 *   wland qmp MSG           message to the AOP over QMP (opens the link if needed)
 *   wland shutdown          PAS shutdown of the modem
 *   wland boot              power votes + PAS boot of the modem + wait for ready
 *   wland smmu              the WLAN DMA stream through a translation-off SMMU bank (radio.c)
 *   wland up [SECS]         all of it from a fresh boot, then the radio: copy engines, WMI, passive scans
 *   wland auto              the boot job (launchd org.iosonpixel.wland): `up 0` once per phone boot, log in
 *                           /var/local/wland.log (last boot's: .prev); off with /var/local/wland.off or sweetwifi=0
 * While joined, SIGUSR1 drops the link (tests the rejoin).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include "pmem.h"
#include "scm.h"
#include "smem.h"
#include "link.h"
#include "rmtfs.h"
#include "pdmapper.h"
#include "tftp.h"
#include "memshare.h"
#include "wlfw.h"
#include "radio.h"
#include "ipnet.h"
#include "bridge.h"

/* cmd-db aux levels (as in the GPU's arc votes) */
/* Every cmd-db entry: name, RPMh slave (3 ARC, 4 VRM, 5 BCM), address. */
static void cmddb_list(void)
{
	uint64_t db = rd32(0x0c3f000cull);
	if (rd32(db + 4) != 0x0c0330db)
		return;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			printf("%-8s slave %u addr %05x\n", name, w0 & 0xffff, rd32(ent + 16));
		}
	}
}

static int cmddb_aux(const char *id, uint64_t *aux, uint32_t *len)
{
	uint64_t db = rd32(0x0c3f000cull);
	if (rd32(db + 4) != 0x0c0330db)
		return -1;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			if (strncmp(name, id, 8))
				continue;
			uint32_t w5 = rd32(ent + 20);
			*len = w5 & 0xffff;
			*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

static uint32_t aux16(uint64_t aux, uint32_t i)
{
	return (rd32(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff;
}

static int levels(const char *res, int print)
{
	uint64_t aux;
	uint32_t len;
	if (cmddb_aux(res, &aux, &len)) {
		fprintf(stderr, "%s: not in cmd-db\n", res);
		return -1;
	}
	if (print) {
		printf("%s:", res);
		for (uint32_t i = 0; i < len / 2; i++)
			printf(" [%u]=%u", i, aux16(aux, i));
		printf("\n");
	}
	return (int)(len / 2);
}

static int vote(const char *res, uint32_t level_index)
{
	uint32_t off[1] = { 0 }, val[1] = { level_index };
	int rc = pm_rpmh(res, 1, off, val);
	printf("vote %s level index %u -> %d\n", res, level_index, rc);
	return rc;
}

/*
 * The WLAN supplies (icnss in the stock DT: vdd-cx-mx, vdd-1.8-xo, vdd-1.3-rfa, vdd-3.3-ch0), RPMh VRM
 * votes as Linux qcom-rpmh-regulator: voltage (mV) at 0, enable at 4, mode at 8 (PMIC5 LDO HPM = 7).
 * Without them the WLAN firmware takes an exception once calibration starts (after the cal report).
 */
static int qmp_send(const char *msg);

static int wlan_power(void)
{
	static const struct { const char *res; uint32_t mv; } ldo[] = {
		/* bonito (stock DT icnss): PM660 L5 0.8 V cx-mx, L9 1.8 V xo, L6 1.3 V rfa, L19 3.0-3.3 V ch0 */
		{ "ldoa5", 800 }, { "ldoa9", 1800 }, { "ldoa6", 1304 }, { "ldoa19", 3104 },
	};
	for (unsigned i = 0; i < sizeof(ldo) / sizeof(ldo[0]); i++) {
		uint32_t off[3] = { 0, 8, 4 }, val[3] = { ldo[i].mv, 7, 1 };
		int rc = pm_rpmh(ldo[i].res, 3, off, val);
		printf("wlan power: %s %u mV on -> %d\n", ldo[i].res, ldo[i].mv, rc);
		if (rc)
			return -1;
	}
	/* the WCN399x reference clock (mainline: clocks = <&rpmhcc RPMH_RF_CLK2>, "cxo_ref_clk_pin"): VRM enable */
	uint32_t off[1] = { 4 }, val[1] = { 1 };
	int rc = pm_rpmh("rfclka2", 1, off, val);
	printf("wlan power: rfclka2 on -> %d\n", rc);
	if (rc)
		return -1;
	/* mainline sm8150 wifi: clocks "cxo_ref_clk_pin", "qdss" = <&aoss_qmp>: the QDSS clock is an AOSS message */
	return qmp_send("{class: clock, res: qdss, val: 1}") ? -1 : 0;
}

/*
 * The rails the stock device tree marks qcom,proxy-consumer-enable (the AP's kernel switches them on at boot for the modem until it takes over): PM660 L1 1.2 V, PM660 L11 1.8 V and
 * PM660L L1 0.88 V. Without them the modem's RF task took an exception about 0.9 s after it was ready ("[rf]EX:kernel:0:rf:171:PC=d949b6d4" in SMEM item 421).
 */
static int modem_rails(void)
{
	static const struct { const char *res; uint32_t mv; } ldo[] = { { "ldoa1", 1200 }, { "ldoa11", 1800 }, { "ldob1", 880 } };
	for (unsigned i = 0; i < sizeof(ldo) / sizeof(ldo[0]); i++) {
		uint32_t off[3] = { 0, 8, 4 }, val[3] = { ldo[i].mv, 7, 1 };
		int rc = pm_rpmh(ldo[i].res, 3, off, val);
		printf("modem rails: %s %u mV on -> %d\n", ldo[i].res, ldo[i].mv, rc);
	}
	return 0;
}

/* SMP2P item: magic "$SMP", version/features, local/remote pid, total/valid entries, flags, entries[16+4] */
static void smp2p_show(const char *what, unsigned item)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(SMEM_HOST_MODEM, item, &pa, &sz)) {
		printf("smp2p %s (item %u): absent\n", what, item);
		return;
	}
	uint32_t w1 = rd32(pa + 4), pids = rd32(pa + 8), counts = rd32(pa + 12);
	printf("smp2p %s (item %u) at %llx: magic %08x ver %u feat %x pids %u->%u entries %u/%u flags %x\n", what, item,
	       (unsigned long long)pa, rd32(pa), w1 & 0xff, w1 >> 8, pids & 0xffff, pids >> 16, counts >> 16,
	       counts & 0xffff, rd32(pa + 16));
	for (uint32_t i = 0; i < (counts >> 16) && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char name[17];
		for (int k = 0; k < 16; k++)
			name[k] = (char)(rd32(e + (k & ~3)) >> (8 * (k & 3)));
		name[16] = 0;
		printf("  entry %u: %-16s value %08x\n", i, name, rd32(e + 16));
	}
}

/* ---- SMP2P outbound (Linux qcom_smp2p_alloc_outbound_item) and the APCS doorbell ---- */

#define APCS_IPC	0x1799000cull	/* bonito: apcs_glb + 12, write BIT(n): AOP 0, glink modem 12, smp2p modem 14 (stock DT) */
#define IPC_MODEM_SMP2P	14
#define IPC_MODEM_GLINK	12
#define SMP2P_OUT	428
#define SMP2P_IN	435

static void kick(unsigned bit)
{
	wr32(APCS_IPC, 1u << bit);
}

static void wr_name(uint64_t pa, const char *name)
{
	char n[16] = { 0 };
	strncpy(n, name, sizeof(n));
	for (int i = 0; i < 16; i += 4) {
		uint32_t w;
		memcpy(&w, n + i, 4);
		wr32(pa + i, w);
	}
}

static uint64_t smp2p_out_pa;

/* Our outbound item (Linux allocates it at probe, before the modem boots: the modem needs it). */
static int smp2p_create(void)
{
	uint64_t pa;
	if (smem_alloc(SMEM_HOST_MODEM, SMP2P_OUT, 20 + 16 * 20, &pa)) {
		fprintf(stderr, "smp2p: cannot allocate item %u\n", SMP2P_OUT);
		return -1;
	}
	if (rd32(pa) != 0x504d5324) {
		wr32(pa + 8, 0 | (1u << 16));		/* local pid 0, remote pid 1 */
		wr32(pa + 12, 16 | (1u << 16));		/* total 16 entries, 1 valid */
		wr32(pa + 16, 0);			/* flags */
		wr_name(pa + 20, "master-kernel");
		wr32(pa + 36, 0);
		wr32(pa + 0, 0x504d5324);		/* "$SMP" */
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(pa + 4, 1 | (1u << 8));		/* version 1 last, features: SSR ack */
	}
	smp2p_out_pa = pa;
	kick(IPC_MODEM_SMP2P);
	return 0;
}

/* Address of the value of the modem's inbound entry NAME (0 = found). */
static int smp2p_in_entry(const char *name, uint64_t *va)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(SMEM_HOST_MODEM, SMP2P_IN, &pa, &sz))
		return -1;
	uint32_t n = rd32(pa + 12) >> 16;
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char nm[17];
		for (int k = 0; k < 16; k++)
			nm[k] = (char)(rd32(e + (k & ~3)) >> (8 * (k & 3)));
		nm[16] = 0;
		if (!strcmp(nm, name)) {
			*va = e + 16;
			return 0;
		}
	}
	return -1;
}

static int smp2p_in_value(const char *name, uint32_t *v)
{
	uint64_t va;
	if (smp2p_in_entry(name, &va))
		return -1;
	*v = rd32(va);
	return 0;
}

/*
 * Keep the SMP2P side going: SSR ack (Linux qcom_smp2p_do_ssr_ack: after a
 * restart the modem flips RESTART_DONE, flags bit 0, and waits for the same
 * in our RESTART_ACK, bit 1), and report changes of the modem's state bits
 * (slave-kernel: 0 err-fatal, 1 ready, 2 handover, 3 stop-ack).
 */
static uint32_t modem_state = 0xffffffff;

/* the modem's crash reason: SMEM item 421 (Linux qcom_q6v5: qcom_smem_get(HOST_ANY, 421)), a text of file:line and message */
static void dump_crash_reason(void)
{
	for (int k = 0; k < 2; k++) {
		unsigned host = k ? SMEM_HOST_MODEM : SMEM_HOST_GLOBAL;
		uint64_t pa;
		uint32_t sz;
		if (smem_get(host, 421, &pa, &sz))
			continue;
		char txt[257] = "";
		for (uint32_t o = 0; o < sz && o < 256; o += 4) {
			uint32_t w = rd32(pa + o);
			for (int b = 0; b < 4 && o + b < 256; b++) {
				uint8_t c = (uint8_t)(w >> (8 * b));
				txt[o + b] = (c >= 32 && c < 127) ? (char)c : (c ? '?' : 0);
			}
		}
		printf("crash reason (smem 421, host %#x, %u bytes): \"%s\"\n", host, sz, txt);
		return;
	}
	printf("crash reason: smem item 421 not found\n");
}

/*
 * After ERR-FATAL: the modem's error handler keeps its crash context in the modem's DDR as well (the message text and the registers beside it). Scan the modem region (152 MiB) for
 * "EX:kernel" and save 512 KiB around each hit (384 KiB before, 128 KiB after; up to 4) to /var/local/modem-crash-N.bin, to read in the recovery.
 */
static uint64_t now_ms(void);
static void verify_modem_image(void);
static void dump_minidump(void);
static void dump_modem_ddr(void);

static void dump_crash_memory(void)
{
	const uint64_t base = 0x8b000000ull, size = 0x09800000ull;	/* the modem region (the B4 overlay), see base below */
	static uint8_t chunk[0x40000 + 64];
	static const char needle[] = "EX:kernel";
	int hits = 0;
	uint64_t t0 = now_ms();
	for (uint64_t o = 0; o < size && hits < 4; o += 0x40000) {
		uint64_t n = size - o < 0x40000 ? size - o : 0x40000;
		if (pm_rd(base + o, chunk, n))
			continue;
		for (uint64_t i = 0; i + sizeof(needle) - 1 <= n; i++) {
			if (chunk[i] != 'E' || memcmp(chunk + i, needle, sizeof(needle) - 1))
				continue;
			uint64_t at = base + o + i, from = (at & ~0xfffull) - 0x60000;
			static uint8_t buf[0x80000];
			if (from < base)
				from = base;
			if (from + sizeof(buf) > base + size)
				from = base + size - sizeof(buf);
			char path[64];
			snprintf(path, sizeof(path), "/var/local/modem-crash-%d.bin", hits);
			if (!pm_rd(from, buf, sizeof(buf))) {
				FILE *f = fopen(path, "wb");
				if (f) {
					fwrite(buf, 1, sizeof(buf), f);
					fclose(f);
				}
			}
			printf("crash memory: \"EX:kernel\" at %llx, 512 KiB from %llx saved to %s\n", (unsigned long long)at, (unsigned long long)from, path);
			hits++;
			i += 0xfff;
			if (hits == 4)
				break;
		}
	}
	printf("crash memory: scan of the modem region done in %llu ms, %d hit(s)\n", (unsigned long long)(now_ms() - t0), hits);
}

static void smp2p_service(void)
{
	uint64_t ip;
	uint32_t sz, v;
	if (!smp2p_out_pa && smem_get(SMEM_HOST_MODEM, SMP2P_OUT, &smp2p_out_pa, &sz))
		return;
	if (smem_get(SMEM_HOST_MODEM, SMP2P_IN, &ip, &sz) == 0) {
		uint32_t want = (rd32(ip + 16) & 1) ? 2 : 0;
		if ((rd32(smp2p_out_pa + 16) & 2) != want) {
			wr32(smp2p_out_pa + 16, (rd32(smp2p_out_pa + 16) & ~2u) | want);
			kick(IPC_MODEM_SMP2P);
			printf("smp2p: restart %s\n", want ? "acked" : "ack cleared");
		}
	}
	if (smp2p_in_value("slave-kernel", &v) == 0 && v != modem_state) {
		printf("[%6llu] smp2p: modem state %x%s%s%s%s\n", (unsigned long long)link_ms(), v, v & 1 ? " ERR-FATAL" : "", v & 2 ? " ready" : "",
		       v & 4 ? " handover" : "", v & 8 ? " stop-ack" : "");
		if ((v & 1) && !(modem_state != 0xffffffff && (modem_state & 1))) {
			dump_crash_reason();
			dump_minidump();
			verify_modem_image();
			dump_crash_memory();
			dump_modem_ddr();
		}
		modem_state = v;
	}
}

static int smp2p_init(void)
{
	if (smp2p_create())
		return -1;
	printf("smp2p: outbound item %u at %llx ready, modem kicked\n", SMP2P_OUT, (unsigned long long)smp2p_out_pa);
	for (int t = 0; t < 50; t++) {
		smp2p_service();
		usleep(100000);
	}
	smp2p_show("in", SMP2P_IN);
	return 0;
}

/* ---- AOSS QMP (Linux qcom_aoss.c): messages to the always-on processor ---- */

#define QMP_RAM		0x0c300000ull
#define IPC_AOSS	0
#define QMP_UP		0x0000ffffu

static int wait_eq(uint64_t pa, uint32_t want, int ms)
{
	for (int i = 0; i < ms; i++) {
		if (rd32(pa) == want)
			return 0;
		usleep(1000);
	}
	return -1;
}

static int qmp_open(void)
{
	if (rd32(QMP_RAM) != 0x4d41494c || rd32(QMP_RAM + 4) != 1) {
		fprintf(stderr, "qmp: magic %08x version %u\n", rd32(QMP_RAM), rd32(QMP_RAM + 4));
		return -1;
	}
	if (rd32(QMP_RAM + 0x24) == QMP_UP && rd32(QMP_RAM + 0x2c) == QMP_UP && rd32(QMP_RAM + 0x30) == QMP_UP)
		return 0;	/* already open */
	wr32(QMP_RAM + 0x10, rd32(QMP_RAM + 0x0c));	/* ack ucore link state */
	wr32(QMP_RAM + 0x24, QMP_UP);			/* mcore link up */
	kick(IPC_AOSS);
	if (wait_eq(QMP_RAM + 0x28, QMP_UP, 1000)) {
		fprintf(stderr, "qmp: link not acked\n");
		return -1;
	}
	wr32(QMP_RAM + 0x2c, QMP_UP);			/* mcore channel up */
	kick(IPC_AOSS);
	if (wait_eq(QMP_RAM + 0x14, QMP_UP, 1000)) {
		fprintf(stderr, "qmp: ucore channel not up\n");
		return -1;
	}
	wr32(QMP_RAM + 0x18, QMP_UP);			/* ack ucore channel */
	kick(IPC_AOSS);
	if (wait_eq(QMP_RAM + 0x30, QMP_UP, 1000)) {
		fprintf(stderr, "qmp: mcore channel not acked\n");
		return -1;
	}
	return 0;
}

static int qmp_send(const char *msg)
{
	if (qmp_open())
		return -1;
	uint32_t off = rd32(QMP_RAM + 0x38), buf[16] = { 0 };
	strncpy((char *)buf, msg, sizeof(buf) - 1);
	for (int i = 0; i < 16; i++)
		wr32(QMP_RAM + off + 4 + 4 * i, buf[i]);
	wr32(QMP_RAM + off, sizeof(buf));
	(void)rd32(QMP_RAM + off);
	kick(IPC_AOSS);
	int rc = wait_eq(QMP_RAM + off, 0, 1000);
	printf("qmp: \"%s\" -> %s\n", msg, rc ? "TIMEOUT" : "acked");
	return rc;
}

/* ---- modem boot: TrustZone PAS (Linux qcom_q6v5_pas + qcom_mdt_loader) ---- */

#define PAS_MODEM	4
#define MPSS_PA		0x8b000000ull	/* bonito modem_region: the B4 dtbo overlay makes it 152 MiB (the base SDM670 DT says 126) */
#define MPSS_SIZE	0x09800000ull
#define MBA_PA		0x95600000ull	/* pil_mba_region in the B4 dtbo overlay (fragment 41), 2 MiB; the base DT says 0x93c00000 which is inside the modem region here */
#define MBA_SIZE	0x00200000ull
#define FW_DIR		"/var/local/fw/modem"
#define SCM_PIL(cmd)	(0x42000000ull | (2u << 8) | (cmd))

static int64_t scm(uint32_t cmd, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *r1)
{
	uint64_t x[8] = { SCM_PIL(cmd), arginfo, a, b, c, 0, 0, 0 };
	pm_smc(x);
	if (r1)
		*r1 = x[1];
	return (int64_t)x[0];
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = calloc(1, *len + 8);	/* zero padding: a segment is written in whole words */
	if (b && fread(b, 1, *len, f) != *len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

struct phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };

/*
 * After ERR-FATAL: the whole modem region (152 MiB) to /var/local/modem-ddr.bin, for offline analysis (page tables, the paged code, thread control blocks).
 */
static void dump_modem_ddr(void)
{
	static uint8_t chunk[0x100000];
	uint64_t t0 = now_ms();
	FILE *f = fopen("/var/local/modem-ddr.bin", "wb");
	if (!f) {
		printf("ddr dump: cannot create /var/local/modem-ddr.bin\n");
		return;
	}
	uint64_t bad = 0;
	for (uint64_t o = 0; o < MPSS_SIZE; o += sizeof(chunk)) {
		uint64_t n = MPSS_SIZE - o < sizeof(chunk) ? MPSS_SIZE - o : sizeof(chunk);
		if (pm_rd(MPSS_PA + o, chunk, n)) {
			memset(chunk, 0xee, n);
			bad++;
		}
		if (fwrite(chunk, 1, n, f) != n) {
			printf("ddr dump: write failed at +%llx\n", (unsigned long long)o);
			break;
		}
	}
	fclose(f);
	printf("ddr dump: modem region saved to /var/local/modem-ddr.bin in %llu ms (%llu unreadable chunks)\n", (unsigned long long)(now_ms() - t0), (unsigned long long)bad);
}

/*
 * After ERR-FATAL: the minidump table (SMEM item 602, Linux qcom_minidump) lists, per subsystem, an array of regions the crashed modem registered (its registers, TLB, ...)
 * at a physical address. The raw table is saved to /var/local/modem-md-toc.bin; regions inside the modem region are saved to modem-md-reg-N.bin (up to 24, 4 MiB each).
 */
static void dump_minidump(void)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(SMEM_HOST_GLOBAL, 602, &pa, &sz) && smem_get(SMEM_HOST_MODEM, 602, &pa, &sz)) {
		printf("minidump: smem item 602 not found\n");
		return;
	}
	static uint8_t toc[0x4000];
	uint32_t n = sz < sizeof(toc) ? sz : (uint32_t)sizeof(toc);
	for (uint32_t o = 0; o < n; o += 4) {
		uint32_t w = rd32(pa + o);
		memcpy(toc + o, &w, 4);
	}
	printf("minidump: smem item 602 at %llx, %u bytes; first words:", (unsigned long long)pa, sz);
	for (uint32_t o = 0; o < 96 && o < n; o += 4) {
		uint32_t w;
		memcpy(&w, toc + o, 4);
		printf(" %08x", w);
	}
	printf("\n");
	FILE *f = fopen("/var/local/modem-md-toc.bin", "wb");
	if (f) {
		fwrite(toc, 1, n, f);
		fclose(f);
	}
	/* subsystem entries: 0x80 bytes from +0x50, marked 0xdeeddeed; region count at +0x10, region array pointer (u64) at +0x18; a region is 40 bytes:
	 * name[16], seq (u64), address (u64), size (u64). Only SMEM and the modem region are read: anything else (TZ carve-outs) stalls the bus. */
	int saved = 0;
	for (uint32_t e = 0x50; e + 0x20 <= n; e += 0x80) {
		uint32_t sig, cnt, blo, bhi;
		memcpy(&sig, toc + e, 4);
		memcpy(&cnt, toc + e + 0x10, 4);
		memcpy(&blo, toc + e + 0x18, 4);
		memcpy(&bhi, toc + e + 0x1c, 4);
		if (sig != 0xdeeddeedu)
			continue;
		uint64_t base = ((uint64_t)bhi << 32) | blo;
		int in_smem = base >= 0x86000000ull && base + 40ull * cnt <= 0x86200000ull;
		int in_mpss = base >= MPSS_PA && base + 40ull * cnt <= MPSS_PA + MPSS_SIZE;
		printf("minidump: entry at toc +%x: %u region(s) at %llx (%s)\n", e, cnt, (unsigned long long)base, in_smem ? "smem" : in_mpss ? "modem region" : "skipped: not a readable range");
		if ((!in_smem && !in_mpss) || cnt == 0 || cnt > 64)
			continue;
		static uint8_t regs[64 * 40 + 8];
		if (in_smem) {
			for (uint32_t o = 0; o < cnt * 40; o += 4) {
				uint32_t w = rd32(base + o);
				memcpy(regs + o, &w, 4);
			}
		} else if (pm_rd(base & ~3ull, regs, (cnt * 40 + 3) & ~3u)) {
			printf("minidump:   region array unreadable\n");
			continue;
		}
		for (uint32_t r = 0; r < cnt; r++) {
			char name[17] = "";
			uint64_t seq, addr, size;
			memcpy(name, regs + r * 40, 16);
			for (int k = 0; k < 16; k++)
				if (name[k] && (name[k] < 32 || name[k] > 126))
					name[k] = '?';
			memcpy(&seq, regs + r * 40 + 16, 8);
			memcpy(&addr, regs + r * 40 + 24, 8);
			memcpy(&size, regs + r * 40 + 32, 8);
			int ok = addr >= MPSS_PA && size && size <= 0x400000ull && addr + size <= MPSS_PA + MPSS_SIZE;
			printf("minidump:   region %2u \"%s\" seq %llu addr %llx size %llx%s\n", r, name, (unsigned long long)seq, (unsigned long long)addr,
			       (unsigned long long)size, ok ? "" : " (not saved)");
			if (!ok || saved >= 24)
				continue;
			static uint8_t rb[0x400000];
			if (pm_rd(addr & ~3ull, rb, (size + 3) & ~3ull)) {
				printf("minidump:   region %u unreadable\n", r);
				continue;
			}
			char path[64];
			snprintf(path, sizeof(path), "/var/local/modem-md-reg-%d.bin", saved);
			FILE *g = fopen(path, "wb");
			if (g) {
				fwrite(rb, 1, size, g);
				fclose(g);
			}
			printf("minidump:   -> %s\n", path);
			saved++;
		}
	}
	printf("minidump: %d region(s) saved\n", saved);
}

/*
 * After ERR-FATAL: compare what is in the modem's DDR with the segment files it was loaded from. Read-only segments (flags & 2 == 0) must be identical: a difference in
 * the compressed, demand-paged code (segment 16) would make a page-in fail. Writable ones differ by design (the modem has run); they are listed for the extent only.
 */
static void verify_modem_image(void)
{
	size_t mlen;
	uint8_t *mdt = slurp(FW_DIR "/modem.mdt", &mlen);
	if (!mdt) {
		printf("verify: no modem.mdt\n");
		return;
	}
	uint32_t phoff, phnum = 0;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	struct phdr *ph = (struct phdr *)(mdt + phoff);
	static uint8_t mem[0x100000];
	uint64_t t0 = now_ms();
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || !ph[i].filesz || ((ph[i].flags >> 24) & 7) == 2)
			continue;
		char path[128];
		size_t sl;
		snprintf(path, sizeof(path), FW_DIR "/modem.b%02u", i);
		uint8_t *seg = slurp(path, &sl);
		if (!seg || sl < ph[i].filesz) {
			printf("verify: segment %u: %s missing or short\n", i, path);
			free(seg);
			continue;
		}
		uint64_t diff = 0, first = ~0ull, last = 0, bad_read = 0;
		for (uint64_t o = 0; o < ph[i].filesz; o += sizeof(mem)) {
			uint64_t n = ph[i].filesz - o < sizeof(mem) ? ph[i].filesz - o : sizeof(mem);
			uint64_t rn = (n + 3) & ~3ull;
			if (pm_rd(ph[i].paddr + o, mem, rn)) {
				bad_read++;
				continue;
			}
			for (uint64_t k = 0; k < n; k += 4) {
				uint32_t a, b;
				memcpy(&a, mem + k, 4);
				memcpy(&b, seg + o + k, 4);
				if (a != b) {
					diff++;
					if (first == ~0ull)
						first = o + k;
					last = o + k;
				}
			}
		}
		printf("verify: segment %2u pa %llx file %8u flags %x (%s): %llu differing words", i, (unsigned long long)ph[i].paddr, ph[i].filesz, ph[i].flags & 7,
		       (ph[i].flags & 2) ? "writable" : "READ-ONLY", (unsigned long long)diff);
		if (diff)
			printf(", first +%llx last +%llx", (unsigned long long)first, (unsigned long long)last);
		if (bad_read)
			printf(", %llu unreadable chunks", (unsigned long long)bad_read);
		printf("\n");
		free(seg);
	}
	printf("verify: done in %llu ms\n", (unsigned long long)(now_ms() - t0));
	free(mdt);
}

/* ---- bonito modem boot: the MBA (downstream drivers/soc/qcom/pil-msa.c and pil-q6v5.c; qcom,pil-q6v55-mss, qdsp6v65-1-0 boot FSM) ---- */

#define GCC_PA		0x00100000ull
#define GCC_APCS_EN	(GCC_PA + 0x52004)	/* apps clock votes: bit 10 boot rom ahb (mem_clk), 13 prng, 17 mss gpll0 div */
#define Q6SS		0x04080000ull		/* qdsp6_base */
#define RMB		0x04180000ull		/* rmb_base */
#define MSS_RESTART	0x0c2b0000ull		/* restart_reg (direct write: no qcom,restart-reg-sec) */
#define MSS_PDC_SYNC	0x0b2e0100ull
#define MSS_ALT_RESET	0x04180044ull
#define RMB_MBA_IMAGE	0x00
#define RMB_PBL_STATUS	0x04
#define RMB_MBA_COMMAND	0x08
#define RMB_MBA_STATUS	0x0c
#define RMB_PMI_META	0x10
#define RMB_PMI_START	0x14
#define RMB_PMI_LENGTH	0x18
#define RMB_MSS_STATUS	0x40

static void rmb_dump(const char *why)
{
	printf("rmb (%s):", why);
	for (unsigned o = 0; o <= 0x20; o += 4)
		printf(" %02x=%08x", o, rd32(RMB + o));
	printf(" status40=%08x\n", rd32(RMB + RMB_MSS_STATUS));
}

/* a branch clock: enable (bit 0), wait until the halt bit (31) clears */
static int cbcr_on(const char *name, uint64_t pa)
{
	wr32(pa, rd32(pa) | 1);
	for (int i = 0; i < 1000; i++) {
		if (!(rd32(pa) & (1u << 31)))
			return 0;
		usleep(100);
	}
	fprintf(stderr, "mba: clock %s (%llx = %08x) did not come on\n", name, (unsigned long long)pa, rd32(pa));
	return -1;
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* real-time waits (a loop count is not time: every read is a system call and every usleep rounds up); the RMB registers are printed every 2 s while waiting */
static int wait_status(unsigned off, uint32_t want, uint32_t want2, int ms, const char *what)
{
	uint64_t t0 = now_ms(), shown = t0;
	for (;;) {
		uint32_t v = rd32(RMB + off);
		if (v == want || (want2 && v == want2))
			return 0;
		if ((int32_t)v < 0) {
			fprintf(stderr, "mba: %s: error status %d\n", what, (int32_t)v);
			rmb_dump(what);
			return -1;
		}
		uint64_t t = now_ms();
		if (t - t0 > (uint64_t)ms)
			break;
		if (t - shown >= 2000) {
			shown = t;
			printf("mba: waiting for %s (%llu ms):\n", what, (unsigned long long)(t - t0));
			rmb_dump(what);
		}
		usleep(1000);
	}
	fprintf(stderr, "mba: %s timed out\n", what);
	rmb_dump(what);
	return -1;
}

static int wait_nonzero(unsigned off, int ms, const char *what, uint32_t *out)
{
	uint64_t t0 = now_ms();
	for (;;) {
		uint32_t v = rd32(RMB + off);
		if (v) {
			*out = v;
			return 0;
		}
		if (now_ms() - t0 > (uint64_t)ms)
			break;
		usleep(1000);
	}
	fprintf(stderr, "mba: %s timed out\n", what);
	rmb_dump(what);
	return -1;
}

static void mss_pdc_sync(int on)
{
	uint32_t v = rd32(MSS_PDC_SYNC);
	wr32(MSS_PDC_SYNC, on ? v | (1u << 9) : v & ~(1u << 9));	/* qcom,mss_pdc_offset = 9 */
	usleep(2);
}

/* pil_mss_reset: clocks, reset the subsystem, give the MBA its address, run the QDSP6 boot FSM, wait for PBL and the MBA */
static int mba_start(uint64_t mba_pa)
{
	wr32(GCC_APCS_EN, rd32(GCC_APCS_EN) | (1u << 10) | (1u << 13) | (1u << 17));
	usleep(1000);
	if (cbcr_on("mss_cfg_ahb", GCC_PA + 0x8a000) || cbcr_on("mss_mfab_axis", GCC_PA + 0x8a004) ||
	    cbcr_on("mss_snoc_axi", GCC_PA + 0x8a150) || cbcr_on("mss_q6_memnoc_axi", GCC_PA + 0x8a154))
		return -1;
	printf("mba: clocks on; rmb before reset:\n");
	rmb_dump("before reset");
	/* assert (pdc sync, alt reset, restart), wait, deassert */
	mss_pdc_sync(1);
	wr32(MSS_ALT_RESET, 1);
	usleep(2);
	wr32(MSS_RESTART, 1);
	usleep(2);
	usleep(200);
	wr32(MSS_RESTART, 0);
	usleep(2);
	usleep(200);
	wr32(MSS_ALT_RESET, 0);
	usleep(2);
	mss_pdc_sync(0);
	/* the MBA's address and no debug policy */
	wr32(RMB + RMB_MBA_IMAGE, (uint32_t)mba_pa);
	wr32(RMB + RMB_PMI_START, 0);
	wr32(RMB + RMB_PMI_LENGTH, 0);
	/* __pil_q6v65_reset: sleep clock, de-assert stop core, trigger the boot FSM */
	wr32(Q6SS + 0x3c, rd32(Q6SS + 0x3c) | 1);
	for (int i = 0; rd32(Q6SS + 0x3c) & (1u << 31); i++) {
		if (i == 200) {
			fprintf(stderr, "mba: sleep clock did not come on\n");
			return -1;
		}
		usleep(1);
	}
	wr32(Q6SS + 0x400, 1);
	wr32(Q6SS + 0x404, 1);
	int fsm = -1;
	for (uint64_t t0 = now_ms(); now_ms() - t0 < 100;) {
		if (rd32(RMB + RMB_MSS_STATUS) & 1) {
			fsm = 0;
			break;
		}
		usleep(100);
	}
	printf("mba: boot FSM %s (mss_status %08x)\n", fsm ? "did not complete" : "complete", rd32(RMB + RMB_MSS_STATUS));
	if (fsm)
		return -1;
	uint32_t st = 0;
	if (wait_nonzero(RMB_PBL_STATUS, 1000, "PBL", &st))
		return -1;
	if (st != 1) {
		fprintf(stderr, "mba: PBL status %u\n", st);
		rmb_dump("PBL");
		return -1;
	}
	if (wait_nonzero(RMB_MBA_STATUS, 1000, "MBA boot", &st))
		return -1;
	if (st != 1 && st != 2) {
		fprintf(stderr, "mba: MBA status %u (want 1: XPU unlocked)\n", st);
		rmb_dump("MBA boot");
		return -1;
	}
	printf("mba: PBL and MBA are up (status %u)\n", st);
	return 0;
}

/* a heartbeat for the modem load: every 5 s it says what the loader is doing, so a stall names its phase (reading a file, writing a segment at an offset) */
static char hb_phase[160] = "";
static volatile int hb_on;

static void *hb_thread(void *arg)
{
	uint64_t t0 = now_ms();
	char last[160] = "";
	while (hb_on) {
		sleep(5);
		if (!hb_on)
			break;
		char cur[160];
		memcpy(cur, hb_phase, sizeof(cur));
		cur[sizeof(cur) - 1] = 0;
		printf("boot: [%llu s] still in: %s%s\n", (unsigned long long)((now_ms() - t0) / 1000), cur, strcmp(cur, last) ? "" : " (no change)");
		memcpy(last, cur, sizeof(last));
	}
	return NULL;
}

static void hb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void hb(const char *fmt, ...)
{
	char tmp[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	memcpy(hb_phase, tmp, sizeof(tmp));
}

static int boot_modem(void)
{
	pthread_t hbt;
	hb_on = 1;
	pthread_create(&hbt, NULL, hb_thread, NULL);
	/* 1. proxy votes as downstream (vdd_cx, vdd_mx, vdd_mss): the top levels */
	int ncx = levels("cx.lvl", 0), nmss = levels("mss.lvl", 0);
	uint64_t aux;
	uint32_t len, top_cx = 0, top_mss = 0, top_mx = 0;
	if (!cmddb_aux("cx.lvl", &aux, &len))
		for (uint32_t i = 0; i < len / 2; i++)
			if (aux16(aux, i))
				top_cx = i;
	if (!cmddb_aux("mss.lvl", &aux, &len))
		for (uint32_t i = 0; i < len / 2; i++)
			if (aux16(aux, i))
				top_mss = i;
	if (!cmddb_aux("mx.lvl", &aux, &len))
		for (uint32_t i = 0; i < len / 2; i++)
			if (aux16(aux, i))
				top_mx = i;
	printf("boot: cx %d levels, mss %d levels: voting cx[%u] mx[%u] mss[%u]\n", ncx, nmss, top_cx, top_mx, top_mss);
	if (vote("cx.lvl", top_cx) || vote("mx.lvl", top_mx) || vote("mss.lvl", top_mss))
		return -1;

	/* 2. metadata: ELF header + program headers + hash segment */
	size_t mlen;
	uint8_t *mdt = slurp(FW_DIR "/modem.mdt", &mlen);
	if (!mdt) {
		fprintf(stderr, "boot: no %s/modem.mdt\n", FW_DIR);
		return -1;
	}
	uint32_t phoff, phnum = 0;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	struct phdr *ph = (struct phdr *)(mdt + phoff);
	int hash = -1;
	for (uint32_t i = 0; i < phnum; i++)
		if (((ph[i].flags >> 24) & 7) == 2)
			hash = (int)i;
	if (hash < 0) {
		fprintf(stderr, "boot: no hash segment\n");
		return -1;
	}
	size_t ehsz = phoff + 32ull * phnum, meta_len = ehsz + ph[hash].filesz;
	uint8_t *meta = malloc(meta_len);
	memcpy(meta, mdt, ehsz);
	if (ph[hash].offset + ph[hash].filesz <= mlen) {
		memcpy(meta + ehsz, mdt + ph[hash].offset, ph[hash].filesz);
	} else {
		char path[128];
		size_t hl;
		snprintf(path, sizeof(path), FW_DIR "/modem.b%02d", hash);
		uint8_t *h = slurp(path, &hl);
		if (!h || hl < ph[hash].filesz) {
			fprintf(stderr, "boot: hash segment %s missing\n", path);
			return -1;
		}
		memcpy(meta + ehsz, h, ph[hash].filesz);
		free(h);
	}
	/* the MBA's address register is 32 bits wide: the metadata goes into the MBA region's second MiB (as the downstream driver, which allocates it from the MBA's own memory-region),
	 * not into managed RAM, which lies above 4 GiB on this phone */
	uint64_t meta_pa = MBA_PA + 0x100000;
	if (meta_len > 0x100000)
		return -1;
	pm_wr(meta_pa, meta, (meta_len + 3) & ~3ull);
	printf("boot: metadata %zu bytes at %llx\n", meta_len, (unsigned long long)meta_pa);

	/* 3. the MBA image into its region, then the AOP, the SMP2P state, and the subsystem reset with the boot FSM */
	size_t mbalen;
	uint8_t *mba = slurp(FW_DIR "/mba.mbn", &mbalen);
	if (!mba || mbalen > 0x100000) {
		fprintf(stderr, "boot: %s/mba.mbn missing or over 1 MiB\n", FW_DIR);
		return -1;
	}
	pm_wr(MBA_PA, mba, (mbalen + 3) & ~3ull);
	free(mba);
	/* the downstream driver hands the MBA's 1 MiB to the modem's VM (qcom,mem-protect-id 0xf = VMID_MSS_MSA; pil_assign_mem_to_subsys) after copying it in */
	{
		const uint32_t hlos[1] = { VMID_HLOS }, msa[1] = { VMID_MSS_MSA };
		if (hyp_assign(MBA_PA, 0x100000, hlos, 1, msa, 1))
			return -1;
	}
	qmp_send("{class: image, res: load_state, name: modem, val: on}");
	/* a restart keeps the last run's state bits until the modem rewrites them: forget them */
	uint64_t ve;
	if (smp2p_in_entry("slave-kernel", &ve) == 0)
		wr32(ve, 0);
	modem_state = 0xffffffff;
	if (mba_start(MBA_PA))
		return -1;

	/* 4. the MBA authenticates the headers (ELF header, program headers, hash segment) */
	{
		/* the metadata buffer belongs to the modem's VM while the MBA reads it, then back to the kernel */
		const uint32_t hlos[1] = { VMID_HLOS }, msa[1] = { VMID_MSS_MSA };
		uint64_t meta_sz = (meta_len + 0xfff) & ~0xfffull;
		if (hyp_assign(meta_pa, meta_sz, hlos, 1, msa, 1))
			return -1;
		wr32(RMB + RMB_PMI_LENGTH, 0);
		wr32(RMB + RMB_PMI_META, (uint32_t)meta_pa);
		wr32(RMB + RMB_MBA_COMMAND, 1);	/* CMD_META_DATA_READY */
		int hrc = wait_status(RMB_MBA_STATUS, 3, 0, 10000, "header authentication");
		(void)hyp_assign(meta_pa, meta_sz, msa, 1, hlos, 1);
		if (hrc)
			return -1;
	}
	printf("boot: MBA authenticated the headers\n");

	/* the whole modem region to the kernel AND the modem's VM before any segment is written (downstream pil_assign_mem_to_subsys_and_linux, region = lowest segment start to
	 * highest segment end): without it the first large write stalled the bus */
	{
		uint64_t rs = ~0ull, re = 0;
		for (uint32_t i = 0; i < phnum; i++) {
			if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz)
				continue;
			if (ph[i].paddr < rs)
				rs = ph[i].paddr;
			if ((uint64_t)ph[i].paddr + ph[i].memsz > re)
				re = (uint64_t)ph[i].paddr + ph[i].memsz;
		}
		rs &= ~0xfffull;
		re = (re + 0xfff) & ~0xfffull;
		const uint32_t hlos[1] = { VMID_HLOS }, both[2] = { VMID_HLOS, VMID_MSS_MSA };
		printf("boot: modem region %llx-%llx to the kernel and the modem's VM\n", (unsigned long long)rs, (unsigned long long)re);
		if (hyp_assign(rs, re - rs, hlos, 1, both, 2))
			return -1;
	}

	/* 5. the segments at their addresses, each announced to the MBA as it is loaded (pil_msa_mba_verify_blob) */
	uint64_t total = 0;
	static uint8_t zeros[0x10000];
	uint32_t img_length = 0;
	uint64_t seg_t0 = now_ms();
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz)
			continue;
		uint64_t pa = ph[i].paddr;
		printf("boot: segment %u: pa %llx file %u mem %u (%llu ms)\n", i, (unsigned long long)pa, ph[i].filesz, ph[i].memsz, (unsigned long long)(now_ms() - seg_t0));
		if (pa < MPSS_PA || pa + ph[i].memsz > MPSS_PA + MPSS_SIZE) {
			fprintf(stderr, "boot: segment %u at %llx outside the region\n", i, (unsigned long long)pa);
			return -1;
		}
		if (ph[i].filesz) {
			char path[128];
			size_t sl;
			snprintf(path, sizeof(path), FW_DIR "/modem.b%02u", i);
			hb("reading %s", path);
			uint64_t rt0 = now_ms();
			uint8_t *seg = slurp(path, &sl);
			printf("boot:   read %s: %zu bytes in %llu ms\n", path, sl, (unsigned long long)(now_ms() - rt0));
			if (!seg || sl < ph[i].filesz) {
				fprintf(stderr, "boot: %s missing or short\n", path);
				return -1;
			}
			/* in 1 MiB steps, each reported when it is slow, with the MBA's status: a stall shows its offset */
			uint64_t want = (ph[i].filesz + 3) & ~3ull;
			for (uint64_t o = 0; o < want; o += 0x100000) {
				uint64_t n = want - o < 0x100000 ? want - o : 0x100000, t1 = now_ms();
				hb("writing segment %u data +%llx (%llu bytes) to %llx", i, (unsigned long long)o, (unsigned long long)n, (unsigned long long)(pa + o));
				if (pm_wr(pa + o, seg + o, n)) {
					fprintf(stderr, "boot: write failed in segment %u at +%llx\n", i, (unsigned long long)o);
					return -1;
				}
				uint64_t dt = now_ms() - t1;
				if (dt > 300 || (o & 0x7fffff) == 0)
					printf("boot:   seg %u data +%llx (%llu bytes) %llu ms, mba status %08x\n", i, (unsigned long long)o, (unsigned long long)n,
					       (unsigned long long)dt, rd32(RMB + RMB_MBA_STATUS));
			}
			free(seg);
			total += ph[i].filesz;
		}
		for (uint64_t o = (ph[i].filesz + 3) & ~3ull, zn = 0; o < ph[i].memsz; o += sizeof(zeros), zn++) {
			uint64_t n = ph[i].memsz - o < sizeof(zeros) ? ph[i].memsz - o : sizeof(zeros), t1 = now_ms();
			hb("zero-filling segment %u +%llx to %llx", i, (unsigned long long)o, (unsigned long long)(pa + o));
			if (pm_wr(pa + o, zeros, (n + 3) & ~3ull)) {
				fprintf(stderr, "boot: zero fill failed in segment %u at +%llx\n", i, (unsigned long long)o);
				return -1;
			}
			uint64_t dt = now_ms() - t1;
			if (dt > 300 || (zn & 15) == 0)
				printf("boot:   seg %u zeros +%llx %llu ms, mba status %08x\n", i, (unsigned long long)o, (unsigned long long)dt, rd32(RMB + RMB_MBA_STATUS));
		}
		if (img_length == 0) {
			wr32(RMB + RMB_PMI_START, (uint32_t)pa);
			wr32(RMB + RMB_MBA_COMMAND, 2);	/* CMD_LOAD_READY */
		}
		img_length += ph[i].memsz;
		hb("announcing segment %u to the MBA (length %u)", i, img_length);
		wr32(RMB + RMB_PMI_LENGTH, img_length);
		if ((int32_t)rd32(RMB + RMB_MBA_STATUS) < 0) {
			fprintf(stderr, "boot: the MBA rejected segment %u\n", i);
			rmb_dump("segment");
			return -1;
		}
	}
	printf("boot: %llu bytes of segments loaded (%u announced)\n", (unsigned long long)total, img_length);
	if (wait_status(RMB_MBA_STATUS, 4, 0, 10000, "image authentication"))
		return -1;
	printf("boot: MBA authenticated the image, the modem is running\n");

	/* 5. wait for SMP2P in: "ready" (bit 1) from the modem */
	for (int t = 0; t < 300; t++) {
		uint32_t v;
		smp2p_service();
		/* after a crash the old entry still says ready + fatal until the modem rewrites it */
		if (smp2p_in_value("slave-kernel", &v) == 0 && (v & 2) && !(v & 1)) {
			printf("boot: modem ready after %d00 ms\n", t);
			return 0;
		}
		usleep(100000);
	}
	printf("boot: no ready from the modem in 30 s\n");
	smp2p_show("in", 435);
	return -1;
}

static void net_cb(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	uint32_t w[5] = { 0 };
	memcpy(w, d, n < sizeof(w) ? n : sizeof(w));
	if (h->type == QRTR_TYPE_DATA) {
		if (h->confirm_rx)
			qrtr_resume_tx(h);
		if (h->dst_port == RMTFS_PORT) {
			rmtfs_handle(h, d, n);
			return;
		}
		if (h->dst_port == WLFW_PORT) {
			wlfw_handle(h, d, n);
			return;
		}
		if (h->dst_port == PDMAPPER_PORT) {
			pdmapper_handle(h, d, n);
			return;
		}
		if (h->dst_port == MEMSHARE_PORT) {
			memshare_handle(h, d, n);
			return;
		}
		if (h->dst_port == TFTP_PORT || (h->dst_port >= TFTP_XFER_PORT0 && h->dst_port < TFTP_XFER_PORT0 + TFTP_XFERS)) {
			tftp_handle(h, d, n);
			return;
		}
		printf("qrtr: < DATA %u:%x -> %u:%x, %u bytes\n", h->src_node, h->src_port, h->dst_node, h->dst_port, n);
		return;
	}
	printf("[%6llu] qrtr: < ctrl type %u (cmd %u) from %u:%x: %x %x %x %x\n", (unsigned long long)link_ms(), h->type, w[0], h->src_node, h->src_port,
	       w[1], w[2], w[3], w[4]);
	if (w[0] == QRTR_TYPE_HELLO) {
		uint32_t hello[5] = { QRTR_TYPE_HELLO };	/* struct qrtr_ctrl_pkt: always 20 bytes */
		qrtr_send(QRTR_TYPE_HELLO, QRTR_PORT_CTRL, h->src_node, QRTR_PORT_CTRL, hello, sizeof(hello));
	} else if (w[0] == QRTR_TYPE_NEW_SERVER) {
		printf("qrtr:   server service %#x instance %#x at %u:%x\n", w[1], w[2], w[3], w[4]);
		if (w[1] == WLFW_SERVICE)
			wlfw_new_server(w[3], w[4]);
	}
}

/* the network to join: "ssid=NAME" and "psk=PASSPHRASE" lines (on the phone only, never in a repo) */
#define WLAN_CONF	"/var/local/wlan.conf"
static char conf_ssid[33], conf_psk[65];

static void read_conf(void)
{
	char line[128];
	FILE *f = fopen(WLAN_CONF, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "ssid=", 5))
			snprintf(conf_ssid, sizeof(conf_ssid), "%s", line + 5);
		else if (!strncmp(line, "psk=", 4))
			snprintf(conf_psk, sizeof(conf_psk), "%s", line + 4);
	}
	fclose(f);
	printf("net: will join '%s' (%s)\n", conf_ssid, conf_psk[0] ? "WPA2-PSK" : "open");
}

/*
 * Join after the first scan and stay joined: a failed try again after 5 s (4 times), then every 30 s with a
 * fresh scan first; a lost link (deauth, kickout, beacon miss) joins again after 3 s.
 */
static void join_poll(void)
{
	static int asked, joined, fails;
	static uint64_t next;
	if (!conf_ssid[0] || !radio_scans() || bridge_joins)
		return;				/* (wifid has taken over: bridge.c) */
	uint64_t now = link_ms();
	int r = radio_join_result();
	if (asked) {
		if (r == -1)
			return;				/* running */
		asked = 0;
		if (r == 0) {
			joined = 1;
			fails = 0;
			return;
		}
		fails++;
		next = now + (fails < 4 ? 5000 : 30000);
		if (fails >= 4)
			radio_rescan(25000);
		printf("[%6llu] net: join failed (%s), try %d, next in %llu s\n", (unsigned long long)now, strerror(r), fails,
		       (unsigned long long)(next - now) / 1000);
		return;
	}
	if (joined) {
		if (r != ENOTCONN)
			return;
		joined = 0;
		fails = 0;
		next = now + 3000;
		printf("[%6llu] net: link lost, joining again in 3 s\n", (unsigned long long)now);
		return;
	}
	if (next && now < next)
		return;
	r = radio_join(conf_ssid, conf_psk);
	printf("[%6llu] net: join '%s' (try %d) -> %d\n", (unsigned long long)now, conf_ssid, fails + 1, r);
	if (r == EINVAL || r == EIO) {
		printf("net: cannot join with this wlan.conf; not trying again\n");
		conf_ssid[0] = 0;
		return;
	}
	asked = 1;
}

static volatile sig_atomic_t drop_asked;

static void on_usr1(int sig)
{
	drop_asked = 1;
}

static void net_idle(void)
{
	if (drop_asked) {
		drop_asked = 0;
		radio_disconnect();
	}
	smp2p_service();
	radio_poll();
	ipnet_poll();
	bridge_poll();
	join_poll();
}

static int net(unsigned secs)
{
	if (getenv("WLAND_VERBOSE"))
		verbose_link = atoi(getenv("WLAND_VERBOSE"));
	link_idle = net_idle;
	signal(SIGUSR1, on_usr1);
	signal(SIGCHLD, SIG_IGN);	/* netsvc children (ipnet.c) reap themselves */
	if (link_up(net_cb, 5000))
		return -1;
	if (rmtfs_init())
		printf("net: no rmtfs\n");
	if (pdmapper_init() || tftp_init())
		printf("net: no pd-mapper/tftp\n");
	if (memshare_init())
		printf("net: no memshare\n");
	read_conf();
	uint64_t end = link_ms() + (uint64_t)secs * 1000, stats = link_ms() + 30000;
	while (!secs || link_ms() < end) {
		link_poll(100);
		if (link_ms() > stats) {
			radio_stats();
			stats = link_ms() + 30000;
		}
		if (!wlfw_pending())
			continue;
		if (wlfw_start()) {
			printf("net: WLFW bring-up failed\n");
		} else if (!getenv("WLAND_NO_RADIO")) {
			delay_ms(100);	/* topaz: only after mission mode; before it a CE access hangs the bus */
			if (radio_start())
				printf("net: radio start failed\n");
			else if (bridge_open() != 0 && conf_ssid[0] && ipnet_init())
				printf("net: no utun: joining without IP\n");
		}
	}
	radio_summary();
	radio_stats();
	return 0;
}

/* everything `up` does, from a fresh boot */
static int up(unsigned secs)
{
	if (smmu_wlan_identity())
		printf("up: no WLAN SMMU stream: the radio will not start\n");
	modem_rails();
	if (wlan_power() || smp2p_create() || boot_modem())
		return 1;
	return net(secs) ? 1 : 0;
}

/*
 * The boot job: `up 0` at most once per phone boot (a second modem boot in the same phone boot is untested and
 * MSA/rmtfs hangs wedge a CPU), unless switched off. stdout/stderr go to /var/local/wland.log.
 */
#define WLAND_LOG	"/var/local/wland.log"
#define WLAND_STAMP	"/var/local/wland.boot"
/* libsystem_kernel: jetsam control (root may change its own limits); MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES */
int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t size);
struct memlimit_properties { int32_t active; uint32_t active_attr; int32_t inactive; uint32_t inactive_attr; };

static int auto_up(void)
{
	/* launchd's per-process limit for a job killed us while the modem firmware loaded (jetsam reason 7) */
	struct memlimit_properties ml = { 2048, 0, 2048, 0 };
	int mrc = memorystatus_control(7, getpid(), 0, &ml, sizeof(ml));
	char args[1024] = "";
	size_t n = sizeof(args) - 1;
	sysctlbyname("kern.bootargs", args, &n, NULL, 0);
	if (!access("/var/local/wland.off", F_OK) || strstr(args, "sweetwifi=0")) {
		fprintf(stderr, "wland: off (/var/local/wland.off or sweetwifi=0)\n");
		return 0;
	}
	struct timeval bt = { 0 };
	n = sizeof(bt);
	sysctlbyname("kern.boottime", &bt, &n, NULL, 0);
	char stamp[32] = "", now[32];
	snprintf(now, sizeof(now), "%ld\n", (long)bt.tv_sec);
	FILE *f = fopen(WLAND_STAMP, "r");
	if (f) {
		if (!fgets(stamp, sizeof(stamp), f))
			stamp[0] = 0;
		fclose(f);
	}
	if (!strcmp(stamp, now)) {
		fprintf(stderr, "wland: already started in this boot\n");
		return 1;
	}
	if ((f = fopen(WLAND_STAMP, "w"))) {
		fputs(now, f);
		fclose(f);
	}
	rename(WLAND_LOG, WLAND_LOG ".prev");
	if (freopen(WLAND_LOG, "a", stdout))
		dup2(fileno(stdout), 2);
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("wland auto: boot %ld, memory limit 2048 MB -> %d\n", (long)bt.tv_sec, mrc);
	return up(0);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: wland smem | smp2p | vote RES LVL | levels RES | boot\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);	/* a stalled bus access must not swallow the output */
	pm_init();
	if (smem_init())
		return 1;
	const char *c = argv[1];
	if (!strcmp(c, "smem")) {
		smem_dump();
	} else if (!strcmp(c, "smp2p")) {
		smp2p_show("in", 435);	/* qcom,smem = <435>, <428>: inbound, outbound */
		smp2p_show("out", 428);
	} else if (!strcmp(c, "levels") && argc > 2) {
		levels(argv[2], 1);
	} else if (!strcmp(c, "qmp") && argc > 2) {
		return qmp_send(argv[2]) ? 1 : 0;
	} else if (!strcmp(c, "smp2p-init")) {
		return smp2p_init() ? 1 : 0;
	} else if (!strcmp(c, "shutdown")) {
		uint64_t r1 = 0;
		int64_t rc = scm(6, 1, PAS_MODEM, 0, 0, &r1);	/* PAS_SHUTDOWN */
		printf("shutdown -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
		return rc ? 1 : 0;
	} else if (!strcmp(c, "items")) {
		smem_list();
	} else if (!strcmp(c, "item") && argc > 2) {
		uint64_t pa;
		uint32_t sz;
		unsigned host = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : SMEM_HOST_MODEM;
		if (smem_get(host, (unsigned)atoi(argv[2]), &pa, &sz)) {
			printf("item %s: not found\n", argv[2]);
			return 1;
		}
		printf("item %s: pa %llx size %u\n", argv[2], (unsigned long long)pa, sz);
		for (uint32_t o = 0; o < sz && o < 4096; o += 16) {
			uint8_t b[16];
			for (int k = 0; k < 16; k += 4) {
				uint32_t w = rd32(pa + o + k);
				memcpy(b + k, &w, 4);
			}
			printf("%04x:", o);
			for (int k = 0; k < 16; k++)
				printf(" %02x", b[k]);
			printf("  ");
			for (int k = 0; k < 16; k++)
				putchar(b[k] >= 32 && b[k] < 127 ? b[k] : '.');
			printf("\n");
		}
	} else if (!strcmp(c, "net")) {
		return net(argc > 2 ? (unsigned)atoi(argv[2]) : 10) ? 1 : 0;
	} else if (!strcmp(c, "auto")) {
		return auto_up();
	} else if (!strcmp(c, "up")) {
		/* everything from a fresh phone boot: SMP2P, the modem, GLINK + QRTR; the WLAN SMMU stream first */
		return up(argc > 2 ? (unsigned)atoi(argv[2]) : 20);
	} else if (!strcmp(c, "smmu")) {
		return smmu_wlan_identity() ? 1 : 0;
	} else if (!strcmp(c, "cmddb")) {
		cmddb_list();
	} else if (!strcmp(c, "wlan-power")) {
		return wlan_power() ? 1 : 0;
	} else if (!strcmp(c, "boot")) {
		/* "boot SECS": wait first. Started at the beginning of the phone's boot, the firmware files (75 MB) are read from an eMMC that iOS is saturating: single reads took
		 * 12 to 100+ seconds. */
		/* launchd's per-process memory limit killed the job while the firmware loaded (kernel log: CRASH wland sig 9, reason ns 1 code 0x7 = jetsam per-process limit): raise it as auto does */
		struct memlimit_properties ml = { 2048, 0, 2048, 0 };
		int mrc = memorystatus_control(7, getpid(), 0, &ml, sizeof(ml));
		printf("boot: memory limit 2048 MB -> %d\n", mrc);
		if (argc > 2)
			sleep((unsigned)atoi(argv[2]));
		return boot_modem() ? 1 : 0;
	} else if (!strcmp(c, "uptest")) {
		/* uptest WAIT NETSECS: for the launchd test job: the memory limit, a wait for the phone's own boot to settle, then the whole chain (SMMU stream, WLAN rails, SMP2P, modem, network) */
		struct memlimit_properties ml = { 2048, 0, 2048, 0 };
		int mrc = memorystatus_control(7, getpid(), 0, &ml, sizeof(ml));
		printf("uptest: memory limit 2048 MB -> %d\n", mrc);
		if (argc > 2)
			sleep((unsigned)atoi(argv[2]));
		return up(argc > 3 ? (unsigned)atoi(argv[3]) : 60);
	} else if (!strcmp(c, "bcm") && argc > 3) {
		/* a BCM (bus) vote: x = average, y = peak, in the BCM's own units (0x3fff = its top level); commit + valid */
		uint32_t x = (uint32_t)strtoul(argv[2 + 1], NULL, 0) & 0x3fff, y = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) & 0x3fff : x;
		uint32_t off[1] = { 0 }, val[1] = { 1u << 30 | 1u << 29 | x << 14 | y };
		int rc = pm_rpmh(argv[2], 1, off, val);
		printf("bcm %s x %#x y %#x (%#x) -> %d\n", argv[2], x, y, val[0], rc);
		return rc ? 1 : 0;
	} else if (!strcmp(c, "vote") && argc > 3) {
		return vote(argv[2], (uint32_t)strtoul(argv[3], NULL, 0)) ? 1 : 0;
	} else {
		fprintf(stderr, "wland: unknown command\n");
		return 2;
	}
	return 0;
}
