/* rp_main.c: the PS2's own renderer (PLAN-PS2.md): the engine's refexport_t
   (renderercommon/tr_public.h) on maxsdk, without renderergl1 and libgl.

   The screen is 1080i by beam racing (<mx/hires.h>: a field of 1920 x 540,
   drawn in strips ahead of the beam, each strip from a record of its own).
   A map's world is rp_world.c's, from maps/<name>.pmap beside the paks
   (tools/ps2map.py makes it from the BSP; a map without one is not drawn).

   Sprites and the engine's polys are built on the EE (rp_poly.c), the
   pictures of the screen are GS sprites.
   The game's shaders are rp_shader.c's, its models rp_model.c's (MD3, on
   VU1).

   Not yet: the map's doors and platforms, beams, lights, marks. They
   are counted. Each main view's camera goes into quake3.cam
   (ps2_ren_report), for the viewer (tools/q3view) to fly the same path.
   r_ps2draw 0: nothing is drawn, a timedemo then costs what the game side
   costs (game, cgame, client, sound, collision); 1 and a sum of: 2 no world,
   4 no models, 8 no sprites and polys, 16 no view weapon, 32 no pictures
   (what each costs a pass). r_ps2debug: rp_list.h's rp_debug. r_ps2shot n: the picture
   of the timedemo's frame n is saved as quake3.tga and the game quits.
   r_ps2passes 1: the timedemo's passes of beam racing as quake3.pass. */

#include "../qcommon/q_shared.h"
#include "../qcommon/qfiles.h"
#include "../qcommon/qcommon.h"
#include "../renderercommon/tr_public.h"

#include <mx/frame.h>
#include <mx/hires.h>
#include <mx/mem.h>
#include <mx/mx.h>
#include <mx/vm.h>

#include "rp_bin.h"
#include "rp_list.h"
#include "rp_model.h"
#include "rp_poly.h"
#include "rp_shader.h"
#include "rp_sky.h"
#include "rp_tex.h"
#include "rp_world.h"

refimport_t ri;
static float scene_time; /* the scene's being drawn, for its shaders */
static cvar_t *r_ps2draw, *r_ps2shot, *r_ps2passes;

/* ---- the screen ------------------------------------------------------------------- */

/* beam racing's records, read by the DMAC: four frames, and four of each of
   the three strips. They hold CALLs (the draws lie elsewhere: rp_list.c): a
   frame of the game is a few KB a strip, and its texture uploads (at most
   rp_tex.c's a frame) fit the frame's own record. */
static u8 rec[4 * 64 * 1024] MX_FAST_BSS MX_ALIGNED(64);
static u8 srec[4 * 3 * 32 * 1024] MX_FAST_BSS MX_ALIGNED(64);
static int screen_on, in_frame;
/* cycles a frame waited for a free record: not the frame's work (sys_ps2.c) */
unsigned ps2_ren_wait;

static void screen_init(void) {
	if (screen_on)
		return;
	mx_hires_strip_lines((u32)ri.Cvar_Get("r_ps2strip", "192", CVAR_LATCH)->integer);
	if (mx_hires_init(&mx_video_1080i, 1920, 540, MX_GS_PSM_CT16S, rec, sizeof rec, (void *)0x70002000, 0x2000) < 0)
		ri.Error(ERR_FATAL, "ps2: no 1080i, or its buffer does not fit");
	u32 sbytes = mx_hires_strip_records(srec, sizeof srec);
	ri.Printf(PRINT_ALL, "ps2: renderer: 1080i, %u strips of %u lines, a strip's record %u KB\n", mx_hires_passes(),
	          mx_hires_strip_height(), sbytes / 1024);
	rp_tex_init();
	rp_list_init();
	/* the images and the models: two files beside the paks, mapped */
	char path[MAX_OSPATH];
	const char *base = ri.Cvar_Get("fs_basepath", "", 0)->string;
	if (base[0] == '.' && base[1] == '/')
		base += 2;
	else if (base[0] == '.' && !base[1])
		base = "";
	Com_sprintf(path, sizeof path, "%s%s%s/ps2tex.bin", base, base[0] ? "/" : "", BASEGAME);
	if (rp_bin_open(&rp_bin_tex, path, 1) < 0)
		ri.Error(ERR_FATAL, "ps2: no %s (tools/ps2bin.py makes it of ps2tex.pk3)", path);
	Com_sprintf(path, sizeof path, "%s%s%s/ps2mdl.bin", base, base[0] ? "/" : "", BASEGAME);
	if (rp_bin_open(&rp_bin_mdl, path, 2) < 0)
		ri.Error(ERR_FATAL, "ps2: no %s (tools/ps2bin.py makes it of the paks)", path);
	ri.Printf(PRINT_ALL, "ps2: renderer: %u images (%u MB) and %u models (%u MB) mapped\n", rp_bin_tex.count,
	          rp_bin_tex.size >> 20, rp_bin_mdl.count, rp_bin_mdl.size >> 20);
	screen_on = 1;
}

/* ---- the world ---------------------------------------------------------------------- */

static qhandle_t sky_shader; /* the map's sky (its first material that is sky) */
static const void *world_map;
static u32 world_size;
static int world_ok;
static struct rp_world_stats world_st, world_sum;

/* Nothing a frame in flight reads may go: an empty frame, and the wait until
   it is the one beam racing plays. */
static void frames_flush(void) {
	if (!screen_on || in_frame)
		return;
	mx_frame_begin_n();
	mx_hires_clear(0, 0, 0);
	u32 f = mx_frame_number();
	mx_frame_end();
	/* the frames before it are done when it has replaced them */
	while (mx_frame_done() < f)
		mx_sleep_us(2000);
}

static void world_free(void) {
	if (world_map) {
		frames_flush();
		rp_world_free();
		mx_vm_unmap_file(world_map);
	}
	world_map = NULL, world_ok = 0;
}
int ps2_demo_frame(void); /* sys_ps2.c: the timedemo's frame, 0: none */

/* ---- what a frame hands the renderer ---------------------------------------------- */

static struct {
	unsigned scenes, views, small; /* RenderScene: all, with the world, without (HUD models) */
	unsigned ents[RT_MAX_REF_ENTITY_TYPE], polys, polyverts, lights, pics, marks;
} st, st_at; /* st_at: at the timedemo's first frame */
static unsigned st_frames; /* the timedemo's frames counted */
static unsigned cur_ents, cur_polys, cur_polyverts, cur_lights; /* since ClearScene */

/* a main view's camera, quake3.cam: a head, then one of these a view */
struct cam {
	int frame; /* the timedemo's frame */
	int time;
	float org[3], axis[3][3], fov_x, fov_y;
	int x, y, w, h;
	unsigned ents, polys, polyverts, lights;
};
#define CAM_MAX 4096
static struct cam *cams;
static int ncams;

/* ---- a scene ---------------------------------------------------------------------------- */

/* what the game added since the last scene was drawn: sprites (the entities
   that are not models are kept as they come), polys and their vertices */
#define MAX_SPRITE 1024
#define MAX_POLY 2048
#define MAX_POLYVERT 12288
#define MAX_MODEL 1024
static refEntity_t sprites[MAX_SPRITE], ments[MAX_MODEL];
static u32 nment, first_ment;
static struct {
	qhandle_t shader;
	u16 first, n;
} polys[MAX_POLY];
static struct rp_vert polyverts[MAX_POLYVERT];
static u32 nsprite, npoly, npolyvert;          /* in the frame */
static u32 first_sprite, first_poly;           /* of the scene */
static unsigned st_nostage, st_quads;

static void RP_ClearScene(void) {
	cur_ents = cur_polys = cur_polyverts = cur_lights = 0;
	first_sprite = nsprite, first_poly = npoly, first_ment = nment;
}

static void RP_AddRefEntityToScene(const refEntity_t *e) {
	if ((unsigned)e->reType < RT_MAX_REF_ENTITY_TYPE)
		st.ents[e->reType]++;
	cur_ents++;
	if (!in_frame)
		return;
	if (e->reType == RT_SPRITE && e->radius > 0 && nsprite < MAX_SPRITE)
		sprites[nsprite++] = *e;
	/* a body seen only in mirrors is not this view's */
	if (e->reType == RT_MODEL && !(e->renderfx & RF_THIRD_PERSON) && nment < MAX_MODEL)
		ments[nment++] = *e;
}

static void RP_AddPolyToScene(qhandle_t shader, int numVerts, const polyVert_t *verts, int num) {
	st.polys += num, st.polyverts += numVerts * num;
	cur_polys += num, cur_polyverts += numVerts * num;
	if (!in_frame || numVerts < 3 || numVerts > 16)
		return;
	for (int j = 0; j < num; j++) {
		if (npoly == MAX_POLY || npolyvert + numVerts > MAX_POLYVERT)
			return;
		polys[npoly].shader = shader, polys[npoly].first = (u16)npolyvert, polys[npoly].n = (u16)numVerts;
		memcpy(&polyverts[npolyvert], verts + j * numVerts, numVerts * sizeof *verts);
		npolyvert += numVerts, npoly++;
	}
}

static void RP_AddLightToScene(const vec3_t org, float intensity, float r, float g, float b) {
	st.lights++, cur_lights++;
}

/* a scene's quads in the order they are drawn: by their shader's sort, then
   as they came */
struct quad {
	qhandle_t shader;
	u8 sort;
	u8 ent[4];           /* the entity's colour */
	float time;          /* the shader's */
	const struct rp_vert *v;
	u32 n;
};
#define MAX_QUAD (MAX_SPRITE + MAX_POLY)
static struct quad quad[MAX_QUAD];
static u16 quad_order[MAX_QUAD];
static struct rp_vert sprite_vert[MAX_SPRITE][4];

float rp_sin1(float turns);

static u32 scene_quads(const refdef_t *fd) {
	u32 n = 0;
	float now = fd->time * 0.001f;
	for (u32 i = first_poly; i < npoly; i++) {
		struct rp_shader *sh = rp_shader_get(polys[i].shader);
		if (!sh || !sh->nstage)
			continue;
		struct quad *q = &quad[n++];
		q->shader = polys[i].shader, q->sort = sh->sort, q->time = now;
		q->ent[0] = q->ent[1] = q->ent[2] = q->ent[3] = 255;
		q->v = &polyverts[polys[i].first], q->n = polys[i].n;
	}
	for (u32 i = first_sprite; i < nsprite; i++) {
		const refEntity_t *e = &sprites[i];
		struct rp_shader *sh = rp_shader_get(e->customShader);
		if (!sh || !sh->nstage)
			continue;
		/* the square faces the eye, turned by its rotation */
		float left[3], up[3];
		if (e->rotation == 0) {
			VectorScale(fd->viewaxis[1], e->radius, left);
			VectorScale(fd->viewaxis[2], e->radius, up);
		} else {
			float sn = rp_sin1(e->rotation * (1.0f / 360.0f)), cs = rp_sin1(e->rotation * (1.0f / 360.0f) + 0.25f);
			VectorScale(fd->viewaxis[1], cs * e->radius, left);
			VectorMA(left, -sn * e->radius, fd->viewaxis[2], left);
			VectorScale(fd->viewaxis[2], cs * e->radius, up);
			VectorMA(up, sn * e->radius, fd->viewaxis[1], up);
		}
		struct rp_vert *v = sprite_vert[i - first_sprite];
		static const float sg[4][4] = {{1, 1, 0, 0}, {-1, 1, 1, 0}, {-1, -1, 1, 1}, {1, -1, 0, 1}};
		for (int k = 0; k < 4; k++) {
			for (int a = 0; a < 3; a++)
				v[k].xyz[a] = e->origin[a] + sg[k][0] * left[a] + sg[k][1] * up[a];
			v[k].st[0] = sg[k][2], v[k].st[1] = sg[k][3];
			memcpy(v[k].rgba, e->shaderRGBA, 4);
		}
		struct quad *q = &quad[n++];
		q->shader = e->customShader, q->sort = sh->sort, q->time = now - e->shaderTime;
		memcpy(q->ent, e->shaderRGBA, 4);
		q->v = v, q->n = 4;
	}
	/* by sort, the order kept (few, and nearly in order: insertion) */
	for (u32 i = 0; i < n; i++) {
		u32 j = i;
		for (; j > 0 && quad[quad_order[j - 1]].sort > quad[i].sort; j--)
			quad_order[j] = quad_order[j - 1];
		quad_order[j] = (u16)i;
	}
	return n;
}

/* the quads from..to of the order: the ones of one shader after each other
   are drawn a stage at a time */
static void quads_draw(u32 from, u32 to) {
	for (u32 i = from; i < to;) {
		const struct quad *q0 = &quad[quad_order[i]];
		struct rp_shader *sh = rp_shader_get(q0->shader);
		u32 end = i + 1;
		while (end < to && quad[quad_order[end]].shader == q0->shader)
			end++;
		for (int s = 0; s < sh->nstage; s++) {
			const struct rp_stage *stg = &sh->st[s];
			struct rp_state state;
			float m[6];
			if (!rp_stage_state(stg, q0->time, &state)) {
				st_nostage++;
				continue;
			}
			rp_poly_state(&state);
			for (u32 k = i; k < end; k++) {
				const struct quad *q = &quad[quad_order[k]];
				struct rp_vert v[16];
				int moved = rp_stage_tcmod(stg, q->time, m);
				for (u32 a = 0; a < q->n; a++) {
					v[a] = q->v[a];
					rp_stage_colour(stg, q->time, q->v[a].rgba, q->ent, v[a].rgba);
					if (moved) {
						float ss = v[a].st[0], tt = v[a].st[1];
						v[a].st[0] = m[0] * ss + m[1] * tt + m[2], v[a].st[1] = m[3] * ss + m[4] * tt + m[5];
					}
				}
				rp_poly(v, q->n);
				st_quads++;
			}
		}
		i = end;
	}
	rp_poly_flush();
}

static struct {
	qhandle_t shader;
	int stage;
	u32 rgba;   /* the last picture's colour as the GS takes it, for colour */
	u32 colour; /* color2d as one word */
	int plain;  /* one stage, its texture coordinates as given */
} last2d;

static void RP_RenderScene(const refdef_t *fd) {
	st.scenes++;
	int world = !(fd->rdflags & RDF_NOWORLDMODEL);
	if (world)
		st.views++;
	else
		st.small++;
	if (in_frame && r_ps2draw->integer) {
		struct rp_cam cam;
		rp_poly_flush();
		last2d.shader = 0;
		VectorCopy(fd->vieworg, cam.org);
		memcpy(cam.axis, fd->viewaxis, sizeof cam.axis);
		cam.fov_x = fd->fov_x, cam.fov_y = fd->fov_y;
		/* the game's 1080 lines are a field's 540 */
		float ys = (float)mx_gs.h / 1080.0f;
		rp_view_rect(&cam, (float)fd->x, (float)fd->y * ys, (float)fd->width, (float)fd->height * ys);
		u32 nq = scene_quads(fd), decals = 0;
		float now = fd->time * 0.001f;
		scene_time = now;
		while (decals < nq && quad[quad_order[decals]].sort <= SORT_DECAL)
			decals++;
		rp_scene_world = world;
		int skip = r_ps2draw->integer;
		int draw_world = world && world_ok && !(skip & 2);
		if (draw_world) {
			rp_world_cull(&world_st);
			/* the sky first, where the view sees the map's sky surfaces */
			u32 sky_strips = sky_shader ? rp_world_sky() : 0;
			const struct rp_shader *sky = sky_strips ? rp_shader_get(sky_shader) : NULL;
			if (sky)
				rp_sky_draw(sky, now, sky_strips);
			rp_world_add(0);
#define ACC(f) world_sum.f += world_st.f
			ACC(seen), ACC(groups), ACC(drawn), ACC(clip), ACC(back), ACC(verts);
#undef ACC
		}
		/* a scene of its own on the screen (the HUD's models): no depth is there yet */
		if (!world)
			rp_zclear((float)fd->x, (float)fd->y * ys, (float)(fd->x + fd->width), (float)(fd->y + fd->height) * ys);
		/* the models that write depth; the marks on the walls; the world's
		   blended surfaces; the models that blend, and the rest; last the view
		   weapon, over depth of its own (the engine squeezes its depth range) */
		int hack = 0;
		static u8 kind[MAX_MODEL]; /* 1: its shader blends; 2: the view weapon's depth */
		for (u32 i = first_ment; i < nment; i++) {
			const refEntity_t *e = &ments[i];
			kind[i] = (u8)(rp_model_blended(e) | (e->renderfx & RF_DEPTHHACK ? 2 : 0));
			if ((skip & 4) || ((skip & 16) && (kind[i] & 2)))
				kind[i] = 4;
			else if (kind[i] & 2)
				hack = 1;
			else if (!kind[i])
				rp_model_draw(e, now - e->shaderTime);
		}
		if (skip & 8)
			nq = decals = 0;
		quads_draw(0, decals);
		if (draw_world)
			rp_world_add(1);
		for (u32 i = first_ment; i < nment; i++)
			if (kind[i] == 1)
				rp_model_draw(&ments[i], now - ments[i].shaderTime);
		quads_draw(decals, nq);
		if (hack) {
			float rc[4] = {1e9f, 1e9f, -1e9f, -1e9f}, r1[4];
			for (u32 i = first_ment; i < nment; i++)
				if ((ments[i].renderfx & RF_DEPTHHACK) && rp_model_rect(&ments[i], r1)) {
					rc[0] = r1[0] < rc[0] ? r1[0] : rc[0], rc[1] = r1[1] < rc[1] ? r1[1] : rc[1];
					rc[2] = r1[2] > rc[2] ? r1[2] : rc[2], rc[3] = r1[3] > rc[3] ? r1[3] : rc[3];
				}
			rp_zclear(rc[0], rc[1], rc[2], rc[3]);
			for (u8 blended = 0; blended < 2; blended++)
				for (u32 i = first_ment; i < nment; i++)
					if (kind[i] == (2 | blended))
						rp_model_draw(&ments[i], now - ments[i].shaderTime);
		}
	}
	first_sprite = nsprite, first_poly = npoly, first_ment = nment;
	if (!world)
		return;
	int df = ps2_demo_frame();
	if (!df)
		return;
	if (!cams)
		cams = ri.Malloc(CAM_MAX * sizeof *cams);
	if (ncams >= CAM_MAX)
		return;
	struct cam *c = &cams[ncams++];
	c->frame = df, c->time = fd->time;
	VectorCopy(fd->vieworg, c->org);
	memcpy(c->axis, fd->viewaxis, sizeof c->axis);
	c->fov_x = fd->fov_x, c->fov_y = fd->fov_y;
	c->x = fd->x, c->y = fd->y, c->w = fd->width, c->h = fd->height;
	c->ents = cur_ents, c->polys = cur_polys, c->polyverts = cur_polyverts, c->lights = cur_lights;
}

/* ---- the screen's pictures ------------------------------------------------------------- */

static u8 color2d[4] = {255, 255, 255, 255};
static float frame_time; /* the frame's, for the pictures' shaders */

static void RP_SetColor(const float *rgba) {
	for (int k = 0; k < 4; k++) {
		float v = rgba ? rgba[k] * 255.0f : 255.0f;
		color2d[k] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
	}
}

/* x, y, w, h in the game's 1920 x 1080 */
static void RP_DrawStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2,
                              qhandle_t shader) {
	st.pics++;
	if (!in_frame || !r_ps2draw->integer || (r_ps2draw->integer & 32))
		return;
	float now = frame_time, ys = (float)mx_gs.h / 1080.0f;
	u32 col = (u32)color2d[0] | (u32)color2d[1] << 8 | (u32)color2d[2] << 16 | (u32)color2d[3] << 24;
	/* as the picture before (a line of text): nothing to look up */
	if (shader == last2d.shader && last2d.plain && col == last2d.colour) {
		rp_pic(x, y * ys, w, h * ys, s1, t1, s2, t2, last2d.rgba);
		return;
	}
	struct rp_shader *sh = rp_shader_get(shader);
	if (!sh)
		return;
	for (int k = 0; k < sh->nstage; k++) {
		const struct rp_stage *stg = &sh->st[k];
		/* the state again only when it is another than the last picture's */
		if (last2d.shader != shader || last2d.stage != k || stg->nimg > 1) {
			struct rp_state state;
			last2d.shader = 0;
			if (!rp_stage_state(stg, now, &state)) {
				st_nostage++;
				continue;
			}
			state.nomip = 1;
			rp_pic_state(&state);
			last2d.shader = shader, last2d.stage = k;
		}
		u8 c[4];
		float m[6], a1 = s1, b1 = t1, a2 = s2, b2 = t2;
		rp_stage_colour(stg, now, color2d, color2d, c);
		if (rp_stage_tcmod(stg, now, m)) {
			a1 = m[0] * s1 + m[1] * t1 + m[2], b1 = m[3] * s1 + m[4] * t1 + m[5];
			a2 = m[0] * s2 + m[1] * t2 + m[2], b2 = m[3] * s2 + m[4] * t2 + m[5];
		}
		u32 rgba = (u32)c[0] | (u32)c[1] << 8 | (u32)c[2] << 16 | (u32)c[3] << 24;
		rp_pic(x, y * ys, w, h * ys, a1, b1, a2, b2, rgba);
		last2d.rgba = rgba, last2d.colour = col;
		last2d.plain = sh->nstage == 1 && stg->nimg == 1 && !stg->ntcmod && last2d.shader == shader &&
		               stg->rgbgen != RGB_WAVE && stg->alphagen != A_WAVE;
	}
}


static void RP_BeginFrame(stereoFrame_t stereoFrame) {
	if (!screen_on || in_frame)
		return;
	u32 t0 = mx_count();
	mx_frame_begin_n();
	ps2_ren_wait += mx_count() - t0;
	in_frame = 1;
	rp_tex_frame();
	mx_hires_clear(0, 0, 0);
	rp_list_begin();
	nsprite = npoly = npolyvert = first_sprite = first_poly = nment = first_ment = 0;
	frame_time = (float)ri.Milliseconds() * 0.001f;
	last2d.shader = 0;
	/* the pictures before a scene: over the whole picture */
	struct rp_cam none = {{0, 0, 0}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, 90, 60};
	rp_view_set(&none);
}

/* r_ps2passes 1: beam racing's passes of the timedemo's frames (maxsdk's log
   of them) with what each frame put into each strip, as a file: which passes
   the beam overtook, and what was in them. */
#define PASS_LOG 8192
#define FRAME_LOG 2048
static struct mx_hires_pass *pass_log;
static struct {
	u32 frame;
	u16 demo, tex_kb;
	u16 sent[8], batches[8], setups[8], msent[8], mbatches[8], draws[8];
} frame_log[FRAME_LOG];
static u32 pass_tex_kb;

static void passes_frame(u32 frame, int df) {
	if (!pass_log) {
		pass_log = mx_dma_alloc(PASS_LOG * sizeof *pass_log);
		if (!pass_log)
			return;
		mx_hires_log(pass_log, PASS_LOG);
	}
	typeof(frame_log[0]) *f = &frame_log[frame % FRAME_LOG];
	f->frame = frame, f->demo = (u16)df;
	f->tex_kb = (u16)(rp_tex_stats.upload_kb - pass_tex_kb);
	pass_tex_kb = rp_tex_stats.upload_kb;
	for (int k = 0; k < 8; k++) {
		f->sent[k] = (u16)rp_list_stats.k_sent[k], f->batches[k] = (u16)rp_list_stats.k_batches[k];
		f->setups[k] = (u16)rp_list_stats.k_setups[k];
		f->msent[k] = (u16)rp_list_stats.k_msent[k], f->mbatches[k] = (u16)rp_list_stats.k_mbatches[k];
		f->draws[k] = (u16)rp_list_stats.k_draws[k];
	}
}

static void passes_report(void) {
	if (!pass_log)
		return;
	u32 n = mx_hires_stats.logged, first = n > PASS_LOG ? n - PASS_LOG : 0;
	mx_hires_log(0, 0);
	static char text[64 * 96];
	FILE *fo = fopen("quake3.pass", "w");
	int len = sprintf(text, "# frame demo strip wait_us run_us over out sent batches setups tex_kb msent mbatches draws\n");
	for (u32 i = first; i < n && fo; i++) {
		const struct mx_hires_pass *e = &pass_log[i % PASS_LOG];
		const typeof(frame_log[0]) *f = &frame_log[e->frame % FRAME_LOG];
		u32 k = e->strip & 7;
		int known = f->frame == e->frame;
		len += sprintf(text + len, "%u %u %u %u %u %u %u %u %u %u %u %u %u %u\n", e->frame, known ? f->demo : 0, e->strip,
		               e->wait_us, e->run_us, e->over, e->out, known ? f->sent[k] : 0, known ? f->batches[k] : 0,
		               known ? f->setups[k] : 0, known ? f->tex_kb : 0, known ? f->msent[k] : 0,
		               known ? f->mbatches[k] : 0, known ? f->draws[k] : 0);
		if (len > (int)sizeof text - 96) {
			fwrite(text, 1, len, fo);
			len = 0;
		}
	}
	if (fo) {
		fwrite(text, 1, len, fo);
		fclose(fo);
	}
	Com_Printf("ps2: renderer: quake3.pass: %u passes%s\n", n - first, fo ? "" : " (not written)");
}

static void RP_EndFrame(int *frontEndMsec, int *backEndMsec) {
	int shot = in_frame && r_ps2shot->integer && ps2_demo_frame() == r_ps2shot->integer;
	if (in_frame) {
		rp_poly_flush();
		rp_list_end();
		u32 f = mx_frame_number();
		mx_frame_end();
		in_frame = 0;
		if (shot) {
			/* the frame's picture: when it is the one played, and a field more */
			while (mx_frame_done() < f)
				mx_sleep_us(2000);
			for (u32 v = mx_gs.vsync; mx_gs.vsync - v < 6;)
				mx_sleep_us(2000);
			int r = mx_hires_save_tga("quake3.tga");
			ri.Printf(PRINT_ALL, "ps2: renderer: the picture of frame %d: quake3.tga (%d)\n", r_ps2shot->integer, r);
			ri.Cmd_ExecuteText(EXEC_NOW, "meminfo\n");
			ri.Cmd_ExecuteText(EXEC_APPEND, "quit\n");
		}
	}
	if (frontEndMsec)
		*frontEndMsec = 0;
	if (backEndMsec)
		*backEndMsec = 0;
	/* the counts are the timedemo's: from its first frame on */
	int df = ps2_demo_frame();
	if (df && r_ps2passes->integer && mx_hires_passes())
		passes_frame(mx_frame_number() - 1, df);
	if (df && !st_frames) {
		st_at = st;
		rp_list_stats = (struct rp_list_stats){0};
		rp_tex_stats = (struct rp_tex_stats){0};
		pass_tex_kb = 0;
		u32 kb = rp_model_stats.kb;
		rp_model_stats = (struct rp_model_stats){.kb = kb};
	}
	if (df)
		st_frames++;
}

static int RP_MarkFragments(int numPoints, const vec3_t *points, const vec3_t projection, int maxPoints,
                            vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer) {
	st.marks++;
	return 0;
}

/* where the renderer's mapped files lie, for a report by address (sys_ps2.c) */
void ps2_ren_regions(void (*fn)(const char *name, const void *p, unsigned bytes)) {
	fn("ps2tex.bin", rp_bin_tex.base, rp_bin_tex.size);
	fn("ps2mdl.bin", rp_bin_mdl.base, rp_bin_mdl.size);
	fn("the map", world_map, world_size);
}

static void tex_line(const char *name, u32 kb, u32 frames, u32 w, u32 h) {
	Com_Printf("ps2: renderer: image %s %u x %u: %u KB of GS memory in %u frames\n", name, w, h, kb, frames);
}

/* at the end of a run: the counts a frame, the cameras as a file */
void ps2_ren_report(void) {
	static int done;
	if (done || !st_frames)
		return;
	done = 1;
	unsigned n = st_frames;
#define A(f) ((st.f - st_at.f) * 10 / n / 10), ((st.f - st_at.f) * 10 / n % 10)
	Com_Printf("ps2: renderer: %u frames; a frame: %u.%u views, %u.%u small scenes, %u.%u models, %u.%u sprites, "
	           "%u.%u beams and lightning, %u.%u polys of %u.%u vertices, %u.%u lights, %u.%u pictures, "
	           "%u.%u mark requests\n",
	           n, A(views), A(small), A(ents[RT_MODEL]), A(ents[RT_SPRITE]),
	           (st.ents[RT_BEAM] + st.ents[RT_RAIL_CORE] + st.ents[RT_RAIL_RINGS] + st.ents[RT_LIGHTNING] -
	            st_at.ents[RT_BEAM] - st_at.ents[RT_RAIL_CORE] - st_at.ents[RT_RAIL_RINGS] -
	            st_at.ents[RT_LIGHTNING]) * 10 / n / 10,
	           (st.ents[RT_BEAM] + st.ents[RT_RAIL_CORE] + st.ents[RT_RAIL_RINGS] + st.ents[RT_LIGHTNING] -
	            st_at.ents[RT_BEAM] - st_at.ents[RT_RAIL_CORE] - st_at.ents[RT_RAIL_RINGS] -
	            st_at.ents[RT_LIGHTNING]) * 10 / n % 10,
	           A(polys), A(polyverts), A(lights), A(pics), A(marks));
#undef A
	Com_Printf("ps2: renderer: %d models (%u KB of vertices; %u frames of surfaces made, %u given up), %d shaders "
	           "registered\n", rp_model_count(), rp_model_stats.kb, rp_model_stats.made, rp_model_stats.evicted,
	           rp_shader_count());
	Com_Printf("ps2: renderer: models, a frame: %u drawn (%u surfaces, %u pieces, %u vertices), %u of them across a "
	           "plane (cut on VU1), %u out of view; in all %u surfaces without a stage, %u later stages not drawn, %u "
	           "times a surface's frame was not ready\n",
	           (rp_model_stats.drawn + rp_model_stats.cut) / n, rp_model_stats.surfs / n, rp_model_stats.pieces / n, rp_model_stats.verts / n,
	           rp_model_stats.cut / n, rp_model_stats.culled / n, rp_model_stats.nostage, rp_model_stats.stages,
	           rp_model_stats.paged);
	rp_shader_report();
	{
		struct mx_vcache_stats vs;
		mx_vcache_stats(&vs);
		Com_Printf("ps2: renderer: GS memory: a frame draws with %u KB of images (at most %u KB); the cache has %u pages "
		           "(%u KB), %u KB in use by %u entries; %u evicted\n",
		           rp_tex_stats.frames ? rp_tex_stats.used_kb / rp_tex_stats.frames : 0, rp_tex_stats.used_max_kb, vs.pages,
		           vs.pages * 8, vs.used_blocks / 4, vs.entries, vs.evicted);
		u32 fr = rp_tex_stats.frames ? rp_tex_stats.frames * 4 : 1;
		const u32 *kb = rp_tex_stats.kind_blocks;
		Com_Printf("ps2: renderer: GS memory: of a frame's images the map's and the scripts' %u KB, lightmap pages %u KB, "
		           "the models' %u KB, the screen's %u KB, others %u KB\n", kb[RP_IMAGE_WORLD] / fr,
		           kb[RP_IMAGE_LIGHTMAP] / fr, kb[RP_IMAGE_MODEL] / fr, kb[RP_IMAGE_SCREEN] / fr, kb[RP_IMAGE_OTHER] / fr);
		if (ri.Cvar_VariableIntegerValue("r_ps2images"))
			rp_tex_report(tex_line);
	}
	Com_Printf("ps2: renderer: the hunk has %d KB left of %d MB\n", Hunk_MemoryRemaining() / 1024,
	           Cvar_VariableIntegerValue("com_hunkMegs"));
	Com_Printf("ps2: renderer: polygons: %u drawn (%u across a plane), %u out of view; %u pictures; %u stages without "
	           "an image in GS memory\n", rp_poly_stats.polys, rp_poly_stats.cut, rp_poly_stats.culled,
	           rp_poly_stats.pics, st_nostage);
	unsigned v = st.views - st_at.views;
	if (v && world_sum.drawn)
		Com_Printf("ps2: renderer: the world, a view: %u groups in view, %u chunks (%u across a plane, %u more facing "
		           "away), %u vertices\n", world_sum.groups / v, world_sum.drawn / v, world_sum.clip / v,
		           world_sum.back / v, world_sum.verts / v);
	if (st_frames)
		Com_Printf("ps2: renderer: a frame: %u runs, %u draws; over the strips %u vertices in %u batches, %u set-ups, %u "
		           "quadwords of records, %u of chains; %u draws left out (no memory); textures sent %u (%u KB), %u "
		           "found no room, %u waited a frame\n", rp_list_stats.runs / st_frames, rp_list_stats.draws / st_frames,
		           rp_list_stats.sent / st_frames, rp_list_stats.batches / st_frames, rp_list_stats.setups / st_frames,
		           rp_list_stats.qw / st_frames, rp_list_stats.dyn_qw / st_frames, rp_list_stats.lost,
		           rp_tex_stats.uploads, rp_tex_stats.upload_kb, rp_tex_stats.full, rp_tex_stats.later);
	if (st_frames) {
		static const char *const prog[4] = {"the world's", "GS packets", "the models'", "the stages'"};
		for (int k = 0; k < 4; k++)
			Com_Printf("ps2: renderer: a frame, %s program: %u vertices (%u through the one that cuts) in %u batches, %u "
			           "set-ups\n", prog[k], rp_list_stats.p_sent[k] / st_frames, rp_list_stats.p_clip[k] / st_frames,
			           rp_list_stats.p_batches[k] / st_frames, rp_list_stats.p_setups[k] / st_frames);
	}
	passes_report();
	if (ncams) {
		FILE *f = fopen("quake3.cam", "wb");
		int head[4] = {0x4d414351 /* "QCAM" */, 1, ncams, sizeof *cams};
		if (f) {
			fwrite(head, sizeof head, 1, f);
			fwrite(cams, sizeof *cams, ncams, f);
			fclose(f);
		}
		Com_Printf("ps2: renderer: quake3.cam: %d cameras%s\n", ncams, f ? "" : " (not written)");
	}
}

int ps2_ren_screenshot(const char *path) { return screen_on ? mx_hires_save_tga(path) : -1; }

/* ---- the rest of the interface -------------------------------------------------------- */

static void RP_Shutdown(qboolean destroyWindow) {
	if (destroyWindow)
		ri.IN_Shutdown();
}

static void RP_BeginRegistration(glconfig_t *config) {
	static int pad;
	/* the hunk was cleared: nothing registered is left */
	memset(config, 0, sizeof *config);
	Q_strncpyz(config->renderer_string, "PS2 GS (maxsdk)", sizeof config->renderer_string);
	Q_strncpyz(config->vendor_string, "ps2max", sizeof config->vendor_string);
	Q_strncpyz(config->version_string, "renderer_ps2", sizeof config->version_string);
	r_ps2draw = ri.Cvar_Get("r_ps2draw", "1", 0);
	r_ps2shot = ri.Cvar_Get("r_ps2shot", "0", 0);
	r_ps2passes = ri.Cvar_Get("r_ps2passes", "0", 0);
	rp_debug = (unsigned)ri.Cvar_Get("r_ps2debug", "0", 0)->integer;
	screen_init();
	/* no frame in flight draws what goes now: the map, the images */
	world_free();
	frames_flush();
	rp_shaders_begin();
	rp_models_begin();
	config->maxTextureSize = 128;
	config->numTextureUnits = 1;
	config->colorBits = 16, config->depthBits = 16;
	config->driverType = GLDRV_ICD, config->hardwareType = GLHW_GENERIC;
	config->vidWidth = 1920, config->vidHeight = 1080;
	config->windowAspect = 16.0f / 9.0f;
	config->displayFrequency = 60;
	config->isFullscreen = qtrue;
	if (!pad++)
		ri.IN_Init(NULL);
}

static qhandle_t RP_RegisterSkin(const char *name) { return rp_skin_register(name); }
static qhandle_t RP_RegisterModel(const char *name) { return rp_model_register(name); }
static int RP_LerpTag(orientation_t *tag, qhandle_t h, int startFrame, int endFrame, float frac, const char *tagName) {
	return rp_model_tag(tag, h, startFrame, endFrame, frac, tagName);
}
static void RP_ModelBounds(qhandle_t h, vec3_t mins, vec3_t maxs) { rp_model_bounds(h, mins, maxs); }
static qhandle_t RP_RegisterShader(const char *name) { return rp_shader_register(name, -1, 1); }
/* the screen's pictures (LIGHTMAP_2D) */
static qhandle_t RP_RegisterShaderNoMip(const char *name) { return rp_shader_register(name, -4, 0); }
/* maps/<name>.bsp: its world is maps/<name>.pmap, a file beside the paks, mapped
   and locked in memory (the DMAC reads its chunks and textures where they lie) */
/* the shaders of the map's materials that are scripts, and the scene's time */
#define MAX_WMAT 1024
static qhandle_t wshader[MAX_WMAT];

/* A material's passes at this frame (rp_world.h): a lightmapped one's first
   stage is the map's own pass (texture and lightmap); the others, and every
   stage of one that is not lightmapped, are passes of the stage's program. */
static u32 world_passes(u32 mat, struct rp_state *pass, int *first) {
	u32 flags = 0, n = 0;
	struct rp_shader *sh = mat < MAX_WMAT ? rp_shader_get(wshader[mat]) : NULL;
	if (!sh || !sh->nstage || !rp_world_material(mat, &flags))
		return 0;
	int lit = (flags & RP_MAT_LIGHTMAP) != 0;
	/* the $lightmap stage after two or more others: they are drawn unlit and
	   the lightmap over all of them (else the map's own pass is the first
	   stage times the lightmap, the order the GS gives a stage and its
	   lightmap in either order) */
	int late = lit && sh->lm_at >= 2 && sh->lm_at < RP_STAGES + 1;
	*first = !lit || late;
	for (int k = lit && !late ? 1 : 0; k <= sh->nstage && n < RP_WORLD_PASSES; k++) {
		if (late && k == sh->lm_at) {
			memset(&pass[n], 0, sizeof pass[n]);
			pass[n].prog = RP_PROG_WORLD, pass[n].lm_only = 1, pass[n].z = RP_Z_TEST;
			if (++n == RP_WORLD_PASSES)
				break;
		}
		if (k == sh->nstage)
			break;
		struct rp_state *s = &pass[n];
		const struct rp_stage *stg = &sh->st[k];
		/* an environment stage is not drawn (no tcGen environment yet), but
		   as the first of a late lightmap's: it is what the others blend
		   over, so it is drawn with the surface's coordinates (as the map's
		   own pass does when the lightmap is not late) */
		int env_skip = stg->env && !(late && k == 0);
		if (env_skip || !rp_stage_state(stg, scene_time, s)) {
			st_nostage += !stg->env;
			continue;
		}
		s->prog = RP_PROG_STAGE;
		s->z = k == 0 && stg->blend == RP_BLEND_NONE ? RP_Z_WRITE : RP_Z_TEST;
		rp_stage_tcmod(stg, scene_time, s->tm);
		/* the colour: the vertex's as the map has it, or the stage's own */
		u8 c[4];
		rp_stage_colour(stg, scene_time, NULL, NULL, c);
		int vrgb = stg->rgbgen == RGB_VERTEX || stg->rgbgen == RGB_EXACT_VERTEX, va = stg->alphagen == A_VERTEX;
		/* before a late lightmap: half as bright (the lightmap pass doubles, as the
		   engine's overbright does after its lightmap) */
		float half = late && k < sh->lm_at ? 0.5f : 1.0f;
		for (int a = 0; a < 4; a++) {
			int v = a < 3 ? vrgb : va;
			float h = a < 3 ? half : 1.0f;
			s->cmul[a] = v ? h : 0, s->cadd[a] = v ? 0 : (float)c[a] * h;
		}
		n++;
	}
	return n;
}

static void RP_LoadWorld(const char *name) {
	char path[MAX_OSPATH], base[MAX_QPATH];
	world_free();
	COM_StripExtension(name, base, sizeof base);
	Com_sprintf(path, sizeof path, "%s/%s/%s.pmap", ri.Cvar_Get("fs_basepath", "", 0)->string, BASEGAME, base);
	world_map = mx_vm_map_file(path[0] == '.' && path[1] == '/' ? path + 2 : path, &world_size);
	if (!world_map) {
		ri.Printf(PRINT_WARNING, "ps2: renderer: no %s (tools/ps2map.py makes it): the world is not drawn\n", path);
		return;
	}
	if (rp_world_load(world_map, world_size) < 0) {
		ri.Printf(PRINT_WARNING, "ps2: renderer: %s is not a map of this version (or no memory for it)\n", path);
		mx_vm_unmap_file(world_map);
		world_map = NULL;
		return;
	}
	world_ok = 1;
	rp_light_load(name);
	/* the materials that are scripts: their stages */
	memset(wshader, 0, sizeof wshader);
	sky_shader = 0;
	for (u32 i = 0; i < rp_world_materials() && i < MAX_WMAT; i++) {
		u32 flags = 0;
		const char *mn = rp_world_material(i, &flags);
		if (mn && (flags & (RP_MAT_SCRIPT | RP_MAT_SKY)) && !(flags & RP_MAT_NODRAW))
			wshader[i] = rp_shader_register(mn, flags & RP_MAT_LIGHTMAP ? 0 : -3, 1);
		if (!sky_shader && (flags & RP_MAT_SKY))
			sky_shader = wshader[i];
	}
	rp_world_script(world_passes);
	ri.Printf(PRINT_ALL, "ps2: renderer: world %s, %u KB\n", path, world_size / 1024);
}
static void RP_SetWorldVisData(const byte *vis) {}
static void RP_EndRegistration(void) {}
static int RP_LightForPoint(vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir) { return qfalse; }
static void RP_DrawStretchRaw(int x, int y, int w, int h, int cols, int rows, const byte *data, int client,
                              qboolean dirty) {}
static void RP_UploadCinematic(int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty) {}
static void RP_RegisterFont(const char *fontName, int pointSize, fontInfo_t *font) { memset(font, 0, sizeof *font); }
static void RP_RemapShader(const char *oldShader, const char *newShader, const char *offsetTime) {}
static qboolean RP_GetEntityToken(char *buffer, int size) { return qfalse; }
static qboolean RP_inPVS(const vec3_t p1, const vec3_t p2) { return qtrue; }
static void RP_TakeVideoFrame(int h, int w, byte *captureBuffer, byte *encodeBuffer, qboolean motionJpeg) {}

refexport_t *GetRefAPI(int apiVersion, refimport_t *rimp) {
	static refexport_t re;
	ri = *rimp;
	memset(&re, 0, sizeof re);
	if (apiVersion != REF_API_VERSION) {
		ri.Printf(PRINT_ALL, "Mismatched REF_API_VERSION: expected %i, got %i\n", REF_API_VERSION, apiVersion);
		return NULL;
	}
	re.Shutdown = RP_Shutdown;
	re.BeginRegistration = RP_BeginRegistration;
	re.RegisterModel = RP_RegisterModel;
	re.RegisterSkin = RP_RegisterSkin;
	re.RegisterShader = RP_RegisterShader;
	re.RegisterShaderNoMip = RP_RegisterShaderNoMip;
	re.LoadWorld = RP_LoadWorld;
	re.SetWorldVisData = RP_SetWorldVisData;
	re.EndRegistration = RP_EndRegistration;
	re.ClearScene = RP_ClearScene;
	re.AddRefEntityToScene = RP_AddRefEntityToScene;
	re.AddPolyToScene = RP_AddPolyToScene;
	re.LightForPoint = RP_LightForPoint;
	re.AddLightToScene = RP_AddLightToScene;
	re.AddAdditiveLightToScene = RP_AddLightToScene;
	re.RenderScene = RP_RenderScene;
	re.SetColor = RP_SetColor;
	re.DrawStretchPic = RP_DrawStretchPic;
	re.DrawStretchRaw = RP_DrawStretchRaw;
	re.UploadCinematic = RP_UploadCinematic;
	re.BeginFrame = RP_BeginFrame;
	re.EndFrame = RP_EndFrame;
	re.MarkFragments = RP_MarkFragments;
	re.LerpTag = RP_LerpTag;
	re.ModelBounds = RP_ModelBounds;
	re.RegisterFont = RP_RegisterFont;
	re.RemapShader = RP_RemapShader;
	re.GetEntityToken = RP_GetEntityToken;
	re.inPVS = RP_inPVS;
	re.TakeVideoFrame = RP_TakeVideoFrame;
	return &re;
}
