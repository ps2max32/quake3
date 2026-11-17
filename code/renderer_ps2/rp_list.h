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
/* The strips of the picture's lines y0 .. y1. */
u32 rp_strip_lines(float y0, float y1);

/* ---- memory for a frame's chains ---------------------------------------------------- */

/* qw quadwords the DMAC may read until the frame being built is done, to be
   written once, in order (uncached); 0: none left (the draw is left out). */
u64 *rp_dyn(u32 qw);
/* Give back the end of the last rp_dyn: it ends at end. */
void rp_dyn_end(u64 *end);

/* ---- runs and draws ------------------------------------------------------------------ */

enum { RP_BLEND_NONE, RP_BLEND_ADD, RP_BLEND_ALPHA, RP_BLEND_FILTER, RP_BLEND_OTHER, RP_BLEND_ADD_ALPHA,
       RP_BLEND_INVFILTER };
enum { RP_ALPHA_NONE, RP_ALPHA_GT0, RP_ALPHA_LT128, RP_ALPHA_GE128 };
/* the programs of a run: the world's (texture and lightmap from one vertex
   when the state has a lightmap); none: its draws are GIF packets (sprites
   of ST, RGBAQ, XYZ2 a vertex, PRIM from the state), no matrix; an MD3
   model's (tools/ps2vu.py, q3m: the state's model uniforms) */
enum { RP_PROG_WORLD, RP_PROG_GS, RP_PROG_MODEL };
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
	float texels;          /* of the texture's first level a unit of the surface (the MIPMAP level) */
	/* RP_PROG_MODEL: the old frame's share (x); the colour without light;
	   the directed light's colour times its direction's x, y, z (in the
	   model's space, for normals of length 127); the colour's limit */
	float model[6][4];
};

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
};
extern struct rp_list_stats rp_list_stats;

/* Once, when the screen is set up. */
void rp_list_init(void);
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
   (TEX0.CLD 0: wrong colours), 2048 none for the lightmap */
extern unsigned rp_debug;

#endif
