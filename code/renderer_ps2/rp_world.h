/* rp_world.h: a map's world as the PS2 renderer draws it (rp_world.c), from
   the .pmap file tools/ps2map.py makes. Used by the renderer and by the
   viewer (tools/q3view). */

#ifndef RP_WORLD_H
#define RP_WORLD_H

#include <mx/types.h>

#include "rp_list.h"

struct rp_world_stats {
	u32 cluster;                 /* the camera's (0xFFFFFFFF: outside) */
	u32 seen, groups;            /* groups the cluster sees; of those in the frustum */
	u32 drawn, clip, back;       /* their chunks: drawn; of those across a plane; facing away (not drawn) */
	u32 verts;                   /* strip vertices of the chunks drawn */
};

/* The map at p (the whole file, mapped: its chunks are locked in memory
   here, the DMAC reads them where they lie), after rp_tex_init. Returns 0,
   or -1 if it is no map of this version or there is no memory to keep its
   chunks in. rp_world_free: before the file goes (and no frame in flight
   may still draw it). */
int rp_world_load(const void *p, u32 size);
void rp_world_free(void);
/* The map's materials: their number; one's name (its shader's) and flags. */
enum { RP_MAT_LIGHTMAP = 1, RP_MAT_SKY = 2, RP_MAT_NODRAW = 4, RP_MAT_DEFORM = 8, RP_MAT_TCMOD = 16, RP_MAT_SCRIPT = 32,
       RP_MAT_VERTEX = 64, RP_MAT_ENV = 128,
       RP_MAT_VLM = 256 /* rgbGen vertex times a lightmap: the vertices' colours are halved already */,
       RP_MAT_ALIT = 512 /* its alpha is tested: the vertices' colours are the lightmap's light */ };
u32 rp_world_materials(void);
const char *rp_world_material(u32 i, u32 *flags);
/* The passes of a material whose shader is a script (RP_MAT_SCRIPT), asked
   for each one a view draws: fn writes up to RP_WORLD_PASSES states (prog
   RP_PROG_STAGE: the texture placed, the blend, the texture matrix and the
   colour at this frame; or prog RP_PROG_WORLD with lm_only: the chunks'
   lightmap alone, filled in here) and returns their number; *first 1: pass 0
   is drawn in place of the map's own pass of the material, 0: they all
   follow it. */
#define RP_WORLD_PASSES 5 /* a script's stages and a lightmap pass */
typedef u32 (*rp_world_passes_fn)(u32 mat, struct rp_state *pass, int *first);
void rp_world_script(rp_world_passes_fn fn);
/* After rp_world_cull: the strips in which the view sees the sky's surfaces
   (0: none). */
u32 rp_world_sky(void);
/* And where in strip k: r[0..3] the picture's pixels x0, x1, y0, y1 that its
   sky surfaces' boxes cover; 0: none. */
int rp_world_sky_rect(u32 strip, float *r);

/* In a frame, after rp_view_set: what of the world the view sees. Then its
   draws into the frame's list: those that write depth (blended 0), and the
   blended ones. */
/* Before rp_world_cull: the game's mask of areas (a bit set: shut off by a
   closed door, its chunks are not drawn); 0: none. */
void rp_world_areas(const u8 *mask);
void rp_world_cull(struct rp_world_stats *st);
/* After rp_world_cull: the view's i-th chunk of quads that turn to the eye
   (deformVertexes autosprite, autosprite2: lamps' glows), 0 when there is
   none: its material and its records, 16 floats each: the centre; the
   radius (autosprite: a square facing the eye) or 0; the half of the line it
   turns about and its half width (autosprite2); its four corners' texture
   coordinates. Returns the records. */
u32 rp_world_sprites(u32 i, const float **rec, u32 *mat);
void rp_world_add(int blended);
/* The view's dynamic lights on the world's opaque chunks (after
   rp_world_add(0)): each light's spot, added to the picture where the chunks
   near it are; at most max lights, the first. */
struct rp_dlight;
void rp_world_dlights(const struct rp_dlight *l, unsigned n, unsigned max);
/* The fog on the view's opaque chunks that lie in a fog volume: after
   rp_world_add(0) and the lights. */
void rp_world_fog(void);
/* the map loaded has textures with a level over 128 texels (the pool of fine levels) */
int rp_world_fine(void);
/* The fog at a place of the world, for what stands in it (a model is small
   against a fog's depth: one strength for all of it, where the engine's is
   each vertex's): its colour (0 .. 255) and strength (0 .. 1) as the
   world's surfaces there have it this frame. 0: no fog there. */
int rp_world_fog_at(const float *p, float *colour, float *strength);
struct rp_dl_stats {
	unsigned lights, runs, chunks; /* lights drawn, their runs, the chunks drawn again for them */
};
extern struct rp_dl_stats rp_dl_stats;
/* The map's brush model n ("*n": a door, a platform) at origin, turned by
   axis: its surfaces that write depth (blended 0) or the blended ones, into
   the frame's list. -1: the list is full. */
int rp_world_bmodel(u32 n, const float *origin, const float (*axis)[3], int blended);

#endif
