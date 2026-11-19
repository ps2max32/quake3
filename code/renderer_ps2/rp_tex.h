/* rp_tex.h: the PS2 renderer's images (rp_tex.c): paletted textures in
   memory, placed in GS memory when a frame draws with them. */

#ifndef RP_TEX_H
#define RP_TEX_H

#include <mx/tex.h>
#include <mx/types.h>
#include <mx/vcache.h>

struct rp_image {
	char name[64];
	u16 w, h;               /* level 0's texels (powers of two) */
	u16 pic_w, pic_h;       /* the picture's own size, before it was made that */
	u8 psm, levels;         /* MX_GS_PSM_T8 or T4 */
	u8 alpha;               /* its palette has alpha of its own */
	u8 used, split;         /* split: its texels are in m (the renderer's heaps) */
	u8 wide;                /* a T4 with a CLUT of 256 words (the file's images without
	                           alpha, rp_image_bin): its colours are the first 16 entries,
	                           the lightmaps' brightness the alpha of 128 .. 255 */
	u8 kind;                /* RP_IMAGE_*: what it is of, for the report */
	u32 asked;              /* the frame a draw asked for it last, + 1 */
	u8 kept;                /* in the list of the screen's pictures that keep their place */
	const u8 *level[MX_TEX_LEVELS]; /* not split: where they lie (a locked file) */
	const void *clut;       /* 256 or 16 words in the GS's order; 0: none of its own */
	u64 clut_key;           /* wide: its CLUT's place of its own in GS memory */
	struct mx_tex_mem m;
	u64 key;
	struct mx_tex t;        /* where it is in GS memory in the frame being built */
	u32 frame;              /* that frame + 1 */
	u8 from;                /* the level that is t's level 0 (a texture whose first levels
	                           are not in memory yet) */
	u8 ok;
	u32 uses;               /* frames that drew with it */
	u8 want, want_lo;       /* the level it is wanted from in want_frame (rp_image_want), and half a level finer */
	u8 want_less;           /* and with the levels a quarter of a level less soon */
	u32 want_frame;         /* that frame, + 1 */
	u8 fine;                /* its first level's unit in the pool of fine levels, + 1; 0: none */
	u32 fine_frame;         /* the frame rp_image_fine gave it that level for, + 1 */
	u32 *luma[2];           /* rp_image_luma's CLUTs */
	u32 *opaque;            /* rp_image_opaque's */
	u64 opaque_key;
	u64 luma_key[2];
};

/* by its name: the map's and the scripts' (textures/), a lightmap page, the
   models', the screen's (gfx/2d, menu, ui, icons), the rest (sprites, effects) */
enum { RP_IMAGE_WORLD, RP_IMAGE_LIGHTMAP, RP_IMAGE_MODEL, RP_IMAGE_SCREEN, RP_IMAGE_OTHER, RP_IMAGE_KINDS };

struct rp_tex_stats {
	u32 uploads, upload_kb; /* textures sent to GS memory, since the start */
	u32 full;               /* not placed: no room in GS memory */
	u32 coarse;             /* placed without their finest levels: no room for all of them */
	u32 far;                /* placed from the level its distance needs (tight), not from its first */
	float far_bias_max;     /* the most levels sooner than the distance asks (tight, images without room) */
	float far_bias_sum;     /* its sum over the tight frames */
	u32 far_floors;         /* times going lower was too much (an image without room for a frame) */
	u32 fine, fine_sent, fine_wait; /* first levels drawn from the pool; sent there; not there yet (its units in flight) */
	u32 tight;              /* frames whose images did not fit GS memory at their full size */
	u32 want_max, want_sum; /* a frame's images at their full size: the most (blocks), the sum (KB) */
	u32 later;              /* not placed in a frame that had sent its share of images */
	u32 frames, used_kb;    /* frames counted; GS memory of the images they drew with, summed (KB) */
	u32 used_max_kb;        /* the most a frame drew with */
	u32 kind_blocks[RP_IMAGE_KINDS]; /* of used_kb, by kind (256-byte blocks) */
};
extern struct rp_tex_stats rp_tex_stats;

/* Once, when the screen is set up; GS memory outside the picture is the
   images'. */
void rp_tex_init(void);
/* At the start of each frame. */
void rp_tex_frame(void);
/* An image whose texels lie where they are read from (a mapped file, the
   CLUT and the levels one after the other: they are locked in memory while
   they are sent to GS memory): level[i] of w >> i x h >> i texels, clut as
   above. */
struct rp_image *rp_image_static(const char *name, u32 psm, u32 w, u32 h, u32 levels, int alpha,
                                 const u8 *const *level, const void *clut);
/* The image of the file of images (rp_bin.h) by its name there, made at
   its first use: its texels where the file has them, read into memory now
   (not by the first frame that draws it). bytes: its size is added. 0:
   none of that name. Without alpha of its own, its CLUT's alpha is the
   lightmaps' brightness (tools/ps2bin.py): draw it with TEX0's TCC 0. */
struct rp_image *rp_image_bin(const char *name, u32 *bytes);
/* An image with memory of its own: fill im->m.level[i] and im->m.clut (in
   the GS's order), then rp_image_ready. 0: no memory, or no image left. */
struct rp_image *rp_image_alloc(const char *name, u32 psm, u32 w, u32 h, u32 levels, int alpha);
void rp_image_ready(struct rp_image *im);
/* Image i (0 ..), or 0 when there is none there. */
struct rp_image *rp_image_at(u32 i);
/* The image by its name, or 0. */
struct rp_image *rp_image_find(const char *name);
/* Its place in GS memory for the frame being built (sent there if it was
   not: beam racing's once record), or 0 if there is no room. Not while a
   strip's record is written (mx_hires_strip). When a frame's images are more
   than GS memory holds, a model's or an effect's is placed without its
   finest level, and any image that finds no room without its finest two. */
const struct mx_tex *rp_image_place(struct rp_image *im);
/* 1 while the view weapon's images are placed: all their levels, whatever
   GS memory is short of (it is the model nearest the eye). */
extern int rp_tex_full;
/* The pool of fine levels. The file's images are up to 256 texels a side,
   but GS memory holds a frame's images only at 128 (the cache: every image
   from its level of 128, as when the file had no more). The first level of
   the few textures whose texels are largest on the screen is in a pool of
   its own: GS memory in units of 4 pages (a level of 256 x 256 is 8 pages
   in a row, which the cache seldom has free between its small pieces).
   rp_tex_fine_units: the pool's units when a map uses it (0: never).
   rp_image_fine_units: the units an image's first level takes (0: it has
   no such level). rp_image_fine: the image is drawn with its first level
   this frame (sent to the pool now if it was not there, in place of one no
   frame in flight draws with); 0: not this frame. rp_tex_level_at: the
   level for rho texels of the first level a pixel, b levels coarser. */
/* While a frame's images are more than GS memory holds (rp_tex_tight), a
   texture of the world is placed from the level its nearest surface in the
   view is drawn with, and no finer: a wall far away takes a quarter or a
   sixteenth of its memory and looks the same (the GS takes that level
   there). rp_image_want: the size of its texels there (in pixels), told
   for the frame before the image is placed. The level stays the one of the
   frame before while that is within half a level of what is needed: a
   texture at a boundary is not sent again each frame. While images still
   find no room, the levels go coarser sooner than the distance asks, a
   quarter of a level a frame, until the frame's images have room. */
int rp_tex_tight(void);
void rp_image_want(struct rp_image *im, float rho);
void rp_tex_fine_units(u32 units);
/* The pool takes its GS memory (on) or gives it back to the cache: when a
   map is loaded, by whether the map has images for it (the cache's entries
   go: no frame in flight may read them). */
void rp_tex_fine_use(int on);
u32 rp_tex_fine_pool(void);
u32 rp_image_fine_units(const struct rp_image *im);
int rp_image_fine(struct rp_image *im);
u32 rp_tex_level_at(float rho, float b);
/* A CLUT that is no image's (16 x 16, or 8 x 2 with small; CT32, the GS's
   order, where it is read from): its block in GS memory for this frame, or
   0. */
u32 rp_clut_place(u64 key, const void *clut, int small);
/* A CLUT of the image's colours whose alpha is their brightness (0x80: white;
   twice: 0x80 from half white on): for blends that are a product with the
   picture, which the GS does with a source alpha. Its block in GS memory for
   this frame, or 0. */
/* For an image with alpha of its own on a lightmapped surface that does not
   use the alpha: a CLUT of its colours with the lightmaps' brightness in the
   alpha of 128 .. 255 (16 x 16, CT32), its block in GS memory for this frame;
   0: none (no room, or the image has no alpha). */
u32 rp_image_opaque(struct rp_image *im);

u32 rp_image_luma(struct rp_image *im, int twice);
/* Free an image; all of them (nothing in flight may read them: after a
   frame without them is the one shown). */
void rp_image_free(struct rp_image *im);
void rp_images_free(void);
/* For a run's report: the images that took most GS memory over its frames
   (their blocks times the frames that drew with them), through print. */
void rp_tex_report(void (*print)(const char *name, u32 kb, u32 frames, u32 w, u32 h));
/* A key no image has. */
u64 rp_tex_key(void);

#endif
