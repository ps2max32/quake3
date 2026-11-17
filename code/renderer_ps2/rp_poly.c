/* rp_poly.c: polygons and pictures built on the EE (PLAN-PS2.md, 5.3: the
   game's polys, sprites, beams and marks; 2D).

   A polygon's vertices go to VU1 as a map's chunk does (tools/ps2map.py: a
   batch of the world's programs, positions and texture coordinates as
   16-bit fixed point), so they are drawn, and cut at the near plane and the
   guard band, by the same programs. Their positions are the view's own
   coordinates (left, up, forward from the eye) in sixteenths of a unit, or
   in whole units when one is farther than that reaches: two matrices a
   view. A batch is a chain of its own in the frame's memory, a draw of the
   strips its polygons touch.

   A picture is a GS sprite, two vertices of ST, RGBAQ and XYZ2, sent as it
   is (PATH2). */

#include <mx/frame.h>
#include <mx/gif.h>
#include <mx/mx.h>
#include <mx/vif.h>

#include "rp_poly.h"
#include "vu/q3vu.h"

struct rp_poly_stats rp_poly_stats;

#define W_NONE 0xA000 /* tools/ps2map.py: the vertex ends no triangle */
#define NEAR_UNIT (1.0f / 16)
#define NEAR_MAX 2000.0f
#define FAR_MAX 32000.0f

static inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* ---- polygons ---------------------------------------------------------------------------- */

static struct rp_state pstate;
static int prun = -1, prun_far;
static u32 pview;                /* the view the matrices are of */
static const void *pmatrix[2];   /* sixteenths, whole units */
/* the batch being gathered: of one kind (far, cut) and one set of strips */
static struct {
	s16 st[Q3VU_BATCH][3];
	u32 col[Q3VU_BATCH];
	s16 pos[Q3VU_BATCH][4];
	u32 n, mask;
	u8 far, cut;
} pb;

static void poly_batch(void) {
	u32 n = pb.n;
	if (!n)
		return;
	pb.n = 0;
	if (pview != rp_view.serial)
		pview = rp_view.serial, pmatrix[0] = pmatrix[1] = 0, prun = -1;
	if (!pmatrix[pb.far]) {
		const float axis[3][3] = {{rp_view.L[0], rp_view.L[1], rp_view.L[2]},
		                          {rp_view.U[0], rp_view.U[1], rp_view.U[2]},
		                          {rp_view.F[0], rp_view.F[1], rp_view.F[2]}};
		pmatrix[pb.far] = rp_matrix(pb.far ? 1.0f : NEAR_UNIT, rp_view.org, axis);
	}
	if (prun < 0 || prun != rp_run_last() || prun_far != pb.far) {
		prun = rp_run(pmatrix[pb.far], &pstate), prun_far = pb.far;
		if (prun < 0)
			return;
	}
	/* an even count (the programs take two vertices a turn): the last one
	   again, ending no triangle */
	if (n & 1) {
		pb.st[n][0] = pb.st[n - 1][0], pb.st[n][1] = pb.st[n - 1][1], pb.st[n][2] = 4096;
		pb.col[n] = pb.col[n - 1];
		pb.pos[n][0] = pb.pos[n - 1][0], pb.pos[n][1] = pb.pos[n - 1][1], pb.pos[n][2] = pb.pos[n - 1][2];
		pb.pos[n][3] = (s16)W_NONE;
		n++;
	}
	/* the batch as VIF1 takes it: 32-bit words */
	u32 stw = (n * 6 + 3) / 4, words = 1 + 1 + stw + 1 + n + 1 + n * 2 + 2;
	u32 qw = (words + 3) / 4;
	u64 *c0 = rp_dyn(qw + 2);
	if (!c0)
		return;
	u32 *w = (u32 *)mx_e_cnt(c0, qw, 0, 0), *end = w + qw * 4;
	*w++ = mx_vif_stcycl(3, 1);
	*w++ = mx_vif_unpack(MX_UNPACK_V3_16, Q3VU_A, n, MX_UNPACK_FLG);
	const u16 *sp = (const u16 *)pb.st;
	for (u32 i = 0; i < stw; i++)
		*w++ = (u32)sp[2 * i] | (u32)(2 * i + 1 < n * 3 ? sp[2 * i + 1] : 0) << 16;
	*w++ = mx_vif_unpack(MX_UNPACK_V4_8, Q3VU_A + 1, n, MX_UNPACK_FLG | MX_UNPACK_USN);
	for (u32 i = 0; i < n; i++)
		*w++ = pb.col[i];
	*w++ = mx_vif_unpack(MX_UNPACK_V4_16, Q3VU_A + 2, n, MX_UNPACK_FLG);
	const u32 *pp = (const u32 *)pb.pos;
	for (u32 i = 0; i < n * 2; i++)
		*w++ = pp[i];
	*w++ = mx_vif_itop(n);
	*w++ = mx_vif_mscnt();
	while (w < end)
		*w++ = mx_vif_nop();
	mx_e_ret((u64 *)end, 0, 0, 0);
	rp_draw(c0, pb.mask, pb.cut, n, 1);
}

void rp_poly_state(const struct rp_state *s) {
	rp_poly_flush();
	pstate = *s;
	pstate.prog = RP_PROG_WORLD, pstate.lm_ok = 0;
	prun = -1;
}

void rp_poly(const struct rp_vert *v, u32 n) {
	const struct rp_view *vw = &rp_view;
	float cam[16][3];
	if (n < 3 || n > 16)
		return;
	/* the vertices from the eye: left, up, forward; where they are against
	   the frustum (all outside one plane: not seen) and the guard band (one
	   outside: the programs that cut); the lines it covers */
	float txl = (vw->cx - 2048.0f + (float)mx_gs.w * 0.5f) / vw->fx, txr = (2048.0f + (float)mx_gs.w * 0.5f - vw->cx) / vw->fx;
	float tyt = (vw->cy - 2048.0f + (float)mx_gs.h * 0.5f) / vw->fy, tyb = (2048.0f + (float)mx_gs.h * 0.5f - vw->cy) / vw->fy;
	float ox = vw->cx - 2048.0f, oy = vw->cy - 2048.0f;
	float gx = (2040.0f - (ox < 0 ? -ox : ox)) / vw->fx, gy = (2040.0f - (oy < 0 ? -oy : oy)) / vw->fy;
	u32 out = 31, cut = 0;
	float big = 0, lo = 1e9f, hi = -1e9f, top = vw->cy - 2048.0f + (float)mx_gs.h * 0.5f;
	for (u32 i = 0; i < n; i++) {
		float d[3] = {v[i].xyz[0] - vw->org[0], v[i].xyz[1] - vw->org[1], v[i].xyz[2] - vw->org[2]};
		float x = dot3(vw->L, d), y = dot3(vw->U, d), w = dot3(vw->F, d);
		cam[i][0] = x, cam[i][1] = y, cam[i][2] = w;
		out &= (u32)(w < 4.0f) | (u32)(x > txl * w) << 1 | (u32)(-x > txr * w) << 2 | (u32)(y > tyt * w) << 3 |
		       (u32)(-y > tyb * w) << 4;
		float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, aw = w < 0 ? -w : w;
		cut |= w < 4.5f || ax > gx * w || ay > gy * w;
		big = ax > big ? ax : big, big = ay > big ? ay : big, big = aw > big ? aw : big;
		if (w >= 4.5f) {
			float py = top - vw->fy * y / w;
			lo = py < lo ? py : lo, hi = py > hi ? py : hi;
		}
	}
	if (out || big > FAR_MAX) {
		rp_poly_stats.culled++;
		return;
	}
	u32 far = big > NEAR_MAX;
	u32 mask = cut ? vw->all : rp_strip_lines(lo, hi);
	if (!mask) {
		rp_poly_stats.culled++;
		return;
	}
	if (pb.n && (pb.far != far || pb.cut != cut || pb.mask != mask || pb.n + n > Q3VU_BATCH - 1))
		poly_batch();
	pb.far = (u8)far, pb.cut = (u8)cut, pb.mask = mask;
	rp_poly_stats.polys++, rp_poly_stats.verts += n, rp_poly_stats.cut += cut;
	/* as a strip: 0, 1, n - 1, 2, n - 2, ... */
	float unit = far ? 1.0f : 1.0f / NEAR_UNIT;
	u32 a = 1, b = n - 1;
	for (u32 k = 0; k < n; k++) {
		u32 i = k == 0 ? 0 : (k & 1) ? a++ : b--;
		u32 j = pb.n++;
		float s = v[i].st[0] * 4096.0f, t = v[i].st[1] * 4096.0f;
		s = s > 32767.0f ? 32767.0f : s < -32767.0f ? -32767.0f : s;
		t = t > 32767.0f ? 32767.0f : t < -32767.0f ? -32767.0f : t;
		pb.st[j][0] = (s16)s, pb.st[j][1] = (s16)t, pb.st[j][2] = 4096;
		pb.col[j] = (u32)v[i].rgba[0] | (u32)v[i].rgba[1] << 8 | (u32)v[i].rgba[2] << 16 | (u32)v[i].rgba[3] << 24;
		pb.pos[j][0] = (s16)(cam[i][0] * unit), pb.pos[j][1] = (s16)(cam[i][1] * unit), pb.pos[j][2] = (s16)(cam[i][2] * unit);
		pb.pos[j][3] = k < 2 ? (s16)W_NONE : 0;
	}
}

/* ---- pictures ------------------------------------------------------------------------------ */

#define PIC_MAX 64
static struct rp_state cstate;
static int crun = -1;
static struct {
	u64 qw[PIC_MAX * 6][2];
	u32 n, mask;
} cb;

static void pic_batch(void) {
	u32 n = cb.n;
	if (!n)
		return;
	cb.n = 0;
	if (crun < 0 || crun != rp_run_last()) {
		crun = rp_run(0, &cstate);
		if (crun < 0)
			return;
	}
	u64 *c0 = rp_dyn(n * 6 + 3), *c = c0;
	if (!c)
		return;
	c = mx_e_cnt(c, n * 6 + 1, mx_vif_nop(), mx_vif_direct(n * 6 + 1));
	/* PRIM is the state's (PRE 0) */
	c = mx_e_qw(c, mx_giftag(n * 2, 1, -1, MX_GIF_FLG_PACKED, 3), 0x512);
	for (u32 i = 0; i < n * 6; i++)
		c = mx_e_qw(c, cb.qw[i][0], cb.qw[i][1]);
	mx_e_ret(c, 0, 0, 0);
	rp_draw(c0, cb.mask, 0, 0, 0);
}

void rp_pic_state(const struct rp_state *s) {
	rp_poly_flush();
	cstate = *s;
	cstate.prog = RP_PROG_GS, cstate.z = RP_Z_NONE, cstate.lm_ok = 0;
	crun = -1;
}

static u32 fbits(float f) {
	union {
		float f;
		u32 u;
	} v = {f};
	return v.u;
}

void rp_pic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, u32 rgba) {
	u32 mask = rp_strip_lines(y, y + h);
	float W = (float)mx_gs.w, H = (float)mx_gs.h;
	if (!mask || x >= W || y >= H || x + w <= 0 || y + h <= 0)
		return;
	if (cb.n && (cb.mask != mask || cb.n == PIC_MAX))
		pic_batch();
	cb.mask = mask;
	u64 (*q)[2] = &cb.qw[cb.n++ * 6];
	u32 one = fbits(1.0f);
	u64 col = (u64)(rgba & 0xFF) | (u64)(rgba >> 8 & 0xFF) << 32, col2 = (u64)(rgba >> 16 & 0xFF) | (u64)(rgba >> 24) << 32;
	float x0 = 2048.0f - W * 0.5f + x, y0 = 2048.0f - H * 0.5f + y;
	q[0][0] = (u64)fbits(s1) | (u64)fbits(t1) << 32, q[0][1] = one;
	q[1][0] = col, q[1][1] = col2;
	q[2][0] = (u64)(u32)(s32)(x0 * 16.0f) | (u64)(u32)(s32)(y0 * 16.0f) << 32, q[2][1] = 0;
	q[3][0] = (u64)fbits(s2) | (u64)fbits(t2) << 32, q[3][1] = one;
	q[4][0] = col, q[4][1] = col2;
	q[5][0] = (u64)(u32)(s32)((x0 + w) * 16.0f) | (u64)(u32)(s32)((y0 + h) * 16.0f) << 32, q[5][1] = 0;
	rp_poly_stats.pics++;
}

void rp_poly_flush(void) {
	poly_batch();
	pic_batch();
}
