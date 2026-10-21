#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * Find property prop of the first node (any depth) whose name is name, or
 * name@unit, or whose compatible list contains compat (whichever is non-0).
 * name "" matches the root node. Returns the value address (big endian)
 * and sets *len, or 0 if absent.
 */
uintptr_t fdt_prop(uintptr_t fdt, const char *name, const char *compat, const char *prop, uint32_t *len);

uint32_t fdt_be32(uintptr_t a);
