/* rp_main.c: the PS2's own renderer (PLAN-PS2.md): the engine's refexport_t
   (renderercommon/tr_public.h) on maxsdk, without renderergl1 and libgl.

   The screen is 1080i by beam racing (<mx/hires.h>: a field of 1920 x 540,
   drawn in strips ahead of the beam, each strip from a record of its own).
   A map's world is rp_world.c's, from maps/<name>.pmap beside the paks
   (tools/ps2map.py makes it from the BSP; a map without one is not drawn).

   Not yet: models, sprites and polys, 2D pictures, lights. They are
   registered and counted: models are loaded (the file as it is, in the
   hunk), since cgame places its entities by their tags and bounds. Each
   main view's camera goes into quake3.cam (ps2_ren_report), for the viewer
   (tools/q3view) to fly the same path. r_ps2draw 0: nothing is drawn, a
   timedemo then costs what the game side costs (game, cgame, client, sound,
   collision). */

#include "../qcommon/q_shared.h"
#include "../qcommon/qfiles.h"
#include "../qcommon/qcommon.h"
#include "../renderercommon/tr_public.h"

#include <mx/frame.h>
#include <mx/hires.h>
#include <mx/mx.h>
#include <mx/vm.h>

#include "rp_tex.h"
#include "rp_world.h"

static refimport_t ri;
static cvar_t *r_ps2draw;

/* ---- the screen ------------------------------------------------------------------- */

/* beam racing's records, read by the DMAC: four frames, and four of each of
   the three strips */
static u8 rec[4 * 128 * 1024] MX_CORE_BSS MX_ALIGNED(64);
static u8 srec[4 * 3 * 96 * 1024] MX_CORE_BSS MX_ALIGNED(64);
static int screen_on, in_frame;
/* cycles a frame waited for a free record: not the frame's work (sys_ps2.c) */
unsigned ps2_ren_wait;

static void screen_init(void) {
	if (screen_on)
		return;
	mx_hires_strip_lines(192);
	if (mx_hires_init(&mx_video_1080i, 1920, 540, MX_GS_PSM_CT16S, rec, sizeof rec, (void *)0x70002000, 0x2000) < 0)
		ri.Error(ERR_FATAL, "ps2: no 1080i, or its buffer does not fit");
	u32 sbytes = mx_hires_strip_records(srec, sizeof srec);
	ri.Printf(PRINT_ALL, "ps2: renderer: 1080i, %u strips of %u lines, a strip's record %u KB\n", mx_hires_passes(),
	          mx_hires_strip_height(), sbytes / 1024);
	rp_tex_init();
	screen_on = 1;
}

/* ---- the world ---------------------------------------------------------------------- */

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
	while (mx_frame_done() <= f)
		mx_sleep_us(2000);
}

static void world_free(void) {
	if (world_map) {
		frames_flush();
		rp_world_free();
		mx_vm_unlock(world_map, world_size);
		mx_vm_unmap_file(world_map);
	}
	world_map = NULL, world_ok = 0;
}
int ps2_demo_frame(void); /* sys_ps2.c: the timedemo's frame, 0: none */

/* ---- names to handles ------------------------------------------------------- */

#define RP_NAMES 2048
struct names {
	char name[RP_NAMES][MAX_QPATH];
	int n;
};
static struct names shaders, skins;

/* 1 .. RP_NAMES - 1; 0 is the engine's "none" */
static qhandle_t name_handle(struct names *t, const char *name) {
	if (!name || !name[0])
		return 0;
	for (int i = 1; i < t->n; i++)
		if (!Q_stricmp(t->name[i], name))
			return i;
	if (t->n >= RP_NAMES)
		return 0;
	if (!t->n)
		t->n = 1;
	Q_strncpyz(t->name[t->n], name, MAX_QPATH);
	return t->n++;
}

/* ---- models --------------------------------------------------------------------- */

#define RP_MODELS 1024
static struct {
	char name[MAX_QPATH];
	md3Header_t *md3; /* NULL: a brush model of the map ("*n") or not an MD3 */
} models[RP_MODELS];
static int nmodels;

static qhandle_t RP_RegisterModel(const char *name) {
	if (!name || !name[0] || strlen(name) >= MAX_QPATH)
		return 0;
	for (int i = 1; i < nmodels; i++)
		if (!Q_stricmp(models[i].name, name))
			return models[i].md3 || name[0] == '*' ? i : 0;
	if (nmodels >= RP_MODELS)
		return 0;
	if (!nmodels)
		nmodels = 1;
	int h = nmodels++;
	Q_strncpyz(models[h].name, name, MAX_QPATH);
	models[h].md3 = NULL;
	if (name[0] == '*')
		return h;
	void *file;
	long size = ri.FS_ReadFile(name, &file);
	if (size <= 0 || !file)
		return 0;
	md3Header_t *m = file;
	if (size >= (long)sizeof *m && m->ident == MD3_IDENT && m->version == MD3_VERSION && m->numFrames > 0) {
		models[h].md3 = ri.Hunk_Alloc(size, h_low);
		memcpy(models[h].md3, file, size);
	}
	ri.FS_FreeFile(file);
	return models[h].md3 ? h : 0;
}

static md3Header_t *model_md3(qhandle_t h) { return h > 0 && h < nmodels ? models[h].md3 : NULL; }

static void RP_ModelBounds(qhandle_t h, vec3_t mins, vec3_t maxs) {
	md3Header_t *m = model_md3(h);
	if (!m) {
		VectorClear(mins);
		VectorClear(maxs);
		return;
	}
	md3Frame_t *f = (md3Frame_t *)((byte *)m + m->ofsFrames);
	VectorCopy(f->bounds[0], mins);
	VectorCopy(f->bounds[1], maxs);
}

static md3Tag_t *model_tag(md3Header_t *m, int frame, const char *name) {
	if (frame >= m->numFrames)
		frame = m->numFrames - 1;
	if (frame < 0)
		frame = 0;
	md3Tag_t *t = (md3Tag_t *)((byte *)m + m->ofsTags) + frame * m->numTags;
	for (int i = 0; i < m->numTags; i++, t++)
		if (!strcmp(t->name, name))
			return t;
	return NULL;
}

static unsigned st_tags;

static int RP_LerpTag(orientation_t *tag, qhandle_t h, int startFrame, int endFrame, float frac,
                      const char *tagName) {
	md3Header_t *m = model_md3(h);
	md3Tag_t *a = m ? model_tag(m, startFrame, tagName) : NULL;
	md3Tag_t *b = m ? model_tag(m, endFrame, tagName) : NULL;
	st_tags++;
	if (!a || !b) {
		AxisClear(tag->axis);
		VectorClear(tag->origin);
		return qfalse;
	}
	float back = 1.0f - frac;
	for (int i = 0; i < 3; i++) {
		tag->origin[i] = a->origin[i] * back + b->origin[i] * frac;
		tag->axis[0][i] = a->axis[0][i] * back + b->axis[0][i] * frac;
		tag->axis[1][i] = a->axis[1][i] * back + b->axis[1][i] * frac;
		tag->axis[2][i] = a->axis[2][i] * back + b->axis[2][i] * frac;
	}
	VectorNormalize(tag->axis[0]);
	VectorNormalize(tag->axis[1]);
	VectorNormalize(tag->axis[2]);
	return qtrue;
}

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

static void RP_ClearScene(void) { cur_ents = cur_polys = cur_polyverts = cur_lights = 0; }

static void RP_AddRefEntityToScene(const refEntity_t *e) {
	if ((unsigned)e->reType < RT_MAX_REF_ENTITY_TYPE)
		st.ents[e->reType]++;
	cur_ents++;
}

static void RP_AddPolyToScene(qhandle_t shader, int numVerts, const polyVert_t *verts, int num) {
	st.polys += num, st.polyverts += numVerts * num;
	cur_polys += num, cur_polyverts += numVerts * num;
}

static void RP_AddLightToScene(const vec3_t org, float intensity, float r, float g, float b) {
	st.lights++, cur_lights++;
}

static void RP_RenderScene(const refdef_t *fd) {
	st.scenes++;
	if (fd->rdflags & RDF_NOWORLDMODEL) {
		st.small++;
		return;
	}
	st.views++;
	if (in_frame && world_ok && r_ps2draw->integer) {
		struct rp_cam cam;
		VectorCopy(fd->vieworg, cam.org);
		memcpy(cam.axis, fd->viewaxis, sizeof cam.axis);
		cam.fov_x = fd->fov_x, cam.fov_y = fd->fov_y;
		rp_world_draw(&cam, &world_st);
#define ACC(f) world_sum.f += world_st.f
		ACC(seen), ACC(groups), ACC(drawn), ACC(clip), ACC(back), ACC(verts), ACC(sent), ACC(batches), ACC(setups), ACC(qw);
#undef ACC
	}
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

static void RP_SetColor(const float *rgba) {}
static void RP_DrawStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2,
                              qhandle_t shader) {
	st.pics++;
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
	rp_frame_setup();
}

static void RP_EndFrame(int *frontEndMsec, int *backEndMsec) {
	if (in_frame) {
		mx_frame_end();
		in_frame = 0;
	}
	if (frontEndMsec)
		*frontEndMsec = 0;
	if (backEndMsec)
		*backEndMsec = 0;
	/* the counts are the timedemo's: from its first frame on */
	int df = ps2_demo_frame();
	if (df && !st_frames)
		st_at = st;
	if (df)
		st_frames++;
}

static int RP_MarkFragments(int numPoints, const vec3_t *points, const vec3_t projection, int maxPoints,
                            vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer) {
	st.marks++;
	return 0;
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
	Com_Printf("ps2: renderer: %d models, %d shaders, %d skins registered; %u tags asked\n", nmodels ? nmodels - 1 : 0,
	           shaders.n ? shaders.n - 1 : 0, skins.n ? skins.n - 1 : 0, st_tags);
	unsigned v = st.views - st_at.views;
	if (v && world_sum.drawn)
		Com_Printf("ps2: renderer: the world, a view: %u groups in view, %u chunks (%u across a plane, %u more facing "
		           "away), %u vertices; over the strips %u vertices in %u batches, %u set-ups, %u quadwords\n",
		           world_sum.groups / v, world_sum.drawn / v, world_sum.clip / v, world_sum.back / v, world_sum.verts / v,
		           world_sum.sent / v, world_sum.batches / v, world_sum.setups / v, world_sum.qw / v);
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
	nmodels = 0, shaders.n = 0, skins.n = 0;
	memset(config, 0, sizeof *config);
	Q_strncpyz(config->renderer_string, "PS2 GS (maxsdk)", sizeof config->renderer_string);
	Q_strncpyz(config->vendor_string, "ps2max", sizeof config->vendor_string);
	Q_strncpyz(config->version_string, "renderer_ps2", sizeof config->version_string);
	r_ps2draw = ri.Cvar_Get("r_ps2draw", "1", 0);
	screen_init();
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

static qhandle_t RP_RegisterSkin(const char *name) { return name_handle(&skins, name); }
static qhandle_t RP_RegisterShader(const char *name) { return name_handle(&shaders, name); }
/* maps/<name>.bsp: its world is maps/<name>.pmap, a file beside the paks, mapped
   and locked in memory (the DMAC reads its chunks and textures where they lie) */
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
	int lk = mx_vm_lock(world_map, world_size);
	if (lk < 0 || rp_world_load(world_map, world_size) < 0) {
		ri.Printf(PRINT_WARNING, "ps2: renderer: %s: %s\n", path, lk < 0 ? "no memory to keep it in" : "not a map of this version");
		world_free();
		return;
	}
	world_ok = 1;
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
	re.RegisterShaderNoMip = RP_RegisterShader;
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
