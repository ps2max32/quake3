/* rp_list.c: a frame's draws (rp_list.h; PLAN-PS2.md, 5.3 and 5.4).

   A draw is a DMA chain that ends with a RET: VIF1 unpacks its vertices and
   runs the program its run has set up. The runs are kept in the order they
   were made. At the frame's end each strip of beam racing gets, in its own
   record, the draws that touch it: a CALL each, before the first one of a
   run a CALL of the run's matrix (if it is another than the last one's) and
   of its set-up (the GIF tags of its passes into VU1's context, its GS
   registers as a packet the set-up's program kicks). So VIF1, VU1 and the
   GS see a strip's draws in a pass, not the frame's. Matrices, set-ups and
   the draws built for a frame lie in memory of this file's (rp_dyn), kept
   until the frame is done.

   Nothing in a strip's stream waits for the GS (tools/ps2vu.py: a program's
   last packet lies where the VIF writes nothing), but: a set-up waits for
   the program before it (FLUSHE: it writes the context, and its packet's
   place is the set-up before's, which has left when that program has
   kicked); and between a run of the models' program and one of the others
   the stream waits for all that was sent (a FLUSH: their buffers are laid
   out differently). */

#include <mx/frame.h>
#include <mx/gif.h>
#include <mx/hires.h>
#include <mx/mem.h>
#include <mx/mx.h>
#include <mx/tex.h>
#include <mx/vif.h>

#include "rp_list.h"
#include "vu/q3vu.h"

unsigned rp_debug;
struct rp_view rp_view;
struct rp_list_stats rp_list_stats;

#define ZNEAR 4.0f
#define ZFAR 65536.0f
#define ZMAX 65280.0f   /* 16-bit Z, with room for a cut vertex's rounding at the near plane */
#define GUARD 2040.0f   /* pixels from the GS's centre that a vertex may be */

static inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static u32 fbits(float f) {
	union {
		float f;
		u32 u;
	} v = {f};
	return v.u;
}

/* ---- memory for a frame's chains: a ring, a frame's part free when it is done ------- */

#define DYN_QW (768 * 1024 / 16)
#define DYN_FRAMES 16
static u64 *dyn; /* uncached: written once, read by the DMAC */
/* the next quadword; those in use, from the oldest frame in flight's first
   to the head, round the end; the last rp_dyn's first */
static u32 d_head, d_used, d_last;
static struct {
	u32 frame, start;
} d_fr[DYN_FRAMES];
static u32 d_nfr;

static void dyn_frame(void) {
	u32 done = mx_frame_done(), k = 0;
	while (k < d_nfr && d_fr[k].frame < done)
		k++;
	for (u32 i = k; i < d_nfr; i++)
		d_fr[i - k] = d_fr[i];
	d_nfr -= k;
	if (!d_nfr)
		d_head = d_used = 0;
	else
		d_used = (d_head + DYN_QW - d_fr[0].start) % DYN_QW;
	if (d_nfr == DYN_FRAMES) /* cannot be: beam racing has fewer records */
		d_nfr--;
	d_fr[d_nfr].frame = mx_frame_number(), d_fr[d_nfr].start = d_head;
	d_nfr++;
}

u64 *rp_dyn(u32 qw) {
	u32 h = d_head, waste = 0;
	if (!dyn || !qw)
		return 0;
	/* in one piece: round the end, what is left there is not used */
	if (h + qw > DYN_QW)
		waste = DYN_QW - h, h = 0;
	if (d_used + waste + qw >= DYN_QW) {
		rp_list_stats.lost++;
		return 0;
	}
	d_last = h;
	d_head = h + qw, d_used += waste + qw;
	rp_list_stats.dyn_qw += qw;
	return dyn + 2 * h;
}

void rp_dyn_end(u64 *end) {
	u32 e = (u32)(end - dyn) / 2;
	if (e >= d_last && e <= d_head) {
		rp_list_stats.dyn_qw -= d_head - e, d_used -= d_head - e;
		d_head = e;
	}
}

/* ---- the view --------------------------------------------------------------------- */

/* tan of deg degrees, 0 .. 89: the FPU has no such instruction and libm's is
   software floating point */
static float tan_deg(float deg) {
	float x = deg * 0.017453292f, x2 = x * x;
	float s = x * (1 - x2 * (1.0f / 6 - x2 * (1.0f / 120 - x2 * (1.0f / 5040 - x2 * (1.0f / 362880)))));
	float c = 1 - x2 * (0.5f - x2 * (1.0f / 24 - x2 * (1.0f / 720 - x2 * (1.0f / 40320 - x2 * (1.0f / 3628800)))));
	return s / c;
}

void rp_view_rect(const struct rp_cam *cam, float x, float y, float w, float h) {
	struct rp_view *v = &rp_view;
	float tx = tan_deg(cam->fov_x * 0.5f), ty = tan_deg(cam->fov_y * 0.5f);
	v->fx = w * 0.5f / tx, v->fy = h * 0.5f / ty;
	v->cx = 2048.0f + x + w * 0.5f - (float)mx_gs.w * 0.5f;
	v->cy = 2048.0f + y + h * 0.5f - (float)mx_gs.h * 0.5f;
	/* the planes of the guard band, as tangents from the view's centre: the
	   nearer of the band's two sides each way */
	float gl = v->cx - (2048.0f - GUARD), gr = 2048.0f + GUARD - v->cx;
	float gt = v->cy - (2048.0f - GUARD), gbm = 2048.0f + GUARD - v->cy;
	float gx = (gl < gr ? gl : gr) / v->fx, gy = (gt < gbm ? gt : gbm) / v->fy;
	for (int k = 0; k < 3; k++) {
		float F = cam->axis[0][k], L = cam->axis[1][k], U = cam->axis[2][k];
		v->org[k] = cam->org[k], v->F[k] = F, v->L[k] = L, v->U[k] = U;
		v->rowx[k] = v->cx * F - v->fx * L;
		v->rowy[k] = v->cy * F - v->fy * U;
		v->fr[0].n[k] = F;
		v->fr[1].n[k] = tx * F + L, v->fr[2].n[k] = tx * F - L;
		v->fr[3].n[k] = ty * F + U, v->fr[4].n[k] = ty * F - U;
		v->gb[0].n[k] = F;
		v->gb[1].n[k] = gx * F + L, v->gb[2].n[k] = gx * F - L;
		v->gb[3].n[k] = gy * F + U, v->gb[4].n[k] = gy * F - U;
	}
	for (int p = 0; p < 5; p++) {
		v->fr[p].d = dot3(v->fr[p].n, v->org), v->gb[p].d = dot3(v->gb[p].n, v->org);
		v->fr_len[p] = __builtin_sqrtf(dot3(v->fr[p].n, v->fr[p].n));
		v->gb_len[p] = __builtin_sqrtf(dot3(v->gb[p].n, v->gb[p].n));
	}
	v->fr[0].d += ZNEAR, v->gb[0].d += ZNEAR + 0.5f;
	v->nstrip = mx_hires_passes(), v->strip_h = mx_hires_strip_height();
	if (!v->nstrip || !v->strip_h || !mx_hires_strip_record_bytes() || (rp_debug & 256))
		v->nstrip = 0;
	if (v->nstrip > 16)
		v->nstrip = 16;
	v->strip_inv = v->strip_h ? 1.0f / (float)v->strip_h : 0;
	v->all = !v->nstrip ? 1 : (1u << v->nstrip) - 1;
	v->serial++;
	/* a point d from the eye is below the picture's line B when (top - B) F . d
	   - fy U . d > 0, top the line of the view's centre */
	float top = v->cy - 2048.0f + (float)mx_gs.h * 0.5f;
	for (u32 j = 1; j < v->nstrip; j++) {
		float B = (float)(j * v->strip_h);
		for (int k = 0; k < 3; k++) {
			v->sb_lo[j - 1].n[k] = (top - (B + 1.5f)) * v->F[k] - v->fy * v->U[k];
			v->sb_hi[j - 1].n[k] = (top - (B - 1.5f)) * v->F[k] - v->fy * v->U[k];
			v->sb_near[j - 1][k] = (u8)(v->sb_lo[j - 1].n[k] >= 0 ? k : 4 + k);
			v->sb_far[j - 1][k] = (u8)(v->sb_hi[j - 1].n[k] >= 0 ? 4 + k : k);
		}
		v->sb_lo[j - 1].d = dot3(v->sb_lo[j - 1].n, v->org), v->sb_hi[j - 1].d = dot3(v->sb_hi[j - 1].n, v->org);
	}
}

void rp_view_set(const struct rp_cam *cam) {
	float W = (float)mx_gs.w, H = (float)mx_gs.h;
	/* rp_debug 16384: the same triangles over a quarter of the pixels (what of
	   a pass's time is the triangles', what the pixels') */
	if (rp_debug & 16384)
		rp_view_rect(cam, W * 0.25f, H * 0.25f, W * 0.5f, H * 0.5f);
	else
		rp_view_rect(cam, 0, 0, W, H);
}

/* The lines its corners fall on, a line more each way (the lower field is
   drawn half a line lower). */
u32 rp_strip_lines(float lo, float hi) {
	const struct rp_view *v = &rp_view;
	if (!v->nstrip)
		return 1;
	if (hi < 0 || lo > (float)mx_gs.h)
		return 0;
	s32 k0 = lo < 1.5f ? 0 : (s32)((lo - 1.5f) * v->strip_inv), k1 = (s32)((hi + 1.5f) * v->strip_inv);
	if (k1 >= (s32)v->nstrip)
		k1 = (s32)v->nstrip - 1;
	return k1 < k0 ? 0 : ((2u << k1) - 1) & ~((1u << k0) - 1);
}

/* A box is out of the strips above a line when all of it is below that line
   (its corner nearest along the line's plane is), and out of those below
   when all of it is above. The lines are planes through the eye, so this
   holds for what of the box is seen (in front of the near plane) wherever
   the rest of it is. */
u32 rp_strip_mask(const float *b) {
	const struct rp_view *v = &rp_view;
	if (!v->nstrip)
		return 1;
	u32 mask = v->all;
	for (u32 j = 0; j + 1 < v->nstrip; j++) {
		const struct rp_plane *lo = &v->sb_lo[j], *hi = &v->sb_hi[j];
		const u8 *n = v->sb_near[j], *f = v->sb_far[j];
		if (lo->n[0] * b[n[0]] + lo->n[1] * b[n[1]] + lo->n[2] * b[n[2]] > lo->d)
			mask &= ~((2u << j) - 1); /* below the line: not in strips 0 .. j */
		if (hi->n[0] * b[f[0]] + hi->n[1] * b[f[1]] + hi->n[2] * b[f[2]] < hi->d)
			mask &= (2u << j) - 1;    /* above it: not in strips j + 1 .. */
	}
	return mask;
}

/* The same for the points' hull. */
u32 rp_strip_points(const float (*p)[3], u32 n) {
	const struct rp_view *v = &rp_view;
	if (!v->nstrip)
		return 1;
	u32 mask = v->all;
	for (u32 j = 0; j + 1 < v->nstrip; j++) {
		const struct rp_plane *lo = &v->sb_lo[j], *hi = &v->sb_hi[j];
		int below = 1, above = 1;
		for (u32 i = 0; i < n; i++) {
			below &= dot3(lo->n, p[i]) > lo->d;
			above &= dot3(hi->n, p[i]) < hi->d;
		}
		if (below)
			mask &= ~((2u << j) - 1);
		if (above)
			mask &= (2u << j) - 1;
	}
	return mask;
}

/* ---- matrices ------------------------------------------------------------------------- */

/* A position of the run to GS coordinates times w (x, y in pixels from the
   GS's corner; z = ZNEAR * ZMAX / w, larger nearer), a sixteenth of them in
   z: FTOI4 gives 12.4. With it the test of the programs that cut: CS * P +
   CN * w inside -w .. w is inside the guard band and between the depth
   planes. The chain: wait for the program before (it reads the context),
   then the six quadwords. */
const void *rp_matrix(float scale, const float *origin, const float (*axis)[3]) {
	const struct rp_view *v = &rp_view;
	static const float ident[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, zero[3] = {0, 0, 0};
	float m[4][4], d[3];
	axis = axis ? axis : ident, origin = origin ? origin : zero;
	for (int k = 0; k < 3; k++) {
		m[k][0] = scale * dot3(v->rowx, axis[k]), m[k][1] = scale * dot3(v->rowy, axis[k]);
		m[k][2] = 0, m[k][3] = scale * dot3(v->F, axis[k]);
		d[k] = origin[k] - v->org[k];
	}
	m[3][0] = dot3(v->rowx, d), m[3][1] = dot3(v->rowy, d), m[3][2] = ZNEAR * ZMAX / 16.0f, m[3][3] = dot3(v->F, d);
	float zc = (ZFAR + ZNEAR) / (ZFAR - ZNEAR), zd = -2.0f * ZFAR * ZNEAR / (ZFAR - ZNEAR);
	u64 *c0 = rp_dyn(9), *c = c0;
	if (!c)
		return 0;
	c = mx_e_cnt(c, 0, mx_vif_flushe(), mx_vif_stcycl(1, 1));
	c = mx_e_cnt(c, 6, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, Q3VU_CTX_C0, 6, 0));
	for (int k = 0; k < 4; k++)
		c = mx_e_w4(c, fbits(m[k][0]), fbits(m[k][1]), fbits(m[k][2]), fbits(m[k][3]));
	c = mx_e_w4(c, fbits(1.0f / GUARD), fbits(1.0f / GUARD), fbits(zd / m[3][2]), 0);
	c = mx_e_w4(c, fbits(-2048.0f / GUARD), fbits(-2048.0f / GUARD), fbits(zc), 0);
	mx_e_ret(c, 0, 0, 0);
	return c0;
}

/* ---- runs and draws ---------------------------------------------------------------------- */

#define MAX_RUN 2048
#define MAX_DRAW 6144
#define LIST_MAX 256
static struct run {
	const void *matrix;
	struct rp_state s;
	const void *setup[2][2]; /* [the programs that cut][with the state] */
	const void *ghost[2];    /* rp_debug 131072: the same, drawing nothing */
	u16 first, n;
} run[MAX_RUN];
static struct draw {
	const void *chain;
	u16 mask, verts;
	u8 clip, batches;
} draw[MAX_DRAW];
static u32 nrun, ndraw;

int rp_run(const void *matrix, const struct rp_state *s) {
	if (nrun == MAX_RUN || (!matrix && s->prog != RP_PROG_GS)) {
		rp_list_stats.lost++;
		return -1;
	}
	struct run *r = &run[nrun];
	r->matrix = matrix;
	__builtin_memcpy(&r->s, s, s->prog == RP_PROG_MODEL || s->prog == RP_PROG_DLIGHT ? sizeof *s : RP_STATE_HEAD);
	r->setup[0][0] = r->setup[0][1] = r->setup[1][0] = r->setup[1][1] = 0;
	r->ghost[0] = r->ghost[1] = 0;
	r->first = (u16)ndraw, r->n = 0;
	return (int)nrun++;
}

int rp_run_last(void) { return (int)nrun - 1; }

void rp_draw(const void *chain, u32 mask, int clip, u32 verts, u32 batches) {
	if (!nrun || ndraw == MAX_DRAW || !chain) {
		rp_list_stats.lost++;
		return;
	}
	if (rp_debug & 12)
		clip = rp_debug >> 3 & 1;
	struct draw *d = &draw[ndraw++];
	d->chain = chain, d->mask = (u16)mask, d->verts = (u16)verts, d->clip = (u8)clip, d->batches = (u8)batches;
	run[nrun - 1].n++;
}

/* The set-up of a run's program (cut 1: the one that cuts): the GIF tags of
   its passes into the context, its GS registers as a packet the set-up's
   program kicks; state 0: the same registers as the set-up before it in the
   strip (the run's other program): an empty packet. */
/* TEX1's K (1/16 of a level) that puts the first level where a texel is a
   pixel, as mx_tex_lod_k but with log2 as the exponent and the fraction as
   it is (a tenth of a level off at most), for the hundreds of set-ups a
   frame */
static s32 lod_k(float texels, float pixels) {
	union {
		float f;
		u32 u;
	} a = {texels}, b = {pixels};
	if (!(texels > 0) || !(pixels > 0))
		return 0;
	s32 d = ((s32)(a.u >> 23) - (s32)(b.u >> 23)) * 16 + (((s32)(a.u >> 19 & 15) - (s32)(b.u >> 19 & 15)));
	return d < -2048 ? -2048 : d > 2047 ? 2047 : d;
}

static u64 blend_reg(const struct rp_state *s) {
	return s->blend == RP_BLEND_ADD         ? mx_gs_alpha(0, 2, 2, 1, 0x80)
	       : s->blend == RP_BLEND_ALPHA     ? mx_gs_alpha(0, 1, 0, 1, 0)
	       : s->blend == RP_BLEND_ADD_ALPHA ? mx_gs_alpha(0, 2, 0, 1, 0)
	       : s->blend == RP_BLEND_INVFILTER ? mx_gs_alpha(2, 1, 0, 1, 0)
	       : s->blend == RP_BLEND_INVALPHA  ? mx_gs_alpha(1, 0, 0, 0, 0)
	       : s->blend == RP_BLEND_BRIGHTEN  ? mx_gs_alpha(1, 2, 0, 1, 0)
	                                        : mx_gs_alpha(1, 2, 0, 2, 0);
}

/* The set-up of a run of GIF packets: its GS registers, context 2's (no Z
   is written), after what VU1 still sends. */
static const void *setup_gs(const struct rp_state *s) {
	u64 *c0 = rp_dyn(12), *c = c0;
	if (!c)
		return 0;
	int tex = s->tex_ok && !(rp_debug & 32), blend = s->blend != RP_BLEND_NONE;
	u64 *tag = c;
	c += 2;
	u64 *gif = c;
	c += 2;
	u32 n = 0;
	if (tex) {
		c = mx_tex_e_regs(c, &s->tex, 1, mx_tex_tex0(&s->tex, s->tcc, 0), mx_gs_tex1(1, 0, 1, 1, 0, 0, 0));
		c = mx_e_ad(c, MX_GS_REG_CLAMP_2, s->clamp ? mx_gs_clamp(1, 1, 0, 0, 0, 0) : 0);
		n += 5;
	}
	u32 atst = s->alpha == RP_ALPHA_GT0 ? 6 : s->alpha == RP_ALPHA_LT128 ? 1 : 5;
	c = mx_e_ad(c, MX_GS_REG_TEST_2, mx_gs_test(s->alpha != RP_ALPHA_NONE, atst, s->alpha == RP_ALPHA_GT0 ? 0 : 0x40, 0, 0, 0,
	                                            1, s->z == RP_Z_NONE ? 1 : 2));
	c = mx_e_ad(c, MX_GS_REG_ALPHA_2, blend_reg(s));
	c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, tex, 0, blend, 0, 0, 1, 0));
	n += 3;
	mx_e_giftag(gif, n, 1, -1, MX_GIF_FLG_PACKED, 1, MX_GIF_REG_AD);
	mx_e_cnt(tag, n + 1, mx_vif_flush(), mx_vif_direct(n + 1));
	c = mx_e_ret(c, 0, 0, 0);
	rp_dyn_end(c);
	return c0;
}

static int ghost; /* the set-up being made draws nothing (rp_debug 131072) */
static const void *setup_chain(const struct rp_state *s, int cut, int state) {
	if (s->prog == RP_PROG_GS)
		return setup_gs(s);
	u64 *c0 = rp_dyn(48), *c = c0;
	if (!c)
		return 0;
	int lit = s->lm_ok && s->prog == RP_PROG_WORLD && !(rp_debug & (2 | 128)), tex = s->tex_ok && !(rp_debug & (32 | 128));
	_Static_assert(Q3VU_T_TM0 + 1 == Q3VU_T_TM1 && Q3VU_T_TM0 + 4 == Q3VU_T_CADD, "q3w1t's uniforms in a row");
	u64 regs = (rp_debug & 16) || ghost ? 0xD12 : 0x512; /* ST, RGBAQ, XYZ2 (XYZ3: no drawing) */
	int blend = s->blend != RP_BLEND_NONE || (rp_debug & 128);
	/* The lightmap's pass is the picture times the lightmap, cut off to the
	   frame's 5 bits a colour a second time: 4 of 255 darker each time, on
	   average (the dither's matrix is -4 .. 3), and the product's own
	   rounding down is 1 or 2 more (measured against the PC's picture). The
	   second time is made up for: the pass adds 6, as its own colour (the
	   fog's, with the fog's share at 0: the lightmap's texels have no colour
	   of their own). The first time: the images' palettes are 4 brighter
	   (tools/ps2bin.py). */
	int lmadd = lit && s->blend == RP_BLEND_NONE && !(rp_debug & 2097152);
	int menv = s->prog == RP_PROG_MODEL && s->menv;
	u32 entry = menv                       ? (cut ? Q3MEC_SETUP : Q3ME_SETUP)
	            : s->prog == RP_PROG_MODEL ? (cut ? Q3MC_SETUP : Q3M_SETUP)
	            : s->prog == RP_PROG_STAGE ? (cut ? Q3W1TC_SETUP : Q3W1T_SETUP)
	            : s->prog == RP_PROG_DLIGHT ? (cut ? Q3WDC_SETUP : Q3WD_SETUP)
	            : lit                    ? (cut ? Q3W2C_SETUP : Q3W2_SETUP)
	                                     : (cut ? Q3W1C_SETUP : Q3W1_SETUP);
	/* context 2 writes no Z (beam racing's): blended draws, and those without depth */
	int ctx = blend || s->z != RP_Z_WRITE ? 1 : 0;
	if (state) {
		/* the passes' tags: a batch's strips (the program adds the count), then a
		   triangle of its own (q3clip's) */
		c = mx_e_cnt(c, 0, mx_vif_flushe(), mx_vif_stcycl(1, 1));
		c = mx_e_cnt(c, 4, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, Q3VU_CTX_TA, 4, 0));
		for (int tri = 0; tri < 2; tri++) {
			u32 prim = tri ? MX_GS_PRIM_TRIANGLE : MX_GS_PRIM_TRISTRIP, n = tri ? 3 : 0;
			/* the lightmap alone: the first pass's vertices as XYZ3, drawing nothing */
			c = mx_e_qw(c, mx_giftag(n, tri, (s32)mx_gs_prim(prim, 1, tex, 0, blend, 0, 0, ctx, 0), MX_GIF_FLG_PACKED, 3),
			            s->lm_only && lit ? 0xD12 : regs);
			c = mx_e_qw(c, mx_giftag(n, tri, (s32)mx_gs_prim(prim, 0, 1, lmadd, 1, 0, 0, 1, 0), MX_GIF_FLG_PACKED, 3), regs);
		}
		if (s->prog == RP_PROG_MODEL) {
			_Static_assert(Q3VU_M_LERP + 6 == Q3VU_M_EX && Q3VU_M_EX + 3 == Q3VU_M_E0, "q3me's uniforms after q3m's");
			u32 nu = menv ? 10 : 6;
			c = mx_e_cnt(c, nu, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, Q3VU_M_LERP, nu, 0));
			for (u32 k = 0; k < nu; k++)
				c = mx_e_w4(c, fbits(s->model[k][0]), fbits(s->model[k][1]), fbits(s->model[k][2]), fbits(s->model[k][3]));
			/* the gather's cull: the MAC flag's sign of z that leaves a triangle out */
			c = mx_e_cnt(c, 1, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, Q3VU_CTX_CULL, 1, 0));
			c = mx_e_w4(c, s->cull == RP_CULL_NEG ? 0x20 : s->cull == RP_CULL_POS ? 0 : 0x40, 0, 0, 0);
		}
		if (s->prog == RP_PROG_DLIGHT) {
			_Static_assert(Q3VU_D_DM0 + 4 == Q3VU_D_DLC, "q3wd's uniforms in a row");
			c = mx_e_cnt(c, 5, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, Q3VU_D_DM0, 5, 0));
			for (int k = 0; k < 5; k++)
				c = mx_e_w4(c, fbits(s->model[k][0]), fbits(s->model[k][1]), fbits(s->model[k][2]), fbits(s->model[k][3]));
		}
		if (s->prog == RP_PROG_STAGE) {
			c = mx_e_cnt(c, 5, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, Q3VU_T_TM0, 5, 0));
			c = mx_e_w4(c, fbits(s->tm[0]), fbits(s->tm[3]), 0, 0);
			c = mx_e_w4(c, fbits(s->tm[1]), fbits(s->tm[4]), 0, 0);
			c = mx_e_w4(c, fbits(s->tm[2]), fbits(s->tm[5]), fbits(1.0f), 0);
			c = mx_e_w4(c, fbits(s->cmul[0]), fbits(s->cmul[1]), fbits(s->cmul[2]), fbits(s->cmul[3]));
			c = mx_e_w4(c, fbits(s->cadd[0]), fbits(s->cadd[1]), fbits(s->cadd[2]), fbits(s->cadd[3]));
		}
		u64 *tag = c;
		c += 2; /* the packet's cnt tag, written when its size is known */
		u64 *gif = c;
		c += 2;
		u32 n = 0;
		/* rp_debug 65536: the run's GS registers left out (what a set-up costs the GS) */
		int regs_on = !(rp_debug & 65536);
		if (!regs_on)
			tex = lit = 0;
		if (tex) {
			const struct mx_tex *t = &s->tex;
			s32 k = lod_k(s->texels, rp_view.fx);
			u64 t1 = (rp_debug & 1) || s->nomip ? mx_gs_tex1(1, 0, 1, 1, 0, 0, 0)
			                                    : mx_tex_tex1(t, 1, t->levels > 1 ? 4 : 1, 0, k);
			/* a texture without alpha of its own: its CLUT's alpha is the
			   lightmaps' (tools/ps2map.py, gs_texture), not read here (TCC 0) */
			u64 t0 = mx_tex_tex0(t, s->tcc, 0);
			if (rp_debug & 1024)
				t0 &= ~((u64)7 << 61);
			c = mx_tex_e_regs(c, t, ctx, t0, t1);
			c = mx_e_ad(c, ctx ? MX_GS_REG_CLAMP_2 : MX_GS_REG_CLAMP_1, s->clamp ? mx_gs_clamp(1, 1, 0, 0, 0, 0) : 0);
			n += 5;
		}
		if (lit) {
			/* the lightmap with its texture's CLUT (the GS has one CLUT
			   buffer for both contexts): brightness in the alpha */
			c = mx_e_ad(c, MX_GS_REG_TEX1_1 + 1, mx_gs_tex1(1, 0, 1, 1, 0, 0, 0));
			c = mx_e_ad(c, MX_GS_REG_TEX0_2, mx_tex_tex0(&s->lm, 1, 1) & ~(rp_debug & (1024 | 2048) ? (u64)7 << 61 : 0));
			c = mx_e_ad(c, MX_GS_REG_CLAMP_2, 0);
			c = mx_e_ad(c, MX_GS_REG_TEST_2, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2));
			n += 4;
			if (lmadd) {
				c = mx_e_ad(c, MX_GS_REG_FOG, 0);
				c = mx_e_ad(c, MX_GS_REG_FOGCOL, 0x060606);
				n += 2;
			}
		}
		u32 atst = s->alpha == RP_ALPHA_GT0 ? 6 : s->alpha == RP_ALPHA_LT128 ? 1 : 5;
		if (regs_on) {
		c = mx_e_ad(c, ctx ? MX_GS_REG_TEST_2 : MX_GS_REG_TEST_1,
		            rp_debug & 128 ? mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1)
		                           : mx_gs_test(s->alpha != RP_ALPHA_NONE, atst, s->alpha == RP_ALPHA_GT0 ? 0 : 0x40, 0, 0, 0, 1,
		                                        s->z == RP_Z_NONE ? 1 : 2));
		n++;
		if (blend || lit) {
			/* context 2's blend: the lightmap's product, or the run's */
			u64 a = lmadd ? mx_gs_alpha(1, 2, 0, 0, 0) : blend_reg(s);
			if (rp_debug & 128)
				a = mx_gs_alpha(0, 2, 2, 1, 8); /* Cs * 8 / 128 + Cd */
			c = mx_e_ad(c, MX_GS_REG_ALPHA_2, a);
			n++;
		}
		}
		mx_e_giftag(gif, n, 1, -1, MX_GIF_FLG_PACKED, 1, MX_GIF_REG_AD);
		mx_e_cnt(tag, n + 1, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, 0, n + 1, MX_UNPACK_FLG));
	} else {
		c = mx_e_cnt(c, 0, mx_vif_nop(), mx_vif_stcycl(1, 1));
		c = mx_e_cnt(c, 1, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, 0, 1, MX_UNPACK_FLG));
		c = mx_e_giftag(c, 0, 1, -1, MX_GIF_FLG_PACKED, 1, MX_GIF_REG_AD);
	}
	c = mx_e_cnt(c, 0, mx_vif_nop(), mx_vif_mscal(entry));
	c = mx_e_ret(c, 0, 0, 0);
	rp_dyn_end(c);
	return c0;
}

/* Z alone is written (FRAME's mask over every bit of the colour), by context
   1, which writes it; then the registers as a frame starts with them. */
void rp_zclear(float x0, float y0, float x1, float y1) {
	struct rp_state s = {0};
	float W = (float)mx_gs.w, H = (float)mx_gs.h;
	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > W ? W : x1, y1 = y1 > H ? H : y1;
	u32 mask = rp_strip_lines(y0, y1);
	if (x1 <= x0 || y1 <= y0 || !mask)
		return;
	s.prog = RP_PROG_GS, s.z = RP_Z_NONE;
	u64 *c0 = rp_dyn(11), *c = c0;
	if (!c || rp_run(0, &s) < 0)
		return;
	u32 X0 = (u32)((2048.0f - W * 0.5f + x0) * 16.0f), Y0 = (u32)((2048.0f - H * 0.5f + y0) * 16.0f);
	u32 X1 = (u32)((2048.0f - W * 0.5f + x1) * 16.0f), Y1 = (u32)((2048.0f - H * 0.5f + y1) * 16.0f);
	c = mx_e_cnt(c, 9, mx_vif_flush(), mx_vif_direct(9));
	c = mx_e_ad_tag(c, 8, 1);
	c = mx_e_ad(c, MX_GS_REG_FRAME_1, mx_hires_frame(0xFFFFFFFFu));
	c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
	c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
	c = mx_e_ad(c, MX_GS_REG_RGBAQ, 0);
	c = mx_e_ad(c, MX_GS_REG_XYZ2, (u64)X0 | (u64)Y0 << 16);
	c = mx_e_ad(c, MX_GS_REG_XYZ2, (u64)X1 | (u64)Y1 << 16);
	c = mx_e_ad(c, MX_GS_REG_FRAME_1, mx_hires_frame(0));
	c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2));
	mx_e_ret(c, 0, 0, 0);
	rp_draw(c0, mask, 0, 0, 0);
}

/* CALLs into the record being written */
static void calls(const void **chain, u32 n) {
	while (n) {
		u32 k = n > 96 ? 96 : n;
		u64 *c = mx_frame_reserve(k);
		for (u32 i = 0; i < k; i++)
			c = mx_e_call(c, chain[i], 0, 0, 0);
		mx_frame_commit(c);
		rp_list_stats.qw += k;
		chain += k, n -= k;
	}
}

static u64 dimx(void) {
	/* the GS manual's 4 x 4 matrix */
	static const s8 m[16] = {-4, 2, -3, 3, 0, -2, 1, -1, -3, 3, -4, 2, 1, -1, 0, -2};
	u64 v = 0;
	for (int i = 0; i < 16; i++)
		v |= (u64)(m[i] & 7) << (4 * i);
	return v;
}

void rp_list_init(void) {
	if (!dyn) {
		void *p = mx_dma_alloc(DYN_QW * 16);
		dyn = p ? (u64 *)MX_UCAB((u8 *)p) : 0;
	}
	d_nfr = d_head = d_used = 0;
	nrun = ndraw = 0;
}

/* The frame's GS registers and VU1 set-up, in the record every pass plays
   before its strip's own. */
void rp_list_begin(void) {
	dyn_frame();
	nrun = ndraw = 0;
	/* what the pass before sent has left VU1's memory */
	u64 *c = mx_frame_reserve(2);
	c = mx_e_cnt(c, 0, mx_vif_flush(), mx_vif_nop());
	c = mx_e_cnt(c, 0, mx_vif_base(Q3VU_BASE), mx_vif_offset(Q3VU_BUF));
	mx_frame_commit(c);
	mx_frame_mpg(q3vu_vu, (u32)(q3vu_vu_end - q3vu_vu), 0);
	/* the packet that sets nothing, which q3clip kicks to wait for the ones before
	   it (0x7F: the GS's NOP register) */
	c = mx_frame_reserve(4);
	c = mx_e_cnt(c, 0, mx_vif_flushe(), mx_vif_stcycl(1, 1));
	c = mx_e_cnt(c, 2, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, Q3VU_CTX_NOP, 2, 0));
	c = mx_e_ad_tag(c, 1, 1);
	c = mx_e_ad(c, 0x7F, 0);
	mx_frame_commit(c);
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
	/* context 2's blend until a run sets another: Cd * As (the lightmap pass) */
	c = mx_e_ad(c, MX_GS_REG_ALPHA_2, mx_gs_alpha(1, 2, 0, 2, 0));
	mx_frame_commit(c);
}

/* Every strip's own record gets the draws that touch it (without strip
   records: all of them into the frame's record, once). */
void rp_list_end(void) {
	static const void *list[2][LIST_MAX];
	u32 nstrip = rp_view.nstrip;
	rp_list_stats.runs += nrun, rp_list_stats.draws += ndraw;
	/* the wait between runs of programs with other buffers */
	u64 *fl = rp_dyn(2);
	if (fl)
		mx_e_ret(mx_e_cnt(fl, 0, mx_vif_flush(), mx_vif_nop()), 0, 0, 0);
	for (u32 k = 0; k < (nstrip ? nstrip : 1); k++) {
		u32 bit = 1u << k;
		const void *matrix = 0;
		int family = -1;
		/* the window of lines this pass draws, for the programs that leave out
		   the triangles outside it: y * SS + w * SN within -w .. w is inside (a
		   line and a half more each way: the lower field is drawn half a line
		   lower) */
		float top = 2048.0f - (float)mx_gs.h * 0.5f, t = top - 1.5f, b = top + (float)mx_gs.h + 1.5f;
		if (nstrip)
			t = top + (float)(k * rp_view.strip_h) - 1.5f, b = top + (float)((k + 1) * rp_view.strip_h) + 1.5f;
		float l = 2048.0f - (float)mx_gs.w * 0.5f - 1.0f, r = 2048.0f + (float)mx_gs.w * 0.5f + 1.0f;
		if (rp_debug & 32768) /* a test of the window: the strip's middle third alone */
			t += (b - t) / 3, b -= (b - t) / 2;
		u64 *wn = rp_dyn(5);
		if (!wn)
			break;
		{
			u64 *c = wn;
			c = mx_e_cnt(c, 0, mx_vif_flushe(), mx_vif_stcycl(1, 1));
			c = mx_e_cnt(c, 2, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V4_32, Q3VU_CTX_SS, 2, 0));
			c = mx_e_w4(c, fbits(2.0f / (r - l)), fbits(2.0f / (b - t)), 0, 0);
			c = mx_e_w4(c, fbits(-(r + l) / (r - l)), fbits(-(b + t) / (b - t)), 0, 0);
			mx_e_ret(c, 0, 0, 0);
		}
		int windowed = 0;
		u32 at_sent = rp_list_stats.sent, at_batches = rp_list_stats.batches, at_setups = rp_list_stats.setups;
		u32 at_msent = rp_list_stats.p_sent[RP_PROG_MODEL], at_mb = rp_list_stats.p_batches[RP_PROG_MODEL];
		u32 ndr = 0;
		if (nstrip)
			mx_hires_strip((int)k);
		for (u32 r = 0; r < nrun; r++) {
			struct run *rn = &run[r];
			const struct draw *d = &draw[rn->first];
			u32 n[2] = {0, 0};
			int set = 0;
			for (u32 i = 0; i <= rn->n; i++) {
				/* a list full, or the run over: sent */
				if (i == rn->n || n[0] == LIST_MAX || n[1] == LIST_MAX) {
					for (int v = 0; v < 2; v++) {
						if (!n[v])
							continue;
						const void *pre[3];
						u32 np = 0;
						int fam = rn->s.prog == RP_PROG_MODEL;
						if (rn->s.prog != RP_PROG_GS) {
							if (family >= 0 && family != fam && fl)
								pre[np++] = fl;
							family = fam;
						}
						if (!windowed) {
							const void *w1 = wn;
							calls(&w1, 1);
							windowed = 1;
						}
						if (rn->matrix && matrix != rn->matrix)
							pre[np++] = matrix = rn->matrix;
						if (!rn->setup[v][!set])
							rn->setup[v][!set] = setup_chain(&rn->s, v, !set);
						if (!rn->setup[v][!set])
							break; /* no memory: the run is left out */
						pre[np++] = rn->setup[v][!set];
						calls(pre, np);
						calls(list[v], n[v]);
						/* rp_debug 131072: the list again, its vertices as XYZ3: VIF1 and VU1
						   work twice, the GS draws once (do they wait for each other?) */
						int again = 0;
						if ((rp_debug & 131072) && rn->s.prog != RP_PROG_GS) {
							if (!rn->ghost[v]) {
								ghost = 1;
								rn->ghost[v] = setup_chain(&rn->s, v, 1);
								ghost = 0;
							}
							if (rn->ghost[v]) {
								calls(&rn->ghost[v], 1);
								calls(list[v], n[v]);
								again = 1; /* the context's tags are the ghost's: the next list sets its own */
							}
						}
						rp_list_stats.setups++;
						rp_list_stats.p_setups[rn->s.prog]++;
						set = !again, n[v] = 0;
					}
					n[0] = n[1] = 0;
					if (i == rn->n)
						break;
				}
				if (d[i].mask & bit) {
					list[d[i].clip][n[d[i].clip]++] = d[i].chain;
					ndr++;
					rp_list_stats.sent += d[i].verts, rp_list_stats.batches += d[i].batches;
					rp_list_stats.p_sent[rn->s.prog] += d[i].verts, rp_list_stats.p_batches[rn->s.prog] += d[i].batches;
					if (d[i].clip)
						rp_list_stats.p_clip[rn->s.prog] += d[i].verts;
				}
			}
		}
		rp_list_stats.k_sent[k] = rp_list_stats.sent - at_sent;
		rp_list_stats.k_batches[k] = rp_list_stats.batches - at_batches;
		rp_list_stats.k_setups[k] = rp_list_stats.setups - at_setups;
		rp_list_stats.k_msent[k] = rp_list_stats.p_sent[RP_PROG_MODEL] - at_msent;
		rp_list_stats.k_mbatches[k] = rp_list_stats.p_batches[RP_PROG_MODEL] - at_mb;
		rp_list_stats.k_draws[k] = ndr;
	}
	if (nstrip)
		mx_hires_strip(-1);
	nrun = ndraw = 0;
}
