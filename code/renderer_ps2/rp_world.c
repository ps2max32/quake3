/* rp_world.c: the world of a map (PLAN-PS2.md, 5.3 and 5.4), from the .pmap
   tools/ps2map.py makes (its head has the file's layout).

   A view: the camera's PVS cluster gives the groups it can see; each one's
   bounds go against the frustum and the guard band; of a group in view the
   chunks that do not face away become draws of the frame's list (rp_list.c),
   a run a material and lightmap page, each with the strips of beam racing
   its bounds touch on the screen. No vertex is touched and nothing is
   sorted: the groups lie in the file by material.

   A group across the near plane or the guard band is drawn with the programs
   that cut such triangles (q3w2c, q3w1c, tools/ps2vu.py).

   Not yet: sky, fog, doors (areas) and the brush models. */

#include <mx/frame.h>
#include <mx/gif.h>
#include <mx/hires.h>
#include <mx/mx.h>
#include <mx/tex.h>
#include <mx/vif.h>
#include <mx/vm.h>

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
	char name[40];
	u16 w, h;
	u8 levels, psm, alpha, pad;
	u32 off_clut;
	u32 off_level[3];
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
	u32 pad;
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
enum { MAT_LIGHTMAP = 1, MAT_SKY = 2, MAT_NODRAW = 4 };
enum { BLEND_NONE, BLEND_ADD, BLEND_ALPHA, BLEND_FILTER, BLEND_OTHER };
/* a material's alpha test: rp_list.h's RP_ALPHA_* */
enum { CH_LIGHTMAP = 1 };

static struct {
	const u8 *base;
	const struct pm_head *h;
	const struct pm_mat *mat;
	const struct pm_tex *tex;
	const struct pm_chunk *chunk;
	const struct pm_group *group;
	const struct pm_model *model;
	const u8 *vis;
	const float (*plane)[4];
	const struct pm_node *node;
	const struct pm_leaf *leaf;
} pm;

/* ---- its textures and lightmap pages ------------------------------------------------ */

#define MAX_TEX 512
#define MAX_LM 64
static struct rp_image *wtex[MAX_TEX], *wlm[MAX_LM];
/* the lightmaps' CLUT for a surface without a texture (one with a texture
   has it in its texture's CLUT): entry i's alpha is brightness i as the
   blend's factor (Cd * As, 0x80 is 1.0): twice the lightmap's value, the
   map's overbright range; in CSM1's order */
static u32 lm_clut[256] MX_CORE_BSS MX_ALIGNED(64);
static u64 lm_clut_key;

static void images_free(void) {
	for (u32 i = 0; i < MAX_TEX; i++)
		rp_image_free(wtex[i]), wtex[i] = 0;
	for (u32 i = 0; i < MAX_LM; i++)
		rp_image_free(wlm[i]), wlm[i] = 0;
}

static void images_load(void) {
	const struct pm_head *h = pm.h;
	u32 bytes = 0;
	for (u32 i = 0; i < h->ntex && i < MAX_TEX; i++) {
		const struct pm_tex *x = &pm.tex[i];
		const u8 *level[MX_TEX_LEVELS];
		u32 off = x->off_level[0], levels = x->levels > MX_TEX_LEVELS ? MX_TEX_LEVELS : x->levels;
		for (u32 l = 0; l < levels; l++) {
			u32 w = x->w >> l, hh = x->h >> l;
			u32 n = x->psm == MX_GS_PSM_T4 ? w * hh / 2 : w * hh;
			if (l < 3)
				off = x->off_level[l];
			level[l] = pm.base + off;
			off += (n + 15) & ~15u;
			bytes += n;
		}
		wtex[i] = rp_image_static(x->name, x->psm, x->w, x->h, levels, x->alpha, level, pm.base + x->off_clut);
	}
	for (u32 i = 0; i < h->nlightmap && i < MAX_LM; i++) {
		const u8 *level = pm.base + h->off_lightmap + i * 16384;
		wlm[i] = rp_image_static("*lightmap", MX_GS_PSM_T8, 128, 128, 1, 0, &level, 0);
	}
	for (u32 i = 0; i < 256; i++) {
		u32 a = i * 2 > 255 ? 255 : i * 2;
		u32 k = (i & 0x18) == 0x08 ? i + 8 : (i & 0x18) == 0x10 ? i - 8 : i;
		lm_clut[k] = a << 24 | 0x808080;
	}
	mx_dcache_writeback(lm_clut, sizeof lm_clut);
	if (!lm_clut_key)
		lm_clut_key = rp_tex_key();
	mx_printf("rp: %u textures (%u KB) and %u lightmap pages\n", h->ntex, bytes / 1024, h->nlightmap);
}

void rp_world_free(void) {
	images_free();
	pm.h = 0;
}

int rp_world_load(const void *p, u32 size) {
	const struct pm_head *h = p;
	rp_world_free();
	if (size < sizeof *h || h->magic != PM_MAGIC || h->version != 4 || h->size > size || h->batch != Q3VU_BATCH)
		return -1;
	pm.base = p, pm.h = h;
	pm.mat = (const void *)(pm.base + h->off_mat);
	pm.tex = (const void *)(pm.base + h->off_tex);
	pm.chunk = (const void *)(pm.base + h->off_chunk);
	pm.group = (const void *)(pm.base + h->off_group);
	pm.model = (const void *)(pm.base + h->off_model);
	pm.vis = pm.base + h->off_vis;
	pm.plane = (const void *)(pm.base + h->off_plane);
	pm.node = (const void *)(pm.base + h->off_node);
	pm.leaf = (const void *)(pm.base + h->off_leaf);
	images_load();
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
#define MAX_WRUN 512
static struct item item[MAX_ITEM];
static struct {
	u16 first, n;
	u8 blended;
} wrun[MAX_WRUN];
static u32 nitem, nwrun;
static const void *wmatrix;

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
	const struct pm_model *world = &pm.model[0];
	for (int pass = 0; pass < 2; pass++) {
		u32 g = world->first_group, end = world->first_group + world->ngroup;
		while (g < end) {
			const struct pm_chunk *c0 = &pm.chunk[pm.group[g].first_chunk];
			int blended = pm.mat[c0->mat].blend != BLEND_NONE;
			u32 first = nitem;
			/* the run of groups of one material and lightmap page */
			for (; g < end; g++) {
				const struct pm_group *gr = &pm.group[g];
				if (pm.chunk[gr->first_chunk].mat != c0->mat || pm.chunk[gr->first_chunk].lightmap != c0->lightmap)
					break;
				if (blended != pass || nitem + gr->nchunk > MAX_ITEM)
					continue;
				if (row && !(row[g >> 3] >> (g & 7) & 1))
					continue;
				if (!pass)
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
				u32 gmask = rp_strip_mask(gr->mins, gr->maxs);
				if (!gmask)
					continue;
				int one = !(gmask & (gmask - 1));
				st->groups++;
				for (u32 j = gr->first_chunk; j < gr->first_chunk + gr->nchunk; j++) {
					const struct pm_chunk *ch = &pm.chunk[j];
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
					u32 mask = one || gr->nchunk == 1 ? gmask : rp_strip_mask(ch->mins, ch->maxs);
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
			if (nitem > first && nwrun < MAX_WRUN) {
				wrun[nwrun].first = (u16)first, wrun[nwrun].n = (u16)(nitem - first), wrun[nwrun].blended = (u8)pass;
				nwrun++;
			}
		}
	}
}

void rp_world_add(int blended) {
	if (!nwrun)
		return;
	if (!wmatrix)
		wmatrix = rp_matrix(pm.h->pos_scale, 0, 0);
	for (u32 r = 0; r < nwrun; r++) {
		if (wrun[r].blended != blended)
			continue;
		const struct item *it = &item[wrun[r].first];
		const struct pm_chunk *c0 = &pm.chunk[it->chunk];
		const struct pm_mat *m = &pm.mat[c0->mat];
		struct rp_state s = {0};
		const struct mx_tex *t;
		/* the run's texture and lightmap page in GS memory (sent there if they
		   were not); the lightmap with its texture's CLUT */
		if (m->tex >= 0 && m->tex < MAX_TEX && wtex[m->tex] && (t = rp_image_place(wtex[m->tex])))
			s.tex = *t, s.tex_ok = 1, s.tcc = pm.tex[m->tex].alpha;
		if ((c0->flags & CH_LIGHTMAP) && c0->lightmap >= 0 && c0->lightmap < MAX_LM && wlm[c0->lightmap] &&
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
		s.texels = m->texels;
		if (rp_run(wmatrix, &s) < 0)
			return;
		for (u32 i = 0; i < wrun[r].n; i++) {
			const struct pm_chunk *ch = &pm.chunk[it[i].chunk];
			rp_draw(pm.base + ch->off_data, it[i].mask, it[i].clip, ch->nverts, ch->nbatch);
		}
	}
}
