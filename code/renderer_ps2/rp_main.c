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

   The lights are counted. Each main view's camera goes into quake3.cam
   (ps2_ren_report), for the viewer (tools/q3view) to fly the same path.
   r_ps2draw 0: nothing is drawn, a timedemo then costs what the game side
   costs (game, cgame, client, sound, collision); 1 and a sum of: 2 no world,
   4 no models, 8 no sprites and polys, 16 no view weapon, 32 no pictures
   (what each costs a pass). r_ps2debug: rp_list.h's rp_debug. r_ps2shot n: the picture
   of the timedemo's frame n is saved as quake3.tga and the game quits.
   r_ps2passes 1: the timedemo's passes of beam racing as quake3.pass.
   r_ps2beamat n: beam racing's counts start at the n-th view of the world.
   r_ps2fill: the limit of the sprites' fill (fill_budget). r_ps2marks 0: no marks.
   r_ps2dlight n: the dynamic lights that light the world, at most (0: none). */

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
extern unsigned world_st_fine, world_st_cand;
/* the Game Options menu's: High Quality Sky (r_fastsky 1: no sky, black
   where it is) and Dynamic Lights (r_dynamiclight 0: none on the world; the
   models are lit by them all the same, as the engine's renderer) */
static cvar_t *r_fastsky, *r_dynamiclight;
static cvar_t *r_ps2draw, *r_ps2shot, *r_ps2passes, *r_ps2beamat, *r_ps2fill, *r_ps2marks, *r_ps2dlight, *r_ps2mirror, *r_ps2clean, *r_ps2markrays;

/* ---- the screen ------------------------------------------------------------------- */

/* the game's 1920 columns to the picture's (r_ps2width) */
#define XS ((float)mx_gs.w * (1.0f / 1920.0f))

/* beam racing's records, read by the DMAC: four frames, and four of each of
   the three strips. They hold CALLs (the draws lie elsewhere: rp_list.c): a
   frame of the game is a few KB a strip, and its texture uploads (at most
   rp_tex.c's a frame) fit the frame's own record. */
static u8 rec[4 * 64 * 1024] MX_FAST_BSS MX_ALIGNED(64);
static u8 srec[4 * 3 * 32 * 1024] MX_FAST_BSS MX_ALIGNED(64);
static int screen_on, in_frame;
/* cycles a frame waited for a free record: not the frame's work (sys_ps2.c) */
unsigned ps2_ren_wait;
/* r_ps2width 0: the width from frame to frame (width_choose) */
static int dyn_width, half_now;
static u32 half_frames, width_changes, calm_frames, press_frames, width_asked, width_held;
static cvar_t *r_ps2widthflip; /* a test: the width asked for changes every n frames */
static cvar_t *r_ps2halfat;    /* r_ps2width 0 with the Z in the picture: half the width when the passes take more than this, % of a field */
static int z_in_picture; /* r_ps2ownz 0: the passes' Z is the next strip of the picture */
static cvar_t *r_ps2trilinear;
static u32 tri_frames, tri_changes, tri_calm;

static void screen_init(void) {
	if (screen_on)
		return;
	/* r_ps2mode: what the settings below are when nothing sets them. 0 (the
	   default), for the frame rate: the textures up to 128 texels. 1, for the
	   picture: the textures up to 256 texels (a file of images made with
	   --max 256). Both: 1920 wide and for a time 960 when the GS cannot keep
	   up with the beam (r_ps2width 0), the Z in the picture, a frame shown
	   from the next strip, and strips of 128 lines, the last one the picture's
	   last 156: four passes a field, each with the time of two strips and of
	   what the others leave, so that none is still drawn when the beam comes.
	   r_ps2strip 192: three strips, 0.4 ms of the EE a frame less, but a pass
	   has one strip's time: at 1920 wide 166 of the timedemo's 9200 passes are
	   overtaken by the beam (the strip shows half drawn for a field) for 4 to
	   8 with four strips. r_ps2width 960: half the pixels for the GS, the
	   passes take 7 ms of a field for 12 and none is overtaken, and the
	   textures of 256 fit GS memory. */
	int mode = ri.Cvar_Get("r_ps2mode", "0", CVAR_ARCHIVE | CVAR_LATCH)->integer;
	mx_hires_strip_lines((u32)ri.Cvar_Get("r_ps2strip", "128", CVAR_LATCH)->integer);
	/* r_ps2merge 1 (the default): the picture's last 28 lines are no strip of
	   their own in strips of 128 lines but the fourth's: four passes a field */
	mx_hires_merge_last(ri.Cvar_Get("r_ps2merge", "1", CVAR_LATCH)->integer);
	/* r_ps2ownz 0 (the default): the passes' Z is the next strip of the picture,
	   and all of GS memory but the picture is the textures' (960 wide: 377 pages of 8 KB).
	   A pass has the time the beam is in the strips that are neither its own
	   nor its Z: one strip's of three, two of four. 1: a Z strip of the
	   passes' own (360 KB at 960 wide, 720 KB at 1920): a pass has the time
	   the beam is outside its strip */
	z_in_picture = !ri.Cvar_Get("r_ps2ownz", "0", CVAR_LATCH)->integer;
	mx_hires_own_z(!z_in_picture);
	/* r_ps2skip 1 (the default), with r_ps2ownz 1: a pass that would still be
	   drawn when the beam comes to its strip is left out (the strip shows the
	   frame before for a field, not a half drawn one) */
	mx_hires_skip_late(ri.Cvar_Get("r_ps2skip", "1", CVAR_LATCH)->integer);
	/* r_ps2ahead: the frames that may wait to be shown. 1: what the game draws
	   is on the screen at the next field (2: two fields later at 60 frames a
	   second, and a frame that takes long is covered by one that waited) */
	mx_hires_ahead((u32)ri.Cvar_Get("r_ps2ahead", "1", CVAR_ARCHIVE)->integer);
	/* r_ps2swap 1: a frame is shown from the next strip's pass on, not from
	   the next field: the frames need not keep to the fields (a field may
	   show strips of two frames) */
	mx_hires_swap_strips(ri.Cvar_Get("r_ps2swap", "1", CVAR_LATCH)->integer);
	/* r_ps2width: the picture's width, 1920 or 960 (the display shows each
	   pixel twice: half the framebuffer and the Z, half the fill, the rest of
	   GS memory the textures'). The textures keep the levels of 1920: the
	   lines are as sharp as before. 0 (the default): 1920, and 960 from frame
	   to frame while the frames need it (width_choose): with the Z in the
	   picture the half width is drawn into the same buffer (GS memory stays
	   as it is, the GS fills half the pixels) */
	int wcfg = ri.Cvar_Get("r_ps2width", "0", CVAR_LATCH)->integer;
	u32 width = wcfg == 960 ? 960 : 1920;
	dyn_width = wcfg == 0;
	mx_hires_widths(dyn_width);
	/* r_ps2margin: lines the beam is past a strip before that strip's pass may
	   start. 8 was too few at 960 wide: a pass clears its strip first, there
	   twice as fast, and reached the strip's last lines before the beam had
	   shown them (black lines at the strips' edges, surfaces not drawn yet) */
	mx_hires_margin(ri.Cvar_Get("r_ps2margin", "8", CVAR_LATCH)->integer);
	if (mx_hires_init(&mx_video_1080i, width, 540, MX_GS_PSM_CT16S, rec, sizeof rec, (void *)0x70002000, 0x2000) < 0)
		ri.Error(ERR_FATAL, "ps2: no 1080i, or its buffer does not fit");
	u32 sbytes = mx_hires_strip_records(srec, sizeof srec);
	ri.Printf(PRINT_ALL, "ps2: renderer: 1080i, %u wide, %u strips of %u lines, a strip's record %u KB\n", mx_gs.w,
	          mx_hires_passes(), mx_hires_strip_height(), sbytes / 1024);
	/* r_ps2texfine: the units (32 KB of GS memory, taken from the cache's) of
	   the pool for the first levels of textures of 256 texels, for a file of
	   images made with tools/ps2pak.py --max 256 (0, the default: none, they
	   are drawn from their level of 128; 8: the four to eight textures
	   nearest the eye are as sharp as the PC's) */
	rp_tex_fine_units((u32)ri.Cvar_Get("r_ps2texfine", "0", CVAR_ARCHIVE | CVAR_LATCH)->integer);
	rp_tex_init();
	rp_tex_room((u32)ri.Cvar_Get("r_ps2texroom", "100", 0)->integer);
	rp_tex_max((u32)ri.Cvar_Get("r_ps2texmax", mode ? "256" : "128", 0)->integer);
	rp_tex_force(ri.Cvar_Get("r_ps2texforce", "0", 0)->integer);
	rp_guard_x = (float)ri.Cvar_Get("r_ps2guardx", "2040", 0)->integer;
	rp_guard_y = (float)ri.Cvar_Get("r_ps2guardy", "2040", 0)->integer;
	/* r_ps2trilinear: the textures filtered between their levels (rp_list.h):
	   the GS's work alone, a fifth to a third more of it. 0 (the default):
	   not; 1: always (in a heavy scene a pass is then overtaken now and then,
	   at textures 256 many); 2: while the passes have room for it
	   (trilinear_choose: a quarter of the timedemo's frames, none overtaken,
	   but the frames are 1 % slower) */
	r_ps2trilinear = ri.Cvar_Get("r_ps2trilinear", "0", 0);
	rp_trilinear = r_ps2trilinear->integer == 1;
	/* the stream area: GS memory (pages of 8 KB, taken from the cache) for the
	   images the cache has no room for, sent in the passes (0: none: such an
	   image is placed coarser, or not drawn); the most a frame sends there */
	rp_tex_stream((u32)ri.Cvar_Get("r_ps2texstream", "0", CVAR_LATCH)->integer,
	              (u32)ri.Cvar_Get("r_ps2texstreamkb", "1024", 0)->integer);
	/* the frames' chains: more frames wait to be shown at half the width
	   (their passes take less), and the stream area's chains are more */
	rp_list_init(width < 1920 || dyn_width || rp_tex_stream_pages() ? 1024 * 1024 : 768 * 1024);
	/* the images and the models: two files beside the paks, mapped */
	char path[MAX_OSPATH];
	const char *base = ri.Cvar_Get("fs_basepath", "", 0)->string;
	if (base[0] == '.' && base[1] == '/')
		base += 2;
	else if (base[0] == '.' && !base[1])
		base = "";
	/* the game's directory: baseq3, or the demo's demoq3 (+set com_basegame demoq3) */
	const char *game = ri.Cvar_Get("com_basegame", BASEGAME, 0)->string;
	Com_sprintf(path, sizeof path, "%s%s%s/ps2tex.bin", base, base[0] ? "/" : "", game);
	if (rp_bin_open(&rp_bin_tex, path, 1) < 0)
		ri.Error(ERR_FATAL, "ps2: no %s (tools/ps2bin.py makes it of ps2tex.pk3)", path);
	Com_sprintf(path, sizeof path, "%s%s%s/ps2mdl.bin", base, base[0] ? "/" : "", game);
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
	/* the frames before it are done when it has replaced them (a second at
	   most: a pipeline that is held plays nothing) */
	u32 t0 = mx_count();
	while (mx_frame_done() < f && mx_count() - t0 < MX_EE_HZ)
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
/* the scene's portal surfaces (the game's misc_portal_surface): where, and
   where its camera is (the same place: a mirror) */
#define MAX_PORTAL 8
static struct {
	vec3_t org, cam, axis[3];
	int frame, oldframe, skin;
} portals[MAX_PORTAL];
static u32 nportal;
static unsigned st_mirrors;
static struct {
	qhandle_t shader;
	u16 first, n;
} polys[MAX_POLY];
static struct rp_vert polyverts[MAX_POLYVERT];
static u32 nsprite, npoly, npolyvert;          /* in the frame */
static u32 first_sprite, first_poly;           /* of the scene */
static unsigned st_nostage, st_quads;
static unsigned long long poly_px_sum, poly_px_max;
static float quad_px; /* the frame's quads: the pixels they fill, every stage */
static unsigned poly_px_over, st_fill_over, st_fill_out, st_mark_frags, st_mark_traces, st_mark_known, st_dl_off;
static unsigned long long st_mark_cycles, st_quad_cycles;
/* the rays of this frame's marks (a ray is 36 us on the PS2: a shotgun's
   eleven marks at a corner were 10 ms of one frame, which cost it a field) */
static unsigned mark_rays, st_mark_rays_max, st_mark_over, st_mark_flat;

static void RP_ClearScene(void) {
	cur_ents = cur_polys = cur_polyverts = cur_lights = 0;
	first_sprite = nsprite, first_poly = npoly, first_ment = nment;
	nportal = 0;
	rp_ndlight = 0;
}

static void RP_AddRefEntityToScene(const refEntity_t *e) {
	if ((unsigned)e->reType < RT_MAX_REF_ENTITY_TYPE)
		st.ents[e->reType]++;
	cur_ents++;
	if (!in_frame)
		return;
	if (e->reType == RT_SPRITE && e->radius > 0 && nsprite < MAX_SPRITE)
		sprites[nsprite++] = *e;
	/* the railgun's trail and the lightning: quads between two points */
	if ((e->reType == RT_RAIL_CORE || e->reType == RT_RAIL_RINGS || e->reType == RT_LIGHTNING) && nsprite < MAX_SPRITE)
		sprites[nsprite++] = *e;
	/* (a body seen only in mirrors, RF_THIRD_PERSON, is kept for a mirror's view) */
	if (e->reType == RT_MODEL && nment < MAX_MODEL)
		ments[nment++] = *e;
	if (e->reType == RT_PORTALSURFACE && nportal < MAX_PORTAL) {
		VectorCopy(e->origin, portals[nportal].org);
		VectorCopy(e->oldorigin, portals[nportal].cam);
		memcpy(portals[nportal].axis, e->axis, sizeof portals[nportal].axis);
		portals[nportal].frame = e->frame, portals[nportal].oldframe = e->oldframe, portals[nportal].skin = e->skinNum;
		nportal++;
	}
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
	if (rp_ndlight < RP_DLIGHTS && intensity > 0) {
		struct rp_dlight *l = &rp_dlights[rp_ndlight++];
		VectorCopy(org, l->org);
		l->radius = intensity, l->col[0] = r, l->col[1] = g, l->col[2] = b;
	}
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
	float px;            /* the pixels it fills, about, over its stages */
	float score;         /* a sprite's: px by how little of it shows (0: never left out) */
};
#define MAX_BEAM_QUAD 512
#define MAX_QUAD (MAX_SPRITE + MAX_POLY + MAX_BEAM_QUAD)
static struct quad quad[MAX_QUAD];
static u16 quad_order[MAX_QUAD];
static struct rp_vert sprite_vert[MAX_SPRITE][4];

float rp_sin1(float turns);

/* ---- beams: the railgun's core and rings, the lightning (the engine's
   RB_SurfaceRailCore, RB_SurfaceRailRings, RB_SurfaceLightningBolt) ---- */

static struct rp_vert beam_vert[MAX_BEAM_QUAD][4];
static u32 nbeam;

static void beam_quad(u32 *n, const refEntity_t *e, const struct rp_shader *sh, float now, float (*p)[3], float (*st)[2],
                      float first) {
	if (nbeam == MAX_BEAM_QUAD || *n == MAX_QUAD)
		return;
	struct rp_vert *v = beam_vert[nbeam++];
	for (int k = 0; k < 4; k++) {
		VectorCopy(p[k], v[k].xyz);
		v[k].st[0] = st[k][0], v[k].st[1] = st[k][1];
		for (int c = 0; c < 4; c++)
			v[k].rgba[c] = (u8)((float)e->shaderRGBA[c] * (k == 0 && c < 3 ? first : 1.0f));
	}
	struct quad *q = &quad[(*n)++];
	q->shader = e->customShader, q->sort = sh->sort, q->time = now - e->shaderTime;
	memcpy(q->ent, e->shaderRGBA, 4);
	q->v = v, q->n = 4;
	/* a beam is never left out for its fill, nor as off the screen by this
	   count (a band from beside the eye into the distance) */
	q->px = rp_poly_area(v, 4) * (float)sh->nstage + 1.0f, q->score = 0;
}

/* a band from start to end, span each way along up; its texture repeats
   every 256 units (in pieces of 1024: the 16-bit coordinates) */
static void beam_core(u32 *n, const refEntity_t *e, const struct rp_shader *sh, float now, const float *start,
                      const float *dir, float len, const float *up, float span) {
	for (float at = 0; at < len; at += 1024.0f) {
		float l = len - at < 1024.0f ? len - at : 1024.0f, a[3], b[3], p[4][3], st[4][2] = {{0, 0}, {0, 1}, {0, 1}, {0, 0}};
		VectorMA(start, at, dir, a);
		VectorMA(a, l, dir, b);
		VectorMA(a, span, up, p[0]);
		VectorMA(a, -span, up, p[1]);
		VectorMA(b, -span, up, p[2]);
		VectorMA(b, span, up, p[3]);
		st[2][0] = st[3][0] = l * (1.0f / 256.0f);
		beam_quad(n, e, sh, now, p, st, 0.25f);
	}
}

static void scene_beam(u32 *n, const refdef_t *fd, const refEntity_t *e, const struct rp_shader *sh, float now) {
	float start[3], end[3], dir[3], v1[3], v2[3], right[3];
	if (e->reType == RT_LIGHTNING) {
		VectorCopy(e->origin, start);
		VectorCopy(e->oldorigin, end);
	} else {
		VectorCopy(e->oldorigin, start);
		VectorCopy(e->origin, end);
	}
	VectorSubtract(end, start, dir);
	float len = VectorNormalize(dir);
	if (len < 1.0f)
		return;
	if (e->reType == RT_RAIL_RINGS) {
		/* a small quad across the trail every 32 units */
		float up[3], pos[4][3], step[3];
		static const float st[4][2] = {{1, 0}, {1, 1}, {0, 1}, {0, 0}};
		int segs = (int)(len * (1.0f / 32.0f));
		segs = segs <= 0 ? 1 : segs;
		PerpendicularVector(right, dir);
		CrossProduct(dir, right, up);
		VectorScale(dir, 32.0f, step);
		for (int i = 0; i < 4; i++) {
			float c = rp_sin1((45.0f + i * 90.0f) * (1.0f / 360.0f) + 0.25f), sn = rp_sin1((45.0f + i * 90.0f) * (1.0f / 360.0f));
			for (int k = 0; k < 3; k++)
				pos[i][k] = start[k] + (right[k] * c + up[k] * sn) * 4.0f + (segs > 1 ? step[k] : 0);
		}
		if (segs > 1)
			segs--;
		for (int i = 0; i < segs && i < 96; i++) {
			beam_quad(n, e, sh, now, pos, (float (*)[2])st, 1.0f);
			for (int j = 0; j < 4; j++)
				VectorAdd(pos[j], step, pos[j]);
		}
		return;
	}
	/* the band faces the eye: across the plane of the eye and its two ends */
	VectorSubtract(start, fd->vieworg, v1);
	VectorNormalize(v1);
	VectorSubtract(end, fd->vieworg, v2);
	VectorNormalize(v2);
	CrossProduct(v1, v2, right);
	if (VectorNormalize(right) == 0)
		return;
	if (e->reType == RT_RAIL_CORE) {
		beam_core(n, e, sh, now, start, dir, len, right, 6.0f);
		return;
	}
	/* the lightning: four bands, each turned 45 degrees about the bolt */
	for (int i = 0; i < 4; i++) {
		float side[3], t[3];
		beam_core(n, e, sh, now, start, dir, len, right, 8.0f);
		CrossProduct(dir, right, side);
		for (int k = 0; k < 3; k++)
			t[k] = right[k] * 0.70710678f + side[k] * 0.70710678f;
		VectorCopy(t, right);
	}
}

/* the shaders of the map's materials that are scripts (RP_LoadWorld) */
#define MAX_WMAT 1024
static qhandle_t wshader[MAX_WMAT];

/* The pixels the scene's quads may fill this frame. A cloud of blood before
   the eye is a hundred sprites, six times the screen: with the rest of a
   busy scene more than the GS draws in a field, and the strips come late.
   r_ps2fill 1 (the default): what the field's time leaves, from what the
   passes took (the sum of the strips' last passes) less what the quads of
   those frames cost; 0: no limit; more: that many hundredths of the screen. */
#define FILL_US 0.0019f /* a pixel of a blended sprite, measured: 1.9 ms a million */
static float fill_was[2]; /* what the last frame's quads filled, and the one's before */

/* what the passes of a field took, us: the sum of the strips' last passes */
static float strip_us[MX_HIRES_PASSES];
static float field_passes_us(void) {
	static u32 was_n[MX_HIRES_PASSES], was_sum[MX_HIRES_PASSES];
	float t = 0;
	u32 np = mx_hires_passes();
	for (u32 k = 0; k < np && k < MX_HIRES_PASSES; k++) {
		u32 n = mx_hires_stats.pass_n[k], sum = mx_hires_stats.pass_sum_us[k];
		if (n > was_n[k])
			strip_us[k] = (float)(sum - was_sum[k]) / (float)(n - was_n[k]);
		was_n[k] = n, was_sum[k] = sum;
		t += strip_us[k];
	}
	return t;
}

/* the pixels the field's time has room for, beside the rest of the frame */
static float fill_room(void) {
	float t = field_passes_us();
	/* the frames those passes showed: the last or the one before (the smaller
	   fill: the larger rest) */
	float had = fill_was[0] < fill_was[1] ? fill_was[0] : fill_was[1];
	float rest = t - FILL_US * had;
	if (z_in_picture && mx_hires_passes() == 3) {
		/* three strips with the Z in the picture: a pass has its strip's time
		   and cannot borrow, so the room is the longest pass's, of which the
		   quads' part is taken as six tenths of their fill (a cloud is mostly
		   in one strip) */
		float worst = 0;
		for (u32 k = 0; k < 3; k++)
			worst = strip_us[k] > worst ? strip_us[k] : worst;
		rest = worst - 0.6f * FILL_US * had;
		return (0.8f * 1e6f / (float)mx_gs.hz / 3.0f - (rest > 0 ? rest : 0)) / (0.6f * FILL_US);
	}
	return (0.88f * 1e6f / (float)mx_gs.hz - (rest > 0 ? rest : 0)) / FILL_US;
}

/* The dynamic lights that light the world this frame, into out (r_ps2dlight
   at most): a light draws the chunks near it again, where its spot is. Each
   is taken for the pixels of its sphere on the screen (most of the screen
   when the eye is in it: a shot's flash), and the lights together may have
   six tenths of the room; one that does not fit is left out (the models
   are lit by it all the same). */
static float light_px; /* what the frame's lights take */
static unsigned dlights_choose(struct rp_dlight *out) {
	const struct rp_view *vw = &rp_view;
	float screen = (float)(mx_gs.w * mx_gs.h), room = r_ps2fill->integer == 1 ? 0.6f * fill_room() : 1e30f;
	unsigned n = 0, max = (unsigned)r_ps2dlight->integer;
	for (unsigned i = 0; i < rp_ndlight && n < max; i++) {
		const struct rp_dlight *l = &rp_dlights[i];
		float d[3] = {l->org[0] - vw->org[0], l->org[1] - vw->org[1], l->org[2] - vw->org[2]};
		float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), R = l->radius, px = 0.6f * screen;
		if (dist > R) {
			/* all of it behind the eye */
			if (d[0] * vw->F[0] + d[1] * vw->F[1] + d[2] * vw->F[2] < -R)
				continue;
			float a = 2.0f * (R * vw->fx / dist) * (R * vw->fy / dist);
			px = a < px ? a : px;
		}
		if (light_px + px > room) {
			st_dl_off++;
			continue;
		}
		out[n++] = *l;
		light_px += px;
	}
	return n;
}

static float fill_budget(void) {
	int f = r_ps2fill->integer;
	float screen = (float)(mx_gs.w * mx_gs.h);
	if (f <= 0)
		return 1e30f;
	if (f > 1)
		return (float)f * 0.01f * screen;
	/* what the lights left, and half the screen whatever the room */
	float b = fill_room() - light_px;
	return b < 0.5f * screen ? 0.5f * screen : b > 8.0f * screen ? 8.0f * screen : b;
}

/* a scene's quads; world: with the world's quads that turn to the eye, of the
   view's cull */
static u32 scene_quads(const refdef_t *fd, int world) {
	u32 n = 0;
	float now = fd->time * 0.001f;
	nbeam = 0;
	for (u32 i = first_poly; i < npoly; i++) {
		struct rp_shader *sh = rp_shader_get(polys[i].shader);
		if (!sh || !sh->nstage)
			continue;
		struct quad *q = &quad[n++];
		q->shader = polys[i].shader, q->sort = sh->sort, q->time = now;
		q->ent[0] = q->ent[1] = q->ent[2] = q->ent[3] = 255;
		q->v = &polyverts[polys[i].first], q->n = polys[i].n;
		q->px = rp_poly_area(q->v, q->n) * (float)sh->nstage, q->score = 0;
	}
	for (u32 i = first_sprite; i < nsprite; i++) {
		const refEntity_t *e = &sprites[i];
		struct rp_shader *sh = rp_shader_get(e->customShader);
		if (!sh || !sh->nstage)
			continue;
		if (e->reType != RT_SPRITE) {
			scene_beam(&n, fd, e, sh, now);
			continue;
		}
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
		/* what shows of it: its alpha, or its colour where that fades */
		u32 c = e->shaderRGBA[0] > e->shaderRGBA[1] ? e->shaderRGBA[0] : e->shaderRGBA[1];
		c = c > e->shaderRGBA[2] ? c : e->shaderRGBA[2];
		c = c < e->shaderRGBA[3] ? c : e->shaderRGBA[3];
		q->px = rp_poly_area(v, 4) * (float)sh->nstage;
		q->score = q->px / (float)(c + 8) + 1e-6f;
	}
	/* the world's quads that turn to the eye (lamps' glows) */
	if (world) {
		const float *rec;
		u32 mat, cnt;
		for (u32 i = 0; (cnt = rp_world_sprites(i, &rec, &mat)) != 0; i++) {
			qhandle_t hs = mat < MAX_WMAT ? wshader[mat] : 0;
			struct rp_shader *sh = rp_shader_get(hs);
			if (!sh || !sh->nstage)
				continue;
			for (u32 k = 0; k < cnt && nbeam < MAX_BEAM_QUAD && n < MAX_QUAD; k++, rec += 16) {
				struct rp_vert *v = beam_vert[nbeam++];
				float a[3], b[3];
				if (rec[3] > 0) {
					/* a square facing the eye */
					VectorScale(fd->viewaxis[1], rec[3], a);
					VectorScale(fd->viewaxis[2], rec[3], b);
				} else {
					/* turned about its line: its width across the line and the eye's ray */
					float ray[3];
					VectorSubtract(rec, fd->vieworg, ray);
					VectorCopy(rec + 4, a);
					CrossProduct(a, ray, b);
					if (VectorNormalize(b) == 0)
						VectorCopy(fd->viewaxis[1], b);
					VectorScale(b, rec[7], b);
				}
				/* the corners: (a square) left and up as a sprite's; (a line) its first
				   end to either side, then its second end, the same sides round */
				static const float sq[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}}, ln[4][2] = {{-1, 1}, {-1, -1}, {1, -1}, {1, 1}};
				const float (*sg)[2] = rec[3] > 0 ? sq : ln;
				for (int c = 0; c < 4; c++) {
					for (int x = 0; x < 3; x++)
						v[c].xyz[x] = rec[x] + sg[c][0] * a[x] + sg[c][1] * b[x];
					v[c].st[0] = rec[8 + 2 * c], v[c].st[1] = rec[9 + 2 * c];
					v[c].rgba[0] = v[c].rgba[1] = v[c].rgba[2] = v[c].rgba[3] = 255;
				}
				struct quad *q = &quad[n++];
				q->shader = hs, q->sort = sh->sort, q->time = now;
				q->ent[0] = q->ent[1] = q->ent[2] = q->ent[3] = 255;
				q->v = v, q->n = 4;
				q->px = rp_poly_area(v, 4) * (float)sh->nstage, q->score = 0;
			}
		}
	}
	/* Over the frame's limit (fill_budget) sprites are left out: first the ones
	   that fill the most for what shows of them (smoke and blood that has
	   grown and faded). */
	float fill = 0, hard = 0, top = 0, limit = 1e30f;
	for (u32 i = 0; i < n; i++) {
		fill += quad[i].px;
		if (quad[i].score == 0)
			hard += quad[i].px;
		else if (quad[i].score > top)
			top = quad[i].score;
	}
	float budget = fill_budget();
	if (fill > budget) {
		/* the highest score that stays: by halving */
		float lo = 0, hi = top;
		for (int it = 0; it < 12; it++) {
			float mid = (lo + hi) * 0.5f, sum = hard;
			for (u32 i = 0; i < n; i++)
				if (quad[i].score != 0 && quad[i].score <= mid)
					sum += quad[i].px;
			if (sum <= budget)
				lo = mid;
			else
				hi = mid;
		}
		limit = lo;
		st_fill_over++;
	}
	/* by sort, the order kept (few, and nearly in order: insertion) */
	u32 m = 0;
	for (u32 i = 0; i < n; i++) {
		if (quad[i].score > limit) {
			st_fill_out++;
			continue;
		}
		/* not on the screen: no stage of it is worked out */
		if (quad[i].px <= 0)
			continue;
		u32 j = m++;
		for (; j > 0 && quad[quad_order[j - 1]].sort > quad[i].sort; j--)
			quad_order[j] = quad_order[j - 1];
		quad_order[j] = (u16)i;
	}
	return m;
}

/* the quads from..to of the order: the ones of one shader after each other
   are drawn a stage at a time */
static void quads_draw(u32 from, u32 to) {
	u32 t0 = mx_count();
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
				if (sh->polygon_offset) {
					/* a mark at the eye (the shadow under the player's own feet, a burn
					   he stands on) is not drawn: across the eye's plane it would be
					   cut, and what a cut leaves wrong of a mark darkens the picture */
					const struct rp_view *vw = &rp_view;
					int at_eye = 0;
					for (u32 a = 0; a < q->n; a++)
						at_eye |= (q->v[a].xyz[0] - vw->org[0]) * vw->F[0] + (q->v[a].xyz[1] - vw->org[1]) * vw->F[1] +
						          (q->v[a].xyz[2] - vw->org[2]) * vw->F[2] < 16.0f;
					if (at_eye)
						continue;
				}
				for (u32 a = 0; a < q->n; a++) {
					v[a] = q->v[a];
					if (a && !memcmp(q->v[a].rgba, q->v[0].rgba, 4))
						memcpy(v[a].rgba, v[0].rgba, 4);
					else
						rp_stage_colour(stg, q->time, q->v[a].rgba, q->ent, v[a].rgba);
					if (sh->polygon_offset) {
						/* on a surface (a mark): towards the eye along its ray by two steps
						   of the depth buffer there (16 bits, 262140 / w), so it is in
						   front of its wall wherever both are rounded */
						const struct rp_view *vw = &rp_view;
						float d[3] = {v[a].xyz[0] - vw->org[0], v[a].xyz[1] - vw->org[1], v[a].xyz[2] - vw->org[2]};
						float w = d[0] * vw->F[0] + d[1] * vw->F[1] + d[2] * vw->F[2];
						if (w > 8.0f) {
							float f = 1.0f - (w * (2.0f / 262140.0f) + 0.25f / w);
							for (int x = 0; x < 3; x++)
								v[a].xyz[x] = vw->org[x] + d[x] * f;
						}
					}
					if (moved) {
						float ss = v[a].st[0], tt = v[a].st[1];
						v[a].st[0] = m[0] * ss + m[1] * tt + m[2], v[a].st[1] = m[3] * ss + m[4] * tt + m[5];
					}
				}
				rp_poly(v, q->n);
				st_quads++;
				if (s == 0)
					quad_px += q->px;

			}
		}
		i = end;
	}
	rp_poly_flush();
	st_quad_cycles += mx_count() - t0;
}

static struct {
	qhandle_t shader;
	int stage;
	u32 rgba;   /* the last picture's colour as the GS takes it, for colour */
	u32 colour; /* color2d as one word */
	int plain;  /* one stage, its texture coordinates as given */
} last2d;

/* A mirror in the view (a chunk of the world whose script is a portal's, at
   a misc_portal_surface without a camera): the scene once more from the eye
   mirrored in its plane, before the view itself, and the depth of it taken
   away again. The view's own surfaces then cover all of it but where the
   mirror is, whose stages are laid over what is there. What is behind the
   mirror's plane is left out by chunk and by model (the plane as that
   view's near one): not cut. Its quads (sprites, marks), lights and fog are
   not in it. 1: drawn (the view is to be set again). */
static int mirror_is(u32 mat) {
	const struct rp_shader *sh = mat < MAX_WMAT ? rp_shader_get(wshader[mat]) : NULL;
	return sh && sh->sort == SORT_PORTAL;
}

/* the engine's portal transform (R_GetPortalOrientations, R_MirrorPoint):
   a point of the view's world as seen through the surface from the camera */
struct portal_or {
	vec3_t sorg, saxis[3], corg, caxis[3];
};
static void portal_point(const struct portal_or *o, const float *v, float *out) {
	vec3_t l;
	VectorSubtract(v, o->sorg, l);
	VectorCopy(o->corg, out);
	for (int i = 0; i < 3; i++)
		VectorMA(out, DotProduct(l, o->saxis[i]), o->caxis[i], out);
}
static void portal_vector(const struct portal_or *o, const float *v, float *out) {
	VectorClear(out);
	for (int i = 0; i < 3; i++)
		VectorMA(out, DotProduct(v, o->saxis[i]), o->caxis[i], out);
}

/* the eye's distance to the nearest portal surface (alphaGen portal); far
   (the haze opaque) when no view through a portal was drawn this scene */
static int portal_drawn;
float rp_portal_dist(const float *eye) {
	float best = 1e9f;
	if (!portal_drawn)
		return best;
	for (u32 i = 0; i < nportal; i++) {
		float d = Distance(eye, portals[i].org);
		best = d < best ? d : best;
	}
	return best;
}

static int mirror_view(const refdef_t *fd, const struct rp_cam *cam, float ys, float now, float *pl) {
	int pick = -1;
	float eye = 0;
	u32 from = 0;
	/* the first surface of a portal in view that the eye is before and that
	   has a portal's entity on it (a mirror: its camera at its own place) */
	while (pick < 0 && nportal && rp_world_find(mirror_is, pl, &from)) {
		eye = DotProduct(pl, cam->org) - pl[3];
		for (u32 i = 0; i < nportal && eye >= 1.0f && pick < 0; i++) {
			float d = DotProduct(pl, portals[i].org) - pl[3];
			if (d > -64.0f && d < 64.0f)
				pick = (int)i;
		}
	}
	if (pick < 0)
		return 0;
	int mirror = VectorCompare(portals[pick].org, portals[pick].cam);
	if (mirror ? !(r_ps2mirror->integer & 1) : !(r_ps2mirror->integer & 4))
		return 0;
	/* the opening: the surface's chunks' box on its plane, its corners */
	float lo[3], hi[3], cn[4][3], mid[3] = {0, 0, 0};
	rp_world_bounds(mirror_is, pl, lo, hi);
	if (lo[0] > hi[0])
		return 0;
	int k = fabsf(pl[0]) > fabsf(pl[1]) ? (fabsf(pl[0]) > fabsf(pl[2]) ? 0 : 2) : (fabsf(pl[1]) > fabsf(pl[2]) ? 1 : 2);
	int u = (k + 1) % 3, v = (k + 2) % 3;
	for (int c = 0; c < 4; c++) {
		cn[c][u] = (c == 1 || c == 2) ? hi[u] : lo[u];
		cn[c][v] = c >= 2 ? hi[v] : lo[v];
		cn[c][k] = (pl[3] - pl[u] * cn[c][u] - pl[v] * cn[c][v]) / pl[k];
		for (int a = 0; a < 3; a++)
			mid[a] += cn[c][a] * 0.25f;
	}
	/* a second scene is as much as the first where it covers the picture:
	   only while the opening covers a quarter of the picture or less
	   (r_ps2mirror 2: always) */
	if (!(r_ps2mirror->integer & 2)) {
		struct rp_vert q[4];
		memset(q, 0, sizeof q);
		for (int c = 0; c < 4; c++)
			VectorCopy(cn[c], q[c].xyz);
		if (rp_poly_area(q, 4) > 0.25f * (float)mx_gs.w * (float)mx_gs.h)
			return 0;
	}
	struct portal_or o;
	VectorCopy(pl, o.saxis[0]);
	PerpendicularVector(o.saxis[1], o.saxis[0]);
	CrossProduct(o.saxis[0], o.saxis[1], o.saxis[2]);
	if (mirror) {
		VectorScale(pl, pl[3], o.sorg);
		VectorCopy(o.sorg, o.corg);
		VectorNegate(o.saxis[0], o.caxis[0]);
		VectorCopy(o.saxis[1], o.caxis[1]);
		VectorCopy(o.saxis[2], o.caxis[2]);
	} else {
		float d = DotProduct(portals[pick].org, pl) - pl[3];
		VectorMA(portals[pick].org, -d, o.saxis[0], o.sorg);
		VectorCopy(portals[pick].cam, o.corg);
		memcpy(o.caxis, portals[pick].axis, sizeof o.caxis);
		VectorNegate(o.caxis[0], o.caxis[0]);
		VectorNegate(o.caxis[1], o.caxis[1]);
		float rot = 0;
		int turn = 1;
		if (portals[pick].oldframe)
			rot = portals[pick].frame ? now * (float)portals[pick].frame
			                          : (float)portals[pick].skin + sinf(now * 3.0f) * 4.0f;
		else if (portals[pick].skin)
			rot = (float)portals[pick].skin;
		else
			turn = 0;
		if (turn) {
			vec3_t t;
			VectorCopy(o.caxis[1], t);
			RotatePointAroundVector(o.caxis[1], o.caxis[0], t, rot);
			CrossProduct(o.caxis[0], o.caxis[1], o.caxis[2]);
		}
	}
	struct rp_cam m = *cam;
	portal_point(&o, cam->org, m.org);
	for (int i = 0; i < 3; i++)
		portal_vector(&o, cam->axis[i], m.axis[i]);
	rp_view_rect(&m, (float)fd->x * XS, (float)fd->y * ys, (float)fd->width * XS, (float)fd->height * ys);
	/* what is before the camera's plane only (the engine's portalPlane) */
	VectorNegate(o.caxis[0], rp_view.fr[0].n);
	rp_view.fr[0].d = DotProduct(o.corg, rp_view.fr[0].n), rp_view.fr_len[0] = 1.0f;
	/* what is seen through the opening only: planes from the camera's eye
	   through the opening's edges, as the camera sees them */
	{
		float tc[4][3], tm[3];
		for (int c = 0; c < 4; c++)
			portal_point(&o, cn[c], tc[c]);
		portal_point(&o, mid, tm);
		for (int c = 0; c < 4; c++) {
			float e0[3], e1[3], n[3];
			VectorSubtract(tc[c], m.org, e0);
			VectorSubtract(tc[(c + 1) & 3], m.org, e1);
			CrossProduct(e0, e1, n);
			float d = DotProduct(n, m.org), len = VectorLength(n);
			if (len <= 0)
				continue;
			if (DotProduct(n, tm) - d < 0)
				VectorNegate(n, n), d = -d;
			VectorCopy(n, rp_view.fr[1 + c].n);
			rp_view.fr[1 + c].d = d, rp_view.fr_len[1 + c] = len;
		}
	}
	rp_view_mirror = mirror;
	float room[3];
	if (mirror)
		VectorMA(portals[pick].org, 8.0f, pl, room);
	else
		VectorCopy(o.corg, room);
	rp_world_pvs(room);
	struct rp_world_stats ws;
	memset(&ws, 0, sizeof ws);
	rp_world_cull(&ws);
	rp_world_add(0);
	for (int blended = 0; blended < 2; blended++) {
		for (u32 i = first_ment; i < nment; i++) {
			const refEntity_t *e = &ments[i];
			if (e->renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK))
				continue;
			if (rp_model_brush(e->hModel))
				rp_world_bmodel((u32)rp_model_brush(e->hModel), e->origin, e->axis, blended);
			else if ((rp_model_blended(e) != 0) == blended)
				rp_model_draw(e, now - e->shaderTime);
		}
		if (!blended) {
			u32 sky_strips = sky_shader ? rp_world_sky() : 0;
			const struct rp_shader *sky = sky_strips ? rp_shader_get(sky_shader) : NULL;
			if (sky && !r_fastsky->integer)
				rp_sky_draw(sky, now, sky_strips);
			rp_world_add(1);
		}
	}
	rp_poly_flush();
	rp_zclear((float)fd->x * XS, (float)fd->y * ys, (float)(fd->x + fd->width) * XS, (float)(fd->y + fd->height) * ys);
	rp_view_mirror = 0;
	rp_world_pvs(NULL);
	st_mirrors++;
	portal_drawn = !mirror;
	return 1;
}

/* r_ps2shotload n: the picture of the screen while the n-th level of the run
   was loaded (the last frame before the level's first view), then quit */
static cvar_t *r_ps2shotload;
static u32 load_views, loads;

static void RP_RenderScene(const refdef_t *fd) {
	st.scenes++;
	int world = !(fd->rdflags & RDF_NOWORLDMODEL);
	if (world)
		st.views++;
	if (world && !load_views++ && r_ps2shotload && r_ps2shotload->integer && ++loads == (u32)r_ps2shotload->integer) {
		int r = mx_hires_save_tga("quake3.tga");
		ri.Printf(PRINT_ALL, "ps2: renderer: the picture of load %u: quake3.tga (%d)\n", loads, r);
		ri.Cmd_ExecuteText(EXEC_APPEND, "quit\n");
	}
	/* r_ps2beamat n: beam racing's counts start at the n-th view of the world
	   (a run's report is then of a place, not of the load before it) */
	if (world && r_ps2beamat->integer && st.views == (unsigned)r_ps2beamat->integer)
		memset(&mx_hires_stats, 0, sizeof mx_hires_stats);
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
		/* a small scene (the head on the HUD, an item's icon) is as large on the
		   screen as it ever gets: its images whole, also while the frame's
		   images are more than GS memory holds */
		rp_tex_full = !world;
		float ys = (float)mx_gs.h / 1080.0f;
		rp_view_rect(&cam, (float)fd->x * XS, (float)fd->y * ys, (float)fd->width * XS, (float)fd->height * ys);
		float now = fd->time * 0.001f;
		scene_time = now;
		rp_scene_world = world;
		int skip = r_ps2draw->integer;
		int draw_world = world && world_ok && !(skip & 2);
		if (draw_world) {
			rp_world_areas(fd->areamask);
			rp_world_cull(&world_st);
			float mpl[4];
			portal_drawn = 0;
			if ((r_ps2mirror->integer & 5) && mirror_view(fd, &cam, ys, now, mpl)) {
				rp_view_rect(&cam, (float)fd->x * XS, (float)fd->y * ys, (float)fd->width * XS, (float)fd->height * ys);
				rp_world_cull(&world_st);
				/* r_ps2debug 33554432: the mirror's view alone */
				if (rp_debug & 33554432)
					draw_world = 0, skip |= 4 | 8;
				else
					rp_world_depth(mirror_is, mpl);
			}
			if (draw_world)
				rp_world_add(0);
			/* the dynamic lights on it: after its depth is there, before what stands in it */
			if (r_dynamiclight->integer && r_ps2dlight->integer && rp_ndlight) {
				static struct rp_dlight lit[RP_DLIGHTS];
				unsigned n = dlights_choose(lit);
				rp_world_dlights(lit, n, n);
			}
			if (!(rp_debug & 4194304))
				rp_world_fog();
#define ACC(f) world_sum.f += world_st.f
			ACC(seen), ACC(groups), ACC(drawn), ACC(clip), ACC(back), ACC(verts);
#undef ACC
		}
		/* the scene's quads (after the cull: the world's own are of this view) */
		u32 nq = scene_quads(fd, draw_world), decals = 0;
		while (decals < nq && quad[quad_order[decals]].sort <= SORT_DECAL)
			decals++;
		/* a scene of its own on the screen (the HUD's models): no depth is there yet */
		if (!world)
			rp_zclear((float)fd->x * XS, (float)fd->y * ys, (float)(fd->x + fd->width) * XS, (float)(fd->y + fd->height) * ys);
		/* the models that write depth; the marks on the walls; the world's
		   blended surfaces; the models that blend, and the rest; last the view
		   weapon, over depth of its own (the engine squeezes its depth range) */
		int hack = 0;
		static u8 kind[MAX_MODEL]; /* 1: its shader blends; 2: the view weapon's depth */
		for (u32 i = first_ment; i < nment; i++) {
			const refEntity_t *e = &ments[i];
			/* a body seen only in mirrors is not this view's */
			if (e->renderfx & RF_THIRD_PERSON) {
				kind[i] = 4;
				continue;
			}
			/* the map's doors and platforms: drawn as the world is */
			if (rp_model_brush(e->hModel)) {
				kind[i] = 8;
				if (draw_world)
					rp_world_bmodel((u32)rp_model_brush(e->hModel), e->origin, e->axis, 0);
				continue;
			}
			kind[i] = (u8)(rp_model_blended(e) | (e->renderfx & RF_DEPTHHACK ? 2 : 0));
			if ((skip & 4) || ((skip & 16) && (kind[i] & 2)))
				kind[i] = 4;
			else if (kind[i] & 2)
				hack = 1;
			else if (!kind[i])
				rp_model_draw(e, now - e->shaderTime);
		}
		/* the sky, behind what was drawn, where the view sees the map's sky
		   surfaces: after all that writes depth (a pixel that fails the
		   depth test costs the GS less than one drawn and blended) */
		if (draw_world) {
			u32 sky_strips = sky_shader ? rp_world_sky() : 0;
			const struct rp_shader *sky = sky_strips ? rp_shader_get(sky_shader) : NULL;
			if (sky && !r_fastsky->integer && !(rp_debug & 524288))
				rp_sky_draw(sky, now, sky_strips);
		}
		if (skip & 8)
			nq = decals = 0;
		quads_draw(0, decals);
		if (draw_world) {
			rp_world_add(1);
			for (u32 i = first_ment; i < nment; i++)
				if (kind[i] == 8)
					rp_world_bmodel((u32)rp_model_brush(ments[i].hModel), ments[i].origin, ments[i].axis, 1);
		}
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
			rp_tex_full = 1;
			for (u8 blended = 0; blended < 2; blended++)
				for (u32 i = first_ment; i < nment; i++)
					if (kind[i] == (2 | blended))
						rp_model_draw(&ments[i], now - ments[i].shaderTime);
			rp_tex_full = 0;
		}
		rp_tex_full = 0;
	}
	first_sprite = nsprite, first_poly = npoly, first_ment = nment;
	nportal = 0;
	rp_ndlight = 0;
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
		rp_pic(x * XS, y * ys, w * XS, h * ys, s1, t1, s2, t2, last2d.rgba);
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
		rp_pic(x * XS, y * ys, w * XS, h * ys, a1, b1, a2, b2, rgba);
		last2d.rgba = rgba, last2d.colour = col;
		last2d.plain = sh->nstage == 1 && stg->nimg == 1 && !stg->ntcmod && last2d.shader == shader &&
		               stg->rgbgen != RGB_WAVE && stg->alphagen != A_WAVE;
	}
}


/* r_ps2width 0: half the width (960) while the frames need more than the
   whole width leaves: textures that are placed coarser than their distance
   asks (the governor's bias), or passes that take more than 80 % of a
   field, ten frames in a row; the whole width again after two seconds at
   half in which the frames' textures at their full size would fit the
   whole width's GS memory (85 %) and the passes (twice the pixels) would
   take under 60 % of a field. */
static void width_choose(void) {
	if (r_ps2widthflip && r_ps2widthflip->integer < 0) {
		/* a test: the whole width, -2 half, always */
		width_asked = r_ps2widthflip->integer == -2 ? 960 : 1920;
		mx_hires_width(width_asked);
		return;
	}
	if (r_ps2widthflip && r_ps2widthflip->integer > 0) {
		static u32 n;
		width_asked = (n++ / (u32)r_ps2widthflip->integer) & 1 ? 1920 : 960;
		mx_hires_width(width_asked);
		return;
	}
	float field = 1e6f / (float)mx_gs.hz, passes = field_passes_us();
	if (z_in_picture) {
		/* the Z in the picture: the half width gives the textures no room, only
		   the GS less to fill (the passes at the whole width take 1.65 times
		   those at half: measured). Half at once when a pass was left out or
		   overtaken by the beam, or when the passes took more than 95 % of a
		   field (r_ps2halfat); for a second at least, and the whole width
		   again when the passes there would take under 80 % of a field (their
		   mean over some eight frames; 15 % less than r_ps2halfat) and the
		   last frame's under 90 %. In the timedemo: 44 % of the frames at
		   half, and of 9900 passes none overtaken (textures of 128) or 5
		   (textures of 256) for 6 and 16 to 24 at the whole width */
		static u32 was_late, hold;
		static float mean;
		float at = (r_ps2halfat ? r_ps2halfat->value : 95.0f) * 0.01f;
		u32 late = mx_hires_stats.late + mx_hires_stats.over;
		int hit = late != was_late;
		was_late = late;
		if (!half_now) {
			width_asked = hit || passes > at * field ? 960 : 1920;
			if (width_asked == 960)
				hold = 60, mean = passes;
		} else {
			mean += (passes - mean) * 0.125f;
			if (hit)
				hold = 60;
			hold -= hold > 0;
			width_asked = !hold && 1.65f * mean < (at - 0.15f) * field && 1.65f * passes < (at - 0.05f) * field ? 1920 : 960;
		}
		mx_hires_width(width_asked);
		return;
	}
	int pressed = rp_tex_far_bias() >= 0.5f || passes > 0.8f * field;
	if (!half_now) {
		press_frames = pressed ? press_frames + 1 : 0;
		width_asked = press_frames >= 10 ? 960 : 1920;
		if (press_frames >= 10)
			press_frames = 0, calm_frames = 0;
		mx_hires_width(width_asked);
		return;
	}
	int fits = rp_tex_want_blocks() * 100 < rp_tex_full_blocks() * 85 && 1.6f * passes < 0.6f * field &&
	           rp_tex_far_bias() == 0;
	calm_frames = fits ? calm_frames + 1 : 0;
	width_asked = calm_frames >= 120 ? 1920 : 960;
	mx_hires_width(width_asked);
}

/* r_ps2trilinear 2: the GS is idle for most of a field, so while the passes
   are short the textures are filtered between their levels; when the passes
   take more than 55 % of a field together, or one of them 65 % of the time a
   pass has, not, until ten frames in a row would have had room for it (the
   passes then are a third longer: under 50 % and 55 %). The passes' times are those of the
   frames shown, a frame or two ago. */
static void trilinear_choose(void) {
	int v = r_ps2trilinear ? r_ps2trilinear->integer : 0;
	u32 np = mx_hires_passes();
	if (v != 2 || !np) {
		rp_trilinear = v == 1;
		return;
	}
	float field = 1e6f / (float)mx_gs.hz, passes = field_passes_us(), worst = 0;
	for (u32 k = 0; k < np && k < MX_HIRES_PASSES; k++)
		worst = strip_us[k] > worst ? strip_us[k] : worst;
	/* the time a pass has: the strips that are neither its own nor its Z */
	float window = field * (z_in_picture ? (float)(np - 2) : (float)(np - 1)) / (float)np;
	if (rp_trilinear) {
		if (passes > 0.55f * field || worst > 0.65f * window)
			rp_trilinear = 0, tri_calm = 0, tri_changes++;
	} else {
		tri_calm = 1.3f * passes < 0.50f * field && 1.3f * worst < 0.55f * window ? tri_calm + 1 : 0;
		if (tri_calm >= 10)
			rp_trilinear = 1, tri_changes++;
	}
	tri_frames += (u32)rp_trilinear;
}

static void RP_BeginFrame(stereoFrame_t stereoFrame) {
	if (!screen_on || in_frame)
		return;
	if (dyn_width)
		width_choose();
	u32 t0 = mx_count();
	mx_frame_begin_n();
	ps2_ren_wait += mx_count() - t0;
	in_frame = 1;
	if (dyn_width) {
		int half = mx_gs.w < 1920;
		width_changes += half != half_now;
		width_held += half && width_asked == 1920;
		half_now = half, half_frames += half;
	}
	if (mark_rays > st_mark_rays_max)
		st_mark_rays_max = mark_rays;
	mark_rays = 0;
	/* the stream area's share of the field: what the passes leave of 80 % */
	if (mx_hires_passes()) {
		static u32 was_late;
		u32 late = mx_hires_stats.late + mx_hires_stats.over;
		rp_tex_stream_room(0.80f * 1e6f / (float)mx_gs.hz - field_passes_us(), late != was_late);
		was_late = late;
	}
	trilinear_choose();
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
	u16 quad_kpx; /* the quads' fill, thousands of pixels */
	u16 sent[8], batches[8], setups[8], msent[8], mbatches[8], draws[8], stream_kb[8];
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
	f->quad_kpx = (u16)(quad_px * 0.001f);
	for (int k = 0; k < 8; k++) {
		f->sent[k] = (u16)rp_list_stats.k_sent[k], f->batches[k] = (u16)rp_list_stats.k_batches[k];
		f->setups[k] = (u16)rp_list_stats.k_setups[k];
		f->msent[k] = (u16)rp_list_stats.k_msent[k], f->mbatches[k] = (u16)rp_list_stats.k_mbatches[k];
		f->draws[k] = (u16)rp_list_stats.k_draws[k];
		f->stream_kb[k] = (u16)rp_list_stats.k_stream_kb[k];
	}
}

static void passes_report(void) {
	if (!pass_log)
		return;
	u32 n = mx_hires_stats.logged, first = n > PASS_LOG ? n - PASS_LOG : 0;
	mx_hires_log(0, 0);
	static char text[64 * 96];
	FILE *fo = fopen("quake3.pass", "w");
	int len = sprintf(text, "# frame demo strip wait_us run_us over out sent batches setups tex_kb msent mbatches draws quad_kpx stream_kb\n");
	for (u32 i = first; i < n && fo; i++) {
		const struct mx_hires_pass *e = &pass_log[i % PASS_LOG];
		const typeof(frame_log[0]) *f = &frame_log[e->frame % FRAME_LOG];
		u32 k = e->strip & 7;
		int known = f->frame == e->frame;
		len += sprintf(text + len, "%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n", e->frame, known ? f->demo : 0, e->strip,
		               e->wait_us, e->run_us, e->over, e->out, known ? f->sent[k] : 0, known ? f->batches[k] : 0,
		               known ? f->setups[k] : 0, known ? f->tex_kb : 0, known ? f->msent[k] : 0,
		               known ? f->mbatches[k] : 0, known ? f->draws[k] : 0, known ? f->quad_kpx : 0,
		               known ? f->stream_kb[k] : 0);
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

/* r_ps2hud 1: the renderer's numbers on the screen, at the top right, the
   averages of the last half second */
static cvar_t *r_ps2hud;
static struct hud_at {
	u32 t, frames, half, late, over, uploads, upload_kb, full, coarse, paged, stream_kb, sent_kb, clut_slot, clut_cache,
	    lost;
} hud_was;
static char hud_line[12][64];
static u32 hud_n;

static void hud_text(float x, float y, const char *t, qhandle_t font) {
	static const float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 1};
	for (int pass = 0; pass < 2; pass++) {
		RP_SetColor(pass ? white : black);
		float cx = x + (pass ? 0 : 2), cy = y + (pass ? 0 : 2);
		for (const char *c = t; *c; c++, cx += 14) {
			u32 ch = (u8)*c;
			if (ch == ' ')
				continue;
			float s = (float)(ch & 15) * 0.0625f, tt = (float)(ch >> 4) * 0.0625f;
			RP_DrawStretchPic(cx, cy, 14, 24, s, tt, s + 0.0625f, tt + 0.0625f, font);
		}
	}
	RP_SetColor(NULL);
}

static void hud_draw(void) {
	static qhandle_t font;
	if (!font)
		font = rp_shader_register("gfx/2d/bigchars", -4, 0);
	struct hud_at now = {mx_count(), rp_tex_stats.frames, half_frames, mx_hires_stats.late, mx_hires_stats.over,
	                     rp_tex_stats.uploads, rp_tex_stats.upload_kb, rp_tex_stats.full, rp_tex_stats.coarse,
	                     rp_tex_stats.paged, rp_tex_stats.stream_kb, rp_tex_stats.stream_sent_kb,
	                     rp_tex_stats.clut_slot, rp_tex_stats.clut_cache, rp_list_stats.lost};
	struct hud_at *w = &hud_was;
	/* the counts were set back (a timedemo's start): from there */
	if (now.frames < w->frames)
		*w = now, w->frames = 0, w->uploads = w->upload_kb = w->full = w->coarse = w->paged = 0,
		w->stream_kb = w->sent_kb = w->clut_slot = w->clut_cache = 0;
	u32 fr = now.frames - w->frames;
	u32 us = (now.t - w->t) / (MX_EE_HZ / 1000000);
	if (fr && us >= 500000) {
		struct mx_vcache_stats vs;
		mx_vcache_stats(&vs);
		u32 n = 0;
		Com_sprintf(hud_line[n++], 64, "%4u x 540 %s  %4.1f fps %5.1f ms", mx_gs.w,
		            dyn_width ? "dyn" : "   ", fr * 1e6f / us, us / 1000.0f / fr);
		if (dyn_width)
			Com_sprintf(hud_line[n++], 64, "half width %3u%% of frames", (now.half - w->half) * 100 / fr);
		Com_sprintf(hud_line[n++], 64, "passes %4.1f ms/field late %u over %u", field_passes_us() / 1000.0f,
		            now.late - w->late, now.over - w->over);
		/* since the start: passes left out (of them ahead, r_ps2skip) and passes
		   the beam overtook */
		Com_sprintf(hud_line[n++], 64, "since start late %u skip %u over %u", mx_hires_stats.late,
		            mx_hires_stats.skipped, mx_hires_stats.over);
		Com_sprintf(hud_line[n++], 64, "cache %4u KB used %4u wants %4u", vs.pages * 8, vs.used_blocks / 4,
		            rp_tex_want_blocks() / 4);
		Com_sprintf(hud_line[n++], 64, "tight %d far bias %.2f no room %u/s", rp_tex_tight(), rp_tex_far_bias(),
		            (u32)((now.full - w->full) * 1e6f / us));
		Com_sprintf(hud_line[n++], 64, "sent %3u KB/fr %2u tex  coarser %u/s", (now.upload_kb - w->upload_kb) / fr,
		            (now.uploads - w->uploads) / fr, (u32)((now.coarse - w->coarse) * 1e6f / us));
		Com_sprintf(hud_line[n++], 64, "stream %3u pg budget %4u KB", rp_tex_stream_pages(), rp_tex_stream_budget());
		Com_sprintf(hud_line[n++], 64, "streamed %4u KB/fr sent %4u", (now.stream_kb - w->stream_kb) / fr,
		            (now.sent_kb - w->sent_kb) / fr);
		Com_sprintf(hud_line[n++], 64, "CLUT slots %3u: %3u/fr cache %u/fr", rp_tex_clut_slots(),
		            (now.clut_slot - w->clut_slot) / fr, (now.clut_cache - w->clut_cache) / fr);
		Com_sprintf(hud_line[n++], 64, "paged in %u/s pinned %u KB lost %u", (u32)((now.paged - w->paged) * 1e6f / us),
		            rp_tex_pinned_kb, now.lost - w->lost);
		hud_n = n;
		*w = now;
	} else if (!w->t)
		*w = now;
	for (u32 i = 0; i < hud_n; i++)
		hud_text(1920 - 14 * 40 - 24, 24 + 28 * (float)i, hud_line[i], font);
}

static void RP_EndFrame(int *frontEndMsec, int *backEndMsec) {
	int shot = in_frame && r_ps2shot->integer && ps2_demo_frame() == r_ps2shot->integer;
	if (in_frame && r_ps2hud && r_ps2hud->integer)
		hud_draw();
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
	/* the counts are the timedemo's: from its first frame on; or a place's,
	   from the view r_ps2beamat names */
	int df = ps2_demo_frame();
	if (df && r_ps2passes->integer && mx_hires_passes())
		passes_frame(mx_frame_number() - 1, df);
	if (!df && r_ps2beamat->integer && st.views >= (unsigned)r_ps2beamat->integer)
		df = 1;
	if (df && !st_frames) {
		st_at = st;
		rp_list_stats = (struct rp_list_stats){0};
		rp_tex_stats = (struct rp_tex_stats){0};
		tri_frames = tri_changes = 0;
		world_st_fine = world_st_cand = 0;
		pass_tex_kb = 0;
		u32 kb = rp_model_stats.kb;
		rp_model_stats = (struct rp_model_stats){.kb = kb};
	}
	if (df) {
		/* what the frame's polygons (sprites, effects, the sky) cover */
		unsigned long long px = (unsigned long long)quad_px;
		if (st_frames) {
			poly_px_sum += px;
			if (px > poly_px_max)
				poly_px_max = px;
			poly_px_over += px > 2u * 1920 * 540;
		}
		st_frames++;
	}
	fill_was[1] = fill_was[0], fill_was[0] = quad_px + light_px;
	light_px = 0;
	quad_px = 0;
}

/* ---- marks: a bullet's hole, a rocket's burn, blood on a wall, a player's
   shadow. The game gives a square on the plane it hit and asks for the parts
   of it that lie on the world. The map's surfaces are not on the EE (they
   are VU1 batches in the map's file), so the answer comes from the collision
   world: a point of the square is on the world when a short ray along the
   projection hits there (25 us a ray on the PS2).
   - All four corners on the world: the square, laid on it.
   - Else (over an edge, at a wall, on steps): a grid of 4 x 4 cells; the
     cells with all corners on the world, as rectangles where the world is
     flat under them and as rows of cells where it is not.
   - The players' shadows ask every frame, each with its own square. For
     squares of their size and larger the rays are not the square's points
     but those of a lattice on its plane, each asked once (the world does
     not change) and a point of a square takes the lattice's nearest: a
     shadow that moves asks for the few points it comes to, one that comes
     where another was for none. ---- */

#include "../qcommon/surfaceflags.h"

static int mark_point_n(const float *p, const float *dir, float *out, float *nrm) {
	vec3_t a, b, zero = {0, 0, 0};
	trace_t tr;
	VectorMA(p, -4.0f, dir, a);
	VectorMA(p, 8.0f, dir, b);
	st_mark_traces++, mark_rays++;
	CM_BoxTrace(&tr, a, b, zero, zero, 0, CONTENTS_SOLID, qfalse);
	if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f)
		return 0;
	if (tr.surfaceFlags & (SURF_NOMARKS | SURF_NODRAW | SURF_SKY))
		return 0;
	/* a surface that faces the projection, as the engine's (not a wall seen along it) */
	if (DotProduct(tr.plane.normal, dir) > -0.5f)
		return 0;
	VectorCopy(tr.endpos, out);
	if (nrm)
		VectorCopy(tr.plane.normal, nrm);
	return 1;
}

static int mark_point(const float *p, const float *dir, float *out) { return mark_point_n(p, dir, out, NULL); }

/* the planes squares were asked on (a direction, how far along it, the
   lattice's step), and the lattice's points known: where along the
   direction the world is there, in eighths from the plane (MARK_MISS: not) */
#define MARK_PLANES 64
#define MARK_SAMPLES 4096
#define MARK_MISS (-128)
static struct mark_plane {
	float dir[3], dc, ax[3], ay[3], step;
	u16 id; /* 0: none */
} mark_plane[MARK_PLANES];
static struct mark_sample {
	u16 plane;
	s16 u, v;
	s8 off;
} mark_sample[MARK_SAMPLES];
static u32 mark_planes; /* made since the map was loaded */
static struct mark_plane *mark_last;

static void marks_forget(void) {
	memset(mark_plane, 0, sizeof mark_plane);
	memset(mark_sample, 0, sizeof mark_sample);
	mark_planes = 0, mark_last = 0;
}

static struct mark_plane *mark_plane_of(const float *dir, float dc, float step) {
	struct mark_plane *pl = mark_last;
	for (int k = -1; k < MARK_PLANES; k++, pl = &mark_plane[k]) {
		if (!pl || !pl->id || pl->step != step)
			continue;
		float d0 = pl->dir[0] - dir[0], d1 = pl->dir[1] - dir[1], d2 = pl->dir[2] - dir[2], dd = pl->dc - dc;
		if (d0 * d0 + d1 * d1 + d2 * d2 < 1e-6f && dd > -0.5f && dd < 0.5f)
			return mark_last = pl;
	}
	if ((mark_planes + 1 & 0xffff) == 0)
		marks_forget();
	pl = &mark_plane[mark_planes % MARK_PLANES];
	pl->id = (u16)(++mark_planes);
	VectorCopy(dir, pl->dir);
	pl->dc = dc, pl->step = step;
	PerpendicularVector(pl->ax, dir);
	CrossProduct(dir, pl->ax, pl->ay);
	return mark_last = pl;
}

/* p of a square on pl, on the world: by the lattice's point nearest to p */
static int mark_at(struct mark_plane *pl, const float *p, float *out) {
	float fu = DotProduct(p, pl->ax) / pl->step, fv = DotProduct(p, pl->ay) / pl->step;
	if (fu > 32000.0f || fu < -32000.0f || fv > 32000.0f || fv < -32000.0f)
		return mark_point(p, pl->dir, out);
	s16 u = (s16)(fu < 0 ? fu - 0.5f : fu + 0.5f), v = (s16)(fv < 0 ? fv - 0.5f : fv + 0.5f);
	u32 hsh = (u32)pl->id * 0x9E3779B1u ^ (u32)(u16)u * 0x85EBCA6Bu ^ (u32)(u16)v * 0xC2B2AE35u;
	struct mark_sample *sm = &mark_sample[(hsh ^ hsh >> 15) % MARK_SAMPLES];
	if (sm->plane == pl->id && sm->u == u && sm->v == v)
		st_mark_known++;
	else {
		float q[3], hit[3];
		for (int a = 0; a < 3; a++)
			q[a] = (float)u * pl->step * pl->ax[a] + (float)v * pl->step * pl->ay[a] + pl->dc * pl->dir[a];
		sm->plane = pl->id, sm->u = u, sm->v = v, sm->off = MARK_MISS;
		if (mark_point(q, pl->dir, hit)) {
			float o = (DotProduct(hit, pl->dir) - pl->dc) * 8.0f;
			sm->off = (s8)(o < 0 ? o - 0.5f : o + 0.5f);
		}
	}
	if (sm->off == MARK_MISS)
		return 0;
	float along = pl->dc + (float)sm->off * 0.125f - DotProduct(p, pl->dir);
	VectorMA(p, along, pl->dir, out);
	return 1;
}

/* the same for the point of a square with its middle at mid */
static int mark_lattice(struct mark_plane *pl, const float *p, const float *mid, float *out) {
	float q[3], d[3], len;
	VectorSubtract(mid, p, d);
	len = VectorLength(d);
	if (len > 0.01f) {
		float pull = pl->step * 0.5f < len ? pl->step * 0.5f / len : 1.0f;
		VectorMA(p, pull, d, q);
		if (!mark_at(pl, q, q))
			return 0;
		/* q is on the world: p as far along the direction as it */
		float along = DotProduct(q, pl->dir) - DotProduct(p, pl->dir);
		VectorMA(p, along, pl->dir, out);
		return 1;
	}
	return mark_at(pl, p, out);
}

#define MARK_GRID 4
static int mark_fragments(int numPoints, const vec3_t *points, const vec3_t projection, int maxPoints,
                          vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer) {
	vec3_t dir, g[MARK_GRID + 1][MARK_GRID + 1];
	u8 ok[MARK_GRID + 1][MARK_GRID + 1], used[MARK_GRID][MARK_GRID];
	float t[MARK_GRID + 1][MARK_GRID + 1];
	float (*out)[3] = (float (*)[3])pointBuffer;
	if (numPoints != 4 || maxFragments < 1 || maxPoints < 4)
		return 0;
	VectorCopy(projection, dir);
	if (VectorNormalize(dir) == 0)
		return 0;
	/* a square of a shadow's size or larger: by the lattice of its plane, a
	   point by the lattice's nearest to it, taken half a step towards the
	   square's middle (so the nearest is not outside the square) */
	float mid[3], edge = 0;
	for (int a = 0; a < 3; a++) {
		mid[a] = (points[0][a] + points[1][a] + points[2][a] + points[3][a]) * 0.25f;
		float e = points[1][a] - points[0][a];
		edge += e * e;
	}
	struct mark_plane *pl = 0;
	if (edge >= 24.0f * 24.0f)
		/* (a shadow, 48 across, is asked every frame and moves with its player:
		   at a step of 12 a running one came to five new points every other
		   frame, a ray each; at 24 to three every fourth) */
		pl = mark_plane_of(dir, DotProduct(mid, dir), edge >= 96.0f * 96.0f ? 32.0f : edge >= 44.0f * 44.0f ? 24.0f : 12.0f);
#define POINT(p, out) (pl ? mark_lattice(pl, p, mid, out) : mark_point(p, dir, out))
	/* the corners: 0 1 2 3 round the square */
	static const u8 corner[4][2] = {{0, 0}, {MARK_GRID, 0}, {MARK_GRID, MARK_GRID}, {0, MARK_GRID}};
	int all = 1, have = 0;
	if (!pl) {
		/* a small mark (a bullet's): no more once the frame's rays are spent */
		if (r_ps2markrays->integer > 0 && mark_rays >= (unsigned)r_ps2markrays->integer) {
			st_mark_over++;
			return 0;
		}
		/* on a flat wall two rays are enough: a corner's gives the wall's plane,
		   and when the opposite corner's ray ends on that plane the other two
		   are where theirs meet it */
		float n[3], dn;
		ok[0][0] = (u8)mark_point_n(points[0], dir, g[0][0], n);
		ok[MARK_GRID][MARK_GRID] = (u8)mark_point(points[2], dir, g[MARK_GRID][MARK_GRID]);
		have = 5; /* corners 0 and 2 are asked */
		if (ok[0][0] && ok[MARK_GRID][MARK_GRID] && (dn = DotProduct(n, dir)) < -0.5f) {
			float q[4][3], d0 = DotProduct(n, g[0][0]);
			for (int c = 0; c < 4; c++)
				VectorMA(points[c], (d0 - DotProduct(n, points[c])) / dn, dir, q[c]);
			float off = DotProduct(q[2], dir) - DotProduct(g[MARK_GRID][MARK_GRID], dir);
			if (off > -0.5f && off < 0.5f) {
				for (int c = 0; c < 4; c++)
					VectorCopy(q[c], out[c]);
				fragmentBuffer[0].firstPoint = 0, fragmentBuffer[0].numPoints = 4;
				st_mark_frags++, st_mark_flat++;
				return 1;
			}
		}
	}
	for (int c = 0; c < 4; c++) {
		int i = corner[c][0], j = corner[c][1];
		if (!(have >> c & 1))
			ok[j][i] = (u8)POINT(points[c], g[j][i]);
		all &= ok[j][i];
	}
	if (all) {
		for (int c = 0; c < 4; c++)
			VectorCopy(g[corner[c][1]][corner[c][0]], out[c]);
		fragmentBuffer[0].firstPoint = 0, fragmentBuffer[0].numPoints = 4;
		st_mark_frags++;
		return 1;
	}
	for (int j = 0; j <= MARK_GRID; j++)
		for (int i = 0; i <= MARK_GRID; i++) {
			if (!((i == 0 || i == MARK_GRID) && (j == 0 || j == MARK_GRID))) {
				float u = (float)i / MARK_GRID, v = (float)j / MARK_GRID;
				vec3_t p;
				for (int a = 0; a < 3; a++)
					p[a] = (1 - v) * ((1 - u) * points[0][a] + u * points[1][a]) + v * ((1 - u) * points[3][a] + u * points[2][a]);
				ok[j][i] = (u8)POINT(p, g[j][i]);
			}
			t[j][i] = ok[j][i] ? DotProduct(g[j][i], dir) : 0;
		}
#define CELL(j, i) (ok[j][i] && ok[j][(i) + 1] && ok[(j) + 1][i] && ok[(j) + 1][(i) + 1] && !used[j][i])
	int nf = 0, np = 0;
	memset(used, 0, sizeof used);
	for (int j = 0; j < MARK_GRID; j++)
		for (int i = 0; i < MARK_GRID; i++) {
			if (!CELL(j, i))
				continue;
			/* the largest rectangle of cells from here: along the row, then rows down */
			int e = i + 1, j2 = j + 1;
			while (e < MARK_GRID && CELL(j, e))
				e++;
			for (; j2 < MARK_GRID; j2++) {
				int k = i;
				while (k < e && CELL(j2, k))
					k++;
				if (k < e)
					break;
			}
			for (int y = j; y < j2; y++)
				for (int x = i; x < e; x++)
					used[y][x] = 1;
			/* flat: its points lie where its corners say */
			int flat = 1;
			for (int y = j; y <= j2 && flat; y++)
				for (int x = i; x <= e; x++) {
					float u = (float)(x - i) / (float)(e - i), v = (float)(y - j) / (float)(j2 - j);
					float want = (1 - v) * ((1 - u) * t[j][i] + u * t[j][e]) + v * ((1 - u) * t[j2][i] + u * t[j2][e]);
					float off = t[y][x] - want;
					if (off < -0.3f || off > 0.3f) {
						flat = 0;
						break;
					}
				}
			if (flat) {
				if (nf == maxFragments || np + 4 > maxPoints)
					return nf;
				fragmentBuffer[nf].firstPoint = np, fragmentBuffer[nf].numPoints = 4, nf++;
				VectorCopy(g[j][i], out[np]), np++;
				VectorCopy(g[j][e], out[np]), np++;
				VectorCopy(g[j2][e], out[np]), np++;
				VectorCopy(g[j2][i], out[np]), np++;
				continue;
			}
			/* each row of it: its near edge, then its far edge back */
			for (int y = j; y < j2; y++) {
				int n = 2 * (e - i + 1);
				if (nf == maxFragments || np + n > maxPoints)
					return nf;
				fragmentBuffer[nf].firstPoint = np, fragmentBuffer[nf].numPoints = n, nf++;
				for (int k = i; k <= e; k++, np++)
					VectorCopy(g[y][k], out[np]);
				for (int k = e; k >= i; k--, np++)
					VectorCopy(g[y + 1][k], out[np]);
			}
		}
#undef CELL
#undef POINT
	st_mark_frags += (unsigned)nf;
	return nf;
}

static int RP_MarkFragments(int numPoints, const vec3_t *points, const vec3_t projection, int maxPoints,
                            vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer) {
	st.marks++;
	if (!r_ps2marks->integer)
		return 0;
	u32 t0 = mx_count();
	int n = mark_fragments(numPoints, points, projection, maxPoints, pointBuffer, maxFragments, fragmentBuffer);
	st_mark_cycles += mx_count() - t0;
	return n;
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
	           "times a surface's frame was not ready; %u tags and %u frames' bounds read from the file while the game waited\n",
	           (rp_model_stats.drawn + rp_model_stats.cut) / n, rp_model_stats.surfs / n, rp_model_stats.pieces / n, rp_model_stats.verts / n,
	           rp_model_stats.cut / n, rp_model_stats.culled / n, rp_model_stats.nostage, rp_model_stats.stages,
	           rp_model_stats.paged, rp_model_stats.tag_waits, rp_model_stats.frame_waits);
	rp_shader_report();
	{
		struct mx_vcache_stats vs;
		mx_vcache_stats(&vs);
		Com_Printf("ps2: renderer: GS memory: a frame draws with %u KB of images (at most %u KB); the cache has %u pages "
		           "(%u KB), %u KB in use by %u entries; %u evicted\n",
		           rp_tex_stats.frames ? rp_tex_stats.used_kb / rp_tex_stats.frames : 0, rp_tex_stats.used_max_kb, vs.pages,
		           vs.pages * 8, vs.used_blocks / 4, vs.entries, vs.evicted);
		{
			struct mx_vcache_space sp;
			mx_vcache_space(&sp);
			Com_Printf("ps2: renderer: GS memory now: the entries' texels %u KB, between them in their pages %u KB, whole "
			           "pages past large pieces %u KB, free in pages of small pieces %u KB\n",
			           sp.need_blocks / 4, sp.span_waste / 4, sp.round_waste / 4, sp.small_free / 4);
		}
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
	           "an image in GS memory; %u views in a mirror\n", rp_poly_stats.polys, rp_poly_stats.cut, rp_poly_stats.culled,
	           rp_poly_stats.pics, st_nostage, st_mirrors);
	Com_Printf("ps2: renderer: trilinear: %u frames with the textures between two levels (r_ps2trilinear %d), %u changes\n",
	           tri_frames, r_ps2trilinear ? r_ps2trilinear->integer : 0, tri_changes);
	Com_Printf("ps2: renderer: the scene's quads fill %u thousand pixels a frame, %u thousand at most (the "
	           "screen is 1037 thousand); %u frames with more than twice the screen; over r_ps2fill in %u frames, %u sprites left out; %u parts of marks by %u rays in %u ms (%u at places known); the quads' draws %u ms; %u lights on the world in %u runs of %u chunks (%u left out for the passes' time); %u of %u textures a frame with levels over 128 texels (%u sent to the pool, %u times not there yet)\n",
	           (unsigned)(poly_px_sum / n / 1000), (unsigned)(poly_px_max / 1000), poly_px_over, st_fill_over, st_fill_out,
	           st_mark_frags, st_mark_traces, (unsigned)(st_mark_cycles / (MX_EE_HZ / 1000)), st_mark_known,
	           (unsigned)(st_quad_cycles / (MX_EE_HZ / 1000)), rp_dl_stats.lights, rp_dl_stats.runs, rp_dl_stats.chunks, st_dl_off, world_st_fine / n, world_st_cand / n, rp_tex_stats.fine_sent, rp_tex_stats.fine_wait);
	unsigned v = st.views - st_at.views;
	u32 tf = rp_tex_stats.frames ? rp_tex_stats.frames : 1;
	Com_Printf("ps2: renderer: textures: %u levels sent while not in memory (%u KB; the game waited %u ms); %u placed "
	           "from a coarser level while their finer one was read in; %u KB pinned (levels under 32 KiB, CLUTs); "
	           "a frame streamed %u images (%u KB) in %u pieces, sent %u times (%u KB) in the strips (area %u pages); "
	           "%u not streamed (over the frame's KB); %u sent without a FLUSH\n",
	           rp_tex_stats.file_waits, rp_tex_stats.file_wait_kb, rp_tex_stats.file_wait_us / 1000, rp_tex_stats.paged,
	           rp_tex_pinned_kb, rp_tex_stats.stream / tf, rp_tex_stats.stream_kb / tf, rp_tex_stats.stream_segs / tf,
	           rp_tex_stats.stream_sent / tf, rp_tex_stats.stream_sent_kb / tf, rp_tex_stream_pages(), rp_tex_stats.stream_over,
	           rp_tex_stats.stream_noflush);
	Com_Printf("ps2: renderer: CLUTs: %u slots below the picture, a frame %u drawn from them (%u sent there), %u from "
	           "the cache\n", rp_tex_clut_slots(), rp_tex_stats.clut_slot / tf, rp_tex_stats.clut_slot_sent,
	           rp_tex_stats.clut_cache / tf);
	if (mx_hires_stats.stuck[15]) {
		const u32 *k = mx_hires_stats.stuck;
		u32 lo, hi;
		rp_dyn_range(&lo, &hi);
		Com_Printf("ps2: stuck pass (strip %u): VIF1 STAT %08x CODE %08x; VIF1 DMA CHCR %08x MADR %08x QWC %x TADR %08x "
		           "ASR0 %08x ASR1 %08x; GIF STAT %08x; GIF DMA CHCR %08x MADR %08x QWC %x TADR %08x; dyn %08x-%08x\n",
		           k[13], k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], k[8], k[9], k[10], k[11], k[12], lo, hi);
		const u32 *v = mx_hires_stats.stuck_vu1;
		Com_Printf("ps2: stuck pass: VPU_STAT %08x; VU1 TPC %04x VI01-15 %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x\n",
		           k[14], v[16], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13],
		           v[14], v[15]);
		for (u32 i = 0; i < 20; i++) {
			const u32 *m = mx_hires_stats.stuck_mem[i];
			Com_Printf("ps2: stuck mem %08x: %08x %08x %08x %08x\n", (k[5] & 0x01FFFFF0u) - 256 + i * 16, m[0], m[1], m[2],
			           m[3]);
		}
	}
	{
		extern unsigned world_st_vu0, world_st_vu0_diff;
		if (world_st_vu0)
			Com_Printf("ps2: renderer: VU0: %u boxes of the world tested both ways, %u differ\n", world_st_vu0, world_st_vu0_diff);
	}
	if (dyn_width)
		Com_Printf("ps2: renderer: width: %u of %u frames at half, %u changes; %u frames at half while the whole "
		           "width was asked for (GS memory not back yet, or the layout between)\n", half_frames,
		           rp_tex_stats.frames, width_changes, width_held);
	Com_Printf("ps2: renderer: marks: %u rays in a frame at most, %u small marks flat (two rays), %u left out for the "
	           "frame's rays (r_ps2markrays)\n", st_mark_rays_max, st_mark_flat, st_mark_over);
	if (v && world_sum.drawn)
		Com_Printf("ps2: renderer: the world, a view: %u groups in view, %u chunks (%u across a plane, %u more facing "
		           "away), %u vertices\n", world_sum.groups / v, world_sum.drawn / v, world_sum.clip / v,
		           world_sum.back / v, world_sum.verts / v);
	if (st_frames)
		Com_Printf("ps2: renderer: a frame: %u runs, %u draws; over the strips %u vertices in %u batches, %u set-ups, %u "
		           "quadwords of records, %u of chains; %u draws left out (no memory); textures sent %u (%u KB), %u "
		           "found no room (%u placed without their finest levels for want of room, %u as their distance needs no more (%u hundredths of a level sooner, up to %u, %u times too low); %u frames with more images than fit: at full size %u KB a frame, %u at most), %u waited a frame\n", rp_list_stats.runs / st_frames, rp_list_stats.draws / st_frames,
		           rp_list_stats.sent / st_frames, rp_list_stats.batches / st_frames, rp_list_stats.setups / st_frames,
		           rp_list_stats.qw / st_frames, rp_list_stats.dyn_qw / st_frames, rp_list_stats.lost,
		           rp_tex_stats.uploads, rp_tex_stats.upload_kb, rp_tex_stats.full, rp_tex_stats.coarse, rp_tex_stats.far, (unsigned)(rp_tex_stats.far_bias_sum * 100 / (rp_tex_stats.tight ? rp_tex_stats.tight : 1)), (unsigned)(rp_tex_stats.far_bias_max * 100), rp_tex_stats.far_floors, rp_tex_stats.tight, rp_tex_stats.frames ? rp_tex_stats.want_sum / rp_tex_stats.frames : 0, rp_tex_stats.want_max / 4, rp_tex_stats.later);
	if (st_frames) {
		static const char *const prog[RP_PROGS] = {"the world's", "GS packets", "the models'", "the stages'", "the lights'"};
		for (int k = 0; k < RP_PROGS; k++)
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

/* the picture, and the images' state then (is GS memory short: tight, the
   levels the world's textures are taken coarser by) */
int ps2_ren_screenshot(const char *path) {
	if (!screen_on)
		return -1;
	const struct rp_tex_stats *t = &rp_tex_stats;
	{
		/* the passes of the frames shown last: a field's together, and each strip's */
		char each[96];
		int len = 0;
		float sum = field_passes_us();
		for (u32 k = 0; k < mx_hires_passes() && k < 8; k++)
			len += sprintf(each + len, "%s%u", k ? "/" : "", (unsigned)strip_us[k]);
		ri.Printf(PRINT_ALL, "ps2: renderer: passes at the picture: %u us a field (%s us); so far %u left out, %u still drawn "
		          "when the beam came\n", (unsigned)sum, each, mx_hires_stats.late, mx_hires_stats.over);
	}
	ri.Printf(PRINT_ALL, "ps2: renderer: textures at the picture: tight %d, far bias %.2f; frames %u, tight %u; "
	          "%u placed coarser for room, %u from their distance's level, %u without room; %u from a coarser level "
	          "while the finer was read in, %u sent while not in memory (waited %u ms); %u KB pinned; streamed %u images "
	          "(%u KB) in %u pieces, sent %u times (%u KB) in the strips, %u over the frame's KB\n",
	          rp_tex_tight(), rp_tex_far_bias(), t->frames, t->tight, t->coarse, t->far, t->full, t->paged, t->file_waits,
	          t->file_wait_us / 1000, rp_tex_pinned_kb, t->stream, t->stream_kb, t->stream_segs, t->stream_sent,
	          t->stream_sent_kb, t->stream_over);
	ri.Printf(PRINT_ALL, "ps2: renderer: CLUTs at the picture: %u slots below it, %u drawn from them, %u from the cache\n",
	          rp_tex_clut_slots(), t->clut_slot, t->clut_cache);
	/* the picture first: it stops the passes at the record played */
	int saved = mx_hires_save_tga(path);
	/* r_ps2debug 4096 also: the frame's images, each with the level it was
	   placed from, the one its distance wanted and its place */
	if (rp_debug & 4096) {
		u32 f = mx_frame_number();
		FILE *out = fopen("quake3.images", "w");
		for (u32 i = 0;; i++) {
			struct rp_image *im = rp_image_at(i);
			if (!im) {
				if (i > 4096)
					break;
				continue;
			}
			if (im->frame + 1 < f)
				continue;
			/* where it is in GS memory, or that it had no room there */
			char v[64] = " (not placed)";
			if (im->ok)
				Com_sprintf(v, sizeof v, " at page %u..%u, CLUT page %u", (im->t.tbp[0] & 0xFFFF) / 32,
				            ((im->t.tbp[0] & 0xFFFF) + im->t.blocks - 1) / 32, (im->t.cbp & 0xFFFF) / 32);
			if (out)
				fprintf(out, "%s %ux%u %s levels %u from %u want %d%s%s\n", im->name, im->w, im->h,
				        im->psm == MX_GS_PSM_T4 ? "T4" : "T8", im->levels, im->from,
				        im->want_frame + 1 >= f ? im->want : -1, im->kind == RP_IMAGE_MODEL ? " (model)" : "", v);
		}
		if (out)
			fclose(out);
	}
	return saved;
}

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
	r_ps2widthflip = ri.Cvar_Get("r_ps2widthflip", "0", 0);
	r_ps2halfat = ri.Cvar_Get("r_ps2halfat", "95", 0);
	r_ps2shotload = ri.Cvar_Get("r_ps2shotload", "0", 0);
	load_views = 0;
	r_ps2hud = ri.Cvar_Get("r_ps2hud", "0", CVAR_ARCHIVE);
	r_ps2beamat = ri.Cvar_Get("r_ps2beamat", "0", 0);
	r_ps2fill = ri.Cvar_Get("r_ps2fill", "1", CVAR_ARCHIVE);
	r_ps2marks = ri.Cvar_Get("r_ps2marks", "1", CVAR_ARCHIVE);
	r_ps2dlight = ri.Cvar_Get("r_ps2dlight", "4", CVAR_ARCHIVE);
	/* mirrors and portals: the scene once more through them, while their
	   opening covers a quarter of the picture or less (a second scene is as
	   much as the first where it covers the picture: a mirror filling it was
	   half the passes left out at 1080i). Bits: 1 mirrors, 2 at any size, 4
	   portals to a camera elsewhere. OFF: on the PS2 both hung the console
	   (a reset needed) and beam racing's strips flickered heavily, also at a
	   quarter of the picture; correct in PCSX2 only. */
	r_ps2mirror = ri.Cvar_Get("r_ps2mirror", "0", CVAR_ARCHIVE);
	r_ps2markrays = ri.Cvar_Get("r_ps2markrays", "64", CVAR_ARCHIVE);
	r_ps2clean = ri.Cvar_Get("r_ps2clean", "1", CVAR_ARCHIVE);
	rp_debug = (unsigned)ri.Cvar_Get("r_ps2debug", "0", 0)->integer;
	r_fastsky = ri.Cvar_Get("r_fastsky", "0", CVAR_ARCHIVE);
	r_dynamiclight = ri.Cvar_Get("r_dynamiclight", "1", CVAR_ARCHIVE);
	/* the Graphics menu's video mode: the one there is */
	ri.Cvar_Get("r_availableModes", "", CVAR_ROM);
	ri.Cvar_Set("r_availableModes", "1920x1080");
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
/* as the engine: the game's shaders without a script are the screen's (LIGHTMAP_2D) */
static qhandle_t RP_RegisterShader(const char *name) { return rp_shader_register(name, -4, 1); }
/* the screen's pictures (LIGHTMAP_2D) */
static qhandle_t RP_RegisterShaderNoMip(const char *name) { return rp_shader_register(name, -4, 0); }
/* maps/<name>.bsp: its world is maps/<name>.pmap, a file beside the paks, mapped
   and locked in memory (the DMAC reads its chunks and textures where they lie) */
/* the shaders of the map's materials that are scripts, and the scene's time */

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
	int late = lit && sh->lm_at >= 2 && sh->lm_at < RP_STAGES + 1 && !(rp_debug & 262144);
	*first = !lit || late;
	/* every stage before the lightmap's changes the picture that is there by
	   a factor (water: the picture times 1 + the texture, three times): the
	   lightmap's pass is then the picture times the lightmap's value itself,
	   not twice it. The engine's frame is half the picture (its overbright
	   doubles it at the end), so a lightmap stage that is a product of what is
	   in the frame halves, where one over a stage drawn at half does not. */
	int product = late;
	for (int k = 0; late && k < sh->lm_at && k < sh->nstage; k++)
		product &= sh->st[k].blend == RP_BLEND_BRIGHTEN || sh->st[k].blend == RP_BLEND_FILTER ||
		           sh->st[k].blend == RP_BLEND_INVFILTER;
	/* alphaGen lightingSpecular on the stage the map's own pass draws (88
	   scripts: the shining walls of the "base" maps, blendFunc GL_DST_COLOR
	   GL_SRC_ALPHA over the lightmap: the lightmap times the texture plus its
	   alpha, which is the shine of a light at a fixed place in the map): the
	   shine as a pass of its own, each chunk's (rp_world.c) */
	if (lit && !late && sh->st[0].alphagen == A_SPECULAR && !(rp_debug & 16777216)) {
		struct rp_state *s = &pass[n++];
		memset(s, 0, RP_STATE_HEAD);
		s->prog = RP_PROG_DLIGHT, s->menv = 2, s->blend = RP_BLEND_BRIGHTEN, s->z = RP_Z_TEST;
		s->clamp = 1, s->nomip = 1, s->tcc = 1, s->alpha = RP_ALPHA_GT0;
	}
	for (int k = lit && !late ? 1 : 0; k <= sh->nstage && n < RP_WORLD_PASSES; k++) {
		if (late && k == sh->lm_at) {
			memset(&pass[n], 0, RP_STATE_HEAD);
			pass[n].prog = RP_PROG_WORLD, pass[n].lm_only = product ? 2 : 1, pass[n].z = RP_Z_TEST;
			if (++n == RP_WORLD_PASSES)
				break;
		}
		if (k == sh->nstage)
			break;
		struct rp_state *s = &pass[n];
		const struct rp_stage *stg = &sh->st[k];
		if (!rp_stage_state(stg, scene_time, s)) {
			st_nostage++;
			continue;
		}
		s->prog = RP_PROG_STAGE;
		s->z = k == 0 && stg->blend == RP_BLEND_NONE ? RP_Z_WRITE : RP_Z_TEST;
		rp_stage_tcmod(stg, scene_time, s->tm);
		/* the colour: the vertex's as the map has it, or the stage's own */
		u8 c[4];
		rp_stage_colour(stg, scene_time, NULL, NULL, c);
		/* alphaGen portal: the portal's haze by the eye's distance to it
		   (none near by, opaque from its range on) */
		if (stg->alphagen == A_PORTAL) {
			float r = stg->awave.base > 0 ? stg->awave.base : 256.0f, d = rp_portal_dist(rp_view.org) / r;
			c[3] = (u8)((d > 1.0f ? 1.0f : d) * 128.0f);
		}
		if (stg->env) {
			/* tcGen environment: the texture's coordinates are the reflection of
			   the eye in the surface, which the world's draw makes of each
			   chunk's direction and place (rp_world.c: the program that lays a
			   texture on the world by position); the stage's colour as it is
			   (half before a late lightmap, which doubles) */
			float h = late && k < sh->lm_at ? 0.5f : 1.0f;
			s->prog = RP_PROG_DLIGHT, s->menv = 1, s->nomip = 1;
			for (int a = 0; a < 3; a++)
				s->cmul[a] = 0, s->cadd[a] = (float)c[a] * h;
			s->cmul[3] = 0, s->cadd[3] = (float)c[3];
			n++;
			continue;
		}
		int vrgb = stg->rgbgen == RGB_VERTEX || stg->rgbgen == RGB_EXACT_VERTEX, va = stg->alphagen == A_VERTEX;
		/* a surface whose alpha is tested has no lightmap pass (it cannot leave
		   out the texels the test leaves out): the map gave its vertices the
		   lightmap's light, which the stages before the script's $lightmap stage
		   take (q3dm19's girders were lit as if there were no light: full) */
		if ((flags & RP_MAT_ALIT) && !(flags & RP_MAT_VLM) && k < sh->lm_at &&
		    (stg->rgbgen == RGB_IDENTITY || stg->rgbgen == RGB_IDENTITY_LIGHTING))
			vrgb = 1;
		/* before a late lightmap: half as bright (the lightmap pass doubles, as the
		   engine's overbright does after its lightmap) */
		float half = late && k < sh->lm_at ? 0.5f : 1.0f;
		/* and of the lightmap's hue: the colours of a lightmapped chunk's
		   vertices (128 is 1.0), as the map's own pass has it */
		int hue = late && k < sh->lm_at;
		/* rgbGen vertex is half the vertex's colour as the map has it (the
		   engine's identityLight; the map's colours are exactVertex's, which an
		   unscripted surface lit by its vertices has): the heads and torches on
		   the walls were twice as bright as the PC's */
		float vhalf = stg->rgbgen == RGB_VERTEX && !(flags & RP_MAT_VLM) ? 0.5f : 1.0f;
		for (int a = 0; a < 4; a++) {
			int v = a < 3 ? vrgb : va;
			float h = a < 3 ? half : 1.0f;
			/* (a material whose first stage is rgbGen vertex has that light in its
			   vertices' colours with the hue: a stage of its own colour would be
			   as dark as the vertices, black in q3dm1's throat; it goes without
			   the hue) */
			if (hue && a < 3 && (vrgb || !(flags & RP_MAT_VLM)))
				s->cmul[a] = vrgb ? 1.0f : (float)c[a] * h * (1.0f / 128.0f), s->cadd[a] = 0;
			else
				s->cmul[a] = v ? (a < 3 ? h * vhalf : h) : 0, s->cadd[a] = v ? 0 : (float)c[a] * h;
		}
		n++;
	}
	return n;
}

static void RP_LoadWorld(const char *name) {
	char path[MAX_OSPATH], base[MAX_QPATH];
	world_free();
	marks_forget();
	COM_StripExtension(name, base, sizeof base);
	Com_sprintf(path, sizeof path, "%s/%s/%s.pmap", ri.Cvar_Get("fs_basepath", "", 0)->string,
	            ri.Cvar_Get("com_basegame", BASEGAME, 0)->string, base);
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
	/* the pool of fine levels for a map that has textures of 256 texels, the
	   cache's memory back for one that has none (no frame in flight then) */
	frames_flush();
	rp_tex_fine_use(rp_world_fine());
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
/* A level is loaded. What the load wrote and the game does not touch again
   is still in memory, and each page the game then brings in (a texture's, a
   model's frames) pushes one of those out: a write to the swap file of 6 ms
   that costs the frame a field (the timedemo's late frames were these). The
   written pages go to the swap file now, where a wait does not show, and
   stay in memory: the VM's mx_vm_clean (kernel 43). r_ps2clean 0: not. */
static void RP_EndRegistration(void) {
	if (!r_ps2clean->integer || !mx_vm_active())
		return;
	int t0 = ri.Milliseconds();
	int n = mx_vm_clean();
	ri.Printf(PRINT_ALL, "ps2: renderer: the level's written pages to the swap file: %d pages in %d ms\n", n,
	          ri.Milliseconds() - t0);
}
static int RP_LightForPoint(vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir) { return qfalse; }
static void RP_DrawStretchRaw(int x, int y, int w, int h, int cols, int rows, const byte *data, int client,
                              qboolean dirty) {}
static void RP_UploadCinematic(int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty) {}
static void RP_RegisterFont(const char *fontName, int pointSize, fontInfo_t *font) { memset(font, 0, sizeof *font); }
static void RP_RemapShader(const char *oldShader, const char *newShader, const char *offsetTime) {}
static qboolean RP_GetEntityToken(char *buffer, int size) { return qfalse; }
static qboolean RP_inPVS(const vec3_t p1, const vec3_t p2) { return qtrue; }
static void RP_TakeVideoFrame(int h, int w, byte *captureBuffer, byte *encodeBuffer, qboolean motionJpeg) {}

/* the renderer's calls marked for the load's samples (code/ps2/ps2_load.c) */
extern volatile unsigned char ps2_load_state;
static void load_RenderScene(const refdef_t *fd) {
	ps2_load_state = 1;
	RP_RenderScene(fd);
	ps2_load_state = 0;
}
static void load_BeginFrame(stereoFrame_t stereoFrame) {
	ps2_load_state = 1;
	RP_BeginFrame(stereoFrame);
	ps2_load_state = 0;
}
static void load_EndFrame(int *frontEndMsec, int *backEndMsec) {
	ps2_load_state = 1;
	RP_EndFrame(frontEndMsec, backEndMsec);
	ps2_load_state = 0;
}
static void load_DrawStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader) {
	ps2_load_state = 1;
	RP_DrawStretchPic(x, y, w, h, s1, t1, s2, t2, hShader);
	ps2_load_state = 0;
}
static int load_MarkFragments(int numPoints, const vec3_t *points, const vec3_t projection, int maxPoints,
                              vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer) {
	ps2_load_state = 1;
	int r = RP_MarkFragments(numPoints, points, projection, maxPoints, pointBuffer, maxFragments, fragmentBuffer);
	ps2_load_state = 0;
	return r;
}

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
	re.RenderScene = load_RenderScene;
	re.SetColor = RP_SetColor;
	re.DrawStretchPic = load_DrawStretchPic;
	re.DrawStretchRaw = RP_DrawStretchRaw;
	re.UploadCinematic = RP_UploadCinematic;
	re.BeginFrame = load_BeginFrame;
	re.EndFrame = load_EndFrame;
	re.MarkFragments = load_MarkFragments;
	re.LerpTag = RP_LerpTag;
	re.ModelBounds = RP_ModelBounds;
	re.RegisterFont = RP_RegisterFont;
	re.RemapShader = RP_RemapShader;
	re.GetEntityToken = RP_GetEntityToken;
	re.inPVS = RP_inPVS;
	re.TakeVideoFrame = RP_TakeVideoFrame;
	return &re;
}
