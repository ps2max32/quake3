/* rp_bin.c: the files of images and of models the renderer maps into memory
   (tools/ps2bin.py). A page of them is read from the file when it is first
   touched and costs no write to give up: nothing of them is loaded or
   copied when a map starts. */

#include <mx/mx.h>
#include <mx/vm.h>

#include "rp_bin.h"

struct rp_bin rp_bin_tex, rp_bin_mdl;

struct head {
	u32 magic, version, kind, count, off_index, size, pad[2];
};

int rp_bin_open(struct rp_bin *b, const char *path, u32 kind) {
	u32 size = 0;
	if (b->base)
		return 0;
	const struct head *h = mx_vm_map_file(path, &size);
	if (!h)
		return -1;
	if (size < sizeof *h || h->magic != 0x4e494250u /* "PBIN" */ || h->version != 1 || h->kind != kind ||
	    h->size > size || h->off_index + h->count * sizeof(struct rp_bin_entry) > size) {
		mx_vm_unmap_file(h);
		return -1;
	}
	b->base = (const u8 *)h, b->size = size, b->count = h->count;
	b->index = (const struct rp_bin_entry *)(b->base + h->off_index);
	return 0;
}

void rp_bin_touch(const void *p, u32 bytes) {
	const volatile u8 *b = p;
	u32 sum = 0;
	for (u32 at = 0; at < bytes; at += MX_VM_PAGE - (u32)((uintptr_t)(b + at) & (MX_VM_PAGE - 1)))
		sum += b[at];
	if (bytes)
		sum += b[bytes - 1];
	(void)sum;
}

const struct rp_bin_entry *rp_bin_find(const struct rp_bin *b, const char *name) {
	u32 lo = 0, hi = b->count;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		const u8 *a = (const u8 *)b->index[mid].name, *n = (const u8 *)name;
		while (*a && *a == *n)
			a++, n++;
		if (*a == *n)
			return &b->index[mid];
		if (*a < *n)
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}
