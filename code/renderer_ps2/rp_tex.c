/* rp_tex.c: the PS2 renderer's images. An image is a paletted texture (T8 or
   T4, its CLUT of 32-bit colours) with its MIPMAP levels, in memory: where a
   locked file has it (a map's, tools/ps2map.py), or in the renderer's heaps,
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

#include "rp_tex.h"

#define MAX_IMAGE 2048
static struct rp_image images[MAX_IMAGE];
static u32 image_hi;
static u64 serial;
static struct mx_heap *heap_pin, *heap_asset;
struct rp_tex_stats rp_tex_stats;

void rp_tex_init(void) {
	mx_vcache_init(0, mx_hires_passes() ? MX_VCACHE_ASYNC : MX_VCACHE_STREAM, MX_VCACHE_AUTO);
	if (!heap_pin) {
		heap_pin = mx_heap_create("q3.tex", MX_MEM_PINNED, 0);
		heap_asset = mx_heap_create("q3.texels", MX_MEM_ASSET, 16u << 20);
	}
}

void rp_tex_frame(void) { mx_vcache_frame(mx_frame_number(), mx_frame_done()); }

u64 rp_tex_key(void) { return ++serial; }

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
	im->key = rp_tex_key();
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
			u32 r = src[i] & 0xFF, g = src[i] >> 8 & 0xFF, b = src[i] >> 16 & 0xFF;
			u32 m = r > g ? r : g;
			m = ((b > m ? b : m) + 1) >> (twice ? 0 : 1);
			c[i] = (src[i] & 0xFFFFFF) | (m > 0x80 ? 0x80 : m) << 24;
		}
		mx_dcache_writeback(c, n * 4);
		im->luma[twice] = c, im->luma_key[twice] = rp_tex_key();
	}
	return rp_clut_place(im->luma_key[twice], im->luma[twice], n == 16);
}

static void luma_free(struct rp_image *im) {
	for (int k = 0; k < 2; k++)
		if (im->luma[k])
			mx_heap_free(heap_pin, im->luma[k]), im->luma[k] = 0;
}

void rp_image_free(struct rp_image *im) {
	if (!im || !im->used)
		return;
	luma_free(im);
	for (u32 f = 0; f < im->levels; f++)
		mx_vcache_drop(mx_vcache_tex_key(im->key, f));
	if (im->split)
		mx_tex_mem_free(&im->m, heap_pin, heap_asset);
	im->used = 0;
}

void rp_images_free(void) {
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
   not read a tag's VIF codes) */
static void image_xfer(u32 dbp, u32 dbw, u32 psm, u32 w, u32 h, u32 bytes, const void *data) {
	u32 qwc = (bytes + 15) / 16;
	u64 *c = mx_frame_reserve(6 + 3 * (qwc / 0x7FFF + 1));
	const u8 *d = data;
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
		c = mx_e_ref(c, d, n, mx_vif_nop(), mx_vif_direct(n));
		d += n * 16;
		qwc -= n;
	}
	mx_frame_commit(c);
	rp_tex_stats.upload_kb += (bytes + 1023) / 1024;
}

static void texflush(void) {
	u64 *c = mx_frame_reserve(3);
	c = mx_e_cnt(c, 2, 0, mx_vif_direct(2));
	c = mx_e_ad_tag(c, 1, 1);
	c = mx_e_ad(c, MX_GS_REG_TEXFLUSH, 0);
	mx_frame_commit(c);
}

const struct mx_tex *rp_image_place(struct rp_image *im) {
	u32 frame = mx_frame_number();
	if (im->frame == frame + 1) {
		if (im->ok)
			mx_vcache_use(mx_vcache_tex_key(im->key, im->from));
		return im->ok ? &im->t : 0;
	}
	im->frame = frame + 1, im->ok = 0;
	u32 tw = log2u(im->w), th = log2u(im->h), from = 0;
	u32 cpsm = im->clut ? MX_GS_PSM_CT32 : MX_VCACHE_NO_CLUT;
	int r = im->split ? mx_vcache_tex_mem(im->key, &im->m, im->psm, tw, th, cpsm, 0, 0, &im->t, &from)
	                  : mx_vcache_tex(im->key, im->psm, tw, th, im->levels, cpsm, &im->t);
	if (r == MX_VCACHE_FULL) {
		rp_tex_stats.full++;
		return 0;
	}
	im->from = (u8)from, im->ok = 1;
	if (r == MX_VCACHE_HIT)
		return &im->t;
	rp_tex_stats.uploads++;
	mx_hires_once(1);
	for (u32 l = 0; l < (u32)im->levels - from; l++) {
		u32 w = im->w >> (l + from), h = im->h >> (l + from);
		w = w ? w : 1, h = h ? h : 1;
		image_xfer(im->t.tbp[l], im->t.tbw[l], im->psm, w, h, im->psm == MX_GS_PSM_T4 ? (w * h + 1) / 2 : w * h,
		           im->level[l + from]);
	}
	if (im->clut)
		image_xfer(im->t.cbp, 1, MX_GS_PSM_CT32, im->psm == MX_GS_PSM_T4 ? 8 : 16, im->psm == MX_GS_PSM_T4 ? 2 : 16,
		           im->psm == MX_GS_PSM_T4 ? 64 : 1024, im->clut);
	texflush();
	mx_hires_once(0);
	return &im->t;
}

u32 rp_clut_place(u64 key, const void *clut, int small) {
	u32 bp = 0, w = small ? 8 : 16, h = small ? 2 : 16;
	int r = mx_vcache_buf(key, w, h, MX_GS_PSM_CT32, &bp);
	if (r == MX_VCACHE_FULL)
		return 0;
	if (r == MX_VCACHE_MISS) {
		mx_hires_once(1);
		image_xfer(bp, 1, MX_GS_PSM_CT32, w, h, w * h * 4, clut);
		texflush();
		mx_hires_once(0);
	}
	return bp;
}
