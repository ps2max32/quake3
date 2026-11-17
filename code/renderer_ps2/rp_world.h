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

/* The map at p (the whole file, locked in memory: the DMAC reads its chunks
   and textures from it), after rp_tex_init. Returns 0, or -1 if it is no map
   of this version. rp_world_free: before the file goes (and no frame in
   flight may still draw it). */
int rp_world_load(const void *p, u32 size);
void rp_world_free(void);
/* In a frame, after rp_view_set: what of the world the view sees. Then its
   draws into the frame's list: those that write depth (blended 0), and the
   blended ones. */
void rp_world_cull(struct rp_world_stats *st);
void rp_world_add(int blended);

#endif
