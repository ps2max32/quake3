/* rp_shader.h: the game's shaders in the PS2 renderer (rp_shader.c): a
   shader script's stages, or an image alone, as states the frame's list
   draws. The game's side of the renderer (it reads through the engine). */

#ifndef RP_SHADER_H
#define RP_SHADER_H

#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_public.h"

#include "rp_list.h"
#include "rp_tex.h"

extern refimport_t ri;

enum { RGB_IDENTITY, RGB_VERTEX, RGB_ENTITY, RGB_ONE_MINUS_ENTITY, RGB_WAVE, RGB_CONST, RGB_LIGHTING,
       RGB_IDENTITY_LIGHTING, RGB_EXACT_VERTEX };
enum { A_IDENTITY, A_VERTEX, A_ENTITY, A_ONE_MINUS_ENTITY, A_WAVE, A_CONST };
enum { WAVE_SIN, WAVE_TRIANGLE, WAVE_SQUARE, WAVE_SAWTOOTH, WAVE_INVSAWTOOTH, WAVE_NOISE };
enum { TCM_SCROLL, TCM_SCALE, TCM_ROTATE, TCM_STRETCH, TCM_TRANSFORM, TCM_TURB };
enum { DEFORM_NONE, DEFORM_AUTOSPRITE, DEFORM_AUTOSPRITE2, DEFORM_OTHER };
/* the engine's shader sorts */
enum { SORT_PORTAL = 1, SORT_SKY = 2, SORT_OPAQUE = 3, SORT_DECAL = 4, SORT_SEE_THROUGH = 5, SORT_BANNER = 6,
       SORT_UNDERWATER = 8, SORT_BLEND0 = 9, SORT_ADDITIVE = 10, SORT_NEAREST = 16 };

struct rp_wave {
	u8 func;
	float base, amp, phase, freq;
};

#define RP_ANIM 8
#define RP_TCMODS 3
struct rp_stage {
	struct rp_image *img[RP_ANIM];
	u8 nimg;
	float fps;              /* animMap: images a second */
	u8 blend, alpha;        /* RP_BLEND_*, RP_ALPHA_* */
	u8 rgbgen, alphagen;
	u8 env;                 /* tcGen environment */
	u8 clamp, zwrite, luma; /* luma: the blend's factor is the texture's brightness (a CLUT of its own) */
	u8 lightmap;            /* $lightmap: the world's (not drawn as a stage) */
	u8 ntcmod;
	struct rp_wave rgbwave, awave;
	u8 rgba[4];             /* const */
	struct {
		u8 type;
		float p[6];
		struct rp_wave wave;
	} tcmod[RP_TCMODS];
};

#define RP_STAGES 4
struct rp_shader {
	char name[MAX_QPATH];
	int lightmap;           /* as registered: LIGHTMAP_2D, LIGHTMAP_NONE, ... */
	u8 nstage;
	u8 sort, cull_none, polygon_offset, sky, deform, nodraw, defaulted, nomip;
	struct rp_stage st[RP_STAGES];
};

/* Before the first shader of a registration (the hunk was cleared): nothing
   is registered, the scripts are read again. */
void rp_shaders_begin(void);
/* The engine's RegisterShader: 0 when there is neither script nor image. */
qhandle_t rp_shader_register(const char *name, int lightmap, int mip);
struct rp_shader *rp_shader_get(qhandle_t h);
int rp_shader_count(void);

/* A stage at time t (seconds): the state to draw it with (its image placed
   in GS memory for this frame); 0: nothing to draw (no image, no room). */
int rp_stage_state(const struct rp_stage *st, float t, struct rp_state *s);
/* Its colour: of the vertex's or entity's rgba (the engine's, 0..255) into
   out, the GS's: 0x80 is the texture's own colour, 0x80 an alpha of 1. The
   engine's colours are shown twice as bright as they say (its overbright
   range) but for rgbGen vertex, wave and identityLighting, which it halves:
   those come out 0x80 at full. */
void rp_stage_colour(const struct rp_stage *st, float t, const u8 *vertex, const u8 *entity, u8 *out);
/* Its texture coordinates' matrix at t: s' = m[0] s + m[1] t + m[2],
   t' = m[3] s + m[4] t + m[5]. Returns 0 when it is the identity. */
int rp_stage_tcmod(const struct rp_stage *st, float t, float *m);

#endif
