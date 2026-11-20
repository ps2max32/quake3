/* rp_tex.c: the PS2 renderer's images. An image is a paletted texture (T8 or
   T4, its CLUT of 32-bit colours) with its MIPMAP levels, in memory: where a
   mapped file has it (the file of images, tools/ps2bin.py; a map's lightmap
   pages, tools/ps2map.py), or in the renderer's heaps,
   the first levels pageable and the rest resident (<mx/vcache.h>: a texture
   whose first levels are paged out is drawn from the rest meanwhile).

   GS memory outside the picture is a cache of them (<mx/vcache.h>): an image
   is placed when a frame draws with it and sent there if it was not, in beam
   racing's once record (the passes replay a frame's stream; an upload is done
   once, before them). What no frame in flight draws with makes room. */

#include <mx/frame.h>
#include <mx/gif.h>
#include <mx/hires.h>
#include <mx/mem.h>
#include <mx/mx.h>
#include <mx/vif.h>
#include <mx/vm.h>
#include <mx/vram.h>

#include "rp_bin.h"
#include "rp_list.h"
#include "rp_tex.h"

#define MAX_IMAGE 2048
static struct rp_image images[MAX_IMAGE];
static u32 image_hi;
static u64 serial;
static struct mx_heap *heap_pin, *heap_asset;
struct rp_tex_stats rp_tex_stats;
u32 rp_tex_pinned_kb;

#define FINE_UNIT 4 /* pages */
#define FINE_MAX 32
static struct rp_image *fine_unit[FINE_MAX];
static u32 fine_page, fine_n, fine_cfg = 16;

void rp_tex_fine_units(u32 units) { fine_cfg = units > FINE_MAX ? FINE_MAX : units & ~1u; }
u32 rp_tex_fine_pool(void) { return fine_n; }

void rp_tex_fine_use(int on) {
	u32 want = on ? fine_cfg : 0;
	if (want == fine_n)
		return;
	/* the cache gives its pages back (its entries go: every image is sent again
	   when it is next drawn), the pool takes or gives its own, the cache takes
	   the rest as it needs them */
	mx_vcache_init(0, mx_hires_passes() ? MX_VCACHE_ASYNC : MX_VCACHE_STREAM, MX_VCACHE_AUTO);
	for (u32 k = 0; k < fine_n; k++)
		if (fine_unit[k])
			fine_unit[k]->fine = 0, fine_unit[k] = 0;
	if (fine_n)
		mx_vram_give(fine_page, fine_n * FINE_UNIT), fine_n = 0;
	if (want) {
		u32 p = mx_vram_find(want * FINE_UNIT);
		if (p != ~0u && mx_vram_take(p, want * FINE_UNIT, MX_VRAM_APP) == 0)
			fine_page = p, fine_n = want;
	}
}

static u32 s_page0, s_pages; /* the stream area (rp_tex_stream) */
static u32 full_blocks;      /* GS memory free at the start (rp_tex_full_blocks) */

/* CLUTs in the framebuffer's spare blocks (mx_hires_spare: below the
   picture's last line, a KB each): a CLUT keeps its place while frames draw
   with it, and takes the place no frame in flight draws with that was used
   longest ago; when there is none, the cache has it, as before. They leave
   the cache's room to the textures (q3dm6: about 150 CLUTs a frame). */
#define CLUT_SLOTS 128
static u32 cs_at[CLUT_SLOTS], cs_n, cs_used[CLUT_SLOTS]; /* the block; the frame + 1 that drew with it last */
static u64 cs_key[CLUT_SLOTS];

u32 rp_tex_clut_slots(void) { return cs_n; }

/* key's place: HIT, MISS (placed now: the caller sends it), FULL (none free) */
static int clut_slot(u64 key, u32 *bp) {
	u32 frame = mx_frame_number(), done = mx_frame_done(), best_used = ~0u;
	int best = -1;
	for (u32 i = 0; i < cs_n; i++) {
		if (cs_key[i] == key) {
			cs_used[i] = frame + 1, *bp = cs_at[i];
			rp_tex_stats.clut_slot++;
			return MX_VCACHE_HIT;
		}
		if (cs_used[i] <= done && cs_used[i] < best_used)
			best = (int)i, best_used = cs_used[i];
	}
	if (best < 0)
		return MX_VCACHE_FULL;
	cs_key[best] = key, cs_used[best] = frame + 1, *bp = cs_at[best];
	rp_tex_stats.clut_slot++, rp_tex_stats.clut_slot_sent++;
	return MX_VCACHE_MISS;
}

static void clut_slot_drop(u64 key) {
	for (u32 i = 0; i < cs_n; i++)
		if (cs_key[i] == key)
			cs_key[i] = 0;
}

/* a CLUT of its own (not with the levels in the cache): the slots, else the cache */
static int clut_place(u64 key, int small, u32 *bp) {
	int r = clut_slot(key, bp);
	if (r == MX_VCACHE_FULL) {
		r = mx_vcache_buf(key, small ? 8 : 16, small ? 2 : 16, MX_GS_PSM_CT32, bp);
		if (r != MX_VCACHE_FULL)
			rp_tex_stats.clut_cache++;
	}
	return r;
}

void rp_tex_init(void) {
	/* no pool until a map has images for it (rp_tex_fine_use) */
	fine_n = 0;
	s_pages = 0;
	cs_n = mx_hires_passes() ? mx_hires_spare(cs_at, CLUT_SLOTS) : 0;
	full_blocks = mx_vram_count(MX_VRAM_FREE) * 32;
	for (u32 i = 0; i < CLUT_SLOTS; i++)
		cs_key[i] = 0, cs_used[i] = 0;
	memset(fine_unit, 0, sizeof fine_unit);
	mx_vcache_init(0, mx_hires_passes() ? MX_VCACHE_ASYNC : MX_VCACHE_STREAM, MX_VCACHE_AUTO);
	if (!heap_pin) {
		heap_pin = mx_heap_create("q3.tex", MX_MEM_PINNED, 0);
		heap_asset = mx_heap_create("q3.texels", MX_MEM_ASSET, 16u << 20);
	}
}

/* images sent to GS memory in the frame being built: no more than fit its
   record (about 60 quadwords of tags each) */
#define FRAME_UPLOADS 32
static u32 frame_uploads;

static u32 frame_blocks; /* GS memory (256-byte blocks) of the images the frame drew with */
/* The same had every image all its levels: against what GS memory holds
   this says whether a frame's images fit. When they do not, the models' and
   the effects' images are placed without their finest level (a quarter of
   the memory; the map's stay as they are) until the frames have fitted
   again for two seconds: images that push each other out of GS memory
   every frame are sent again every frame. */
static u32 want_blocks, tight, calm, want_last;
u32 rp_tex_want_blocks(void) { return want_last; }
u32 rp_tex_full_blocks(void) { return full_blocks; }
/* While tight, how much sooner (levels) a texture of the world goes to a
   coarser level than its distance asks: a quarter of a level more after each
   frame in which an image found no room at all, so up to where the frame's
   images fit (to half a level when images only lost their finest levels for
   want of room: the view weapon's, which then stay so, as going further
   takes more from the walls than it gives the weapon); back to none when
   the frames fit at full size again (tight ends). It goes down a quarter
   when the frame's images would take under 93 % of GS memory with it, for
   half a second (lower_blocks: what the textures placed coarser would take
   more). When that was too much (an image without room right after), the
   value it came from is the floor: no lower again but under 85 %, until the
   view is another (its images at full size 5 % of GS memory more or less)
   or tight ends: a view at the edge loses an image for one frame once. */
static float far_bias, far_floor;
static u32 room_pct = 100;
void rp_tex_room(u32 pct) { room_pct = pct < 10 ? 10 : pct; }
/* r_ps2texmax: the largest level (texels a side) the cache places itself;
   a larger one only through the pool of fine levels (a test: 256, every
   image from its level of 256 through the cache) */
static u32 tex_max = 128;
void rp_tex_max(u32 texels) { tex_max = texels < 128 ? 128 : texels; }
static u32 full_at, coarse_at; /* rp_tex_stats.full and coarse at the frame before */
static u32 lower_blocks, lower_calm, lowered_at, floor_want;
#define LOWER_WAIT 30
#define FAR_BIAS_MAX 2.0f
static int keeping; /* rp_tex_frame asks for the screen's pictures: not a draw's asking */
#define KEPT 128
static struct rp_image *kept[KEPT]; /* the screen's pictures asked for lately */
static u32 nkept;
int rp_tex_full;

u32 rp_tex_level_at(float rho, float b) {
	union {
		float f;
		u32 u;
	} a = {rho};
	if (!(rho > 0))
		return 0;
	/* log2 (below 0 where a texel is larger than a pixel: such a texture keeps
	   its first level under a bias): the exponent and the fraction's first
	   bits (a sixteenth of a level) */
	float l = (float)((s32)(a.u >> 23) - 127) + (float)(a.u >> 19 & 15) * (1.0f / 16.0f) + b;
	return l < 0 ? 0 : l > 6.0f ? 6 : (u32)l;
}



static int starts(const char *s, const char *with) {
	while (*with && *s == *with)
		s++, with++;
	return !*with;
}

static u32 kind_of(const char *name) {
	return starts(name, "*lightmap")   ? RP_IMAGE_LIGHTMAP
	       : starts(name, "textures/") ? RP_IMAGE_WORLD
	       : starts(name, "models/")   ? RP_IMAGE_MODEL
	       : starts(name, "gfx/2d/") || starts(name, "menu/") || starts(name, "ui/") || starts(name, "icons/")
	           ? RP_IMAGE_SCREEN
	           : RP_IMAGE_OTHER;
}

static u32 nsinst, s_kb;
static u32 stream_cap_blocks(void);

void rp_tex_frame(void) {
	mx_vcache_frame(mx_frame_number(), mx_frame_done());
	frame_uploads = 0;
	nsinst = 0, s_kb = 0;
	u32 kb = frame_blocks / 4, drawn = frame_blocks;
	rp_tex_stats.frames++, rp_tex_stats.used_kb += kb;
	if (kb > rp_tex_stats.used_max_kb)
		rp_tex_stats.used_max_kb = kb;
	frame_blocks = 0;
	{
		struct mx_vcache_stats vs;
		mx_vcache_stats(&vs);
		/* r_ps2texroom (rp_tex_room): the GS memory the governor reckons
		   with, in percent of the cache's (a test: more than there is
		   streams more images through it); and what the stream area may
		   send this frame */
		u32 cap = vs.pages * 32 * room_pct / 100 + stream_cap_blocks();
		/* tight: the frame's images at full size are 90 % of what there is;
		   until the frames have been under 80 % for a second */
		if (want_blocks * 100 > cap * 90)
			tight = 1, calm = 0;
		else if (want_blocks * 100 > cap * 80)
			calm = 0;
		else if (tight && ++calm > 60)
			tight = 0;
		u32 frame = mx_frame_number();
		u32 pred = (drawn + lower_blocks) * 100;
		/* the counts were set to 0 (a run's statistics begin) */
		if (rp_tex_stats.full < full_at || rp_tex_stats.coarse < coarse_at)
			full_at = rp_tex_stats.full, coarse_at = rp_tex_stats.coarse;
		if (far_floor > 0 && (want_blocks > floor_want ? want_blocks - floor_want : floor_want - want_blocks) * 20 > cap)
			far_floor = 0;
		if (!tight)
			far_bias = 0, far_floor = 0, lower_calm = 0;
		else if ((rp_tex_stats.full != full_at && far_bias < FAR_BIAS_MAX) ||
		         (rp_tex_stats.coarse != coarse_at && far_bias < 0.5f)) {
			far_bias += 0.25f, lower_calm = 0;
			if (lowered_at && frame - lowered_at < 120 && rp_tex_stats.full != full_at)
				far_floor = far_bias, floor_want = want_blocks, rp_tex_stats.far_floors++;
		} else if (far_bias > 0 && rp_tex_stats.full == full_at &&
		           (pred < cap * 85 || (pred < cap * 93 && far_bias > far_floor))) {
			if (++lower_calm >= LOWER_WAIT)
				far_bias -= 0.25f, lower_calm = 0, lowered_at = frame;
		} else
			lower_calm = 0;
		lower_blocks = 0;
		full_at = rp_tex_stats.full, coarse_at = rp_tex_stats.coarse;
		if (far_bias > rp_tex_stats.far_bias_max)
			rp_tex_stats.far_bias_max = far_bias;
		rp_tex_stats.far_bias_sum += far_bias;
		if (want_blocks > rp_tex_stats.want_max)
			rp_tex_stats.want_max = want_blocks;
		rp_tex_stats.want_sum += want_blocks / 4;
		rp_tex_stats.tight += tight;
		want_last = want_blocks;
		want_blocks = 0;
	}
	/* The screen's pictures (the text, the HUD) drawn in the last half second
	   keep their place: asked for here, first in every frame. They are drawn
	   last, and a frame none of them was drawn in gave their room to the
	   world's images; once every image in GS memory is one a frame in flight
	   draws with, the text's 256 x 256 finds no 64 KB in one piece again, and
	   it has one level (no coarser one to fall back on). */
	u32 now = mx_frame_number();
	keeping = 1;
	for (u32 i = 0; i < nkept;) {
		struct rp_image *im = kept[i];
		if (im->used && im->kind == RP_IMAGE_SCREEN && im->asked && now + 1 - im->asked <= 30) {
			rp_image_place(im);
			i++;
		} else
			im->kept = 0, kept[i] = kept[--nkept];
	}
	keeping = 0;
}

u64 rp_tex_key(void) { return ++serial; }

void rp_tex_report(void (*print)(const char *name, u32 kb, u32 frames, u32 w, u32 h)) {
	for (int n = 0; n < 24; n++) {
		struct rp_image *best = 0;
		u64 bw = 0;
		for (u32 i = 0; i < image_hi; i++) {
			struct rp_image *im = &images[i];
			u64 w = (u64)im->uses * im->t.blocks;
			if (im->used && im->uses && w > bw)
				best = im, bw = w;
		}
		if (!best)
			break;
		print(best->name, best->t.blocks / 4, best->uses, best->w, best->h);
		best->uses = 0;
	}
}

static u32 log2u(u32 v) {
	u32 n = 0;
	while ((1u << n) < v)
		n++;
	return n;
}

static struct rp_image *image_new(const char *name, u32 psm, u32 w, u32 h, u32 levels, int alpha) {
	u32 i;
	for (i = 0; i < image_hi && images[i].used; i++)
		;
	if (i == MAX_IMAGE)
		return 0;
	struct rp_image *im = &images[i];
	*im = (struct rp_image){0};
	u32 k = 0;
	for (; name[k] && k < sizeof im->name - 1; k++)
		im->name[k] = name[k];
	im->w = im->pic_w = (u16)w, im->h = im->pic_h = (u16)h;
	im->psm = (u8)psm, im->levels = (u8)(levels > MX_TEX_LEVELS ? MX_TEX_LEVELS : levels);
	im->alpha = (u8)alpha;
	im->kind = (u8)kind_of(im->name);
	im->key = rp_tex_key(), im->clut_key = rp_tex_key();
	im->used = 1;
	if (i == image_hi)
		image_hi++;
	return im;
}

/* Paging of a mapped file's images: a level of PAGED_MIN bytes or more may
   be paged out, as long as a coarser level is there to draw with meanwhile
   (rp_image_place sends that one and asks the pager for the finer, never
   waiting for it). The rest, the smaller levels, the last level and the
   CLUT, is pinned in memory while the image is there. */
#define PAGED_MIN 32768u

static u32 level_bytes(const struct rp_image *im, u32 l) {
	u32 w = im->w >> l, h = im->h >> l;
	w = w ? w : 1, h = h ? h : 1;
	return im->psm == MX_GS_PSM_T4 ? (w * h + 1) / 2 : w * h;
}

static u32 clut_bytes(const struct rp_image *im) { return im->psm == MX_GS_PSM_T4 && !im->wide ? 64 : 1024; }

static int level_pageable(const struct rp_image *im, u32 l) {
	return l + 1 < im->levels && level_bytes(im, l) >= PAGED_MIN;
}

/* how a level (l, or the CLUT: -1) is sent (xfer_lock): core memory, a
   mapped file's page locked for the frame, or pinned already */
enum { XFER_CORE, XFER_FILE, XFER_PINNED };
static int xfer_mode(const struct rp_image *im, int l) {
	if (im->split)
		return XFER_CORE;
	return im->pinned && (l < 0 || !level_pageable(im, (u32)l)) ? XFER_PINNED : XFER_FILE;
}

static void image_pin(struct rp_image *im, int on) {
	if (im->split || (on && im->pinned) || (!on && !im->pinned))
		return;
	u32 kb = 0;
	for (u32 l = 0; l < im->levels; l++)
		if (!level_pageable(im, l) && mx_is_vm(im->level[l])) {
			if (on)
				mx_vm_pin(im->level[l], level_bytes(im, l), 0);
			else
				mx_vm_unpin(im->level[l], level_bytes(im, l));
			kb += level_bytes(im, l);
		}
	if (im->clut && mx_is_vm(im->clut)) {
		if (on)
			mx_vm_pin(im->clut, clut_bytes(im), 0);
		else
			mx_vm_unpin(im->clut, clut_bytes(im));
		kb += clut_bytes(im);
	}
	im->pinned = (u8)on;
	kb = (kb + 1023) / 1024;
	if (on)
		rp_tex_pinned_kb += kb;
	else
		rp_tex_pinned_kb -= kb < rp_tex_pinned_kb ? kb : rp_tex_pinned_kb;
}

static struct rp_image *image_static(const char *name, u32 psm, u32 w, u32 h, u32 levels, int alpha,
                                     const u8 *const *level, const void *clut, int wide) {
	struct rp_image *im = image_new(name, psm, w, h, levels, alpha);
	if (!im)
		return 0;
	for (u32 l = 0; l < im->levels; l++)
		im->level[l] = level[l];
	im->clut = clut;
	im->wide = (u8)wide;
	image_pin(im, 1);
	return im;
}

struct rp_image *rp_image_static(const char *name, u32 psm, u32 w, u32 h, u32 levels, int alpha,
                                 const u8 *const *level, const void *clut) {
	return image_static(name, psm, w, h, levels, alpha, level, clut, 0);
}

struct rp_image *rp_image_bin(const char *name, u32 *bytes) {
	struct rp_image *im = rp_image_find(name);
	if (im)
		return im;
	const struct rp_bin_entry *e = rp_bin_tex.base ? rp_bin_find(&rp_bin_tex, name) : 0;
	if (!e)
		return 0;
	const u8 *level[MX_TEX_LEVELS];
	u32 levels = e->levels > MX_TEX_LEVELS ? MX_TEX_LEVELS : e->levels;
	for (u32 l = 0; l < levels; l++)
		level[l] = rp_bin_tex.base + e->off_level[l];
	im = image_static(name, e->psm, e->w, e->h, levels, e->alpha, level, rp_bin_tex.base + e->off_clut,
	                  e->psm == MX_GS_PSM_T4 && !e->alpha);
	if (!im)
		return 0;
	im->pic_w = e->pic_w, im->pic_h = e->pic_h;
	if (bytes)
		*bytes += e->size;
	/* its entry is in memory now, pinned (image_static): its levels under 32
	   KiB and its CLUT. Its levels of 32 KiB or more lie on pages of their own
	   after the entries (tools/ps2bin.py), read by the pager when a frame first
	   wants them (rp_image_place), not waited for. */
	return im;
}

struct rp_image *rp_image_alloc(const char *name, u32 psm, u32 w, u32 h, u32 levels, int alpha) {
	struct rp_image *im = image_new(name, psm, w, h, levels, alpha);
	if (!im)
		return 0;
	u32 clut_bytes = psm == MX_GS_PSM_T4 ? 64 : 1024;
	/* the first levels pageable when there are three or more and the first is
	   32 KiB or more (PAGED_MIN); else all of it pinned */
	if (!heap_pin || mx_tex_mem_alloc(&im->m, psm, log2u(w), log2u(h), im->levels, clut_bytes, heap_pin,
	                                  im->levels >= 3 && level_bytes(im, 0) >= PAGED_MIN ? heap_asset : 0) < 0) {
		im->used = 0;
		return 0;
	}
	im->split = 1;
	for (u32 l = 0; l < im->levels; l++)
		im->level[l] = im->m.level[l];
	im->clut = im->m.clut;
	return im;
}

void rp_image_ready(struct rp_image *im) {
	/* the DMAC reads memory, not the cache */
	if (im->m.head)
		mx_dcache_writeback(im->m.head, im->m.head_bytes);
	mx_dcache_writeback(im->m.tail_mem, im->m.tail_bytes + (im->psm == MX_GS_PSM_T4 ? 64 : 1024));
}

struct rp_image *rp_image_at(u32 i) { return i < image_hi && images[i].used ? &images[i] : 0; }

struct rp_image *rp_image_find(const char *name) {
	for (u32 i = 0; i < image_hi; i++) {
		if (!images[i].used)
			continue;
		const char *a = images[i].name, *b = name;
		while (*a && *a == *b)
			a++, b++;
		if (!*a && !*b)
			return &images[i];
	}
	return 0;
}

u32 rp_image_luma(struct rp_image *im, int twice) {
	u32 n = im->psm == MX_GS_PSM_T4 ? 16 : 256;
	twice = twice != 0;
	if (!im->clut)
		return 0;
	if (!im->luma[twice]) {
		u32 *c = heap_pin ? mx_heap_alloc(heap_pin, n * 4, 64) : 0;
		const u32 *src = im->clut;
		if (!c)
			return 0;
		for (u32 i = 0; i < n; i++) {
			/* a wide CLUT's entries 8 .. 15 are its second line of 16 */
			u32 s = src[im->wide && i >= 8 ? i + 8 : i];
			u32 r = s & 0xFF, g = s >> 8 & 0xFF, b = s >> 16 & 0xFF;
			u32 m = r > g ? r : g;
			m = ((b > m ? b : m) + 1) >> (twice ? 0 : 1);
			c[i] = (s & 0xFFFFFF) | (m > 0x80 ? 0x80 : m) << 24;
		}
		mx_dcache_writeback(c, n * 4);
		im->luma[twice] = c, im->luma_key[twice] = rp_tex_key();
	}
	return rp_clut_place(im->luma_key[twice], im->luma[twice], n == 16);
}

/* The CLUT of an image with alpha of its own for a surface that does not
   use that alpha and has a lightmap: its colours, and in the alpha of the
   entries 128 .. 255 the lightmaps' brightness (as the CLUT of an image
   without alpha has it, tools/ps2bin.py), so that the texture and the
   lightmap are drawn with one CLUT, from one vertex. A T4's 16 colours are
   the first 16 of the 256. Its block in GS memory for this frame, or 0. */
u32 rp_image_opaque(struct rp_image *im) {
	if (!im->clut || !im->alpha)
		return 0;
	if (!im->opaque) {
		u32 *c = heap_pin ? mx_heap_alloc(heap_pin, 256 * 4, 64) : 0;
		const u32 *src = im->clut;
		if (!c)
			return 0;
		int t4 = im->psm == MX_GS_PSM_T4;
		for (u32 k = 0; k < 256; k++) {
			/* k: a place in the GS's order; i: the entry that lies there */
			u32 i = (k & 0x18) == 0x08 ? k + 8 : (k & 0x18) == 0x10 ? k - 8 : k;
			u32 rgb = !t4 ? src[k] & 0xFFFFFF : i < 16 ? src[i] & 0xFFFFFF : 0x808080;
			u32 a = i < 128 ? 0x80 : i == 255 ? 255 : (i - 128) * 2;
			c[k] = rgb | a << 24;
		}
		mx_dcache_writeback(c, 256 * 4);
		im->opaque = c, im->opaque_key = rp_tex_key();
	}
	return rp_clut_place(im->opaque_key, im->opaque, 0);
}

static void luma_free(struct rp_image *im) {
	for (int k = 0; k < 2; k++)
		if (im->luma[k])
			mx_heap_free(heap_pin, im->luma[k]), im->luma[k] = 0;
	if (im->opaque)
		mx_heap_free(heap_pin, im->opaque), im->opaque = 0;

}

static void fine_drop(struct rp_image *o);
void rp_image_free(struct rp_image *im) {
	if (!im || !im->used)
		return;
	for (u32 i = 0; im->kept && i < nkept; i++)
		if (kept[i] == im)
			kept[i] = kept[--nkept], im->kept = 0;
	luma_free(im);
	image_pin(im, 0);
	if (im->fine)
		fine_drop(im);
	for (u32 f = 0; f < im->levels; f++)
		mx_vcache_drop(mx_vcache_tex_key(im->key, f));
	if (im->wide)
		mx_vcache_drop(im->clut_key);
	if (im->split)
		mx_tex_mem_free(&im->m, heap_pin, heap_asset);
	im->used = 0;
}

void rp_images_free(void) {
	for (u32 i = 0; i < nkept; i++)
		kept[i]->kept = 0;
	nkept = 0;
	for (u32 i = 0; i < image_hi; i++) {
		if (!images[i].used)
			continue;
		luma_free(&images[i]);
		image_pin(&images[i], 0);
		if (images[i].split)
			mx_tex_mem_free(&images[i].m, heap_pin, heap_asset);
	}
	image_hi = 0;
	mx_vcache_reset();
}

/* the locks (file 1: a mapped file's data, each part of it in one page of
   memory locked until the frame is done; 2: one pinned already, image_pin);
   1: its parts are REFs a page, 0: one REF, -1: a part not in memory (asked
   in, nothing locked) */
static int xfer_lock(const u8 *d, u32 bytes, int file) {
	u32 qwc = (bytes + 15) / 16;
	if (!file || !mx_is_vm(d))
		return 0;
	if (file == XFER_PINNED)
		return 1;
	u32 t0 = 0;
	int away = !mx_mem_resident(d, qwc * 16);
	if (away)
		t0 = mx_count();
	for (u32 at = 0; at < qwc * 16;) {
		u32 n = MX_VM_PAGE - (u32)((uintptr_t)(d + at) & (MX_VM_PAGE - 1));
		n = n > qwc * 16 - at ? qwc * 16 - at : n;
		if (mx_mem_lock_try(d + at, n) < 0) { /* not in memory: asked in, not waited for */
			for (u32 un = 0; un < at;) {
				u32 k = MX_VM_PAGE - (u32)((uintptr_t)(d + un) & (MX_VM_PAGE - 1));
				k = k > at - un ? at - un : k;
				mx_vm_unlock(d + un, k);
				un += k;
			}
			return -1;
		}
		mx_frame_unlock(d + at, n);
		at += n;
	}
	if (away)
		rp_tex_stats.file_waits++, rp_tex_stats.file_wait_kb += qwc / 64,
		    rp_tex_stats.file_wait_us += (mx_count() - t0) / (MX_EE_HZ / 1000000);
	return 1;
}

/* the quadwords of xfer_e's tags */
static u32 xfer_qw(u32 bytes, int pages) {
	u32 qwc = (bytes + 15) / 16;
	return 6 + 3 * (qwc / 0x7FFF + 1) + (pages ? 2 * (qwc * 16 / MX_VM_PAGE + 2) : 0);
}

/* The transfer's GIF packets go by DIRECTHL: an IMAGE transfer over PATH3
   (a later frame's once record, mx_hires_once) in intermittent mode lets
   DIRECT in between its slices (EE User's Manual 7.1.3), and this one's
   BITBLTBUF .. TRXDIR would come in the middle of it (the GS then waits for
   data that went elsewhere: a hang); DIRECTHL waits for its end. */
static u64 *xfer_e(u64 *c, u32 dbp, u32 dbw, u32 psm, u32 w, u32 h, u32 bytes, const u8 *d, int pages) {
	u32 qwc = (bytes + 15) / 16;
	c = mx_e_cnt(c, 5, mx_vif_nop(), mx_vif_directhl(5));
	c = mx_e_ad_tag(c, 4, 0);
	c = mx_e_ad(c, MX_GS_REG_BITBLTBUF, mx_gs_bitbltbuf(0, 0, 0, dbp, dbw, psm));
	c = mx_e_ad(c, MX_GS_REG_TRXPOS, mx_gs_trxpos(0, 0, 0, 0, 0));
	c = mx_e_ad(c, MX_GS_REG_TRXREG, mx_gs_trxreg(w, h));
	c = mx_e_ad(c, MX_GS_REG_TRXDIR, 0);
	while (qwc) {
		u32 n = qwc > 0x7FFF ? 0x7FFF : qwc;
		c = mx_e_cnt(c, 1, mx_vif_nop(), mx_vif_directhl(1));
		c = mx_e_giftag(c, n, n == qwc, -1, MX_GIF_FLG_IMAGE, 0, 0);
		/* the data: in one piece, or a REF a page */
		for (u32 left = n; left;) {
			u32 k = left;
			if (pages) {
				u32 room = (MX_VM_PAGE - (u32)((uintptr_t)d & (MX_VM_PAGE - 1))) / 16;
				k = room < left ? room : left;
			}
			c = mx_e_ref(c, d, k, mx_vif_nop(), mx_vif_directhl(k));
			d += k * 16, left -= k;
		}
		qwc -= n;
	}
	return c;
}

/* an IMAGE transfer into GS memory, its data read where it lies; works in
   the frame's stream (VIF1) and in the once record (the GIF alone: it does
   not read a tag's VIF codes). file: the data is a mapped file's (xfer_lock).
   0, or -1 when a part is not in memory (nothing is sent). */
static int image_xfer(u32 dbp, u32 dbw, u32 psm, u32 w, u32 h, u32 bytes, const void *data, int file) {
	int pages = xfer_lock(data, bytes, file);
	if (pages < 0)
		return -1;
	u64 *c = mx_frame_reserve(xfer_qw(bytes, pages));
	c = xfer_e(c, dbp, dbw, psm, w, h, bytes, data, pages);
	mx_frame_commit(c);
	rp_tex_stats.upload_kb += (bytes + 1023) / 1024;
	return 0;
}

static void texflush(void) {
	u64 *c = mx_frame_reserve(3);
	c = mx_e_cnt(c, 2, 0, mx_vif_direct(2));
	c = mx_e_ad_tag(c, 1, 1);
	c = mx_e_ad(c, MX_GS_REG_TEXFLUSH, 0);
	mx_frame_commit(c);
}

u32 rp_image_fine_units(const struct rp_image *im) {
	u32 bytes = im->psm == MX_GS_PSM_T4 ? (u32)im->w * im->h / 2 : (u32)im->w * im->h;
	if (!fine_cfg || im->split || im->levels < 3 || (im->w > im->h ? im->w : im->h) != 256 || bytes > 2u * FINE_UNIT * 8192)
		return 0;
	return bytes > FINE_UNIT * 8192 ? 2 : 1;
}

static void fine_drop(struct rp_image *o) {
	for (u32 k = 0; k < fine_n; k++)
		if (fine_unit[k] == o)
			fine_unit[k] = 0;
	o->fine = 0;
}

int rp_image_fine(struct rp_image *im) {
	u32 frame = mx_frame_number(), n = rp_image_fine_units(im);
	if (!n)
		return 0;
	u32 fbytes = im->psm == MX_GS_PSM_T4 ? ((u32)im->w * im->h + 1) / 2 : (u32)im->w * im->h;
	if (!im->fine) {
		if (frame_uploads >= FRAME_UPLOADS)
			return 0;
		/* its texels are read from the file by the pager, ahead: not waited for */
		if (!mx_mem_resident(im->level[0], fbytes)) {
			mx_mem_prefetch(im->level[0], fbytes);
			rp_tex_stats.fine_wait++;
			return 0;
		}
		/* the units free, or of the image drawn with longest ago; not of one a
		   frame in flight draws with, or this frame */
		u32 done = mx_frame_done(), best_age = ~0u;
		int best = -1;
		for (u32 u = 0; u + n <= fine_n; u += n) {
			u32 age = 0;
			int ok = 1;
			for (u32 k = u; k < u + n && ok; k++) {
				const struct rp_image *o = fine_unit[k];
				if (!o)
					continue;
				if (o->fine_frame > done || o->fine_frame == frame + 1)
					ok = 0;
				age = o->fine_frame > age ? o->fine_frame : age;
			}
			if (ok && age < best_age)
				best = (int)u, best_age = age;
		}
		if (best < 0) {
			rp_tex_stats.fine_wait++;
			return 0;
		}
		for (u32 k = (u32)best; k < (u32)best + n; k++)
			if (fine_unit[k])
				fine_drop(fine_unit[k]);
		for (u32 k = (u32)best; k < (u32)best + n; k++)
			fine_unit[k] = im;
		im->fine = (u8)(best + 1);
		mx_hires_once(1);
		int bad = image_xfer((fine_page + (u32)best * FINE_UNIT) * 32, mx_tex_tbw(im->w, im->psm), im->psm, im->w, im->h,
		                     im->psm == MX_GS_PSM_T4 ? ((u32)im->w * im->h + 1) / 2 : (u32)im->w * im->h, im->level[0], 1);
		texflush();
		mx_hires_once(0);
		if (bad) {
			fine_drop(im);
			return 0;
		}
		rp_tex_stats.uploads++, rp_tex_stats.fine_sent++, frame_uploads++;
	}
	im->fine_frame = frame + 1;
	return 1;
}

/* the image's first level from the pool before the cache's levels (from its
   second) in t */
static void fine_join(struct rp_image *im) {
	struct mx_tex c = im->t;
	if (c.levels + 1 > MX_TEX_LEVELS)
		return;
	for (u32 l = c.levels; l > 0; l--)
		im->t.tbp[l] = c.tbp[l - 1], im->t.tbw[l] = c.tbw[l - 1];
	im->t.tbp[0] = (fine_page + (u32)(im->fine - 1) * FINE_UNIT) * 32;
	im->t.tbw[0] = (u8)mx_tex_tbw(im->w, im->psm);
	im->t.tw = (u8)log2u(im->w), im->t.th = (u8)log2u(im->h), im->t.levels = (u8)(c.levels + 1);
	im->from = 0;
	rp_tex_stats.fine++;
}

/* ---- the stream area (rp_tex.h) ------------------------------------------------------ */

#define STREAM_INST 256      /* images streamed in a frame */
#define STREAM_SEG 1024      /* their pieces */
#define STREAM_PAGES_MAX 128
#define STREAM_RUNS 4096     /* runs of a frame (rp_list.c's MAX_RUN or more) */
#define STREAM_MARK 16       /* a block of an instance: its number + 1 from this bit on */
static struct stream_inst {
	struct rp_image *im;
	struct mx_tex t;         /* its levels' blocks from the first of its pages */
	u32 from, pages, bytes, cb, clut_bytes;
} sinst[STREAM_INST];
static struct stream_seg {
	u16 inst, page;
	const void *chain;       /* its sending (rp_dyn), made when a strip first needs it */
} sseg[STREAM_SEG];
static u32 nsseg, s_kb_cfg = 1024, s_kb_budget = 256;
/* GS time to send a KB in the passes, us (about 1 GB/s, measured) */
#define STREAM_US_KB 1.0f

/* what the stream area adds to GS memory for a frame's images: its KB (256-byte blocks) */
static u32 stream_cap_blocks(void) { return s_pages ? s_kb_budget * 4 : 0; }

void rp_tex_stream_room(float us, int late) {
	static u32 was_sent, hold;
	/* the frame before's: what it streamed, and how many times that was sent
	   (its strips) */
	u32 kb = s_kb, sent = rp_tex_stats.stream_sent_kb - was_sent;
	was_sent = rp_tex_stats.stream_sent_kb;
	float times = kb ? (float)sent / (float)kb : 1.5f;
	times = times < 1.0f ? 1.0f : times;
	/* that, and a quarter of what is to spare (less all of what is over);
	   after a pass the beam overtook, the least for half a second */
	float b = (float)kb + (us > 0 ? 0.25f * us : us) / (STREAM_US_KB * times);
	if (late)
		hold = 30;
	if (hold)
		hold--, b = 0;
	b = b < 64.0f ? 64.0f : b;
	s_kb_budget = b > (float)s_kb_cfg ? s_kb_cfg : (u32)b;
}
static u16 s_cont[STREAM_PAGES_MAX]; /* the piece + 1 each page holds in the strip being written; 0: not known */
static u8 s_drawn[STREAM_PAGES_MAX];  /* a run of the strip being written drew from the page */

void rp_tex_stream(u32 pages, u32 kb) {
	pages = pages > STREAM_PAGES_MAX ? STREAM_PAGES_MAX : pages;
	s_kb_cfg = kb;
	if (pages == s_pages)
		return;
	/* the cache gives its pages back (every image is sent again), the area
	   takes its own */
	mx_vcache_init(0, mx_hires_passes() ? MX_VCACHE_ASYNC : MX_VCACHE_STREAM, MX_VCACHE_AUTO);
	if (s_pages)
		mx_vram_give(s_page0, s_pages), s_pages = 0;
	if (pages) {
		u32 p = mx_vram_find(pages);
		if (p != ~0u && mx_vram_take(p, pages, MX_VRAM_APP) == 0)
			s_page0 = p, s_pages = pages;
	}
}

u32 rp_tex_stream_pages(void) { return s_pages; }
u32 rp_tex_stream_budget(void) { return s_pages ? s_kb_budget : 0; }

/* im from level from into the stream area this frame (an instance), or 0:
   no area, not an image for it, over the frame's KB, or a level not in
   memory */
static const struct mx_tex *stream_place(struct rp_image *im, u32 from) {
	if (!s_pages || im->split || im->kind == RP_IMAGE_LIGHTMAP || im->kind == RP_IMAGE_SCREEN || nsinst == STREAM_INST)
		return 0;
	struct stream_inst *si = &sinst[nsinst];
	u32 tw = log2u(im->w) - from, th = log2u(im->h) - from, levels = (u32)im->levels - from;
	u32 blocks = mx_tex_init(&si->t, 0, im->psm, tw, th, levels, MX_TEX_PACKED);
	u32 cbytes = im->clut ? clut_bytes(im) : 0, cb = 0;
	if (cbytes)
		cb = (blocks + 3) & ~3u, blocks = cb + 4;
	u32 pages = (blocks + 31) / 32, bytes = cbytes;
	for (u32 l = 0; l < levels; l++)
		bytes += level_bytes(im, l + from);
	if (pages > s_pages || s_kb + bytes / 1024 > s_kb_budget) {
		rp_tex_stats.stream_over++;
		return 0;
	}
	/* in memory until the frame is done (its chains are made at the list's end) */
	for (u32 l = 0; l < levels; l++)
		if (xfer_lock(im->level[l + from], level_bytes(im, l + from), xfer_mode(im, (int)(l + from))) < 0)
			return 0;
	if (cbytes && xfer_lock(im->clut, cbytes, xfer_mode(im, -1)) < 0)
		return 0;
	u32 id = nsinst++;
	si->im = im, si->from = from, si->pages = pages, si->bytes = bytes, si->cb = cb, si->clut_bytes = cbytes;
	s_kb += bytes / 1024;
	struct mx_tex *t = &im->t;
	*t = si->t;
	for (u32 l = 0; l < levels; l++)
		t->tbp[l] |= (id + 1) << STREAM_MARK;
	if (cbytes)
		t->cbp = cb | (id + 1) << STREAM_MARK, t->cpsm = MX_GS_PSM_CT32, t->csm = 0;
	im->from = (u8)from, im->ok = 1;
	im->stream = (u16)(id + 1), im->stream_frame = mx_frame_number() + 1;
	rp_tex_stats.stream++, rp_tex_stats.stream_kb += bytes / 1024;
	return t;
}

int rp_tex_stream_id(const struct mx_tex *t) {
	u32 m = t->tbp[0] >> STREAM_MARK;
	return m && m <= nsinst ? (int)m - 1 : -1;
}

/* The frame's runs in order (id: each run's instance, -1 none) into pieces
   of the area (seg: each run's, -1 none): an instance keeps its pages from
   the run that first needs it on; when another needs pages, the run of them
   whose pieces are drawn with again the latest is taken, in the order the
   passes draw them: the runs of this strip, then those of the next strip
   from the first run on (Belady's choice: free pages first; then those of
   instances done for the strip, the one the next strip needs last first;
   then the others, the one needed again last first). An instance put out so
   gets a piece of its own when it is drawn with again. Every strip draws its
   runs in this order, so a piece's pages are its own from its first run to
   its last in each, and the next strip finds a piece still there when no
   later one took its pages. */
void rp_tex_stream_plan(const s16 *id, s16 *seg, u32 n) {
	static u16 nxt[STREAM_RUNS], next_use[STREAM_INST], first_use[STREAM_INST];
	static s16 last[STREAM_INST], cur[STREAM_INST];
	static u16 occ[STREAM_PAGES_MAX];
	nsseg = 0;
	for (u32 r = 0; r < n; r++)
		seg[r] = -1;
	if (!nsinst)
		return;
	n = n > STREAM_RUNS ? STREAM_RUNS : n;
	for (u32 i = 0; i < nsinst; i++)
		last[i] = -1, cur[i] = -1, next_use[i] = (u16)n;
	for (u32 r = n; r-- > 0;) {
		if (id[r] < 0)
			continue;
		nxt[r] = last[id[r]] < 0 ? (u16)n : (u16)last[id[r]];
		last[id[r]] = (s16)r;
	}
	for (u32 i = 0; i < nsinst; i++)
		first_use[i] = last[i] < 0 ? (u16)n : (u16)last[i];
	for (u32 p = 0; p < s_pages; p++)
		occ[p] = 0;
	for (u32 r = 0; r < n; r++) {
		seg[r] = -1;
		int x = id[r];
		if (x < 0)
			continue;
		next_use[x] = nxt[r];
		if (cur[x] >= 0) {
			seg[r] = cur[x];
			continue;
		}
		if (nsseg == STREAM_SEG)
			continue;
		/* each page's cost: when its piece is drawn with next, counted on
		   into the next strip (2n + 1: free, or a piece put out already) */
		u32 need = sinst[x].pages, best = 0, best_cost = 0, free = 2 * n + 1;
		for (u32 w = 0; w + need <= s_pages; w++) {
			u32 cost = free;
			for (u32 p = w; p < w + need; p++) {
				u32 c = free;
				if (occ[p]) {
					const struct stream_seg *o = &sseg[occ[p] - 1];
					if (cur[o->inst] == (s16)(occ[p] - 1))
						c = next_use[o->inst] < n ? next_use[o->inst] : n + first_use[o->inst];
				}
				cost = c < cost ? c : cost;
			}
			if (cost > best_cost)
				best = w, best_cost = cost;
			if (cost == free)
				break;
		}
		for (u32 p = best; p < best + need; p++)
			if (occ[p] && cur[sseg[occ[p] - 1].inst] == (s16)(occ[p] - 1))
				cur[sseg[occ[p] - 1].inst] = -1;
		struct stream_seg *g = &sseg[nsseg];
		g->inst = (u16)x, g->page = (u16)best, g->chain = 0;
		for (u32 p = best; p < best + need; p++)
			occ[p] = (u16)(nsseg + 1);
		cur[x] = (s16)nsseg, seg[r] = (s16)nsseg++;
	}
	rp_tex_stats.stream_segs += nsseg;
}

void rp_tex_stream_fix(u32 seg, struct mx_tex *t, u32 *lm_cbp) {
	const struct stream_seg *g = &sseg[seg];
	const struct stream_inst *si = &sinst[g->inst];
	u32 base = (s_page0 + g->page) * 32;
	for (u32 l = 0; l < t->levels && l < si->t.levels; l++)
		t->tbp[l] = base + si->t.tbp[l];
	if (t->cbp >> STREAM_MARK)
		t->cbp = base + si->cb;
	if (lm_cbp && *lm_cbp >> STREAM_MARK)
		*lm_cbp = base + si->cb;
}

void rp_tex_stream_strip(int first) {
	for (u32 p = 0; p < s_pages; p++) {
		if (first)
			s_cont[p] = 0;
		/* a pass begins with a FLUSH (rp_list_begin): what the pass before drew is done */
		s_drawn[p] = 0;
	}
}

const void *rp_tex_stream_need(u32 seg) {
	struct stream_seg *g = &sseg[seg];
	const struct stream_inst *si = &sinst[g->inst];
	int have = 1, drawn = 0;
	for (u32 p = g->page; p < g->page + si->pages; p++)
		have &= s_cont[p] == seg + 1, drawn |= s_drawn[p];
	for (u32 p = g->page; p < g->page + si->pages; p++)
		s_drawn[p] = 1; /* the run drawn now reads it */
	if (have)
		return 0;
	if (!g->chain) {
		const struct rp_image *im = si->im;
		u32 levels = si->t.levels, qw = 1 + 3 + 1;
		for (u32 l = 0; l < levels; l++)
			qw += xfer_qw(level_bytes(im, l + si->from), mx_is_vm(im->level[l + si->from]));
		if (si->clut_bytes)
			qw += xfer_qw(si->clut_bytes, mx_is_vm(im->clut));
		u64 *c0 = rp_dyn(qw), *c = c0;
		if (!c)
			return 0;
		u32 base = (s_page0 + g->page) * 32;
		/* what was drawn before may still read these pages: VU1's last kicks */
		c = mx_e_cnt(c, 0, mx_vif_flush(), mx_vif_nop());
		for (u32 l = 0; l < levels; l++) {
			u32 w = im->w >> (l + si->from), h = im->h >> (l + si->from);
			w = w ? w : 1, h = h ? h : 1;
			const u8 *d = im->level[l + si->from];
			c = xfer_e(c, base + si->t.tbp[l], si->t.tbw[l], im->psm, w, h, level_bytes(im, l + si->from), d, mx_is_vm(d));
		}
		if (si->clut_bytes) {
			int small = si->clut_bytes == 64;
			c = xfer_e(c, base + si->cb, 1, MX_GS_PSM_CT32, small ? 8 : 16, small ? 2 : 16, si->clut_bytes, im->clut,
			           mx_is_vm(im->clut));
		}
		c = mx_e_cnt(c, 2, 0, mx_vif_directhl(2));
		c = mx_e_ad_tag(c, 1, 1);
		c = mx_e_ad(c, MX_GS_REG_TEXFLUSH, 0);
		c = mx_e_ret(c, 0, 0, 0);
		rp_dyn_end(c);
		g->chain = c0;
	}
	for (u32 p = g->page; p < g->page + si->pages; p++)
		s_cont[p] = (u16)(seg + 1);
	rp_tex_stats.stream_sent++, rp_tex_stats.stream_sent_kb += si->bytes / 1024;
	/* a FLUSH first only when a run of this strip drew from these pages (its
	   batches may still be on VU1): else past it */
	if (rp_debug & 1048576) /* r_ps2debug: always the FLUSH */
		drawn = 1;
	if (!drawn)
		rp_tex_stats.stream_noflush++;
	return drawn ? g->chain : (const u64 *)g->chain + 2;
}

int rp_tex_tight(void) { return tight; }
float rp_tex_far_bias(void) { return far_bias; }

void rp_image_want(struct rp_image *im, float rho) {
	im->want = (u8)rp_tex_level_at(rho, far_bias), im->want_lo = (u8)rp_tex_level_at(rho, far_bias - 0.5f);
	im->want_less = (u8)rp_tex_level_at(rho, far_bias - 0.75f);
	im->want_frame = mx_frame_number() + 1;
}

/* a model's image: the level its size on the screen asks for (rho: its texels
   a pixel), the finest of the frame's models that draw with it */
void rp_image_want_model(struct rp_image *im, float rho) {
	u8 w = (u8)rp_tex_level_at(rho, 0);
	if (im->want_frame == mx_frame_number() + 1 && im->want <= w)
		return;
	im->want = im->want_lo = im->want_less = w;
	im->want_frame = mx_frame_number() + 1;
}

const struct mx_tex *rp_image_place(struct rp_image *im) {
	u32 frame = mx_frame_number();
	if (!keeping) {
		im->asked = frame + 1;
		if (im->kind == RP_IMAGE_SCREEN && !im->kept && nkept < KEPT)
			im->kept = 1, kept[nkept++] = im;
	}
	/* placed this frame already; but without its finest level, and now for
	   the view weapon (rp_tex_full): once more, whole if there is room */
	int again = im->frame == frame + 1 && rp_tex_full && im->ok && im->from;
	if (im->frame == frame + 1 && !again)
		return im->ok ? &im->t : 0;
	struct mx_tex was = im->t;
	u32 was_from = im->from, was_there = im->frame == frame && im->ok;
	u32 tw = log2u(im->w), th = log2u(im->h), from = 0;
	/* its CLUT a place of its own (clut_place): a wide one (the cache gives
	   a T4 a CLUT of 16), and every one while there are CLUT slots */
	int own_clut = im->clut && (im->wide || cs_n);
	u32 cpsm = im->clut && !own_clut ? MX_GS_PSM_CT32 : MX_VCACHE_NO_CLUT;
	/* not there, and the frame has sent its share: not this frame (asked again
	   by the next draw with it) */
	if (frame_uploads >= FRAME_UPLOADS) {
		struct mx_tex t;
		if (!mx_vcache_peek(mx_vcache_tex_key(im->key, 0), &t) && !mx_vcache_peek(mx_vcache_tex_key(im->key, 1), &t) &&
		    !mx_vcache_peek(mx_vcache_tex_key(im->key, 2), &t) &&
		    !(im->split && im->m.tail && mx_vcache_peek(mx_vcache_tex_key(im->key, im->m.tail), &t))) {
			if (again)
				return &im->t;
			rp_tex_stats.later++;
			return 0;
		}
	}
	im->frame = frame + 1, im->ok = 0;
	/* a frame's images do not fit: this one without its finest level, when it
	   is a model's or an effect's */
	u32 f0 = tight && !rp_tex_full && (im->kind == RP_IMAGE_MODEL || im->kind == RP_IMAGE_OTHER) && im->levels > 2 && im->w >= 64;
	if (f0 && im->split && f0 > im->m.tail)
		f0 = 0;
	/* an image of the file larger than 128 texels a side: the cache has it
	   from its level of 128, as when the file had no more; its first level is
	   the pool's, in the frames it has a unit there (rp_image_fine) */
	u32 base = 0;
	while (!im->split && base + 2 < (u32)im->levels && (im->w > im->h ? im->w : im->h) >> base > tex_max)
		base++;
	/* but not a model's that is big on the screen (rp_image_want_model): its
	   finest level is seen there (a player up close, the view weapon's) */
	if (f0 && im->kind != RP_IMAGE_WORLD && im->want_frame == frame + 1 && im->want <= base)
		f0 = 0;
	int fine = base == 1 && im->fine && im->fine_frame == frame + 1 && !f0;
	f0 = f0 + base;
	/* tight: a texture of the world from the level its distance needs */
	if (tight && !fine && !rp_tex_full && im->kind == RP_IMAGE_WORLD && im->want_frame == frame + 1 && im->want > f0) {
		u32 last = im->split ? im->m.tail : im->levels > 2 ? (u32)im->levels - 2 : 0, f = im->want_lo;
		if (was_there && was_from >= im->want_lo && was_from <= im->want)
			f = was_from;
		f = f > last ? last : f, f = f > 2 ? 2 : f;
		if (f > f0)
			f0 = f, rp_tex_stats.far++;
	}
	/* a level of the file not in memory (paged out, or never read): from the
	   first coarser one that is, the pager asked for it; a later frame sends
	   the finer one when it has come */
	if (!im->split && f0 + 1 < (u32)im->levels && level_pageable(im, f0) &&
	    !mx_mem_resident(im->level[f0], level_bytes(im, f0))) {
		u32 f = f0;
		while (level_pageable(im, f) && !mx_mem_resident(im->level[f], level_bytes(im, f)))
			f++;
		mx_mem_prefetch(im->level[f0], level_bytes(im, f0));
		f0 = f, rp_tex_stats.paged++;
	}
	/* streamed in the frame before: again, without asking the cache (which
	   had no room: a lookup that finds none looks at every entry); one frame
	   in 16 it is asked */
	int r = MX_VCACHE_FULL;
	if (im->split || again || im->stream_frame != frame || ((frame ^ (u32)im->key) & 15) == 0)
		r = im->split ? mx_vcache_tex_mem(im->key, &im->m, im->psm, tw, th, cpsm, f0, 0, &im->t, &from)
		              : mx_vcache_tex(mx_vcache_tex_key(im->key, f0), im->psm, tw - f0, th - f0, im->levels - f0, cpsm,
		                              &im->t);
	if (!im->split)
		from = f0;
	if (again && r == MX_VCACHE_FULL) {
		im->t = was, im->from = (u8)was_from, im->ok = 1;
		return &im->t;
	}
	/* no room in the cache: sent in the passes, from the stream area */
	if (r == MX_VCACHE_FULL && !again) {
		const struct mx_tex *st = stream_place(im, f0);
		if (st) {
			frame_blocks += st->blocks, rp_tex_stats.kind_blocks[im->kind] += st->blocks;
			want_blocks += st->blocks << (2 * (f0 > base ? f0 - base : 0));
			im->uses++;
			return st;
		}
	}
	/* no room for all of it: without its finest level, or its two finest (a
	   quarter, a sixteenth of the memory): less sharp while the frame's images
	   are more than GS memory holds, in place of not drawn */
	for (u32 need = f0 + 1; r == MX_VCACHE_FULL && im->split && need <= im->m.tail; need++) {
		r = mx_vcache_tex_mem(im->key, &im->m, im->psm, tw, th, cpsm, need, 0, &im->t, &from);
		if (r != MX_VCACHE_FULL)
			rp_tex_stats.coarse++;
	}
	for (u32 f = f0 + 1; r == MX_VCACHE_FULL && !im->split && f <= 2 && f + 1 < (u32)im->levels; f++) {
		r = mx_vcache_tex(mx_vcache_tex_key(im->key, f), im->psm, tw - f, th - f, im->levels - f, cpsm, &im->t);
		if (r != MX_VCACHE_FULL)
			from = f, rp_tex_stats.coarse++;
	}
	if (r == MX_VCACHE_FULL) {
		rp_tex_stats.full++;
		return 0;
	}
	int rc = MX_VCACHE_HIT, small = im->psm == MX_GS_PSM_T4 && !im->wide;
	u32 clut_blocks = 0;
	if (own_clut) {
		u32 cbp = 0, slots = rp_tex_stats.clut_slot;
		rc = clut_place(im->clut_key, small, &cbp);
		if (rc == MX_VCACHE_FULL) {
			rp_tex_stats.full++;
			return 0;
		}
		clut_blocks = rp_tex_stats.clut_slot != slots ? 0 : small ? 1 : 4;
		im->t.cbp = cbp, im->t.cpsm = MX_GS_PSM_CT32, im->t.csm = 0;
	}
	im->from = (u8)from, im->ok = 1;
	u32 blocks = im->t.blocks + clut_blocks;
	/* what it would take more with the levels a quarter less soon */
	if (tight && im->kind == RP_IMAGE_WORLD && im->want_frame == frame + 1 && from > base) {
		u32 less = im->want_less > base ? im->want_less : base;
		if (less < from)
			lower_blocks += (blocks << (2 * (from - less))) - blocks;
	}
	frame_blocks += blocks, rp_tex_stats.kind_blocks[im->kind] += blocks;
	want_blocks += blocks << (2 * (from > base ? from - base : 0));
	im->uses++;
	if (r == MX_VCACHE_HIT && rc == MX_VCACHE_HIT) {
		if (fine && from == 1)
			fine_join(im);
		return &im->t;
	}
	rp_tex_stats.uploads++, frame_uploads++;
	mx_hires_once(1);
	int bad = 0;
	for (u32 l = 0; r != MX_VCACHE_HIT && l < (u32)im->levels - from; l++) {
		u32 w = im->w >> (l + from), h = im->h >> (l + from);
		w = w ? w : 1, h = h ? h : 1;
		bad |= image_xfer(im->t.tbp[l], im->t.tbw[l], im->psm, w, h, im->psm == MX_GS_PSM_T4 ? (w * h + 1) / 2 : w * h,
		                  im->level[l + from], xfer_mode(im, (int)(l + from)));
	}
	if (im->clut && (own_clut ? rc : r) != MX_VCACHE_HIT)
		bad |= image_xfer(im->t.cbp, 1, MX_GS_PSM_CT32, small ? 8 : 16, small ? 2 : 16, small ? 64 : 1024, im->clut,
		                  xfer_mode(im, -1));
	texflush();
	mx_hires_once(0);
	if (bad) {
		/* a part of it is not in GS memory: its place is given up, the draw goes without */
		mx_vcache_drop(mx_vcache_tex_key(im->key, from));
		if (own_clut)
			mx_vcache_drop(im->clut_key), clut_slot_drop(im->clut_key);
		im->ok = 0;
		rp_tex_stats.full++;
		return 0;
	}
	if (fine && from == 1)
		fine_join(im);
	return &im->t;
}

u32 rp_clut_place(u64 key, const void *clut, int small) {
	u32 bp = 0, w = small ? 8 : 16, h = small ? 2 : 16;
	int r = clut_place(key, small, &bp);
	if (r == MX_VCACHE_FULL)
		return 0;
	if (r == MX_VCACHE_MISS) {
		mx_hires_once(1);
		image_xfer(bp, 1, MX_GS_PSM_CT32, w, h, w * h * 4, clut, 0);
		texflush();
		mx_hires_once(0);
	}
	return bp;
}
