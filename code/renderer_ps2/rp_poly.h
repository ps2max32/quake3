/* rp_poly.h: what the game hands the renderer vertex by vertex (rp_poly.c):
   polygons in the world (sprites, marks, beams, the engine's polys) and
   pictures on the screen. */

#ifndef RP_POLY_H
#define RP_POLY_H

#include "rp_list.h"

/* the engine's polyVert_t; the colour here is the GS's: 0x80 the texture's
   own colour, 0x80 an alpha of 1 */
struct rp_vert {
	float xyz[3];
	float st[2];
	u8 rgba[4];
};

/* The state of the polygons that follow (the texture placed, the blend):
   they become draws of runs of it, of the view set last. */
void rp_poly_state(const struct rp_state *s);
/* A convex polygon of n vertices (3 .. 16), world coordinates. */
void rp_poly(const struct rp_vert *v, u32 n);

/* The state of the pictures that follow (prog and z are set here). */
void rp_pic_state(const struct rp_state *s);
/* A picture: x, y, w, h in the picture's pixels (mx_gs.w x mx_gs.h; the
   game's 1080 lines are a field's 540), s, t of its corners, its colour
   (R in the low byte; 0x80 a channel: the texture's own). */
void rp_pic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, u32 rgba);

/* What was gathered into draws: before anything else makes runs, and before
   the view changes. */
void rp_poly_flush(void);

struct rp_poly_stats {
	u32 polys, verts, cut, culled; /* polygons: drawn, their vertices, across a plane, out of view */
	u32 pics;
};
extern struct rp_poly_stats rp_poly_stats;

#endif
