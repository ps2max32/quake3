/* rp_model.h: the game's MD3 models in the PS2 renderer (rp_model.c), and
   the light they stand in (the map's light grid). The game's side of the
   renderer. */

#ifndef RP_MODEL_H
#define RP_MODEL_H

#include "../qcommon/q_shared.h"
#include "../qcommon/qfiles.h"
#include "../renderercommon/tr_public.h"

/* Before the first model of a registration (the hunk was cleared; no frame
   in flight draws a model). */
void rp_models_begin(void);
/* The engine's RegisterModel, RegisterSkin, LerpTag, ModelBounds. */
qhandle_t rp_model_register(const char *name);
qhandle_t rp_skin_register(const char *name);
int rp_model_tag(orientation_t *tag, qhandle_t h, int startFrame, int endFrame, float frac, const char *tagName);
void rp_model_bounds(qhandle_t h, vec3_t mins, vec3_t maxs);
int rp_model_count(void);

/* The map's light grid, from its BSP (name: maps/<name>.bsp); none: every
   model is lit as on the screen's small scenes. */
void rp_light_load(const char *name);
/* The light at a point: 0..255 as the engine has them, the direction to
   the light. world 0: a small scene's (no map). */
void rp_light_at(const vec3_t p, int world, vec3_t ambient, vec3_t directed, vec3_t dir);

/* A model entity of the scene being drawn (the view set), at time t: its
   surfaces become runs of the frame's list. */
void rp_model_draw(const refEntity_t *e, float t);

/* Its shader blends: it is drawn after the world's blended surfaces. */
int rp_model_blended(const refEntity_t *e);
/* The picture's pixels its sphere covers in the view set (rect: x0, y0, x1,
   y1); 0: no model. */
int rp_model_rect(const refEntity_t *e, float *rect);
/* the scene being drawn has the map's world: the models stand in its light */
extern int rp_scene_world;

struct rp_model_stats {
	u32 drawn, culled, cut;   /* entities: inside the guard band, out of view, across a plane (cut on VU1) */
	u32 surfs, pieces, verts; /* of those drawn */
	u32 nostage, stages;      /* surfaces without a stage to draw; stages after the first, not drawn yet */
	u32 paged;                /* frames of surfaces not ready when a draw wanted them (their vertices not in memory, or no room) */
	u32 made, evicted;        /* frames of surfaces made ready; of them given up for others */
	u32 kb;                   /* memory of the models' vertices */
};
extern struct rp_model_stats rp_model_stats;

#endif
