#pragma once
#include <stdint.h>

/* Display (MDSS/SDE 4.x, SDM670) state found by sde_probe(). */
struct sde_state {
	uintptr_t pipe;		/* first SSPP that is scanning out a buffer */
	int pipe_index;
	uintptr_t pp;		/* pingpong whose tear check is enabled */
	uint32_t tear_en;
};
extern struct sde_state sde;

void sde_probe(void);
void sde_autorefresh(int frames);
