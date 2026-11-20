/* The load of the PS2's units over a timedemo (ps2_load <hz>): a timer's
   interrupt samples what the EE runs (the game, the renderer, the kernel,
   or nothing: the kernel's idle loop) and what the drawing units do (VIF1's
   DMA: a pass of beam racing is being sent; VU1 running, or waiting at an
   XGKICK for the GIF; the GS's input FIFO not empty; VU0's micro program),
   each with the beam's line (maxsdk's passes' clock). At the end of the run
   quake3.load: the samples counted by the place in the field, and the
   samples themselves of a second from the demo's frame ps2_loadraw, for
   tools/ps2load.py to read. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mx/hires.h>
#include <mx/mx.h>
#include <mx/thread.h>
#include <mx/timer.h>
#include <mx/trace.h>
#include <mx/vu.h>

#define LINES 16 /* the beam's lines of a bucket */
#define NPH 80
#define NRAW 16384
enum { C_GAME, C_REN, C_KGAME, C_KREN, C_IDLE_GAME, C_IDLE_REN, C_N };

/* 1 while the renderer's calls run (rp_main.c): the rest of the frame is the game's */
volatile unsigned char ps2_load_state MX_IRQ_DATA = 0;

static struct {
	u32 ph_ee[NPH][C_N];
	u32 ph_u[NPH][16]; /* by VIF1's DMA, VU1 running, VU1 at an XGKICK, the GS's FIFO */
	u32 ee_u[C_N][16];
	u32 unit[MX_TS_N];
	u32 vif[16]; /* VIF1's STAT (VPS, VEW, VGW) while its DMA runs */
	u32 n;
	u32 idle_lo, idle_hi; /* the kernel's idle loop */
	u32 kpc[64][2];       /* the kernel's other places: PC >> 4, samples */
	u32 raw_n, raw_on, frame;
	u32 cal[64][2], cal_on, nop;
} ld MX_IRQ_BSS;
static u32 raw[NRAW] MX_IRQ_BSS;
static int timer = -1;
static u32 period_us;

static MX_IRQ_FN int is_kernel(u32 pc) { return pc >= 0x80000000u || (pc >= 0x01F00000u && pc < 0x02000000u); }

static MX_IRQ_FN void count_pc(u32 t[64][2], u32 pc) {
	u32 key = (pc >> 4) + 1, hh = (key * 2654435761u) >> 26;
	for (u32 i = 0; i < 8; i++) {
		u32 at = (hh + i) & 63;
		if (t[at][0] == key || !t[at][0]) {
			t[at][0] = key, t[at][1]++;
			return;
		}
	}
}

static MX_IRQ_FN void sample(void *arg) {
	(void)arg;
	if (ld.nop) /* ps2_load < 0: the timer's calls alone (what a call costs) */
		return;
	u32 pc = mx_timer_epc();
	if (ld.cal_on) {
		if (is_kernel(pc))
			count_pc(ld.cal, pc);
		return;
	}
	u32 ren = ps2_load_state ? 1 : 0, cls;
	if (pc >= ld.idle_lo && pc < ld.idle_hi)
		cls = C_IDLE_GAME + ren;
	else if (is_kernel(pc)) {
		cls = C_KGAME + ren;
		count_pc(ld.kpc, pc);
	} else
		cls = C_GAME + ren;
	u32 un = mx_trace_units();
	u32 u = (un & MX_TS_VIF1 ? 1 : 0) | (un & MX_TS_VU1 ? 2 : 0) | (un & MX_TS_VU1GW ? 4 : 0) | (un & MX_TS_GSFIFO ? 8 : 0);
	u32 vif = MX_R32(0x10003C00) & 15;
	u32 line = mx_hires_line(), ph = line / LINES;
	if (ph >= NPH)
		ph = NPH - 1;
	ld.n++;
	ld.ph_ee[ph][cls]++, ld.ph_u[ph][u]++, ld.ee_u[cls][u]++;
	for (u32 i = 0; i < MX_TS_N; i++)
		if (un & (1u << i))
			ld.unit[i]++;
	if (un & MX_TS_VIF1)
		ld.vif[vif]++;
	if (ld.raw_on && ld.raw_n < NRAW)
		raw[ld.raw_n++] = (line > 2047 ? 2047 : line) | cls << 11 | (un & 1023) << 14 | vif << 24 | (ld.frame & 15) << 28;
}

/* hz samples a second from now on; first the kernel's idle loop is found:
   where the kernel is while this thread sleeps */
void ps2_load_start(unsigned hz) {
	if (timer >= 0 || !hz)
		return;
	memset(&ld, 0, sizeof ld);
	if ((int)hz < 0) {
		ld.nop = 1;
		timer = mx_timer_add(1000000u / (unsigned)-(int)hz, 1000000u / (unsigned)-(int)hz, sample, 0);
		return;
	}
	period_us = 1000000u / hz;
	ld.cal_on = 1;
	timer = mx_timer_add(197, 197, sample, 0);
	if (timer < 0)
		return;
	mx_sleep_us(60000);
	mx_timer_remove(timer);
	u32 best = 0, at = 0, all = 0;
	for (u32 i = 0; i < 64; i++) {
		all += ld.cal[i][1];
		if (ld.cal[i][1] > best)
			best = ld.cal[i][1], at = i;
	}
	u32 pc = best ? (ld.cal[at][0] - 1) << 4 : 0;
	/* the loop is a branch to itself: its bucket, and the one beside it */
	ld.idle_lo = pc ? pc - 16 : 0, ld.idle_hi = pc ? pc + 32 : 0;
	mx_printf("ps2: load: the kernel's idle loop at %08x (%u of %u samples asleep); a sample every %u us\n", pc, best, all,
	          period_us);
	ld.cal_on = 0;
	timer = mx_timer_add(period_us, period_us, sample, 0);
}

/* the demo's frame that begins; raw_at: the samples themselves from that frame */
void ps2_load_frame(unsigned demo_frame, unsigned raw_at) {
	ld.frame = demo_frame;
	if (timer >= 0 && raw_at && demo_frame == raw_at)
		ld.raw_on = 1;
}

void ps2_load_report(void) {
	if (timer < 0)
		return;
	mx_timer_remove(timer);
	timer = -1;
	if (ld.nop)
		return;
	u32 n = ld.n ? ld.n : 1, ee[C_N] = {0}, u[16] = {0};
	for (u32 p = 0; p < NPH; p++) {
		for (u32 c = 0; c < C_N; c++)
			ee[c] += ld.ph_ee[p][c];
		for (u32 c = 0; c < 16; c++)
			u[c] += ld.ph_u[p][c];
	}
	u32 vif1 = 0, vu1 = 0, gw = 0, gs = 0;
	for (u32 c = 0; c < 16; c++)
		vif1 += c & 1 ? u[c] : 0, vu1 += c & 2 ? u[c] : 0, gw += c & 4 ? u[c] : 0, gs += c & 8 ? u[c] : 0;
#define PM(x) (unsigned)((unsigned long long)(x) * 1000 / n / 10), (unsigned)((unsigned long long)(x) * 1000 / n % 10)
	mx_printf("ps2: load: %u samples. The EE: the game %u.%u %%, the renderer %u.%u %%, the kernel %u.%u %% and %u.%u %%, "
	          "idle %u.%u %% (in the game's part) and %u.%u %% (in the renderer's)\n",
	          ld.n, PM(ee[C_GAME]), PM(ee[C_REN]), PM(ee[C_KGAME]), PM(ee[C_KREN]), PM(ee[C_IDLE_GAME]), PM(ee[C_IDLE_REN]));
	mx_printf("ps2: load: a pass being sent (VIF1's DMA) %u.%u %%; VU1 running %u.%u %%, of it at an XGKICK waiting for the "
	          "GIF %u.%u %%; the GS's FIFO not empty %u.%u %%; VU0's micro program %u.%u %%; PATH3's DMA %u.%u %%\n",
	          PM(vif1), PM(vu1), PM(gw), PM(gs), PM(ld.unit[5]), PM(ld.unit[2]));
	char *text = malloc(NPH * 400 + NRAW * 9 + 8192);
	FILE *f = text ? fopen("quake3.load", "w") : NULL;
	if (!f) {
		mx_printf("ps2: load: quake3.load not written\n");
		return;
	}
	int len = sprintf(text, "load 1\nperiod_us %u\nsamples %u\nlines %u\nidle %08x\nstrips %u", period_us, ld.n, LINES,
	                  ld.idle_lo + 16, mx_hires_passes());
	for (u32 k = 0; k < mx_hires_passes(); k++)
		len += sprintf(text + len, " %u", mx_hires_strip_line(k));
	len += sprintf(text + len, "\nunits");
	for (u32 i = 0; i < MX_TS_N; i++)
		len += sprintf(text + len, " %u", ld.unit[i]);
	len += sprintf(text + len, "\nvif");
	for (u32 i = 0; i < 16; i++)
		len += sprintf(text + len, " %u", ld.vif[i]);
	len += sprintf(text + len, "\n");
	for (u32 c = 0; c < C_N; c++) {
		len += sprintf(text + len, "eeu %u", c);
		for (u32 i = 0; i < 16; i++)
			len += sprintf(text + len, " %u", ld.ee_u[c][i]);
		len += sprintf(text + len, "\n");
	}
	for (u32 p = 0; p < NPH; p++) {
		len += sprintf(text + len, "ph %u", p);
		for (u32 c = 0; c < C_N; c++)
			len += sprintf(text + len, " %u", ld.ph_ee[p][c]);
		len += sprintf(text + len, " :");
		for (u32 c = 0; c < 16; c++)
			len += sprintf(text + len, " %u", ld.ph_u[p][c]);
		len += sprintf(text + len, "\n");
	}
	for (u32 i = 0; i < 64; i++)
		if (ld.kpc[i][0])
			len += sprintf(text + len, "kpc %08x %u\n", (ld.kpc[i][0] - 1) << 4, ld.kpc[i][1]);
	len += sprintf(text + len, "raw %u\n", ld.raw_n);
	for (u32 i = 0; i < ld.raw_n; i++)
		len += sprintf(text + len, "%08x%c", raw[i], i % 16 == 15 ? '\n' : ' ');
	len += sprintf(text + len, "\nend\n");
	fwrite(text, 1, len, f);
	fclose(f);
	free(text);
	mx_printf("ps2: load: quake3.load written (%d bytes, %u samples of the frames themselves)\n", len, ld.raw_n);
}
