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

   Fog: rp_world_fog (the world's surfaces; not yet what stands in it). */

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
#include "rp_model.h"
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
	u32 nfog, off_fog;
};
/* a fog volume: its colour (0 .. 1), the depth of it that is opaque, its top
   as a plane (inside where n . p - d > 0; surface 0: none, all of the
   volume), its box */
struct pm_fog {
	float colour[3], depth;
	float plane[4];
	u32 surface;
	float mins[3], maxs[3];
	u32 pad;
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
static u8 wfine[MAX_TEX]; /* the units a texture's first level takes in the pool of fine levels; 0: it has none */
static int fine_on;       /* a texture of the map has */
/* the lightmaps' CLUT for a surface without a texture (one with a texture
   has it in its texture's CLUT, tools/ps2bin.py): entry 128 + i's alpha is
   brightness i as the blend's factor (Cd * As, 0x80 is 1.0): twice the
   lightmap's value, the map's overbright range; in CSM1's order */
static u32 lm_clut[256] MX_FAST_BSS MX_ALIGNED(64);
static u64 lm_clut_key;
/* the same at half: the lightmap's value itself (a pass with lm_only 2) */
static u32 lm_clut_half[256] MX_FAST_BSS MX_ALIGNED(64);
static u64 lm_clut_half_key;

static void images_free(void) {
	/* the textures are the file of images' (the shaders' too): not freed here */
	for (u32 i = 0; i < MAX_TEX; i++)
		wtex[i] = 0, wfine[i] = 0;
	fine_on = 0;
	for (u32 i = 0; i < MAX_LM; i++)
		rp_image_free(wlm[i]), wlm[i] = 0;
}

static void images_load(void) {
	const struct pm_head *h = pm.h;
	u32 bytes = 0, none = 0;
	for (u32 i = 0; i < h->ntex && i < MAX_TEX; i++) {
		wtex[i] = rp_image_bin(pm.tex[i].name, &bytes);
		/* a model the map compiled into its surfaces (models/mapobjects/: the
		   statues, the torches) is the world's: a level by its distance, not
		   the first to go coarser when GS memory is short */
		if (wtex[i] && wtex[i]->kind == RP_IMAGE_MODEL)
			wtex[i]->kind = RP_IMAGE_WORLD;
		wfine[i] = wtex[i] ? (u8)rp_image_fine_units(wtex[i]) : 0;
		fine_on |= wfine[i] != 0;
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
		lm_clut_half[k] = (i < 128 ? 0x40 : i - 128) << 24 | 0x808080;
	}
	mx_dcache_writeback(lm_clut, sizeof lm_clut);
	mx_dcache_writeback(lm_clut_half, sizeof lm_clut_half);
	if (!lm_clut_key)
		lm_clut_key = rp_tex_key(), lm_clut_half_key = rp_tex_key();
	mx_printf("rp: %u textures (%u KB; %u not in the file of images) and %u lightmap pages\n", h->ntex, bytes / 1024,
	          none, h->nlightmap);
}

/* The world's groups in runs of one material and lightmap page, as they lie
   in the file: a view walks these and looks at a group only when the
   camera's cluster sees it. In the order of the passes (the opaque, then
   the blended, each as in the file): a pass walks its own. */
#define MAX_GRUN 4096
static struct grun {
	u16 first, n; /* groups */
	u8 blended, sky;
} grun[MAX_GRUN] MX_FAST_BSS MX_ALIGNED(64);
static u32 ngrun, grun_pass[3]; /* where each pass's runs begin; [2]: the end */

static void runs_make(void) {
	const struct pm_model *world = &pm.model[0];
	u32 g = world->first_group, end = world->first_group + world->ngroup;
	ngrun = 0, grun_pass[0] = grun_pass[1] = grun_pass[2] = 0;
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
	/* the opaque ones first: a stable sort by the pass */
	static struct grun tmp[MAX_GRUN];
	u32 k = 0;
	for (int pass = 0; pass < 2; pass++) {
		grun_pass[pass] = k;
		for (u32 r = 0; r < ngrun; r++)
			if (grun[r].blended == pass)
				tmp[k++] = grun[r];
	}
	grun_pass[2] = k;
	__builtin_memcpy(grun, tmp, k * sizeof *grun);
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
	if (size < sizeof *h || h->magic != PM_MAGIC || h->version != 11 || h->size > size || h->batch != Q3VU_BATCH)
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
		float t = (float)k * sh, e = k + 1 < n ? t + sh : h0; /* the last strip: the picture's rest */
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

/* The same tests of a group's or a chunk's box by VU0 (the compiler's vectors are VU0's
   registers, -mvu0): four planes at a time, from the box's centre and
   extent (both times two): the farthest corner along a plane's normal is at
   (n . c2 + |n| . e2) / 2, the nearest at (n . c2 - |n| . e2) / 2. Five sets
   of four planes: the frustum's first four; its fifth and three of the guard
   band's; the guard band's other two; the lines between the strips from
   below (four at most: five strips); the same from above. */
typedef float v4f __attribute__((vector_size(16), aligned(16)));
typedef float v4fa __attribute__((vector_size(16), aligned(16), may_alias));
typedef int v4i __attribute__((vector_size(16), aligned(16)));
static struct pset {
	v4f nx, ny, nz, ax, ay, az, d2;
} ps[5];
static u32 ps_lines; /* the lines between strips in the sets; 0: the view has more than fit (no VU0) */
unsigned world_st_vu0, world_st_vu0_diff;

static void pset_plane(int s, int lane, const float *n, float d) {
	ps[s].nx[lane] = n[0], ps[s].ny[lane] = n[1], ps[s].nz[lane] = n[2];
	ps[s].ax[lane] = n[0] < 0 ? -n[0] : n[0], ps[s].ay[lane] = n[1] < 0 ? -n[1] : n[1];
	ps[s].az[lane] = n[2] < 0 ? -n[2] : n[2];
	ps[s].d2[lane] = 2.0f * d;
}

static void psets(const struct rp_view *v) {
	memset(ps, 0, sizeof ps);
	for (int p = 0; p < 4; p++)
		pset_plane(0, p, v->fr[p].n, v->fr[p].d);
	pset_plane(1, 0, v->fr[4].n, v->fr[4].d);
	for (int p = 0; p < 3; p++)
		pset_plane(1, 1 + p, v->gb[p].n, v->gb[p].d);
	pset_plane(2, 0, v->gb[3].n, v->gb[3].d);
	pset_plane(2, 1, v->gb[4].n, v->gb[4].d);
	ps_lines = v->nstrip && v->nstrip <= 5 ? v->nstrip - 1 : 0;
	for (u32 j = 0; j < ps_lines; j++) {
		pset_plane(3, (int)j, v->sb_lo[j].n, v->sb_lo[j].d);
		pset_plane(4, (int)j, v->sb_hi[j].n, v->sb_hi[j].d);
	}
}

/* a bit a lane: the number is below 0 */
static inline u32 signs(v4f x) {
	v4i i = (v4i)x;
	return (u32)i[0] >> 31 | (u32)i[1] >> 31 << 1 | (u32)i[2] >> 31 << 2 | (u32)i[3] >> 31 << 3;
}

/* A box (16-byte aligned: a group of the map's file): the strips it is in,
   0: out of view; *part: not wholly inside the frustum; *cross: across a
   plane of the guard band. */
static inline u32 box_vu0(const float *b, u32 all, int *part, int *cross) {
	v4f mn = *(const v4fa *)b, mx = *(const v4fa *)(b + 4);
	v4f c = mn + mx, e = mx - mn;
#define PS_A(s) (ps[s].nx * c[0] + ps[s].ny * c[1] + ps[s].nz * c[2] - ps[s].d2)
#define PS_B(s) (ps[s].ax * e[0] + ps[s].ay * e[1] + ps[s].az * e[2])
	v4f a0 = PS_A(0), b0 = PS_B(0), a1 = PS_A(1), b1 = PS_B(1);
	if (signs(a0 + b0) | (signs(a1 + b1) & 1))
		return 0;
	u32 n1 = signs(a1 - b1);
	*part = (signs(a0 - b0) | (n1 & 1)) != 0;
	*cross = ((n1 & 14) | (signs(PS_A(2) - PS_B(2)) & 3)) != 0;
	/* below a line: its nearest corner is; above: its farthest is not below */
	u32 lo = signs(PS_B(3) - PS_A(3)), hi = signs(PS_A(4) + PS_B(4)), mask = all;
#undef PS_A
#undef PS_B
	for (u32 j = 0; j < ps_lines; j++) {
		if (lo >> j & 1)
			mask &= ~((2u << j) - 1);
		if (hi >> j & 1)
			mask &= (2u << j) - 1;
	}
	return mask;
}

/* The pool of fine levels (rp_tex.h): the view's textures that have a level
   over 128 texels and are drawn with it (the texels of that level a pixel
   where the texture is nearest: under 2), and the pool's units to those
   whose texels are largest on the screen, nearest first. One that has a
   unit goes before others: the levels do not come and go with every step. */
static float want_rho[MAX_TEX];
static u16 want_tex[MAX_TEX];
static u32 nwant;
unsigned world_st_fine, world_st_cand;

int rp_world_fine(void) { return fine_on; }

static const float *pvs_org;
void rp_world_pvs(const float *org) { pvs_org = org; }

static void want_levels(void) {
	static u16 cand[MAX_TEX];
	static float key[MAX_TEX];
	u32 nc = 0, units = rp_tex_fine_pool();
	for (u32 i = 0; i < nwant; i++) {
		u32 t = want_tex[i], lv = rp_tex_level_at(want_rho[t], 0);
		rp_image_want(wtex[t], want_rho[t]);
		if (lv == 0 && wfine[t]) {
			float k = want_rho[t] * (wtex[t]->fine ? 0.6f : 1.0f);
			u32 n = nc++;
			for (; n > 0 && key[cand[n - 1]] > k; n--)
				cand[n] = cand[n - 1];
			cand[n] = (u16)t, key[t] = k;
		}
		want_rho[t] = 0;
	}
	for (u32 c = 0; c < nc; c++) {
		u32 t = cand[c], n = wfine[t];
		if (n > units)
			continue;
		units -= n;
		world_st_fine += (unsigned)rp_image_fine(wtex[t]);
	}
	world_st_cand += nc;
	nwant = 0;
}

void rp_world_cull(struct rp_world_stats *st) {
	const struct pm_head *h = pm.h;
	const struct rp_view *v = &rp_view;
	static struct rp_world_stats none;
	st = st ? st : &none;
	*st = (struct rp_world_stats){0};
	nitem = nwrun = 0;
	/* GS memory is short: the textures from the level their distance needs */
	int far_on = rp_tex_tight();
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
	/* the groups' and the chunks' boxes are tested by VU0, four planes at a
	   time (rp_debug 268435456: by the FPU, a plane at a time): they lie
	   16-byte aligned in the map's file. Measured in the timedemo: 1 % of a
	   frame's time (62.8 for 62.3 frames a second), and no box of 635,592
	   judged otherwise than by the FPU (rp_debug 536870912) */
	int vu0 = !(rp_debug & 268435456) && !((uintptr_t)pm.group & 15) && !((uintptr_t)pm.chunk & 15);
	if (vu0) {
		psets(v);
		vu0 = ps_lines > 0;
	}
	u32 cluster = cam_cluster(pvs_org ? pvs_org : org);
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
		for (u32 r = grun_pass[pass]; r < grun_pass[pass + 1]; r++) {
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
				u32 gmask;
				if (vu0 && !(rp_debug & 536870912)) {
					gmask = box_vu0(gr->mins, v->all, &part, &cross);
					if (!gmask)
						continue;
				} else {
					for (int p = 0; p < 5 && !out; p++) {
						out = sel_far(&fr[p], gr->mins) < 0;
						part |= sel_near(&fr[p], gr->mins) < 0;
					}
					if (vu0) {
						/* rp_debug 536870912: both, and where they differ counted */
						int vpart = 0, vcross = 0, scross = 0;
						u32 vmask = box_vu0(gr->mins, v->all, &vpart, &vcross);
						u32 smask = out ? 0 : rp_strip_mask(gr->mins);
						for (int p = 0; p < 5 && !scross; p++)
							scross = sel_near(&gb[p], gr->mins) < 0;
						world_st_vu0++;
						world_st_vu0_diff += vmask != smask || (smask && (vpart != part || vcross != scross));
					}
					if (out)
						continue;
					for (int p = 0; p < 5 && !cross; p++)
						cross = sel_near(&gb[p], gr->mins) < 0;
					/* the strips the group touches: in one, its chunks are in that one */
					gmask = rp_strip_mask(gr->mins);
					if (!gmask)
						continue;
				}
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
					int cut = cross;
					u32 mask;
					int own = !(one || gr->nchunk == 1 || (rp_debug & 134217728)); /* the strips of its own box */
					if (vu0 && !(rp_debug & 536870912)) {
						/* the chunk's own box, where the group's does not say it all */
						mask = gmask;
						if (gr->nchunk > 1 && (part || cross || own)) {
							int cpart = 0, ccross = 0;
							u32 cm = box_vu0(ch->mins, v->all, &cpart, &ccross);
							if (!cm && (part || own))
								continue;
							cut = cross && ccross;
							if (own)
								mask = cm;
						}
					} else {
						int gone = 0;
						if (part && gr->nchunk > 1)
							for (int p = 0; p < 5 && !gone; p++)
								gone = sel_far(&fr[p], ch->mins) < 0;
						/* a group across a plane: which of its chunks are */
						if (!gone && cut && gr->nchunk > 1) {
							cut = 0;
							for (int p = 0; p < 5 && !cut; p++)
								cut = sel_near(&gb[p], ch->mins) < 0;
						}
						/* rp_debug 134217728: a chunk in its group's strips, not its own */
						mask = gone ? 0 : own ? rp_strip_mask(ch->mins) : gmask;
						if (vu0 && gr->nchunk > 1 && (part || cross || own)) {
							/* rp_debug 536870912: both, and where they differ counted */
							int cpart = 0, ccross = 0;
							u32 cm = box_vu0(ch->mins, v->all, &cpart, &ccross);
							u32 vmask = !cm && (part || own) ? 0 : own ? cm : gmask;
							world_st_vu0++;
							world_st_vu0_diff += vmask != mask || (mask && (cross && ccross) != cut);
						}
					}
					if (!mask)
						continue;
					st->drawn++;
					st->clip += (u32)cut;
					st->verts += ch->nverts;
					item[nitem].chunk = (u16)j, item[nitem].clip = (u8)cut;
					item[nitem].mask = (u16)mask;
					nitem++;
					/* how large its texture's texels are where it is nearest (the GS
					   takes the level by the depth): for the pool of fine levels */
					{
						const struct pm_mat *cm = &pm.mat[ch->mat];
						if ((far_on || wfine[cm->tex < MAX_TEX && cm->tex >= 0 ? cm->tex : 0]) && cm->tex >= 0 &&
						    cm->tex < MAX_TEX && wtex[cm->tex]) {
							float w = cut ? 4.0f : rp_box_dist(&v->fr[0], ch->mins, ch->maxs, 0) / v->fr_len[0] + 4.0f;
							float rho = cm->texels * (w < 4.0f ? 4.0f : w) / v->flod;
							if (!want_rho[cm->tex])
								want_tex[nwant++] = (u16)cm->tex, want_rho[cm->tex] = rho;
							else if (rho < want_rho[cm->tex])
								want_rho[cm->tex] = rho;
						}
					}
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
	if (nwant)
		want_levels();
}

/* tcGen environment for a chunk: the engine's texture coordinates are 0.5 +
   0.5 y and 0.5 - 0.5 z of the eye's direction from the vertex, reflected in
   the surface. With the chunk's direction for the surface (its normals'
   cone; none: towards the eye) and the eye's distance taken at the chunk's
   middle, they are linear in the vertex's position: the uniforms of the
   program that lays a texture on the world by position (q3wd), with the
   stage's texture matrix (s->tm) and colour (s->cadd). */
static void env_rows(struct rp_state *s, const struct pm_chunk *ch) {
	const struct rp_view *v = &rp_view;
	float ps = pm.h->pos_scale, n[3], d[3], len = 0;
	for (int a = 0; a < 3; a++) {
		d[a] = v->org[a] - (ch->mins[a] + ch->maxs[a]) * 0.5f;
		len += d[a] * d[a];
	}
	len = sqrtf(len);
	len = len < 16.0f ? 16.0f : len;
	if (ch->sine < 200) {
		float l = sqrtf((float)(ch->cone[0] * ch->cone[0] + ch->cone[1] * ch->cone[1] + ch->cone[2] * ch->cone[2]));
		for (int a = 0; a < 3; a++)
			n[a] = l > 0 ? (float)ch->cone[a] / l : 0;
	} else
		for (int a = 0; a < 3; a++)
			n[a] = d[a] / len;
	float k = 1.0f / len, ne = n[0] * v->org[0] + n[1] * v->org[1] + n[2] * v->org[2];
	float sp[3], tp[3];
	for (int a = 0; a < 3; a++) {
		sp[a] = 0.5f * k * (-2.0f * n[1] * n[a] + (a == 1));
		tp[a] = -0.5f * k * (-2.0f * n[2] * n[a] + (a == 2));
	}
	float s0 = 0.5f + 0.5f * k * (2.0f * n[1] * ne - v->org[1]), t0 = 0.5f - 0.5f * k * (2.0f * n[2] * ne - v->org[2]);
	const float *tm = s->tm;
	float (*m)[4] = s->model;
	memset(m, 0, 5 * sizeof m[0]);
	for (int a = 0; a < 3; a++) {
		m[a][0] = ps * (tm[0] * sp[a] + tm[1] * tp[a]);
		m[a][1] = ps * (tm[3] * sp[a] + tm[4] * tp[a]);
	}
	m[3][0] = tm[0] * s0 + tm[1] * t0 + tm[2];
	m[3][1] = tm[3] * s0 + tm[4] * t0 + tm[5];
	for (int a = 0; a < 4; a++)
		m[4][a] = s->cadd[a];
}

/* alphaGen lightingSpecular: the engine's alpha of a vertex is the fourth
   power of the cosine between the eye's direction and the reflection of the
   direction to a light at a fixed place of the map. With the eye's direction
   mirrored in the chunk's direction that is the cosine between two unit
   vectors, l to the light and v' : 1 - |l - v'|^2 / 2. The difference l - v'
   is made straight around the chunk's middle (its three parts linear in the
   vertex's position), which the program that lays a texture on the world by
   position takes: two parts, across the bisector of l and v', are the
   coordinates in a texture of (1 - r^2 / 2)^4; the third, along the bisector
   (0 at the middle), is the program's fall-off 1 - z^2. The picture times
   1 + that: the texture brighter by its shine. 0: none on this chunk. */
#define SPEC 64
static u8 spec_tex[SPEC * SPEC] MX_FAST_BSS MX_ALIGNED(64);
static u32 spec_clut[256] MX_FAST_BSS MX_ALIGNED(64);
static struct rp_image *spec_img;

static struct rp_image *spec_image(void) {
	if (spec_img)
		return spec_img;
	for (int y = 0; y < SPEC; y++)
		for (int x = 0; x < SPEC; x++) {
			float dx = ((float)x + 0.5f) * (3.0f / SPEC) - 1.5f, dy = ((float)y + 0.5f) * (3.0f / SPEC) - 1.5f;
			float v = 1.0f - (dx * dx + dy * dy) * 0.5f;
			int edge = x == 0 || y == 0 || x == SPEC - 1 || y == SPEC - 1;
			v = v <= 0 || edge ? 0 : v * v;
			spec_tex[y * SPEC + x] = (u8)(v * v * 255.0f);
		}
	for (u32 i = 0; i < 256; i++)
		spec_clut[(i & ~0x18u) | (i & 8) << 1 | (i & 16) >> 1] = (i + 1) >> 1 << 24 | 0x808080;
	mx_dcache_writeback(spec_tex, sizeof spec_tex);
	mx_dcache_writeback(spec_clut, sizeof spec_clut);
	static const u8 *level;
	level = spec_tex;
	spec_img = rp_image_static("*shine", MX_GS_PSM_T8, SPEC, SPEC, 1, 0, &level, spec_clut);
	return spec_img;
}

static int spec_rows(struct rp_state *s, const struct pm_chunk *ch) {
	static const float lorg[3] = {-960.0f, 1980.0f, 96.0f}; /* the engine's (RB_CalcSpecularAlpha) */
	const struct rp_view *vw = &rp_view;
	if (ch->sine >= 200)
		return 0;
	float ps = pm.h->pos_scale, n[3], c[3], l[3], v[3], vr[3], a0[3], e[3][3], dl = 0, de = 0, nl = 0, nv = 0, aa = 0, el = 0;
	for (int a = 0; a < 3; a++) {
		c[a] = (ch->mins[a] + ch->maxs[a]) * 0.5f;
		l[a] = lorg[a] - c[a], v[a] = vw->org[a] - c[a];
		dl += l[a] * l[a], de += v[a] * v[a], nl += (float)(ch->cone[a] * ch->cone[a]);
	}
	dl = sqrtf(dl), de = sqrtf(de), nl = sqrtf(nl);
	if (dl < 1.0f || de < 1.0f || nl < 1.0f)
		return 0;
	for (int a = 0; a < 3; a++) {
		n[a] = (float)ch->cone[a] / nl, l[a] /= dl, v[a] /= de;
		nv += n[a] * v[a];
	}
	for (int a = 0; a < 3; a++) {
		vr[a] = 2.0f * nv * n[a] - v[a];
		a0[a] = l[a] - vr[a], e[2][a] = l[a] + vr[a];
		aa += a0[a] * a0[a], el += e[2][a] * e[2][a];
	}
	/* no shine at its middle (the cosine under 0.2) */
	if (aa > 1.6f || el < 0.01f)
		return 0;
	el = 1.0f / sqrtf(el);
	for (int a = 0; a < 3; a++)
		e[2][a] *= el;
	/* two directions across the bisector */
	int k = fabsf(e[2][0]) < fabsf(e[2][1]) ? (fabsf(e[2][0]) < fabsf(e[2][2]) ? 0 : 2) : (fabsf(e[2][1]) < fabsf(e[2][2]) ? 1 : 2);
	float u[3] = {k == 0, k == 1, k == 2};
	e[0][0] = e[2][1] * u[2] - e[2][2] * u[1], e[0][1] = e[2][2] * u[0] - e[2][0] * u[2], e[0][2] = e[2][0] * u[1] - e[2][1] * u[0];
	el = 1.0f / sqrtf(e[0][0] * e[0][0] + e[0][1] * e[0][1] + e[0][2] * e[0][2]);
	for (int a = 0; a < 3; a++)
		e[0][a] *= el;
	e[1][0] = e[2][1] * e[0][2] - e[2][2] * e[0][1], e[1][1] = e[2][2] * e[0][0] - e[2][0] * e[0][2], e[1][2] = e[2][0] * e[0][1] - e[2][1] * e[0][0];
	float (*m)[4] = s->model;
	memset(m, 0, 5 * sizeof m[0]);
	for (int j = 0; j < 3; j++) {
		/* the part of l - v' along e[j] by the vertex's position: at the middle,
		   and its slope there */
		float sc = j < 2 ? 1.0f / 3.0f : 1.41421356f, off = j < 2 ? 0.5f : 0.0f;
		float le = 0, ne = 0, er[3], ver = 0, at = 0, gc = 0;
		for (int a = 0; a < 3; a++)
			le += l[a] * e[j][a], ne += n[a] * e[j][a], at += a0[a] * e[j][a];
		for (int a = 0; a < 3; a++) {
			er[a] = 2.0f * ne * n[a] - e[j][a];
			ver += v[a] * er[a];
		}
		for (int a = 0; a < 3; a++) {
			float g = -(e[j][a] - l[a] * le) / dl + (er[a] - v[a] * ver) / de;
			m[a][j] = ps * sc * g;
			gc += g * c[a];
		}
		m[3][j] = off + sc * (at - gc);
	}
	m[4][0] = m[4][1] = m[4][2] = 128.0f, m[4][3] = 128.0f;
	return 1;
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
		/* a script with nothing to draw (no stage, texture or lightmap): a fog's
		   face. The engine draws it with the fog's pass alone, as rp_world_fog
		   does (seen from inside the fog it is the haze over what lies outside);
		   drawn by a pass of its own it was a white or black sheet over the
		   fog's floor. */
		if ((m->flags & MAT_SCRIPT) && !np && m->tex < 0 && !(c0->flags & CH_LIGHTMAP))
			return 0;
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
					l->lm.cbp = l->lm_only == 2 ? rp_clut_place(lm_clut_half_key, lm_clut_half, 0)
					                            : rp_clut_place(lm_clut_key, lm_clut, 0);
					l->lm.cpsm = MX_GS_PSM_CT32, l->lm.csm = 0;
					l->lm_ok = l->lm.cbp != 0;
				}
				if (!l->lm_ok)
					continue;
			}
			if (k && pass[k - 1].prog == RP_PROG_DLIGHT) {
				/* a stage with tcGen environment: a run a chunk, with the chunk's
				   reflection of the eye */
				if (pass[k - 1].menv == 2) {
					/* the shine of alphaGen lightingSpecular: its texture */
					struct rp_image *im = spec_image();
					if (!im || !(t = rp_image_place(im)))
						continue;
					pass[k - 1].tex = *t, pass[k - 1].tex_ok = 1, pass[k - 1].texels = 1.0f;
				}
				for (u32 i = 0; i < n; i++) {
					const struct pm_chunk *ch = &pm.chunk[it[i].chunk];
					if (pass[k - 1].menv == 2) {
						if (!spec_rows(&pass[k - 1], ch))
							continue;
					} else
						env_rows(&pass[k - 1], ch);
					if (rp_run(matrix, &pass[k - 1]) < 0)
						return -1;
					rp_draw(pm.base + ch->off_data, it[i].mask, it[i].clip, ch->nverts, ch->nbatch);
				}
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

/* ---- dynamic lights. A light's spot is a texture (round, strong in the
   middle) laid on the chunks near the light, and the picture there is made
   brighter by it (times 1 + the spot's alpha, as the engine's lights
   multiply what is there: the GS has no product of colours, so a light's
   own colour is not in it; the models take that). Its
   coordinates are the vertex's position along two of the world's axes, and
   along the third the vertex's colour falls off (q3wd, tools/ps2vu.py). The
   third axis is the one a chunk faces most (its normals' cone; else its
   box's thinnest side), so on a floor or a wall the spot is round and
   exact in every pixel; along the third axis it is by vertex. A light is
   up to three runs: the chunks by axis. ---- */

struct rp_dl_stats rp_dl_stats;
#define dl_stats rp_dl_stats
#define SPOT 64
static u8 spot_tex[SPOT * SPOT] MX_FAST_BSS MX_ALIGNED(64);
static u32 spot_clut[256] MX_FAST_BSS MX_ALIGNED(64);
static struct rp_image *spot_img;

static struct rp_image *spot_image(void) {
	if (spot_img)
		return spot_img;
	for (int y = 0; y < SPOT; y++)
		for (int x = 0; x < SPOT; x++) {
			float dx = ((float)x + 0.5f) * (2.0f / SPOT) - 1.0f, dy = ((float)y + 0.5f) * (2.0f / SPOT) - 1.0f;
			float v = 1.0f - (dx * dx + dy * dy);
			int edge = x == 0 || y == 0 || x == SPOT - 1 || y == SPOT - 1;
			spot_tex[y * SPOT + x] = (u8)(v <= 0 || edge ? 0 : v * 255.0f);
		}
	/* the strength by index, as alpha (0x80: 1), in the GS's order of a CLUT's entries */
	for (u32 i = 0; i < 256; i++)
		spot_clut[(i & ~0x18u) | (i & 8) << 1 | (i & 16) >> 1] = (i + 1) >> 1 << 24 | 0x808080;
	mx_dcache_writeback(spot_tex, sizeof spot_tex);
	mx_dcache_writeback(spot_clut, sizeof spot_clut);
	static const u8 *level;
	level = spot_tex;
	spot_img = rp_image_static("*dlight", MX_GS_PSM_T8, SPOT, SPOT, 1, 0, &level, spot_clut);
	return spot_img;
}

void rp_world_dlights(const struct rp_dlight *lights, unsigned nl, unsigned max) {
	static u16 list[3][256];
	if (!pm.h || !nitem || !nl)
		return;
	struct rp_image *im = spot_image();
	if (!im)
		return;
	if (!wmatrix)
		wmatrix = rp_matrix(pm.h->pos_scale, 0, 0);
	float ps = pm.h->pos_scale;
	if (!max)
		return;
	for (unsigned li = 0; li < nl && li < max; li++) {
		const struct rp_dlight *l = &lights[li];
		float R = l->radius;
		if (R < 32.0f)
			continue;
		u32 cnt[3] = {0, 0, 0};
		for (u32 r = 0; r < nwrun; r++) {
			if (wrun[r].blended)
				continue;
			for (u32 i = wrun[r].first; i < (u32)wrun[r].first + wrun[r].n; i++) {
				const struct pm_chunk *ch = &pm.chunk[item[i].chunk];
				/* not what is seen through (its holes have the depth of what is behind) */
				if (pm.mat[ch->mat].alpha)
					continue;
				/* the light reaches its box */
				float d2 = 0, ext[3];
				for (int a = 0; a < 3; a++) {
					float d = l->org[a] < ch->mins[a] ? ch->mins[a] - l->org[a] : l->org[a] > ch->maxs[a] ? l->org[a] - ch->maxs[a] : 0;
					d2 += d * d;
					ext[a] = ch->maxs[a] - ch->mins[a];
				}
				if (d2 >= R * R)
					continue;
				int w;
				if (ch->sine < 128) {
					/* it faces one way: not lit from behind */
					float back = 0;
					for (int a = 0; a < 3; a++)
						back += (float)ch->cone[a] * (l->org[a] - (ch->mins[a] + ch->maxs[a]) * 0.5f);
					if (back < -127.0f * 8.0f)
						continue;
					int ax = ch->cone[0] < 0 ? -ch->cone[0] : ch->cone[0], ay = ch->cone[1] < 0 ? -ch->cone[1] : ch->cone[1],
					    az = ch->cone[2] < 0 ? -ch->cone[2] : ch->cone[2];
					w = ax > ay ? (ax > az ? 0 : 2) : (ay > az ? 1 : 2);
				} else
					w = ext[0] < ext[1] ? (ext[0] < ext[2] ? 0 : 2) : (ext[1] < ext[2] ? 1 : 2);
				if (cnt[w] < 256)
					list[w][cnt[w]++] = (u16)i;
			}
		}
		for (int w = 0; w < 3; w++) {
			if (!cnt[w])
				continue;
			struct rp_state s;
			memset(&s, 0, sizeof s);
			const struct mx_tex *t = rp_image_place(im);
			if (!t)
				return;
			s.tex = *t, s.tex_ok = 1;
			s.blend = RP_BLEND_BRIGHTEN, s.z = RP_Z_TEST, s.prog = RP_PROG_DLIGHT, s.clamp = 1, s.nomip = 1;
			s.tcc = 1;
			/* no pixel outside the spot is read and written again */
			s.alpha = RP_ALPHA_GT0;
			s.texels = 1.0f;
			/* a position of the file (times ps: the world's) to the spot's s, t along
			   the two other axes and, along w, how far off the light in radii */
			int u = (w + 1) % 3, v = (w + 2) % 3;
			float (*m)[4] = s.model;
			m[u][0] = ps * 0.5f / R, m[3][0] = 0.5f - l->org[u] * 0.5f / R;
			m[v][1] = ps * 0.5f / R, m[3][1] = 0.5f - l->org[v] * 0.5f / R;
			m[w][2] = ps / R, m[3][2] = -l->org[w] / R;
			/* its strength: the mean of its colour (the engine's light is times each
			   channel: a rocket's leaves the blue as it is, and the brightest alone
			   lit everything twice as bright), as the vertices' alpha (0x80: the
			   spot's own) */
			float lum = (l->col[0] + l->col[1] + l->col[2]) * (1.0f / 3.0f);
			m[4][0] = m[4][1] = m[4][2] = 128.0f;
			m[4][3] = 128.0f * (lum > 1.0f ? 1.0f : lum);
			if (rp_run(wmatrix, &s) < 0)
				return;
			for (u32 k = 0; k < cnt[w]; k++) {
				const struct item *it = &item[list[w][k]];
				const struct pm_chunk *ch = &pm.chunk[it->chunk];
				rp_draw(pm.base + ch->off_data, it->mask, it->clip, ch->nverts, ch->nbatch);
			}
			dl_stats.runs++, dl_stats.chunks += cnt[w];
		}
		dl_stats.lights++;
	}
}

/* ---- fog. A fog volume's surfaces (a chunk's fog: tools/ps2map.py) are
   drawn once more with the fog's colour over them, as much of it as the
   ray from the eye passes through fog: the engine's way (a texture of the
   fog's strength by depth and by the share of the ray that is in the fog,
   its coordinates from the vertex), by the program that lays a texture on
   the world by position (q3wd, the dynamic lights'). The strength is the
   root of depth / the fog's opaque depth, times the share; the share is the
   engine's t / (t - eye) (t the vertex's depth under the fog's top, eye the
   eye's height over it): the coordinate, linear in the position as the
   program's are, is t in eye's heights, and the texture has u / (1 + u)
   down it; from within the fog its last row, all of the ray. ---- */

#define FOG_W 64
#define FOG_H 64
#define FOG_U 8.0f /* eye's heights over the fog's top down the texture */
static u8 fog_tex[FOG_W * FOG_H] MX_FAST_BSS MX_ALIGNED(64);
static u32 fog_clut[256] MX_FAST_BSS MX_ALIGNED(64);
static struct rp_image *fog_img;

static struct rp_image *fog_image(void) {
	if (fog_img)
		return fog_img;
	for (int y = 0; y < FOG_H; y++)
		for (int x = 0; x < FOG_W; x++) {
			/* the first column and row: none (the coordinates are clamped there).
			   Down the texture: how far the vertex is under the fog's top, in
			   eye's heights over it (u, to FOG_U): the share of the eye's ray that
			   is in the fog is u / (1 + u), which no straight coordinate is; the
			   last row is all of the ray (the eye in the fog itself) */
			float d = x ? ((float)x + 0.5f) / FOG_W : 0, u = ((float)y + 0.5f) * (FOG_U / FOG_H);
			float share = !y ? 0 : y == FOG_H - 1 ? 1.0f : u / (1.0f + u);
			float a = sqrtf(d * share);
			fog_tex[y * FOG_W + x] = (u8)(a >= 1.0f ? 255 : a * 255.0f);
		}
	for (u32 i = 0; i < 256; i++)
		fog_clut[(i & ~0x18u) | (i & 8) << 1 | (i & 16) >> 1] = (i + 1) >> 1 << 24 | 0x808080;
	mx_dcache_writeback(fog_tex, sizeof fog_tex);
	mx_dcache_writeback(fog_clut, sizeof fog_clut);
	static const u8 *level;
	level = fog_tex;
	fog_img = rp_image_static("*fog", MX_GS_PSM_T8, FOG_W, FOG_H, 1, 1, &level, fog_clut);
	return fog_img;
}

void rp_world_fog(void) {
	static u16 list[256];
	const struct pm_head *h = pm.h;
	const struct rp_view *v = &rp_view;
	if (!h || !h->nfog || !nitem)
		return;
	const struct pm_fog *fogs = (const void *)(pm.base + h->off_fog);
	float ps = h->pos_scale;
	for (u32 f = 0; f < h->nfog && f < 255; f++) {
		u32 cnt = 0;
		for (u32 r = 0; r < nwrun; r++) {
			if (wrun[r].blended)
				continue;
			for (u32 i = wrun[r].first; i < (u32)wrun[r].first + wrun[r].n && cnt < 256; i++)
				/* not what is seen through (a grate): its holes have the depth of what
				   is behind them, and the fog would fill them */
				if (pm.chunk[item[i].chunk].fog == f && !pm.mat[pm.chunk[item[i].chunk].mat].alpha)
					list[cnt++] = (u16)i;
		}
		if (!cnt)
			continue;
		const struct pm_fog *fg = &fogs[f];
		struct rp_image *im = fog_image();
		const struct mx_tex *t = im ? rp_image_place(im) : 0;
		if (!t)
			return;
		if (!wmatrix)
			wmatrix = rp_matrix(ps, 0, 0);
		struct rp_state s;
		memset(&s, 0, sizeof s);
		s.tex = *t, s.tex_ok = 1, s.tcc = 1;
		s.blend = RP_BLEND_ALPHA, s.z = RP_Z_TEST, s.prog = RP_PROG_DLIGHT, s.clamp = 1, s.nomip = 1;
		s.texels = 1.0f;
		float (*m)[4] = s.model;
		/* s: the depth before the eye by the fog's opaque depth */
		float id = 1.0f / (fg->depth > 1.0f ? fg->depth : 1.0f);
		for (int a = 0; a < 3; a++)
			m[a][0] = ps * v->F[a] * id;
		m[3][0] = -(v->org[0] * v->F[0] + v->org[1] * v->F[1] + v->org[2] * v->F[2]) * id;
		/* t: the share of the ray in the fog, by how far the vertex is under the
		   fog's top */
		if (!fg->surface)
			m[3][1] = 1.0f;
		else {
			float eye = v->org[0] * fg->plane[0] + v->org[1] * fg->plane[1] + v->org[2] * fg->plane[2] - fg->plane[3];
			/* the eye over the fog: the vertex's depth under the top by the eye's
			   height, FOG_U of them down the texture; in it: the last row for all
			   that is in the fog */
			float k = eye < 0 ? 1.0f / (FOG_U * (eye > -4.0f ? 4.0f : -eye)) : 1.0f / 32.0f, t0 = eye < 0 ? 0.0f : 1.0f;
			for (int a = 0; a < 3; a++)
				m[a][1] = ps * fg->plane[a] * k;
			m[3][1] = t0 - fg->plane[3] * k;
		}
		/* the colour: the fog's as it is (the texture's is 0x80), its strength the
		   texture's alpha */
		for (int a = 0; a < 3; a++) {
			float c = fg->colour[a] * 255.0f;
			m[4][a] = c > 255.0f ? 255.0f : c;
		}
		m[4][3] = 128.0f;
		if (rp_run(wmatrix, &s) < 0)
			return;
		for (u32 k = 0; k < cnt; k++) {
			const struct item *it = &item[list[k]];
			const struct pm_chunk *ch = &pm.chunk[it->chunk];
			rp_draw(pm.base + ch->off_data, it->mask, it->clip, ch->nverts, ch->nbatch);
		}
	}
}

/* a flat chunk's plane; 0: it is not flat */
static int chunk_plane(const struct pm_chunk *ch, float *plane) {
	float l = sqrtf((float)(ch->cone[0] * ch->cone[0] + ch->cone[1] * ch->cone[1] + ch->cone[2] * ch->cone[2]));
	if (ch->sine >= 64 || l < 1.0f)
		return 0;
	plane[3] = 0;
	for (int a = 0; a < 3; a++) {
		plane[a] = (float)ch->cone[a] / l;
		plane[3] += plane[a] * (ch->mins[a] + ch->maxs[a]) * 0.5f;
	}
	return 1;
}

int rp_world_find(int (*is)(u32 mat), float *plane, u32 *from) {
	for (u32 i = *from; i < nitem; i++)
		if (is(pm.chunk[item[i].chunk].mat) && chunk_plane(&pm.chunk[item[i].chunk], plane)) {
			*from = i + 1;
			return 1;
		}
	return 0;
}

void rp_world_bounds(int (*is)(u32 mat), const float *plane, float *mins, float *maxs) {
	for (int a = 0; a < 3; a++)
		mins[a] = 1e9f, maxs[a] = -1e9f;
	for (u32 i = 0; i < nitem; i++) {
		const struct pm_chunk *ch = &pm.chunk[item[i].chunk];
		float pl[4];
		if (!is(ch->mat) || !chunk_plane(ch, pl))
			continue;
		float d = pl[3] - plane[3];
		if (pl[0] * plane[0] + pl[1] * plane[1] + pl[2] * plane[2] < 0.99f || d < -8.0f || d > 8.0f)
			continue;
		for (int a = 0; a < 3; a++) {
			mins[a] = ch->mins[a] < mins[a] ? ch->mins[a] : mins[a];
			maxs[a] = ch->maxs[a] > maxs[a] ? ch->maxs[a] : maxs[a];
		}
	}
}

void rp_world_depth(int (*is)(u32 mat), const float *plane) {
	int run = 0;
	for (u32 i = 0; i < nitem; i++) {
		const struct pm_chunk *ch = &pm.chunk[item[i].chunk];
		float pl[4];
		if (!is(ch->mat) || !chunk_plane(ch, pl))
			continue;
		float d = pl[3] - plane[3];
		if (pl[0] * plane[0] + pl[1] * plane[1] + pl[2] * plane[2] < 0.99f || d < -8.0f || d > 8.0f)
			continue;
		if (!run) {
			struct rp_state s;
			memset(&s, 0, sizeof s);
			s.prog = RP_PROG_WORLD, s.blend = RP_BLEND_NONE, s.z = RP_Z_WRITE, s.alpha = RP_ALPHA_DEPTH, s.texels = 1.0f;
			if (!wmatrix)
				wmatrix = rp_matrix(pm.h->pos_scale, 0, 0);
			if (rp_run(wmatrix, &s) < 0)
				return;
			run = 1;
		}
		rp_draw(pm.base + ch->off_data, item[i].mask, item[i].clip, ch->nverts, ch->nbatch);
	}
}

int rp_world_fog_at(const float *p, float *colour, float *strength) {
	const struct pm_head *h = pm.h;
	const struct rp_view *v = &rp_view;
	if (!h || !h->nfog)
		return 0;
	const struct pm_fog *fogs = (const void *)(pm.base + h->off_fog);
	for (u32 f = 0; f < h->nfog && f < 255; f++) {
		const struct pm_fog *fg = &fogs[f];
		if (p[0] < fg->mins[0] || p[1] < fg->mins[1] || p[2] < fg->mins[2] || p[0] > fg->maxs[0] || p[1] > fg->maxs[1] ||
		    p[2] > fg->maxs[2])
			continue;
		/* as rp_world_fog's texture coordinates: the depth before the eye by the
		   fog's opaque depth, and the share of the eye's ray that is in the fog */
		float d = ((p[0] - v->org[0]) * v->F[0] + (p[1] - v->org[1]) * v->F[1] + (p[2] - v->org[2]) * v->F[2]) /
		          (fg->depth > 1.0f ? fg->depth : 1.0f);
		float share = 1.0f;
		if (fg->surface) {
			float eye = v->org[0] * fg->plane[0] + v->org[1] * fg->plane[1] + v->org[2] * fg->plane[2] - fg->plane[3];
			float dp = p[0] * fg->plane[0] + p[1] * fg->plane[1] + p[2] * fg->plane[2] - fg->plane[3];
			share = dp <= 0 ? 0 : eye < 0 ? dp / (dp - eye) : 1.0f;
		}
		d = d < 0 ? 0 : d > 1.0f ? 1.0f : d, share = share < 0 ? 0 : share > 1.0f ? 1.0f : share;
		float a = sqrtf(d * share);
		if (a < 0.02f)
			return 0;
		for (int c = 0; c < 3; c++) {
			float x = fg->colour[c] * 255.0f;
			colour[c] = x > 255.0f ? 255.0f : x;
		}
		*strength = a;
		return 1;
	}
	return 0;
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
