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
static u32 pstrips = ~0u;
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
	pstrips = ~0u;
}

void rp_poly_strips(u32 mask) { pstrips = mask; }

/* what the tests of a polygon take from the view: once a view. The
   vectors: the same numbers in every lane, for four vertices at a time on
   VU0 (the compiler's vectors are its registers, -mvu0). */
typedef float v4f __attribute__((vector_size(16), aligned(16)));
typedef float v4fa __attribute__((vector_size(16), aligned(16), may_alias));
typedef int v4i __attribute__((vector_size(16), aligned(16)));
typedef u32 u32a __attribute__((may_alias));
static struct {
	u32 serial, w, h;
	float left, top;          /* the picture's corner from the view's centre */
	float txl, txr, tyt, tyb; /* the frustum's sides: x and y a unit of depth */
	float gx, gy;             /* the guard band's */
	v4f o[3], l[3], u[3], f[3];
	v4f vtxl, vtxr, vtyt, vtyb, vnear, vone, vfx, vfy, vleft, vtop;
} pv = {.serial = ~0u};

static inline v4f all4(float a) { return (v4f){a, a, a, a}; }

static void pv_set(void) {
	const struct rp_view *vw = &rp_view;
	if (pv.serial == vw->serial && pv.w == mx_gs.w && pv.h == mx_gs.h)
		return;
	float W = (float)mx_gs.w, H = (float)mx_gs.h;
	pv.serial = vw->serial, pv.w = mx_gs.w, pv.h = mx_gs.h;
	pv.left = vw->cx - 2048.0f + W * 0.5f, pv.top = vw->cy - 2048.0f + H * 0.5f;
	pv.txl = pv.left / vw->fx, pv.txr = (W - pv.left) / vw->fx, pv.tyt = pv.top / vw->fy, pv.tyb = (H - pv.top) / vw->fy;
	float ox = vw->cx - 2048.0f, oy = vw->cy - 2048.0f;
	pv.gx = (rp_guard_x - (ox < 0 ? -ox : ox)) / vw->fx, pv.gy = (rp_guard_y - (oy < 0 ? -oy : oy)) / vw->fy;
	for (int a = 0; a < 3; a++)
		pv.o[a] = all4(vw->org[a]), pv.l[a] = all4(vw->L[a]), pv.u[a] = all4(vw->U[a]), pv.f[a] = all4(vw->F[a]);
	pv.vtxl = all4(pv.txl), pv.vtxr = all4(pv.txr), pv.vtyt = all4(pv.tyt), pv.vtyb = all4(pv.tyb);
	pv.vnear = all4(4.5f), pv.vone = all4(1.0f), pv.vfx = all4(vw->fx), pv.vfy = all4(vw->fy);
	pv.vleft = all4(pv.left), pv.vtop = all4(pv.top);
}

/* a bit a lane: the number is below 0 */
static inline u32 signs(v4f x) {
	v4i i = (v4i)x;
	return (u32)i[0] >> 31 | (u32)i[1] >> 31 << 1 | (u32)i[2] >> 31 << 2 | (u32)i[3] >> 31 << 3;
}

float rp_poly_area(const struct rp_vert *v, u32 n, rp_cam4 *keep) {
	const struct rp_view *vw = &rp_view;
	float W = (float)mx_gs.w, H = (float)mx_gs.h;
	float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
	rp_cam4 own[4];
	rp_cam4 *cam = keep ? keep : own;
	/* the vertices on the screen (17: one more where the eye's plane cuts) */
	float pxy[2][20] __attribute__((aligned(16)));
	float *px = pxy[0], *py = pxy[1];
	u32 m = 0, out = 31, behind = 0;
	if (n < 3 || n > 16)
		return 0;
	pv_set();
	/* Four vertices at a time (the last ones again where n is no multiple of
	   four): from the eye, against the view's sides (all of it outside one:
	   none, most of a level's marks), and onto the screen. The numbers go to
	   VU0 and back through memory, never through the FPU. */
	if (rp_debug & 8388608) {
		/* the same by the FPU, a vertex at a time (for comparing) */
		for (u32 i = 0; i < n; i++) {
			float d[3] = {v[i].xyz[0] - vw->org[0], v[i].xyz[1] - vw->org[1], v[i].xyz[2] - vw->org[2]};
			float x = dot3(vw->L, d), y = dot3(vw->U, d), w = dot3(vw->F, d);
			RP_CAM(cam, i, 0) = x, RP_CAM(cam, i, 1) = y, RP_CAM(cam, i, 2) = w;
			out &= (u32)(w < 4.5f) | (u32)(x > pv.txl * w) << 1 | (u32)(-x > pv.txr * w) << 2 | (u32)(y > pv.tyt * w) << 3 |
			       (u32)(-y > pv.tyb * w) << 4;
			behind |= w < 4.5f;
			if (!behind) {
				float iw = 1.0f / w;
				px[i] = pv.left - vw->fx * x * iw, py[i] = pv.top - vw->fy * y * iw;
			}
		}
	} else
	for (u32 k = 0; 4 * k < n; k++) {
		float in[3][4] __attribute__((aligned(16)));
		for (u32 j = 0; j < 4; j++) {
			u32 i = 4 * k + j < n ? 4 * k + j : n - 1;
			const u32a *p = (const u32a *)v[i].xyz;
			((u32a *)in[0])[j] = p[0], ((u32a *)in[1])[j] = p[1], ((u32a *)in[2])[j] = p[2];
		}
		v4f dx = *(const v4fa *)in[0] - pv.o[0], dy = *(const v4fa *)in[1] - pv.o[1], dz = *(const v4fa *)in[2] - pv.o[2];
		v4f x = dx * pv.l[0] + dy * pv.l[1] + dz * pv.l[2];
		v4f y = dx * pv.u[0] + dy * pv.u[1] + dz * pv.u[2];
		v4f w = dx * pv.f[0] + dy * pv.f[1] + dz * pv.f[2];
		*(v4fa *)cam[k][0] = x, *(v4fa *)cam[k][1] = y, *(v4fa *)cam[k][2] = w;
		u32 sw = signs(w - pv.vnear);
		out &= (u32)(sw == 15) | (u32)(signs(pv.vtxl * w - x) == 15) << 1 | (u32)(signs(pv.vtxr * w + x) == 15) << 2 |
		       (u32)(signs(pv.vtyt * w - y) == 15) << 3 | (u32)(signs(pv.vtyb * w + y) == 15) << 4;
		behind |= sw;
		if (!behind) {
			v4f iw = pv.vone / w;
			*(v4fa *)&pxy[0][4 * k] = pv.vleft - pv.vfx * x * iw;
			*(v4fa *)&pxy[1][4 * k] = pv.vtop - pv.vfy * y * iw;
		}
	}
	if (out)
		return 0;
	if (!behind)
		m = n;
	else {
		/* across the eye's plane (few): the part of it before the plane, on the screen */
		float left = pv.left, top = pv.top;
		for (u32 i = 0, j = n - 1; i < n; j = i++) {
			float a[3] = {RP_CAM(cam, j, 0), RP_CAM(cam, j, 1), RP_CAM(cam, j, 2)};
			float b[3] = {RP_CAM(cam, i, 0), RP_CAM(cam, i, 1), RP_CAM(cam, i, 2)};
			int ain = a[2] >= 4.5f, bin = b[2] >= 4.5f;
			if (ain != bin) {
				float t = (4.5f - a[2]) / (b[2] - a[2]), iw = 1.0f / 4.5f;
				px[m] = left - vw->fx * (a[0] + t * (b[0] - a[0])) * iw, py[m] = top - vw->fy * (a[1] + t * (b[1] - a[1])) * iw;
				m++;
			}
			if (bin) {
				float iw = 1.0f / b[2];
				px[m] = left - vw->fx * b[0] * iw, py[m] = top - vw->fy * b[1] * iw;
				m++;
			}
		}
		if (m < 3)
			return 0;
	}
	float a = 0;
	for (u32 i = 0, j = m - 1; i < m; j = i++) {
		a += px[j] * py[i] - px[i] * py[j];
		x0 = px[i] < x0 ? px[i] : x0, x1 = px[i] > x1 ? px[i] : x1;
		y0 = py[i] < y0 ? py[i] : y0, y1 = py[i] > y1 ? py[i] : y1;
	}
	float bx0 = x0 < 0 ? 0 : x0, bx1 = x1 > W ? W : x1, by0 = y0 < 0 ? 0 : y0, by1 = y1 > H ? H : y1;
	if (bx1 <= bx0 || by1 <= by0)
		return 0;
	float box = (bx1 - bx0) * (by1 - by0), full = (x1 - x0) * (y1 - y0);
	a = (a < 0 ? -a : a) * 0.5f;
	/* the part of it on the screen: as its box's part */
	if (full > box && full > 0)
		a *= box / full;
	return a < box ? a : box;
}

void rp_poly(const struct rp_vert *v, u32 n) {
	const struct rp_view *vw = &rp_view;
	rp_cam4 cam[4];
	if (n < 3 || n > 16)
		return;
	for (u32 i = 0; i < n; i++) {
		float d[3] = {v[i].xyz[0] - vw->org[0], v[i].xyz[1] - vw->org[1], v[i].xyz[2] - vw->org[2]};
		RP_CAM(cam, i, 0) = dot3(vw->L, d), RP_CAM(cam, i, 1) = dot3(vw->U, d), RP_CAM(cam, i, 2) = dot3(vw->F, d);
	}
	rp_poly_cam(v, cam, n);
}

void rp_poly_cam(const struct rp_vert *v, const rp_cam4 *cam, u32 n) {
	const struct rp_view *vw = &rp_view;
	if (n < 3 || n > 16)
		return;
	pv_set();
	/* the vertices from the eye (left, up, forward): where they are against
	   the frustum (all outside one plane: not seen) and the guard band (one
	   outside: the programs that cut); the lines it covers */
	float txl = pv.txl, txr = pv.txr, tyt = pv.tyt, tyb = pv.tyb, gx = pv.gx, gy = pv.gy, top = pv.top;
	u32 out = 31, cut = 0;
	float big = 0, lo = 1e9f, hi = -1e9f;
	for (u32 i = 0; i < n; i++) {
		float x = RP_CAM(cam, i, 0), y = RP_CAM(cam, i, 1), w = RP_CAM(cam, i, 2);
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
	u32 mask = (cut ? vw->all : rp_strip_lines(lo, hi)) & pstrips;
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
		pb.pos[j][0] = (s16)(RP_CAM(cam, i, 0) * unit), pb.pos[j][1] = (s16)(RP_CAM(cam, i, 1) * unit),
		pb.pos[j][2] = (s16)(RP_CAM(cam, i, 2) * unit);
		pb.pos[j][3] = k < 2 ? (s16)W_NONE : 0;
	}
}

/* ---- pictures ------------------------------------------------------------------------------ */

#define PIC_MAX 64
static struct rp_state cstate;
static int crun = -1;
/* The batch being written and what a picture takes from the view, in one
   cache line of the core's fast part: between two pictures the game's own
   data has had the cache, and each line of the renderer's that a picture
   reads is fetched again. A picture goes straight into the frame's memory
   (written, not read: no line is fetched for it). */
static struct {
	u64 *c0, *c;        /* the batch's chain; the next picture's place */
	u32 n, mask;
	float ly, lh;       /* the last picture's lines and their strips (a line of text) */
	u32 lmask;
	float x0, y0, W, H; /* the picture's corner in the GS's coordinates; its size */
	float strip_inv;
	s32 nstrip;
} pc MX_FAST_BSS MX_ALIGNED(64);

void rp_pic_view(void) {
	pc.W = (float)mx_gs.w, pc.H = (float)mx_gs.h;
	pc.x0 = 2048.0f - pc.W * 0.5f, pc.y0 = 2048.0f - pc.H * 0.5f;
	pc.strip_inv = rp_view.strip_inv, pc.nstrip = (s32)rp_view.nstrip;
	pc.lh = -1.0f;
}

static void pic_batch(void) {
	u32 n = pc.n;
	if (!n)
		return;
	pc.n = 0;
	u64 *c0 = pc.c0, *end = pc.c;
	mx_e_cnt(c0, n * 6 + 1, mx_vif_nop(), mx_vif_direct(n * 6 + 1));
	/* PRIM is the state's (PRE 0) */
	mx_e_qw(c0 + 2, mx_giftag(n * 2, 1, -1, MX_GIF_FLG_PACKED, 3), 0x512);
	/* what the batch did not take goes back, if nothing else took memory since */
	rp_dyn_end(mx_e_ret(end, 0, 0, 0));
	rp_poly_stats.pics += n;
	if (crun < 0 || crun != rp_run_last())
		crun = rp_run(0, &cstate);
	if (crun >= 0)
		rp_draw(c0, pc.mask, 0, 0, 0);
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
	/* the strips of its lines (rp_strip_lines), as the picture before when it is on the same */
	u32 mask = pc.lmask;
	if (y != pc.ly || h != pc.lh) {
		float lo = y, hi = y + h;
		if (!pc.nstrip)
			mask = 1;
		else if (hi < 0 || lo > pc.H)
			mask = 0;
		else {
			s32 k0 = lo < 1.5f ? 0 : (s32)((lo - 1.5f) * pc.strip_inv), k1 = (s32)((hi + 1.5f) * pc.strip_inv);
			k1 = k1 >= pc.nstrip ? pc.nstrip - 1 : k1, k0 = k0 >= pc.nstrip ? pc.nstrip - 1 : k0;
			mask = k1 < k0 ? 0 : ((2u << k1) - 1) & ~((1u << k0) - 1);
		}
		pc.ly = y, pc.lh = h, pc.lmask = mask;
	}
	if (!mask || x >= pc.W || y >= pc.H || x + w <= 0 || y + h <= 0)
		return;
	if (pc.n && (pc.mask != mask || pc.n == PIC_MAX))
		pic_batch();
	if (!pc.n) {
		/* a batch's memory at most; what it leaves is given back at its end */
		pc.c0 = rp_dyn(PIC_MAX * 6 + 3);
		if (!pc.c0)
			return;
		pc.c = pc.c0 + 4, pc.mask = mask;
	}
	u64 *q = pc.c;
	u64 one = fbits(1.0f);
	u64 col = (u64)(rgba & 0xFF) | (u64)(rgba >> 8 & 0xFF) << 32, col2 = (u64)(rgba >> 16 & 0xFF) | (u64)(rgba >> 24) << 32;
	float x0 = pc.x0 + x, y0 = pc.y0 + y;
	q[0] = (u64)fbits(s1) | (u64)fbits(t1) << 32, q[1] = one;
	q[2] = col, q[3] = col2;
	q[4] = (u64)(u32)(s32)(x0 * 16.0f) | (u64)(u32)(s32)(y0 * 16.0f) << 32, q[5] = 0;
	q[6] = (u64)fbits(s2) | (u64)fbits(t2) << 32, q[7] = one;
	q[8] = col, q[9] = col2;
	q[10] = (u64)(u32)(s32)((x0 + w) * 16.0f) | (u64)(u32)(s32)((y0 + h) * 16.0f) << 32, q[11] = 0;
	pc.c = q + 12, pc.n++;
}

void rp_poly_flush(void) {
	poly_batch();
	pic_batch();
}
