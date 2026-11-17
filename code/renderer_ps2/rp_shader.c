/* rp_shader.c: the game's shaders (rp_shader.h). A shader is its script
   (scripts/*.shader: stages of an image, a blend, colours and texture
   coordinates that move), or, without one, an image as the engine draws it:
   lit by the light grid on a model, blended with the vertex colours on the
   screen.

   The images are the file of images' (rp_bin.c; tools/ps2bin.py makes it of
   tools/ps2pak.py's pak: paletted, with their MIPMAP levels); one that is
   not in it is not drawn.

   The GS blends (A - B) * C + D with C an alpha: the script's blends are
   mapped onto that, a product with the picture (blendFunc filter, the marks'
   GL_ZERO GL_ONE_MINUS_SRC_COLOR) by an alpha that is the texture's
   brightness (rp_image_luma). */

#include <mx/mem.h>
#include <mx/mx.h>

#include "rp_bin.h"
#include "rp_shader.h"

#define LIGHTMAP_2D -4
#define LIGHTMAP_NONE -1

/* ---- images ------------------------------------------------------------------------------- */

static unsigned img_missing, img_loaded, img_bytes;

/* the image of a script's or the game's name (with or without extension):
   in the file of images (rp_bin.c), its texels where they lie */
static struct rp_image *image_load(const char *name) {
	char base[MAX_QPATH];
	COM_StripExtension(name, base, sizeof base);
	Q_strlwr(base);
	for (char *c = base; *c; c++)
		if (*c == '\\')
			*c = '/';
	struct rp_image *im = rp_image_find(base);
	if (im)
		return im;
	im = rp_image_bin(base, &img_bytes);
	if (!im) {
		if (img_missing++ < 40)
			ri.Printf(PRINT_DEVELOPER, "ps2: renderer: no image %s\n", base);
		return NULL;
	}
	img_loaded++;
	return im;
}

static struct rp_image *white;
static struct rp_image *image_white(void) {
	if (white)
		return white;
	white = rp_image_alloc("*white", MX_GS_PSM_T4, 8, 8, 1, 0);
	if (!white)
		return NULL;
	memset(white->m.level[0], 0, 32);
	u32 *clut = white->m.clut;
	for (int i = 0; i < 16; i++)
		clut[i] = 0x80FFFFFFu;
	rp_image_ready(white);
	return white;
}

/* ---- the scripts' text ----------------------------------------------------------------------- */

#define SCRIPT_HASH 2048
static char *script_text;
static struct {
	const char *name; /* in script_text, ended by a 0 written over what followed it */
	char *body;       /* after its { */
} *script_tab;
static int script_n, script_max;

static unsigned name_hash(const char *s) {
	unsigned h = 0;
	for (; *s && *s != '.'; s++) {
		int c = *s == '\\' ? '/' : tolower((unsigned char)*s);
		h = h * 31 + (unsigned)c;
	}
	return h;
}

static void scripts_load(void) {
	int nfiles = 0;
	char **files = ri.FS_ListFiles("scripts", ".shader", &nfiles);
	long total = 0;
	void **bufs = ri.Hunk_AllocateTempMemory((int)(nfiles * sizeof *bufs + 1));
	long *sizes = ri.Hunk_AllocateTempMemory((int)(nfiles * sizeof *sizes + 1));
	script_n = 0, script_text = NULL, script_tab = NULL;
	for (int i = 0; i < nfiles; i++) {
		char path[MAX_QPATH];
		Com_sprintf(path, sizeof path, "scripts/%s", files[i]);
		sizes[i] = ri.FS_ReadFile(path, &bufs[i]);
		if (sizes[i] < 0 || !bufs[i])
			sizes[i] = 0, bufs[i] = NULL;
		total += sizes[i] + 1;
	}
	script_text = ri.Hunk_Alloc((int)total + 1, h_low);
	char *at = script_text;
	for (int i = nfiles - 1; i >= 0; i--) {
		if (bufs[i]) {
			memcpy(at, bufs[i], sizes[i]);
			at += sizes[i];
			*at++ = '\n';
			ri.FS_FreeFile(bufs[i]);
		}
	}
	*at = 0;
	ri.Hunk_FreeTempMemory(sizes);
	ri.Hunk_FreeTempMemory(bufs);
	ri.FS_FreeFileList(files);
	COM_Compress(script_text);
	/* every script's name and where its body starts */
	script_max = 0;
	for (int pass = 0; pass < 2; pass++) {
		char *p = script_text;
		int n = 0;
		for (;;) {
			char *tok = COM_ParseExt(&p, qtrue);
			if (!tok[0])
				break;
			char *name = p - strlen(tok);
			char *brace = p;
			tok = COM_ParseExt(&brace, qtrue);
			if (tok[0] != '{') {
				p = brace;
				continue;
			}
			if (pass) {
				script_tab[n].name = name, script_tab[n].body = brace;
			}
			n++;
			p = brace;
			if (!SkipBracedSection(&p, 1))
				break;
		}
		if (!pass) {
			script_max = n;
			script_tab = ri.Hunk_Alloc((int)((n + 1) * sizeof *script_tab), h_low);
		} else
			script_n = n < script_max ? n : script_max;
	}
	ri.Printf(PRINT_ALL, "ps2: renderer: %d shader scripts in %d files, %ld KB\n", script_n, nfiles, total / 1024);
}

/* a name as the script has it against the one asked for (no extension, any case) */
static int name_is(const char *script, const char *name) {
	for (;; script++, name++) {
		int a = *script == '\\' ? '/' : tolower((unsigned char)*script);
		int b = *name == '\\' ? '/' : tolower((unsigned char)*name);
		int ea = !a || a == '.' || a == ' ' || a == '\t' || a == '\n' || a == '{';
		int eb = !b || b == '.';
		if (ea || eb)
			return ea && eb;
		if (a != b)
			return 0;
	}
}

static char *script_find(const char *name) {
	(void)name_hash;
	for (int i = 0; i < script_n; i++)
		if (name_is(script_tab[i].name, name))
			return script_tab[i].body;
	return NULL;
}

/* ---- parsing ----------------------------------------------------------------------------------- */

static float tokf(char **p) { return (float)atof(COM_ParseExt(p, qfalse)); }

static void parse_wave(char **p, struct rp_wave *w) {
	const char *t = COM_ParseExt(p, qfalse);
	w->func = !Q_stricmp(t, "triangle")          ? WAVE_TRIANGLE
	          : !Q_stricmp(t, "square")          ? WAVE_SQUARE
	          : !Q_stricmp(t, "sawtooth")        ? WAVE_SAWTOOTH
	          : !Q_stricmp(t, "inversesawtooth") ? WAVE_INVSAWTOOTH
	          : !Q_stricmp(t, "noise")           ? WAVE_NOISE
	                                             : WAVE_SIN;
	w->base = tokf(p), w->amp = tokf(p), w->phase = tokf(p), w->freq = tokf(p);
}

enum { F_ZERO, F_ONE, F_DST_COLOR, F_ONE_MINUS_DST_COLOR, F_SRC_ALPHA, F_ONE_MINUS_SRC_ALPHA, F_SRC_COLOR,
       F_ONE_MINUS_SRC_COLOR, F_DST_ALPHA, F_ONE_MINUS_DST_ALPHA };
static int blend_factor(const char *t) {
	static const char *const names[] = {"GL_ZERO", "GL_ONE", "GL_DST_COLOR", "GL_ONE_MINUS_DST_COLOR", "GL_SRC_ALPHA",
	                                    "GL_ONE_MINUS_SRC_ALPHA", "GL_SRC_COLOR", "GL_ONE_MINUS_SRC_COLOR", "GL_DST_ALPHA",
	                                    "GL_ONE_MINUS_DST_ALPHA"};
	for (int i = 0; i < 10; i++)
		if (!Q_stricmp(t, names[i]))
			return i;
	return F_ONE;
}

/* a blend as the GS does it; luma: by the texture's brightness as alpha (2: twice it) */
static void blend_set(struct rp_stage *st, int s, int d) {
	/* the picture keeps no alpha: DST_ALPHA is 1 */
	s = s == F_DST_ALPHA ? F_ONE : s == F_ONE_MINUS_DST_ALPHA ? F_ZERO : s;
	d = d == F_DST_ALPHA ? F_ONE : d == F_ONE_MINUS_DST_ALPHA ? F_ZERO : d;
	st->luma = 0;
	if (s == F_ONE && d == F_ZERO)
		st->blend = RP_BLEND_NONE;
	else if (s == F_SRC_ALPHA && d == F_ONE_MINUS_SRC_ALPHA)
		st->blend = RP_BLEND_ALPHA;
	else if (s == F_SRC_ALPHA && d == F_ONE)
		st->blend = RP_BLEND_ADD_ALPHA;
	else if ((s == F_DST_COLOR && d == F_ZERO) || (s == F_ZERO && d == F_SRC_COLOR))
		st->blend = RP_BLEND_FILTER, st->luma = 1;
	else if (s == F_DST_COLOR && d == F_SRC_COLOR)
		st->blend = RP_BLEND_FILTER, st->luma = 2;
	else if (s == F_ZERO && d == F_ONE_MINUS_SRC_COLOR)
		st->blend = RP_BLEND_INVFILTER, st->luma = 1;
	else if (s == F_ZERO && d == F_ONE_MINUS_SRC_ALPHA)
		st->blend = RP_BLEND_INVFILTER;
	else if (d == F_ONE_MINUS_SRC_ALPHA || d == F_SRC_ALPHA)
		st->blend = RP_BLEND_ALPHA;
	else
		st->blend = RP_BLEND_ADD; /* ONE ONE, DST_COLOR ONE, ONE ONE_MINUS_SRC_COLOR: about additive */
}

static void parse_stage(char **p, struct rp_shader *sh, struct rp_stage *st) {
	int depth_write = -1;
	memset(st, 0, sizeof *st);
	st->rgbgen = 0xFF;
	for (;;) {
		const char *t = COM_ParseExt(p, qtrue);
		if (!t[0] || t[0] == '}')
			break;
		if (!Q_stricmp(t, "map") || !Q_stricmp(t, "clampmap")) {
			st->clamp = !Q_stricmp(t, "clampmap");
			t = COM_ParseExt(p, qfalse);
			if (!Q_stricmp(t, "$lightmap"))
				st->lightmap = 1;
			else if (!Q_stricmp(t, "$whiteimage") || !Q_stricmp(t, "*white"))
				st->img[0] = image_white(), st->nimg = 1;
			else
				st->img[0] = image_load(t), st->nimg = 1;
		} else if (!Q_stricmp(t, "animMap")) {
			st->fps = tokf(p);
			for (;;) {
				t = COM_ParseExt(p, qfalse);
				if (!t[0])
					break;
				if (st->nimg < RP_ANIM)
					st->img[st->nimg++] = image_load(t);
			}
		} else if (!Q_stricmp(t, "blendFunc")) {
			t = COM_ParseExt(p, qfalse);
			if (!Q_stricmp(t, "add"))
				blend_set(st, F_ONE, F_ONE);
			else if (!Q_stricmp(t, "filter"))
				blend_set(st, F_DST_COLOR, F_ZERO);
			else if (!Q_stricmp(t, "blend"))
				blend_set(st, F_SRC_ALPHA, F_ONE_MINUS_SRC_ALPHA);
			else {
				int s = blend_factor(t);
				blend_set(st, s, blend_factor(COM_ParseExt(p, qfalse)));
			}
		} else if (!Q_stricmp(t, "alphaFunc")) {
			t = COM_ParseExt(p, qfalse);
			st->alpha = !Q_stricmp(t, "GT0") ? RP_ALPHA_GT0 : !Q_stricmp(t, "LT128") ? RP_ALPHA_LT128 : RP_ALPHA_GE128;
		} else if (!Q_stricmp(t, "rgbGen")) {
			t = COM_ParseExt(p, qfalse);
			if (!Q_stricmp(t, "wave"))
				st->rgbgen = RGB_WAVE, parse_wave(p, &st->rgbwave);
			else if (!Q_stricmp(t, "const")) {
				COM_ParseExt(p, qfalse); /* ( */
				for (int k = 0; k < 3; k++) {
					float v = tokf(p) * 255.0f;
					st->rgba[k] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
				}
				COM_ParseExt(p, qfalse);
				st->rgbgen = RGB_CONST;
			} else
				st->rgbgen = !Q_stricmp(t, "vertex")             ? RGB_VERTEX
				             : !Q_stricmp(t, "exactVertex")      ? RGB_EXACT_VERTEX
				             : !Q_stricmp(t, "entity")           ? RGB_ENTITY
				             : !Q_stricmp(t, "oneMinusEntity")   ? RGB_ONE_MINUS_ENTITY
				             : !Q_stricmp(t, "lightingDiffuse")  ? RGB_LIGHTING
				             : !Q_stricmp(t, "identityLighting") ? RGB_IDENTITY_LIGHTING
				                                                 : RGB_IDENTITY;
		} else if (!Q_stricmp(t, "alphaGen")) {
			t = COM_ParseExt(p, qfalse);
			if (!Q_stricmp(t, "wave"))
				st->alphagen = A_WAVE, parse_wave(p, &st->awave);
			else if (!Q_stricmp(t, "const")) {
				float v = tokf(p) * 255.0f;
				st->rgba[3] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
				st->alphagen = A_CONST;
			} else
				st->alphagen = !Q_stricmp(t, "vertex")           ? A_VERTEX
				               : !Q_stricmp(t, "entity")         ? A_ENTITY
				               : !Q_stricmp(t, "oneMinusEntity") ? A_ONE_MINUS_ENTITY
				                                                 : A_IDENTITY;
		} else if (!Q_stricmp(t, "tcGen") || !Q_stricmp(t, "texgen")) {
			t = COM_ParseExt(p, qfalse);
			st->env = !Q_stricmp(t, "environment");
		} else if (!Q_stricmp(t, "tcMod")) {
			t = COM_ParseExt(p, qfalse);
			int k = st->ntcmod < RP_TCMODS ? st->ntcmod : RP_TCMODS - 1;
			int type = !Q_stricmp(t, "scroll")      ? TCM_SCROLL
			           : !Q_stricmp(t, "scale")     ? TCM_SCALE
			           : !Q_stricmp(t, "rotate")    ? TCM_ROTATE
			           : !Q_stricmp(t, "stretch")   ? TCM_STRETCH
			           : !Q_stricmp(t, "transform") ? TCM_TRANSFORM
			           : !Q_stricmp(t, "turb")      ? TCM_TURB
			                                        : -1;
			if (type >= 0) {
				st->tcmod[k].type = (u8)type;
				if (type == TCM_STRETCH)
					parse_wave(p, &st->tcmod[k].wave);
				else {
					int np = type == TCM_ROTATE ? 1 : type == TCM_TRANSFORM ? 6 : type == TCM_TURB ? 4 : 2;
					for (int i = 0; i < np; i++)
						st->tcmod[k].p[i] = tokf(p);
				}
				if (st->ntcmod < RP_TCMODS && type != TCM_TURB)
					st->ntcmod++;
			}
		} else if (!Q_stricmp(t, "depthWrite"))
			depth_write = 1;
		SkipRestOfLine(p);
	}
	(void)sh;
	/* the engine's default: identity where the blend is a product with the
	   picture, else the half that is shown as it is */
	if (st->rgbgen == 0xFF)
		st->rgbgen = st->blend == RP_BLEND_FILTER || st->blend == RP_BLEND_INVFILTER ? RGB_IDENTITY : RGB_IDENTITY_LIGHTING;
	st->zwrite = depth_write >= 0 ? (u8)depth_write : st->blend == RP_BLEND_NONE;
}

static void parse_shader(char *body, struct rp_shader *sh) {
	char *p = body;
	int sort = 0;
	for (;;) {
		const char *t = COM_ParseExt(&p, qtrue);
		if (!t[0] || t[0] == '}')
			break;
		if (t[0] == '{') {
			struct rp_stage st;
			parse_stage(&p, sh, &st);
			/* a stage of the world's light, or without an image: not one to draw */
			if (!st.lightmap && st.nimg && sh->nstage < RP_STAGES)
				sh->st[sh->nstage++] = st;
			continue;
		}
		if (!Q_stricmp(t, "cull")) {
			t = COM_ParseExt(&p, qfalse);
			sh->cull_none = !Q_stricmp(t, "none") || !Q_stricmp(t, "disable") || !Q_stricmp(t, "twosided");
		} else if (!Q_stricmp(t, "sort")) {
			t = COM_ParseExt(&p, qfalse);
			sort = !Q_stricmp(t, "portal")       ? SORT_PORTAL
			       : !Q_stricmp(t, "sky")        ? SORT_SKY
			       : !Q_stricmp(t, "opaque")     ? SORT_OPAQUE
			       : !Q_stricmp(t, "decal")      ? SORT_DECAL
			       : !Q_stricmp(t, "seeThrough") ? SORT_SEE_THROUGH
			       : !Q_stricmp(t, "banner")     ? SORT_BANNER
			       : !Q_stricmp(t, "additive")   ? SORT_ADDITIVE
			       : !Q_stricmp(t, "nearest")    ? SORT_NEAREST
			       : !Q_stricmp(t, "underwater") ? SORT_UNDERWATER
			                                     : atoi(t);
		} else if (!Q_stricmp(t, "polygonOffset"))
			sh->polygon_offset = 1;
		else if (!Q_stricmp(t, "nomipmaps") || !Q_stricmp(t, "nopicmip"))
			sh->nomip |= !Q_stricmp(t, "nomipmaps");
		else if (!Q_stricmp(t, "skyparms"))
			sh->sky = 1;
		else if (!Q_stricmp(t, "deformVertexes")) {
			t = COM_ParseExt(&p, qfalse);
			sh->deform = !Q_stricmp(t, "autosprite") ? DEFORM_AUTOSPRITE : !Q_stricmp(t, "autosprite2") ? DEFORM_AUTOSPRITE2
			                                                                                           : DEFORM_OTHER;
		} else if (!Q_stricmp(t, "surfaceparm")) {
			t = COM_ParseExt(&p, qfalse);
			sh->nodraw |= !Q_stricmp(t, "nodraw");
		}
		SkipRestOfLine(&p);
	}
	int blended = sh->nstage && sh->st[0].blend != RP_BLEND_NONE;
	sh->sort = (u8)(sort ? sort : sh->polygon_offset ? SORT_DECAL : blended ? SORT_BLEND0 : SORT_OPAQUE);
}

/* ---- the shaders ---------------------------------------------------------------------------------- */

#define MAX_SHADER 1024
static struct rp_shader **shaders; /* each its own piece of the hunk */
static int nshader;
static unsigned sh_script, sh_image, sh_none;

void rp_shaders_begin(void) {
	/* the hunk was cleared: the scripts' text and the table went with it; the
	   images are the renderer's own memory */
	rp_images_free();
	white = NULL;
	img_missing = img_loaded = img_bytes = 0;
	sh_script = sh_image = sh_none = 0;
	shaders = ri.Hunk_Alloc(MAX_SHADER * sizeof *shaders, h_low);
	nshader = 1;
	scripts_load();
}

void rp_shader_report(void) {
	ri.Printf(PRINT_ALL, "ps2: renderer: shaders: %u of scripts, %u images alone, %u without either; images: %u loaded "
	          "(%u KB), %u not found\n", sh_script, sh_image, sh_none, img_loaded, img_bytes / 1024, img_missing);
}

struct rp_shader *rp_shader_get(qhandle_t h) { return h > 0 && h < nshader ? shaders[h] : NULL; }
int rp_shader_count(void) { return nshader ? nshader - 1 : 0; }

qhandle_t rp_shader_register(const char *name, int lightmap, int mip) {
	char base[MAX_QPATH];
	if (!name || !name[0] || strlen(name) >= MAX_QPATH || !shaders)
		return 0;
	COM_StripExtension(name, base, sizeof base);
	for (int i = 1; i < nshader; i++)
		if (!Q_stricmp(shaders[i]->name, base))
			return shaders[i]->defaulted ? 0 : i;
	if (nshader >= MAX_SHADER)
		return 0;
	struct rp_shader *sh = shaders[nshader] = ri.Hunk_Alloc(sizeof *sh, h_low);
	memset(sh, 0, sizeof *sh);
	Q_strncpyz(sh->name, base, sizeof sh->name);
	sh->lightmap = lightmap, sh->nomip = !mip;
	char *body = script_find(base);
	if (body) {
		parse_shader(body, sh);
		sh_script++;
	} else {
		/* an image alone: on the screen blended with the vertex colours and
		   no depth; else a model's, lit by the light grid */
		struct rp_stage *st = &sh->st[0];
		st->img[0] = image_load(name);
		if (st->img[0]) {
			sh->nstage = 1, st->nimg = 1;
			if (lightmap == LIGHTMAP_2D) {
				st->blend = RP_BLEND_ALPHA, st->rgbgen = RGB_VERTEX, st->alphagen = A_VERTEX;
				sh->sort = SORT_BLEND0, st->clamp = 1;
			} else {
				st->rgbgen = lightmap == LIGHTMAP_NONE ? RGB_LIGHTING : RGB_IDENTITY_LIGHTING;
				st->zwrite = 1, sh->sort = SORT_OPAQUE;
			}
			sh_image++;
		}
	}
	/* every stage must have its images */
	int ok = sh->nstage > 0 || sh->nodraw || sh->sky;
	for (int s = 0; s < sh->nstage; s++)
		for (int k = 0; k < sh->st[s].nimg; k++)
			ok &= sh->st[s].img[k] != NULL;
	if (!ok && !body) {
		sh->defaulted = 1;
		sh_none++;
		nshader++;
		return 0;
	}
	if (!ok) {
		/* a script with an image missing: the stages that are whole */
		int n = 0;
		for (int s = 0; s < sh->nstage; s++) {
			int whole = 1;
			for (int k = 0; k < sh->st[s].nimg; k++)
				whole &= sh->st[s].img[k] != NULL;
			if (whole)
				sh->st[n++] = sh->st[s];
		}
		sh->nstage = (u8)n;
	}
	return nshader++;
}

/* ---- a stage at a time ------------------------------------------------------------------------------- */

/* sin(2 pi t) for any t: folded to a quarter turn either side of 0, then a
   polynomial (the FPU has no sine and libm's is software floating point) */
float rp_sin1(float t);
static float sin1(float t) { return rp_sin1(t); }
float rp_sin1(float t) {
	float v = t + 0.25f;
	v = v - (float)(int)v;
	if (v < 0)
		v += 1.0f;
	v -= 0.5f;
	float x = (0.25f - (v < 0 ? -v : v)) * 6.2831853f, x2 = x * x;
	return x * (1.0f - x2 * (0.16666667f - x2 * 0.0083333333f));
}

static float wave(const struct rp_wave *w, float t) {
	float x = w->phase + t * w->freq, f;
	x -= (float)(int)x;
	if (x < 0)
		x += 1.0f;
	switch (w->func) {
	case WAVE_TRIANGLE: f = x < 0.25f ? 4 * x : x < 0.75f ? 2 - 4 * x : 4 * x - 4; break;
	case WAVE_SQUARE: f = x < 0.5f ? 1.0f : -1.0f; break;
	case WAVE_SAWTOOTH: f = x; break;
	case WAVE_INVSAWTOOTH: f = 1.0f - x; break;
	case WAVE_NOISE: f = (float)(rand() & 255) * (1.0f / 127.5f) - 1.0f; break;
	default: f = sin1(x); break;
	}
	return w->base + w->amp * f;
}

int rp_stage_state(const struct rp_stage *st, float t, struct rp_state *s) {
	memset(s, 0, sizeof *s);
	if (!st->nimg)
		return 0;
	u32 k = st->nimg > 1 ? (u32)(t * st->fps) % st->nimg : 0;
	struct rp_image *im = st->img[k];
	const struct mx_tex *x = im ? rp_image_place(im) : NULL;
	if (!x)
		return 0;
	s->tex = *x, s->tex_ok = 1;
	s->tcc = im->alpha;
	if (st->luma) {
		u32 cbp = rp_image_luma(im, st->luma == 2);
		if (cbp)
			s->tex.cbp = cbp, s->tcc = 1;
	}
	s->blend = st->blend, s->alpha = st->alpha, s->clamp = st->clamp;
	s->z = st->zwrite ? RP_Z_WRITE : RP_Z_TEST;
	s->texels = 1.0f;
	return 1;
}

void rp_stage_colour(const struct rp_stage *st, float t, const u8 *vertex, const u8 *entity, u8 *out) {
	static const u8 full[4] = {255, 255, 255, 255};
	vertex = vertex ? vertex : full, entity = entity ? entity : full;
	u32 a;
	switch (st->rgbgen) {
	case RGB_VERTEX: out[0] = (u8)((vertex[0] + 1) >> 1), out[1] = (u8)((vertex[1] + 1) >> 1), out[2] = (u8)((vertex[2] + 1) >> 1); break;
	case RGB_EXACT_VERTEX: out[0] = vertex[0], out[1] = vertex[1], out[2] = vertex[2]; break;
	case RGB_ENTITY: out[0] = entity[0], out[1] = entity[1], out[2] = entity[2]; break;
	case RGB_ONE_MINUS_ENTITY: out[0] = (u8)(255 - entity[0]), out[1] = (u8)(255 - entity[1]), out[2] = (u8)(255 - entity[2]); break;
	case RGB_CONST: out[0] = st->rgba[0], out[1] = st->rgba[1], out[2] = st->rgba[2]; break;
	case RGB_WAVE: {
		float v = wave(&st->rgbwave, t) * 128.0f;
		out[0] = out[1] = out[2] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
		break;
	}
	case RGB_IDENTITY_LIGHTING: out[0] = out[1] = out[2] = 128; break;
	default: out[0] = out[1] = out[2] = 255; break;
	}
	switch (st->alphagen) {
	case A_VERTEX: a = vertex[3]; break;
	case A_ENTITY: a = entity[3]; break;
	case A_ONE_MINUS_ENTITY: a = 255u - entity[3]; break;
	case A_CONST: a = st->rgba[3]; break;
	case A_WAVE: {
		float v = wave(&st->awave, t) * 255.0f;
		a = (u32)(v < 0 ? 0 : v > 255 ? 255 : v);
		break;
	}
	default: a = 255; break;
	}
	out[3] = (u8)((a + 1) >> 1);
}

int rp_stage_tcmod(const struct rp_stage *st, float t, float *m) {
	m[0] = 1, m[1] = 0, m[2] = 0, m[3] = 0, m[4] = 1, m[5] = 0;
	for (int k = 0; k < st->ntcmod; k++) {
		const float *p = st->tcmod[k].p;
		float a[6] = {1, 0, 0, 0, 1, 0};
		switch (st->tcmod[k].type) {
		case TCM_SCROLL: {
			float s = p[0] * t, v = p[1] * t;
			a[2] = s - (float)(int)s, a[5] = v - (float)(int)v;
			break;
		}
		case TCM_SCALE: a[0] = p[0], a[4] = p[1]; break;
		case TCM_ROTATE: {
			float turn = -p[0] * t * (1.0f / 360.0f), sn = sin1(turn), cs = sin1(turn + 0.25f);
			a[0] = cs, a[1] = -sn, a[2] = 0.5f - 0.5f * cs + 0.5f * sn;
			a[3] = sn, a[4] = cs, a[5] = 0.5f - 0.5f * sn - 0.5f * cs;
			break;
		}
		case TCM_STRETCH: {
			float w = wave(&st->tcmod[k].wave, t), q = w != 0 ? 1.0f / w : 1.0f;
			a[0] = q, a[2] = 0.5f - 0.5f * q, a[4] = q, a[5] = 0.5f - 0.5f * q;
			break;
		}
		case TCM_TRANSFORM: a[0] = p[0], a[1] = p[2], a[2] = p[4], a[3] = p[1], a[4] = p[3], a[5] = p[5]; break;
		default: break;
		}
		/* after what came before: a * m */
		float r[6] = {a[0] * m[0] + a[1] * m[3], a[0] * m[1] + a[1] * m[4], a[0] * m[2] + a[1] * m[5] + a[2],
		              a[3] * m[0] + a[4] * m[3], a[3] * m[1] + a[4] * m[4], a[3] * m[2] + a[4] * m[5] + a[5]};
		for (int i = 0; i < 6; i++)
			m[i] = r[i];
	}
	return st->ntcmod != 0;
}
