/* sys_ps2.c: the system layer of the engine on maxsdk: main, time, files
   (host: through ps2_file.c), the game modules linked in, the pad, and the
   network and sound stubs (loopback only, no sound device yet). */

#include <stdlib.h>
#include <string.h>

#include <mx/mx.h>
#include <mx/hires.h>
#include <mx/mem.h>
#include <mx/pad.h>
#include <mx/vm.h>

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/keycodes.h"
#include <mx/prof.h>

void mx_exit(int status) __attribute__((noreturn));
int ps2_listdir(const char *dir, void (*fn)(const char *name, int is_dir, void *ctx), void *ctx);
/* the renderer's (ps2_glimp.c for renderergl1, renderer_ps2/rp_main.c): a
   picture of the screen as a TGA file, and its counts at the end of a run */
int ps2_ren_screenshot(const char *path);
void ps2_ren_report(void);
int ps2_demo_frame(void);
void ps2_load_start(unsigned hz);
void ps2_load_frame(unsigned demo_frame, unsigned raw_at);
void ps2_load_report(void);
static void ps2_run_report(void);

/* frames to run before the program ends by itself (0: never), and the
   picture taken then (PS2_SHOT); set by the Makefile */
#include "runcfg.h"
#ifndef PS2_FRAMES
#define PS2_FRAMES 0
#endif
#ifndef PS2_SHOT
#define PS2_SHOT "quake3.tga"
#endif
#ifndef PS2_ARGS
#define PS2_ARGS ""
#endif
#ifndef PS2_AUTOQUIT
#define PS2_AUTOQUIT 0
#endif
#ifndef PS2_PERF
#define PS2_PERF 0
#endif

/* ---- time ----------------------------------------------------------------- */

static u64 cycles(void) {
	static u32 last, high;
	u32 c = mx_count();
	if (c < last)
		high++;
	last = c;
	return (u64)high << 32 | c;
}

int Sys_Milliseconds(void) {
	static u64 base;
	static int init;
	u64 c = cycles();
	if (!init) {
		base = c;
		init = 1;
	}
	/* a millisecond is 294,912 cycles: 2^15 * 9 (no 64-bit division: this is
	   asked hundreds of times a frame) */
	_Static_assert(MX_EE_HZ / 1000 == 9 << 15, "the EE's clock");
	return (int)((u32)((c - base) >> 15) / 9u);
}

void Sys_Sleep(int msec) {
	if (msec <= 0)
		return;
	mx_sleep_us((u32)msec * 1000u);
}

/* ---- the game modules -------------------------------------------------------- */

typedef intptr_t (QDECL *syscall_t)(intptr_t, ...);
#define MODULE(m)                                                                  \
	intptr_t m##_vmMain(int command, int arg0, int arg1, int arg2, int arg3, int arg4, \
	                    int arg5, int arg6, int arg7, int arg8, int arg9, int arg10,   \
	                    int arg11);                                                    \
	void m##_dllEntry(syscall_t syscallptr);
MODULE(qagame)
MODULE(cgame)
MODULE(ui)

void *QDECL Sys_LoadGameDll(const char *name, vmMainProc *entryPoint,
                            intptr_t (QDECL *systemcalls)(intptr_t, ...)) {
	const char *base = strrchr(name, '/');
	base = base ? base + 1 : name;
	if (!Q_stricmpn(base, "qagame", 6)) {
		qagame_dllEntry(systemcalls);
		*entryPoint = (vmMainProc)qagame_vmMain;
		return (void *)1;
	}
	if (!Q_stricmpn(base, "cgame", 5)) {
		cgame_dllEntry(systemcalls);
		*entryPoint = (vmMainProc)cgame_vmMain;
		return (void *)2;
	}
	if (!Q_stricmpn(base, "ui", 2)) {
		ui_dllEntry(systemcalls);
		*entryPoint = (vmMainProc)ui_vmMain;
		return (void *)3;
	}
	return NULL;
}

void Sys_UnloadDll(void *dllHandle) {}
void *Sys_LoadDll(const char *name, qboolean useSystemLib) { return NULL; }
qboolean Sys_DllExtension(const char *name) { return qfalse; }

/* ---- the console and errors ------------------------------------------------- */

void Sys_Print(const char *msg) {
	/* without the colour codes */
	char line[1024];
	int n = 0;
	for (const char *p = msg; *p; p++) {
		if (Q_IsColorString(p)) {
			p++;
			continue;
		}
		line[n++] = *p;
		if (n == sizeof line - 1 || *p == '\n') {
			line[n] = 0;
			mx_printf("%s", line);
			n = 0;
		}
	}
	if (n) {
		line[n] = 0;
		mx_printf("%s", line);
	}
}

void QDECL Sys_Error(const char *error, ...) {
	va_list ap;
	char text[4096];
	va_start(ap, error);
	Q_vsnprintf(text, sizeof text, error, ap);
	va_end(ap);
	mx_printf("Sys_Error: %s\n", text);
	mx_exit(1);
}

void Sys_Quit(void) {
	mx_printf("Sys_Quit\n");
	ps2_run_report();
	mx_exit(0);
}

void Sys_SetErrorText(const char *text) {}
char *Sys_ConsoleInput(void) { return NULL; }
char *Sys_GetClipboardData(void) { return NULL; }
void Sys_DisplaySystemConsole(qboolean show) {}
dialogResult_t Sys_Dialog(dialogType_t type, const char *message, const char *title) {
	mx_printf("%s: %s\n", title, message);
	return DR_OK;
}
void CON_Init(void) {}
void CON_Shutdown(void) {}

/* ---- the system ------------------------------------------------------------- */

void Sys_Init(void) {
	Cvar_Set("arch", OS_STRING " " ARCH_STRING);
	Cvar_Set("username", "player");
}

char *Sys_GetCurrentUser(void) { return "player"; }
qboolean Sys_RandomBytes(byte *string, int len) { return qfalse; }
cpuFeatures_t Sys_GetProcessorFeatures(void) { return (cpuFeatures_t)0; }
qboolean Sys_LowPhysicalMemory(void) { return qtrue; }
void Sys_SetEnv(const char *name, const char *value) {}
qboolean Sys_OpenFolderInFileManager(const char *path, qboolean create) { return qfalse; }
void Sys_RemovePIDFile(const char *gamedir) {}
void Sys_InitPIDFile(const char *gamedir) {}
void Sys_GLimpSafeInit(void) {}
void Sys_GLimpInit(void) {}

/* ---- paths and files -------------------------------------------------------- */

static char install_path[MAX_OSPATH] = ".";
void Sys_SetDefaultInstallPath(const char *path) { Q_strncpyz(install_path, path, sizeof install_path); }
char *Sys_DefaultInstallPath(void) { return install_path; }
char *Sys_DefaultAppPath(void) { return install_path; }
char *Sys_SteamPath(void) { return ""; }
char *Sys_GogPath(void) { return ""; }
char *Sys_MicrosoftStorePath(void) { return ""; }
char *Sys_DefaultHomeConfigPath(void) { return ""; }
char *Sys_DefaultHomeDataPath(void) { return ""; }
char *Sys_DefaultHomeStatePath(void) { return ""; }
char *Sys_Cwd(void) { return install_path; }

FILE *Sys_FOpen(const char *ospath, const char *mode) { return fopen(ospath, mode); }
qboolean Sys_Mkdir(const char *path) { return qtrue; }
FILE *Sys_Mkfifo(const char *ospath) { return NULL; }

const char *Sys_Dirname(char *path) {
	static char dir[MAX_OSPATH];
	Q_strncpyz(dir, path, sizeof dir);
	char *s = strrchr(dir, '/');
	if (s)
		*s = 0;
	return dir;
}

const char *Sys_Basename(char *path) {
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

#define MAX_FOUND_FILES 0x1000

struct list_ctx {
	const char *extension;
	char *filter;
	qboolean wantsubs, dironly;
	char *list[MAX_FOUND_FILES];
	int n;
};

static void list_one(const char *name, int is_dir, void *vctx) {
	struct list_ctx *c = vctx;
	if (c->n >= MAX_FOUND_FILES - 1 || !strcmp(name, ".") || !strcmp(name, ".."))
		return;
	if (c->filter) {
		if (!Com_FilterPath(c->filter, (char *)name, qfalse))
			return;
	} else {
		if (c->dironly && !is_dir)
			return;
		if (!c->dironly && is_dir && !c->wantsubs)
			return;
		if (*c->extension && !c->dironly) {
			size_t ln = strlen(name), le = strlen(c->extension);
			if (ln < le || Q_stricmp(name + ln - le, c->extension))
				return;
		}
	}
	c->list[c->n++] = CopyString(name);
}

char **Sys_ListFiles(const char *directory, const char *extension, char *filter, int *numfiles,
                     qboolean wantsubs) {
	static struct list_ctx c;
	memset(&c, 0, sizeof c);
	if (!extension)
		extension = "";
	c.extension = extension;
	c.filter = filter;
	c.wantsubs = wantsubs;
	if (extension[0] == '/' && extension[1] == 0) {
		c.dironly = qtrue;
		c.extension = "";
	}
	ps2_listdir(directory, list_one, &c);
	*numfiles = c.n;
	if (!c.n)
		return NULL;
	char **list = Z_Malloc((c.n + 1) * sizeof(char *));
	for (int i = 0; i < c.n; i++)
		list[i] = c.list[i];
	list[c.n] = NULL;
	return list;
}

void Sys_FreeFileList(char **list) {
	if (!list)
		return;
	for (int i = 0; list[i]; i++)
		Z_Free(list[i]);
	Z_Free(list);
}

/* ---- network: loopback only ----------------------------------------------- */

void NET_Init(void) {}
void NET_Shutdown(void) {}
void NET_Restart_f(void) {}
void NET_Config(qboolean enableNetworking) {}
void NET_Sleep(int msec) {}
void NET_JoinMulticast6(void) {}
void NET_LeaveMulticast6(void) {}
void Sys_SendPacket(int length, const void *data, netadr_t to) {}
qboolean Sys_StringToAdr(const char *s, netadr_t *a, netadrtype_t family) { return qfalse; }
qboolean Sys_IsLANAddress(netadr_t adr) { return adr.type == NA_LOOPBACK; }
void Sys_ShowIP(void) {}

qboolean NET_CompareBaseAdrMask(netadr_t a, netadr_t b, int netmask) {
	if (a.type != b.type)
		return qfalse;
	if (a.type == NA_LOOPBACK || a.type == NA_BOT)
		return qtrue;
	return !memcmp(a.ip, b.ip, 4);
}
qboolean NET_CompareBaseAdr(netadr_t a, netadr_t b) { return NET_CompareBaseAdrMask(a, b, -1); }
qboolean NET_CompareAdr(netadr_t a, netadr_t b) {
	return NET_CompareBaseAdr(a, b) && (a.type == NA_LOOPBACK || a.type == NA_BOT || a.port == b.port);
}
qboolean NET_IsLocalAddress(netadr_t adr) { return adr.type == NA_LOOPBACK; }
const char *NET_AdrToString(netadr_t a) {
	static char s[64];
	if (a.type == NA_LOOPBACK)
		return "loopback";
	if (a.type == NA_BOT)
		return "bot";
	Com_sprintf(s, sizeof s, "%i.%i.%i.%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3]);
	return s;
}
const char *NET_AdrToStringwPort(netadr_t a) { return NET_AdrToString(a); }

/* ---- the pad ---------------------------------------------------------------- */

static int pad_ok;
static const struct {
	u32 bit;
	int key;
} pad_keys[] = {
	{MX_PAD_UP, K_UPARROW},     {MX_PAD_DOWN, K_DOWNARROW}, {MX_PAD_LEFT, K_LEFTARROW},
	{MX_PAD_RIGHT, K_RIGHTARROW}, {MX_PAD_CROSS, K_JOY1},   {MX_PAD_CIRCLE, K_JOY2},
	{MX_PAD_SQUARE, K_JOY3},    {MX_PAD_TRIANGLE, K_JOY4},  {MX_PAD_R1, K_MOUSE1},
	{MX_PAD_R2, K_JOY5},        {MX_PAD_L1, K_JOY6},        {MX_PAD_L2, K_JOY7},
	{MX_PAD_START, K_ESCAPE},   {MX_PAD_SELECT, K_JOY8},
};

void IN_Init(void *windowData) {
	pad_ok = mx_pad_init() >= 0;
	Com_Printf("pad: %s\n", pad_ok ? "ok" : "no driver");
}

void IN_Frame(void) {
	if (!pad_ok)
		return;
	static u32 last;
	u32 held = mx_pad_buttons(0);
	int t = Sys_Milliseconds();
	for (unsigned i = 0; i < ARRAY_LEN(pad_keys); i++) {
		u32 b = pad_keys[i].bit;
		if ((held ^ last) & b)
			Com_QueueEvent(t, SE_KEY, pad_keys[i].key, (held & b) != 0, 0, NULL);
	}
	last = held;
	/* the sticks: axes 0, 1 left (side, forward), 2, 3 right (yaw, pitch), in
	   half SDL's range (-16383 .. 16383: j_yaw -0.012 is 215 degrees a second
	   at full turn, j_pitch 0.008 145), shaped as a PS4 or PS5 game does: a
	   round dead zone (j_deadzone, of the stick's reach) around the stick's
	   own middle, and on the right stick a curve (j_curve: the reach to that
	   power) and the Controls menu's Mouse Speed (sensitivity: its default 5
	   is the speeds above, 10 twice them). The move (j_forward, j_side) is
	   clamped to 127 by the game: they are set so that full reach is 127
	   (q3's 0.25 reached it at 3% of the reach: a stick a little off
	   straight walked at 45 degrees). maxsdk moves each stick's middle
	   (where it rests) to 128 (mx_pad_sticks). */
	u8 st[4];
	static int axis[4];
	if (mx_pad_sticks(0, st)) {
		static const int map[4] = {2, 3, 0, 1}; /* st: right x, y, left x, y */
		static cvar_t *dz, *curve, *sens;
		if (!dz) {
			dz = Cvar_Get("j_deadzone", "0.22", CVAR_ARCHIVE);
			curve = Cvar_Get("j_curve", "2", CVAR_ARCHIVE);
			sens = Cvar_Get("sensitivity", "5", CVAR_ARCHIVE);
		}
		float f[4];
		for (int k = 0; k < 4; k++) {
			f[k] = ((int)st[k] - 128) / 127.0f;
			f[k] = f[k] < -1 ? -1 : f[k] > 1 ? 1 : f[k];
		}
		for (int s = 0; s < 2; s++) {
			float *x = &f[2 * s], *y = &f[2 * s + 1];
			float m = sqrtf(*x * *x + *y * *y), d = dz->value;
			float r = m <= d || m <= 0 ? 0 : (m - d) / (1 - d);
			if (r > 1)
				r = 1;
			if (s == 0)
				r = powf(r, curve->value) * (sens->value > 0 ? sens->value / 5.0f : 1);
			/* the left stick (the move): the game's speed is its largest
			   component (PM_CmdScale), so the circle is stretched to the
			   square, the direction kept: full reach is full speed in every
			   direction */
			float a = fabsf(*x) > fabsf(*y) ? fabsf(*x) : fabsf(*y);
			float n = s == 1 ? a : m;
			*x = n > 0 ? *x * r / n : 0;
			*y = n > 0 ? *y * r / n : 0;
		}
		for (int k = 0; k < 4; k++) {
			int v = (int)(f[k] * 16383);
			if (v != axis[k]) {
				axis[k] = v;
				Com_QueueEvent(t, SE_JOYSTICK_AXIS, map[k], v, 0, NULL);
			}
		}
	}
}

void IN_Shutdown(void) {}
void IN_Restart(void) {}

/* ---- main --------------------------------------------------------------------- */

/* maxsdk's virtual memory (<mx/vm.h>): malloc (the hunk, the zone, the
   sounds) in up to 128 MiB, paged to a swap file next to the game's data;
   what the GS reads by DMA (libgl: arenas, textures, display lists) is
   pinned memory (<mx/mem.h>) */
#ifndef Q3_CLEAN_FRAMES
#define Q3_CLEAN_FRAMES 0 /* maxsdk's writer thread (mx_vm_config.clean_frames): off */
#endif
#ifndef Q3_VM_HIST
#define Q3_VM_HIST 0 /* 1: the timedemo's TLB refills by page (mx_vm_refill_hist): a line for each
                        region in the report, every page with its count in quake3.hist */
#endif
#ifndef Q3_VM_TRACE
#define Q3_VM_TRACE 0 /* bytes of maxsdk's VM trace (mx_vm_config.trace, taken from the
                         frames), from 100 frames before PROF to the end, saved as
                         host:quake3.trc: off */
#endif
const struct mx_vm_config mx_vm_config = {
	.swap = "quake3.swp",
	/* no core heap: libgl's pinned memory (arenas, lists, textures; beam
	   racing's records while it is on) is frames taken from the pool as
	   needed, the rest of RAM VM's pages (Quake 3 waits on the swap file) */
	.clean_frames = Q3_CLEAN_FRAMES,
	.trace = Q3_VM_TRACE,
};

/* ---- measuring a run -------------------------------------------------------- */

/* A timedemo's frame k is the demo at k * 50 ms in every build, so its
   frames compare between builds: the cycles of each frame's Com_Frame, the
   EE's performance counters (PERF=1: a pair of events a frame, by turns:
   EE Core User's Manual, table 7-1), the VM's counts and the profile
   (PROF=n) are taken over the measured frames: with FRAMES the loop's
   frames from PROF on, without it the timedemo's from its frame PROF on
   (all but the first without PROF). */
#if defined(Q3_PERF) && !PS2_PERF
#undef PS2_PERF
#define PS2_PERF 1
#endif
#define DEMO_MAX 4096
#define FIELD_60 4920116u /* cycles of a 59.94 Hz field (maxsdk's docs/gif-gs.md) */
#define FIELD_50 (MX_EE_HZ / 50)
static u32 demo_cyc[DEMO_MAX];
static int demo_first, demo_last;
static int prof_on, measuring;
static struct mx_vm_stats vm_at;
static const u8 perf_ev[3][2] = {{6, 6}, {12, 4}, {5, 5}};
static u64 perf_sum[3][2], perf_cyc[3];
static u32 perf_nf[3];

static int cmp_u32(const void *a, const void *b) {
	u32 x = *(const u32 *)a, y = *(const u32 *)b;
	return x < y ? -1 : x > y;
}

static u32 cyc_us(u32 c) { return (u32)((u64)c * 1000 / (MX_EE_HZ / 1000)); }

/* beam racing's counts */
static void ps2_beam_report(void) {
	if (!mx_hires_stats.fields)
		return;
	u32 sum = 0;
	for (u32 k = 0; k < mx_hires_passes(); k++)
		sum += mx_hires_stats.pass_sum_us[k];
	char longest[MX_HIRES_PASSES * 8];
	int len = 0;
	for (u32 k = 0; k < mx_hires_passes(); k++)
		len += sprintf(longest + len, "%s%u", k ? "/" : "", mx_hires_stats.pass_us[k]);
	mx_printf("ps2: beam: fields %u, passes %u, left out %u (%u of them ahead), still drawn when the beam came %u, records shown %u (%u from a pass within a field), "
	          "held %u; a field's passes together %u us, longest pass %s us; largest record %u KB, a strip's %u "
	          "KB, %u frames cut off (too large)\n", mx_hires_stats.fields, mx_hires_stats.passes, mx_hires_stats.late,
	          mx_hires_stats.skipped, mx_hires_stats.over, mx_hires_stats.shown, mx_hires_stats.shown_mid, mx_hires_stats.held, sum / mx_hires_stats.fields, longest,
	          mx_hires_stats.rec_max / 1024, mx_hires_stats.strip_max / 1024, mx_hires_stats.rec_full);
}

/* A run nobody watches must end and say where it was (AUTOQUIT builds): a
   thread above the game's looks once a second whether the main loop still
   comes by. A map load is one long frame, so it waits a minute; then it
   profiles the next five seconds (where the game hangs: spinning shows as
   its addresses, waiting as the kernel's idle thread), prints that and
   ends the program. It cannot help when interrupts are off. */
#include <mx/thread.h>
#include <mx/vu.h>
static volatile u32 wd_beat;
static u8 wd_stack[16384] MX_FAST_BSS MX_ALIGNED(16);
static void watchdog(void *arg) {
	u32 last = wd_beat;
	int still = 0;
	for (;;) {
		mx_sleep_us(1000000);
		if (wd_beat != last) {
			last = wd_beat, still = 0;
			continue;
		}
		if (++still == 60) {
			mx_printf("ps2: watchdog: frame %u for 60 s; where it is, the next 5 s:\n", last);
			/* what the drawing waits for: VU1 running or at an XGKICK, VIF1's
			   command and its DMA, the GIF */
			mx_printf("ps2: watchdog: VPU-STAT %08x (VU1 %s%s), VIF1 STAT %08x CODE %08x NUM %u, D1 CHCR %08x MADR %08x "
			          "TADR %08x QWC %u, GIF STAT %08x\n", mx_vpu_stat(), mx_vpu_stat() & MX_VPU_VBS1 ? "runs" : "idle",
			          mx_vpu_stat() & MX_VPU_VGW1 ? ", waits for the GIF" : "", MX_R32(0x10003C00), MX_R32(0x10003C30),
			          MX_R32(0x10003C40), MX_DMA_REG(MX_CH_VIF1, CHCR), MX_DMA_REG(MX_CH_VIF1, MADR),
			          MX_DMA_REG(MX_CH_VIF1, TADR), MX_DMA_REG(MX_CH_VIF1, QWC), MX_R32(0x10003020));
			mx_prof_stop();
			mx_prof_start(1000);
		}
		if (still == 65) {
			mx_prof_stop();
			mx_prof_report(40);
			mx_printf("ps2: watchdog: ending the run\n");
			mx_exit(3);
		}
	}
}

/* the timedemo's TLB refills by page (Q3_VM_HIST): by region, and as a file */
static struct hist_region {
	const char *name;
	u32 base, size, pages, count;
} hist_rg[12];
static u32 hist_nrg;
static void hist_region(const char *name, const void *p, unsigned bytes) {
	if (p && bytes && hist_nrg < 9)
		hist_rg[hist_nrg++] = (struct hist_region){name, (u32)(uintptr_t)p, bytes, 0, 0};
}
static void hist_report(int frames) {
	static struct mx_vm_hot hot[256];
	static char text[256 * 24];
	void Com_PS2_Regions(void (*fn)(const char *name, const void *p, unsigned bytes));
	Com_PS2_Regions(hist_region);
#ifdef PS2_REN_PS2
	void ps2_ren_regions(void (*fn)(const char *name, const void *p, unsigned bytes));
	ps2_ren_regions(hist_region);
#endif
	/* what is in none of them: by the VM's windows */
	hist_rg[hist_nrg++] = (struct hist_region){"the program's code and data", 0x08000000u, 0x08000000u, 0, 0};
	hist_rg[hist_nrg++] = (struct hist_region){"other mapped files", 0x40000000u, 0x08000000u, 0, 0};
	hist_rg[hist_nrg++] = (struct hist_region){"other heaps", 0x48000000u, 0x08000000u, 0, 0};
	FILE *f = fopen("quake3.hist", "w");
	u32 pages = 0, count = 0;
	for (;;) {
		int k = mx_vm_refill_hist(hot, 256), len = 0;
		if (k <= 0)
			break;
		for (int i = 0; i < k; i++) {
			for (u32 r = 0; r < hist_nrg; r++)
				if (hot[i].va - hist_rg[r].base < hist_rg[r].size) {
					hist_rg[r].pages++, hist_rg[r].count += hot[i].count;
					break;
				}
			pages++, count += hot[i].count;
			len += sprintf(text + len, "%08x %u\n", hot[i].va, hot[i].count);
		}
		if (f)
			fwrite(text, 1, len, f);
		if (k < 256)
			break;
	}
	if (f)
		fclose(f);
	mx_printf("hist: %u pages had %u TLB refills, %u a frame (quake3.hist: each page's)\n", pages, count,
	          count / (u32)frames);
	for (u32 r = 0; r < hist_nrg; r++)
		mx_printf("hist: %08x %5u KB %s: %u pages, %u refills, %u a frame\n", hist_rg[r].base, hist_rg[r].size / 1024,
		          hist_rg[r].name, hist_rg[r].pages, hist_rg[r].count, hist_rg[r].count / (u32)frames);
}

/* at the end of a run, however it ends (FRAMES, the quit command, AUTOQUIT) */
static void ps2_run_report(void) {
	static int done;
	static u32 sorted[DEMO_MAX];
	if (done)
		return;
	done = 1;
	ps2_load_report();
	if (prof_on) {
		mx_prof_stop();
		mx_vm_trace_mark(2);
		mx_prof_report(4000);
	}
	if (Q3_VM_TRACE)
		mx_printf("ps2: VM trace quake3.trc: %d events\n", mx_vm_trace_save("quake3.trc"));
	int n = demo_first ? demo_last - demo_first + 1 : 0;
	if (n > 1) {
		u64 sum = 0;
		int over60 = 0, over50 = 0;
		for (int i = 0; i < n; i++) {
			u32 c = demo_cyc[demo_first + i];
			sorted[i] = c, sum += c;
			over60 += c > FIELD_60, over50 += c > FIELD_50;
		}
		qsort(sorted, n, sizeof sorted[0], cmp_u32);
		u32 avg = (u32)(sum / n), fps10 = (u32)((u64)MX_EE_HZ * 10 / (avg ? avg : 1));
		mx_printf("ps2: timedemo frames %d-%d: a frame %u cycles (%u us, %u.%u fps); median %u us, 95 %% %u us, "
		          "99 %% %u us, longest %u us; %d of %d frames over a 59.94 Hz field, %d over a 50 Hz field\n",
		          demo_first, demo_last, avg, cyc_us(avg), fps10 / 10, fps10 % 10, cyc_us(sorted[n / 2]),
		          cyc_us(sorted[n * 95 / 100]), cyc_us(sorted[n * 99 / 100]), cyc_us(sorted[n - 1]), over60, n, over50);
		{
			/* the frames by the fields they took (the picture is a field's: a frame
			   takes one, or as many more as it is late), and which frames took
			   more than one, as runs of the demo's frames */
			u32 by[5] = {0, 0, 0, 0, 0};
			u64 extra = 0;
			static char runs[900];
			int len = 0, from = -1, nruns = 0;
			for (int i = 0; i <= n; i++) {
				u32 c = i < n ? demo_cyc[demo_first + i] : 0;
				u32 f = (c + FIELD_60 / 2) / FIELD_60;
				f = f < 1 ? 1 : f > 4 ? 5 : f;
				if (i < n) {
					by[f - 1]++;
					if (f > 1)
						extra += c - FIELD_60;
				}
				if (f > 1 && from < 0)
					from = i;
				if (f <= 1 && from >= 0) {
					nruns++;
					if (len < (int)sizeof runs - 24)
						len += sprintf(runs + len, i - 1 > from ? " %d-%d" : " %d", demo_first + from, demo_first + i - 1);
					from = -1;
				}
			}
			mx_printf("ps2: timedemo fields: %u frames took one field, %u two, %u three, %u four, %u more; over one field "
			          "%u us a frame in all; the frames over one, in %d runs:%s\n",
			          by[0], by[1], by[2], by[3], by[4], cyc_us((u32)(extra / n)), nruns, runs);
		}
		struct mx_vm_stats v;
		mx_vm_stats(&v);
		mx_printf("ps2: VM in those frames: a frame %u TLB refills (%u of code); in all %u swap ins (%u ms), "
		          "%u swap outs (%u ms, %u by the writer), %u pages from files (%u ms), %u pages dropped; %u ms waiting "
		          "for the VM's lock; %u of %u frames pinned\n",
		          (v.refills - vm_at.refills) / n, (v.refills_i - vm_at.refills_i) / n, v.swap_ins - vm_at.swap_ins,
		          (v.us_swap_in - vm_at.us_swap_in) / 1000, v.swap_outs - vm_at.swap_outs,
		          (v.us_swap_out - vm_at.us_swap_out) / 1000, v.writer_outs - vm_at.writer_outs, v.file_ins - vm_at.file_ins,
		          (v.us_file_in - vm_at.us_file_in) / 1000, v.drops - vm_at.drops, (v.us_lock - vm_at.us_lock) / 1000,
		          v.pinned_frames, v.frames);
		if (Q3_VM_HIST)
			hist_report(n);
		/* every frame's cycles, for the frames that are over */
		char *text = malloc(n * 24 + 64);
		FILE *f = text ? fopen("quake3.frames", "w") : NULL;
		if (f) {
			int len = 0;
			for (int i = 0; i < n; i++)
				len += sprintf(text + len, "%d %u\n", demo_first + i, demo_cyc[demo_first + i]);
			fwrite(text, 1, len, f);
			fclose(f);
		}
	}
	if (PS2_PERF && perf_nf[0] && perf_nf[1] && perf_nf[2])
		mx_printf("perf: a frame: I-cache misses %u, D-cache misses %u (%u cycles); instructions %u, "
		          "TLB misses %u (%u cycles); ITLB misses %u, DTLB misses %u\n",
		          (u32)(perf_sum[0][0] / perf_nf[0]), (u32)(perf_sum[0][1] / perf_nf[0]),
		          (u32)(perf_cyc[0] / perf_nf[0]), (u32)(perf_sum[1][0] / perf_nf[1]),
		          (u32)(perf_sum[1][1] / perf_nf[1]), (u32)(perf_cyc[1] / perf_nf[1]),
		          (u32)(perf_sum[2][0] / perf_nf[2]), (u32)(perf_sum[2][1] / perf_nf[2]));
	ps2_ren_report();
#ifdef Q3_VM_CENSUS
	{
		/* what is in memory now: the pages resident of each 512 KiB of the
		   program's VM, of the mapped files and of the heaps (for the symbols:
		   the link map) */
		static const struct {
			u32 base, size;
			const char *what;
		} rg[] = {{0x08000000u, 0x08000000u, "vm"}, {0x40000000u, 0x04000000u, "files"}, {0x48000000u, 0x04000000u, "heaps"}};
		for (unsigned r = 0; r < 3; r++) {
			u32 tot = 0;
			for (u32 a = rg[r].base; a < rg[r].base + rg[r].size; a += 0x80000) {
				u32 n = 0;
				for (u32 pg = a; pg < a + 0x80000; pg += 32768)
					n += mx_mem_resident((const void *)(uintptr_t)pg, 32768) != 0;
				if (n)
					mx_printf("census: %s %08x %u\n", rg[r].what, a, n);
				tot += n;
			}
			mx_printf("census: %s in all %u pages (%u KB)\n", rg[r].what, tot, tot * 32);
		}
	}
#endif
	ps2_beam_report();
}

static void ps2shot_f(void) {
	const char *path = Cmd_Argc() > 1 ? Cmd_Argv(1) : "quake3.tga";
	Com_Printf("ps2shot %s: %d\n", path, ps2_ren_screenshot(path));
}

int main(int argc, char **argv) {
	mx_printf("Quake 3 (ioquake3) on maxsdk: virtual memory %s\n", mx_vm_active() ? "on" : "off");
	Sys_Milliseconds();
	/* r_picmip 2: the textures are core memory (the GS reads them by DMA)
	   and copied into 1.8 MiB of GS memory; with the pak of 8-bit textures
	   (make pak: baseq3/ps2tex.pk3) a byte a texel. Measured on the PS2 with
	   demo four: 13.6 fps against 12.4 with r_picmip 1 (less swapping, the
	   textures fit), the HUD's pictures as sharp (they have no picmip) */
	/* the PS2's own settings are 800 characters: room for a run's arguments
	   after them (a line cut off loses its last ones without a word) */
	static char cmdline[4 * MAX_STRING_CHARS];
	Q_strncpyz(cmdline,
	           "+set fs_basepath . +set fs_homepath . +set vm_game 0 +set vm_cgame 0 "
	           "+set vm_ui 0 +set sv_pure 0 +set r_vertexLight 0 +set r_textureMode GL_LINEAR_MIPMAP_LINEAR +set r_picmip 2 "
	           "+set s_mixPreStep 0 +set s_mixahead 0.14 +set in_joystick 1 +set j_yaw -0.012 +set j_pitch 0.008 +set j_forward -0.00776 +set j_side 0.00776 +set j_deadzone 0.22 +set j_curve 2 "
	           "+set r_ignoreGLErrors 1 +set com_introplayed 1 +set cl_allowDownload 0 "
	           "+set ui_cdkeychecked 1 +set bot_enable 1 +set cg_drawFPS 0 +set com_maxfps 0 "
	           "+bind JOY1 \"+moveup\" +bind JOY2 \"+movedown\" +bind JOY3 weapnext "
	           "+bind JOY4 weapprev +bind JOY5 \"+attack\" +bind JOY6 \"+speed\" " PS2_ARGS,
	           sizeof cmdline);
	for (int i = 1; i < argc; i++) {
		Q_strcat(cmdline, sizeof cmdline, " ");
		Q_strcat(cmdline, sizeof cmdline, argv[i]);
	}
	/* the game's data: baseq3 (the full game), else the demo's demoq3 (its
	   pak0 is refused in baseq3: the full game's pak1-8 are wanted there).
	   A com_basegame on the command line goes first; fs_basepath is the last
	   one set there (a run's arguments may come as one argument) */
	{
		char bp[MAX_OSPATH] = ".";
		int given = strstr(cmdline, "+set com_basegame ") != NULL;
		for (const char *p = cmdline; (p = strstr(p, "+set fs_basepath ")) != NULL;) {
			p += strlen("+set fs_basepath ");
			size_t n = strcspn(p, " ");
			if (n >= sizeof bp)
				n = sizeof bp - 1;
			memcpy(bp, p, n);
			bp[n] = 0;
		}
		char path[MAX_OSPATH];
		FILE *f;
		Com_sprintf(path, sizeof path, "%s/" BASEGAME "/pak0.pk3", bp);
		if (!given && !(f = fopen(path, "rb"))) {
			Com_sprintf(path, sizeof path, "%s/demoq3/pak0.pk3", bp);
			if ((f = fopen(path, "rb")) != NULL) {
				fclose(f);
				Q_strcat(cmdline, sizeof cmdline, " +set com_basegame demoq3");
				mx_printf("ps2: no " BASEGAME "/pak0.pk3: the demo's data (demoq3)\n");
			}
		} else if (!given)
			fclose(f);
	}
	Com_Init(cmdline);
	/* ps2shot <file>: the picture as it is now, as a TGA (host: or a path):
	   for pictures of fixed views, e.g. +devmap q3dm1 +wait 300 +setviewpos
	   x y z yaw +wait 60 +ps2shot a.tga +quit */
	Cmd_AddCommand("ps2shot", ps2shot_f);
	NET_Init();
	if (PS2_AUTOQUIT)
		mx_thread_start(watchdog, NULL, wd_stack, sizeof wd_stack, mx_thread_prio(mx_thread_self()) - 1);

	for (u32 frame = 0;; frame++) {
		wd_beat = frame;
		int df0 = ps2_demo_frame();
		int window = PS2_FRAMES ? PS2_PROF && frame >= PS2_PROF : df0 && df0 + 1 >= PS2_PROF;
		if (Q3_VM_TRACE && (PS2_FRAMES ? PS2_PROF && frame + 100 == PS2_PROF : df0 == 1))
			mx_vm_trace_restart();
		if (window && !measuring) {
			measuring = 1;
			/* ps2_load <hz>: the units' load over the demo (ps2_load.c) */
			ps2_load_start((unsigned)Cvar_VariableIntegerValue("ps2_load"));
			mx_vm_stats(&vm_at);
			if (Q3_VM_HIST)
				mx_vm_refill_hist(NULL, 0);
			if (PS2_PROF) {
				mx_vm_trace_mark(1);
				mx_prof_start(2000);
				prof_on = 1;
			}
		}
		/* the counters only over measured frames, which are short: one that
		   reaches 2^31 raises an exception (a map load is long enough) */
		if (PS2_PERF && window) {
			mx_pcr0_write(0), mx_pcr1_write(0);
			mx_pccr_write(0x80000000u | (u32)perf_ev[frame % 3][0] << 5 | 0x1Eu |
			              (u32)perf_ev[frame % 3][1] << 15 | 0x7800u);
		}
#ifdef PS2_REN_PS2
		/* the wait for a free record of beam racing is not the frame's work */
		extern unsigned ps2_ren_wait;
		ps2_ren_wait = 0;
#endif
		if (measuring)
			ps2_load_frame((unsigned)df0, (unsigned)Cvar_VariableIntegerValue("ps2_loadraw"));
		u32 c0 = mx_count();
		Com_Frame();
		u32 cyc = mx_count() - c0;
#ifdef PS2_REN_PS2
		cyc -= ps2_ren_wait;
#endif
		if (PS2_PERF && window) {
			mx_pccr_write(0);
			perf_sum[frame % 3][0] += mx_pcr0(), perf_sum[frame % 3][1] += mx_pcr1();
			perf_cyc[frame % 3] += cyc, perf_nf[frame % 3]++;
		}
		int df1 = ps2_demo_frame();
		if (window && df1 == df0 + 1 && df1 < DEMO_MAX) {
			demo_cyc[df1] = cyc;
			if (!demo_first)
				demo_first = df1;
			demo_last = df1;
		}
		/* a run nobody watches ends anyway (a demo that did not start leaves
		   the game in its menu) */
		if (PS2_AUTOQUIT && Sys_Milliseconds() > PS2_AUTOQUIT) {
			mx_printf("ps2: AUTOQUIT: %d ms, quitting\n", Sys_Milliseconds());
			Sys_Quit();
		}
#if defined(Q3_PERF) && !defined(PS2_REN_PS2)
		{
			/* renderergl1's own counts (tr_surface.c, tr_shade.c) */
			extern unsigned long long ps2_perf[10];
			static unsigned long long at[10];
			if (PS2_PROF && frame == PS2_PROF)
				memcpy(at, ps2_perf, sizeof at);
			if (PS2_FRAMES && frame + 2 == PS2_FRAMES) {
				u32 nfr = frame - PS2_PROF, v[10];
				for (int i = 0; i < 10; i++)
					v[i] = (u32)((ps2_perf[i] - at[i]) / nfr);
				{
					extern unsigned ps2_perfModel[5];
					mx_printf("perf: MD3 surfaces since the start: %u by VU1 (%u strip vertices), not: %u shader, "
					          "%u bounds, %u stage\n", ps2_perfModel[0], ps2_perfModel[4], ps2_perfModel[1],
					          ps2_perfModel[2], ps2_perfModel[3]);
				}
				mx_printf("perf: a frame: MD3 surfaces %u, vertices %u, triangles %u, %u cycles making them; "
				          "%u batches of them, %u cycles drawing those; all: %u batches, %u cycles, %u vertices, "
				          "%u indexes times passes\n", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
			}
		}
#endif
		if (frame % 60 == 0)
			mx_printf("ps2: frame %u at %d ms\n", frame, Sys_Milliseconds());
		if (PS2_FRAMES && frame + 2 == PS2_FRAMES)
			Cbuf_AddText("meminfo\n");
		if (PS2_FRAMES && frame + 1 == PS2_FRAMES) {
			ps2_run_report();
			int r = ps2_ren_screenshot(PS2_SHOT);
			mx_printf("ps2: %u frames, picture %s: %d\n", frame + 1, PS2_SHOT, r);
			mx_exit(0);
		}
	}
	return 0;
}

/* ---- libc and engine odds ---------------------------------------------------- */

#include <sys/stat.h>
mode_t umask(mode_t mask) { return 0; }

/* no OpenAL: snd_main.c falls back to the DMA mixer */
struct soundInterface_s;
qboolean S_AL_Init(struct soundInterface_s *si) { return qfalse; }

/* ---- cinematics: none (cl_cin.c's buffers take 3 MiB) ------------------------ */

#include "../client/client.h"
void CIN_CloseAllVideos(void) {}
e_status CIN_StopCinematic(int handle) { return FMV_EOF; }
e_status CIN_RunCinematic(int handle) { return FMV_EOF; }
int CIN_PlayCinematic(const char *arg, int x, int y, int w, int h, int systemBits) { return -1; }
void CIN_SetExtents(int handle, int x, int y, int w, int h) {}
void CIN_SetLooping(int handle, qboolean loop) {}
void CIN_DrawCinematic(int handle) {}
void CIN_UploadCinematic(int handle) {}
void CL_PlayCinematic_f(void) {}
void SCR_DrawCinematic(void) {}
void SCR_RunCinematic(void) {}
void SCR_StopCinematic(void) {}

/* the timedemo's frame (1 the first), 0: no timedemo is playing */
int ps2_demo_frame(void) {
	return clc.demoplaying && cl_timedemo && cl_timedemo->integer ? clc.timeDemoFrames : 0;
}
