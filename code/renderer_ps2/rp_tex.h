/* rp_tex.h: the PS2 renderer's images (rp_tex.c): paletted textures in
   memory, placed in GS memory when a frame draws with them. */

#ifndef RP_TEX_H
#define RP_TEX_H

#include <mx/tex.h>
#include <mx/types.h>
#include <mx/vcache.h>

struct rp_image {
	/* what rp_image_place reads of an image placed the frame before: its
	   first cache line, then t (the struct is aligned to a line) */
	u32 frame;              /* the frame it was placed in (t), + 1 */
	u32 asked;              /* the frame a draw asked for it last, + 1 */
	u32 uses;               /* frames that drew with it */
	/* placed the plain way (whole, from the cache, no choice of level made
	   for it): the next frame takes its entries again without asking
	   (rp_image_place) while nothing has changed */
	int ent, clut_ent;      /* the cache's entries (mx_vcache_entry) */
	u32 plain_max;          /* the largest level's size it was placed for */
	u32 plain_blocks;       /* what it counts for in the frame's memory */
	u32 fine_frame;         /* the frame rp_image_fine gave it that level for, + 1 */
	u8 from;                /* the level that is t's level 0 (a texture whose first levels
	                           are not in memory yet) */
	u8 ok;
	u8 plain, plain_clut;   /* plain_clut: with a CLUT of its own in the cache */
	u8 fine;                /* its first level's unit in the pool of fine levels, + 1; 0: none */
	u8 kind;                /* RP_IMAGE_*: what it is of, for the report */
	u8 kept;                /* in the list of the screen's pictures that keep their place */
	u64 key;
	u64 clut_key;           /* wide: its CLUT's place of its own in GS memory */
	struct mx_tex t;        /* where it is in GS memory in the frame being built */
	char name[64];
	u16 w, h;               /* level 0's texels (powers of two) */
	u16 pic_w, pic_h;       /* the picture's own size, before it was made that */
	u8 psm, levels;         /* MX_GS_PSM_T8 or T4 */
	u8 alpha;               /* its palette has alpha of its own */
	u8 used, split;         /* split: its texels are in m (the renderer's heaps) */
	u8 wide;                /* a T4 with a CLUT of 256 words (the file's images without
	                           alpha, rp_image_bin): its colours are the first 16 entries,
	                           the lightmaps' brightness the alpha of 128 .. 255 */
	u8 pinned;              /* its small levels and its CLUT are pinned in memory (image_pin) */
	u16 stream;             /* in the stream area in frame stream_frame: its instance + 1 */
	u32 stream_frame;       /* that frame + 1 */
	const u8 *level[MX_TEX_LEVELS]; /* not split: where they lie (a locked file) */
	const void *clut;       /* 256 or 16 words in the GS's order; 0: none of its own */
	struct mx_tex_mem m;
	u8 want, want_lo;       /* the level it is wanted from in want_frame (rp_image_want), and half a level finer */
	u8 want_less;           /* and with the levels a quarter of a level less soon */
	u32 want_frame;         /* that frame, + 1 */
	u32 *luma[2];           /* rp_image_luma's CLUTs */
	u32 *opaque;            /* rp_image_opaque's */
	u64 opaque_key;
	u64 luma_key[2];
} __attribute__((aligned(64)));

/* by its name: the map's and the scripts' (textures/), a lightmap page, the
   models', the screen's (gfx/2d, menu, ui, icons), the rest (sprites, effects) */
enum { RP_IMAGE_WORLD, RP_IMAGE_LIGHTMAP, RP_IMAGE_MODEL, RP_IMAGE_SCREEN, RP_IMAGE_OTHER, RP_IMAGE_KINDS };

struct rp_tex_stats {
	u32 uploads, upload_kb; /* textures sent to GS memory, since the start */
	u32 full;               /* not placed: no room in GS memory */
	u32 coarse;             /* placed without their finest levels: no room for all of them */
	u32 far;                /* placed from the level its distance needs (tight), not from its first */
	u32 file_waits, file_wait_kb, file_wait_us; /* levels sent from the file while not in memory: the game waited */
	u32 paged;              /* placed from a coarser level as their finer ones were not in memory (asked in) */
	u32 stream, stream_kb;  /* images streamed (sent in the passes, rp_tex_stream_*), their KB, frames summed */
	u32 stream_segs;        /* their pieces of the frame's runs (an image put out of the area and back: two) */
	u32 stream_sent, stream_sent_kb; /* sent in the strips' records, frames summed (a record's, not a field's) */
	u32 stream_over;        /* not streamed: over the frame's KB (r_ps2texstreamkb), or larger than the area */
	u32 stream_noflush;     /* of stream_sent, without waiting for VU1 first */
	u32 clut_slot, clut_cache; /* CLUTs drawn with from the framebuffer's spare blocks, from the cache (frames summed) */
	u32 clut_slot_sent;     /* sent into those blocks */
	float far_bias_max;     /* the most levels sooner than the distance asks (tight, images without room) */
	float far_bias_sum;     /* its sum over the tight frames */
	u32 far_floors;         /* times going lower was too much (an image without room for a frame) */
	u32 fine, fine_sent, fine_wait; /* first levels drawn from the pool; sent there; not there yet (its units in flight) */
	u32 tight;              /* frames whose images did not fit GS memory at their full size */
	u32 want_max, want_sum; /* a frame's images at their full size: the most (blocks), the sum (KB) */
	u32 later;              /* not placed in a frame that had sent its share of images */
	u32 again;              /* taken as the frame before had them, without asking the cache (frames summed) */
	u32 frames, used_kb;    /* frames counted; GS memory of the images they drew with, summed (KB) */
	u32 used_max_kb;        /* the most a frame drew with */
	u32 kind_blocks[RP_IMAGE_KINDS]; /* of used_kb, by kind (256-byte blocks) */
};
extern struct rp_tex_stats rp_tex_stats;
/* the images' memory pinned now (their levels under 32 KiB, their CLUTs), KB */
extern u32 rp_tex_pinned_kb;
/* the CLUTs' places in the framebuffer's spare blocks (mx_hires_spare), 0: none */
u32 rp_tex_clut_slots(void);

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
/* the GS memory the governor reckons with, in percent of the cache's (100) */
void rp_tex_room(u32 pct);
/* the largest level the cache places itself (128) */
void rp_tex_max(u32 texels);
/* a test (r_ps2texforce): no image coarser for want of room; the stream area
   takes what the cache has no room for, or it is not drawn */
void rp_tex_force(int on);
float rp_tex_far_bias(void);
/* the frame before's images at their full size, and what the cache had at
   the start (GS memory beside the whole width's picture), 256-byte blocks */
u32 rp_tex_want_blocks(void);
u32 rp_tex_full_blocks(void);
void rp_image_want(struct rp_image *im, float rho);
void rp_image_want_model(struct rp_image *im, float rho);
void rp_tex_fine_units(u32 units);
/* The pool takes its GS memory (on) or gives it back to the cache: when a
   map is loaded, by whether the map has images for it (the cache's entries
   go: no frame in flight may read them). */
void rp_tex_fine_use(int on);
u32 rp_tex_fine_pool(void);
u32 rp_image_fine_units(const struct rp_image *im);
int rp_image_fine(struct rp_image *im);
u32 rp_tex_level_at(float rho, float b);
/* The stream area (r_ps2texstream: its pages, 0 none; r_ps2texstreamkb: the
   most a frame streams): GS memory for images the cache has no room for, sent
   in the passes (the strips' records) right before the runs that draw with
   them. rp_image_place gives such an image's mx_tex with an instance in
   its blocks (not yet a place); at the end of the frame's list:
   rp_tex_stream_plan places the frame's instances, by the runs that draw
   with them (an instance whose pages a later one needs before it is drawn
   with again is put out, and back in a piece of its own: seg), then for each
   run rp_tex_stream_fix gives its texture the blocks of its piece, and in
   each strip rp_tex_stream_need the chain that sends it when the strip's GS
   memory does not hold it yet (rp_tex_stream_strip: a strip begins; first:
   the field's first, which knows nothing of the field before). A pass left
   out (mx_hires_stats.late) leaves the next strip without what it did not
   send: that strip is drawn with another texture for a field. */
void rp_tex_stream(u32 pages, u32 kb);
/* Before rp_tex_frame: the time the passes of a field had to spare lately
   (us, below 0: over) and whether a pass was late since (overtaken by the
   beam, or left out): the KB a frame streams follows it, from the frame
   before's, up to r_ps2texstreamkb; after a late pass the least for a
   while. */
void rp_tex_stream_room(float us, int late);
u32 rp_tex_stream_pages(void);
/* the KB this frame may stream (rp_tex_stream_room) */
u32 rp_tex_stream_budget(void);
int rp_tex_stream_id(const struct mx_tex *t);
void rp_tex_stream_plan(const s16 *id, s16 *seg, u32 n);
void rp_tex_stream_fix(u32 seg, struct mx_tex *t, u32 *lm_cbp);
void rp_tex_stream_strip(int first);
const void *rp_tex_stream_need(u32 seg);

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
