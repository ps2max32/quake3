/* rp_world.h: a map's world as the PS2 renderer draws it (rp_world.c), from
   the .pmap file tools/ps2map.py makes. Used by the renderer and by the
   viewer (tools/q3view). */

#ifndef RP_WORLD_H
#define RP_WORLD_H

#include <mx/types.h>

struct rp_cam {
	float org[3];
	float axis[3][3]; /* forward, left, up */
	float fov_x, fov_y; /* degrees */
};

struct rp_world_stats {
	u32 cluster;                 /* the camera's (0xFFFFFFFF: outside) */
	u32 seen, groups;            /* groups the cluster sees; of those in the frustum */
	u32 drawn, clip, back;       /* their chunks: drawn; of those across a plane; facing away (not drawn) */
	u32 verts;                   /* strip vertices of the chunks drawn */
	u32 sent, batches;           /* strip vertices and VU1 batches sent, over all strips of beam racing */
	u32 setups;                  /* program set-ups (a material, or the other program for it) */
	u32 qw;                      /* quadwords written into the frame's stream */
};

/* The map at p (the whole file, locked in memory: the DMAC reads its chunks
   and textures from it) for a picture of w x h pixels in GS memory (mx_gs.w,
   mx_gs.h): its textures get their places in GS memory. Returns 0, or -1 if
   it is no map of this version. */
int rp_world_load(const void *p, u32 size);
/* In a frame, before drawing: sends the next textures to GS memory (beam
   racing: in the frame's once record). Returns how many are left; draw the
   world when it is 0. */
int rp_world_upload(void);
/* In a frame: the frame's GS registers and VU1 set-up, once a frame before
   anything is drawn. */
void rp_frame_setup(void);
/* In a frame: the world seen by cam. */
void rp_world_draw(const struct rp_cam *cam, struct rp_world_stats *st);

#endif
