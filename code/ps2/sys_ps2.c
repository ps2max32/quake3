/* sys_ps2.c: the system layer of the engine on maxsdk: main, time, files
   (host: through ps2_file.c), the game modules linked in, the pad, and the
   network and sound stubs (loopback only, no sound device yet). */

#include <stdlib.h>
#include <string.h>

#include <mx/mx.h>
#include <mx/pad.h>

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/keycodes.h"
#include <mxgl.h>

void mx_exit(int status) __attribute__((noreturn));
int ps2_listdir(const char *dir, void (*fn)(const char *name, int is_dir, void *ctx), void *ctx);

/* frames to run before the program ends by itself (0: never), and the
   picture taken then (PS2_SHOT, "host:..."); set by the Makefile */
#include "runcfg.h"
#ifndef PS2_FRAMES
#define PS2_FRAMES 0
#endif
#ifndef PS2_SHOT
#define PS2_SHOT "host:quake3.tga"
#endif
#ifndef PS2_ARGS
#define PS2_ARGS ""
#endif

static void ps2_end(int status) {
	mx_printf("@@PS2TEST_END@@ status=%d\n", status);
	mx_exit(status);
}

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
	ps2_end(1);
}

void Sys_Quit(void) {
	mx_printf("Sys_Quit\n");
	ps2_end(0);
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

static char install_path[MAX_OSPATH] = "host:";
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

/* ---- sound: none yet -------------------------------------------------------- */

qboolean SNDDMA_Init(void) { return qfalse; }
int SNDDMA_GetDMAPos(void) { return 0; }
void SNDDMA_Shutdown(void) {}
void SNDDMA_BeginPainting(void) {}
void SNDDMA_Submit(void) {}
void SNDDMA_StartCapture(void) {}
int SNDDMA_AvailableCaptureSamples(void) { return 0; }
void SNDDMA_Capture(int samples, byte *data) {}
void SNDDMA_StopCapture(void) {}
void SNDDMA_MasterGain(float val) {}

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

/* the free heap: the largest block malloc gives, and the sum of blocks of
   64 KiB and up it gives one after the other */
void ps2_memreport(const char *when) {
	void *blk[512];
	int n = 0;
	unsigned total = 0, largest = 0;
	for (unsigned sz = 16u << 20; sz >= 64 * 1024 && n < 512;) {
		void *p = malloc(sz);
		if (p) {
			blk[n++] = p;
			total += sz;
			if (sz > largest)
				largest = sz;
		} else {
			sz >>= 1;
		}
	}
	while (n)
		free(blk[--n]);
	mx_printf("ps2: free heap %s: largest %u KiB, about %u KiB\n", when, largest >> 10, total >> 10);
}

/* ---- main --------------------------------------------------------------------- */

int main(int argc, char **argv) {
	extern void *mx_heap_start(void), *mx_heap_end(void);
	mx_printf("Quake 3 (ioquake3) on maxsdk: heap %u KiB\n",
	          (unsigned)((char *)mx_heap_end() - (char *)mx_heap_start()) / 1024);
	Sys_Milliseconds();
	char cmdline[MAX_STRING_CHARS];
	Q_strncpyz(cmdline,
	           "+set fs_basepath host: +set fs_homepath host: +set com_hunkMegs 10 +set com_zoneMegs 3 +set vm_game 0 +set vm_cgame 0 "
	           "+set vm_ui 0 +set sv_pure 0 +set r_vertexLight 0 +set r_textureMode GL_LINEAR_MIPMAP_LINEAR +set r_picmip 2 "
	           "+set com_soundMegs 1 +set s_initsound 0 +set in_joystick 1 "
	           "+set r_ignoreGLErrors 1 +set com_introplayed 1 +set cl_allowDownload 0 "
	           "+set ui_cdkeychecked 1 +set bot_enable 0 +set cg_drawFPS 1 +set com_maxfps 0 "
	           "+bind JOY1 \"+moveup\" +bind JOY2 \"+movedown\" +bind JOY3 weapnext "
	           "+bind JOY4 weapprev +bind JOY5 \"+attack\" +bind JOY6 \"+speed\" " PS2_ARGS,
	           sizeof cmdline);
	for (int i = 1; i < argc; i++) {
		Q_strcat(cmdline, sizeof cmdline, " ");
		Q_strcat(cmdline, sizeof cmdline, argv[i]);
	}
	Com_Init(cmdline);
	NET_Init();
	ps2_memreport("after Com_Init");

	for (u32 frame = 0;; frame++) {
		Com_Frame();
		if (frame % 60 == 0)
			mx_printf("ps2: frame %u at %d ms\n", frame, Sys_Milliseconds());
		if (PS2_FRAMES && frame + 2 == PS2_FRAMES)
			Cbuf_AddText("meminfo\n");
		if (PS2_FRAMES && frame + 1 == PS2_FRAMES) {
			int r = mxgl_screenshot(PS2_SHOT);
			mx_printf("ps2: %u frames, picture %s: %d\n", frame + 1, PS2_SHOT, r);
			ps2_end(0);
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
