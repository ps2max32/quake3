/* CONVERT.ELF: Quake 3's data made for the PS2 on the PS2 itself, the
   game's setup= (MXSHELL starts it before the game, each time; mx-os.md,
   "Packaging"). Its screen is the store's install screen: the game's cover
   as a slab, the light filling it, the steps and a bar on the floor; Square
   stops it.

     CONVERT.ELF <data dir> [<version>] [max=128|256]

   The data directory is game.ini's data= ({data}); the version the shell
   chose of game.ini's versions= ({version}: baseq3, the full game, or
   demoq3, the demo), without it baseq3 when it has pak0.pk3, else demoq3. What it makes goes next
   to the game (a game writes only there; the data directory is the user's):

     <game>/ps2tex.pk3          the textures as .p8 (pak.c), max a side:
                                for renderergl1 (Quake 3 GL), which takes a
                                .p8 before the original
     <game>/ps2snd.pk3          the sounds as SPU ADPCM (snd.c)
                                (both paks of the game's directory, which
                                the game's file system finds: fs_homepath ".")
     ps2/<game>/ps2tex.bin      renderer_ps2's file of images (bin.c)
     ps2/<game>/ps2mdl.bin      its file of models
     ps2/<game>/maps/<map>.pmap the maps (map.c)
     ps2/<game>/stamp           what they were made of: this converter's
                                version (a hash of its sources that decide
                                what it makes: any change to them makes all
                                again), max, and each pak's name and size

   The mods beside it in the data directory (a directory with paks of its
   own, as LegoC: the game's fs_game, its menu's Mods) are made the same
   way, each directory alone and after the game's: <mod>/ps2tex.pk3,
   <mod>/ps2snd.pk3, ps2/<mod>/ps2tex.bin, ps2mdl.bin and stamp (the game
   takes them over baseq3's; a mod's maps are not made: they would need the
   base game's textures).

   When each directory's stamp says the same as its paks there now and what
   it made is there, it ends at once (status 0) without a screen; else it
   makes those directories again. Status 0: done; 2:
   stopped by the user (nothing half made is used: the stamp is written
   last); 1: failed (the screen says why until a button is pressed). */
#include <GL/gl.h>
#include <GL/glext.h>
#include <mx/debug.h>
#include <mx/loader.h>
#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/prof.h>
#include <mx/sif.h>
#include <mx/thread.h>
#include <stdlib.h>
#include <string.h>

#include "conv.h"
#include "mxgl.h"
#include "pn.h"

MX_CORE_HEAP(512 << 10);

u32 pad_pressed, pad_held;
int screen;
int pad_on; /* pn.h */

/* its version: a hash of the sources that decide what it makes (the
   Makefile's CONV_HASH, made at each build), in the stamp */
extern const char cv_hash[];

static char gamedir[128], game[24], outdir[64], title[48] = "Quake III Arena", version_name[24];
static int maxside = 128;
static char stamp_now[2048];
/* the directories to look at: the game's (baseq3 or demoq3) first, then the
   mods beside it */
#define MAX_UNITS 9
static struct unit {
  char dir[24];      /* below the data directory */
  char name[48];     /* on the screen: the version's name, a mod's description.txt */
  u32 weight;        /* its paks' bytes: its share of the bar */
  int todo;          /* its stamp is not the same */
  char stamp[2048];  /* what its stamp is to say */
} units[MAX_UNITS];
static int nunits;
static volatile int unit_at; /* the one being made */
static const char *data = ".";
static void unit_use(int i) {
  mx_snprintf(game, sizeof game, "%s", units[i].dir);
  mx_snprintf(gamedir, sizeof gamedir, "%s/%s", data, game);
  mx_snprintf(outdir, sizeof outdir, "ps2/%s", game);
}

/* ---- the stamp ---- */
static u32 stamp_weight;
static void stamp_make(void) {
  stamp_weight = 0;
  int n;
  char *list = cv_list(gamedir, &n);
  u32 at = (u32)mx_snprintf(stamp_now, sizeof stamp_now, "ps2conv %s\nmax %d\n", cv_hash, maxside);
  /* the paks in the order of their names */
  char *names[64];
  int k = 0;
  for (char *s = list; list && n-- && k < 64; s += strlen(s) + 1)
    if (pk3_is_pak(s))
      names[k++] = s;
  for (int i = 1; i < k; i++)
    for (int j = i; j > 0 && pk3_name_cmp(names[j - 1], names[j]) > 0; j--) {
      char *t = names[j];
      names[j] = names[j - 1], names[j - 1] = t;
    }
  for (int i = 0; i < k; i++) {
    char p[192];
    mx_snprintf(p, sizeof p, "%s/%s", gamedir, names[i]);
    cv_file *f = cv_open(p, 0);
    at += (u32)mx_snprintf(stamp_now + at, sizeof stamp_now - at, "%s %u\n", names[i], f ? cv_size(f) : 0);
    stamp_weight += f ? cv_size(f) : 0;
    cv_close(f);
  }
  free(list);
}
static int stamp_same(void) {
  char p[96], buf[2048];
  mx_snprintf(p, sizeof p, "%s/stamp", outdir);
  cv_file *f = cv_open(p, 0);
  if (!f)
    return 0;
  int n = cv_read(f, buf, sizeof buf - 1);
  cv_close(f);
  buf[n > 0 ? n : 0] = 0;
  if (strcmp(buf, stamp_now))
    return 0;
  /* and what it made is there (an older converter put it elsewhere) */
  static const char *const made[] = {"%s/ps2tex.pk3", "%s/ps2snd.pk3", 0};
  for (int i = 0; made[i]; i++) {
    mx_snprintf(p, sizeof p, made[i], game);
    if (!(f = cv_open(p, 0)))
      return 0;
    cv_close(f);
  }
  static const char *const made2[] = {"%s/ps2tex.bin", "%s/ps2mdl.bin", 0};
  for (int i = 0; made2[i]; i++) {
    mx_snprintf(p, sizeof p, made2[i], outdir);
    if (!(f = cv_open(p, 0)))
      return 0;
    cv_close(f);
  }
  return 1;
}

/* ---- the work, on a thread of its own ---- */
enum { ST_TEX, ST_SND, ST_MDL, ST_MAP, NSTEPS };
static const char *const step_name[NSTEPS] = {"Textures", "Sounds", "Models", "Maps"};
/* the share of the time each takes (measured on the PS2, the demo) */
static const float step_share[NSTEPS] = {0.68f, 0.05f, 0.03f, 0.24f};
static volatile int step = -1, finished, failed;
static volatile u32 p_done, p_total;
static char p_what[32], fail_msg[96];
static int nmaps, map_at;
static int prof_tex; /* tests: prof, the textures' profile (mx_prof) */
/* tests: shot=<frames>, the screen to host:convert.tga after that many
   frames, then stopped (status 2); here=<dir>, game.ini and coverhd.t8
   read from there (the shell starts it in the game's directory) */
static int shot_at;
static const char *here = "";

static void progress(const char *what, u32 done, u32 total) {
  mx_snprintf(p_what, sizeof p_what, "%s", what);
  p_done = done, p_total = total;
}

static int make_one(int base);
static void work(void *arg) {
  (void)arg;
  int r = 0;
  for (int i = 0; i < nunits && !r; i++) {
    if (!units[i].todo)
      continue;
    unit_at = i;
    unit_use(i);
    mx_snprintf(stamp_now, sizeof stamp_now, "%s", units[i].stamp);
    step = -1, nmaps = 0, map_at = 0, p_done = p_total = 0;
    r = make_one(i == 0);
  }
  if (r && !cv_cancel)
    mx_snprintf(fail_msg, sizeof fail_msg, "%s could not be made (a full device?)", step >= 0 ? step_name[step] : "It");
  failed = r != 0;
  finished = 1;
}

/* one directory: the game's (base: with its maps) or a mod's */
static int make_one(int base) {
  char p[160], tex[160], s2[160];
  cv_mkdir("ps2");
  cv_mkdir(outdir);
  /* the old stamp first away: what follows is half made until the new one */
  mx_snprintf(p, sizeof p, "%s/stamp", outdir);
  cv_remove(p);
  /* an older converter's place of the textures' pak */
  mx_snprintf(p, sizeof p, "%s/ps2tex.pk3", outdir);
  cv_remove(p);
  cv_mkdir(game);
  mx_snprintf(tex, sizeof tex, "%s/ps2tex.pk3", game);
  int r = 0;
  step = ST_TEX;
  if (prof_tex)
    mx_prof_start(1000);
  r = cv_pak(tex, gamedir, maxside);
  if (prof_tex)
    mx_prof_stop(), mx_prof_report(100);
  if (!r) {
    step = ST_SND;
    mx_snprintf(s2, sizeof s2, "%s/ps2snd.pk3", game);
    r = cv_snd(s2, gamedir);
  }
  if (!r) {
    step = ST_MDL;
    mx_snprintf(p, sizeof p, "%s/ps2tex.bin", outdir);
    r = cv_bin(p, 1, tex);
    mx_snprintf(p, sizeof p, "%s/ps2mdl.bin", outdir);
    if (!r)
      r = cv_bin(p, 2, gamedir);
  }
  if (!r && !base)
    step = ST_MAP;
  if (!r && base) {
    step = ST_MAP;
    pk3_set *s = pk3_open(gamedir);
    mx_snprintf(p, sizeof p, "%s/maps", outdir);
    cv_mkdir(p);
    nmaps = 0;
    for (int i = 0; s && i < pk3_count(s); i++) {
      const char *n = pk3_name(s, i);
      u32 l = (u32)strlen(n);
      nmaps += !strncmp(n, "maps/", 5) && l > 9 && !strcmp(n + l - 4, ".bsp") && !strchr(n + 5, '/');
    }
    map_at = 0;
    for (int i = 0; s && i < pk3_count(s) && !r; i++) {
      const char *n = pk3_name(s, i);
      u32 l = (u32)strlen(n);
      if (strncmp(n, "maps/", 5) || l <= 9 || strcmp(n + l - 4, ".bsp") || strchr(n + 5, '/'))
        continue;
      char name[64];
      mx_snprintf(name, sizeof name, "%.*s", (int)(l - 9), n + 5);
      mx_snprintf(p, sizeof p, "%s/maps/%s.pmap", outdir, name);
      if (cv_map(p, gamedir, name, tex))
        r = cv_cancel ? -1 : 0, cv_log("%s: not made", name); /* one map that fails is not drawn: the rest goes on */
      map_at++;
    }
    pk3_close(s);
  }
  if (!r) {
    mx_snprintf(p, sizeof p, "%s/stamp", outdir);
    cv_file *f = cv_open(p, 1);
    if (!f || cv_write(f, stamp_now, (u32)strlen(stamp_now)))
      r = -1;
    cv_close(f);
  }
  return r;
}

/* ---- the game's cover (coverhd.t8: 256 x 361, the shell's format) ---- */
static int cover_tex;
static void cover_load(void) {
  char cp[160];
  mx_snprintf(cp, sizeof cp, "%scoverhd.t8", here);
  cv_file *f = cv_open(cp, 0);
  if (!f)
    return;
  u8 *buf = malloc(1024 + 256 * 512);
  static u8 clut[1024];
  if (!buf || cv_read(f, clut, 1024) != 1024 || cv_read(f, buf + 1024, 256 * 361) != 256 * 361) {
    free(buf), cv_close(f);
    return;
  }
  cv_close(f);
  for (int k = 0; k < 256; k++) {
    int at = (k & ~0x18) | (k & 8) << 1 | (k & 0x10) >> 1;
    buf[4 * k] = clut[4 * at], buf[4 * k + 1] = clut[4 * at + 1], buf[4 * k + 2] = clut[4 * at + 2], buf[4 * k + 3] = 255;
  }
  memset(buf + 1024 + 256 * 361, buf[1024 + 256 * 360], 256 * (512 - 361));
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_PALETTE8_RGBA8_OES, 256, 512, 0, 1024 + 256 * 512, buf);
  free(buf);
  cover_tex = (int)t;
}

/* ---- the screen (the store's install screen) ---- */
static float overall(void) {
  float k = 0;
  for (int i = 0; i < NSTEPS && i < step; i++)
    k += step_share[i];
  if (step >= 0 && step < NSTEPS) {
    float in = step == ST_MAP ? (nmaps ? (map_at + (p_total ? (float)p_done / p_total : 0)) / nmaps : 0)
                              : (p_total ? (float)p_done / p_total : 0);
    k += step_share[step] * (in > 1 ? 1 : in);
  }
  /* the directories to make, by their paks' bytes */
  u32 all = 0, before = 0;
  for (int i = 0; i < nunits; i++)
    if (units[i].todo) {
      all += units[i].weight;
      if (i < unit_at)
        before += units[i].weight;
    }
  if (all)
    k = ((float)before + (float)units[unit_at].weight * k) / (float)all;
  return finished && !failed ? 1 : k;
}

static void draw(void) {
  glClearColor((C_INK >> 16 & 255) / 255.0f, (C_INK >> 8 & 255) / 255.0f, (C_INK & 255) / 255.0f, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  room_begin(-1);
  room_light(SCR_W * 0.5f, 240, 480, 300, 0.07f);
  d_begin_2d();
  room_draw();
  d_rect(0, 0, SCR_W, SCR_H, argb(C_INK, 0.55f), argb(C_INK, 1));
  /* the slab in the middle, the light filling it from below */
  struct tile_look slab;
  memset(&slab, 0, sizeof slab);
  slab.w = 204, slab.h = 288, slab.x = SCR_W * 0.5f - 102, slab.y = 92, slab.lit = 1, slab.fade = 1;
  slab.base = 0x1B232E;
  if (cover_tex)
    slab.tex = cover_tex, slab.tex_u = 1, slab.tex_v = 361.0f / 512.0f;
  d_begin_3d();
  light_at(SCR_W * 0.5f, 0);
  d_tile(&slab);
  d_begin_2d();
  if (!slab.tex) /* without a cover: its title on the slab, as the shelf's tile */
    d_text_fit(FONT_TITLE, FONT_BODY, slab.x + 16, slab.y + slab.h - 22, slab.w - 32, title, argb(C_WHITE, 1));
  float k = overall();
  d_rect(slab.x, slab.y + slab.h * (1 - 0.62f * k), slab.x + slab.w, slab.y + slab.h, argb(C_LAMP, 0.05f),
         argb(C_LAMP, 0.22f));
  /* the game and its data's version; what is going on */
  char t[96], where[128];
  int stopped = finished && cv_cancel;
  mx_snprintf(t, sizeof t, "%s" DOT "%s", title, unit_at ? units[unit_at].name : version_name);
  float tw = d_text_w(FONT_TITLE, t);
  d_text(FONT_TITLE, SCR_W * 0.5f - tw * 0.5f, 430, t, argb(C_WHITE, 1));
  if (failed && !stopped)
    mx_snprintf(where, sizeof where, "Not ready: %s", fail_msg);
  else if (stopped)
    mx_snprintf(where, sizeof where, "Stopped" DOT "optimized again at the next start");
  else if (finished)
    mx_snprintf(where, sizeof where, "Optimized for PS2");
  else
    mx_snprintf(where, sizeof where, "Optimizing for PS2" DOT "once, before its first start");
  tw = d_text_w(FONT_BODY, where);
  d_text(FONT_BODY, SCR_W * 0.5f - tw * 0.5f, 462, where, argb(failed && !stopped ? C_WARN : C_DIM, 1));
  /* the progress: the floor lit from the left */
  float bx0 = SCR_W * 0.5f - 400, bx1 = SCR_W * 0.5f + 400;
  if (SCR_W < 1100)
    bx0 = MARGIN, bx1 = SCR_W - MARGIN;
  d_bar(bx0, 500, bx1, 502, k, C_LAMP, 1);
  d_glow(bx0 + (bx1 - bx0) * k, 501, 240, 60, argb(C_LAMP, 0.14f));
  /* the steps */
  float cw = (bx1 - bx0) / NSTEPS;
  for (int i = 0; i < NSTEPS; i++) {
    float sx = bx0 + cw * i;
    int ph = finished && !failed ? NSTEPS : step;
    int is_done = ph > i, now = ph == i && !finished;
    u32 c = now ? C_LAMP : is_done ? C_WHITE : C_FAINT;
    if (now)
      d_glow(sx + 6, 532, 12, 12, argb(C_LAMP, 0.6f));
    d_text(FONT_BODY, sx + (now || is_done ? 20 : 0), 538, step_name[i], argb(c, 1));
    if (is_done)
      d_ring(sx + 6, 532, 5, argb(C_WHITE, 0.9f));
    char m[64] = "";
    if (now && i == ST_MAP && nmaps)
      mx_snprintf(m, sizeof m, "%s" DOT "%d of %d", p_what, map_at + 1 > nmaps ? nmaps : map_at + 1, nmaps);
    else if (now && p_total)
      mx_snprintf(m, sizeof m, "%u of %u", p_done, p_total);
    d_text_fit(FONT_SMALL, FONT_LABEL, sx, 564, cw - 16, m, argb(C_FAINT, 1));
  }
  static const char *const h_run[] = {SYM_SQUARE, "Stop", 0};
  static const char *const h_end[] = {SYM_CROSS, "Back", 0};
  if (!finished)
    hints_draw(h_run);
  else if (failed)
    hints_draw(h_end);
  toast_draw();
  mxgl_end_frame();
  mxgl_frame_done();
}

static u8 stack[256 << 10] MX_ALIGNED(16) MX_IRQ_BSS; /* a thread stack: core */

int main(int argc, char **argv) {
  const char *version = 0;
  int npos = 0;
  for (int i = 1; i < argc; i++) {
    if (!strncmp(argv[i], "max=", 4))
      maxside = atoi(argv[i] + 4) >= 256 ? 256 : 128;
    else if (!strcmp(argv[i], "prof"))
      prof_tex = 1;
    else if (!strncmp(argv[i], "shot=", 5))
      shot_at = atoi(argv[i] + 5);
    else if (!strncmp(argv[i], "here=", 5))
      here = argv[i] + 5;
    else if (npos++ == 0)
      data = argv[i];
    else
      version = argv[i];
  }
  /* the game: the version the shell chose (game.ini's versions=, {version}),
     else baseq3 when it has pak0.pk3, else the demo's demoq3 */
  cv_file *f = 0;
  if (version && *version)
    mx_snprintf(game, sizeof game, "%s", version);
  else {
    mx_snprintf(gamedir, sizeof gamedir, "%s/baseq3/pak0.pk3", data);
    f = cv_open(gamedir, 0);
    mx_snprintf(game, sizeof game, "%s", f ? "baseq3" : "demoq3");
    cv_close(f);
  }
  /* the game's directory, then the mods beside it: directories with paks
     of their own (not the other base game, nor Team Arena's missionpack,
     whose game code this does not have) */
  mx_snprintf(units[0].dir, sizeof units[0].dir, "%s", game);
  nunits = 1;
  int dn;
  char *dl = cv_list(data, &dn);
  for (char *d = dl; dl && dn-- && nunits < MAX_UNITS; d += strlen(d) + 1) {
    if (d[0] == '.' || strlen(d) >= sizeof units[0].dir || !pk3_name_cmp(d, "baseq3") || !pk3_name_cmp(d, "demoq3") ||
        !pk3_name_cmp(d, "missionpack") || !pk3_name_cmp(d, game))
      continue;
    char sub[160];
    int sn, paks = 0;
    mx_snprintf(sub, sizeof sub, "%s/%s", data, d);
    char *sl = cv_list(sub, &sn);
    for (char *e = sl; sl && sn--; e += strlen(e) + 1)
      paks += pk3_is_pak(e);
    free(sl);
    if (!paks)
      continue;
    struct unit *u = &units[nunits++];
    mx_snprintf(u->dir, sizeof u->dir, "%s", d);
    mx_snprintf(u->name, sizeof u->name, "%s", d);
    /* its name: description.txt, as the game's Mods menu shows it */
    mx_snprintf(sub, sizeof sub, "%s/%s/description.txt", data, d);
    cv_file *df = cv_open(sub, 0);
    if (df) {
      char b[48];
      int n = cv_read(df, b, sizeof b - 1);
      cv_close(df);
      u32 l = 0;
      while (n > 0 && l < (u32)n && b[l] != '\n' && b[l] != '\r')
        l++;
      while (l && b[l - 1] == ' ')
        l--;
      if (l)
        memcpy(u->name, b, l), u->name[l] = 0;
    }
  }
  free(dl);
  int todo = 0;
  for (int i = 0; i < nunits; i++) {
    unit_use(i);
    stamp_make();
    struct unit *u = &units[i];
    memcpy(u->stamp, stamp_now, sizeof u->stamp);
    u->weight = stamp_weight ? stamp_weight : 1;
    u->todo = shot_at || !stamp_same();
    todo += u->todo;
    mx_printf("ps2conv: %s: %s (max %d)\n", gamedir, u->todo ? "to be made" : "made already", maxside);
  }
  if (!todo)
    return 0;
  unit_use(0);
  /* the version's name: game.ini's versions= ("<name>:<file>:<word>"),
     else these */
  mx_snprintf(version_name, sizeof version_name, "%s", !strcmp(game, "demoq3") ? "Demo" : "Full game");
  /* the title and the version from the game's own game.ini (its directory
     is where the shell starts this; a game.ini without setup=CONVERT.ELF
     is not Quake 3's) */
  char ip[160];
  mx_snprintf(ip, sizeof ip, "%sgame.ini", here);
  f = cv_open(ip, 0);
  if (f) {
    static char buf[2048];
    int n = cv_read(f, buf, sizeof buf - 1);
    cv_close(f);
    buf[n > 0 ? n : 0] = 0;
    char *t = strstr(buf, "title=");
    if (strstr(buf, "setup=CONVERT.ELF") && t && (t == buf || t[-1] == '\n')) {
      t += 6;
      u32 l = 0;
      while (t[l] && t[l] != '\n' && t[l] != '\r' && l + 1 < sizeof title)
        l++;
      memcpy(title, t, l), title[l] = 0;
    }
    char *v = strstr(buf, "versions=");
    if (strstr(buf, "setup=CONVERT.ELF") && v && (v == buf || v[-1] == '\n'))
      for (v += 9; *v && *v != '\n' && *v != '\r';) {
        char *c1 = strchr(v, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : 0;
        if (!c2)
          break;
        u32 wl = (u32)strcspn(c2 + 1, ",\r\n");
        if (wl == strlen(game) && !strncmp(c2 + 1, game, wl))
          mx_snprintf(version_name, sizeof version_name, "%.*s", (int)(c1 - v), v);
        v = c2 + 1 + wl;
        if (*v == ',')
          v++;
      }
  }
  cfg_load();
  mxgl_init();
  pad_on = mx_pad_init() >= 0;
  video_apply();
  cover_load();
  cv_progress = progress;
  mx_thread_start(work, 0, stack, sizeof stack, mx_thread_prio(mx_thread_self()) + 1);
  u32 end_at = 0;
  for (;;) {
    /* while it works, the screen about ten times a second (its frames take
       the EE from the converter: a fifth of its time at sixty), ticks
       counted as frames */
    ticks += finished ? 1 : 6;
    pad_pressed = pad_on ? mx_pad_pressed(0) | mx_pad_pressed(1) : 0;
    toast_step();
    if (!finished && (pad_pressed & MX_PAD_SQUARE))
      cv_cancel = 1;
    draw();
    if (shot_at && ticks / 6 == (u32)shot_at) {
      int r = mxgl_screenshot("host:convert.tga");
      mx_printf("ps2conv: shot %d\n", r);
      return 2; /* at once, as the shell's and the store's shot= */
    }
    if (!finished)
      mx_sleep_us(80000);
    if (finished) {
      if (cv_cancel)
        return 2;
      if (!failed) {
        /* "Ready" a moment, then the game */
        if (!end_at)
          end_at = ticks;
        if (ticks - end_at > 45)
          return 0;
      } else if (pad_pressed & (MX_PAD_CROSS | MX_PAD_CIRCLE))
        return 1;
    }
  }
}
