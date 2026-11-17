/* rp_model.c: the game's MD3 models (PLAN-PS2.md, 5.3 and 5.4).

   A model is loaded as the engine has it (the file, in the hunk: cgame asks
   for its tags and bounds), and its surfaces are made ready for VU1
   (tools/ps2vu.py, q3m): a surface is cut into pieces of at most
   Q3VU_M_VERTS vertices, each with its triangles as entries of a strip
   (three a triangle now: the first two end none). Of a piece there are its
   texture coordinates (16-bit fixed point) and its entries, once, in memory
   the DMAC reads; and for every frame its positions (the file's 16-bit ones)
   and its normals (three bytes, decoded from the file's two angles), in
   memory that may be paged out. A frame's block is made when a draw first
   reads it (a model has hundreds of frames and a game shows few of them);
   the two frames a draw reads are locked until its frame is done, and a
   surface whose frames are not in memory is left out of that frame (the
   pager brings them in).

   A draw of a surface is a chain built for the frame: for each piece five
   REFs (texture coordinates, the new frame's normals and positions, the old
   frame's positions, the entries) and the program's start. The EE touches
   no vertex: VU1 blends the two frames and lights the vertices.

   A model across the near plane or the guard band is built on the EE and
   goes the polygons' way (rp_poly.c), whose programs cut.

   The light is the map's light grid's at the model's origin, as the engine
   has it (R_SetupEntityLighting): an ambient colour, a directed one and its
   direction. */

#include <mx/frame.h>
#include <mx/mem.h>
#include <mx/mx.h>
#include <mx/vif.h>

#include "rp_list.h"
#include "rp_model.h"
#include "rp_poly.h"
#include "rp_shader.h"
#include "vu/q3vu.h"

struct rp_model_stats rp_model_stats;
float rp_sin1(float turns);

/* ---- the light grid ------------------------------------------------------------------- */

static struct {
	const u8 *data; /* 8 bytes a point: ambient, directed, the direction's two angles */
	float origin[3], inv[3];
	int bounds[3];
} grid;
static float sin_tab[256]; /* of n / 256 turns */

/* twice as bright (the map's overbright range against the screen's), the
   hue kept */
static void color_shift(u8 *c) {
	u32 r = c[0] * 2u, g = c[1] * 2u, b = c[2] * 2u, m = r > g ? r : g;
	m = b > m ? b : m;
	if (m > 255)
		r = r * 255 / m, g = g * 255 / m, b = b * 255 / m;
	c[0] = (u8)r, c[1] = (u8)g, c[2] = (u8)b;
}

void rp_light_load(const char *name) {
	grid.data = NULL;
	for (int i = 0; i < 256; i++)
		sin_tab[i] = rp_sin1((float)i * (1.0f / 256.0f));
	void *buf;
	long size = ri.FS_ReadFile(name, &buf);
	if (size < (long)sizeof(dheader_t) || !buf)
		return;
	const dheader_t *h = buf;
	const lump_t *lg = &h->lumps[LUMP_LIGHTGRID], *lm = &h->lumps[LUMP_MODELS], *le = &h->lumps[LUMP_ENTITIES];
	if (lg->filelen >= 8 && lm->filelen >= (int)sizeof(dmodel_t) && lg->fileofs + lg->filelen <= size) {
		const dmodel_t *world = (const dmodel_t *)((const u8 *)buf + lm->fileofs);
		float gs[3] = {64, 64, 128};
		/* the worldspawn's gridsize */
		const char *ents = (const char *)buf + le->fileofs, *end = ents + le->filelen;
		for (const char *p = ents; p + 10 < end && *p != '}'; p++)
			if (!Q_strncmp(p, "\"gridsize\"", 10)) {
				p += 10;
				while (p < end && *p != '"')
					p++;
				sscanf(p + 1, "%f %f %f", &gs[0], &gs[1], &gs[2]);
				break;
			}
		int n = 1;
		for (int i = 0; i < 3; i++) {
			grid.inv[i] = 1.0f / gs[i];
			grid.origin[i] = gs[i] * ceilf(world->mins[i] / gs[i]);
			float maxs = gs[i] * floorf(world->maxs[i] / gs[i]);
			grid.bounds[i] = (int)((maxs - grid.origin[i]) / gs[i] + 1);
			n *= grid.bounds[i];
		}
		if (n > 0 && lg->filelen == n * 8) {
			u8 *d = ri.Hunk_Alloc(lg->filelen, h_low);
			memcpy(d, (const u8 *)buf + lg->fileofs, lg->filelen);
			for (int i = 0; i < n; i++)
				color_shift(d + i * 8), color_shift(d + i * 8 + 3);
			grid.data = d;
		}
	}
	ri.FS_FreeFile(buf);
	ri.Printf(PRINT_ALL, "ps2: renderer: light grid %d x %d x %d%s\n", grid.bounds[0], grid.bounds[1], grid.bounds[2],
	          grid.data ? "" : " (none)");
}

void rp_light_at(const vec3_t p, int world, vec3_t ambient, vec3_t directed, vec3_t dir) {
	if (!world || !grid.data) {
		ambient[0] = ambient[1] = ambient[2] = 75 + 16;
		directed[0] = directed[1] = directed[2] = 75;
		VectorSet(dir, 0.45f, 0.3f, 0.9f);
		VectorNormalize(dir);
		return;
	}
	int pos[3], step[3] = {8, 8 * grid.bounds[0], 8 * grid.bounds[0] * grid.bounds[1]};
	float frac[3], total = 0, direction[3] = {0, 0, 0};
	VectorClear(ambient);
	VectorClear(directed);
	for (int i = 0; i < 3; i++) {
		float v = (p[i] - grid.origin[i]) * grid.inv[i];
		pos[i] = (int)floorf(v);
		frac[i] = v - (float)pos[i];
		if (pos[i] < 0)
			pos[i] = 0;
		else if (pos[i] > grid.bounds[i] - 1)
			pos[i] = grid.bounds[i] - 1;
	}
	const u8 *base = grid.data + pos[0] * step[0] + pos[1] * step[1] + pos[2] * step[2];
	for (int i = 0; i < 8; i++) {
		float f = 1.0f;
		const u8 *d = base;
		int j;
		for (j = 0; j < 3; j++) {
			if (i & (1 << j)) {
				if (pos[j] + 1 > grid.bounds[j] - 1)
					break; /* outside the grid */
				f *= frac[j];
				d += step[j];
			} else
				f *= 1.0f - frac[j];
		}
		if (j < 3 || !(d[0] + d[1] + d[2]))
			continue; /* in a wall */
		total += f;
		for (int k = 0; k < 3; k++)
			ambient[k] += f * d[k], directed[k] += f * d[3 + k];
		float slng = sin_tab[d[6]], clng = sin_tab[(d[6] + 64) & 255];
		direction[0] += f * sin_tab[(d[7] + 64) & 255] * slng;
		direction[1] += f * sin_tab[d[7]] * slng;
		direction[2] += f * clng;
	}
	if (total > 0 && total < 0.99f) {
		VectorScale(ambient, 1.0f / total, ambient);
		VectorScale(directed, 1.0f / total, directed);
	}
	for (int k = 0; k < 3; k++)
		ambient[k] += 16;
	VectorNormalize2(direction, dir);
}

/* ---- models ------------------------------------------------------------------------------- */

struct piece {
	u8 nvert, st_qw, pos_qw, nrm_qw;
	u16 nent, idx_qw;
	u32 st, idx;   /* in the surface's fixed memory */
	u32 pos, nrm;  /* in a frame's block */
};
struct msurf {
	const md3Surface_t *md3;
	qhandle_t shader;   /* the file's own */
	int npiece;
	struct piece *piece;
	u8 *fixed;          /* memory the DMAC reads */
	u8 *frames;         /* pageable: a block of frame_bytes a frame */
	u32 frame_bytes;
	u32 lock_at[2];     /* the frames (of the picture) + 1 its blocks lock_f were locked for */
	int lock_f[2];
	u32 lock_n;
	u16 (*pv)[Q3VU_M_VERTS]; /* a piece's vertices in the file */
	u8 *made;           /* a bit a frame: its block is made */
};
#define RP_MODELS 1024
static struct rmodel {
	char name[MAX_QPATH];
	md3Header_t *md3; /* NULL: a brush model of the map ("*n") or not an MD3 */
	struct msurf *surf;
} *models;
static int nmodels;
static struct mx_heap *heap, *heap_frames;
static unsigned model_bytes;

#define SKIN_SURFS 32
#define RP_SKINS 256
static struct skin {
	char name[MAX_QPATH];
	int n;
	struct {
		char surf[MAX_QPATH];
		qhandle_t shader;
	} s[SKIN_SURFS];
} *skins;
static int nskins;

void rp_models_begin(void) {
	if (!heap) {
		heap = mx_heap_create("q3.models", MX_MEM_PINNED, 0);
		heap_frames = mx_heap_create("q3.frames", MX_MEM_ASSET, 24u << 20);
	} else {
		mx_heap_reset(heap);
		mx_heap_reset(heap_frames);
	}
	models = ri.Hunk_Alloc(RP_MODELS * sizeof *models, h_low);
	skins = ri.Hunk_Alloc(RP_SKINS * sizeof *skins, h_low);
	nmodels = nskins = 1;
	model_bytes = 0;
	rp_model_stats = (struct rp_model_stats){0};
}

int rp_model_count(void) { return nmodels ? nmodels - 1 : 0; }

/* A surface for VU1: its pieces, then their memory. */
static int surf_make(struct msurf *sf) {
	const md3Surface_t *s = sf->md3;
	const md3Triangle_t *tri = (const md3Triangle_t *)((const u8 *)s + s->ofsTriangles);
	const md3St_t *st = (const md3St_t *)((const u8 *)s + s->ofsSt);
	const md3XyzNormal_t *xyz = (const md3XyzNormal_t *)((const u8 *)s + s->ofsXyzNormals);
	int nv = s->numVerts, nt = s->numTriangles, nf = s->numFrames;
	if (nv <= 0 || nt <= 0 || nf <= 0 || nv > MD3_MAX_VERTS)
		return -1;
	/* at most: a piece takes 9 triangles at least (27 new vertices) */
	int maxp = nt / 9 + 2;
	struct piece *pc = ri.Hunk_Alloc(maxp * (int)sizeof *pc, h_low);
	u16 (*pv)[Q3VU_M_VERTS] = ri.Hunk_Alloc(maxp * (int)sizeof *pv, h_low);
	sf->made = ri.Hunk_Alloc((nf + 7) / 8, h_low);
	u16 (*pe)[Q3VU_M_ENTRIES] = ri.Hunk_AllocateTempMemory(maxp * (int)sizeof *pe);
	s16 *map = ri.Hunk_AllocateTempMemory(nv * (int)sizeof *map);
	memset(map, 0xFF, nv * sizeof *map);
	int np = 0;
	u32 fixed = 0, frame = 0;
	pc[0].nvert = 0, pc[0].nent = 0;
	for (int t = 0; t <= nt; t++) {
		struct piece *p = &pc[np];
		int fresh = 0;
		if (t < nt)
			for (int k = 0; k < 3; k++) {
				int v = tri[t].indexes[k];
				if (v < 0 || v >= nv)
					v = 0;
				fresh += map[v] < 0 && (k < 1 || tri[t].indexes[k] != tri[t].indexes[0]) &&
				         (k < 2 || tri[t].indexes[k] != tri[t].indexes[1]);
			}
		if (t == nt || p->nvert + fresh > Q3VU_M_VERTS || p->nent + 3 > Q3VU_M_ENTRIES) {
			/* the piece is whole: a multiple of four entries (the rest end no
			   triangle), its places */
			if (p->nent) {
				while (p->nent & 3)
					pe[np][p->nent++] = 0x8000;
				p->st_qw = (u8)((p->nvert * 6 + 15) / 16), p->idx_qw = (u16)((8 * (1 + p->nent / 4) + 15) / 16);
				p->pos_qw = (u8)((p->nvert * 6 + 15) / 16), p->nrm_qw = (u8)((p->nvert * 3 + 15) / 16);
				p->st = fixed, fixed += p->st_qw * 16u;
				p->idx = fixed, fixed += p->idx_qw * 16u;
				p->pos = frame, frame += p->pos_qw * 16u;
				p->nrm = frame, frame += p->nrm_qw * 16u;
				for (int k = 0; k < p->nvert; k++)
					map[pv[np][k]] = -1;
				np++;
			}
			if (t == nt || np == maxp)
				break;
			p = &pc[np];
			p->nvert = 0, p->nent = 0;
		}
		for (int k = 0; k < 3; k++) {
			int v = tri[t].indexes[k];
			if (v < 0 || v >= nv)
				v = 0;
			if (map[v] < 0)
				map[v] = (s16)p->nvert, pv[np][p->nvert++] = (u16)v;
			pe[np][p->nent++] = (u16)(map[v] * 4 | (k < 2 ? 0x8000 : 0));
		}
	}
	sf->npiece = np, sf->piece = pc, sf->frame_bytes = frame;
	sf->fixed = mx_heap_alloc(heap, fixed, 64);
	sf->frames = mx_heap_alloc(heap_frames, frame * (u32)nf, 64);
	sf->lock_at[0] = sf->lock_at[1] = 0, sf->lock_n = 0;
	sf->pv = pv;
	int ok = sf->fixed && sf->frames;
	if (ok) {
		memset(sf->fixed, 0, fixed);
		for (int i = 0; i < np; i++) {
			const struct piece *p = &pc[i];
			s16 *d = (s16 *)(sf->fixed + p->st);
			for (int k = 0; k < p->nvert; k++) {
				float a = st[pv[i][k]].st[0] * 4096.0f, b = st[pv[i][k]].st[1] * 4096.0f;
				d[3 * k] = (s16)(a > 32767 ? 32767 : a < -32767 ? -32767 : a);
				d[3 * k + 1] = (s16)(b > 32767 ? 32767 : b < -32767 ? -32767 : b);
				d[3 * k + 2] = 4096;
			}
			u16 *e = (u16 *)(sf->fixed + p->idx);
			e[0] = (u16)(p->nent / 4);
			memcpy(e + 4, pe[i], p->nent * sizeof *e);
		}
		mx_dcache_writeback(sf->fixed, fixed);
		model_bytes += fixed;
	}
	(void)xyz;
	ri.Hunk_FreeTempMemory(map);
	ri.Hunk_FreeTempMemory(pe);
	return ok ? 0 : -1;
}

/* A frame's block: its pieces' positions and normals from the file's. */
static void frame_make(struct msurf *sf, int f) {
	const md3Surface_t *s = sf->md3;
	const md3XyzNormal_t *x = (const md3XyzNormal_t *)((const u8 *)s + s->ofsXyzNormals) + f * s->numVerts;
	u8 *blk = sf->frames + (u32)f * sf->frame_bytes;
	memset(blk, 0, sf->frame_bytes);
	for (int i = 0; i < sf->npiece; i++) {
		const struct piece *p = &sf->piece[i];
		s16 *pos = (s16 *)(blk + p->pos);
		s8 *nrm = (s8 *)(blk + p->nrm);
		for (int k = 0; k < p->nvert; k++) {
			const md3XyzNormal_t *v = &x[sf->pv[i][k]];
			pos[3 * k] = v->xyz[0], pos[3 * k + 1] = v->xyz[1], pos[3 * k + 2] = v->xyz[2];
			/* the engine's: X cos(lat) sin(lng), Y sin(lat) sin(lng), Z cos(lng) */
			u32 lat = (u32)(v->normal >> 8) & 255, lng = (u32)v->normal & 255;
			float sl = sin_tab[lng];
			nrm[3 * k] = (s8)(sin_tab[(lat + 64) & 255] * sl * 127.0f);
			nrm[3 * k + 1] = (s8)(sin_tab[lat] * sl * 127.0f);
			nrm[3 * k + 2] = (s8)(sin_tab[(lng + 64) & 255] * 127.0f);
		}
	}
	mx_dcache_writeback(blk, sf->frame_bytes);
	sf->made[f >> 3] |= (u8)(1 << (f & 7));
	model_bytes += sf->frame_bytes;
	rp_model_stats.kb = model_bytes / 1024;
	rp_model_stats.made++;
}

qhandle_t rp_model_register(const char *name) {
	if (!name || !name[0] || strlen(name) >= MAX_QPATH || !models)
		return 0;
	for (int i = 1; i < nmodels; i++)
		if (!Q_stricmp(models[i].name, name))
			return models[i].md3 || name[0] == '*' ? i : 0;
	if (nmodels >= RP_MODELS)
		return 0;
	int h = nmodels++;
	struct rmodel *m = &models[h];
	Q_strncpyz(m->name, name, MAX_QPATH);
	m->md3 = NULL, m->surf = NULL;
	if (name[0] == '*')
		return h;
	void *file;
	long size = ri.FS_ReadFile(name, &file);
	if (size <= 0 || !file)
		return 0;
	md3Header_t *hd = file;
	if (size >= (long)sizeof *hd && hd->ident == MD3_IDENT && hd->version == MD3_VERSION && hd->numFrames > 0 &&
	    hd->numSurfaces <= MD3_MAX_SURFACES) {
		m->md3 = ri.Hunk_Alloc((int)size, h_low);
		memcpy(m->md3, file, size);
	}
	ri.FS_FreeFile(file);
	if (!m->md3)
		return 0;
	if (!sin_tab[64])
		for (int i = 0; i < 256; i++)
			sin_tab[i] = rp_sin1((float)i * (1.0f / 256.0f));
	hd = m->md3;
	m->surf = ri.Hunk_Alloc((hd->numSurfaces + 1) * (int)sizeof *m->surf, h_low);
	const md3Surface_t *s = (const md3Surface_t *)((const u8 *)hd + hd->ofsSurfaces);
	for (int i = 0; i < hd->numSurfaces; i++) {
		struct msurf *sf = &m->surf[i];
		sf->md3 = s;
		const md3Shader_t *sh = (const md3Shader_t *)((const u8 *)s + s->ofsShaders);
		sf->shader = s->numShaders > 0 ? rp_shader_register(sh->name, -1, 1) : 0;
		if (surf_make(sf) < 0)
			sf->npiece = 0;
		s = (const md3Surface_t *)((const u8 *)s + s->ofsEnd);
	}
	rp_model_stats.kb = model_bytes / 1024;
	return h;
}

static struct rmodel *model_get(qhandle_t h) { return h > 0 && h < nmodels && models[h].md3 ? &models[h] : NULL; }

void rp_model_bounds(qhandle_t h, vec3_t mins, vec3_t maxs) {
	struct rmodel *m = model_get(h);
	if (!m) {
		VectorClear(mins);
		VectorClear(maxs);
		return;
	}
	md3Frame_t *f = (md3Frame_t *)((byte *)m->md3 + m->md3->ofsFrames);
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

int rp_model_tag(orientation_t *tag, qhandle_t h, int startFrame, int endFrame, float frac, const char *tagName) {
	struct rmodel *m = model_get(h);
	md3Tag_t *a = m ? model_tag(m->md3, startFrame, tagName) : NULL;
	md3Tag_t *b = m ? model_tag(m->md3, endFrame, tagName) : NULL;
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

/* ---- skins ----------------------------------------------------------------------------------- */

/* a .skin file: lines of surface,shader; another name: a skin of that one
   shader for every surface */
qhandle_t rp_skin_register(const char *name) {
	if (!name || !name[0] || strlen(name) >= MAX_QPATH || !skins)
		return 0;
	for (int i = 1; i < nskins; i++)
		if (!Q_stricmp(skins[i].name, name))
			return i;
	if (nskins >= RP_SKINS)
		return 0;
	struct skin *sk = &skins[nskins];
	Q_strncpyz(sk->name, name, MAX_QPATH);
	sk->n = 0;
	const char *ext = strrchr(name, '.');
	if (!ext || Q_stricmp(ext, ".skin")) {
		sk->n = 1, sk->s[0].surf[0] = 0;
		sk->s[0].shader = rp_shader_register(name, -1, 1);
		return nskins++;
	}
	char *text;
	long size = ri.FS_ReadFile(name, (void **)&text);
	if (size <= 0 || !text)
		return 0;
	for (const char *p = text; *p && sk->n < SKIN_SURFS;) {
		char surf[MAX_QPATH], shader[MAX_QPATH];
		int a = 0, b = 0;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		while (*p && *p != ',' && *p != '\n' && *p != '\r') {
			if (a < MAX_QPATH - 1)
				surf[a++] = (char)tolower((unsigned char)*p);
			p++;
		}
		surf[a] = 0;
		if (*p == ',')
			p++;
		while (*p && *p != '\n' && *p != '\r') {
			if (b < MAX_QPATH - 1 && *p != ' ' && *p != '\t')
				shader[b++] = *p;
			p++;
		}
		shader[b] = 0;
		if (!a || !b || strstr(surf, "tag_"))
			continue;
		Q_strncpyz(sk->s[sk->n].surf, surf, MAX_QPATH);
		sk->s[sk->n].shader = rp_shader_register(shader, -1, 1);
		sk->n++;
	}
	ri.FS_FreeFile(text);
	return nskins++;
}

static qhandle_t skin_shader(qhandle_t h, const char *surf) {
	if (h <= 0 || h >= nskins)
		return 0;
	const struct skin *sk = &skins[h];
	if (sk->n == 1 && !sk->s[0].surf[0])
		return sk->s[0].shader;
	for (int i = 0; i < sk->n; i++)
		if (!Q_stricmp(sk->s[i].surf, surf))
			return sk->s[i].shader;
	return 0;
}

/* The entity's shader is one drawn after the world's blended surfaces (its
   first surface's first stage blends). */
int rp_model_blended(const refEntity_t *e) {
	struct rmodel *m = model_get(e->hModel);
	if (!m || !m->md3->numSurfaces)
		return 0;
	qhandle_t hs = e->customShader ? e->customShader
	               : e->customSkin ? skin_shader(e->customSkin, m->surf[0].md3->name)
	                               : m->surf[0].shader;
	struct rp_shader *sh = rp_shader_get(hs);
	return sh && sh->nstage && sh->st[0].blend != RP_BLEND_NONE;
}

/* The picture's pixels the entity's sphere covers in the view set; 0: none. */
int rp_model_rect(const refEntity_t *e, float *rect) {
	const struct rp_view *vw = &rp_view;
	struct rmodel *m = model_get(e->hModel);
	if (!m)
		return 0;
	const md3Frame_t *fr = (const md3Frame_t *)((const u8 *)m->md3 + m->md3->ofsFrames);
	int frame = e->frame >= 0 && e->frame < m->md3->numFrames ? e->frame : 0;
	float c[3], d[3], r = fr[frame].radius * 1.2f;
	for (int a = 0; a < 3; a++)
		c[a] = e->origin[a] + fr[frame].localOrigin[0] * e->axis[0][a] + fr[frame].localOrigin[1] * e->axis[1][a] +
		       fr[frame].localOrigin[2] * e->axis[2][a];
	VectorSubtract(c, vw->org, d);
	float w = vw->F[0] * d[0] + vw->F[1] * d[1] + vw->F[2] * d[2];
	float W = (float)mx_gs.w, H = (float)mx_gs.h;
	rect[0] = 0, rect[1] = 0, rect[2] = W, rect[3] = H;
	if (w - r > 4.0f) {
		float x = vw->cx - 2048.0f + W * 0.5f - vw->fx * (vw->L[0] * d[0] + vw->L[1] * d[1] + vw->L[2] * d[2]) / w;
		float y = vw->cy - 2048.0f + H * 0.5f - vw->fy * (vw->U[0] * d[0] + vw->U[1] * d[1] + vw->U[2] * d[2]) / w;
		float rx = vw->fx * r / (w - r), ry = vw->fy * r / (w - r);
		rect[0] = x - rx, rect[1] = y - ry, rect[2] = x + rx, rect[3] = y + ry;
	}
	return 1;
}

/* ---- a model in a scene -------------------------------------------------------------------- */

static inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

int rp_scene_world; /* the scene being drawn has the map's world (rp_main.c) */

/* A frame's block locked until the picture's frame being built is done; -1:
   it is not in memory. */
static int frame_lock(struct msurf *sf, int f) {
	const u8 *p = sf->frames + (u32)f * sf->frame_bytes;
	u32 now = mx_frame_number() + 1;
	for (int k = 0; k < 2; k++)
		if (sf->lock_at[k] == now && sf->lock_f[k] == f)
			return 0;
	if (!(sf->made[f >> 3] >> (f & 7) & 1))
		frame_make(sf, f);
	if (mx_mem_lock_try(p, sf->frame_bytes) < 0) {
		rp_model_stats.paged++;
		return -1;
	}
	mx_frame_unlock(p, sf->frame_bytes);
	u32 k = sf->lock_n++ & 1;
	sf->lock_at[k] = now, sf->lock_f[k] = f;
	return 0;
}

/* the surface on VU1: a chain of its pieces */
static const void *surf_chain(struct msurf *sf, int frame, int oldframe) {
	if (frame_lock(sf, frame) < 0 || frame_lock(sf, oldframe) < 0)
		return 0;
	u64 *c0 = rp_dyn((u32)sf->npiece * 6 + 1), *c = c0;
	if (!c)
		return 0;
	const u8 *fn = sf->frames + (u32)frame * sf->frame_bytes, *fo = sf->frames + (u32)oldframe * sf->frame_bytes;
	for (int i = 0; i < sf->npiece; i++) {
		const struct piece *p = &sf->piece[i];
		u32 n = p->nvert;
		c = mx_e_ref(c, sf->fixed + p->st, p->st_qw, mx_vif_stcycl(4, 1),
		             mx_vif_unpack(MX_UNPACK_V3_16, Q3VU_M_VERT, n, MX_UNPACK_FLG));
		c = mx_e_ref(c, fn + p->nrm, p->nrm_qw, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V3_8, Q3VU_M_VERT + 1, n, MX_UNPACK_FLG));
		c = mx_e_ref(c, fn + p->pos, p->pos_qw, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V3_16, Q3VU_M_VERT + 2, n, MX_UNPACK_FLG));
		c = mx_e_ref(c, fo + p->pos, p->pos_qw, mx_vif_nop(), mx_vif_unpack(MX_UNPACK_V3_16, Q3VU_M_VERT + 3, n, MX_UNPACK_FLG));
		c = mx_e_ref(c, sf->fixed + p->idx, p->idx_qw, mx_vif_stcycl(1, 1),
		             mx_vif_unpack(MX_UNPACK_V4_16, Q3VU_M_IDX, 1 + p->nent / 4u, MX_UNPACK_FLG));
		c = mx_e_cnt(c, 0, mx_vif_itop(n), mx_vif_mscnt());
		rp_model_stats.verts += n;
	}
	mx_e_ret(c, 0, 0, 0);
	rp_model_stats.pieces += (u32)sf->npiece;
	return c0;
}

/* the surface on the EE: its triangles as polygons, lit here */
static void surf_polys(const struct msurf *sf, const refEntity_t *e, int frame, int oldframe, const float *amb,
                       const float *dirc, const float *ld, const u8 *fixed, int lit) {
	const md3Surface_t *s = sf->md3;
	const md3Triangle_t *tri = (const md3Triangle_t *)((const u8 *)s + s->ofsTriangles);
	const md3St_t *st = (const md3St_t *)((const u8 *)s + s->ofsSt);
	const md3XyzNormal_t *xn = (const md3XyzNormal_t *)((const u8 *)s + s->ofsXyzNormals) + frame * s->numVerts;
	const md3XyzNormal_t *xo = (const md3XyzNormal_t *)((const u8 *)s + s->ofsXyzNormals) + oldframe * s->numVerts;
	float back = e->backlerp, keep = 1.0f - back;
	for (int t = 0; t < s->numTriangles; t++) {
		struct rp_vert v[3];
		for (int k = 0; k < 3; k++) {
			int i = tri[t].indexes[k];
			if (i < 0 || i >= s->numVerts)
				i = 0;
			float p[3];
			for (int a = 0; a < 3; a++)
				p[a] = ((float)xn[i].xyz[a] * keep + (float)xo[i].xyz[a] * back) * (float)MD3_XYZ_SCALE;
			for (int a = 0; a < 3; a++)
				v[k].xyz[a] = e->origin[a] + p[0] * e->axis[0][a] + p[1] * e->axis[1][a] + p[2] * e->axis[2][a];
			v[k].st[0] = st[i].st[0], v[k].st[1] = st[i].st[1];
			if (lit) {
				u32 lat = (u32)(xn[i].normal >> 8) & 255, lng = (u32)xn[i].normal & 255;
				float sl = sin_tab[lng];
				float n[3] = {sin_tab[(lat + 64) & 255] * sl, sin_tab[lat] * sl, sin_tab[(lng + 64) & 255]};
				float d = dot3(n, ld);
				d = d < 0 ? 0 : d;
				for (int a = 0; a < 3; a++) {
					float cc = amb[a] + dirc[a] * d;
					v[k].rgba[a] = (u8)(cc > 255 ? 255 : cc);
				}
				v[k].rgba[3] = fixed[3];
			} else
				memcpy(v[k].rgba, fixed, 4);
		}
		rp_poly(v, 3);
	}
}

void rp_model_draw(const refEntity_t *e, float t) {
	const struct rp_view *vw = &rp_view;
	struct rmodel *m = model_get(e->hModel);
	if (!m)
		return;
	const md3Header_t *h = m->md3;
	int frame = e->frame, oldframe = e->oldframe;
	if (e->renderfx & RF_WRAP_FRAMES)
		frame %= h->numFrames, oldframe %= h->numFrames;
	if (frame < 0 || frame >= h->numFrames)
		frame = 0;
	if (oldframe < 0 || oldframe >= h->numFrames)
		oldframe = 0;
	/* its sphere (the frames') against the frustum and the guard band */
	const md3Frame_t *fr = (const md3Frame_t *)((const u8 *)h + h->ofsFrames);
	const md3Frame_t *fn = &fr[frame], *fo = &fr[oldframe];
	float r = fn->radius > fo->radius ? fn->radius : fo->radius, c[3], d[3];
	float sc = e->nonNormalizedAxes ? VectorLength(e->axis[0]) : 1.0f;
	for (int a = 0; a < 3; a++)
		c[a] = e->origin[a] + fn->localOrigin[0] * e->axis[0][a] + fn->localOrigin[1] * e->axis[1][a] +
		       fn->localOrigin[2] * e->axis[2][a];
	r = (r + Distance(fn->localOrigin, fo->localOrigin)) * sc;
	int cut = 0;
	for (int p = 0; p < 5; p++) {
		if (dot3(vw->fr[p].n, c) - vw->fr[p].d < -r * vw->fr_len[p]) {
			rp_model_stats.culled++;
			return;
		}
		cut |= dot3(vw->gb[p].n, c) - vw->gb[p].d < r * vw->gb_len[p];
	}
	VectorSubtract(c, vw->org, d);
	float w = dot3(vw->F, d);
	u32 mask = vw->all;
	if (!cut && w - r > 4.0f) {
		float y = vw->cy - 2048.0f + (float)mx_gs.h * 0.5f - vw->fy * dot3(vw->U, d) / w, ry = vw->fy * r / (w - r);
		mask = rp_strip_lines(y - ry, y + ry);
	}
	if (!mask) {
		rp_model_stats.culled++;
		return;
	}
	/* the light, its direction in the model's space */
	vec3_t amb, dirc, dir;
	float ld[3];
	rp_light_at(e->renderfx & RF_LIGHTING_ORIGIN ? e->lightingOrigin : e->origin, rp_scene_world, amb, dirc, dir);
	for (int a = 0; a < 3; a++) {
		amb[a] = amb[a] > 127 ? 127 : amb[a];
		ld[a] = dot3(dir, e->axis[a]) / sc;
	}
	const void *matrix = 0;
	if (cut)
		rp_model_stats.cut++;
	else
		rp_model_stats.drawn++;
	for (int i = 0; i < h->numSurfaces; i++) {
		struct msurf *sf = &m->surf[i];
		qhandle_t hs = e->customShader ? e->customShader : e->customSkin ? skin_shader(e->customSkin, sf->md3->name) : sf->shader;
		struct rp_shader *sh = rp_shader_get(hs);
		if (!sh || !sh->nstage || sh->nodraw || !sf->npiece) {
			rp_model_stats.nostage++;
			continue;
		}
		rp_model_stats.stages += sh->nstage - 1u;
		const struct rp_stage *stg = &sh->st[0];
		struct rp_state s;
		if (!rp_stage_state(stg, t, &s)) {
			rp_model_stats.nostage++;
			continue;
		}
		/* the colour: lit, or the stage's own */
		u8 col[4];
		int lit = stg->rgbgen == RGB_LIGHTING;
		rp_stage_colour(stg, t, e->shaderRGBA, e->shaderRGBA, col);
		s.texels = 2.0f;
		if (cut) {
			rp_poly_state(&s);
			surf_polys(sf, e, frame, oldframe, amb, dirc, ld, col, lit);
			rp_poly_flush();
			continue;
		}
		s.prog = RP_PROG_MODEL;
		float (*u)[4] = s.model;
		u[0][0] = e->backlerp;
		for (int a = 0; a < 3; a++) {
			u[1][a] = lit ? amb[a] : (float)col[a];
			/* normals of length 127 */
			u[2][a] = lit ? dirc[a] * ld[0] * (1.0f / 127.0f) : 0;
			u[3][a] = lit ? dirc[a] * ld[1] * (1.0f / 127.0f) : 0;
			u[4][a] = lit ? dirc[a] * ld[2] * (1.0f / 127.0f) : 0;
			u[5][a] = 255.0f;
		}
		u[1][3] = (float)col[3], u[5][3] = 255.0f;
		if (!matrix)
			matrix = rp_matrix((float)MD3_XYZ_SCALE, e->origin, e->axis);
		const void *chain = surf_chain(sf, frame, oldframe);
		if (!chain || rp_run(matrix, &s) < 0)
			continue;
		rp_draw(chain, mask, 0, (u32)sf->md3->numTriangles * 3u, (u32)sf->npiece);
		rp_model_stats.surfs++;
	}
}
