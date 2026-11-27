/* Quake 3's textures as 8-bit (or 4-bit) paletted textures for the PS2 (was
   tools/ps2pak.py).

     cv_pak(out.pk3, gamedir, max)

   Reads the pak*.pk3 of the game directory in order (a later pak's file
   replaces an earlier one's; in one pak a .tga is taken before a .jpg of the
   same name, as the renderer does), and writes for each image a <name>.p8
   into OUT.pk3. renderergl1 (tr_image.c, PS2) loads a .p8 for a mipmapped
   image before the original, and uploads it with one glCompressedTexImage2D;
   the PS2's own renderer (code/renderer_ps2) draws with nothing else: an
   image without a .p8 is not drawn. The pictures of the screen (gfx/2d,
   menu, ui, icons: the font, the HUD) are kept up to 256 a side, the ones
   that fill it (the menus' background, the levels' pictures while one
   loads: seen only there, so they cost no frame of play) up to 512.

   Each image is made the size the renderer would (sides rounded down to
   powers of two, by Pillow's BOX filter as the Python tool did: the colour
   and the alpha each by itself), then halved on both sides until it fits max
   (GL_MAX_TEXTURE_SIZE). Its own palette comes from that level (no
   dithering): exact when it has 256 colours or fewer; else Wu's quantizer
   (a 32 x 32 x 32 histogram of its colours, boxes cut where the variance
   drops most) and k-means rounds in RGB, and with alpha the entries made
   RGBA by k-means as before. The Python tool took libimagequant's palette:
   on the EE that takes hours, this one seconds (its error against the
   full-colour texels is reported, as the Python's was). The levels below are
   made from the full-colour level (2 x 2 box, as R_MipMap) and given the
   nearest palette entry. A 4-bit palette (half the GS memory, half the bytes
   to send) when the image has 16 colours or fewer, or when 16 colours leave
   level 0 within T4DB dB (peak signal to noise, over its channels) of the
   full-colour texels.

   .p8 layout (little-endian):
       char  magic[4]   "P8T1"
       u16   width, height        the image's own size (image_t width/height)
       u16   pw, ph               the sides rounded to powers of two
       u16   w0, h0               the size of the first level stored
       u16   levels               levels stored (w0 x h0 down to 1 x 1)
       u16   pad
       u32   format               GL_PALETTE8_RGB8_OES, _RGBA8, PALETTE4_*
       then the glCompressedTexImage2D blob: the palette (16 or 256 entries,
       3 or 4 bytes each) and the levels' indices (4-bit: two a byte, the
       first in the high bits; each level whole bytes). */
#include "conv.h"

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeglib.h"

#define GL_PALETTE4_RGB8_OES 0x8B90
#define GL_PALETTE4_RGBA8_OES 0x8B91
#define GL_PALETTE8_RGB8_OES 0x8B95
#define GL_PALETTE8_RGBA8_OES 0x8B96
#define MAX2D 256
#define MAXFULL 512
#define T4DB 35.0
#define KMEANS 3 /* rounds after Wu's cut */

/* ---- decoding: TGA and JPEG to RGBA ---- */
struct img {
  int w, h;
  u8 *p; /* w * h * 4 */
};

static int tga(const u8 *d, u32 n, struct img *im) {
  if (n < 18)
    return -1;
  int idlen = d[0], cmtype = d[1], type = d[2], cmfirst = d[3] | d[4] << 8, cmlen = d[5] | d[6] << 8;
  int cmbits = d[7], w = d[12] | d[13] << 8, h = d[14] | d[15] << 8, bpp = d[16], desc = d[17];
  int rle = type >= 9, base = type & 7;
  if (!w || !h || (base != 1 && base != 2 && base != 3) || (base == 1 && (!cmtype || bpp != 8)))
    return -1;
  if ((base == 2 && bpp != 16 && bpp != 15 && bpp != 24 && bpp != 32) || (base == 3 && bpp != 8))
    return -1;
  u32 at = 18 + (u32)idlen;
  u8 cmap[256][4];
  memset(cmap, 0, sizeof cmap);
  if (cmtype) {
    u32 eb = (u32)(cmbits + 7) / 8;
    for (int i = 0; i < cmlen; i++, at += eb) {
      if (at + eb > n)
        return -1;
      int k = cmfirst + i;
      if (k < 0 || k > 255)
        continue;
      if (eb >= 3)
        cmap[k][0] = d[at + 2], cmap[k][1] = d[at + 1], cmap[k][2] = d[at], cmap[k][3] = eb == 4 ? d[at + 3] : 255;
      else {
        u32 v = d[at] | d[at + 1] << 8;
        cmap[k][0] = (u8)((v >> 10 & 31) * 255 / 31), cmap[k][1] = (u8)((v >> 5 & 31) * 255 / 31);
        cmap[k][2] = (u8)((v & 31) * 255 / 31), cmap[k][3] = 255;
      }
    }
  }
  int pb = (bpp + 7) / 8;
  u8 *px = malloc((size_t)w * h * 4);
  if (!px)
    return -1;
  u32 total = (u32)w * h;
  u8 raw[4] = {0, 0, 0, 0};
  for (u32 i = 0; i < total;) {
    int count = 1, run = 0;
    if (rle) {
      if (at >= n)
        goto bad;
      int c = d[at++];
      count = (c & 127) + 1, run = c & 128;
      if (run) {
        if (at + (u32)pb > n)
          goto bad;
        memcpy(raw, d + at, (size_t)pb);
        at += (u32)pb;
      }
    } else
      run = 0;
    for (int k = 0; k < count && i < total; k++, i++) {
      if (!run) {
        if (at + (u32)pb > n)
          goto bad;
        memcpy(raw, d + at, (size_t)pb);
        at += (u32)pb;
      }
      /* the row on the screen: bottom-up unless desc bit 5 */
      u32 x = i % (u32)w, y = i / (u32)w;
      if (!(desc & 0x20))
        y = (u32)h - 1 - y;
      if (desc & 0x10)
        x = (u32)w - 1 - x;
      u8 *o = px + 4 * (y * (u32)w + x);
      if (base == 1)
        memcpy(o, cmap[raw[0]], 4);
      else if (base == 3)
        o[0] = o[1] = o[2] = raw[0], o[3] = 255;
      else if (pb == 2) {
        u32 v = raw[0] | raw[1] << 8;
        o[0] = (u8)((v >> 10 & 31) * 255 / 31), o[1] = (u8)((v >> 5 & 31) * 255 / 31), o[2] = (u8)((v & 31) * 255 / 31);
        o[3] = 255;
      } else
        o[0] = raw[2], o[1] = raw[1], o[2] = raw[0], o[3] = pb == 4 ? raw[3] : 255;
    }
  }
  im->w = w, im->h = h, im->p = px;
  return 0;
bad:
  free(px);
  return -1;
}

struct jerr {
  struct jpeg_error_mgr pub;
  jmp_buf jb;
};
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }
static void jerr_out(j_common_ptr c) { (void)c; }

static int jpg(const u8 *d, u32 n, struct img *im) {
  struct jpeg_decompress_struct c;
  struct jerr e;
  u8 *volatile px = 0;
  c.err = jpeg_std_error(&e.pub);
  e.pub.error_exit = jerr_exit, e.pub.output_message = jerr_out;
  if (setjmp(e.jb)) {
    jpeg_destroy_decompress(&c);
    free(px);
    return -1;
  }
  jpeg_create_decompress(&c);
  jpeg_mem_src(&c, (unsigned char *)d, n);
  jpeg_read_header(&c, TRUE);
  c.out_color_space = c.jpeg_color_space == JCS_GRAYSCALE ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_start_decompress(&c);
  int w = (int)c.output_width, h = (int)c.output_height, ch = c.output_components;
  px = malloc((size_t)w * h * 4);
  u8 *row = malloc((size_t)w * ch);
  if (!px || !row) {
    free(row);
    longjmp(e.jb, 1);
  }
  while (c.output_scanline < c.output_height) {
    int y = (int)c.output_scanline;
    JSAMPROW r = row;
    jpeg_read_scanlines(&c, &r, 1);
    for (int x = 0; x < w; x++) {
      u8 *o = px + 4 * ((size_t)y * w + x);
      if (ch == 1)
        o[0] = o[1] = o[2] = row[x];
      else
        o[0] = row[3 * x], o[1] = row[3 * x + 1], o[2] = row[3 * x + 2];
      o[3] = 255;
    }
  }
  free(row);
  jpeg_finish_decompress(&c);
  jpeg_destroy_decompress(&c);
  im->w = w, im->h = h, im->p = px;
  return 0;
}

/* ---- sizes ---- */
static int floor_pow2(int n) {
  int p = 1;
  while (p * 2 <= n)
    p *= 2;
  return p;
}

/* Pillow's BOX resampling of one 8-bit channel (Resample.c: coefficients in
   double, normalised, 22-bit fixed point; the rows first, then the columns) */
#define PREC 22
static int box_coeffs(int in, int out, int **bounds, s32 **kk) {
  double scale = (double)in / out, fs = scale < 1 ? 1 : scale, support = 0.5 * fs, ss = 1.0 / fs;
  int ksize = (int)ceil(support) * 2 + 1;
  *bounds = malloc((size_t)out * 2 * sizeof **bounds);
  *kk = malloc((size_t)out * ksize * sizeof **kk);
  double *k = malloc((size_t)ksize * sizeof *k);
  for (int xx = 0; xx < out; xx++) {
    double center = (xx + 0.5) * scale, ww = 0;
    int xmin = (int)(center - support + 0.5), xmax = (int)(center + support + 0.5);
    if (xmin < 0)
      xmin = 0;
    if (xmax > in)
      xmax = in;
    xmax -= xmin;
    for (int x = 0; x < xmax; x++) {
      double t = (x + xmin - center + 0.5) * ss;
      double w = t > -0.5 && t <= 0.5 ? 1.0 : 0.0;
      k[x] = w, ww += w;
    }
    for (int x = 0; x < xmax; x++) {
      double v = ww ? k[x] / ww : k[x];
      (*kk)[xx * ksize + x] = (s32)(v < 0 ? v * (1 << PREC) - 0.5 : v * (1 << PREC) + 0.5);
    }
    for (int x = xmax; x < ksize; x++)
      (*kk)[xx * ksize + x] = 0;
    (*bounds)[2 * xx] = xmin, (*bounds)[2 * xx + 1] = xmax;
  }
  free(k);
  return ksize;
}
static u8 clip8(int64_t v) {
  v >>= PREC;
  return (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
}
/* channel c of src (w x h, 4 a texel) into dst (pw x ph, 4 a texel) */
static void box_resize(const u8 *src, int w, int h, u8 *dst, int pw, int ph, int c) {
  int *bx, *by;
  s32 *kx, *ky;
  int nx = box_coeffs(w, pw, &bx, &kx), ny = box_coeffs(h, ph, &by, &ky);
  u8 *tmp = malloc((size_t)pw * h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < pw; x++) {
      int64_t s = 1 << (PREC - 1);
      for (int k = 0; k < bx[2 * x + 1]; k++)
        s += (int64_t)src[4 * ((size_t)y * w + bx[2 * x] + k) + c] * kx[x * nx + k];
      tmp[(size_t)y * pw + x] = clip8(s);
    }
  for (int y = 0; y < ph; y++)
    for (int x = 0; x < pw; x++) {
      int64_t s = 1 << (PREC - 1);
      for (int k = 0; k < by[2 * y + 1]; k++)
        s += (int64_t)tmp[(size_t)(by[2 * y] + k) * pw + x] * ky[y * ny + k];
      dst[4 * ((size_t)y * pw + x) + c] = clip8(s);
    }
  free(tmp), free(bx), free(by), free(kx), free(ky);
}

/* one 2 x 2 box level (a side of 1 stays) */
static u8 *mip(const u8 *a, int w, int h, int *ow, int *oh) {
  int nh = h > 1 ? h / 2 : 1, nw = w > 1 ? w / 2 : 1;
  u8 *t = malloc((size_t)w * nh * 4), *o = malloc((size_t)nw * nh * 4);
  for (int y = 0; y < nh; y++)
    for (int x = 0; x < w * 4; x++)
      t[(size_t)y * w * 4 + x] = h > 1 ? (u8)((a[(size_t)(2 * y) * w * 4 + x] + a[(size_t)(2 * y + 1) * w * 4 + x]) >> 1)
                                       : a[x];
  for (int y = 0; y < nh; y++)
    for (int x = 0; x < nw; x++)
      for (int c = 0; c < 4; c++)
        o[4 * ((size_t)y * nw + x) + c] =
            w > 1 ? (u8)((t[4 * ((size_t)y * w + 2 * x) + c] + t[4 * ((size_t)y * w + 2 * x + 1) + c]) >> 1)
                  : t[4 * (size_t)y * w + c];
  free(t);
  *ow = nw, *oh = nh;
  return o;
}

/* ---- the nearest palette entry (squared distance over ch channels; ties:
   the lowest index, as numpy's argmin) ---- */
struct nn {
  int n, ch;
  u8 pal[256][4];
  u8 order[256]; /* sorted by channel 0 */
  u8 key[256];
  /* a cache of the last answers */
  u32 cv[4096];
  u8 ci[4096];
  u8 cok[4096];
};
static void nn_init(struct nn *t, const u8 (*pal)[4], int n, int ch) {
  t->n = n, t->ch = ch;
  memcpy(t->pal, pal, (size_t)n * 4);
  for (int i = 0; i < n; i++)
    t->order[i] = (u8)i;
  for (int i = 1; i < n; i++) { /* insertion sort by channel 0, then index */
    u8 x = t->order[i];
    int j = i - 1;
    while (j >= 0 && t->pal[t->order[j]][0] > t->pal[x][0])
      t->order[j + 1] = t->order[j], j--;
    t->order[j + 1] = x;
  }
  for (int i = 0; i < n; i++)
    t->key[i] = t->pal[t->order[i]][0];
  memset(t->cok, 0, sizeof t->cok);
}
static int nn_find(struct nn *t, const u8 *p) {
  u32 v = p[0] | p[1] << 8 | p[2] << 16 | (t->ch == 4 ? (u32)p[3] << 24 : 0);
  u32 h = (v * 2654435761u) >> 20;
  if (t->cok[h] && t->cv[h] == v)
    return t->ci[h];
  /* from the entries nearest in channel 0 outwards, while that alone is no farther */
  int lo = 0, hi = t->n;
  while (lo < hi) {
    int m = (lo + hi) / 2;
    if (t->key[m] < p[0])
      lo = m + 1;
    else
      hi = m;
  }
  int best = -1;
  u32 bd = 0xFFFFFFFF;
  int up = lo, down = lo - 1;
  while (up < t->n || down >= 0) {
    int pick;
    u32 d0u = up < t->n ? (u32)(t->key[up] - p[0]) * (t->key[up] - p[0]) : 0xFFFFFFFF;
    u32 d0d = down >= 0 ? (u32)(p[0] - t->key[down]) * (p[0] - t->key[down]) : 0xFFFFFFFF;
    if (d0u <= d0d)
      pick = up++;
    else
      pick = down--;
    u32 d0 = d0u <= d0d ? d0u : d0d;
    if (d0 > bd)
      break;
    int e = t->order[pick];
    const u8 *q = t->pal[e];
    u32 d = d0;
    for (int c = 1; c < t->ch; c++) {
      int x = (int)p[c] - q[c];
      d += (u32)(x * x);
    }
    if (d < bd || (d == bd && e < best))
      bd = d, best = e;
  }
  t->cok[h] = 1, t->cv[h] = v, t->ci[h] = (u8)best;
  return best;
}

/* the squared distance from entry e to its nearest other entry (0xFFFFFFFF
   when it is alone): outwards from it in channel 0's order, as nn_find */
static u32 nn_apart(const struct nn *t, int e) {
  int at = 0;
  while (t->order[at] != e)
    at++;
  const u8 *p = t->pal[e];
  u32 bd = 0xFFFFFFFF;
  for (int dir = -1; dir <= 1; dir += 2)
    for (int j = at + dir; j >= 0 && j < t->n; j += dir) {
      int x = (int)t->key[j] - p[0];
      u32 d = (u32)(x * x);
      if (d >= bd)
        break;
      const u8 *q = t->pal[t->order[j]];
      for (int c = 1; c < t->ch; c++)
        d += (u32)((p[c] - q[c]) * (p[c] - q[c]));
      if (d < bd)
        bd = d;
    }
  return bd;
}

/* ---- Wu's quantizer (Xiaolin Wu, 1991) on RGB ---- */
#define WS 33
typedef struct {
  int r0, r1, g0, g1, b0, b1, vol;
} box;
struct wu {
  int64_t wt[WS * WS * WS], mr[WS * WS * WS], mg[WS * WS * WS], mb[WS * WS * WS], m2[WS * WS * WS];
};
#define IX(r, g, b) ((r) * WS * WS + (g) * WS + (b))
static int64_t vol(const box *c, const int64_t *m) {
  return m[IX(c->r1, c->g1, c->b1)] - m[IX(c->r1, c->g1, c->b0)] - m[IX(c->r1, c->g0, c->b1)] +
         m[IX(c->r1, c->g0, c->b0)] - m[IX(c->r0, c->g1, c->b1)] + m[IX(c->r0, c->g1, c->b0)] +
         m[IX(c->r0, c->g0, c->b1)] - m[IX(c->r0, c->g0, c->b0)];
}
static int64_t bottom(const box *c, int dir, const int64_t *m) {
  switch (dir) {
  case 0:
    return -m[IX(c->r0, c->g1, c->b1)] + m[IX(c->r0, c->g1, c->b0)] + m[IX(c->r0, c->g0, c->b1)] - m[IX(c->r0, c->g0, c->b0)];
  case 1:
    return -m[IX(c->r1, c->g0, c->b1)] + m[IX(c->r1, c->g0, c->b0)] + m[IX(c->r0, c->g0, c->b1)] - m[IX(c->r0, c->g0, c->b0)];
  default:
    return -m[IX(c->r1, c->g1, c->b0)] + m[IX(c->r1, c->g0, c->b0)] + m[IX(c->r0, c->g1, c->b0)] - m[IX(c->r0, c->g0, c->b0)];
  }
}
static int64_t top(const box *c, int dir, int pos, const int64_t *m) {
  switch (dir) {
  case 0:
    return m[IX(pos, c->g1, c->b1)] - m[IX(pos, c->g1, c->b0)] - m[IX(pos, c->g0, c->b1)] + m[IX(pos, c->g0, c->b0)];
  case 1:
    return m[IX(c->r1, pos, c->b1)] - m[IX(c->r1, pos, c->b0)] - m[IX(c->r0, pos, c->b1)] + m[IX(c->r0, pos, c->b0)];
  default:
    return m[IX(c->r1, c->g1, pos)] - m[IX(c->r1, c->g0, pos)] - m[IX(c->r0, c->g1, pos)] + m[IX(c->r0, c->g0, pos)];
  }
}
static u64 sq3(int64_t r, int64_t g, int64_t b) {
  return (u64)(u32)r * (u32)r + (u64)(u32)g * (u32)g + (u64)(u32)b * (u32)b;
}
static double var(const struct wu *w, const box *c) {
  double dr = (double)vol(c, w->mr), dg = (double)vol(c, w->mg), db = (double)vol(c, w->mb);
  double xx = (double)vol(c, w->m2), wt = (double)vol(c, w->wt);
  return wt > 0 ? xx - (dr * dr + dg * dg + db * db) / wt : 0;
}
static double maximize(const struct wu *w, const box *c, int dir, int first, int last, int *cut, int64_t wr,
                       int64_t wg, int64_t wb, int64_t ww) {
  int64_t br = bottom(c, dir, w->mr), bg = bottom(c, dir, w->mg), bb = bottom(c, dir, w->mb), bw = bottom(c, dir, w->wt);
  double max = 0;
  *cut = -1;
  for (int i = first; i < last; i++) {
    int64_t hr = br + top(c, dir, i, w->mr), hg = bg + top(c, dir, i, w->mg), hb = bb + top(c, dir, i, w->mb);
    int64_t hw = bw + top(c, dir, i, w->wt);
    if (!hw)
      continue;
    /* the sums are below 2^32 and their squares' sum below 2^53: in
       integers, exact, as the doubles were (the EE has no double FPU) */
    double t = (double)sq3(hr, hg, hb) / (double)hw;
    hr = wr - hr, hg = wg - hg, hb = wb - hb, hw = ww - hw;
    if (!hw)
      continue;
    t += (double)sq3(hr, hg, hb) / (double)hw;
    if (t > max)
      max = t, *cut = i;
  }
  return max;
}
static int wu_cut(const struct wu *w, box *s1, box *s2) {
  int64_t wr = vol(s1, w->mr), wg = vol(s1, w->mg), wb = vol(s1, w->mb), ww = vol(s1, w->wt);
  int cr, cg, cb;
  double mr = maximize(w, s1, 0, s1->r0 + 1, s1->r1, &cr, wr, wg, wb, ww);
  double mg = maximize(w, s1, 1, s1->g0 + 1, s1->g1, &cg, wr, wg, wb, ww);
  double mb = maximize(w, s1, 2, s1->b0 + 1, s1->b1, &cb, wr, wg, wb, ww);
  int dir;
  if (mr >= mg && mr >= mb) {
    dir = 0;
    if (cr < 0)
      return 0;
  } else
    dir = mg >= mr && mg >= mb ? 1 : 2;
  s2->r1 = s1->r1, s2->g1 = s1->g1, s2->b1 = s1->b1;
  if (dir == 0)
    s2->r0 = s1->r1 = cr, s2->g0 = s1->g0, s2->b0 = s1->b0;
  else if (dir == 1)
    s2->g0 = s1->g1 = cg, s2->r0 = s1->r0, s2->b0 = s1->b0;
  else
    s2->b0 = s1->b1 = cb, s2->r0 = s1->r0, s2->g0 = s1->g0;
  s1->vol = (s1->r1 - s1->r0) * (s1->g1 - s1->g0) * (s1->b1 - s1->b0);
  s2->vol = (s2->r1 - s2->r0) * (s2->g1 - s2->g0) * (s2->b1 - s2->b0);
  return 1;
}

/* k colours (RGB, alpha 128) for the texels: Wu's cut, then k-means; the
   number made in *got. The texels as their distinct colours, each with its
   count (and the first texel that has it): the sums are the same */
struct uc {
  u8 c[4];
  u32 n, first;
};
static struct wu wu_mem; /* 1.4 MB: one at a time */
static void wu_palette(const struct uc *px, u32 npx, int k, u8 (*pal)[4], int *got) {
  struct wu *w = &wu_mem;
  memset(w, 0, sizeof *w);
  for (u32 i = 0; i < npx; i++) {
    const u8 *p = px[i].c;
    int64_t n = px[i].n;
    int ix = IX((p[0] >> 3) + 1, (p[1] >> 3) + 1, (p[2] >> 3) + 1);
    w->wt[ix] += n, w->mr[ix] += p[0] * n, w->mg[ix] += p[1] * n, w->mb[ix] += p[2] * n;
    w->m2[ix] += (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) * n;
  }
  /* the cumulative moments */
  for (int r = 1; r < WS; r++) {
    int64_t a[WS] = {0}, ar[WS] = {0}, ag[WS] = {0}, ab[WS] = {0}, a2[WS] = {0};
    for (int g = 1; g < WS; g++) {
      int64_t l = 0, lr = 0, lg = 0, lb = 0, l2 = 0;
      for (int b = 1; b < WS; b++) {
        int i1 = IX(r, g, b), i2 = IX(r - 1, g, b);
        l += w->wt[i1], lr += w->mr[i1], lg += w->mg[i1], lb += w->mb[i1], l2 += w->m2[i1];
        a[b] += l, ar[b] += lr, ag[b] += lg, ab[b] += lb, a2[b] += l2;
        w->wt[i1] = w->wt[i2] + a[b], w->mr[i1] = w->mr[i2] + ar[b], w->mg[i1] = w->mg[i2] + ag[b];
        w->mb[i1] = w->mb[i2] + ab[b], w->m2[i1] = w->m2[i2] + a2[b];
      }
    }
  }
  box cube[256];
  double vv[256];
  cube[0] = (box){0, 32, 0, 32, 0, 32, 32 * 32 * 32};
  int next = 0, n = k;
  for (int i = 1; i < k; i++) {
    if (wu_cut(w, &cube[next], &cube[i])) {
      vv[next] = cube[next].vol > 1 ? var(w, &cube[next]) : 0;
      vv[i] = cube[i].vol > 1 ? var(w, &cube[i]) : 0;
    } else {
      vv[next] = 0;
      i--;
    }
    next = 0;
    double t = vv[0];
    for (int j = 1; j <= i; j++)
      if (vv[j] > t)
        t = vv[j], next = j;
    if (t <= 0) {
      n = i + 1;
      break;
    }
  }
  int m = 0;
  for (int i = 0; i < n; i++) {
    int64_t wt = vol(&cube[i], w->wt);
    if (!wt)
      continue;
    pal[m][0] = (u8)((vol(&cube[i], w->mr) + wt / 2) / wt), pal[m][1] = (u8)((vol(&cube[i], w->mg) + wt / 2) / wt);
    pal[m][2] = (u8)((vol(&cube[i], w->mb) + wt / 2) / wt), pal[m][3] = 128;
    m++;
  }
  *got = m;
}

/* k-means rounds over ch channels: each entry the mean of its texels */
static struct nn nn_mem;
static u32 dist2(const u8 *p, const u8 *q, int ch) {
  u32 d = 0;
  for (int c = 0; c < ch; c++)
    d += (u32)((p[c] - q[c]) * (p[c] - q[c]));
  return d;
}
static void kmeans(const struct uc *px, u32 npx, u8 (*pal)[4], int n, int ch, int rounds) {
  static int64_t sum[256][4];
  static u32 cnt[256], apart[256];
  /* each colour's entry of the round before: still its only nearest when
     four times its distance is below the entry's distance to its nearest
     other one, squared (the triangle inequality), so no search */
  s16 *was = malloc((size_t)(npx ? npx : 1) * sizeof *was);
  for (u32 i = 0; i < npx; i++)
    was[i] = -1;
  for (int it = 0; it < rounds; it++) {
    nn_init(&nn_mem, (const u8(*)[4])pal, n, ch);
    for (int e = 0; e < n; e++)
      apart[e] = nn_apart(&nn_mem, e);
    memset(sum, 0, sizeof sum), memset(cnt, 0, sizeof cnt);
    for (u32 i = 0; i < npx; i++) {
      int e = was[i];
      if (e < 0 || (u64)dist2(px[i].c, pal[e], ch) * 4 >= apart[e])
        e = was[i] = (s16)nn_find(&nn_mem, px[i].c);
      for (int c = 0; c < ch; c++)
        sum[e][c] += (int64_t)px[i].c[c] * px[i].n;
      cnt[e] += px[i].n;
    }
    for (int e = 0; e < n; e++)
      if (cnt[e])
        for (int c = 0; c < ch; c++)
          pal[e][c] = (u8)((sum[e][c] + cnt[e] / 2) / cnt[e]);
  }
  free(was);
}

/* n colours for the texels (w x h x 4): Wu and k-means in RGB; with alpha,
   the entries left to the texels farthest from the ones there are and
   k-means in RGBA (as the Python tool after libimagequant). The count in *got */
static void palette(const struct uc *a, u32 npx, int alpha, int n, u8 (*pal)[4], int *got) {
  int m;
  wu_palette(a, npx, n, pal, &m);
  kmeans(a, npx, pal, m, 3, KMEANS);
  if (alpha && m < n) {
    /* each texel's distance to its nearest entry, brought down by each
       entry added: one pass an entry, no search */
    /* the farthest texel: the first in the image of those as far */
    u32 *dist = malloc((size_t)npx * sizeof *dist);
    nn_init(&nn_mem, (const u8(*)[4])pal, m, 4);
    u32 bd = 0, bi = 0;
    for (u32 i = 0; i < npx; i++) {
      const u8 *p = a[i].c, *q = pal[nn_find(&nn_mem, p)];
      u32 d = 0;
      for (int c = 0; c < 4; c++)
        d += (u32)((p[c] - q[c]) * (p[c] - q[c]));
      dist[i] = d;
      if (d > bd || (d == bd && d && a[i].first < a[bi].first))
        bd = d, bi = i;
    }
    while (m < n && bd) {
      u8 q[4];
      memcpy(q, a[bi].c, 4);
      memcpy(pal[m++], q, 4);
      bd = 0, bi = 0;
      for (u32 i = 0; i < npx; i++) {
        const u8 *p = a[i].c;
        int d0 = p[0] - q[0], d1 = p[1] - q[1], d2 = p[2] - q[2], d3 = p[3] - q[3];
        u32 d = (u32)(d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3);
        if (d < dist[i])
          dist[i] = d;
        d = dist[i];
        if (d > bd || (d == bd && d && a[i].first < a[bi].first))
          bd = d, bi = i;
      }
    }
    free(dist);
  }
  if (alpha)
    kmeans(a, npx, pal, m, 4, 12);
  *got = m;
}

/* ---- one image ---- */

/* the .p8 (malloc'd) of an image's file; its level 0's squared error in *mse */
static u8 *convert(const u8 *data, u32 n, int is_tga, int maxside, u32 *outn, double *mse) {
  struct img im;
  if ((is_tga ? tga : jpg)(data, n, &im))
    return 0;
  int width = im.w, height = im.h, alpha = 0;
  for (int i = 0; i < width * height && !alpha; i++)
    alpha = im.p[4 * i + 3] != 255;
  int pw = floor_pow2(width), ph = floor_pow2(height), w0 = pw, h0 = ph;
  while (w0 > maxside || h0 > maxside)
    w0 = w0 / 2 > 1 ? w0 / 2 : 1, h0 = h0 / 2 > 1 ? h0 / 2 : 1;
  u8 *a = im.p;
  int aw = width, ah = height;
  if (pw != width || ph != height) {
    u8 *r = malloc((size_t)pw * ph * 4);
    for (int c = 0; c < 4; c++)
      box_resize(im.p, width, height, r, pw, ph, c);
    free(im.p);
    a = r, aw = pw, ah = ph;
  }
  while (aw > w0 || ah > h0) {
    u8 *m = mip(a, aw, ah, &aw, &ah);
    free(a);
    a = m;
  }
  int chans = alpha ? 4 : 3;
  u32 npx = (u32)aw * ah;
  /* its distinct colours (alpha left out without alpha) in byte order,
     with their counts and first texels: a stable radix sort */
  u32 nu = 0;
  struct uc *uc = malloc((size_t)npx * sizeof *uc);
  u32 *uix = malloc((size_t)npx * sizeof *uix); /* each texel's colour in uc */
  {
    u32 *k0 = malloc((size_t)npx * 8), *k1 = malloc((size_t)npx * 8);
    for (u32 i = 0; i < npx; i++) {
      const u8 *p = a + 4 * i;
      k0[2 * i] = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | (alpha ? p[3] : 0), k0[2 * i + 1] = i;
    }
    for (int sh = alpha ? 0 : 8; sh < 32; sh += 8) {
      u32 cnt[257] = {0};
      for (u32 i = 0; i < npx; i++)
        cnt[(k0[2 * i] >> sh & 255) + 1]++;
      for (int b = 0; b < 256; b++)
        cnt[b + 1] += cnt[b];
      for (u32 i = 0; i < npx; i++) {
        u32 at = cnt[k0[2 * i] >> sh & 255]++;
        k1[2 * at] = k0[2 * i], k1[2 * at + 1] = k0[2 * i + 1];
      }
      u32 *x = k0;
      k0 = k1, k1 = x;
    }
    for (u32 i = 0; i < npx; i++) {
      u32 key = k0[2 * i];
      if (!nu || key != k0[2 * (i - 1)]) {
        struct uc *u = &uc[nu++];
        u->c[0] = (u8)(key >> 24), u->c[1] = (u8)(key >> 16), u->c[2] = (u8)(key >> 8), u->c[3] = (u8)key;
        u->n = 0, u->first = k0[2 * i + 1];
      }
      uc[nu - 1].n++;
      uix[k0[2 * i + 1]] = nu - 1;
    }
    free(k0), free(k1);
  }
  /* its own colours, when there are 256 or fewer: exact */
  static u8 pal[256][4];
  int np = 0;
  if (nu <= 256) {
    for (u32 i = 0; i < nu; i++)
      memcpy(pal[i], uc[i].c, 4);
    np = (int)nu;
  } else
    palette(uc, nu, alpha, 256, pal, &np);
  static struct nn t;
  /* 16 colours, when they keep level 0 within T4DB dB */
  if (np > 16) {
    static u8 p16[256][4];
    int n16;
    palette(uc, nu, alpha, 16, p16, &n16);
    nn_init(&t, (const u8(*)[4])p16, n16, chans);
    u64 se = 0;
    for (u32 i = 0; i < nu; i++) {
      const u8 *p = uc[i].c, *q = p16[nn_find(&t, p)];
      u32 d = 0;
      for (int c = 0; c < chans; c++)
        d += (u32)((p[c] - q[c]) * (p[c] - q[c]));
      se += (u64)d * uc[i].n;
    }
    double m = (double)se / ((double)npx * chans);
    if (m == 0 || 10 * log10(255.0 * 255.0 / m) >= T4DB) {
      memcpy(pal, p16, sizeof p16);
      np = n16;
    }
  }
  int bits = np <= 16 ? 4 : 8, ncol = bits == 4 ? 16 : 256;
  nn_init(&t, (const u8(*)[4])pal, np, chans);
  /* level 0's entries by its colours, and its error */
  u8 *ue = malloc(nu ? nu : 1);
  u64 se = 0;
  for (u32 i = 0; i < nu; i++) {
    ue[i] = (u8)nn_find(&t, uc[i].c);
    se += (u64)dist2(uc[i].c, pal[ue[i]], chans) * uc[i].n;
  }
  free(uc);
  /* the levels' size */
  u32 total = 24 + (u32)(ncol * chans);
  int nlev = 0;
  for (int w = aw, h = ah;; nlev++) {
    u32 k = (u32)w * h;
    total += bits == 4 ? (k + 1) / 2 : k;
    if (w == 1 && h == 1) {
      nlev++;
      break;
    }
    w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1;
  }
  u8 *out = calloc(1, total);
  u8 *o = out;
  memcpy(o, "P8T1", 4);
  u16 hv[9] = {(u16)width, (u16)height, (u16)pw, (u16)ph, (u16)aw, (u16)ah, (u16)nlev, 0};
  for (int i = 0; i < 8; i++)
    o[4 + 2 * i] = (u8)hv[i], o[5 + 2 * i] = (u8)(hv[i] >> 8);
  u32 fmt = bits == 4 ? (alpha ? GL_PALETTE4_RGBA8_OES : GL_PALETTE4_RGB8_OES)
                      : (alpha ? GL_PALETTE8_RGBA8_OES : GL_PALETTE8_RGB8_OES);
  o[20] = (u8)fmt, o[21] = (u8)(fmt >> 8), o[22] = (u8)(fmt >> 16), o[23] = (u8)(fmt >> 24);
  o += 24;
  for (int i = 0; i < np; i++)
    memcpy(o + i * chans, pal[i], (size_t)chans);
  o += ncol * chans;
  /* the levels */
  u8 *lv = a;
  int lw = aw, lh = ah;
  for (int l = 0; l < nlev; l++) {
    u32 k = (u32)lw * lh;
    for (u32 i = 0; i < k; i++) {
      int e = l == 0 ? ue[uix[i]] : nn_find(&t, lv + 4 * i);
      if (bits == 4)
        o[i / 2] |= (u8)(i & 1 ? e : e << 4);
      else
        o[i] = (u8)e;
    }
    o += bits == 4 ? (k + 1) / 2 : k;
    if (l + 1 < nlev) {
      u8 *m = mip(lv, lw, lh, &lw, &lh);
      if (lv != a)
        free(lv);
      lv = m;
    }
  }
  if (lv != a)
    free(lv);
  free(a), free(ue), free(uix);
  *mse = (double)se / ((double)npx * chans);
  *outn = total;
  return out;
}

/* the pictures that fill the screen: the menus' background (menuback's
   map), the levels' pictures and the one for a level without */
static int full_screen(const char *n) {
  return !strncmp(n, "textures/sfx/logo512.", 21) || !strncmp(n, "levelshots/", 11) ||
         !strncmp(n, "menu/art/unknownmap.", 20);
}

static int two_d(const char *n) {
  return !strncmp(n, "gfx/2d/", 7) || !strncmp(n, "menu/", 5) || !strncmp(n, "ui/", 3) || !strncmp(n, "icons/", 6);
}

int cv_pak(const char *out_pk3, const char *gamedir, int maxside) {
  pk3_set *s = pk3_open(gamedir);
  if (!s)
    return -1;
  /* the images by name without extension: a later pak's, in one pak the .tga */
  int cnt = pk3_count(s), nimg = 0;
  int *pick = malloc((size_t)(cnt ? cnt : 1) * sizeof *pick);
  for (int i = 0; i < cnt; i++) {
    const char *nm = pk3_name(s, i);
    u32 l = (u32)strlen(nm);
    int tga_ = l > 4 && !strcmp(nm + l - 4, ".tga"), jpg_ = l > 4 && !strcmp(nm + l - 4, ".jpg");
    if (!tga_ && !jpg_)
      continue;
    if (jpg_) {
      /* the .tga of the same name, when it is there and from this pak or a later one, wins */
      char t[260];
      snprintf(t, sizeof t, "%.*s.tga", (int)(l - 4), nm);
      int k = pk3_find(s, t);
      if (k >= 0 && pk3_pak_of(s, k) >= pk3_pak_of(s, i))
        continue;
    } else {
      char j[260];
      snprintf(j, sizeof j, "%.*s.jpg", (int)(l - 4), nm);
      int k = pk3_find(s, j);
      if (k >= 0 && pk3_pak_of(s, k) > pk3_pak_of(s, i))
        continue;
    }
    pick[nimg++] = i;
  }
  pk3_out *o = pk3_create(out_pk3);
  if (!o) {
    free(pick);
    pk3_close(s);
    return -1;
  }
  int r = 0, made = 0, small = 0;
  double sum = 0;
  u32 bytes = 0;
  for (int k = 0; k < nimg && !r; k++) {
    if (cv_cancel) {
      r = -1;
      break;
    }
    cv_progress("textures", (u32)k, (u32)nimg);
    const char *nm = pk3_name(s, pick[k]);
    u32 l = (u32)strlen(nm), n, on;
    u8 *d = pk3_read(s, pick[k], &n);
    double mse;
    u8 *p8 = d ? convert(d, n, !strcmp(nm + l - 4, ".tga"), full_screen(nm) ? MAXFULL : two_d(nm) ? MAX2D : maxside, &on, &mse) : 0;
    free(d);
    if (!p8) {
      cv_log("skipped %s: not an image this reads", nm);
      continue;
    }
    char name[260];
    snprintf(name, sizeof name, "%.*s.p8", (int)(l - 4), nm);
    r = pk3_add(o, name, p8, on);
    u32 fmt = p8[20] | p8[21] << 8;
    small += fmt == GL_PALETTE4_RGB8_OES || fmt == GL_PALETTE4_RGBA8_OES;
    made++, bytes += on, sum += mse;
    free(p8);
  }
  cv_progress("textures", (u32)nimg, (u32)nimg);
  if (pk3_finish(o))
    r = -1;
  if (!r)
    cv_log("%d textures (%d of 16 colours), %u.%u MiB; mean squared error of level 0: %.2f", made, small,
           bytes >> 20, ((bytes & 0xFFFFF) * 10) >> 20, made ? sum / made : 0.0);
  free(pick);
  pk3_close(s);
  return r;
}
