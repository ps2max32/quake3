/* q3view: flies a recorded camera through a map with the PS2 renderer's world
   (code/renderer_ps2/rp_world.c) in 1080i beam racing, and says what a frame
   costs: the EE's cycles building it, the chunks and vertices sent, and beam
   racing's passes (PLAN-PS2.md, phase 1).

     q3view.elf [map.pmap [cameras.cam [shot [first [count]]]]]

   map.pmap: tools/ps2map.py's (default q3dm6.pmap); cameras.cam: quake3.cam, as
   a REN=ps2 build of the game writes it during a timedemo; shot: the camera
   whose picture is saved as q3view.tga when the flight is over (0: none);
   first, count: the cameras flown (default all). Each camera is one frame.
   A sixth argument: rp_world.c's rp_debug. */

#include <mx/hires.h>
#include <mx/mx.h>
#include <mx/vm.h>

#include "rp_world.h"

struct cam {
	int frame, time;
	float org[3], axis[3][3], fov_x, fov_y;
	int x, y, w, h;
	unsigned ents, polys, polyverts, lights;
};

static u8 rec[4 * 128 * 1024] MX_CORE_BSS MX_ALIGNED(64);

static u32 num(const char *s) {
	u32 v = 0;
	for (; *s >= '0' && *s <= '9'; s++)
		v = v * 10 + (u32)(*s - '0');
	return v;
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
	/* the DMAC reads the map where it lies */
	int lk = mx_vm_lock(map, msize);
	if (mx_hires_init(&mx_video_1080i, 1920, 540, MX_GS_PSM_CT16S, rec, sizeof rec, (void *)0x70002000, 0x2000) < 0) {
		mx_printf("q3view: no 1080i, or its buffer does not fit\n");
		MX_TEST_END(1);
		return 1;
	}
	if (rp_world_load(map, msize) < 0) {
		mx_printf("q3view: %s is not a map of this version\n", mapname);
		MX_TEST_END(1);
		return 1;
	}
	mx_printf("q3view: %s %u bytes (lock %d), %u cameras, %u strips of beam racing\n", mapname, msize, lk, ncam,
	          mx_hires_passes());
	/* the textures, over a few frames */
	int left;
	do {
		mx_frame_begin_n();
		mx_hires_clear(0, 0, 0);
		left = rp_world_upload();
		mx_frame_end();
	} while (left);
	for (u32 v = mx_gs.vsync; mx_gs.vsync - v < 6;)
		;
	mx_hires_stats = (struct mx_hires_stats){0};

	u64 build = 0, wait = 0;
	u32 build_max = 0, over = 0;
	/* each strip's pass, a frame (a frame is a field here, so the longest
	   since the frame before is that field's): its sum, how many took more
	   than their 5.5 ms, and the fields whose three together took more than
	   12 and 16 ms */
	u64 pass_sum[MX_HIRES_PASSES] = {0};
	u32 pass_over[MX_HIRES_PASSES] = {0}, field_sum = 0, field_12 = 0, field_16 = 0, field_max = 0;
	struct rp_world_stats st, sum = {0}, max = {0};
	u32 t_start = mx_gs.vsync, shot_at = ~0u;
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
		if (rp_debug & 128)
			mx_hires_clear(0, 0, 0);
		else
			mx_hires_clear(0x30, 0x40, 0x60);
		rp_frame_setup();
		rp_world_draw(&c, &st);
		mx_frame_end();
		u32 t2 = mx_count() - t1;
		wait += t1 - t0, build += t2;
		build_max = t2 > build_max ? t2 : build_max;
		over += t2 > 4920116u;
		u32 f = 0;
		for (u32 k = 0; k < mx_hires_passes(); k++) {
			u32 p = mx_hires_stats.pass_us[k];
			mx_hires_stats.pass_us[k] = 0;
			pass_sum[k] += p, pass_over[k] += p > 5500, f += p;
		}
		field_sum += f, field_12 += f > 12000, field_16 += f > 16000, field_max = f > field_max ? f : field_max;
#define ACC(f) sum.f += st.f, max.f = st.f > max.f ? st.f : max.f
		ACC(seen), ACC(groups), ACC(drawn), ACC(clip), ACC(back), ACC(verts), ACC(batches), ACC(setups), ACC(qw);
		if (shot && (u32)cm->frame == shot) {
			shot_at = i;
			break;
		}
	}
	u32 n = shot_at != ~0u ? shot_at + 1 : count;
	/* the last record's passes */
	for (u32 v = mx_gs.vsync; mx_gs.vsync - v < 4;)
		;
	u32 fields = mx_gs.vsync - t_start;
	n = n ? n : 1;
	mx_printf("q3view: %u frames in %u fields; the EE a frame: %u cycles (%u us), the longest %u us, %u over a field; "
	          "waiting for a record %u us a frame\n", n, fields, (u32)(build / n), us((u32)(build / n)), us(build_max),
	          over, us((u32)(wait / n)));
	mx_printf("q3view: a frame: %u groups seen from the cluster, %u in view; chunks: %u drawn (%u across a plane), %u "
	          "facing away; %u vertices in %u batches, %u set-ups, %u quadwords of stream; at most %u chunks, %u "
	          "vertices, %u set-ups, %u quadwords\n", sum.seen / n, sum.groups / n, sum.drawn / n, sum.clip / n,
	          sum.back / n, sum.verts / n, sum.batches / n, sum.setups / n, sum.qw / n, max.drawn, max.verts, max.setups,
	          max.qw);
	mx_printf("q3view: beam: fields %u, passes %u, late %u, records shown %u, held %u; largest record %u KB, %u cut off\n",
	          mx_hires_stats.fields, mx_hires_stats.passes, mx_hires_stats.late, mx_hires_stats.shown,
	          mx_hires_stats.held, mx_hires_stats.rec_max / 1024, mx_hires_stats.rec_full);
	mx_printf("q3view: a strip's pass, us (over 5500):");
	for (u32 k = 0; k < mx_hires_passes(); k++)
		mx_printf(" %u (%u)", (u32)(pass_sum[k] / n), pass_over[k]);
	mx_printf("; a field's passes together %u us, the longest %u, %u over 12 ms, %u over 16 ms\n", field_sum / n,
	          field_max, field_12, field_16);
	int r = 0;
	if (shot) {
		r = mx_hires_save_tga("q3view.tga");
		mx_printf("q3view: picture of camera %u: %d\n", shot, r);
	}
	MX_TEST_END(r ? 1 : 0);
	return 0;
}
