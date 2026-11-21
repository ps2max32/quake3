/* rp_sky.c: the map's sky, as the engine draws it (renderergl1/tr_sky.c):
   a box around the eye, each side 8 x 8 quads; its far box's images on the
   six sides, then each cloud layer (a stage of the shader) on all sides but
   the bottom, its texture coordinates where the side's direction meets a
   sphere of the clouds' height over the world (in radians, through the
   stage's tcMods).

   The quads go the polygons' way (rp_poly.c) after the world's surfaces
   that write depth, behind all of them and with the depth test: the sky is
   drawn where nothing else was, which is where the map's sky surfaces are
   (they are not drawn). Of a strip only the part is drawn
   that its sky surfaces' boxes cover (rp_world_sky_rect), the quads cut to
   it: a layer over the whole picture costs the GS as much hidden as seen. */

#include <math.h>
#include <string.h>

#include <mx/frame.h>

#include "rp_list.h"
#include "rp_tex.h"
#include "rp_poly.h"
#include "rp_sky.h"
#include "rp_world.h"

#define SUB 8
#define HALF (SUB / 2)
#define BOX 16000.0f        /* behind all of a map: its depth is the farthest drawn */
#define RADIUS_WORLD 4096.0f

static float pts[6][SUB + 1][SUB + 1][3] MX_FAST_BSS;
static float box_st[6][SUB + 1][SUB + 1][2] MX_FAST_BSS;
static float cloud_st[6][SUB + 1][SUB + 1][2] MX_FAST_BSS;
static float made_for = -1;

/* the engine's MakeSkyVec: side axis, s and t in -1 .. 1 */
static void sky_vec(float s, float t, int axis, float *st, float *xyz) {
	static const int to_vec[6][3] = {{3, -1, 2}, {-3, 1, 2}, {1, 3, 2}, {-1, -3, 2}, {-2, -1, 3}, {2, -1, -3}};
	float b[3] = {s * BOX, t * BOX, BOX};
	for (int j = 0; j < 3; j++) {
		int k = to_vec[axis][j];
		xyz[j] = k < 0 ? -b[-k - 1] : b[k - 1];
	}
	st[0] = (s + 1) * 0.5f, st[1] = 1.0f - (t + 1) * 0.5f;
}

static void make(float height) {
	made_for = height;
	for (int i = 0; i < 6; i++)
		for (int t = 0; t <= SUB; t++)
			for (int s = 0; s <= SUB; s++) {
				float *v = pts[i][t][s];
				sky_vec((float)(s - HALF) / HALF, (float)(t - HALF) / HALF, i, box_st[i][t][s], v);
				/* where the direction meets the clouds' sphere (the engine's
				   R_InitSkyTexCoords) */
				float h = height, r = RADIUS_WORLD, d2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
				float p = (1.0f / (2 * d2)) *
				          (-2 * v[2] * r + 2 * sqrtf(v[2] * v[2] * r * r + 2 * d2 * r * h + d2 * h * h));
				float w[3] = {v[0] * p, v[1] * p, v[2] * p + r};
				float n = 1.0f / sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
				cloud_st[i][t][s][0] = acosf(w[0] * n);
				cloud_st[i][t][s][1] = acosf(w[1] * n);
			}
}

/* the part of the picture a strip's sky is drawn in: four planes through
   the eye (n . p >= 0 inside, p from the eye) */
static float clip[4][3];

static void clip_set(const float *r) {
	const struct rp_view *v = &rp_view;
	float cxs = v->cx - 2048.0f + (float)mx_gs.w * 0.5f, cys = v->cy - 2048.0f + (float)mx_gs.h * 0.5f;
	for (int k = 0; k < 3; k++) {
		clip[0][k] = (cxs - r[0]) * v->F[k] - v->fx * v->L[k]; /* x >= x0 */
		clip[1][k] = v->fx * v->L[k] - (cxs - r[1]) * v->F[k]; /* x <= x1 */
		clip[2][k] = (cys - r[2]) * v->F[k] - v->fy * v->U[k]; /* y >= y0 */
		clip[3][k] = v->fy * v->U[k] - (cys - r[3]) * v->F[k]; /* y <= y1 */
	}
}

/* a polygon of n vertices (xyz from the eye) cut to the part; its vertices
   into out (16), their number */
static u32 clip_poly(const struct rp_vert *in, u32 n, struct rp_vert *out) {
	struct rp_vert tmp[2][16];
	const struct rp_vert *src = in;
	for (int p = 0; p < 4 && n >= 3; p++) {
		struct rp_vert *dst = p == 3 ? out : tmp[p & 1];
		float d[16];
		u32 m = 0, outside = 0;
		for (u32 i = 0; i < n; i++) {
			d[i] = clip[p][0] * src[i].xyz[0] + clip[p][1] * src[i].xyz[1] + clip[p][2] * src[i].xyz[2];
			outside += d[i] < 0;
		}
		if (outside == n)
			return 0;
		for (u32 i = 0; i < n && m < 15; i++) {
			u32 j = i + 1 == n ? 0 : i + 1;
			if (d[i] >= 0)
				dst[m++] = src[i];
			if ((d[i] < 0) != (d[j] < 0)) {
				float f = d[i] / (d[i] - d[j]);
				struct rp_vert *q = &dst[m++];
				*q = src[i];
				for (int c = 0; c < 3; c++)
					q->xyz[c] = src[i].xyz[c] + f * (src[j].xyz[c] - src[i].xyz[c]);
				q->st[0] = src[i].st[0] + f * (src[j].st[0] - src[i].st[0]);
				q->st[1] = src[i].st[1] + f * (src[j].st[1] - src[i].st[1]);
			}
		}
		n = m, src = dst;
	}
	return n >= 3 ? n : 0;
}

/* the quads of side i, their texture coordinates st through m (6 floats,
   or 0), the colour col: what of them is in the part set */
static void side(int i, float (*st)[SUB + 1][2], const float *m, const u8 *col) {
	const float *org = rp_view.org;
	for (int t = 0; t < SUB; t++)
		for (int s = 0; s < SUB; s++) {
			static const int cs[4] = {0, 1, 1, 0}, ct[4] = {0, 0, 1, 1};
			struct rp_vert v[4], c[16];
			/* all of it outside one plane: not in the part */
			u32 out = 15;
			for (int k = 0; k < 4; k++) {
				const float *p = pts[i][t + ct[k]][s + cs[k]];
				for (int a = 0; a < 4; a++)
					if (clip[a][0] * p[0] + clip[a][1] * p[1] + clip[a][2] * p[2] >= 0)
						out &= ~(1u << a);
			}
			if (out)
				continue;
			for (int k = 0; k < 4; k++) {
				const float *p = pts[i][t + ct[k]][s + cs[k]], *a = st[t + ct[k]][s + cs[k]];
				for (int x = 0; x < 3; x++)
					v[k].xyz[x] = p[x];
				v[k].st[0] = m ? m[0] * a[0] + m[1] * a[1] + m[2] : a[0];
				v[k].st[1] = m ? m[3] * a[0] + m[4] * a[1] + m[5] : a[1];
				for (int x = 0; x < 4; x++)
					v[k].rgba[x] = col[x];
			}
			/* a texture repeats: whole units off, so that a scroll that grows
			   with time stays within the 16-bit coordinates */
			if (m) {
				float fs = floorf(v[0].st[0]), ft = floorf(v[0].st[1]);
				for (int k = 0; k < 4; k++)
					v[k].st[0] -= fs, v[k].st[1] -= ft;
			}
			u32 n = clip_poly(v, 4, c);
			if (!n)
				continue;
			for (u32 k = 0; k < n; k++)
				for (int x = 0; x < 3; x++)
					c[k].xyz[x] += org[x];
			rp_poly(c, n);
		}
}

/* a state's quads of sides 0 .. nside - 1, in each strip's part */
static void layer(const struct rp_state *s, u32 strips, int nside, int box, const float *m, const u8 *col) {
	rp_poly_state(s);
	for (u32 k = 0; k < 16; k++) {
		float r[4];
		if (!(strips >> k & 1) || !rp_world_sky_rect(k, r))
			continue;
		/* a pixel more each way: the lower field lies half a line lower */
		r[0] -= 2, r[1] += 2, r[2] -= 2, r[3] += 2;
		clip_set(r);
		rp_poly_strips(1u << k);
		if (box >= 0)
			side(box, box_st[box], NULL, col);
		else
			for (int i = 0; i < nside; i++)
				side(i, cloud_st[i], m, col);
	}
}

void rp_sky_draw(const struct rp_shader *sh, float t, u32 strips) {
	float h = sh->cloud > 0 ? sh->cloud : 512.0f;
	if (h != made_for)
		make(h);
	struct rp_state s;
	/* the far box: its images clamped, no blend */
	static const int texorder[6] = {0, 2, 1, 3, 4, 5};
	static const u8 white[4] = {128, 128, 128, 128};
	for (int i = 0; i < 6; i++) {
		struct rp_image *im = sh->box[texorder[i]];
		const struct mx_tex *x = im ? rp_image_place(im) : NULL;
		if (!x)
			continue;
		memset(&s, 0, sizeof s);
		s.tex = *x, s.tex_ok = 1, s.clamp = 1, s.z = RP_Z_TEST, s.blend = RP_BLEND_NONE, s.texels = 1.0f;
		s.nomip = 1;
		layer(&s, strips, 0, i, NULL, white);
	}
	/* the cloud layers, in the shader's order, on all sides but the bottom */
	for (int k = 0; k < sh->nstage; k++) {
		const struct rp_stage *stg = &sh->st[k];
		if (!rp_stage_state(stg, t, &s))
			continue;
		s.z = RP_Z_TEST;
		/* the first level: a cloud's texel is pixels wide, but the box is far
		   away and a level chosen by its distance is the smallest (a sky of one
		   colour) */
		s.nomip = 1;
		float m[6];
		rp_stage_tcmod(stg, t, m);
		u8 col[4];
		rp_stage_colour(stg, t, NULL, NULL, col);
		layer(&s, strips, 5, -1, m, col);
	}
	rp_poly_flush();
}
