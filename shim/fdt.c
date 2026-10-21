/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location). Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

uint32_t fdt_be32(uintptr_t a)
{
	return be32(a);
}

/* name matches node, or node is "name@unit" */
static int name_match(const char *node, const char *name)
{
	while (*name && *node == *name)
		node++, name++;
	return !*name && (!*node || *node == '@');
}

/* compatible is a list of NUL-terminated strings */
static int compat_match(uintptr_t val, uint32_t len, const char *compat)
{
	for (uintptr_t s = val; s < val + len; s = skip_str(s))
		if (streq((const char *)s, compat))
			return 1;
	return 0;
}

#define MAX_DEPTH	16

uintptr_t fdt_prop(uintptr_t fdt, const char *name, const char *compat, const char *prop, uint32_t *len)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	/* per open node: did it match, and where is the wanted prop (props precede children) */
	uint8_t match[MAX_DEPTH];
	uintptr_t found[MAX_DEPTH];
	uint32_t flen[MAX_DEPTH];
	int depth = -1;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE:
			if (++depth >= MAX_DEPTH)
				return 0;
			match[depth] = name && name_match((const char *)p, name);
			found[depth] = 0;
			p = align4(skip_str(p));
			break;
		case FDT_END_NODE:
			if (depth < 0)
				return 0;
			if (match[depth] && found[depth]) {
				*len = flen[depth];
				return found[depth];
			}
			depth--;
			break;
		case FDT_PROP: {
			uint32_t plen = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			const char *pname = (const char *)(strings + nameoff);
			if (depth >= 0) {
				if (compat && streq(pname, "compatible") && compat_match(val, plen, compat))
					match[depth] = 1;
				if (streq(pname, prop)) {
					found[depth] = val;
					flen[depth] = plen;
				}
			}
			p = align4(val + plen);
			break;
		}
		case FDT_NOP:
			break;
		default:
			return 0;
		}
	}
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}
