/* q3view: flies a recorded camera through a map with the PS2 renderer's world
   (code/renderer_ps2/rp_world.c) in 1080i beam racing, and says what a frame
   costs: the EE's cycles building it, the chunks and vertices sent, and beam
   racing's passes (PLAN-PS2.md, phase 1).

     q3view.elf [map.pmap [cameras.cam [shot [first [count]]]]]

   map.pmap: tools/ps2map.py's (default q3dm6.pmap); cameras.cam: quake3.cam, as
   a REN=ps2 build of the game writes it during a timedemo; shot: the camera
   whose picture is saved as q3view.tga when the flight is over (0: none);
   first, count: the cameras flown (default all). Each camera is one frame.
   A sixth argument: rp_world.c's rp_debug (and 4096: a profile of the flight). */

#include <mx/hires.h>
#include <mx/mx.h>
#include <mx/prof.h>
#include <mx/vm.h>

#include "rp_poly.h"
#include "rp_tex.h"
#include "rp_world.h"

struct cam {
	int frame, time;
	float org[3], axis[3][3], fov_x, fov_y;
	int x, y, w, h;
	unsigned ents, polys, polyverts, lights;
};

static u8 rec[4 * 128 * 1024] MX_CORE_BSS MX_ALIGNED(64);
/* the strips' own records: four frames of up to 16 strips */
static u8 srec[4 * 16 * 24 * 1024] MX_CORE_BSS MX_ALIGNED(64);

static u32 num(const char *s) {
	u32 v = 0;
	for (; *s >= '0' && *s <= '9'; s++)
		v = v * 10 + (u32)(*s - '0');
	return v;
}

/* a frame's counts: the world's and the list's */
struct fstats {
	u32 seen, groups, drawn, clip, back, verts, sent, batches, setups, qw;
};

static void pv(struct rp_vert *v, const struct rp_cam *c, float f, float l, float u, float s, float t) {
	for (int k = 0; k < 3; k++)
		v->xyz[k] = c->org[k] + f * c->axis[0][k] + l * c->axis[1][k] + u * c->axis[2][k];
	v->st[0] = s, v->st[1] = t, v->rgba[0] = v->rgba[1] = v->rgba[2] = v->rgba[3] = 128;
}

/* rp_debug 8192: polygons and pictures (rp_poly.c) with the map's first
   textures: a square 64 units ahead; one 24 units ahead, turned, that goes
   behind the eye; a floor 40 units below from behind the eye to far ahead;
   two blended squares; pictures in the corners and a row of small ones. */
static void test_draws(const struct rp_cam *c) {
	struct rp_image *im[3] = {rp_image_at(0), rp_image_at(1), rp_image_at(2)};
	struct rp_state s = {0};
	const struct mx_tex *t;
	struct rp_vert q[4];
	s.z = RP_Z_WRITE, s.texels = 2;
	if (im[0] && (t = rp_image_place(im[0])))
		s.tex = *t, s.tex_ok = 1;
	rp_poly_state(&s);
	pv(&q[0], c, 64, 20, 20, 0, 0), pv(&q[1], c, 64, -20, 20, 1, 0), pv(&q[2], c, 64, -20, -20, 1, 1), pv(&q[3], c, 64, 20, -20, 0, 1);
	rp_poly(q, 4);
	pv(&q[0], c, 60, 50, 12, 0, 0), pv(&q[1], c, -8, 30, 12, 2, 0), pv(&q[2], c, -8, 30, -12, 2, 1), pv(&q[3], c, 60, 50, -12, 0, 1);
	rp_poly(q, 4);
	if (im[1] && (t = rp_image_place(im[1])))
		s.tex = *t, s.tex_ok = 1;
	rp_poly_state(&s);
	pv(&q[0], c, 3000, 300, -40, 0, 0), pv(&q[1], c, 3000, -300, -40, 4, 0), pv(&q[2], c, -100, -300, -40, 4, 7), pv(&q[3], c, -100, 300, -40, 0, 7);
	rp_poly(q, 4);
	s.blend = RP_BLEND_ADD;
	rp_poly_state(&s);
	pv(&q[0], c, 50, 40, 10, 0, 0), pv(&q[1], c, 50, 10, 10, 1, 0), pv(&q[2], c, 50, 10, -20, 1, 1), pv(&q[3], c, 50, 40, -20, 0, 1);
	rp_poly(q, 4);
	s.blend = RP_BLEND_ALPHA;
	if (im[2] && (t = rp_image_place(im[2])))
		s.tex = *t, s.tex_ok = 1;
	rp_poly_state(&s);
	pv(&q[0], c, 40, -5, 0, 0, 0), pv(&q[1], c, 40, -30, 0, 1, 0), pv(&q[2], c, 40, -30, -25, 1, 1), pv(&q[3], c, 40, -5, -25, 0, 1);
	q[0].rgba[3] = q[1].rgba[3] = 32, q[2].rgba[3] = q[3].rgba[3] = 100;
	rp_poly(q, 4);
	s = (struct rp_state){0};
	if (im[2] && (t = rp_image_place(im[2])))
		s.tex = *t, s.tex_ok = 1;
	s.clamp = 1;
	rp_pic_state(&s);
	rp_pic(16, 8, 256, 128, 0, 0, 1, 1, 0x80808080);
	rp_pic(1920 - 16 - 256, 540 - 8 - 128, 256, 128, 0, 0, 1, 1, 0x80808080);
	s.blend = RP_BLEND_ALPHA;
	rp_pic_state(&s);
	for (int i = 0; i < 40; i++)
		rp_pic(300.0f + 32.0f * (float)i, 180.0f + 4.0f * (float)i, 24, 12, 0, 0, 0.25f, 0.25f, 0x40808080u + ((u32)i << 25));
	rp_poly_flush();
}

static void frame_draw(const struct rp_cam *c, struct fstats *f) {
	struct rp_world_stats w;
	struct rp_list_stats l0 = rp_list_stats;
	rp_list_begin();
	rp_view_set(c);
	rp_world_cull(&w);
	rp_world_add(0);
	rp_world_add(1);
	if (rp_debug & 8192)
		test_draws(c);
	rp_list_end();
	if (f)
		*f = (struct fstats){w.seen, w.groups, w.drawn, w.clip, w.back, w.verts, rp_list_stats.sent - l0.sent,
		                     rp_list_stats.batches - l0.batches, rp_list_stats.setups - l0.setups,
		                     rp_list_stats.qw - l0.qw};
}

static u32 us(u32 cycles) { return (u32)((u64)cycles * 1000 / (MX_EE_HZ / 1000)); }

int main(int argc, char **argv) {
	const char *mapname = argc > 1 ? argv[1] : "q3dm6.pmap";
	const char *camname = argc > 2 ? argv[2] : "quake3.cam";
	u32 shot = argc > 3 ? num(argv[3]) : 0, first = argc > 4 ? num(argv[4]) : 0;
	extern unsigned rp_debug;
	rp_debug = argc > 6 ? num(argv[6]) : 0;
	u32 msize = 0, csize = 0;
	const void *map = mx_vm_map_file(mapname, &msize);
	const u32 *cf = mx_vm_map_file(camname, &csize);
	if (!map || !cf || csize < 16 || cf[0] != 0x4d414351 || cf[3] != sizeof(struct cam)) {
		mx_printf("q3view: %s or %s is not there (or not what it should be)\n", mapname, camname);
		MX_TEST_END(1);
		return 1;
	}
	const struct cam *cams = (const void *)(cf + 4);
	u32 ncam = cf[2], count = argc > 5 ? num(argv[5]) : ncam;
	if (first >= ncam)
		first = 0;
	if (count > ncam - first)
		count = ncam - first;
	int lk = 0;
	/* a seventh argument: the strips' lines (192: three strips; 64: nine; 0: three strips
	   without records of their own, the whole frame in every pass) */
	u32 lines = argc > 7 ? num(argv[7]) : 192;
	mx_hires_strip_lines(lines);
	if (mx_hires_init(&mx_video_1080i, 1920, 540, MX_GS_PSM_CT16S, rec, sizeof rec, (void *)0x70002000, 0x2000) < 0) {
		mx_printf("q3view: no 1080i, or its buffer does not fit\n");
		MX_TEST_END(1);
		return 1;
	}
	u32 sbytes = lines ? mx_hires_strip_records(srec, sizeof srec) : 0;
	rp_tex_init();
	rp_list_init();
	if (rp_world_load(map, msize) < 0) {
		mx_printf("q3view: %s is not a map of this version\n", mapname);
		MX_TEST_END(1);
		return 1;
	}
	mx_printf("q3view: %s %u bytes (lock %d), %u cameras, %u strips of %u lines, their own records %u bytes\n", mapname,
	          msize, lk, ncam, mx_hires_passes(), mx_hires_strip_height(), sbytes);
	/* the first camera's textures into GS memory before the flight is timed */
	for (int k = 0; k < 2 && count; k++) {
		struct rp_cam c;
		const struct cam *cm = &cams[first];
		for (int a = 0; a < 3; a++) {
			c.org[a] = cm->org[a];
			for (int j = 0; j < 3; j++)
				c.axis[a][j] = cm->axis[a][j];
		}
		c.fov_x = cm->fov_x, c.fov_y = cm->fov_y;
		mx_frame_begin_n();
		rp_tex_frame();
		mx_hires_clear(0, 0, 0);
		frame_draw(&c, 0);
		mx_frame_end();
	}
	for (u32 v = mx_gs.vsync; mx_gs.vsync - v < 6;)
		mx_sleep_us(2000);
	mx_hires_stats = (struct mx_hires_stats){0};
	rp_tex_stats = (struct rp_tex_stats){0};

	u64 build = 0, wait = 0;
	u32 build_max = 0, over = 0;
	/* each strip's pass, a frame (a frame is a field here, so the longest
	   since the frame before is that field's): its sum, how many took more
	   than their 5.5 ms, and the fields whose three together took more than
	   12 and 16 ms */
	u32 pass_long[MX_HIRES_PASSES] = {0}, field_12 = 0, field_16 = 0, field_max = 0;
#define TOP 8
	struct {
		u32 us, cam;
		struct fstats st;
	} top[TOP] = {{0}};
	struct fstats prev_st = {0};
#define MAXROW 1300
	static struct rowt {
		u32 sent, batches, setups, chunks, clip, us;
	} row[MAXROW];
	u32 nrow = 0;
	struct fstats st, sum = {0}, max = {0};
	u32 t_start = mx_gs.vsync, shot_at = ~0u;
	/* rp_debug 4096: where the EE's time goes */
	if (rp_debug & 4096)
		mx_prof_start(4000);
	for (u32 i = 0; i < count; i++) {
		const struct cam *cm = &cams[first + i];
		struct rp_cam c;
		for (int k = 0; k < 3; k++) {
			c.org[k] = cm->org[k];
			for (int j = 0; j < 3; j++)
				c.axis[k][j] = cm->axis[k][j];
		}
		c.fov_x = cm->fov_x, c.fov_y = cm->fov_y;
		u32 t0 = mx_count();
		mx_frame_begin_n();
		u32 t1 = mx_count();
		rp_tex_frame();
		if (rp_debug & 128)
			mx_hires_clear(0, 0, 0);
		else
			mx_hires_clear(0x30, 0x40, 0x60);
		frame_draw(&c, &st);
		mx_frame_end();
		u32 t2 = mx_count() - t1;
		wait += t1 - t0, build += t2;
		build_max = t2 > build_max ? t2 : build_max;
		over += t2 > 4920116u;
		u32 f = 0;
		for (u32 k = 0; k < mx_hires_passes(); k++) {
			u32 p = mx_hires_stats.pass_us[k];
			mx_hires_stats.pass_us[k] = 0;
			pass_long[k] = p > pass_long[k] ? p : pass_long[k], f += p;
		}
		field_12 += f > 12000, field_16 += f > 16000, field_max = f > field_max ? f : field_max;
		/* the heaviest fields: the passes' times read here are of the frame
		   before this one */
		if (i) {
			int at = -1;
			for (int k = 0; k < TOP; k++)
				if (f > top[k].us && (at < 0 || top[k].us < top[at].us))
					at = k;
			if (at >= 0)
				top[at].us = f, top[at].cam = (u32)cams[first + i - 1].frame, top[at].st = prev_st;
		}
		/* rp_debug 65536: each field's counts and the time of its passes, printed
		   at the end (q3row lines) for a fit on the PC */
		if (i && nrow < MAXROW)
			row[nrow++] = (struct rowt){prev_st.sent, prev_st.batches, prev_st.setups, prev_st.drawn, prev_st.clip, f};
		prev_st = st;
#define ACC(f) sum.f += st.f, max.f = st.f > max.f ? st.f : max.f
		ACC(seen), ACC(groups), ACC(drawn), ACC(clip), ACC(back), ACC(verts), ACC(sent), ACC(batches), ACC(setups), ACC(qw);
		if (shot && (u32)cm->frame == shot) {
			shot_at = i;
			break;
		}
	}
	u32 n = shot_at != ~0u ? shot_at + 1 : count;
	if (rp_debug & 4096) {
		mx_prof_stop();
		mx_prof_report(30);
	}
	/* the last record's passes */
	for (u32 v = mx_gs.vsync; mx_gs.vsync - v < 4;)
		mx_sleep_us(2000);
	u32 fields = mx_gs.vsync - t_start;
	n = n ? n : 1;
	mx_printf("q3view: %u frames in %u fields; the EE a frame: %u cycles (%u us), the longest %u us, %u over a field; "
	          "waiting for a record %u us a frame\n", n, fields, (u32)(build / n), us((u32)(build / n)), us(build_max),
	          over, us((u32)(wait / n)));
	mx_printf("q3view: a frame: %u groups seen from the cluster, %u in view; chunks: %u drawn (%u across a plane), %u "
	          "facing away; %u vertices, over all strips %u sent in %u batches, %u set-ups, %u quadwords of stream; at "
	          "most %u chunks, %u vertices (%u sent), %u set-ups, %u quadwords\n", sum.seen / n, sum.groups / n,
	          sum.drawn / n, sum.clip / n, sum.back / n, sum.verts / n, sum.sent / n, sum.batches / n, sum.setups / n,
	          sum.qw / n, max.drawn, max.verts, max.sent, max.setups, max.qw);
	mx_printf("q3view: textures sent to GS memory in the flight: %u (%u KB), %u found no room; draws left out for want "
	          "of memory: %u\n", rp_tex_stats.uploads, rp_tex_stats.upload_kb, rp_tex_stats.full, rp_list_stats.lost);
	mx_printf("q3view: beam: fields %u, passes %u, left out %u, still drawn when the beam came %u, records shown %u, "
	          "held %u; largest record %u KB, a strip's %u KB, %u cut off\n", mx_hires_stats.fields,
	          mx_hires_stats.passes, mx_hires_stats.late, mx_hires_stats.over, mx_hires_stats.shown, mx_hires_stats.held,
	          mx_hires_stats.rec_max / 1024, mx_hires_stats.strip_max / 1024, mx_hires_stats.rec_full);
	u32 all_us = 0, all_n = 0;
	mx_printf("q3view: a strip's pass, us, mean (the longest):");
	for (u32 k = 0; k < mx_hires_passes(); k++) {
		u32 pn = mx_hires_stats.pass_n[k];
		mx_printf(" %u (%u)", pn ? mx_hires_stats.pass_sum_us[k] / pn : 0, pass_long[k]);
		all_us += mx_hires_stats.pass_sum_us[k], all_n += pn;
	}
	mx_printf("; a field's passes together %u us, the longest about %u, %u fields over 12 ms, %u over 16 ms\n",
	          mx_hires_stats.fields ? all_us / mx_hires_stats.fields : 0, field_max, field_12, field_16);
	for (int k = 0; k < TOP; k++)
		if (top[k].us)
			mx_printf("q3view: a heavy field: camera %u: passes %u us; %u chunks (%u across a plane), %u vertices, %u "
			          "sent in %u batches, %u set-ups\n", top[k].cam, top[k].us, top[k].st.drawn, top[k].st.clip,
			          top[k].st.verts, top[k].st.sent, top[k].st.batches, top[k].st.setups);
	if (rp_debug & 65536)
		for (u32 k = 0; k < nrow; k++)
			mx_printf("q3row,%u,%u,%u,%u,%u,%u\n", row[k].sent, row[k].batches, row[k].setups, row[k].chunks, row[k].clip,
			          row[k].us);
	int r = 0;
	if (shot) {
		r = mx_hires_save_tga("q3view.tga");
		mx_printf("q3view: picture of camera %u: %d\n", shot, r);
	}
	MX_TEST_END(r ? 1 : 0);
	return 0;
}
