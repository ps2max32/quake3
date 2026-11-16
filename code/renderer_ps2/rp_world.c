/* rp_world.c: the world of a map (PLAN-PS2.md, 5.3 and 5.4), from the .pmap
   tools/ps2map.py makes (its head has the file's layout).

   A frame: the camera's PVS cluster gives the groups it can see; each one's
   bounds go against the frustum and the guard band; of a group in view the
   chunks that do not face away are sent, one CALL each into the frame's
   stream, a VU1 program set-up when the material changes. With beam
   racing's strip records (mx_hires_strip) a chunk goes only into the
   records of the strips its group's bounds touch on the screen. No vertex
   is touched and nothing is sorted: the groups lie in the file by material.

   Not yet: the chunks across the near plane or the guard band are drawn
   with the programs that leave such triangles out (q3w2c, q3w1c) in place
   of cutting them; sky, fog, doors (areas) and the brush models. */

#include <mx/frame.h>
#include <mx/gif.h>
#include <mx/hires.h>
#include <mx/mx.h>
#include <mx/tex.h>
#include <mx/vif.h>
#include <mx/vm.h>

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
enum { ALPHA_NONE, ALPHA_GT0, ALPHA_LT128, ALPHA_GE128 };
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

/* ---- textures in GS memory --------------------------------------------------------- */

#define MAX_TEX 512
#define MAX_LM 64
static struct mx_tex gtex[MAX_TEX], glm[MAX_LM];
static u8 gtex_ok[MAX_TEX], glm_ok[MAX_LM];
/* the lightmaps' CLUT for a surface without a texture (one with a texture
   has it in its texture's CLUT): entry i's alpha is brightness i as the
   blend's factor (Cd * As, 0x80 is 1.0): twice the lightmap's value, the
   map's overbright range; in CSM1's order */
static u32 lm_clut[256] MX_CORE_BSS MX_ALIGNED(64);

struct upload {
	u32 dbp;
	u8 dbw, psm;
	u16 w, h;
	u32 qwc;
	const void *data;
};
#define MAX_UPLOAD 2048
static struct upload uploads[MAX_UPLOAD];
static u32 nupload, upload_at;

/* the free blocks of GS memory: below beam racing's buffer and above it */
static u32 vr_at[2], vr_end[2];

static void add_upload(u32 dbp, u32 dbw, u32 psm, u32 w, u32 h, u32 bytes, const void *data) {
	if (nupload < MAX_UPLOAD)
		uploads[nupload++] = (struct upload){dbp, (u8)dbw, (u8)psm, (u16)w, (u16)h, bytes / 16, data};
}

static int vr_blocks(u32 n, u32 align, u32 *at) {
	for (int r = 0; r < 2; r++) {
		u32 a = (vr_at[r] + align - 1) & ~(align - 1);
		if (a + n <= vr_end[r]) {
			*at = a;
			vr_at[r] = a + n;
			return 0;
		}
	}
	return -1;
}

static int place(struct mx_tex *t, u32 psm, u32 tw, u32 th, u32 levels) {
	for (int r = 0; r < 2; r++) {
		u32 n = mx_tex_init(t, vr_at[r], psm, tw, th, levels, MX_TEX_PACKED);
		if (vr_at[r] + n <= vr_end[r]) {
			vr_at[r] += n;
			return 0;
		}
	}
	return -1;
}

static u32 log2u(u32 v) {
	u32 n = 0;
	while ((1u << n) < v)
		n++;
	return n;
}

static void textures_place(void) {
	const struct pm_head *h = pm.h;
	u32 total = 0, lost = 0;
	vr_at[0] = 0, vr_end[0] = mx_gs.fbp[0] * 32;
	vr_at[1] = mx_gs.tbp, vr_end[1] = 16384;
	nupload = upload_at = 0;
	/* the largest first: they start on pages */
	for (u32 size = 256; size >= 8; size >>= 1)
		for (u32 i = 0; i < h->ntex && i < MAX_TEX; i++) {
			const struct pm_tex *x = &pm.tex[i];
			u32 big = x->w > x->h ? x->w : x->h;
			if (big != size && !(size == 256 && big > 256))
				continue;
			struct mx_tex *t = &gtex[i];
			u32 ncol = x->psm == MX_GS_PSM_T4 ? 16 : 256, cbp;
			gtex_ok[i] = 0;
			if (place(t, x->psm, log2u(x->w), log2u(x->h), x->levels) < 0 ||
			    vr_blocks(ncol == 256 ? 4 : 1, ncol == 256 ? 4 : 1, &cbp) < 0) {
				lost++;
				continue;
			}
			t->cbp = cbp;
			gtex_ok[i] = 1;
			total += t->blocks + (ncol == 256 ? 4 : 1);
			add_upload(cbp, 1, MX_GS_PSM_CT32, ncol == 256 ? 16 : 8, ncol == 256 ? 16 : 2, ncol * 4,
			           pm.base + x->off_clut);
			u32 off = x->off_level[0];
			for (u32 l = 0; l < x->levels; l++) {
				u32 w = x->w >> l, hh = x->h >> l;
				u32 bytes = x->psm == MX_GS_PSM_T4 ? w * hh / 2 : w * hh;
				if (l < 3)
					off = x->off_level[l];
				add_upload(t->tbp[l], t->tbw[l], x->psm, w, hh, bytes, pm.base + off);
				off += (bytes + 15) & ~15u;
			}
		}
	/* the lightmap pages and their CLUT */
	u32 lcbp = 0;
	int lc = vr_blocks(4, 4, &lcbp);
	for (u32 i = 0; i < 256; i++) {
		u32 a = i * 2 > 255 ? 255 : i * 2;
		u32 k = (i & 0x18) == 0x08 ? i + 8 : (i & 0x18) == 0x10 ? i - 8 : i;
		lm_clut[k] = a << 24 | 0x808080;
	}
	mx_dcache_writeback(lm_clut, sizeof lm_clut);
	if (lc == 0)
		add_upload(lcbp, 1, MX_GS_PSM_CT32, 16, 16, 1024, lm_clut);
	for (u32 i = 0; i < h->nlightmap && i < MAX_LM; i++) {
		glm_ok[i] = 0;
		if (lc < 0 || place(&glm[i], MX_GS_PSM_T8, 7, 7, 1) < 0) {
			lost++;
			continue;
		}
		glm[i].cbp = lcbp;
		glm_ok[i] = 1;
		total += glm[i].blocks;
		add_upload(glm[i].tbp[0], glm[i].tbw[0], MX_GS_PSM_T8, 128, 128, 16384, pm.base + h->off_lightmap + i * 16384);
	}
	mx_printf("rp: %u textures and %u lightmap pages in %u KB of GS memory (%u KB free), %u did not fit; %u uploads\n",
	          h->ntex, h->nlightmap, total / 4, (vr_end[0] - vr_at[0] + vr_end[1] - vr_at[1]) / 4, lost, nupload);
}

int rp_world_load(const void *p, u32 size) {
	const struct pm_head *h = p;
	if (size < sizeof *h || h->magic != PM_MAGIC || h->version != 2 || h->size > size || h->batch != Q3VU_BATCH)
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
	textures_place();
	return 0;
}

/* an IMAGE transfer into GS memory, its data read where it lies */
static void image_xfer(const struct upload *u) {
	u32 qwc = u->qwc;
	u64 *c = mx_frame_reserve(6 + 3 * (qwc / 0x7FFF + 1));
	const u8 *d = u->data;
	c = mx_e_cnt(c, 5, mx_vif_nop(), mx_vif_direct(5));
	c = mx_e_ad_tag(c, 4, 0);
	c = mx_e_ad(c, MX_GS_REG_BITBLTBUF, mx_gs_bitbltbuf(0, 0, 0, u->dbp, u->dbw, u->psm));
	c = mx_e_ad(c, MX_GS_REG_TRXPOS, mx_gs_trxpos(0, 0, 0, 0, 0));
	c = mx_e_ad(c, MX_GS_REG_TRXREG, mx_gs_trxreg(u->w, u->h));
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
}

int rp_world_upload(void) {
	if (upload_at >= nupload)
		return 0;
	/* beam racing: not in every pass (the once record, sent over PATH3) */
	mx_hires_once(1);
	u32 kb = 0;
	while (upload_at < nupload && kb < 192) {
		image_xfer(&uploads[upload_at]);
		kb += uploads[upload_at].qwc / 64 + 1;
		upload_at++;
	}
	u64 *c = mx_frame_reserve(3);
	c = mx_e_cnt(c, 2, 0, mx_vif_direct(2));
	c = mx_e_ad_tag(c, 1, 1);
	c = mx_e_ad(c, MX_GS_REG_TEXFLUSH, 0);
	mx_frame_commit(c);
	mx_hires_once(0);
	return (int)(nupload - upload_at);
}

/* ---- a frame ---------------------------------------------------------------------- */

/* for finding faults and costs: 1 the textures' first level only, 2 no
   lightmap pass, 4 every chunk with the programs that test nothing, 8 with
   those that test, 16 the GS draws nothing (the vertices as XYZ3: VIF1 and
   VU1 work as ever), 32 no textures, 64 chunks that face away drawn too,
   256 no strip records (everything in every pass), 128 a picture of the overdraw: every triangle adds 1/31 of its colour,
   without texture, lightmap or depth test (with a black clear: a pixel's
   largest channel times 31 / 255 is about the triangles drawn over it) */
unsigned rp_debug;

#define ZNEAR 4.0f
#define ZFAR 65536.0f
#define ZMAX 65535.0f   /* 16-bit Z */
#define GUARD 2040.0f   /* pixels from the GS's centre that a vertex may be */

static u64 dimx(void) {
	/* the GS manual's 4 x 4 matrix */
	static const s8 m[16] = {-4, 2, -3, 3, 0, -2, 1, -1, -3, 3, -4, 2, 1, -1, 0, -2};
	u64 v = 0;
	for (int i = 0; i < 16; i++)
		v |= (u64)(m[i] & 7) << (4 * i);
	return v;
}

void rp_frame_setup(void) {
	u64 *c = mx_frame_reserve(1);
	c = mx_e_cnt(c, 0, mx_vif_base(Q3VU_BASE), mx_vif_offset(Q3VU_BUF));
	mx_frame_commit(c);
	mx_frame_mpg(q3vu_vu, (u32)(q3vu_vu_end - q3vu_vu), 0);
	c = mx_frame_direct(14);
	c = mx_e_ad_tag(c, 13, 1);
	c = mx_e_ad(c, MX_GS_REG_PRMODECONT, 1);
	c = mx_e_ad(c, MX_GS_REG_COLCLAMP, 1);
	c = mx_e_ad(c, MX_GS_REG_DTHE, !(rp_debug & 128));
	c = mx_e_ad(c, MX_GS_REG_DIMX, dimx());
	c = mx_e_ad(c, MX_GS_REG_TEXA, 0x80 | (u64)0x80 << 32);
	c = mx_e_ad(c, MX_GS_REG_PABE, 0);
	c = mx_e_ad(c, MX_GS_REG_FBA_1, 0);
	c = mx_e_ad(c, MX_GS_REG_FBA_2, 0);
	c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2));
	c = mx_e_ad(c, MX_GS_REG_TEST_2, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2));
	c = mx_e_ad(c, MX_GS_REG_CLAMP_1, mx_gs_clamp(0, 0, 0, 0, 0, 0));
	c = mx_e_ad(c, MX_GS_REG_CLAMP_2, mx_gs_clamp(0, 0, 0, 0, 0, 0));
	/* context 2's blend until a material sets another: Cd * As (the lightmap pass) */
	c = mx_e_ad(c, MX_GS_REG_ALPHA_2, mx_gs_alpha(1, 2, 0, 2, 0));
	mx_frame_commit(c);
}

/* tan of deg degrees, 0 .. 89: the FPU has no such instruction and libm's is
   software floating point */
static float tan_deg(float deg) {
	float x = deg * 0.017453292f, x2 = x * x;
	float s = x * (1 - x2 * (1.0f / 6 - x2 * (1.0f / 120 - x2 * (1.0f / 5040 - x2 * (1.0f / 362880)))));
	float c = 1 - x2 * (0.5f - x2 * (1.0f / 24 - x2 * (1.0f / 720 - x2 * (1.0f / 40320 - x2 * (1.0f / 3628800)))));
	return s / c;
}

static inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static u32 fbits(float f) {
	union {
		float f;
		u32 u;
	} v = {f};
	return v.u;
}

/* a plane a point is inside of when n . p >= d */
struct plane {
	float n[3], d;
};
/* the corner of the box farthest along n (far 1) or least far (0) */
static inline float box_dist(const struct plane *p, const float *mins, const float *maxs, int far) {
	float d = -p->d;
	for (int k = 0; k < 3; k++)
		d += p->n[k] * ((p->n[k] >= 0) == far ? maxs[k] : mins[k]);
	return d;
}

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

static struct rp_world_stats *cur_st;
static float cur_fx;

/* the set-up of a material's program: the GIF tags of its passes into the
   context, its GS registers as a packet the set-up kicks; fast 0: the
   program that tests its vertices */
static u64 *material_packet(u64 *c, const struct pm_chunk *ch, int fast, int state) {
	const struct pm_mat *m = &pm.mat[ch->mat];
	int lit = (ch->flags & CH_LIGHTMAP) && ch->lightmap >= 0 && ch->lightmap < MAX_LM && glm_ok[ch->lightmap] &&
	          !(rp_debug & (2 | 128));
	int tex = m->tex >= 0 && m->tex < MAX_TEX && gtex_ok[m->tex] && !(rp_debug & (32 | 128));
	u64 regs = rp_debug & 16 ? 0xD12 : 0x512; /* ST, RGBAQ, XYZ2 (XYZ3: no drawing) */
	int blend = m->blend != BLEND_NONE || (rp_debug & 128);
	u32 entry = lit ? (fast ? Q3W2_SETUP : Q3W2C_SETUP) : (fast ? Q3W1_SETUP : Q3W1C_SETUP);
	/* blended surfaces write no Z: context 2 */
	int ctx = blend ? 1 : 0;
	u64 ta = mx_giftag(0, 0, (s32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, tex, 0, blend, 0, 0, ctx, 0), MX_GIF_FLG_PACKED, 3);
	u64 tb = mx_giftag(0, 0, (s32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 0, 1, 0, 1, 0, 0, 1, 0), MX_GIF_FLG_PACKED, 3);
	if (state) {
		c = mx_e_cnt(c, 0, mx_vif_flushe(), mx_vif_stcycl(1, 1));
		c = mx_e_cnt(c, 2, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, Q3VU_CTX_TA, 2, 0));
		c = mx_e_qw(c, ta, regs);
		c = mx_e_qw(c, tb, regs);
		u64 *tag = c;
		c += 2; /* the packet's cnt tag, written when its size is known */
		u64 *gif = c;
		c += 2;
		u32 n = 0;
		if (tex) {
			const struct mx_tex *t = &gtex[m->tex];
			s32 k = mx_tex_lod_k(m->texels, cur_fx, 0);
			u64 t1 = rp_debug & 1 ? mx_gs_tex1(1, 0, 1, 1, 0, 0, 0) : mx_tex_tex1(t, 1, t->levels > 1 ? 4 : 1, 0, k);
			/* a texture without alpha of its own: its CLUT's alpha is the
			   lightmaps' (tools/ps2map.py, gs_texture), not read here (TCC 0) */
			c = mx_tex_e_regs(c, t, ctx, mx_tex_tex0(t, pm.tex[m->tex].alpha, 0), t1);
			n += 4;
		}
		if (lit) {
			/* the lightmap with its texture's CLUT (the GS has one CLUT
			   buffer for both contexts): brightness in the alpha */
			struct mx_tex t = glm[ch->lightmap];
			if (tex)
				t.cbp = gtex[m->tex].cbp;
			c = mx_e_ad(c, MX_GS_REG_TEX1_1 + 1, mx_gs_tex1(1, 0, 1, 1, 0, 0, 0));
			c = mx_e_ad(c, MX_GS_REG_TEX0_2, mx_tex_tex0(&t, 1, 1));
			n += 2;
		}
		u32 atst = m->alpha == ALPHA_GT0 ? 6 : m->alpha == ALPHA_LT128 ? 1 : 5;
		c = mx_e_ad(c, ctx ? MX_GS_REG_TEST_2 : MX_GS_REG_TEST_1,
		            rp_debug & 128 ? mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1)
		                           : mx_gs_test(m->alpha != ALPHA_NONE, atst, m->alpha == ALPHA_GT0 ? 0 : 0x40, 0, 0, 0, 1, 2));
		n++;
		if (blend || lit) {
			/* context 2's blend: the lightmap's product, or the material's */
			u64 a = m->blend == BLEND_ADD     ? mx_gs_alpha(0, 2, 2, 1, 0x80)
			        : m->blend == BLEND_ALPHA ? mx_gs_alpha(0, 1, 0, 1, 0)
			                                  : mx_gs_alpha(1, 2, 0, 2, 0);
			if (rp_debug & 128)
				a = mx_gs_alpha(0, 2, 2, 1, 8); /* Cs * 8 / 128 + Cd */
			c = mx_e_ad(c, MX_GS_REG_ALPHA_2, a);
			n++;
		}
		mx_e_giftag(gif, n, 1, -1, MX_GIF_FLG_PACKED, 1, MX_GIF_REG_AD);
		mx_e_cnt(tag, n + 1, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, 0, n + 1, MX_UNPACK_FLG));
	} else {
		/* the other program for the same material: an empty packet */
		c = mx_e_cnt(c, 0, mx_vif_nop(), mx_vif_stcycl(1, 1));
		c = mx_e_cnt(c, 1, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, 0, 1, MX_UNPACK_FLG));
		c = mx_e_giftag(c, 0, 1, -1, MX_GIF_FLG_PACKED, 1, MX_GIF_REG_AD);
	}
	return mx_e_cnt(c, 0, mx_vif_nop(), mx_vif_mscal(entry));
}

/* A run's set-ups are made once a frame and copied into each strip's
   record: [run][the program that tests][with the material's state] */
#define SETUP_QW 24
#define MAX_RUN 512
static u64 setup_pkt[MAX_RUN][2][2][SETUP_QW * 2];
static u8 setup_n[MAX_RUN][2][2];

static void material_setup(u32 r, const struct pm_chunk *ch, int fast, int state) {
	u8 *n = &setup_n[r][!fast][state];
	u64 *pkt = setup_pkt[r][!fast][state];
	if (!*n)
		*n = (u8)((material_packet(pkt, ch, fast, state) - pkt) / 2);
	u64 *c = mx_frame_reserve(*n);
	for (u32 i = 0; i < 2u * *n; i++)
		c[i] = pkt[i];
	mx_frame_commit(c + 2 * *n);
	cur_st->qw += *n;
	cur_st->setups++;
}

#define RUN_MAX 256
/* a frame's chunks in view */
struct rp_item {
	u16 chunk, mask; /* the strips it is drawn in */
	u8 clip;         /* across a plane: the programs that test */
};
#define MAX_ITEM 4096
static struct rp_item item[MAX_ITEM];
static struct {
	u16 first, n;
} run[MAX_RUN];
static u32 nitem, nrun;

/* The strips of beam racing a box is drawn in: the lines its corners fall
   on, a line more each way (the lower field is drawn half a line lower); a
   box that reaches behind the near plane may be anywhere. */
static struct {
	const float *org, *F, *U;
	float fy, half_h;
	u32 nstrip, strip_h, all;
} sm;
static u32 strip_mask(const float *mins, const float *maxs, const struct plane *near) {
	if (box_dist(near, mins, maxs, 0) < 0)
		return sm.all;
	float lo = 1e9f, hi = -1e9f;
	for (int c8 = 0; c8 < 8; c8++) {
		float d[3] = {(c8 & 1 ? maxs[0] : mins[0]) - sm.org[0], (c8 & 2 ? maxs[1] : mins[1]) - sm.org[1],
		              (c8 & 4 ? maxs[2] : mins[2]) - sm.org[2]};
		float y = sm.half_h - sm.fy * dot3(sm.U, d) / dot3(sm.F, d);
		lo = y < lo ? y : lo, hi = y > hi ? y : hi;
	}
	s32 k0 = lo < 1.5f ? 0 : (s32)(lo - 1.5f) / (s32)sm.strip_h, k1 = hi < 0 ? -1 : (s32)(hi + 1.5f) / (s32)sm.strip_h;
	if (k1 >= (s32)sm.nstrip)
		k1 = (s32)sm.nstrip - 1;
	return k1 < k0 ? 0 : ((2u << k1) - 1) & ~((1u << k0) - 1);
}

static void calls(const u16 *ids, u32 n) {
	while (n) {
		u32 k = n > 96 ? 96 : n;
		u64 *c = mx_frame_reserve(k);
		for (u32 i = 0; i < k; i++) {
			const struct pm_chunk *ch = &pm.chunk[ids[i]];
			c = mx_e_call(c, pm.base + ch->off_data, 0, 0, 0);
			cur_st->sent += ch->nverts, cur_st->batches += ch->nbatch;
		}
		mx_frame_commit(c);
		cur_st->qw += k;
		ids += k, n -= k;
	}
}

void rp_world_draw(const struct rp_cam *cam, struct rp_world_stats *st) {
	const struct pm_head *h = pm.h;
	static struct rp_world_stats none;
	st = st ? st : &none;
	*st = (struct rp_world_stats){0};
	cur_st = st;
	if (!h || !h->nmodel)
		return;
	const float *F = cam->axis[0], *L = cam->axis[1], *U = cam->axis[2], *org = cam->org;
	float tx = tan_deg(cam->fov_x * 0.5f), ty = tan_deg(cam->fov_y * 0.5f);
	float fx = (float)mx_gs.w * 0.5f / tx, fy = (float)mx_gs.h * 0.5f / ty;
	cur_fx = fx;

	/* the matrix: a position of the file to GS coordinates times w (x, y in
	   pixels from the GS's corner, 2048 its centre; z = ZNEAR * ZMAX / w,
	   larger nearer), a sixteenth of them in z: FTOI4 gives 12.4 */
	float m[4][4]; /* m[column][row] */
	float s = h->pos_scale;
	float rowx[3], rowy[3];
	for (int k = 0; k < 3; k++) {
		rowx[k] = 2048.0f * F[k] - fx * L[k];
		rowy[k] = 2048.0f * F[k] - fy * U[k];
		m[k][0] = s * rowx[k], m[k][1] = s * rowy[k], m[k][2] = 0, m[k][3] = s * F[k];
	}
	m[3][0] = -dot3(rowx, org), m[3][1] = -dot3(rowy, org), m[3][2] = ZNEAR * ZMAX / 16.0f, m[3][3] = -dot3(F, org);
	/* the test of q3w2c, q3w1c: CS * P + CN * w inside -w .. w is inside the
	   guard band and between the depth planes */
	float zc = (ZFAR + ZNEAR) / (ZFAR - ZNEAR), zd = -2.0f * ZFAR * ZNEAR / (ZFAR - ZNEAR);
	float cs[4] = {1.0f / GUARD, 1.0f / GUARD, zd / m[3][2], 0};
	float cn[4] = {-2048.0f / GUARD, -2048.0f / GUARD, zc, 0};
	u64 *c = mx_frame_reserve(10);
	c = mx_e_cnt(c, 0, mx_vif_flushe(), mx_vif_stcycl(1, 1));
	c = mx_e_cnt(c, 8, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, 0, 8, 0));
	for (int k = 0; k < 4; k++)
		c = mx_e_w4(c, fbits(m[k][0]), fbits(m[k][1]), fbits(m[k][2]), fbits(m[k][3]));
	c = mx_e_w4(c, 0, 0, 0, 0); /* the tags' places */
	c = mx_e_w4(c, 0, 0, 0, 0);
	c = mx_e_w4(c, fbits(cs[0]), fbits(cs[1]), fbits(cs[2]), 0);
	c = mx_e_w4(c, fbits(cn[0]), fbits(cn[1]), fbits(cn[2]), 0);
	mx_frame_commit(c);
	st->qw += 10;

	/* the frustum's planes (out: wholly outside one), then the planes a chunk
	   must be wholly inside of for the programs that test nothing */
	struct plane fr[5], gb[5];
	float gx = GUARD / fx, gy = GUARD / fy;
	for (int k = 0; k < 3; k++) {
		fr[0].n[k] = F[k];
		fr[1].n[k] = tx * F[k] + L[k], fr[2].n[k] = tx * F[k] - L[k];
		fr[3].n[k] = ty * F[k] + U[k], fr[4].n[k] = ty * F[k] - U[k];
		gb[0].n[k] = F[k];
		gb[1].n[k] = gx * F[k] + L[k], gb[2].n[k] = gx * F[k] - L[k];
		gb[3].n[k] = gy * F[k] + U[k], gb[4].n[k] = gy * F[k] - U[k];
	}
	for (int p = 0; p < 5; p++)
		fr[p].d = dot3(fr[p].n, org), gb[p].d = dot3(gb[p].n, org);
	fr[0].d += ZNEAR, gb[0].d += ZNEAR + 0.5f;

	u32 cluster = cam_cluster(org);
	st->cluster = cluster;
	const u8 *row = cluster < h->ncluster ? pm.vis + cluster * h->vis_bytes : 0;
	const struct pm_model *world = &pm.model[0];
	/* what is in view: the chunks, each with the strips of beam racing its
	   group's bounds touch on the screen, in runs of one material and
	   lightmap page (the opaque materials, then the blended ones) */
	u32 nstrip = mx_hires_passes(), strip_h = mx_hires_strip_height();
	int binned = nstrip && strip_h && mx_hires_strip_record_bytes() && !(rp_debug & 256);
	u32 all = nstrip >= 32 ? ~0u : (1u << nstrip) - 1;
	sm.org = org, sm.F = F, sm.U = U, sm.fy = fy, sm.half_h = (float)mx_gs.h * 0.5f;
	sm.nstrip = nstrip, sm.strip_h = strip_h, sm.all = all;
	nitem = nrun = 0;
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
				int out = 0, cross = 0;
				for (int p = 0; p < 5 && !out; p++)
					out = box_dist(&fr[p], gr->mins, gr->maxs, 1) < 0;
				if (out)
					continue;
				for (int p = 0; p < 5 && !cross; p++)
					cross = box_dist(&gb[p], gr->mins, gr->maxs, 0) < 0;
				if (rp_debug & 12)
					cross = rp_debug >> 3 & 1;
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
					st->drawn++;
					st->clip += (u32)cross;
					st->verts += ch->nverts;
					item[nitem].chunk = (u16)j, item[nitem].clip = (u8)cross;
					item[nitem].mask = (u16)(binned ? strip_mask(ch->mins, ch->maxs, &fr[0]) : all);
					nitem++;
				}
			}
			if (nitem > first && nrun < MAX_RUN) {
				run[nrun].first = (u16)first, run[nrun].n = (u16)(nitem - first);
				setup_n[nrun][0][0] = setup_n[nrun][0][1] = setup_n[nrun][1][0] = setup_n[nrun][1][1] = 0;
				nrun++;
			}
		}
	}
	/* sent: every strip's own record gets the chunks that touch it (without
	   strip records: all of them into the main record, once) */
	static u16 list[2][RUN_MAX];
	for (u32 k = 0; k < (binned ? nstrip : 1); k++) {
		u32 bit = binned ? 1u << k : all;
		if (binned)
			mx_hires_strip((int)k);
		for (u32 r = 0; r < nrun; r++) {
			const struct rp_item *it = &item[run[r].first];
			const struct pm_chunk *c0 = &pm.chunk[it->chunk];
			u32 n[2] = {0, 0};
			int set = 0;
			for (u32 i = 0; i <= run[r].n; i++) {
				/* a list full, or the run over: sent */
				if (i == run[r].n || n[0] == RUN_MAX || n[1] == RUN_MAX) {
					for (int v = 0; v < 2; v++)
						if (n[v]) {
							material_setup(r, c0, !v, !set);
							calls(list[v], n[v]);
							set = 1, n[v] = 0;
						}
					if (i == run[r].n)
						break;
				}
				if (it[i].mask & bit)
					list[it[i].clip][n[it[i].clip]++] = it[i].chunk;
			}
		}
	}
	if (binned)
		mx_hires_strip(-1);
}
