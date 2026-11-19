/* rp_world.c: the world of a map (PLAN-PS2.md, 5.3 and 5.4), from the .pmap
   tools/ps2map.py makes (its head has the file's layout).

   A view: the camera's PVS cluster gives the groups it can see; each one's
   bounds go against the frustum and the guard band; of a group in view the
   chunks that the cluster sees and that do not face away become draws of the frame's list (rp_list.c),
   a run a material and lightmap page, each with the strips of beam racing
   its bounds touch on the screen. No vertex is touched and nothing is
   sorted: the groups lie in the file by material.

   A group across the near plane or the guard band is drawn with the programs
   that cut such triangles (q3w2c, q3w1c, tools/ps2vu.py).

   Not yet: fog and doors' areas (what is behind a closed door is drawn). */

#include <mx/frame.h>
#include <mx/gif.h>
#include <mx/hires.h>
#include <mx/mem.h>
#include <mx/mx.h>
#include <mx/tex.h>
#include <mx/vif.h>
#include <mx/vm.h>

#include "rp_bin.h"
#include "rp_list.h"
#include "rp_tex.h"
#include "rp_world.h"
#include "vu/q3vu.h"

/* ---- the file (tools/ps2map.py) ------------------------------------------------- */

struct pm_head {
	u32 magic, version;
	u32 nmat, ntex, nchunk, nmodel, nlightmap, ncluster, vis_bytes;
	u32 nplane, nnode, nleaf;
	u32 off_mat, off_tex, off_chunk, off_model, off_lightmap, off_vis, off_plane, off_node, off_leaf, off_data, size;
	float pos_scale;
	u32 batch;
	u32 ngroup, off_group;
	u32 off_cvis, cvis_bytes;
	u32 off_lmdim;
};
struct pm_mat {
	char name[64];
	s16 tex;
	u16 flags;
	u8 blend, alpha, cull, pad;
	u32 surf, cont;
	float texels;
	u8 pad2[12];
};
struct pm_tex {
	char name[64];
	u16 w, h;
	u8 alpha, pad[3];
};
struct pm_chunk {
	float mins[3];
	u16 mat;
	s16 lightmap;
	float maxs[3];
	u16 nverts;
	u8 flags, fog;
	u32 off_data;
	u16 qwc, nbatch;
	s8 cone[3]; /* the axis of its normals, length 127 */
	u8 sine;    /* of the cone's half angle, in 1/255; 255: no cone */
	u8 area;    /* the area it lies in (255: several, or none) */
	u8 pad[3];
};
struct pm_group {
	float mins[3];
	u32 first_chunk;
	float maxs[3];
	u32 nchunk;
};
struct pm_model {
	float mins[3], maxs[3];
	u32 first_group, ngroup;
};
struct pm_node {
	s32 plane, child[2];
};
struct pm_leaf {
	s16 cluster, area;
};
#define PM_MAGIC 0x50414d50u /* "PMAP" */
enum { MAT_LIGHTMAP = 1, MAT_SKY = 2, MAT_NODRAW = 4, MAT_SCRIPT = 32 }; /* rp_world.h's RP_MAT_* */
enum { BLEND_NONE, BLEND_ADD, BLEND_ALPHA, BLEND_FILTER, BLEND_OTHER };
/* a material's alpha test: rp_list.h's RP_ALPHA_* */
enum { CH_LIGHTMAP = 1, CH_SPRITE = 2 /* no vertices: quads that turn to the eye */ };

static struct {
	const u8 *base;
	const struct pm_head *h;
	const struct pm_mat *mat;
	const struct pm_tex *tex;
	const struct pm_chunk *chunk;
	const struct pm_group *group;
	const struct pm_model *model;
	const u8 *vis, *cvis; /* a cluster's row: the groups it sees, the chunks */
	const float (*plane)[4];
	const struct pm_node *node;
	const struct pm_leaf *leaf;
} pm;

/* ---- its textures and lightmap pages ------------------------------------------------ */

#define MAX_TEX 512
#define MAX_LM 512
static struct rp_image *wtex[MAX_TEX], *wlm[MAX_LM];
/* the lightmaps' CLUT for a surface without a texture (one with a texture
   has it in its texture's CLUT, tools/ps2bin.py): entry 128 + i's alpha is
   brightness i as the blend's factor (Cd * As, 0x80 is 1.0): twice the
   lightmap's value, the map's overbright range; in CSM1's order */
static u32 lm_clut[256] MX_FAST_BSS MX_ALIGNED(64);
static u64 lm_clut_key;

static void images_free(void) {
	/* the textures are the file of images' (the shaders' too): not freed here */
	for (u32 i = 0; i < MAX_TEX; i++)
		wtex[i] = 0;
	for (u32 i = 0; i < MAX_LM; i++)
		rp_image_free(wlm[i]), wlm[i] = 0;
}

static void images_load(void) {
	const struct pm_head *h = pm.h;
	u32 bytes = 0, none = 0;
	for (u32 i = 0; i < h->ntex && i < MAX_TEX; i++) {
		wtex[i] = rp_image_bin(pm.tex[i].name, &bytes);
		none += !wtex[i];
	}
	/* the lightmap pages: 64 x 64 mostly (a view takes the ones it draws with) */
	const struct {
		u16 w, h;
		u32 off;
	} *dim = (const void *)(pm.base + h->off_lmdim);
	for (u32 i = 0; i < h->nlightmap && i < MAX_LM; i++) {
		const u8 *level = pm.base + h->off_lightmap + dim[i].off;
		wlm[i] = rp_image_static("*lightmap", MX_GS_PSM_T8, dim[i].w, dim[i].h, 1, 0, &level, 0);
	}
	for (u32 i = 0; i < 256; i++) {
		u32 a = i < 128 ? 0x80 : i == 255 ? 255 : (i - 128) * 2;
		u32 k = (i & 0x18) == 0x08 ? i + 8 : (i & 0x18) == 0x10 ? i - 8 : i;
		lm_clut[k] = a << 24 | 0x808080;
	}
	mx_dcache_writeback(lm_clut, sizeof lm_clut);
	if (!lm_clut_key)
		lm_clut_key = rp_tex_key();
	mx_printf("rp: %u textures (%u KB; %u not in the file of images) and %u lightmap pages\n", h->ntex, bytes / 1024,
	          none, h->nlightmap);
}

/* The world's groups in runs of one material and lightmap page, as they lie
   in the file: a view walks these and looks at a group only when the
   camera's cluster sees it. */
#define MAX_GRUN 4096
static struct {
	u16 first, n; /* groups */
	u8 blended, sky;
} grun[MAX_GRUN];
static u32 ngrun;

static void runs_make(void) {
	const struct pm_model *world = &pm.model[0];
	u32 g = world->first_group, end = world->first_group + world->ngroup;
	ngrun = 0;
	if (!pm.h->nmodel)
		return;
	while (g < end && ngrun < MAX_GRUN) {
		const struct pm_chunk *c0 = &pm.chunk[pm.group[g].first_chunk];
		u32 first = g;
		for (; g < end && g - first < 65535; g++) {
			const struct pm_chunk *c = &pm.chunk[pm.group[g].first_chunk];
			if (c->mat != c0->mat || c->lightmap != c0->lightmap)
				break;
		}
		grun[ngrun].first = (u16)first, grun[ngrun].n = (u16)(g - first);
		grun[ngrun].blended = pm.mat[c0->mat].blend != BLEND_NONE;
		grun[ngrun].sky = (pm.mat[c0->mat].flags & MAT_SKY) != 0;
		ngrun++;
	}
	/* a limit reached is named: what is left out is not drawn anywhere */
	if (g < end)
		mx_printf("rp: the map has more runs of groups than %d: %u groups of the world are not drawn\n", MAX_GRUN, end - g);
}

void rp_world_free(void) {
	images_free();
	if (pm.h)
		mx_vm_unlock(pm.base + pm.h->off_data, pm.h->size - pm.h->off_data);
	pm.h = 0;
}

int rp_world_load(const void *p, u32 size) {
	const struct pm_head *h = p;
	rp_world_free();
	if (size < sizeof *h || h->magic != PM_MAGIC || h->version != 10 || h->size > size || h->batch != Q3VU_BATCH)
		return -1;
	/* the chunks stay in memory: the DMAC reads them every frame. The textures
	   are locked when they are sent to GS memory (rp_tex.c); the rest only
	   the EE reads */
	if (mx_vm_lock((const u8 *)p + h->off_data, h->size - h->off_data) < 0)
		return -1;
	pm.base = p, pm.h = h;
	pm.mat = (const void *)(pm.base + h->off_mat);
	pm.tex = (const void *)(pm.base + h->off_tex);
	pm.chunk = (const void *)(pm.base + h->off_chunk);
	pm.group = (const void *)(pm.base + h->off_group);
	pm.model = (const void *)(pm.base + h->off_model);
	pm.vis = pm.base + h->off_vis;
	pm.cvis = pm.base + h->off_cvis;
	pm.plane = (const void *)(pm.base + h->off_plane);
	pm.node = (const void *)(pm.base + h->off_node);
	pm.leaf = (const void *)(pm.base + h->off_leaf);
	/* its tables and textures in memory now, not at the first frame's draws */
	rp_bin_touch(pm.base, h->off_cvis);
	images_load();
	runs_make();
	return 0;
}

/* ---- a frame ---------------------------------------------------------------------- */

static inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static u32 cam_cluster(const float *org) {
	const struct pm_head *h = pm.h;
	s32 n = 0;
	if (!h->nnode)
		return ~0u;
	while (n >= 0) {
		const struct pm_node *nd = &pm.node[n];
		const float *pl = pm.plane[nd->plane];
		n = nd->child[dot3(pl, org) - pl[3] >= 0 ? 0 : 1];
	}
	s32 leaf = -(n + 1);
	if ((u32)leaf >= h->nleaf || pm.leaf[leaf].cluster < 0)
		return ~0u;
	return (u32)pm.leaf[leaf].cluster;
}

/* a view's chunks: each with the strips of beam racing its bounds touch on
   the screen, in runs of one material and lightmap page */
struct item {
	u16 chunk, mask;
	u8 clip; /* across a plane: the programs that cut */
};
#define MAX_ITEM 4096
#define MAX_WRUN 1024
static struct item item[MAX_ITEM];
static struct {
	u16 first, n;
	u8 blended;
} wrun[MAX_WRUN];
static u32 nitem, nwrun;
static const void *wmatrix;
/* the chunks of quads that turn to the eye (lamps' glows) a view sees: their
   records and material, for the scene's quads */
#define MAX_WSPRITE 128
static struct {
	const float *rec;
	u16 n, mat;
} wsprite[MAX_WSPRITE];
static u32 nwsprite;
u32 rp_world_sprites(u32 i, const float **rec, u32 *mat) {
	if (i >= nwsprite)
		return 0;
	*rec = wsprite[i].rec, *mat = wsprite[i].mat;
	return wsprite[i].n;
}
static const u8 *areamask; /* rp_world_areas */
void rp_world_areas(const u8 *mask) { areamask = mask; }
static u32 sky_seen; /* the strips the view sees sky surfaces in */
static u32 sky_full; /* those whose part is all of the strip */
/* and where in each: the picture's pixels its sky surfaces' boxes cover */
static struct {
	float x0, x1, y0, y1;
} sky_rect[16];
static rp_world_passes_fn passes_fn;

u32 rp_world_materials(void) { return pm.h ? pm.h->nmat : 0; }
const char *rp_world_material(u32 i, u32 *flags) {
	if (!pm.h || i >= pm.h->nmat)
		return 0;
	if (flags)
		*flags = pm.mat[i].flags;
	return pm.mat[i].name;
}
void rp_world_script(rp_world_passes_fn fn) { passes_fn = fn; }
u32 rp_world_sky(void) { return sky_seen; }
int rp_world_sky_rect(u32 strip, float *r) {
	if (strip >= 16 || !(sky_seen >> strip & 1))
		return 0;
	r[0] = sky_rect[strip].x0, r[1] = sky_rect[strip].x1, r[2] = sky_rect[strip].y0, r[3] = sky_rect[strip].y1;
	return 1;
}

/* A sky surface's box (mins at b[0..2], maxs at b[4..6]) seen by the view:
   the pixels it covers go into the rectangles of the strips it is in. Its
   sides are cut to the frustum first (a box beside the eye that reaches
   behind it would cover the picture otherwise): a flat box is one side. */
static void sky_box(const struct rp_view *v, const float *b) {
	float w0 = (float)mx_gs.w, h0 = (float)mx_gs.h;
	float cxs = v->cx - 2048.0f + w0 * 0.5f, cys = v->cy - 2048.0f + h0 * 0.5f;
	float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
	int flat = -1;
	for (int a = 0; a < 3; a++)
		if (b[4 + a] - b[a] < 1.0f)
			flat = a;
	for (int f = 0; f < 6; f++) {
		int a = f >> 1, hi = f & 1;
		if (flat >= 0 && (a != flat || hi))
			continue;
		/* the side of axis a at its low or high end: four corners around */
		int a1 = (a + 1) % 3, a2 = (a + 2) % 3;
		float poly[2][12][3];
		u32 n = 4;
		for (int k = 0; k < 4; k++) {
			poly[0][k][a] = hi ? b[4 + a] : b[a];
			poly[0][k][a1] = (k == 1 || k == 2) ? b[4 + a1] : b[a1];
			poly[0][k][a2] = k >= 2 ? b[4 + a2] : b[a2];
		}
		int cur = 0;
		for (int p = 0; p < 5 && n >= 3; p++) {
			const float *pn = v->fr[p].n;
			float pd = v->fr[p].d, d[12];
			u32 m = 0;
			for (u32 i = 0; i < n; i++)
				d[i] = pn[0] * poly[cur][i][0] + pn[1] * poly[cur][i][1] + pn[2] * poly[cur][i][2] - pd;
			for (u32 i = 0; i < n && m < 11; i++) {
				u32 q = i + 1 == n ? 0 : i + 1;
				if (d[i] >= 0) {
					for (int c = 0; c < 3; c++)
						poly[cur ^ 1][m][c] = poly[cur][i][c];
					m++;
				}
				if ((d[i] < 0) != (d[q] < 0)) {
					float t = d[i] / (d[i] - d[q]);
					for (int c = 0; c < 3; c++)
						poly[cur ^ 1][m][c] = poly[cur][i][c] + t * (poly[cur][q][c] - poly[cur][i][c]);
					m++;
				}
			}
			n = m, cur ^= 1;
		}
		if (n < 3)
			continue;
		for (u32 i = 0; i < n; i++) {
			float d[3] = {poly[cur][i][0] - v->org[0], poly[cur][i][1] - v->org[1], poly[cur][i][2] - v->org[2]};
			float w = v->F[0] * d[0] + v->F[1] * d[1] + v->F[2] * d[2];
			float iw = 1.0f / (w < 1.0f ? 1.0f : w);
			float px = cxs - v->fx * (v->L[0] * d[0] + v->L[1] * d[1] + v->L[2] * d[2]) * iw;
			float py = cys - v->fy * (v->U[0] * d[0] + v->U[1] * d[1] + v->U[2] * d[2]) * iw;
			x0 = px < x0 ? px : x0, x1 = px > x1 ? px : x1, y0 = py < y0 ? py : y0, y1 = py > y1 ? py : y1;
		}
	}
	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > w0 ? w0 : x1, y1 = y1 > h0 ? h0 : y1;
	if (x1 <= x0 || y1 <= y0)
		return;
	u32 n = v->nstrip ? v->nstrip : 1;
	float sh = v->nstrip ? (float)v->strip_h : h0;
	for (u32 k = 0; k < n && k < 16; k++) {
		float t = (float)k * sh, e = t + sh;
		if (y1 <= t || y0 >= e)
			continue;
		float a = y0 > t ? y0 : t, c = y1 < e ? y1 : e;
		if (x0 <= 0 && x1 >= w0 && a <= t && c >= (e < h0 ? e : h0))
			sky_full |= 1u << k;
		if (!(sky_seen >> k & 1)) {
			sky_seen |= 1u << k;
			sky_rect[k].x0 = x0, sky_rect[k].x1 = x1, sky_rect[k].y0 = a, sky_rect[k].y1 = c;
		} else {
			sky_rect[k].x0 = x0 < sky_rect[k].x0 ? x0 : sky_rect[k].x0, sky_rect[k].x1 = x1 > sky_rect[k].x1 ? x1 : sky_rect[k].x1;
			sky_rect[k].y0 = a < sky_rect[k].y0 ? a : sky_rect[k].y0, sky_rect[k].y1 = c > sky_rect[k].y1 ? c : sky_rect[k].y1;
		}
	}
}

/* A plane against a box (mins at b[0..2], maxs at b[4..6], as a group and a
   chunk have them): which of the box's numbers make the corner farthest
   along the plane's normal, and the nearest. */
struct psel {
	float n[3], d;
	u8 far[3], near[3];
};
static inline float sel_far(const struct psel *p, const float *b) {
	return p->n[0] * b[p->far[0]] + p->n[1] * b[p->far[1]] + p->n[2] * b[p->far[2]] - p->d;
}
static inline float sel_near(const struct psel *p, const float *b) {
	return p->n[0] * b[p->near[0]] + p->n[1] * b[p->near[1]] + p->n[2] * b[p->near[2]] - p->d;
}

void rp_world_cull(struct rp_world_stats *st) {
	const struct pm_head *h = pm.h;
	const struct rp_view *v = &rp_view;
	static struct rp_world_stats none;
	st = st ? st : &none;
	*st = (struct rp_world_stats){0};
	nitem = nwrun = 0;
	wmatrix = 0;
	sky_seen = sky_full = 0;
	nwsprite = 0;
	if (!h || !h->nmodel)
		return;
	const float *org = v->org;
	struct psel fr[5], gb[5];
	for (int p = 0; p < 5; p++)
		for (int k = 0; k < 3; k++) {
			fr[p].n[k] = v->fr[p].n[k], fr[p].d = v->fr[p].d;
			fr[p].far[k] = (u8)(fr[p].n[k] >= 0 ? 4 + k : k), fr[p].near[k] = (u8)(fr[p].n[k] >= 0 ? k : 4 + k);
			gb[p].n[k] = v->gb[p].n[k], gb[p].d = v->gb[p].d;
			gb[p].near[k] = (u8)(gb[p].n[k] >= 0 ? k : 4 + k);
		}
	u32 cluster = cam_cluster(org);
	st->cluster = cluster;
	const u8 *row = cluster < h->ncluster ? pm.vis + cluster * h->vis_bytes : 0;
	const u8 *crow = cluster < h->ncluster ? pm.cvis + cluster * h->cvis_bytes : 0;
	/* the chunks' rows are read from the file as the camera comes to them: a
	   row that is not in memory is asked for, and the view goes by the groups */
	if (crow && !mx_mem_resident(crow, h->cvis_bytes)) {
		mx_mem_prefetch(crow, h->cvis_bytes);
		crow = 0;
	}
	for (int pass = 0; pass < 2; pass++) {
		for (u32 r = 0; r < ngrun; r++) {
			if (grun[r].blended != pass)
				continue;
			u32 first = nitem;
			int sky = grun[r].sky;
			for (u32 g = grun[r].first; g < (u32)grun[r].first + grun[r].n; g++) {
				if (row && !(row[g >> 3] >> (g & 7) & 1))
					continue;
				const struct pm_group *gr = &pm.group[g];
				if (nitem + gr->nchunk > MAX_ITEM)
					continue;
				st->seen++;
				/* out: wholly outside a plane of the frustum; partly in view: its
				   chunks are tested each; across: not wholly inside the planes the
				   programs that test nothing need */
				int out = 0, cross = 0, part = 0;
				for (int p = 0; p < 5 && !out; p++) {
					out = sel_far(&fr[p], gr->mins) < 0;
					part |= sel_near(&fr[p], gr->mins) < 0;
				}
				if (out)
					continue;
				for (int p = 0; p < 5 && !cross; p++)
					cross = sel_near(&gb[p], gr->mins) < 0;
				/* the strips the group touches: in one, its chunks are in that one */
				u32 gmask = rp_strip_mask(gr->mins);
				if (!gmask)
					continue;
				/* the sky's surfaces are not drawn: where they are seen, the sky is */
				if (sky) {
					/* every strip's part is the whole of it already: nothing to add */
					if (sky_full == v->all && v->all)
						continue;
					for (u32 j = gr->first_chunk; j < gr->first_chunk + gr->nchunk; j++) {
						const struct pm_chunk *ch = &pm.chunk[j];
						if (crow && !(crow[j >> 3] >> (j & 7) & 1))
							continue;
						int gone = 0;
						for (int p = 0; p < 5 && !gone; p++)
							gone = sel_far(&fr[p], ch->mins) < 0;
						if (!gone)
							sky_box(v, ch->mins);
					}
					continue;
				}
				int one = !(gmask & (gmask - 1));
				st->groups++;
				for (u32 j = gr->first_chunk; j < gr->first_chunk + gr->nchunk; j++) {
					const struct pm_chunk *ch = &pm.chunk[j];
					/* the group is seen from here, this chunk of it is not */
					if (crow && !(crow[j >> 3] >> (j & 7) & 1))
						continue;
					/* behind a closed door: the game says which areas are shut off */
					if (ch->area != 255 && areamask && (areamask[ch->area >> 3] >> (ch->area & 7) & 1))
						continue;
					/* quads that turn to the eye: the scene's, not the list's */
					if (ch->flags & CH_SPRITE) {
						int gone = 0;
						for (int p = 0; p < 5 && !gone; p++)
							gone = sel_far(&fr[p], ch->mins) < 0;
						if (!gone && nwsprite < MAX_WSPRITE) {
							wsprite[nwsprite].rec = (const float *)(pm.base + ch->off_data);
							wsprite[nwsprite].n = ch->qwc, wsprite[nwsprite].mat = ch->mat;
							nwsprite++;
						}
						continue;
					}
					/* all of it facing away: N . (eye - P) <= -sin |eye - P|
					   for every P of its box (the widest normal still faces
					   away) */
					if (ch->sine != 255 && !(rp_debug & 64)) {
						float a = 0, far2 = 0;
						for (int k = 0; k < 3; k++) {
							float n = (float)ch->cone[k], lo = org[k] - ch->maxs[k], hi = org[k] - ch->mins[k];
							a += n * (n >= 0 ? hi : lo);
							float f = hi > -lo ? hi : -lo;
							far2 += f * f;
						}
						float sn = (float)ch->sine * (127.0f / 255.0f);
						if (a < 0 && a * a >= sn * sn * far2) {
							st->back++;
							continue;
						}
					}
					if (part && gr->nchunk > 1) {
						int gone = 0;
						for (int p = 0; p < 5 && !gone; p++)
							gone = sel_far(&fr[p], ch->mins) < 0;
						if (gone)
							continue;
					}
					/* a group across a plane: which of its chunks are */
					int cut = cross;
					if (cut && gr->nchunk > 1) {
						cut = 0;
						for (int p = 0; p < 5 && !cut; p++)
							cut = sel_near(&gb[p], ch->mins) < 0;
					}
					u32 mask = one || gr->nchunk == 1 ? gmask : rp_strip_mask(ch->mins);
					if (!mask)
						continue;
					st->drawn++;
					st->clip += (u32)cut;
					st->verts += ch->nverts;
					item[nitem].chunk = (u16)j, item[nitem].clip = (u8)cut;
					item[nitem].mask = (u16)mask;
					nitem++;
				}
			}
			if (nitem > first && nwrun == MAX_WRUN)
				rp_list_stats.lost++; /* counted with the draws left out */
			if (nitem > first && nwrun < MAX_WRUN) {
				wrun[nwrun].first = (u16)first, wrun[nwrun].n = (u16)(nitem - first), wrun[nwrun].blended = (u8)pass;
				nwrun++;
			}
		}
	}
}

/* The draws of a run of n items (chunks of one material and lightmap page)
   with the matrix given: the map's own pass and the script's; -1: the list
   is full. */
static int run_draw(const void *matrix, const struct item *it, u32 n) {
	{
		const struct pm_chunk *c0 = &pm.chunk[it->chunk];
		const struct pm_mat *m = &pm.mat[c0->mat];
		struct rp_state s;
		const struct mx_tex *t;
		memset(&s, 0, RP_STATE_HEAD);
		/* a script's material: its stages' passes, after the map's own or in its place */
		static struct rp_state pass[RP_WORLD_PASSES + 1];
		u32 np = 0;
		int first = 0;
		if ((m->flags & MAT_SCRIPT) && passes_fn)
			np = passes_fn(c0->mat, pass, &first);
		int own = !(np && first);
		/* the run's texture and lightmap page in GS memory (sent there if they
		   were not); the lightmap with its texture's CLUT */
		if (own && m->tex >= 0 && m->tex < MAX_TEX && wtex[m->tex] && (t = rp_image_place(wtex[m->tex])))
			s.tex = *t, s.tex_ok = 1, s.tcc = wtex[m->tex]->alpha;
		/* a texture whose CLUT's alpha is its own has no room for the lightmaps'
		   brightness (the material neither blends nor tests that alpha): drawn
		   with a CLUT made for it; without one (no room) its lightmap is a
		   pass of its own, after the texture's */
		u32 ocbp = own && s.tex_ok && wtex[m->tex]->alpha && (c0->flags & CH_LIGHTMAP) ? rp_image_opaque(wtex[m->tex]) : 0;
		if (ocbp) {
			/* a CLUT of the image's colours and the lightmaps' brightness: as an
			   image without alpha, the texture and the lightmap from one vertex */
			s.tex.cbp = ocbp, s.tex.cpsm = MX_GS_PSM_CT32, s.tex.csm = 0, s.tcc = 0;
		}
		if (own && s.tex_ok && wtex[m->tex]->alpha && (c0->flags & CH_LIGHTMAP) && !ocbp) {
			memmove(&pass[1], &pass[0], np * sizeof pass[0]);
			memset(&pass[0], 0, sizeof pass[0]);
			pass[0].prog = RP_PROG_WORLD, pass[0].lm_only = 1, pass[0].z = RP_Z_TEST;
			np++;
			s.tcc = 0;
		} else if (own && (c0->flags & CH_LIGHTMAP) && c0->lightmap >= 0 && c0->lightmap < MAX_LM && wlm[c0->lightmap] &&
		    (t = rp_image_place(wlm[c0->lightmap]))) {
			s.lm = *t;
			s.lm.cbp = s.tex_ok ? s.tex.cbp : rp_clut_place(lm_clut_key, lm_clut, 0);
			s.lm.cpsm = MX_GS_PSM_CT32, s.lm.csm = 0;
			s.lm_ok = s.lm.cbp != 0;
		}
		s.blend = m->blend == BLEND_NONE    ? RP_BLEND_NONE
		          : m->blend == BLEND_ADD   ? RP_BLEND_ADD
		          : m->blend == BLEND_ALPHA ? RP_BLEND_ALPHA
		                                    : RP_BLEND_FILTER;
		s.alpha = m->alpha;
		s.z = RP_Z_WRITE;
		s.prog = RP_PROG_WORLD;
		/* placed without its finest levels (no room for them): as many texels less */
		s.texels = s.tex_ok && wtex[m->tex]->from ? m->texels / (float)(1u << wtex[m->tex]->from) : m->texels;
		for (u32 k = own ? 0 : 1; k <= np; k++) {
			if (k)
				pass[k - 1].texels = m->texels;
			/* a pass of the lightmap alone (a script whose $lightmap stage comes
			   after others): the chunks' page, a CLUT of brightness */
			if (k && pass[k - 1].lm_only) {
				struct rp_state *l = &pass[k - 1];
				l->lm_ok = 0;
				if ((c0->flags & CH_LIGHTMAP) && c0->lightmap >= 0 && c0->lightmap < MAX_LM && wlm[c0->lightmap] &&
				    (t = rp_image_place(wlm[c0->lightmap]))) {
					l->lm = *t;
					l->lm.cbp = rp_clut_place(lm_clut_key, lm_clut, 0);
					l->lm.cpsm = MX_GS_PSM_CT32, l->lm.csm = 0;
					l->lm_ok = l->lm.cbp != 0;
				}
				if (!l->lm_ok)
					continue;
			}
			if (rp_run(matrix, k ? &pass[k - 1] : &s) < 0)
				return -1;
			for (u32 i = 0; i < n; i++) {
				const struct pm_chunk *ch = &pm.chunk[it[i].chunk];
				rp_draw(pm.base + ch->off_data, it[i].mask, it[i].clip, ch->nverts, ch->nbatch);
			}
		}
	}
	return 0;
}

void rp_world_add(int blended) {
	if (!nwrun)
		return;
	if (!wmatrix)
		wmatrix = rp_matrix(pm.h->pos_scale, 0, 0);
	for (u32 r = 0; r < nwrun; r++)
		if (wrun[r].blended == blended && run_draw(wmatrix, &item[wrun[r].first], wrun[r].n) < 0)
			return;
}

int rp_world_bmodel(u32 n, const float *origin, const float (*axis)[3], int blended) {
	if (!pm.h || n == 0 || n >= pm.h->nmodel)
		return 0;
	const struct pm_model *md = &pm.model[n];
	const struct rp_view *v = &rp_view;
	const void *matrix = 0;
	static struct item bitem[256];
	for (u32 g = md->first_group; g < md->first_group + md->ngroup; g++) {
		const struct pm_group *gr = &pm.group[g];
		const struct pm_mat *m = &pm.mat[pm.chunk[gr->first_chunk].mat];
		if ((m->blend != BLEND_NONE) != blended || (m->flags & (MAT_SKY | MAT_NODRAW)))
			continue;
		/* its box where the entity is: the corners turned and moved */
		float box[8] = {1e30f, 1e30f, 1e30f, 0, -1e30f, -1e30f, -1e30f, 0};
		for (int c = 0; c < 8; c++) {
			float p[3] = {c & 1 ? gr->maxs[0] : gr->mins[0], c & 2 ? gr->maxs[1] : gr->mins[1],
			              c & 4 ? gr->maxs[2] : gr->mins[2]};
			for (int k = 0; k < 3; k++) {
				float w = origin[k] + p[0] * axis[0][k] + p[1] * axis[1][k] + p[2] * axis[2][k];
				box[k] = w < box[k] ? w : box[k], box[4 + k] = w > box[4 + k] ? w : box[4 + k];
			}
		}
		int out = 0, cut = 0;
		for (int p = 0; p < 5 && !out; p++)
			out = rp_box_dist(&v->fr[p], box, box + 4, 1) < 0;
		if (out)
			continue;
		for (int p = 0; p < 5 && !cut; p++)
			cut = rp_box_dist(&v->gb[p], box, box + 4, 0) < 0;
		u32 mask = rp_strip_mask(box);
		if (!mask)
			continue;
		u32 k = 0;
		for (u32 j = gr->first_chunk; j < gr->first_chunk + gr->nchunk && k < 256; j++, k++)
			bitem[k].chunk = (u16)j, bitem[k].mask = (u16)mask, bitem[k].clip = (u8)cut;
		if (!matrix && !(matrix = rp_matrix(pm.h->pos_scale, origin, axis)))
			return -1;
		if (run_draw(matrix, bitem, k) < 0)
			return -1;
	}
	return 0;
}
