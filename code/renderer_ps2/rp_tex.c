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
#include "rp_tex.h"

#define MAX_IMAGE 2048
static struct rp_image images[MAX_IMAGE];
static u32 image_hi;
static u64 serial;
static struct mx_heap *heap_pin, *heap_asset;
struct rp_tex_stats rp_tex_stats;

#define FINE_UNIT 4 /* pages */
#define FINE_MAX 32
static struct rp_image *fine_unit[FINE_MAX];
static u32 fine_page, fine_n, fine_cfg = 16;

void rp_tex_fine_units(u32 units) { fine_cfg = units > FINE_MAX ? FINE_MAX : units & ~1u; }
u32 rp_tex_fine_pool(void) { return fine_n; }

void rp_tex_init(void) {
	/* the pool's pages first: the cache takes the rest */
	fine_n = 0;
	memset(fine_unit, 0, sizeof fine_unit);
	if (fine_cfg) {
		u32 p = mx_vram_find(fine_cfg * FINE_UNIT);
		if (p != ~0u && mx_vram_take(p, fine_cfg * FINE_UNIT, MX_VRAM_APP) == 0)
			fine_page = p, fine_n = fine_cfg;
	}
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
static u32 want_blocks, tight, calm;
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

void rp_tex_frame(void) {
	mx_vcache_frame(mx_frame_number(), mx_frame_done());
	frame_uploads = 0;
	u32 kb = frame_blocks / 4;
	rp_tex_stats.frames++, rp_tex_stats.used_kb += kb;
	if (kb > rp_tex_stats.used_max_kb)
		rp_tex_stats.used_max_kb = kb;
	frame_blocks = 0;
	{
		struct mx_vcache_stats vs;
		mx_vcache_stats(&vs);
		u32 cap = vs.pages * 32;
		/* tight: the frame's images at full size are 90 % of what there is;
		   until the frames have been under 80 % for a second */
		if (want_blocks * 100 > cap * 90)
			tight = 1, calm = 0;
		else if (want_blocks * 100 > cap * 80)
			calm = 0;
		else if (tight && ++calm > 60)
			tight = 0;
		if (want_blocks > rp_tex_stats.want_max)
			rp_tex_stats.want_max = want_blocks;
		rp_tex_stats.want_sum += want_blocks / 4;
		rp_tex_stats.tight += tight;
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

struct rp_image *rp_image_static(const char *name, u32 psm, u32 w, u32 h, u32 levels, int alpha,
                                 const u8 *const *level, const void *clut) {
	struct rp_image *im = image_new(name, psm, w, h, levels, alpha);
	if (!im)
		return 0;
	for (u32 l = 0; l < im->levels; l++)
		im->level[l] = level[l];
	im->clut = clut;
	return im;
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
	im = rp_image_static(name, e->psm, e->w, e->h, levels, e->alpha, level, rp_bin_tex.base + e->off_clut);
	if (!im)
		return 0;
	im->pic_w = e->pic_w, im->pic_h = e->pic_h;
	im->wide = e->psm == MX_GS_PSM_T4 && !e->alpha;
	if (bytes)
		*bytes += e->size;
	/* into memory now, but a first level of 256 texels: that one is read when
	   the image is first given a place in the pool of fine levels
	   (rp_image_fine), by the pager, ahead: it is three quarters of the
	   image's bytes, and most images never come that near. (Letting it go
	   again once it is in GS memory was worse: read again from the file at
	   6 ms a page every time it comes back.) */
	if (levels > 2 && (e->w > e->h ? e->w : e->h) == 256) {
		const u8 *d = rp_bin_tex.base + e->off;
		rp_bin_touch(d, (u32)(level[0] - d));
		rp_bin_touch(level[1], e->size - (u32)(level[1] - d));
	} else
		rp_bin_touch(rp_bin_tex.base + e->off, e->size);
	return im;
}

struct rp_image *rp_image_alloc(const char *name, u32 psm, u32 w, u32 h, u32 levels, int alpha) {
	struct rp_image *im = image_new(name, psm, w, h, levels, alpha);
	if (!im)
		return 0;
	u32 clut_bytes = psm == MX_GS_PSM_T4 ? 64 : 1024;
	/* the first levels pageable when there are three or more */
	if (!heap_pin || mx_tex_mem_alloc(&im->m, psm, log2u(w), log2u(h), im->levels, clut_bytes, heap_pin,
	                                  im->levels >= 3 ? heap_asset : 0) < 0) {
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
		if (images[i].split)
			mx_tex_mem_free(&images[i].m, heap_pin, heap_asset);
	}
	image_hi = 0;
	mx_vcache_reset();
}

/* an IMAGE transfer into GS memory, its data read where it lies; works in
   the frame's stream (VIF1) and in the once record (the GIF alone: it does
   not read a tag's VIF codes). file: the data is a mapped file's: each part
   of it in one page of memory is locked there until the frame is done (a
   part a REF: pages need not lie one after the other). 0, or -1 when a part
   could not be locked (nothing is sent). */
static int image_xfer(u32 dbp, u32 dbw, u32 psm, u32 w, u32 h, u32 bytes, const void *data, int file) {
	u32 qwc = (bytes + 15) / 16;
	const u8 *d = data;
	if (file && mx_is_vm(d)) {
		for (u32 at = 0; at < qwc * 16;) {
			u32 n = MX_VM_PAGE - (u32)((uintptr_t)(d + at) & (MX_VM_PAGE - 1));
			n = n > qwc * 16 - at ? qwc * 16 - at : n;
			if (mx_vm_lock(d + at, n) < 0) {
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
	} else
		file = 0;
	u64 *c = mx_frame_reserve(6 + 3 * (qwc / 0x7FFF + 1) + (file ? 2 * (qwc * 16 / MX_VM_PAGE + 2) : 0));
	c = mx_e_cnt(c, 5, mx_vif_nop(), mx_vif_direct(5));
	c = mx_e_ad_tag(c, 4, 0);
	c = mx_e_ad(c, MX_GS_REG_BITBLTBUF, mx_gs_bitbltbuf(0, 0, 0, dbp, dbw, psm));
	c = mx_e_ad(c, MX_GS_REG_TRXPOS, mx_gs_trxpos(0, 0, 0, 0, 0));
	c = mx_e_ad(c, MX_GS_REG_TRXREG, mx_gs_trxreg(w, h));
	c = mx_e_ad(c, MX_GS_REG_TRXDIR, 0);
	while (qwc) {
		u32 n = qwc > 0x7FFF ? 0x7FFF : qwc;
		c = mx_e_cnt(c, 1, mx_vif_nop(), mx_vif_direct(1));
		c = mx_e_giftag(c, n, n == qwc, -1, MX_GIF_FLG_IMAGE, 0, 0);
		/* the data: in one piece, or a REF a page */
		for (u32 left = n; left;) {
			u32 k = left;
			if (file) {
				u32 room = (MX_VM_PAGE - (u32)((uintptr_t)d & (MX_VM_PAGE - 1))) / 16;
				k = room < left ? room : left;
			}
			c = mx_e_ref(c, d, k, mx_vif_nop(), mx_vif_direct(k));
			d += k * 16, left -= k;
		}
		qwc -= n;
	}
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
	if (!fine_n || im->split || im->levels < 3 || (im->w > im->h ? im->w : im->h) != 256 || bytes > 2u * FINE_UNIT * 8192)
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
	u32 was_from = im->from;
	u32 tw = log2u(im->w), th = log2u(im->h), from = 0;
	u32 cpsm = im->clut && !im->wide ? MX_GS_PSM_CT32 : MX_VCACHE_NO_CLUT;
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
	while (!im->split && base + 2 < (u32)im->levels && (im->w > im->h ? im->w : im->h) >> base > 128)
		base++;
	int fine = base == 1 && im->fine && im->fine_frame == frame + 1 && !f0;
	f0 = f0 + base;
	int r = im->split ? mx_vcache_tex_mem(im->key, &im->m, im->psm, tw, th, cpsm, f0, 0, &im->t, &from)
	                  : mx_vcache_tex(mx_vcache_tex_key(im->key, f0), im->psm, tw - f0, th - f0, im->levels - f0, cpsm, &im->t);
	if (!im->split)
		from = f0;
	if (again && r == MX_VCACHE_FULL) {
		im->t = was, im->from = (u8)was_from, im->ok = 1;
		return &im->t;
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
	/* a wide CLUT: a place of its own (the cache gives a T4 a CLUT of 16) */
	int rc = MX_VCACHE_HIT;
	if (im->wide) {
		u32 cbp = 0;
		rc = mx_vcache_buf(im->clut_key, 16, 16, MX_GS_PSM_CT32, &cbp);
		if (rc == MX_VCACHE_FULL) {
			rp_tex_stats.full++;
			return 0;
		}
		im->t.cbp = cbp, im->t.cpsm = MX_GS_PSM_CT32, im->t.csm = 0;
	}
	im->from = (u8)from, im->ok = 1;
	u32 blocks = im->t.blocks + (im->wide ? 4 : 0);
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
		                  im->level[l + from], !im->split);
	}
	int small = im->psm == MX_GS_PSM_T4 && !im->wide;
	if (im->clut && (im->wide ? rc : r) != MX_VCACHE_HIT)
		bad |= image_xfer(im->t.cbp, 1, MX_GS_PSM_CT32, small ? 8 : 16, small ? 2 : 16, small ? 64 : 1024, im->clut,
		                  !im->split);
	texflush();
	mx_hires_once(0);
	if (bad) {
		/* a part of it is not in GS memory: its place is given up, the draw goes without */
		mx_vcache_drop(mx_vcache_tex_key(im->key, from));
		if (im->wide)
			mx_vcache_drop(im->clut_key);
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
	int r = mx_vcache_buf(key, w, h, MX_GS_PSM_CT32, &bp);
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
