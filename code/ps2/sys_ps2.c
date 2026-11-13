/* sys_ps2.c: the system layer of the engine on maxsdk: main, time, files
   (host: through ps2_file.c), the game modules linked in, the pad, and the
   network and sound stubs (loopback only, no sound device yet). */

#include <stdlib.h>
#include <string.h>

#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/vm.h>

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/keycodes.h"
#include <mx/prof.h>
#include <mxgl.h>

void mx_exit(int status) __attribute__((noreturn));
int ps2_listdir(const char *dir, void (*fn)(const char *name, int is_dir, void *ctx), void *ctx);

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
	return (int)((c - base) / (MX_EE_HZ / 1000));
}

void Sys_Sleep(int msec) {
	if (msec <= 0)
		return;
	int t = Sys_Milliseconds();
	while (Sys_Milliseconds() - t < msec)
		;
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
	   half SDL's range (-16383 .. 16383: j_yaw -0.022 is 360 degrees a
	   second at full turn), the dead zone (32 of 128) taken out */
	u8 st[4];
	static int axis[4];
	if (mx_pad_sticks(0, st)) {
		static const int map[4] = {2, 3, 0, 1}; /* st: right x, y, left x, y */
		for (int k = 0; k < 4; k++) {
			int v = (int)st[k] - 128;
			v = v > -32 && v < 32 ? 0 : v > 0 ? (v - 32) * 16383 / 95 : (v + 32) * 16383 / 96;
			if (v > 16383)
				v = 16383;
			if (v < -16383)
				v = -16383;
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

int main(int argc, char **argv) {
	mx_printf("Quake 3 (ioquake3) on maxsdk: virtual memory %s\n", mx_vm_active() ? "on" : "off");
	Sys_Milliseconds();
	/* r_picmip 2: the textures are core memory (the GS reads them by DMA)
	   and copied into 1.8 MiB of GS memory; with the pak of 8-bit textures
	   (make pak: baseq3/ps2tex.pk3) a byte a texel. Measured on the PS2 with
	   demo four: 13.6 fps against 12.4 with r_picmip 1 (less swapping, the
	   textures fit), the HUD's pictures as sharp (they have no picmip) */
	char cmdline[MAX_STRING_CHARS];
	Q_strncpyz(cmdline,
	           "+set fs_basepath . +set fs_homepath . +set vm_game 0 +set vm_cgame 0 "
	           "+set vm_ui 0 +set sv_pure 0 +set r_vertexLight 0 +set r_textureMode GL_LINEAR_MIPMAP_LINEAR +set r_picmip 2 "
	           "+set s_mixPreStep 0 +set s_mixahead 0.14 +set in_joystick 1 "
	           "+set r_ignoreGLErrors 1 +set com_introplayed 1 +set cl_allowDownload 0 "
	           "+set ui_cdkeychecked 1 +set bot_enable 0 +set cg_drawFPS 0 +set com_maxfps 0 "
	           "+bind JOY1 \"+moveup\" +bind JOY2 \"+movedown\" +bind JOY3 weapnext "
	           "+bind JOY4 weapprev +bind JOY5 \"+attack\" +bind JOY6 \"+speed\" " PS2_ARGS,
	           sizeof cmdline);
	for (int i = 1; i < argc; i++) {
		Q_strcat(cmdline, sizeof cmdline, " ");
		Q_strcat(cmdline, sizeof cmdline, argv[i]);
	}
	Com_Init(cmdline);
	NET_Init();

	for (u32 frame = 0;; frame++) {
#ifdef Q3_PERF
		/* the EE's performance counters over the profile's frames: a pair of
		   events a frame, by turns (EE Core User's Manual, table 7-1) */
		static const u8 ev[3][2] = {{6, 6}, {12, 4}, {5, 5}};
		static u64 sum[3][2], cyc[3];
		static u32 nf[3];
		/* only over the profile's frames, which are short: a counter that
		   reaches 2^31 raises an exception (a map load is long enough) */
		u32 pc0 = mx_count();
		int counting = PS2_PROF && frame >= PS2_PROF;
		if (counting) {
			mx_pcr0_write(0), mx_pcr1_write(0);
			mx_pccr_write(0x80000000u | (u32)ev[frame % 3][0] << 5 | 0x1Eu | (u32)ev[frame % 3][1] << 15 | 0x7800u);
		}
#endif
		Com_Frame();
#ifdef Q3_PERF
		if (counting) {
			mx_pccr_write(0);
			sum[frame % 3][0] += mx_pcr0(), sum[frame % 3][1] += mx_pcr1();
			cyc[frame % 3] += mx_count() - pc0, nf[frame % 3]++;
		}
		{
			extern unsigned long long ps2_perf[10];
			static unsigned long long at[10];
			if (PS2_PROF && frame == PS2_PROF)
				memcpy(at, ps2_perf, sizeof at);
			if (PS2_FRAMES && frame + 2 == PS2_FRAMES) {
				u32 nfr = frame - PS2_PROF, v[10];
				for (int i = 0; i < 10; i++)
					v[i] = (u32)((ps2_perf[i] - at[i]) / nfr);
				mx_printf("perf: a frame: MD3 surfaces %u, vertices %u, triangles %u, %u cycles making them; "
				          "%u batches of them, %u cycles drawing those; all: %u batches, %u cycles, %u vertices, "
				          "%u indexes times passes\n", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
			}
		}
		if (PS2_FRAMES && frame + 2 == PS2_FRAMES)
			mx_printf("perf: a frame: I-cache misses %u, D-cache misses %u (%u cycles); instructions %u, "
			          "TLB misses %u (%u cycles); ITLB misses %u, DTLB misses %u\n",
			          (u32)(sum[0][0] / nf[0]), (u32)(sum[0][1] / nf[0]), (u32)(cyc[0] / nf[0]),
			          (u32)(sum[1][0] / nf[1]), (u32)(sum[1][1] / nf[1]), (u32)(cyc[1] / nf[1]),
			          (u32)(sum[2][0] / nf[2]), (u32)(sum[2][1] / nf[2]));
#endif
		if (frame % 60 == 0)
			mx_printf("ps2: frame %u at %d ms\n", frame, Sys_Milliseconds());
		if (PS2_FRAMES && frame + 2 == PS2_FRAMES)
			Cbuf_AddText("meminfo\n");
		if (Q3_VM_TRACE && PS2_PROF && frame + 100 == PS2_PROF)
			mx_vm_trace_restart();
		if (PS2_PROF && frame == PS2_PROF) {
			mx_vm_trace_mark(1);
			mx_prof_start(2000);
		}
		if (PS2_FRAMES && frame + 1 == PS2_FRAMES) {
			if (PS2_PROF) {
				mx_prof_stop();
				mx_vm_trace_mark(2);
				mx_prof_report(4000);
			}
			if (Q3_VM_TRACE)
				mx_printf("ps2: VM trace quake3.trc: %d events\n",
				          mx_vm_trace_save("quake3.trc"));
			int r = mxgl_screenshot(PS2_SHOT);
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
