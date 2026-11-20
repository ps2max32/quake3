/* rp_list.h: a frame's draws (rp_list.c). Everything VU1 or the GS draws is
   a DMA chain that ends with a RET (a map's chunk in its file, or one built
   for the frame in rp_dyn's memory), CALLed from the record of each strip of
   beam racing it touches. Draws are kept in the order they were made, in
   runs: the draws of one matrix and one state (texture, blend, program). */

#ifndef RP_LIST_H
#define RP_LIST_H

#include <mx/tex.h>
#include <mx/types.h>

/* ---- the view ------------------------------------------------------------------------ */

struct rp_cam {
	float org[3];
	float axis[3][3]; /* forward, left, up */
	float fov_x, fov_y; /* degrees */
};

/* a plane a point is inside of when n . p >= d */
struct rp_plane {
	float n[3], d;
};

struct rp_view {
	float org[3], F[3], L[3], U[3];
	float fx, fy;               /* pixels a unit at distance 1 */
	float flod;                 /* fx at a picture 1920 wide: the textures' levels (r_ps2width) */
	float cx, cy;               /* the view's centre in the GS's coordinates (2048: the picture's) */
	float rowx[3], rowy[3];     /* a world direction to GS x * w and y * w */
	struct rp_plane fr[5];      /* the frustum: near, left, right, bottom, top */
	struct rp_plane gb[5];      /* the same for the guard band (a vertex outside is cut on VU1) */
	float fr_len[5], gb_len[5]; /* the lengths of their normals */
	u32 nstrip, strip_h, all;   /* beam racing's strips: how many, their lines, all of them as a mask */
	u32 serial;                 /* another for every view set */
	/* the lines between strips as planes through the eye, a line and a half
	   below (lo) and above (hi) each: a point is below the line when n . p > d */
	struct rp_plane sb_lo[15], sb_hi[15];
	u8 sb_near[15][3], sb_far[15][3]; /* a box's numbers (mins at 0, maxs at 4) nearest and farthest along them */
	float strip_inv;            /* 1 / strip_h */
	/* the same lines for a point's F . d and fy U . d (d from the eye): below
	   line j when sb_klo[j] * F . d - fy U . d > 0, above when sb_khi[j]'s is < 0 */
	float sb_klo[15], sb_khi[15];
};
extern struct rp_view rp_view;

/* The view of cam over the whole picture; x, y, w, h (pixels of the
   picture, mx_gs.w x mx_gs.h): over that part of it. */
void rp_view_set(const struct rp_cam *cam);
void rp_view_rect(const struct rp_cam *cam, float x, float y, float w, float h);

/* the corner of a box farthest along a plane's normal (far 1) or least far
   (0): its distance from the plane */
static inline float rp_box_dist(const struct rp_plane *p, const float *mins, const float *maxs, int far) {
	float d = -p->d;
	for (int k = 0; k < 3; k++)
		d += p->n[k] * ((p->n[k] >= 0) == far ? maxs[k] : mins[k]);
	return d;
}
/* The strips a box (world coordinates; mins, a number between, maxs: as a
   map's group and chunk have them) is drawn in, as a mask. */
u32 rp_strip_mask(const float *box);
/* The same for the hull of n points. */
u32 rp_strip_points(const float (*p)[3], u32 n);
/* The strips of the picture's lines y0 .. y1. */
u32 rp_strip_lines(float y0, float y1);

/* ---- memory for a frame's chains ---------------------------------------------------- */

/* qw quadwords the DMAC may read until the frame being built is done, to be
   written once, in order (uncached); 0: none left (the draw is left out). */
u64 *rp_dyn(u32 qw);
/* Give back the end of the last rp_dyn: it ends at end. */
void rp_dyn_end(u64 *end);
/* its memory's physical range (for finding a stuck DMA) */
void rp_dyn_range(u32 *lo, u32 *hi);

/* ---- runs and draws ------------------------------------------------------------------ */

enum { RP_BLEND_NONE, RP_BLEND_ADD, RP_BLEND_ALPHA, RP_BLEND_FILTER, RP_BLEND_OTHER, RP_BLEND_ADD_ALPHA,
       RP_BLEND_INVFILTER, RP_BLEND_INVALPHA /* the picture where the alpha is 1 */,
       RP_BLEND_BRIGHTEN /* the picture times 1 + alpha: a light on what is there */ };
enum { RP_ALPHA_NONE, RP_ALPHA_GT0, RP_ALPHA_LT128, RP_ALPHA_GE128,
       RP_ALPHA_DEPTH /* no pixel passes, and one that fails writes its depth alone: depth without colour */ };
/* the programs of a run: the world's (texture and lightmap from one vertex
   when the state has a lightmap); none: its draws are GIF packets (sprites
   of ST, RGBAQ, XYZ2 a vertex, PRIM from the state), no matrix; an MD3
   model's (tools/ps2vu.py, q3m: the state's model uniforms); a stage's: the
   world's one-pass program with the state's texture matrix and colour
   (q3w1t), for draws in the world's batch format; a dynamic light's: its
   spot laid on the world's batches (q3wd: the state's model[0 .. 4]: the
   position to the spot's coordinates by columns, the light's colour) */
enum { RP_PROG_WORLD, RP_PROG_GS, RP_PROG_MODEL, RP_PROG_STAGE, RP_PROG_DLIGHT, RP_PROGS };
/* a model's triangles left out by the sign of their screen's cross product:
   none, those of one sign, those of the other */
enum { RP_CULL_NONE, RP_CULL_NEG, RP_CULL_POS };
/* depth: tested and written; tested; neither (2D) */
enum { RP_Z_WRITE, RP_Z_TEST, RP_Z_NONE };

struct rp_state {
	struct mx_tex tex, lm; /* in GS memory this frame (rp_image_place) */
	u8 tex_ok, lm_ok;
	u8 tcc;                /* the texture's alpha is its own (else 0x80) */
	u8 blend, alpha;       /* RP_BLEND_*, RP_ALPHA_* */
	u8 z;                  /* RP_Z_* */
	u8 prog;               /* RP_PROG_* */
	u8 nomip;              /* the first level only, bilinear */
	u8 clamp;              /* the texture is not repeated */
	u8 cull;               /* RP_PROG_MODEL: RP_CULL_* */
	u8 lm_only;            /* RP_PROG_WORLD: the lightmap pass alone (the texture's draws nothing); 2: at half */
	u8 menv;               /* RP_PROG_MODEL: tcGen environment (model[6 .. 9]); the world's passes: 1 a stage
	                          with tcGen environment, 2 the shine of alphaGen lightingSpecular (rp_world.c) */
	float texels;          /* of the texture's first level a unit of the surface (the MIPMAP level) */
	/* RP_PROG_STAGE: the texture coordinates' matrix (s' = tm[0] s + tm[1] t +
	   tm[2], t' = tm[3] s + tm[4] t + tm[5]); the colour: the vertex's times
	   cmul plus cadd (the GS's units, 0 .. 255) */
	float tm[6], cmul[4], cadd[4];
	/* RP_PROG_MODEL: the old frame's share (x); the colour without light;
	   the directed light's colour times its direction's x, y, z (in the
	   model's space, for normals of length 127); the colour's limit; menv:
	   the eye's direction from the model, each of its numbers times (-1, 0,
	   1) / 127^2, and (0.5 - 0.5 y, 0.5 + 0.5 z, 1) of it (tools/ps2vu.py,
	   q3me). The last: a state of another program is set and kept up to
	   here (RP_STATE_HEAD). */
	float model[10][4];
};
#define RP_STATE_HEAD __builtin_offsetof(struct rp_state, model)

/* A matrix for positions in units of scale at origin, turned by axis (0:
   not turned): its chain, to give to rp_run. 0: no memory. */
const void *rp_matrix(float scale, const float *origin, const float (*axis)[3]);
/* A new run: the draws after it are of this matrix and state. -1: no room. */
int rp_run(const void *matrix, const struct rp_state *s);
/* The last run made (-1: none): who makes draws for a run of its own asks
   whether it still is the last. */
int rp_run_last(void);
/* A draw of the last run: the chain, the strips it is drawn in, 1: across
   the near plane or the guard band (the programs that cut); verts, batches:
   for the counts. */
void rp_draw(const void *chain, u32 mask, int clip, u32 verts, u32 batches);
/* The depth of the picture's pixels x0, y0 .. x1, y1 back to the farthest
   (a scene of its own on the screen, the view weapon): a draw of a run of
   its own. */
void rp_zclear(float x0, float y0, float x1, float y1);

struct rp_list_stats {
	u32 runs, draws;
	u32 sent, batches; /* vertices and VU1 batches, over all strips */
	u32 setups;        /* program set-ups */
	u32 qw;            /* quadwords written into the records */
	u32 dyn_qw;        /* and into rp_dyn's memory */
	u32 lost;          /* draws left out: no room */
	u32 k_sent[16], k_batches[16], k_setups[16]; /* the last frame's, by strip */
	u32 k_msent[16], k_mbatches[16], k_draws[16]; /* of them the models' program's; draws (CALLs) */
	u32 k_stream_kb[16];                          /* textures sent into the stream area (rp_tex.h) */
	u32 p_sent[RP_PROGS], p_batches[RP_PROGS], p_setups[RP_PROGS]; /* sent, batches and setups by program (RP_PROG_*) */
	u32 p_clip[RP_PROGS];                        /* of p_sent, by the programs that cut */
};
extern struct rp_list_stats rp_list_stats;

/* Once, when the screen is set up: bytes of memory for the frames' chains
   (rp_dyn), for all the frames in flight. */
void rp_list_init(u32 bytes);
/* A frame: after mx_frame_begin_n (the frame's GS registers and VU1 set-up
   into the record, the list empty); before mx_frame_end (the draws into the
   strips' records). */
void rp_list_begin(void);
void rp_list_end(void);

/* for finding faults and costs: 1 the textures' first level only, 2 no
   lightmap pass, 4 every draw with the programs that test nothing, 8 with
   those that cut, 16 the GS draws nothing (the vertices as XYZ3: VIF1 and
   VU1 work as ever), 32 no textures, 64 chunks that face away drawn too,
   128 a picture of the overdraw: every triangle adds 1/31 of its colour,
   without texture, lightmap or depth test (with a black clear: a pixel's
   largest channel times 31 / 255 is about the triangles drawn over it),
   256 no strip records (everything in every pass), 1024 no CLUT is loaded
   (TEX0.CLD 0: wrong colours), 2048 none for the lightmap, 65536 the runs'
   GS registers left out (wrong textures and blends: what they cost), 131072
   every list sent twice, the second time not drawn (VIF1 and VU1 work
   twice, the GS once: whether they wait for each other), 262144 no late
   lightmap (a script's $lightmap after two stages: the map's own pass),
   524288 no sky, 1048576 every texture sent into the stream area waits for VU1 first (a FLUSH), 67108864 a model's
   pieces each in the strips of its own box (not in all the model is in),
   134217728 a chunk of the world in all the strips its group is in,
   268435456 the world's boxes tested by the FPU, not by VU0, 536870912 by
   both, and the boxes where they differ counted (the run's report) */
/* The guard band: how far from the GS's centre (2048, the picture's) a vertex
   may be, in pixels each way; a triangle with a vertex outside is cut there
   on VU1 (the programs that cut). The GS's own range is 2047. r_ps2guardx,
   r_ps2guardy. */
extern float rp_guard_x, rp_guard_y;
extern unsigned rp_debug;
/* 1: the world's and the models' textures between two of their levels
   (TEX1's MMIN 5), not from the nearest one (4): no line on a floor where
   the level changes. The GS reads eight texels a pixel for four. */
extern int rp_trilinear;

#endif
