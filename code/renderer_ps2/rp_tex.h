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
	const u8 *level[MX_TEX_LEVELS]; /* not split: where they lie (a locked file) */
	const void *clut;       /* 256 or 16 words in the GS's order; 0: none of its own */
	struct mx_tex_mem m;
	u64 key;
	struct mx_tex t;        /* where it is in GS memory in the frame being built */
	u32 frame;              /* that frame + 1 */
	u8 from;                /* the level that is t's level 0 (a texture whose first levels
	                           are not in memory yet) */
	u8 ok;
	u32 *luma[2];           /* rp_image_luma's CLUTs */
	u64 luma_key[2];
};

struct rp_tex_stats {
	u32 uploads, upload_kb; /* textures sent to GS memory, since the start */
	u32 full;               /* not placed: no room in GS memory */
	u32 later;              /* not placed in a frame that had sent its share of images */
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
   strip's record is written (mx_hires_strip). */
const struct mx_tex *rp_image_place(struct rp_image *im);
/* A CLUT that is no image's (16 x 16, or 8 x 2 with small; CT32, the GS's
   order, where it is read from): its block in GS memory for this frame, or
   0. */
u32 rp_clut_place(u64 key, const void *clut, int small);
/* A CLUT of the image's colours whose alpha is their brightness (0x80: white;
   twice: 0x80 from half white on): for blends that are a product with the
   picture, which the GS does with a source alpha. Its block in GS memory for
   this frame, or 0. */
u32 rp_image_luma(struct rp_image *im, int twice);
/* Free an image; all of them (nothing in flight may read them: after a
   frame without them is the one shown). */
void rp_image_free(struct rp_image *im);
void rp_images_free(void);
/* A key no image has. */
u64 rp_tex_key(void);

#endif
