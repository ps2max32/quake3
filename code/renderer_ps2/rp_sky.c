/* rp_sky.c: the map's sky, as the engine draws it (renderergl1/tr_sky.c):
   a box around the eye, each side 8 x 8 quads; its far box's images on the
   six sides, then each cloud layer (a stage of the shader) on all sides but
   the bottom, its texture coordinates where the side's direction meets a
   sphere of the clouds' height over the world (in radians, through the
   stage's tcMods).

   The quads go the polygons' way (rp_poly.c) without depth, before the
   world: the world's surfaces cover the sky but where the map's sky
   surfaces are (they are not drawn). */

#include <math.h>
#include <string.h>

#include "rp_list.h"
#include "rp_tex.h"
#include "rp_poly.h"
#include "rp_sky.h"

#define SUB 8
#define HALF (SUB / 2)
#define BOX 585.0f          /* the engine's: a far plane of 1024 / 1.75 */
#define RADIUS_WORLD 4096.0f

static float pts[6][SUB + 1][SUB + 1][3];
static float box_st[6][SUB + 1][SUB + 1][2];
static float cloud_st[6][SUB + 1][SUB + 1][2];
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

/* the quads of side i, their texture coordinates st through m (6 floats,
   or 0), the colour col */
static void side(int i, float (*st)[SUB + 1][2], const float *m, const u8 *col) {
	const float *org = rp_view.org;
	for (int t = 0; t < SUB; t++)
		for (int s = 0; s < SUB; s++) {
			static const int cs[4] = {0, 1, 1, 0}, ct[4] = {0, 0, 1, 1};
			struct rp_vert v[4];
			for (int k = 0; k < 4; k++) {
				const float *p = pts[i][t + ct[k]][s + cs[k]], *a = st[t + ct[k]][s + cs[k]];
				for (int c = 0; c < 3; c++)
					v[k].xyz[c] = org[c] + p[c];
				v[k].st[0] = m ? m[0] * a[0] + m[1] * a[1] + m[2] : a[0];
				v[k].st[1] = m ? m[3] * a[0] + m[4] * a[1] + m[5] : a[1];
				for (int c = 0; c < 4; c++)
					v[k].rgba[c] = col[c];
			}
			/* a texture repeats: whole units off, so that a scroll that grows
			   with time stays within the 16-bit coordinates */
			if (m) {
				float fs = floorf(v[0].st[0]), ft = floorf(v[0].st[1]);
				for (int k = 0; k < 4; k++)
					v[k].st[0] -= fs, v[k].st[1] -= ft;
			}
			rp_poly(v, 4);
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
		s.tex = *x, s.tex_ok = 1, s.clamp = 1, s.z = RP_Z_NONE, s.blend = RP_BLEND_NONE, s.texels = 1.0f;
		rp_poly_state(&s);
		rp_poly_strips(strips);
		side(i, box_st[i], NULL, white);
	}
	/* the cloud layers, in the shader's order, on all sides but the bottom */
	for (int k = 0; k < sh->nstage; k++) {
		const struct rp_stage *stg = &sh->st[k];
		if (!rp_stage_state(stg, t, &s))
			continue;
		s.z = RP_Z_NONE;
		float m[6];
		rp_stage_tcmod(stg, t, m);
		u8 col[4];
		rp_stage_colour(stg, t, NULL, NULL, col);
		rp_poly_state(&s);
		rp_poly_strips(strips);
		for (int i = 0; i < 5; i++)
			side(i, cloud_st[i], m, col);
	}
	rp_poly_flush();
}
