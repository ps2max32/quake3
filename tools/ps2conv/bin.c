/* The PS2 renderer's images and models as two files it maps into memory
   (code/renderer_ps2; was tools/ps2bin.py): nothing is copied or decoded
   when a map loads, a page of them is read when it is first touched and
   given up without a write when memory is short.

     cv_bin(out, 1, ps2tex.pk3)   the images of pak.c's ps2tex.pk3
     cv_bin(out, 2, gamedir)      the MD3 models of the game's paks

   Both files (little-endian):

     head (32 bytes): char magic[4] "PBIN"; u32 version 2; u32 kind (1
         images, 2 models); u32 count; u32 off_index; u32 size; u32 pad[2]
     index: count entries of 128 bytes, sorted by name (bytes, lower case,
         no extension, '/' between directories):
         char name[64]
         u32  off, size                 the entry's data (16-byte aligned)
       images:
         u16  w, h                      level 0's texels (powers of two)
         u16  pic_w, pic_h              the picture's own size
         u8   psm (0x13 T8, 0x14 T4), levels, alpha (its palette has
              alpha), pad
         u32  off_clut                  the GS's order. With alpha: 256
                                        words (T8) or 16 (T4). Without: 256
                                        words also for T4 (its 16 colours
                                        are the first 16 entries: a T4 CLUT
                                        at the same place), and the alpha
                                        of entries 128 .. 255 is a
                                        lightmap's brightness as a blend's
                                        factor: entry 128 + i has 2 i (255
                                        for the last), 0x80 is 1.0. A map's
                                        lightmap pages (T8, map.c) are
                                        drawn with the CLUT of the
                                        surface's texture, whose own pass
                                        takes no alpha from it
         u32  off_level[7]              the levels, each padded to 16 bytes
                                        (T4: the first texel of a byte in
                                        its low four bits); the CLUT and the
                                        levels lie one after the other (off,
                                        size), but the levels over 128
                                        texels a side of 32 KiB or more (a
                                        page of maxsdk's VM, what is read at
                                        a time): each from a page's start on
                                        pages of its own, in the region
                                        after the entries' data (256 x 256:
                                        T8 two pages, T4 one), so mapping
                                        them in is optional: one is read
                                        when the image is drawn that near
                                        and can be given up again alone, no
                                        padding read with it, and the rest
                                        of the images stay as close together
                                        as without them. Smaller ones (256 x
                                        64 and the like) stay with the
                                        image's other levels: alone on a
                                        page they would be read with its
                                        padding
       models:
         the file as it is in the pak (an MD3), at off

   The levels of an image go down to 8 texels a side: the GS's bilinear
   filter and its IMAGE transfers want no less. An image smaller than that
   is left out. The pictures of the screen (gfx/2d, menu, ui, icons) have
   one level: they are not drawn smaller than they are. The models' files of
   fewer details (name_1.md3, name_2.md3) are left out: the renderer draws
   the first.

   The file is made in passes, not in memory (the retail game's are 25 and
   28 MB): the index from each entry's sizes, then the entries' data, then
   the large levels. */
#include "conv.h"

#include <stdlib.h>
#include <string.h>

#define GL_PALETTE4_RGB8_OES 0x8B90
#define GL_PALETTE4_RGBA8_OES 0x8B91
#define GL_PALETTE8_RGB8_OES 0x8B95
#define GL_PALETTE8_RGBA8_OES 0x8B96
#define LEVELS 7
/* The PS2's frame has 5 bits a colour: what is drawn is cut off to them (the
   GS adds its dither's -4 .. 3 first), 4 of 255 darker on average. Every
   colour of a palette is that much brighter here (black stays black: 4 + 3
   is cut off to 0). */
#define BRIGHTER 4
#define PAGE 32768u /* maxsdk's VM page: what is read from the file, and given up, at a time */

static u32 rd16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void put16(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }
static u32 pad16(u32 n) { return (n + 15) & ~15u; }

static int one_level(const char *n) {
  return !strncmp(n, "gfx/2d/", 7) || !strncmp(n, "menu/", 5) || !strncmp(n, "ui/", 3) || !strncmp(n, "icons/", 6);
}

/* an entry: its index fields and its data (malloc'd: the CLUT and levels,
   each padded to 16; or the model) */
struct made {
  u8 rec[128];
  u8 *data;
  u32 size;
  u32 offs[LEVELS]; /* images: the levels' offsets in data */
  int nl;
};

/* a .p8 (pak.c) as an image entry, at most levels levels: 0 or -1 */
static int image(const u8 *blob, u32 len, int levels, struct made *m) {
  if (len < 24 || memcmp(blob, "P8T1", 4))
    return -1;
  u32 width = rd16(blob + 4), height = rd16(blob + 6), w0 = rd16(blob + 12), h0 = rd16(blob + 14);
  u32 nlev = rd16(blob + 16), fmt = rd32(blob + 20);
  if ((w0 & (w0 - 1)) || (h0 & (h0 - 1)) || w0 > 512 || h0 > 512)
    return -1;
  int t4 = fmt == GL_PALETTE4_RGB8_OES || fmt == GL_PALETTE4_RGBA8_OES;
  int alpha = fmt == GL_PALETTE4_RGBA8_OES || fmt == GL_PALETTE8_RGBA8_OES;
  u32 ncol = t4 ? 16 : 256, chans = alpha ? 4 : 3, at = 24;
  if (at + ncol * chans > len)
    return -1;
  const u8 *pal = blob + at;
  at += ncol * chans;
  u32 nclut = t4 && alpha ? 16 : 256;
  u32 clut[256];
  memset(clut, 0, sizeof clut);
  for (u32 i = 0; i < ncol; i++) {
    /* the GS's order of a 256-entry CLUT */
    u32 k = nclut == 16 ? i : (i & 0x18) == 0x08 ? i + 8 : (i & 0x18) == 0x10 ? i - 8 : i;
    const u8 *c = pal + i * chans;
    u32 a = alpha ? (c[3] + 1u) >> 1 : 0x80;
    u32 r = c[0] + BRIGHTER > 255 ? 255 : c[0] + BRIGHTER, g = c[1] + BRIGHTER > 255 ? 255 : c[1] + BRIGHTER;
    u32 b = c[2] + BRIGHTER > 255 ? 255 : c[2] + BRIGHTER;
    clut[k] = r | g << 8 | b << 16 | a << 24;
  }
  if (!alpha)
    for (u32 i = 0; i < 128; i++) {
      u32 k = 128 + i;
      k = (k & 0x18) == 0x08 ? k + 8 : (k & 0x18) == 0x10 ? k - 8 : k;
      clut[k] = (clut[k] & 0xFFFFFF) | (i == 127 ? 255u : 2 * i) << 24;
    }
  /* the size first */
  u32 size = pad16(nclut * 4);
  int nl = 0;
  for (u32 lv = 0, a2 = at; (int)lv < (int)nlev && (int)lv < levels && (w0 >> lv) >= 8 && (h0 >> lv) >= 8; lv++) {
    u32 n = (w0 >> lv) * (h0 >> lv);
    n = t4 ? (n + 1) / 2 : n;
    if (a2 + n > len)
      break;
    m->offs[nl++] = size;
    size += pad16(n);
    a2 += n;
  }
  if (!nl)
    return -1;
  u8 *d = calloc(1, size);
  if (!d)
    return -1;
  for (u32 i = 0; i < nclut; i++)
    put32(d + 4 * i, clut[i]);
  for (int lv = 0; lv < nl; lv++) {
    u32 n = (w0 >> lv) * (h0 >> lv);
    n = t4 ? (n + 1) / 2 : n;
    const u8 *src = blob + at;
    u8 *dst = d + m->offs[lv];
    if (t4) /* the first texel of a byte: the file's high four bits, the GS's low ones */
      for (u32 i = 0; i < n; i++)
        dst[i] = (u8)(src[i] << 4 | src[i] >> 4);
    else
      memcpy(dst, src, n);
    at += n;
  }
  m->data = d, m->size = size, m->nl = nl;
  /* the fields after name, off, size */
  u8 *r = m->rec + 72;
  put16(r, w0), put16(r + 2, h0), put16(r + 4, width), put16(r + 6, height);
  r[8] = t4 ? 0x14 : 0x13, r[9] = (u8)nl, r[10] = (u8)alpha, r[11] = 0;
  return 0;
}

struct ent {
  int src;      /* in the pk3 set */
  char name[64];
  u32 off, size; /* its data, without the large levels */
  int nbig;      /* the first levels that go to the region */
  u32 big_off[LEVELS], big_size[LEVELS];
};

static int by_name(const void *a, const void *b) {
  return strcmp(((const struct ent *)a)->name, ((const struct ent *)b)->name);
}

/* entry e of the set made (an image or a model) */
static int make(pk3_set *s, int kind, const struct ent *e, struct made *m) {
  memset(m, 0, sizeof *m);
  u32 n;
  u8 *blob = pk3_read(s, e->src, &n);
  if (!blob)
    return -1;
  if (kind == 2) {
    m->data = blob, m->size = n;
    return 0;
  }
  int r = image(blob, n, one_level(e->name) ? 1 : LEVELS, m);
  free(blob);
  return r;
}

int cv_bin(const char *out, int kind, const char *src) {
  pk3_set *s = kind == 1 ? pk3_open_file(src) : pk3_open(src);
  if (!s)
    return -1;
  const char *what = kind == 1 ? "images" : "models";
  int cap = pk3_count(s), n = 0, left = 0;
  struct ent *es = calloc((size_t)(cap ? cap : 1), sizeof *es);
  for (int i = 0; i < pk3_count(s); i++) {
    const char *nm = pk3_name(s, i);
    u32 l = (u32)strlen(nm);
    if (kind == 1 ? (l > 3 && !strcmp(nm + l - 3, ".p8"))
                  : (l > 4 && !strcmp(nm + l - 4, ".md3") && !(l >= 6 && nm[l - 6] == '_' &&
                                                               (nm[l - 5] == '1' || nm[l - 5] == '2')))) {
      struct ent *e = &es[n++];
      e->src = i;
      u32 k = l - (kind == 1 ? 3 : 4);
      if (k > 63)
        k = 63;
      memcpy(e->name, nm, k);
    }
  }
  qsort(es, (size_t)n, sizeof *es, by_name);
  struct made m;
  u8 *index = calloc((size_t)(n ? n : 1), 128);
  /* pass 1: the sizes, the index */
  /* offsets from the data's start first: the count is known after the pass */
  u32 off_index = 32, off = 0;
  int k = 0, r = 0;
  for (int i = 0; i < n && !r; i++) {
    if (cv_cancel) {
      r = -1;
      break;
    }
    cv_progress(what, (u32)i, (u32)(2 * n));
    struct ent *e = &es[i];
    if (make(s, kind, e, &m)) {
      left++;
      continue;
    }
    es[k] = *e, e = &es[k];
    u8 *rec = index + 128 * k++;
    memcpy(rec, m.rec, 128);
    memcpy(rec, e->name, strlen(e->name));
    u32 size = m.size;
    e->nbig = 0;
    if (kind == 1) {
      /* its levels over 128 texels a side to the region of their own pages */
      u32 w = rd16(m.rec + 72), h = rd16(m.rec + 74), mx = w > h ? w : h;
      int nb = 0;
      while (nb + 2 < m.nl && (mx >> nb) > 128 && m.offs[nb + 1] - m.offs[nb] >= PAGE)
        nb++;
      u32 cut = nb ? m.offs[nb] - m.offs[0] : 0;
      for (int l = 0; l < nb; l++)
        e->big_size[l] = m.offs[l + 1] - m.offs[l];
      e->nbig = nb;
      size -= cut;
      put32(rec + 84, off); /* the CLUT */
      for (int l = 0; l < LEVELS; l++)
        put32(rec + 88 + 4 * l, off + (l < nb || l >= m.nl ? 0 : m.offs[l] - cut));
    }
    put32(rec + 64, off), put32(rec + 68, size);
    e->off = off, e->size = size;
    off += pad16(size);
    free(m.data);
  }
  n = k;
  u32 base = pad16(off_index + 128 * (u32)n);
  for (int i = 0; i < n; i++) {
    u8 *rec = index + 128 * i;
    es[i].off += base;
    put32(rec + 64, rd32(rec + 64) + base);
    if (kind == 1)
      for (int l = -1; l < LEVELS; l++)
        if (l >= es[i].nbig || l < 0)
          put32(rec + 88 + 4 * l, rd32(rec + 88 + 4 * l) + base);
  }
  off += base;
  /* the levels over 128: each from a page's start, on pages of its own */
  u32 at = off;
  int any = 0;
  for (int i = 0; i < n; i++)
    for (int l = 0; l < es[i].nbig; l++) {
      at += (PAGE - at % PAGE) % PAGE;
      es[i].big_off[l] = at;
      put32(index + 128 * i + 88 + 4 * l, at);
      at += es[i].big_size[l];
      any = 1;
    }
  u32 end = any ? at : off;
  cv_file *f = r ? 0 : cv_open(out, 1);
  if (!f)
    r = -1;
  if (!r) {
    u8 head[32];
    memset(head, 0, sizeof head);
    memcpy(head, "PBIN", 4);
    put32(head + 4, 2), put32(head + 8, (u32)kind), put32(head + 12, (u32)n), put32(head + 16, off_index);
    put32(head + 20, end);
    static const u8 zero[16];
    if (cv_write(f, head, 32) || cv_write(f, index, 128 * (u32)n) ||
        cv_write(f, zero, pad16(32 + 128 * (u32)n) - (32 + 128 * (u32)n)))
      r = -1;
  }
  /* pass 2: the entries' data; pass 3: the large levels */
  u32 pos = pad16(32 + 128 * (u32)n);
  for (int pass = 2; pass <= 3 && !r; pass++)
    for (int i = 0; i < n && !r; i++) {
      struct ent *e = &es[i];
      if (pass == 3 && !e->nbig)
        continue;
      if (cv_cancel) {
        r = -1;
        break;
      }
      if (pass == 2)
        cv_progress(what, (u32)(n + i), (u32)(2 * n));
      if (make(s, kind, e, &m)) {
        r = -1;
        break;
      }
      static u8 zeros[PAGE];
      if (pass == 2) {
        u32 cut = e->nbig ? m.offs[e->nbig] - m.offs[0] : 0;
        u32 first = kind == 1 ? m.offs[0] : m.size;
        if (cv_write(f, m.data, first) || cv_write(f, m.data + first + cut, m.size - first - cut) ||
            cv_write(f, zeros, pad16(e->size) - e->size))
          r = -1;
        pos += pad16(e->size);
      } else
        for (int l = 0; l < e->nbig && !r; l++) {
          if (cv_write(f, zeros, e->big_off[l] - pos) || cv_write(f, m.data + m.offs[l], e->big_size[l]))
            r = -1;
          pos = e->big_off[l] + e->big_size[l];
        }
      free(m.data);
    }
  cv_close(f);
  cv_progress(what, (u32)(2 * n), (u32)(2 * n));
  if (r)
    cv_remove(out);
  else
    cv_log("%s: %d %s, %u.%u MiB%s", out, n, what, end >> 20, ((end & 0xFFFFF) * 10) >> 20,
           kind == 1 && left ? "; some left out (smaller than 8 texels)" : "");
  free(index);
  free(es);
  pk3_close(s);
  return r;
}
