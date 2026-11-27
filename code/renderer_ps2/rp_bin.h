/* rp_bin.h: the PS2 renderer's images and models as files mapped into memory
   (rp_bin.c; tools/ps2conv/bin.c makes them and has their layout). */

#ifndef RP_BIN_H
#define RP_BIN_H

#include <mx/types.h>

struct rp_bin_entry {
	char name[64];
	u32 off, size;
	/* an image's: */
	u16 w, h, pic_w, pic_h;
	u8 psm, levels, alpha, pad;
	u32 off_clut;
	u32 off_level[7];
	u8 pad2[12];
};

struct rp_bin {
	const u8 *base;
	const struct rp_bin_entry *index;
	u32 size, count;
	/* the file a mod's lies over (fs_game's over the base game's): what this
	   one has not is looked for there */
	const struct rp_bin *under;
};
/* the images (kind 1) and the models (2): the mod's when one is played,
   over the base game's; else the base game's */
extern struct rp_bin rp_bin_tex, rp_bin_mdl;

/* Maps the file at path, which must be of kind: 0, or -1. */
int rp_bin_open(struct rp_bin *b, const char *path, u32 kind);
/* Reads [p, p + bytes) of a mapped file into memory now (a byte of each
   page): what a map's start should not leave to the first frame that needs
   it. */
void rp_bin_touch(const void *p, u32 bytes);
/* The entry of a name (lower case, no extension, '/' between directories) in
   b or the files under it, the file it is in in *in; or 0. */
const struct rp_bin_entry *rp_bin_find(const struct rp_bin *b, const char *name, const struct rp_bin **in);

#endif
