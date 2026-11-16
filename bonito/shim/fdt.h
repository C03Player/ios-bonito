#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/* Calls cb for every top-level "memory*" node's reg property (words converted to CPU order). */
void fdt_memory_regs(uintptr_t fdt, void (*cb)(const char *, const uint32_t *, unsigned));
