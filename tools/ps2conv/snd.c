/* The sounds as SPU ADPCM (was tools/ps2snd.py): ps2snd.pk3 holds for each
   .wav under sound/ a <name>.adp. The PS2's sound code (code/ps2/snd_ps2.c,
   s_ps2hw 1) takes a sound's .adp before the .wav: the SPU2's voices play it
   as it is, so it need not be encoded when it is loaded, and the file is a
   third and a half of the 16-bit one. The music (music/) stays as it is: it
   is streamed.

   .adp layout (little-endian):
       char  magic[4]   "ADP1"
       u32   rate       Hz
       u32   samples    of the sound (mono; stereo files are mixed)
       then the ADPCM: blocks of 16 bytes for 28 samples each (shift and
       filter, flags, 28 nibbles), the last one flagged as the end, as
       maxsdk's mx_snd_encode makes them (ee/src/base/snd.c). */
#include "conv.h"

#include <stdlib.h>
#include <string.h>

static const int FILT[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

/* the block's 14 data bytes for filter f and shift sh: its squared error,
   and the decoder's last two samples */
static int64_t try_block(const int *x, int f, int sh, int h1, int h2, u8 *b, int *o1, int *o2) {
  int f0 = FILT[f][0], f1 = FILT[f][1];
  int step = 1 << (12 - sh), half = step >> 1;
  int s1 = h1, s2 = h2;
  int64_t err = 0;
  memset(b, 0, 14);
  for (int i = 0; i < 28; i++) {
    int pred = (s1 * f0 + s2 * f1 + 32) >> 6;
    int r = x[i] - pred;
    int q = r >= 0 ? (r + half) / step : -((half - r) / step);
    q = q < -8 ? -8 : q > 7 ? 7 : q;
    int d = q * step + pred;
    d = d < -32768 ? -32768 : d > 32767 ? 32767 : d;
    int e = x[i] - d;
    err += (int64_t)e * e;
    s2 = s1, s1 = d;
    b[i >> 1] |= (u8)((q & 15) << (i & 1 ? 4 : 0));
  }
  *o1 = s1, *o2 = s2;
  return err;
}

/* 16-bit mono samples as SPU ADPCM, played once: ((n + 27) / 28) * 16 bytes */
static void encode(const int *pcm, u32 n, u8 *out) {
  u32 blocks = (n + 27) / 28;
  int h1 = 0, h2 = 0;
  for (u32 k = 0; k < blocks; k++) {
    int x[28];
    for (int i = 0; i < 28; i++)
      x[i] = k * 28 + i < n ? pcm[k * 28 + i] : 0;
    int64_t best = -1;
    u8 head = 0, bb[14];
    int b1 = 0, b2 = 0;
    for (int f = 0; f < 5; f++) {
      int f0 = FILT[f][0], f1 = FILT[f][1];
      /* the shift for the largest residual, predicted from the source */
      int p1 = h1, p2 = h2, m = 0;
      for (int i = 0; i < 28; i++) {
        int r = x[i] - ((p1 * f0 + p2 * f1 + 32) >> 6);
        if (r < 0)
          r = -r;
        if (r > m)
          m = r;
        p2 = p1, p1 = x[i];
      }
      int sh = 12;
      while (sh > 0 && (7 << (12 - sh)) < m)
        sh--;
      for (int s = sh; s >= sh - 1 && s >= 0; s--) {
        u8 b[14];
        int t1, t2;
        int64_t err = try_block(x, f, s, h1, h2, b, &t1, &t2);
        if (best < 0 || err < best) {
          best = err, head = (u8)(f << 4 | s), b1 = t1, b2 = t2;
          memcpy(bb, b, 14);
        }
      }
    }
    h1 = b1, h2 = b2;
    out[k * 16] = head;
    out[k * 16 + 1] = 0;
    memcpy(out + k * 16 + 2, bb, 14);
  }
  out[(blocks - 1) * 16 + 1] |= 1; /* the end: the voice is released */
}

static u32 rd16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void put32(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }

/* a PCM .wav as its .adp (malloc'd), or 0 */
static u8 *convert(const u8 *d, u32 len, u32 *outn) {
  if (len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4))
    return 0;
  const u8 *fmt = 0, *body = 0;
  u32 bsize = 0;
  for (u32 at = 12; at + 8 <= len;) {
    u32 size = rd32(d + at + 4);
    if (!memcmp(d + at, "fmt ", 4) && at + 8 + 16 <= len)
      fmt = d + at + 8;
    else if (!memcmp(d + at, "data", 4)) {
      body = d + at + 8;
      bsize = size;
      if (at + 8 > len)
        bsize = 0;
      else if (bsize > len - (at + 8))
        bsize = len - (at + 8); /* as Python's slice: what is there */
    }
    if (size > len)
      break;
    at += 8 + size + (size & 1);
  }
  if (!fmt || !body || rd16(fmt) != 1)
    return 0;
  u32 channels = rd16(fmt + 2), rate = rd32(fmt + 4), bits = rd16(fmt + 14), width = bits / 8;
  if ((width != 1 && width != 2) || channels < 1)
    return 0;
  u32 n = bsize / (width * channels);
  if (!n)
    return 0;
  int *pcm = malloc(n * sizeof *pcm);
  if (!pcm)
    return 0;
  for (u32 i = 0; i < n; i++) {
    int sum = 0;
    for (u32 c = 0; c < channels; c++) {
      const u8 *s = body + (i * channels + c) * width;
      sum += width == 2 ? (int)(s16)rd16(s) : ((int)s[0] - 128) << 8;
    }
    /* the average, rounded towards zero */
    pcm[i] = sum / (int)channels;
  }
  u32 blocks = (n + 27) / 28;
  u8 *out = malloc(12 + blocks * 16);
  if (out) {
    memcpy(out, "ADP1", 4);
    put32(out + 4, rate), put32(out + 8, n);
    encode(pcm, n, out + 12);
    *outn = 12 + blocks * 16;
  }
  free(pcm);
  return out;
}

int cv_snd(const char *out_pk3, const char *gamedir) {
  pk3_set *s = pk3_open(gamedir);
  if (!s)
    return -1;
  pk3_out *o = pk3_create(out_pk3);
  if (!o) {
    pk3_close(s);
    return -1;
  }
  int total = 0;
  for (int i = 0; i < pk3_count(s); i++) {
    const char *nm = pk3_name(s, i);
    u32 l = (u32)strlen(nm);
    total += !strncmp(nm, "sound/", 6) && l > 4 && !strcmp(nm + l - 4, ".wav");
  }
  int r = 0, done = 0, made = 0;
  u32 src = 0, dst = 0;
  for (int i = 0; i < pk3_count(s) && !r; i++) {
    const char *nm = pk3_name(s, i);
    u32 l = (u32)strlen(nm);
    if (strncmp(nm, "sound/", 6) || l <= 4 || strcmp(nm + l - 4, ".wav"))
      continue;
    if (cv_cancel) {
      r = -1;
      break;
    }
    cv_progress("sounds", (u32)done++, (u32)total);
    u32 n, on;
    u8 *d = pk3_read(s, i, &n);
    u8 *a = d ? convert(d, n, &on) : 0;
    if (!a)
      cv_log("skipped (not PCM): %s", nm);
    else {
      char name[256];
      memcpy(name, nm, l - 4);
      memcpy(name + l - 4, ".adp", 5);
      r = pk3_add(o, name, a, on);
      src += n, dst += on, made++;
    }
    free(a);
    free(d);
  }
  cv_progress("sounds", (u32)total, (u32)total);
  if (pk3_finish(o))
    r = -1;
  pk3_close(s);
  if (!r)
    cv_log("%d sounds: %u KB of .wav, %u KB of .adp", made, src / 1024, dst / 1024);
  return r;
}
