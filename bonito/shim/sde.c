/*
 * Display (MDSS/SDE 4.x) probing on bonito (Pixel 3a XL, SDM670).
 *
 * ABL leaves a command-mode DSI panel (SOFEF00 or NT37700F, 1080x2160)
 * running from the splash framebuffer. Offsets are from the stock SDM670
 * DTB (qcom,mdss_mdp@ae00000: sde-ctl-off, sde-sspp-off, sde-mixer-off,
 * sde-pp-off, sde-dsc-off, sde-intf-off) and checked against
 * drivers/gpu/drm/msm/sde/sde_hw_{pingpong,sspp,ctl}.c of the Lineage
 * msm-4.9 kernel. Unlike SDE 5 (flame), tear check and autorefresh live in
 * the PINGPONG block, not in the INTF.
 *
 * Derived from ios-flame's sde.c (MIT, (c) 2026 dipa-studio / ios-flame
 * contributors).
 */
#include <stdint.h>
#include "log.h"
#include "sde.h"
#include "util.h"

#define MDP		0x0ae00000UL
#define CTL(n)		(MDP + 0x2000 + 0x200 * (n))	/* 5 CTLs */
#define LM(n)		(MDP + 0x45000 + 0x1000 * (n))	/* LM0, LM1, LM2, (LM5 at 0x4a000) */
#define PP(n)		(MDP + 0x71000 + 0x800 * (n))	/* 5 pingpongs */
#define DSC(n)		(MDP + 0x81000 + 0x400 * (n))
#define INTF(n)		(MDP + 0x6b000 + 0x800 * (n))	/* 0 dp, 1 dsi, 2 dsi, 3 dp */

static const uintptr_t sspp_off[5] = { 0x5000, 0x7000, 0x25000, 0x27000, 0x29000 };
static const char sspp_name[5][5] = { "vig0", "vig1", "dma0", "dma1", "dma2" };	/* no pointer table: image runs unrelocated */

#define CTL_LAYER(lm)	(0x04 * (lm))
#define CTL_TOP		0x014
#define CTL_FLUSH	0x018

#define SSPP_SRC_SIZE	0x00
#define SSPP_SRC0_ADDR	0x14
#define SSPP_YSTRIDE0	0x24
#define SSPP_FORMAT	0x30
#define SSPP_UNPACK	0x34
#define SSPP_OP_MODE	0x38

#define PP_TEAR_CHECK_EN	0x000
#define PP_INT_COUNT_VAL	0x014
#define PP_LINE_COUNT		0x02c
#define PP_AUTOREFRESH_CONFIG	0x030

struct sde_state sde;
int sde_verbose;	/* set by the shim: dump the LM/DSC/INTF blocks too */

static void dump_block(const char *name, uintptr_t base, uint32_t len)
{
	log_printf("sde %s @%lx:", name, (uint64_t)base);
	int printed = 0;
	for (uint32_t off = 0; off < len; off += 4) {
		uint32_t v = rd32(base + off);
		if (!v)
			continue;
		log_printf(" %x=%x", off, v);
		if (++printed % 6 == 0)
			log_printf("\n  ");
	}
	log_printf("\n");
}

void sde_probe(void)
{
	log_printf("sde: hw %x\n", rd32(MDP));
	for (int i = 0; i < 5; i++)
		log_printf("ctl%d: l0 %x l1 %x top %x flush %x\n", i, rd32(CTL(i) + CTL_LAYER(0)),
			   rd32(CTL(i) + CTL_LAYER(1)), rd32(CTL(i) + CTL_TOP),
			   rd32(CTL(i) + CTL_FLUSH));
	sde.pipe = 0;
	for (int i = 0; i < 5; i++) {
		uintptr_t b = MDP + sspp_off[i];
		uint32_t size = rd32(b + SSPP_SRC_SIZE);
		if (!size)
			continue;
		log_printf("sspp %s: %ux%u addr %x stride %x fmt %x unp %x op %x\n", sspp_name[i],
			   size & 0xffff, size >> 16, rd32(b + SSPP_SRC0_ADDR), rd32(b + SSPP_YSTRIDE0),
			   rd32(b + SSPP_FORMAT), rd32(b + SSPP_UNPACK), rd32(b + SSPP_OP_MODE));
		if (!sde.pipe) {
			sde.pipe = b;
			sde.pipe_index = i;
		}
	}
	if (sde_verbose)
		for (int i = 0; i < 3; i++)
			dump_block("lm", LM(i), 0x120);
	for (int i = 0; i < 2; i++)
		log_printf("pp%d: tear_en %x intcnt %x line %x autoref %x\n", i,
			   rd32(PP(i) + PP_TEAR_CHECK_EN), rd32(PP(i) + PP_INT_COUNT_VAL),
			   rd32(PP(i) + PP_LINE_COUNT), rd32(PP(i) + PP_AUTOREFRESH_CONFIG));
	if (sde_verbose) {
		dump_block("dsc0", DSC(0), 0x140);
		dump_block("intf1", INTF(1), 0x280);
	}
	sde.pp = 0;
	sde.tear_en = 0;
	for (int i = 0; i < 2; i++) {
		if (rd32(PP(i) + PP_TEAR_CHECK_EN) & 1) {
			sde.pp = PP(i);
			sde.tear_en = 1;
			break;
		}
	}
	log_printf("sde: pipe %s pp %lx tear %x\n", sde.pipe ? sspp_name[sde.pipe_index] : "none",
		   (uint64_t)sde.pp, sde.tear_en);
}

/* Command-mode panels show a frame only when one is started: autorefresh sends one per TE pulse. */
void sde_autorefresh(int frames)
{
	if (!sde.pp || !sde.tear_en)
		return;
	wr32(sde.pp + PP_AUTOREFRESH_CONFIG, frames ? (1u << 31) | (uint32_t)frames : 0);
	log_printf("sde: autorefresh -> %x\n", rd32(sde.pp + PP_AUTOREFRESH_CONFIG));
}
