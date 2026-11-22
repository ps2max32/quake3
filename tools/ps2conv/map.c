/* A Quake 3 map as the PS2 renderer draws it (PLAN-PS2.md, 5.2; was
   tools/ps2map.py, whose output this makes byte for byte).

     cv_map(out.pmap, gamedir, map, ps2tex.pk3)

   Reads maps/MAP.bsp and the shader scripts from the game's pak*.pk3 (a
   later pak's file replaces an earlier one's) and the textures' sizes from
   ps2tex.pk3 (pak.c), and writes OUT.pmap: the map's drawn surfaces as
   chunks VU1 takes as they are. The textures themselves are the renderer's
   file of images (bin.c), found there by name. The file is mapped and locked
   on the PS2; the DMAC reads the chunks where they lie, so everything it
   reads is 16-byte aligned.

   What is made of the BSP:
   - Curves (patches) are tessellated (a chord at most CURVE units off the
     curve). Edges longer than EDGE units are halved (the same on both sides
     of an edge, so no cracks): dynamic lights are per vertex, and a
     triangle stays inside the guard band longer. Triangles whose texture
     repeats more than 3.5 times are halved too: texture coordinates are
     4.12 fixed point.
   - Surfaces of one shader, lightmap page and fog are gathered and cut by
     place into groups of about CHUNK strip vertices within EXTENT units: a
     group is in a view or not by its bounds and by the PVS clusters it is
     seen from. A group's triangles of one direction (a flat surface's
     normal; of a curve's triangles the axis they face most) are a chunk,
     drawn or left out as a whole: left out when all of it faces away, by
     the cone of its normals (the GS draws both sides of a triangle).
   - A chunk's triangles become strips, cut into batches of at most BATCH
     vertices: what one VU1 buffer holds.
   - A lightmapped vertex's colour is the hue of the lightmap at it (its
     largest channel made 1.0); the lightmap pages keep the brightness (T8).
     Other vertices keep the BSP's vertex light.
   - The lightmaps' rectangles are packed again by place (pages of 64 x 64,
     128 x 128 for larger ones), so a view draws with few pages.

   File layout (little-endian; "off" are bytes from the file's start):

     head (128 bytes):
       u32 magic "PMAP", version 11
       u32 nmat, ntex, nchunk, nmodel, nlightmap, ncluster, vis_bytes
       u32 nplane, nnode, nleaf
       u32 off_mat, off_tex, off_chunk, off_model, off_lightmap, off_vis,
           off_plane, off_node, off_leaf, off_data, size
       f32 pos_scale          world units a position unit
       u32 batch              BATCH
       u32 ngroup, off_group
       u32 off_cvis, cvis_bytes
       u32 off_lmdim
       u32 nfog, off_fog      the fog volumes, 64 bytes each: f32 colour[3],
                              depth at which it is opaque; f32 plane[4] of
                              its top (n . p - d > 0 inside), u32 1 when it
                              has one; f32 mins[3], maxs[3]; a chunk's fog
                              is its number here, 255: none
     material (96 bytes): char name[64]; s16 tex (-1: none); u16 flags
       (MAT_*); u8 blend (BLEND_*), alpha (ALPHA_*), cull (CULL_*), pad;
       u32 surf, cont (the BSP's flags); f32 texels (of its texture's first
       level a world unit, for the mipmap level); pad to 96
     texture (72 bytes): char name[64] (the image's in the file of images);
       u16 w, h; u8 alpha (its palette has alpha of its own), pad[3]
     chunk (48 bytes): f32 mins[3]; u16 mat; s16 lightmap (-1: none);
       f32 maxs[3]; u16 nverts; u8 flags (CH_*), fog; u32 off_data; u16 qwc
       (quadwords the DMAC sends for it, tags included), nbatch; s8 cone[3]
       (the axis of its triangles' normals, length 127); u8 sine (of the
       widest angle between a normal and the axis, in 1/255; 255: no cone);
       u8 area (of the world's chunks: the area of the leafs it lies in, the
       room between doors; 255: in more than one, or none), pad[3]
     group (32 bytes): f32 mins[3]; u32 first_chunk; f32 maxs[3]; u32 nchunk
       (its chunks are of one material and lightmap page)
     model (32 bytes): f32 mins[3], maxs[3]; u32 first_group, ngroup (model
       0 is the world; the others the map's doors and platforms)
     lmdim: nlightmap pages: u16 w, h; u32 off (its texels in lightmap)
     lightmap: w x h bytes a page, T8: 128 + b, b the brightness (the
       largest of R, G, B; 127 and more: 127, twice the texture's colour,
       the map's overbright range). It is drawn with the CLUT of the
       surface's texture, whose entries 128 .. 255 have that brightness as
       their alpha (bin.c)
     vis: ncluster rows of vis_bytes: bit g of row k set: the world's group g
       is seen from cluster k
     cvis: ncluster rows of cvis_bytes: bit c of row k set: chunk c is seen
       from cluster k
     plane (16 bytes): f32 normal[3], dist;  node (12 bytes): s32 plane,
       child[2] (negative: -(leaf + 1));  leaf (4 bytes): s16 cluster, area

   A chunk's data is a DMA chain a frame CALLs: a CNT tag, the VIF codes and
   data of its batches, a RET tag. A batch (n vertices; the addresses are in
   a buffer of VU1's double buffering, VU_* below, shared with the programs):

       STCYCL cl 3 wl 1
       UNPACK V3-16  n -> VU_A + 0 + 3 i   texture s, t (4.12) and 4096
       UNPACK V4-8   n -> VU_A + 1 + 3 i   colour (unsigned)
       UNPACK V4-16  n -> VU_A + 2 + 3 i   position, and w: 0xA000 the vertex
                                           ends no triangle, 0x4000 its
                                           triangle is wound the other way
       UNPACK V3-16  n -> VU_IB + 0 + 3 i  lightmap s, t and 4096 (CH_LIGHTMAP)
       ITOP n, MSCNT

   Numbers: Python's round() is rint() here (half to even); what numpy did
   in float32 is done in double and rounded to float after each step (f32()),
   which is the same on the PC and on the EE (whose own FPU is not IEEE). */
#include "conv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- shared with the VU1 programs (code/renderer_ps2) ---- */
#define BATCH 46
#define VU_A 18
#define VU_IB 331
#define W_NONE 0xA000
#define W_REV 0x4000

enum { MAT_LIGHTMAP = 1, MAT_SKY = 2, MAT_NODRAW = 4, MAT_DEFORM = 8, MAT_TCMOD = 16, MAT_SCRIPT = 32,
       MAT_VERTEX = 64, MAT_ENV = 128, MAT_VLM = 256, MAT_ALIT = 512 };
enum { BLEND_NONE, BLEND_ADD, BLEND_ALPHA, BLEND_FILTER, BLEND_OTHER };
enum { ALPHA_NONE, ALPHA_GT0, ALPHA_LT128, ALPHA_GE128 };
enum { CULL_FRONT, CULL_BACK, CULL_NONE };
#define CH_LIGHTMAP 1
#define CH_SPRITE 2
#define SURF_SKY 0x4
#define SURF_NODRAW 0x80
enum { MST_PLANAR = 1, MST_PATCH = 2, MST_TRIANGLES = 3, MST_FLARE = 4 };
#define VIF_STCYCL 0x01
#define VIF_ITOP 0x04
#define VIF_MSCNT 0x17
#define UNPACK_V3_16 0x69
#define UNPACK_V4_16 0x6D
#define UNPACK_V4_8 0x6E
#define DMA_CNT 1
#define DMA_RET 6

/* the Python tool's defaults */
#define CURVE 4.0
#define EDGE 384.0
#define CHUNK 200
#define EXTENT 768.0
#define LEAST 6

static float f32(double x) { return (float)x; }
static double rnd(double x) { return rint(x); }

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static s32 rds32(const u8 *p) { return (s32)rd32(p); }
static float rdf(const u8 *p) {
  u32 v = rd32(p);
  float f;
  memcpy(&f, &v, 4);
  return f;
}

/* ---- growing arrays and a byte buffer ---- */
#define VEC(T)                                                                                             \
  struct {                                                                                                 \
    T *a;                                                                                                  \
    int n, cap;                                                                                            \
  }
#define PUSH(v, x)                                                                                         \
  do {                                                                                                     \
    if ((v).n == (v).cap) {                                                                                \
      (v).cap = (v).cap ? (v).cap * 2 : 16;                                                                \
      (v).a = realloc((v).a, (size_t)(v).cap * sizeof *(v).a);                                             \
    }                                                                                                      \
    (v).a[(v).n++] = (x);                                                                                  \
  } while (0)

typedef VEC(u8) bytes;
static void put(bytes *b, const void *d, u32 n) {
  while ((u32)b->n + n > (u32)b->cap) {
    b->cap = b->cap ? b->cap * 2 : 4096;
    b->a = realloc(b->a, (size_t)b->cap);
  }
  memcpy(b->a + b->n, d, n);
  b->n += (int)n;
}
static void put_u32(bytes *b, u32 v) {
  u8 x[4] = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)};
  put(b, x, 4);
}
static void put_u16(bytes *b, u32 v) {
  u8 x[2] = {(u8)v, (u8)(v >> 8)};
  put(b, x, 2);
}
static void put_u8(bytes *b, u32 v) {
  u8 x = (u8)v;
  put(b, &x, 1);
}
static void put_f(bytes *b, double v) {
  float f = f32(v);
  u32 u;
  memcpy(&u, &f, 4);
  put_u32(b, u);
}
static void put_zero(bytes *b, u32 n) {
  static const u8 z[64];
  while (n) {
    u32 k = n < 64 ? n : 64;
    put(b, z, k);
    n -= k;
  }
}
static void pad16(bytes *b) { put_zero(b, (u32)(-b->n & 15)); }
static void put_name(bytes *b, const char *s, u32 field) {
  u32 l = (u32)strlen(s);
  if (l > field - 1)
    l = field - 1;
  put(b, s, l);
  put_zero(b, field - l);
}
static void vif(bytes *b, u32 cmd, u32 num, u32 imm) { put_u32(b, cmd << 24 | (num & 255) << 16 | (imm & 0xFFFF)); }

/* sets of small integers (clusters, areas) as sorted arrays */
typedef VEC(int) iset;
static void iset_add(iset *s, int v) {
  int lo = 0, hi = s->n;
  while (lo < hi) {
    int m = (lo + hi) / 2;
    if (s->a[m] < v)
      lo = m + 1;
    else
      hi = m;
  }
  if (lo < s->n && s->a[lo] == v)
    return;
  PUSH(*s, 0);
  memmove(s->a + lo + 1, s->a + lo, (size_t)(s->n - 1 - lo) * sizeof(int));
  s->a[lo] = v;
}
static void iset_union(iset *s, const iset *o) {
  for (int i = 0; i < o->n; i++)
    iset_add(s, o->a[i]);
}

/* ---- shader scripts: what the map tool needs of them ---- */
/* a token: a string; "\n" a line's end; "{" and "}" */
typedef VEC(char *) toks;
struct sline {
  int first, n; /* tokens in the shader's token list */
};
struct shader {
  char *name;
  toks t;              /* its tokens */
  VEC(struct sline) lines;
  VEC(int) stage_start; /* stages: ranges of slines in stage_lines */
  VEC(struct sline) stage_lines;
  VEC(int) stage_n;
};

static int lowc(int c) {
  /* Python's str.lower() of a Latin-1 character */
  if (c >= 'A' && c <= 'Z')
    return c + 32;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
    return c + 32;
  return c;
}
static void lower_slash(char *s) {
  for (; *s; s++)
    *s = *s == '\\' ? '/' : (char)lowc((u8)*s);
}
static int ieq(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (lowc((u8)*a) != lowc((u8)*b))
      return 0;
  return !*a && !*b;
}

static char *dupn(const char *s, u32 n) {
  char *d = malloc(n + 1);
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}

static void tokens(const char *text, u32 n, toks *out) {
  u32 i = 0;
  while (i < n) {
    char c = text[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (c == '\n')
        PUSH(*out, dupn("\n", 1));
      i++;
    } else if (c == '/' && i + 1 < n && text[i + 1] == '/') {
      while (i < n && text[i] != '\n')
        i++;
    } else if (c == '/' && i + 1 < n && text[i + 1] == '*') {
      u32 j = i + 2;
      while (j + 1 < n && !(text[j] == '*' && text[j + 1] == '/'))
        j++;
      i = j + 1 < n ? j + 2 : n;
    } else if (c == '{' || c == '}') {
      PUSH(*out, dupn(&text[i], 1));
      i++;
    } else if (c == '"') {
      u32 j = i + 1;
      while (j < n && text[j] != '"')
        j++;
      PUSH(*out, dupn(text + i + 1, j - i - 1));
      i = j + 1;
    } else {
      u32 j = i;
      while (j < n && !strchr(" \t\r\n{}", text[j]))
        j++;
      PUSH(*out, dupn(text + i, j - i));
      i = j;
    }
  }
}

struct scripts {
  VEC(struct shader *) sh; /* sorted by name after reading */
};

static int sh_cmp(const void *a, const void *b) {
  return strcmp((*(struct shader *const *)a)->name, (*(struct shader *const *)b)->name);
}

static void parse_scripts(pk3_set *paks, struct scripts *out) {
  memset(out, 0, sizeof *out);
  for (int f = 0; f < pk3_count(paks); f++) {
    const char *fn = pk3_name(paks, f);
    u32 fl = (u32)strlen(fn);
    if (strncmp(fn, "scripts/", 8) || fl < 7 || strcmp(fn + fl - 7, ".shader"))
      continue;
    u32 len;
    u8 *text = pk3_read(paks, f, &len);
    if (!text)
      continue;
    toks t = {0};
    tokens((const char *)text, len, &t);
    free(text);
    int i = 0, n = t.n;
    while (i < n) {
      while (i < n && !strcmp(t.a[i], "\n"))
        i++;
      if (i >= n)
        break;
      char *name = strdup(t.a[i]);
      lower_slash(name);
      i++;
      while (i < n && !strcmp(t.a[i], "\n"))
        i++;
      if (i >= n || strcmp(t.a[i], "{")) {
        free(name);
        continue;
      }
      i++;
      struct shader *sh = calloc(1, sizeof *sh);
      sh->name = name;
      int depth = 1, in_stage = 0, line_first = -1, line_n = 0;
      int stage_first = 0;
#define END_LINE()                                                                                         \
  do {                                                                                                     \
    if (line_n) {                                                                                          \
      struct sline L = {line_first, line_n};                                                               \
      if (in_stage && depth == 2)                                                                          \
        PUSH(sh->stage_lines, L);                                                                          \
      else                                                                                                 \
        PUSH(sh->lines, L);                                                                                \
      line_n = 0;                                                                                          \
    }                                                                                                      \
  } while (0)
      while (i < n && depth) {
        const char *k = t.a[i++];
        if (!strcmp(k, "{")) {
          depth++;
          in_stage = 1, stage_first = sh->stage_lines.n;
          line_n = 0;
        } else if (!strcmp(k, "}")) {
          END_LINE();
          if (depth == 2 && in_stage) {
            PUSH(sh->stage_start, stage_first);
            PUSH(sh->stage_n, sh->stage_lines.n - stage_first);
            in_stage = 0;
          }
          depth--;
        } else if (!strcmp(k, "\n")) {
          END_LINE();
        } else {
          if (!line_n)
            line_first = sh->t.n;
          PUSH(sh->t, strdup(k));
          line_n++;
        }
      }
#undef END_LINE
      /* the first definition is the one used, as the engine */
      int dup = 0;
      for (int k = 0; k < out->sh.n && !dup; k++)
        dup = !strcmp(out->sh.a[k]->name, sh->name);
      if (dup) {
        for (int k = 0; k < sh->t.n; k++)
          free(sh->t.a[k]);
        free(sh->t.a), free(sh->lines.a), free(sh->stage_lines.a), free(sh->stage_start.a), free(sh->stage_n.a);
        free(sh->name), free(sh);
      } else
        PUSH(out->sh, sh);
    }
    for (int k = 0; k < t.n; k++)
      free(t.a[k]);
    free(t.a);
  }
  qsort(out->sh.a, (size_t)out->sh.n, sizeof *out->sh.a, sh_cmp);
}
static struct shader *script_get(struct scripts *s, const char *name) {
  int lo = 0, hi = s->sh.n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2, c = strcmp(s->sh.a[m]->name, name);
    if (!c)
      return s->sh.a[m];
    if (c < 0)
      lo = m + 1;
    else
      hi = m - 1;
  }
  return 0;
}
static void scripts_free(struct scripts *s) {
  for (int i = 0; i < s->sh.n; i++) {
    struct shader *sh = s->sh.a[i];
    for (int k = 0; k < sh->t.n; k++)
      free(sh->t.a[k]);
    free(sh->t.a), free(sh->lines.a), free(sh->stage_lines.a), free(sh->stage_start.a), free(sh->stage_n.a);
    free(sh->name), free(sh);
  }
  free(s->sh.a);
}
#define TOK(sh, L, k) ((sh)->t.a[(L).first + (k)])

static int blend_of(struct shader *sh, struct sline L) {
  const char *a0 = L.n > 1 ? TOK(sh, L, 1) : "", *a1 = L.n > 2 ? TOK(sh, L, 2) : 0;
  if (L.n > 1 && (ieq(a0, "add") || (a1 && ieq(a0, "gl_one") && ieq(a1, "gl_one"))))
    return BLEND_ADD;
  if (L.n > 1 && (ieq(a0, "blend") || (a1 && ieq(a0, "gl_src_alpha") && ieq(a1, "gl_one_minus_src_alpha"))))
    return BLEND_ALPHA;
  if (L.n > 1 && (ieq(a0, "filter") || (a1 && ((ieq(a0, "gl_dst_color") && ieq(a1, "gl_zero")) ||
                                               (ieq(a0, "gl_zero") && ieq(a1, "gl_src_color"))))))
    return BLEND_FILTER;
  return BLEND_OTHER;
}

/* (texture name or 0 (malloc'd), flags, blend, alpha, cull) */
static char *material_of(const char *name, struct shader *sh, int lightmapped, int *flags_, int *blend_,
                         int *alpha_, int *cull_) {
  int flags = 0, blend = BLEND_NONE, alpha = ALPHA_NONE, cull = CULL_FRONT;
  if (!sh) {
    *flags_ = lightmapped ? MAT_LIGHTMAP : MAT_VERTEX, *blend_ = blend, *alpha_ = alpha, *cull_ = cull;
    return strdup(name);
  }
  flags |= MAT_SCRIPT;
  for (int i = 0; i < sh->lines.n; i++) {
    struct sline L = sh->lines.a[i];
    const char *k = TOK(sh, L, 0);
    if (ieq(k, "cull") && L.n > 1) {
      const char *v = TOK(sh, L, 1);
      cull = ieq(v, "none") || ieq(v, "twosided") || ieq(v, "disable")       ? CULL_NONE
             : ieq(v, "back") || ieq(v, "backside") || ieq(v, "backsided") ? CULL_BACK
                                                                             : CULL_FRONT;
    } else if (ieq(k, "deformvertexes"))
      flags |= MAT_DEFORM;
    else if (ieq(k, "skyparms"))
      flags |= MAT_SKY;
  }
  int have_base = 0, b_blend = -1, b_alpha = ALPHA_NONE, b_vertex = 0, b_env = 0;
  char *b_map = 0;
  for (int s = 0; s < sh->stage_start.n; s++) {
    int m_blend = -1, m_alpha = ALPHA_NONE, m_vertex = 0, m_env = 0;
    char *m_map = 0;
    for (int j = 0; j < sh->stage_n.a[s]; j++) {
      struct sline L = sh->stage_lines.a[sh->stage_start.a[s] + j];
      const char *k = TOK(sh, L, 0);
      if ((ieq(k, "map") || ieq(k, "clampmap")) && L.n > 1) {
        free(m_map);
        m_map = strdup(TOK(sh, L, 1));
        lower_slash(m_map);
      } else if (ieq(k, "animmap") && L.n > 2) {
        free(m_map);
        m_map = strdup(TOK(sh, L, 2));
        lower_slash(m_map);
      } else if (ieq(k, "blendfunc"))
        m_blend = blend_of(sh, L);
      else if (ieq(k, "alphafunc") && L.n > 1) {
        const char *v = TOK(sh, L, 1);
        m_alpha = ieq(v, "gt0") ? ALPHA_GT0 : ieq(v, "lt128") ? ALPHA_LT128 : ieq(v, "ge128") ? ALPHA_GE128 : ALPHA_NONE;
      } else if (ieq(k, "tcmod"))
        flags |= MAT_TCMOD;
      else if (ieq(k, "tcgen") && L.n > 1 && ieq(TOK(sh, L, 1), "environment"))
        m_env = 1;
      else if (ieq(k, "rgbgen") && L.n > 1 && (ieq(TOK(sh, L, 1), "vertex") || ieq(TOK(sh, L, 1), "exactvertex")))
        m_vertex = 1;
    }
    if (m_map && !strcmp(m_map, "$lightmap")) {
      if (lightmapped)
        flags |= MAT_LIGHTMAP;
      free(m_map);
      continue;
    }
    if (!m_map || !strcmp(m_map, "$whiteimage")) {
      free(m_map);
      continue;
    }
    if (!have_base) {
      have_base = 1, b_blend = m_blend, b_alpha = m_alpha, b_vertex = m_vertex, b_env = m_env, b_map = m_map;
    } else
      free(m_map);
  }
  char *tex = 0;
  if (have_base) {
    tex = b_map;
    alpha = b_alpha;
    if (b_env)
      flags |= MAT_ENV;
    if (b_vertex && (flags & MAT_LIGHTMAP))
      flags |= MAT_VLM;
    if (b_vertex || !(flags & MAT_LIGHTMAP))
      flags |= MAT_VERTEX;
    if (b_blend >= 0 && !(flags & MAT_LIGHTMAP))
      blend = b_blend;
  } else
    flags |= MAT_VERTEX;
  *flags_ = flags, *blend_ = blend, *alpha_ = alpha, *cull_ = cull;
  return tex;
}

/* ---- textures of ps2tex.pk3 ---- */
struct p8 {
  int w, h, alpha, ok;
};
static struct p8 load_p8(const u8 *blob, u32 len) {
  struct p8 r = {0, 0, 0, 0};
  if (!blob || len < 24 || memcmp(blob, "P8T1", 4))
    return r;
  int w0 = blob[12] | blob[13] << 8, h0 = blob[14] | blob[15] << 8, levels = blob[16] | blob[17] << 8;
  u32 fmt = rd32(blob + 20);
  int four = fmt == 0x8B90 || fmt == 0x8B91, has_a = fmt == 0x8B91 || fmt == 0x8B96;
  int ncol = four ? 16 : 256, bpe = has_a ? 4 : 3;
  int any = 0;
  for (int i = 0, w = w0, h = h0; i < levels; i++) {
    if (w >= 8 && h >= 8 && any < 7)
      any++;
    w = w / 2 > 1 ? w / 2 : 1, h = h / 2 > 1 ? h / 2 : 1;
  }
  if (!any)
    return r;
  r.w = w0, r.h = h0, r.ok = 1;
  if (has_a && 24 + (u32)(ncol * bpe) <= len)
    for (int i = 0; i < ncol && !r.alpha; i++)
      r.alpha = blob[24 + i * bpe + 3] < 255;
  return r;
}

/* ---- the BSP ---- */
enum { L_ENTITIES, L_SHADERS, L_PLANES, L_NODES, L_LEAFS, L_LEAFSURFACES, L_LEAFBRUSHES, L_MODELS, L_BRUSHES,
       L_BRUSHSIDES, L_DRAWVERTS, L_DRAWINDEXES, L_FOGS, L_SURFACES, L_LIGHTMAPS, L_LIGHTGRID, L_VISIBILITY };
struct vert { /* as the BSP's, in float32 */
  float xyz[3], st[2], lm[2], n[3];
  u8 c[4];
};
struct surf {
  int s[14]; /* shader, fog, type, firstVert, numVerts, firstIndex, numIndexes, lm, lmx, lmy, lmw, lmh, pw, ph */
};
struct page { /* a lightmap page: side x side RGB */
  int side;
  u8 *rgb;
};
struct bsp {
  const u8 *l[17];
  u32 ln[17];
  int nshaders, nplanes, nnodes, nleafs, nleafsurf, nmodels, nverts, nindexes, nsurf, nlm;
  struct vert *verts;
  struct surf *surf;
  struct page *lm;
  int nclusters, cluster_bytes;
  const u8 *vis;
};
static const char *shader_name(const struct bsp *b, int i, char *out) {
  memcpy(out, b->l[L_SHADERS] + 72 * i, 64);
  out[64] = 0;
  lower_slash(out);
  return out;
}
static s32 shader_surf(const struct bsp *b, int i) { return rds32(b->l[L_SHADERS] + 72 * i + 64); }
static s32 shader_cont(const struct bsp *b, int i) { return rds32(b->l[L_SHADERS] + 72 * i + 68); }
static float plane_f(const struct bsp *b, int i, int k) { return rdf(b->l[L_PLANES] + 16 * i + 4 * k); }
static s32 node_i(const struct bsp *b, int i, int k) { return rds32(b->l[L_NODES] + 36 * i + 4 * k); }
static s32 leaf_i(const struct bsp *b, int i, int k) { return rds32(b->l[L_LEAFS] + 48 * i + 4 * k); }
static s32 leafsurf(const struct bsp *b, int i) { return rds32(b->l[L_LEAFSURFACES] + 4 * i); }
static float model_f(const struct bsp *b, int i, int k) { return rdf(b->l[L_MODELS] + 40 * i + 4 * k); }
static s32 model_i(const struct bsp *b, int i, int k) { return rds32(b->l[L_MODELS] + 40 * i + 4 * k); }

static int bsp_load(struct bsp *b, const u8 *data, u32 n) {
  memset(b, 0, sizeof *b);
  if (n < 8 + 17 * 8 || memcmp(data, "IBSP", 4) || rds32(data + 4) != 46)
    return -1;
  for (int i = 0; i < 17; i++) {
    u32 o = rd32(data + 8 + 8 * i), s = rd32(data + 12 + 8 * i);
    if (o > n || s > n - o)
      return -1;
    b->l[i] = data + o, b->ln[i] = s;
  }
  b->nshaders = (int)(b->ln[L_SHADERS] / 72), b->nplanes = (int)(b->ln[L_PLANES] / 16);
  b->nnodes = (int)(b->ln[L_NODES] / 36), b->nleafs = (int)(b->ln[L_LEAFS] / 48);
  b->nleafsurf = (int)(b->ln[L_LEAFSURFACES] / 4), b->nmodels = (int)(b->ln[L_MODELS] / 40);
  b->nverts = (int)(b->ln[L_DRAWVERTS] / 44), b->nindexes = (int)(b->ln[L_DRAWINDEXES] / 4);
  b->nsurf = (int)(b->ln[L_SURFACES] / 104), b->nlm = (int)(b->ln[L_LIGHTMAPS] / (128 * 128 * 3));
  b->verts = malloc((size_t)(b->nverts ? b->nverts : 1) * sizeof *b->verts);
  for (int i = 0; i < b->nverts; i++) {
    const u8 *p = b->l[L_DRAWVERTS] + 44 * i;
    struct vert *v = &b->verts[i];
    for (int k = 0; k < 3; k++)
      v->xyz[k] = rdf(p + 4 * k), v->n[k] = rdf(p + 28 + 4 * k);
    v->st[0] = rdf(p + 12), v->st[1] = rdf(p + 16), v->lm[0] = rdf(p + 20), v->lm[1] = rdf(p + 24);
    memcpy(v->c, p + 40, 4);
  }
  b->surf = malloc((size_t)(b->nsurf ? b->nsurf : 1) * sizeof *b->surf);
  for (int i = 0; i < b->nsurf; i++) {
    const u8 *p = b->l[L_SURFACES] + 104 * i;
    for (int k = 0; k < 12; k++)
      b->surf[i].s[k] = rds32(p + 4 * k);
    b->surf[i].s[12] = rds32(p + 96), b->surf[i].s[13] = rds32(p + 100);
  }
  b->lm = malloc((size_t)(b->nlm ? b->nlm : 1) * sizeof *b->lm);
  for (int i = 0; i < b->nlm; i++) {
    b->lm[i].side = 128;
    b->lm[i].rgb = malloc(128 * 128 * 3);
    memcpy(b->lm[i].rgb, b->l[L_LIGHTMAPS] + 128 * 128 * 3 * i, 128 * 128 * 3);
  }
  if (b->ln[L_VISIBILITY] >= 8) {
    b->nclusters = rds32(b->l[L_VISIBILITY]), b->cluster_bytes = rds32(b->l[L_VISIBILITY] + 4);
    b->vis = b->l[L_VISIBILITY] + 8;
  }
  return 0;
}

/* ---- geometry: a vertex is 14 doubles: xyz 0-2, st 3-4, lm 5-6, normal 7-9, rgba 10-13 ---- */
typedef struct {
  double v[14];
} V;
typedef VEC(V) vlist;
typedef struct {
  int i[3];
} T;
typedef VEC(T) tlist;

static void surface_vertices(const struct bsp *b, const struct surf *s, vlist *out) {
  out->n = 0;
  for (int k = 0; k < s->s[4]; k++) {
    const struct vert *d = &b->verts[s->s[3] + k];
    V x;
    for (int j = 0; j < 3; j++)
      x.v[j] = d->xyz[j], x.v[7 + j] = d->n[j];
    x.v[3] = d->st[0], x.v[4] = d->st[1], x.v[5] = d->lm[0], x.v[6] = d->lm[1];
    for (int j = 0; j < 4; j++)
      x.v[10 + j] = d->c[j];
    PUSH(*out, x);
  }
}

/* a grid of rows x cols vertices; along: 0 rows (axis 0), 1 columns */
static int segs_for(const V *g, int rows, int cols, int along, double tol) {
  int len = along ? cols : rows, other = along ? rows : cols;
  int k = (len - 1) / 2;
  double worst = 0;
  for (int i = 0; i < k; i++)
    for (int o = 0; o < other; o++) {
#define AT(a) (along ? &g[(o) * cols + (a)] : &g[(a) * cols + (o)])
      const V *p0 = AT(2 * i), *p1 = AT(2 * i + 1), *p2 = AT(2 * i + 2);
#undef AT
      double d[3];
      for (int j = 0; j < 3; j++)
        d[j] = p0->v[j] - 2 * p1->v[j] + p2->v[j];
      double l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (l > worst)
        worst = l;
    }
  int n = 1;
  while (n < 16 && worst / (4.0 * n * n) > tol)
    n *= 2;
  return n;
}

/* bezier_rows along an axis of a rows x cols grid: n segments a span */
static V *bezier(const V *g, int rows, int cols, int along, int n, int *orows, int *ocols) {
  int len = along ? cols : rows, other = along ? rows : cols;
  int k = (len - 1) / 2, olen = k * n + 1;
  *orows = along ? rows : olen, *ocols = along ? olen : cols;
  V *out = malloc((size_t)olen * (size_t)other * sizeof *out);
  for (int o = 0; o < other; o++) {
#define IN(a) (along ? &g[(o) * cols + (a)] : &g[(a) * cols + (o)])
#define OUT(a) (along ? &out[(o) * olen + (a)] : &out[(a) * cols + (o)])
    int at = 0;
    for (int i = 0; i < k; i++) {
      const V *p0 = IN(2 * i), *p1 = IN(2 * i + 1), *p2 = IN(2 * i + 2);
      for (int j = 0; j < n; j++) {
        double t = (double)j / n, u = (1 - t) * (1 - t);
        V *q = OUT(at++);
        for (int c = 0; c < 14; c++)
          q->v[c] = p0->v[c] * u + p1->v[c] * 2 * t * (1 - t) + p2->v[c] * t * t;
      }
    }
    *OUT(at) = *IN(len - 1);
#undef IN
#undef OUT
  }
  return out;
}

static void patch_triangles(const struct bsp *b, const struct surf *s, vlist *verts, tlist *tris) {
  int w = s->s[12], h = s->s[13];
  vlist c = {0};
  surface_vertices(b, s, &c);
  verts->n = 0, tris->n = 0;
  if (w < 3 || h < 3 || w * h > c.n) {
    free(c.a);
    return;
  }
  int nu = segs_for(c.a, h, w, 1, CURVE), nv = segs_for(c.a, h, w, 0, CURVE);
  int r1, c1, r2, c2;
  V *g1 = bezier(c.a, h, w, 0, nv, &r1, &c1);
  V *g = bezier(g1, r1, c1, 1, nu, &r2, &c2);
  free(g1);
  free(c.a);
  for (int i = 0; i < r2 * c2; i++) {
    double *n = g[i].v + 7;
    double ln = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    double d = ln > 0 ? ln : 1;
    n[0] /= d, n[1] /= d, n[2] /= d;
    PUSH(*verts, g[i]);
  }
  free(g);
  for (int y = 0; y < r2 - 1; y++)
    for (int x = 0; x < c2 - 1; x++) {
      int a = y * c2 + x, bb = a + 1, cc = (y + 1) * c2 + x, d = cc + 1;
      T t1 = {{a, cc, bb}}, t2 = {{bb, cc, d}};
      PUSH(*tris, t1);
      PUSH(*tris, t2);
    }
}

/* the midpoints made: (lo, hi) -> vertex */
struct mid {
  int a, b, m;
};
static void split_long(vlist *verts, tlist *tris, double edge_max, double st_max) {
  VEC(struct mid) mids = {0};
  tlist out = {0}, stack = {0};
  for (int i = 0; i < tris->n; i++)
    PUSH(stack, tris->a[i]);
  /* the midpoints by key, for lookups: a hash */
  int hcap = 1024;
  int *hash = malloc((size_t)hcap * sizeof *hash);
  for (int i = 0; i < hcap; i++)
    hash[i] = -1;
  while (stack.n) {
    T t = stack.a[--stack.n];
    double best = 0;
    int be = -1;
    for (int e = 0; e < 3; e++) {
      const double *a = verts->a[t.i[e]].v, *bb = verts->a[t.i[(e + 1) % 3]].v;
      double d[5];
      for (int k = 0; k < 5; k++)
        d[k] = a[k] - bb[k];
      double l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / edge_max;
      double l3 = fabs(d[3]) / st_max, l4 = fabs(d[4]) / st_max;
      /* max(l, a, b): the first of the largest */
      double m = l;
      if (l3 > m)
        m = l3;
      if (l4 > m)
        m = l4;
      if (m > best)
        best = m, be = e;
    }
    if (best <= 1.0 || verts->n > 60000) {
      PUSH(out, t);
      continue;
    }
    int i0 = t.i[be], i1 = t.i[(be + 1) % 3], i2 = t.i[(be + 2) % 3];
    int lo = i0 < i1 ? i0 : i1, hi = i0 < i1 ? i1 : i0;
    u32 h = ((u32)lo * 2654435761u ^ (u32)hi * 40503u) & (u32)(hcap - 1);
    int m = -1;
    while (hash[h] >= 0) {
      struct mid *x = &mids.a[hash[h]];
      if (x->a == lo && x->b == hi) {
        m = x->m;
        break;
      }
      h = (h + 1) & (u32)(hcap - 1);
    }
    if (m < 0) {
      m = verts->n;
      V nv;
      for (int k = 0; k < 14; k++)
        nv.v[k] = (verts->a[i0].v[k] + verts->a[i1].v[k]) * 0.5;
      PUSH(*verts, nv);
      struct mid x = {lo, hi, m};
      PUSH(mids, x);
      hash[h] = mids.n - 1;
      if (mids.n * 2 > hcap) {
        hcap *= 2;
        hash = realloc(hash, (size_t)hcap * sizeof *hash);
        for (int i = 0; i < hcap; i++)
          hash[i] = -1;
        for (int i = 0; i < mids.n; i++) {
          u32 hh = ((u32)mids.a[i].a * 2654435761u ^ (u32)mids.a[i].b * 40503u) & (u32)(hcap - 1);
          while (hash[hh] >= 0)
            hh = (hh + 1) & (u32)(hcap - 1);
          hash[hh] = i;
        }
      }
    }
    T a1 = {{i0, m, i2}}, a2 = {{m, i1, i2}};
    PUSH(stack, a1);
    PUSH(stack, a2);
  }
  free(hash);
  free(mids.a);
  free(stack.a);
  free(tris->a);
  *tris = out;
}

/* triangles -> strips: (index, reversed) with -1 for a strip's first two */
struct sv {
  int i, rev;
};
typedef VEC(struct sv) strip;
static int stripify(const T *tris, int nt, strip *out) {
  /* the edges in the order the Python dict lists them: by triangle, by edge */
  int ne = nt * 3;
  int *ek = malloc((size_t)(ne ? ne : 1) * 2 * sizeof *ek), *et = malloc((size_t)(ne ? ne : 1) * sizeof *et);
  for (int ti = 0; ti < nt; ti++)
    for (int e = 0; e < 3; e++) {
      int a = tris[ti].i[e], b = tris[ti].i[(e + 1) % 3];
      ek[2 * (ti * 3 + e)] = a < b ? a : b, ek[2 * (ti * 3 + e) + 1] = a < b ? b : a;
      et[ti * 3 + e] = ti;
    }
  /* the dict's list of a key: its entries in insertion order; a key's
     first insertion decides nothing else, so a scan in order is the same */
  u8 *used = calloc((u32)nt + 1, 1), *mark = calloc((u32)nt + 1, 1);
  u32 cap = (u32)nt + 2;
  int *seq = malloc(cap * sizeof *seq), *tl = malloc(cap * sizeof *tl);
  int *bseq = malloc(cap * sizeof *bseq), *btl = malloc(cap * sizeof *btl);
  int ns = 0;
  for (int start = 0; start < nt; start++) {
    if (used[start])
      continue;
    int bn = -1;
    for (int e = 0; e < 3; e++) {
      const int *t = tris[start].i;
      int a = t[e], b = t[(e + 1) % 3], c = t[(e + 2) % 3];
      int sn = 3, tn = 1;
      seq[0] = a, seq[1] = b, seq[2] = c, tl[0] = start;
      mark[start] = 1;
      int cur = start;
      for (;;) {
        int x = seq[sn - 2], y = seq[sn - 1], lo = x < y ? x : y, hi = x < y ? y : x, tj = -1;
        for (int k = 0; k < ne && tj < 0; k++)
          if (ek[2 * k] == lo && ek[2 * k + 1] == hi) {
            int cand = et[k];
            if (cand != cur && !used[cand] && !mark[cand])
              tj = cand;
          }
        if (tj < 0)
          break;
        int o = -1;
        for (int k = 0; k < 3 && o < 0; k++)
          if (tris[tj].i[k] != x && tris[tj].i[k] != y)
            o = tris[tj].i[k];
        seq[sn++] = o;
        tl[tn++] = tj;
        mark[tj] = 1;
        cur = tj;
      }
      for (int k = 0; k < tn; k++)
        mark[tl[k]] = 0;
      if (bn < 0 || tn > bn) {
        bn = tn;
        memcpy(bseq, seq, (size_t)sn * sizeof *seq);
        memcpy(btl, tl, (size_t)tn * sizeof *tl);
      }
    }
    for (int k = 0; k < bn; k++)
      used[btl[k]] = 1;
    strip s = {0};
    struct sv v0 = {bseq[0], -1}, v1 = {bseq[1], -1};
    PUSH(s, v0);
    PUSH(s, v1);
    for (int k = 0; k < bn; k++) {
      const int *t = tris[btl[k]].i;
      int a = bseq[k], b = bseq[k + 1], c = bseq[k + 2], rev = 1;
      for (int e = 0; e < 3; e++)
        if (t[e] == a && t[(e + 1) % 3] == b && t[(e + 2) % 3] == c)
          rev = 0;
      struct sv v = {c, rev};
      PUSH(s, v);
    }
    out[ns++] = s;
  }
  free(ek), free(et), free(used), free(mark), free(seq), free(tl), free(bseq), free(btl);
  return ns;
}

static void sample_lightmap(const struct page *pg, double s, double t, double c[3]) {
  int side = pg->side;
  double x = s * side - 0.5, y = t * side - 0.5;
  double x0 = floor(x), y0 = floor(y);
  double fx = x - x0, fy = y - y0;
  c[0] = c[1] = c[2] = 0;
  for (int dy = 0; dy < 2; dy++)
    for (int dx = 0; dx < 2; dx++) {
      double wy = dy ? fy : 1 - fy, wx = dx ? fx : 1 - fx;
      double xx = x0 + dx, yy = y0 + dy;
      int xi = (int)(xx < 0 ? 0 : xx > side - 1 ? side - 1 : xx);
      int yi = (int)(yy < 0 ? 0 : yy > side - 1 ? side - 1 : yy);
      const u8 *p = pg->rgb + 3 * (yi * side + xi);
      for (int k = 0; k < 3; k++)
        c[k] += p[k] * (wx * wy);
    }
}

/* the lightmap's colour for each vertex of a surface (ps2map.py's
   vertex_hues): of the page's texels in the surface's rectangle, those
   nearer to this vertex than to any other, summed; mean: their mean (a
   vertex without a texel: the lightmap at it). 0 when there are too many */
static double *vertex_hues(const struct page *pg, const V *verts, int nv, int mean) {
  int h = pg->side, w = pg->side;
  double *lx = malloc((size_t)nv * sizeof *lx), *ly = malloc((size_t)nv * sizeof *ly);
  double mnx = 1e300, mxx = -1e300, mny = 1e300, mxy = -1e300;
  for (int i = 0; i < nv; i++) {
    lx[i] = verts[i].v[5] * w - 0.5, ly[i] = verts[i].v[6] * h - 0.5;
    if (lx[i] < mnx)
      mnx = lx[i];
    if (lx[i] > mxx)
      mxx = lx[i];
    if (ly[i] < mny)
      mny = ly[i];
    if (ly[i] > mxy)
      mxy = ly[i];
  }
  double fx0 = floor(mnx), fx1 = ceil(mxx), fy0 = floor(mny), fy1 = ceil(mxy);
  int x0 = (int)(fx0 > 0 ? fx0 : 0), x1 = (int)(fx1 < w - 1 ? fx1 : w - 1);
  int y0 = (int)(fy0 > 0 ? fy0 : 0), y1 = (int)(fy1 < h - 1 ? fy1 : h - 1);
  if (x1 < x0 || y1 < y0 || (double)(x1 - x0 + 1) * (y1 - y0 + 1) * nv > 4000000.0) {
    free(lx), free(ly);
    return 0;
  }
  double *out = calloc((size_t)nv * 3, sizeof *out);
  int *cnt = calloc((size_t)nv, sizeof *cnt);
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) {
      double px = x, py = y, best = 0;
      int near = 0;
      for (int i = 0; i < nv; i++) {
        double dx = px - lx[i], dy = py - ly[i];
        double d = dx * dx + dy * dy;
        if (i == 0 || d < best)
          best = d, near = i;
      }
      const u8 *p = pg->rgb + 3 * (y * w + x);
      for (int k = 0; k < 3; k++)
        out[3 * near + k] += p[k];
      cnt[near]++;
    }
  if (mean)
    for (int i = 0; i < nv; i++) {
      if (cnt[i])
        for (int k = 0; k < 3; k++)
          out[3 * i + k] /= cnt[i];
      else
        sample_lightmap(pg, verts[i].v[5], verts[i].v[6], out + 3 * i);
    }
  free(cnt), free(lx), free(ly);
  return out;
}

/* ---- the lightmaps packed again by place (ps2map.py's repack_lightmaps) ---- */
typedef VEC(struct page) pagevec;
struct rect {
  int page, x, y, w, h;
  VEC(int) surfs;
  u32 z;
  int seq;
  int np, nx, ny, nside; /* its place */
};
static int rect_by_z(const void *a, const void *b) {
  const struct rect *ra = *(struct rect *const *)a, *rb = *(struct rect *const *)b;
  return ra->z < rb->z ? -1 : ra->z > rb->z ? 1 : ra->seq - rb->seq;
}
static int rect_by_h(const void *a, const void *b) {
  const struct rect *ra = *(struct rect *const *)a, *rb = *(struct rect *const *)b;
  return ra->h != rb->h ? rb->h - ra->h : ra->seq - rb->seq; /* stable: seq is the batch's order */
}

static void pack(struct rect **order, int n, int side, const struct page *old, pagevec *pages) {
  int i = 0;
  VEC(struct rect *) carry = {0}, batch = {0};
  while (i < n || carry.n) {
    batch.n = 0;
    long area = 0;
    for (int k = 0; k < carry.n; k++) {
      PUSH(batch, carry.a[k]);
      area += (long)carry.a[k]->w * carry.a[k]->h;
    }
    carry.n = 0;
    while (i < n && area + (long)order[i]->w * order[i]->h <= 0.97 * side * side) {
      PUSH(batch, order[i]);
      area += (long)order[i]->w * order[i]->h;
      i++;
    }
    if (!batch.n) {
      PUSH(batch, order[i]);
      i++;
    }
    /* sorted by height, stably: the batch's own order breaks ties */
    for (int k = 0; k < batch.n; k++)
      batch.a[k]->seq = k;
    qsort(batch.a, (size_t)batch.n, sizeof *batch.a, rect_by_h);
    u8 *used = calloc((size_t)side * side, 1), *pg = calloc((size_t)side * side * 3, 1);
    int x = 0, y = 0, shelf = 0;
    for (int k = 0; k < batch.n; k++) {
      struct rect *r = batch.a[k];
      int w = r->w, h = r->h;
      if (x + w > side)
        x = 0, y += shelf, shelf = 0;
      if (y + h > side) {
        PUSH(carry, r);
        continue;
      }
      for (int yy = 0; yy < h; yy++) {
        memcpy(pg + 3 * ((y + yy) * side + x), old[r->page].rgb + 3 * ((r->y + yy) * 128 + r->x), (size_t)w * 3);
        memset(used + (y + yy) * side + x, 1, (size_t)w);
      }
      r->np = pages->n, r->nx = x, r->ny = y, r->nside = side;
      x += w;
      if (h > shelf)
        shelf = h;
    }
    /* the texels no surface has take a neighbour's (np.roll: with wrap) */
    u8 *su = malloc((size_t)side * side), *sp = malloc((size_t)side * side * 3);
    static const int dirs[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
    for (int it = 0; it < 2; it++)
      for (int d = 0; d < 4; d++) {
        int dy = dirs[d][0], dx = dirs[d][1];
        memcpy(su, used, (size_t)side * side);
        memcpy(sp, pg, (size_t)side * side * 3);
        for (int yy = 0; yy < side; yy++)
          for (int xx = 0; xx < side; xx++) {
            int fy = ((yy - dy) % side + side) % side, fx = ((xx - dx) % side + side) % side;
            int k = yy * side + xx, f = fy * side + fx;
            if (!su[k] && su[f]) {
              memcpy(pg + 3 * k, sp + 3 * f, 3);
              used[k] = 1;
            }
          }
      }
    free(su), free(sp), free(used);
    struct page p = {side, pg};
    PUSH(*pages, p);
  }
  free(carry.a), free(batch.a);
}

static void repack_lightmaps(struct bsp *b) {
  VEC(struct rect *) rects = {0};
  for (int si = 0; si < b->nsurf; si++) {
    const int *s = b->surf[si].s;
    if (s[7] < 0 || s[4] <= 0 || (s[2] != MST_PLANAR && s[2] != MST_PATCH))
      continue;
    /* the texels the bilinear filter reads for these coordinates (float32) */
    float mn[2] = {0, 0}, mx[2] = {0, 0};
    for (int k = 0; k < s[4]; k++)
      for (int j = 0; j < 2; j++) {
        float l = f32((double)b->verts[s[3] + k].lm[j] * 128.0 - 0.5);
        if (!k || l < mn[j])
          mn[j] = l;
        if (!k || l > mx[j])
          mx[j] = l;
      }
    int x0, y0, x1, y1;
    double v;
#define CLIP(a) ((a) < 0 ? 0 : (a) > 127 ? 127 : (a))
    v = floor(mn[0]), x0 = (int)CLIP(v);
    v = floor(mn[1]), y0 = (int)CLIP(v);
    v = floor(mx[0]) + 1, x1 = (int)CLIP(v);
    v = floor(mx[1]) + 1, y1 = (int)CLIP(v);
#undef CLIP
    int w = x1 - x0 + 1, h = y1 - y0 + 1, found = -1;
    for (int k = 0; k < rects.n && found < 0; k++) {
      struct rect *r = rects.a[k];
      if (r->page == s[7] && r->x == x0 && r->y == y0 && r->w == w && r->h == h)
        found = k;
    }
    if (found < 0) {
      struct rect *r = calloc(1, sizeof *r);
      r->page = s[7], r->x = x0, r->y = y0, r->w = w, r->h = h;
      PUSH(rects, r);
      found = rects.n - 1;
    }
    PUSH(rects.a[found]->surfs, si);
  }
  if (!rects.n)
    return;
  /* the place of its first surface: a Z-order of cells of 256 units (float32) */
  for (int k = 0; k < rects.n; k++) {
    struct rect *r = rects.a[k];
    const int *s = b->surf[r->surfs.a[0]].s;
    float c[3] = {0, 0, 0};
    for (int i = 0; i < s[4]; i++)
      for (int j = 0; j < 3; j++)
        c[j] = f32((double)c[j] + b->verts[s[3] + i].xyz[j]);
    u32 z = 0;
    int q[3];
    for (int j = 0; j < 3; j++) {
      float m = f32((double)c[j] / s[4]);
      float a = f32((double)m + 65536);
      q[j] = (int)floor((double)a / 256);
    }
    for (int bb = 0; bb < 10; bb++)
      for (int a = 0; a < 3; a++)
        z |= (u32)(q[a] >> bb & 1) << (3 * bb + a);
    r->z = z, r->seq = k;
  }
  qsort(rects.a, (size_t)rects.n, sizeof *rects.a, rect_by_z);
  pagevec pages = {0};
  VEC(struct rect *) part = {0};
  for (int k = 0; k < rects.n; k++)
    if (rects.a[k]->w <= 64 && rects.a[k]->h <= 64)
      PUSH(part, rects.a[k]);
  pack(part.a, part.n, 64, b->lm, &pages);
  part.n = 0;
  for (int k = 0; k < rects.n; k++)
    if (rects.a[k]->w > 64 || rects.a[k]->h > 64)
      PUSH(part, rects.a[k]);
  pack(part.a, part.n, 128, b->lm, &pages);
  free(part.a);
  /* the rects in their first order (the dict's), the vertices moved once */
  struct rect **byseq = malloc((size_t)rects.n * sizeof *byseq);
  for (int k = 0; k < rects.n; k++)
    byseq[k] = rects.a[k];
  /* seq was overwritten by pack: the order of creation is the surfaces' */
  for (int k = 0; k < rects.n; k++)
    for (int j = k + 1; j < rects.n; j++)
      if (byseq[j]->surfs.a[0] < byseq[k]->surfs.a[0]) {
        struct rect *t = byseq[k];
        byseq[k] = byseq[j], byseq[j] = t;
      }
  u8 *done = calloc((size_t)(b->nverts ? b->nverts : 1), 1);
  for (int k = 0; k < rects.n; k++) {
    struct rect *r = byseq[k];
    for (int j = 0; j < r->surfs.n; j++) {
      int *s = b->surf[r->surfs.a[j]].s;
      if (s[3] >= 0 && s[3] < b->nverts && !done[s[3]]) {
        done[s[3]] = 1;
        for (int i = 0; i < s[4]; i++) {
          float *lm = b->verts[s[3] + i].lm;
          lm[0] = f32((double)f32(f32((double)lm[0] * 128.0) + (double)(float)(r->nx - r->x)) / r->nside);
          lm[1] = f32((double)f32(f32((double)lm[1] * 128.0) + (double)(float)(r->ny - r->y)) / r->nside);
        }
      }
      s[7] = r->np, s[8] = r->nx, s[9] = r->ny, s[10] = r->w, s[11] = r->h;
    }
  }
  free(done), free(byseq);
  for (int k = 0; k < rects.n; k++)
    free(rects.a[k]->surfs.a), free(rects.a[k]);
  free(rects.a);
  for (int i = 0; i < b->nlm; i++)
    free(b->lm[i].rgb);
  free(b->lm);
  b->lm = pages.a, b->nlm = pages.n;
}

/* ---- the map ---- */
struct mat {
  char *name, *texname;
  int tex, flags, blend, alpha, cull, sprite, alit;
  s32 surf, cont;
  double area, texels;
  int has_area;
};
struct gtri { /* a triangle for the chunks */
  int key[4], seq, surf;
  s32 p[3][3], st[3][2], lmst[3][2], col[3][4];
  double c[3], n[3];
  int okey[4];
};
struct chunkrec {
  double mins[3], maxs[3];
  int mat, lm, nverts, flags, fog, nbatch, model, area, group;
  u32 off, qwc;
  int cone[4];
  iset clusters;
};
struct sprite {
  int mat, fog, surf;
  bytes rec;
  double lo[3], hi[3];
};

static int gkey_cmp(const void *a, const void *b) {
  const struct gtri *x = a, *y = b;
  for (int k = 0; k < 4; k++)
    if (x->key[k] != y->key[k])
      return x->key[k] < y->key[k] ? -1 : 1;
  return x->seq - y->seq;
}

/* the chunks being made */
struct work {
  struct gtri *t;
  VEC(struct chunkrec) recs;
  bytes blob;
  int ngroup;
  struct mat *mats;
  iset *sclusters, *sareas;
};

static int okey_cmp(const int *a, const int *b) {
  /* Python's comparison of tuples of different lengths */
  int n = a[3] < b[3] ? a[3] : b[3];
  for (int k = 0; k < n; k++)
    if (a[k] != b[k])
      return a[k] < b[k] ? -1 : 1;
  return a[3] - b[3];
}

static void make_chunk(struct work *w, int model, int mat, int lm, int fog, const int *tris, int nt, int group);

/* the group's triangles (indices into w->t) cut by place */
static void cut(struct work *w, int model, int mat, int lm, int fog, int *tris, int nt) {
  double mn[3], mx[3];
  for (int k = 0; k < 3; k++)
    mn[k] = mx[k] = w->t[tris[0]].c[k];
  for (int i = 1; i < nt; i++)
    for (int k = 0; k < 3; k++) {
      double v = w->t[tris[i]].c[k];
      if (v < mn[k])
        mn[k] = v;
      if (v > mx[k])
        mx[k] = v;
    }
  double size[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
  double smax = size[0];
  int axis = 0;
  for (int k = 1; k < 3; k++)
    if (size[k] > smax)
      smax = size[k], axis = k;
  if (nt * 1.4 <= CHUNK && (smax <= EXTENT || nt <= LEAST)) {
    /* by direction: the okeys in order, each one's triangles in theirs */
    int *by = malloc((size_t)nt * sizeof *by);
    memcpy(by, tris, (size_t)nt * sizeof *by);
    /* a stable sort by okey (insertion sort: a chunk is small) */
    for (int i = 1; i < nt; i++) {
      int x = by[i], j = i - 1;
      while (j >= 0 && okey_cmp(w->t[by[j]].okey, w->t[x].okey) > 0)
        by[j + 1] = by[j], j--;
      by[j + 1] = x;
    }
    for (int i = 0; i < nt;) {
      int j = i;
      while (j < nt && !okey_cmp(w->t[by[j]].okey, w->t[by[i]].okey))
        j++;
      make_chunk(w, model, mat, lm, fog, by + i, j - i, w->ngroup);
      i = j;
    }
    free(by);
    w->ngroup++;
    return;
  }
  /* argsort, stable, by the axis's coordinate: a merge sort */
  int *ord = malloc((size_t)nt * sizeof *ord), *tmp = malloc((size_t)nt * sizeof *tmp);
  memcpy(ord, tris, (size_t)nt * sizeof *ord);
  for (int width = 1; width < nt; width *= 2)
    for (int lo = 0; lo < nt; lo += 2 * width) {
      int mid = lo + width < nt ? lo + width : nt, hi = lo + 2 * width < nt ? lo + 2 * width : nt;
      int a = lo, b = mid, k = lo;
      while (a < mid && b < hi)
        tmp[k++] = w->t[ord[b]].c[axis] < w->t[ord[a]].c[axis] ? ord[b++] : ord[a++];
      while (a < mid)
        tmp[k++] = ord[a++];
      while (b < hi)
        tmp[k++] = ord[b++];
      memcpy(ord + lo, tmp + lo, (size_t)(hi - lo) * sizeof *ord);
    }
  free(tmp);
  int half = nt / 2;
  cut(w, model, mat, lm, fog, ord, half);
  cut(w, model, mat, lm, fog, ord + half, nt - half);
  free(ord);
}

/* a chunk's vertex: position, st, lmst, colour */
struct cvert {
  s32 p[3], st[2], lmst[2], col[4];
};
static int cvert_eq(const struct cvert *a, const struct cvert *b) { return !memcmp(a, b, sizeof *a); }

static void make_chunk(struct work *w, int model, int mat, int lm, int fog, const int *tris, int nt, int group) {
  VEC(struct cvert) vl = {0};
  T *it = malloc((size_t)nt * sizeof *it);
  int hcap = 64;
  while (hcap < nt * 6)
    hcap *= 2;
  int *hash = malloc((size_t)hcap * sizeof *hash);
  for (int i = 0; i < hcap; i++)
    hash[i] = -1;
  struct chunkrec r;
  memset(&r, 0, sizeof r);
  double nsum[3] = {0, 0, 0};
  iset areas = {0};
  for (int i = 0; i < nt; i++) {
    const struct gtri *g = &w->t[tris[i]];
    for (int k = 0; k < 3; k++)
      nsum[k] += g->n[k];
    iset_union(&r.clusters, &w->sclusters[g->surf]);
    iset_union(&areas, &w->sareas[g->surf]);
    for (int k = 0; k < 3; k++) {
      struct cvert v;
      memcpy(v.p, g->p[k], sizeof v.p), memcpy(v.st, g->st[k], sizeof v.st);
      memcpy(v.lmst, g->lmst[k], sizeof v.lmst), memcpy(v.col, g->col[k], sizeof v.col);
      u32 h = 2166136261u;
      const u8 *bp = (const u8 *)&v;
      for (u32 j = 0; j < sizeof v; j++)
        h = (h ^ bp[j]) * 16777619u;
      h &= (u32)(hcap - 1);
      int id = -1;
      while (hash[h] >= 0) {
        if (cvert_eq(&vl.a[hash[h]], &v)) {
          id = hash[h];
          break;
        }
        h = (h + 1) & (u32)(hcap - 1);
      }
      if (id < 0) {
        PUSH(vl, v);
        id = hash[h] = vl.n - 1;
      }
      it[i].i[k] = id;
    }
  }
  free(hash);
  strip *strips = malloc((size_t)(nt ? nt : 1) * sizeof *strips);
  int ns = stripify(it, nt, strips);
  /* the stream: (vertex, w) */
  VEC(int) sv = {0}, sw = {0};
  for (int s = 0; s < ns; s++) {
    for (int k = 0; k < strips[s].n; k++) {
      PUSH(sv, strips[s].a[k].i);
      PUSH(sw, strips[s].a[k].rev < 0 ? W_NONE : strips[s].a[k].rev ? W_REV : 0);
    }
    free(strips[s].a);
  }
  free(strips);
  /* batches: a strip that goes on in the next one starts there with its last two vertices */
  bytes body = {0};
  int pos = 0, nv = 0, nbatch = 0, lit = lm >= 0;
  int bi[BATCH + 2], bw[BATCH + 2];
  while (pos < sv.n) {
    int n = 0;
    if (pos && sw.a[pos] != W_NONE) {
      bi[n] = sv.a[pos - 2], bw[n++] = W_NONE;
      bi[n] = sv.a[pos - 1], bw[n++] = W_NONE;
      for (int k = pos; k < sv.n && k < pos + BATCH - 2; k++)
        bi[n] = sv.a[k], bw[n++] = sw.a[k];
      pos += BATCH - 2;
    } else {
      for (int k = pos; k < sv.n && k < pos + BATCH; k++)
        bi[n] = sv.a[k], bw[n++] = sw.a[k];
      pos += BATCH;
    }
    /* a batch does not end on a strip's first or second vertex */
    while (n && bw[n - 1] == W_NONE)
      n--, pos--;
    if (!n)
      continue;
    /* an even count (the programs take two vertices a turn) */
    if (n & 1)
      bi[n] = bi[n - 1], bw[n++] = W_NONE;
    nv += n, nbatch++;
    vif(&body, VIF_STCYCL, 0, 1 << 8 | 3);
    vif(&body, UNPACK_V3_16, (u32)n, 0x8000 | VU_A);
    for (int k = 0; k < n; k++) {
      const struct cvert *v = &vl.a[bi[k]];
      put_u16(&body, (u32)v->st[0]), put_u16(&body, (u32)v->st[1]), put_u16(&body, 4096);
    }
    put_zero(&body, (u32)(-(n * 6) & 3));
    vif(&body, UNPACK_V4_8, (u32)n, 0xC000 | (VU_A + 1));
    for (int k = 0; k < n; k++)
      for (int c = 0; c < 4; c++) {
        int x = vl.a[bi[k]].col[c];
        put_u8(&body, (u32)(x < 0 ? 0 : x > 255 ? 255 : x));
      }
    vif(&body, UNPACK_V4_16, (u32)n, 0x8000 | (VU_A + 2));
    for (int k = 0; k < n; k++) {
      const struct cvert *v = &vl.a[bi[k]];
      put_u16(&body, (u32)v->p[0]), put_u16(&body, (u32)v->p[1]), put_u16(&body, (u32)v->p[2]);
      put_u16(&body, (u32)bw[k]);
    }
    if (lit) {
      vif(&body, UNPACK_V3_16, (u32)n, 0x8000 | VU_IB);
      for (int k = 0; k < n; k++) {
        const struct cvert *v = &vl.a[bi[k]];
        put_u16(&body, (u32)v->lmst[0]), put_u16(&body, (u32)v->lmst[1]), put_u16(&body, 4096);
      }
      put_zero(&body, (u32)(-(n * 6) & 3));
    }
    vif(&body, VIF_ITOP, 0, (u32)n);
    vif(&body, VIF_MSCNT, 0, 0);
  }
  pad16(&body);
  u32 qwc = (u32)body.n / 16, off = (u32)w->blob.n;
  put_u32(&w->blob, DMA_CNT << 28 | qwc), put_u32(&w->blob, 0), put_zero(&w->blob, 8);
  put(&w->blob, body.a, (u32)body.n);
  put_u32(&w->blob, DMA_RET << 28), put_u32(&w->blob, 0), put_zero(&w->blob, 8);
  free(body.a);
  for (int k = 0; k < 3; k++)
    r.mins[k] = 1e300, r.maxs[k] = -1e300;
  for (int i = 0; i < vl.n; i++)
    for (int k = 0; k < 3; k++) {
      double v = vl.a[i].p[k]; /* divided by q at the end (cv_map) */
      if (v < r.mins[k])
        r.mins[k] = v;
      if (v > r.maxs[k])
        r.maxs[k] = v;
    }
  /* the cone of its triangles' normals */
  double ln = sqrt(nsum[0] * nsum[0] + nsum[1] * nsum[1] + nsum[2] * nsum[2]);
  r.cone[0] = r.cone[1] = r.cone[2] = 0, r.cone[3] = 255;
  if (ln > 0 && w->mats[mat].cull == CULL_FRONT) {
    double ax[3], an[3];
    for (int k = 0; k < 3; k++)
      ax[k] = rint(nsum[k] / ln * 127);
    double al = sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    for (int k = 0; k < 3; k++)
      an[k] = ax[k] / al;
    double cosw = 0;
    for (int i = 0; i < nt; i++) {
      const double *n = w->t[tris[i]].n;
      double d = n[0] * an[0] + n[1] * an[1] + n[2] * an[2];
      if (!i || d < cosw)
        cosw = d;
    }
    if (cosw > 0.2) {
      double sinw = sqrt(1 - cosw * cosw > 0 ? 1 - cosw * cosw : 0) + 0.01;
      int sn = (int)ceil(sinw * 255);
      r.cone[0] = (int)ax[0], r.cone[1] = (int)ax[1], r.cone[2] = (int)ax[2], r.cone[3] = sn < 254 ? sn : 254;
    }
  }
  r.mat = mat, r.lm = lm, r.nverts = nv, r.flags = lit ? CH_LIGHTMAP : 0, r.fog = fog & 255, r.off = off;
  r.qwc = qwc + 2, r.nbatch = nbatch, r.model = model, r.group = group;
  r.area = model == 0 && areas.n == 1 && areas.a[0] >= 0 && areas.a[0] < 255 ? areas.a[0] : 255;
  free(areas.a);
  PUSH(w->recs, r);
  free(vl.a), free(it), free(sv.a), free(sw.a);
}

static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static double norm3(const double *a) { return sqrt(dot3(a, a)); }

/* Python float() of a word, or 0 when it is not a number */
static int pyfloat(const char *s, double *out) {
  char *e;
  while (*s == ' ')
    s++;
  if (!*s)
    return 0;
  double v = strtod(s, &e);
  while (*e == ' ')
    e++;
  if (*e || e == s)
    return 0;
  *out = v;
  return 1;
}

int cv_map(const char *out, const char *gamedir, const char *mapname, const char *texpk3) {
  pk3_set *paks = pk3_open(gamedir);
  if (!paks)
    return -1;
  char path[128];
  snprintf(path, sizeof path, "maps/%s.bsp", mapname);
  for (char *p = path; *p; p++)
    *p = (char)lowc((u8)*p);
  int bi = pk3_find(paks, path);
  u32 bn;
  u8 *data = bi >= 0 ? pk3_read(paks, bi, &bn) : 0;
  struct bsp b;
  if (!data || bsp_load(&b, data, bn)) {
    cv_log("%s: not a Quake 3 BSP (version 46) in the paks", path);
    free(data);
    pk3_close(paks);
    return -1;
  }
  if (b.nlm)
    repack_lightmaps(&b);
  struct scripts scripts;
  parse_scripts(paks, &scripts);
  pk3_set *texpak = texpk3 ? pk3_open_file(texpk3) : 0;

  /* which model a surface belongs to; the clusters and areas it is in */
  int *surf_model = malloc((size_t)(b.nsurf ? b.nsurf : 1) * sizeof *surf_model);
  for (int i = 0; i < b.nsurf; i++)
    surf_model[i] = -1;
  for (int mi = 0; mi < b.nmodels; mi++)
    for (int si = model_i(&b, mi, 6); si < model_i(&b, mi, 6) + model_i(&b, mi, 7); si++)
      if (si >= 0 && si < b.nsurf)
        surf_model[si] = mi;
  iset *scl = calloc((size_t)(b.nsurf ? b.nsurf : 1), sizeof *scl), *sar = calloc((size_t)(b.nsurf ? b.nsurf : 1), sizeof *sar);
  for (int li = 0; li < b.nleafs; li++) {
    int cl = leaf_i(&b, li, 0);
    if (cl < 0)
      continue;
    for (int k = leaf_i(&b, li, 8); k < leaf_i(&b, li, 8) + leaf_i(&b, li, 9); k++) {
      int s = leafsurf(&b, k);
      if (s >= 0 && s < b.nsurf)
        iset_add(&scl[s], cl), iset_add(&sar[s], leaf_i(&b, li, 1));
    }
  }

  /* materials and textures */
  VEC(struct mat) mats = {0};
  VEC(char *) texnames = {0};
  VEC(struct p8) textures = {0};
  VEC(char *) tex_seen = {0};
  VEC(int) tex_seen_i = {0};
  int *mat_index = malloc((size_t)(b.nshaders ? b.nshaders : 1) * 2 * sizeof *mat_index);
  for (int i = 0; i < b.nshaders * 2; i++)
    mat_index[i] = -1;

  /* the world's size picks the positions' unit */
  double reach = 0;
  for (int i = 0; i < b.nverts; i++)
    for (int k = 0; k < 3; k++) {
      double v = fabs(b.verts[i].xyz[k]);
      if (v > reach)
        reach = v;
    }
  if (!b.nverts)
    reach = 1.0;
  double q = 8.0;
  while (reach * q > 32000 && q > 0.25)
    q /= 2;

  VEC(struct gtri) gt = {0};
  VEC(struct sprite) sprites = {0};
  vlist verts = {0};
  tlist tris = {0};
  int r = 0;
  for (int si = 0; si < b.nsurf && !r; si++) {
    if (cv_cancel) {
      r = -1;
      break;
    }
    if (!(si & 15))
      cv_progress(mapname, (u32)si, (u32)b.nsurf);
    const struct surf *S = &b.surf[si];
    int shader_num = S->s[0], fog = S->s[1], stype = S->s[2], lm = S->s[7];
    if (stype == MST_FLARE || surf_model[si] < 0 || shader_num < 0 || shader_num >= b.nshaders)
      continue;
    /* its material */
    int lmd = lm >= 0, mi = mat_index[2 * shader_num + lmd];
    if (mi < 0) {
      char nm[65];
      shader_name(&b, shader_num, nm);
      s32 sf = shader_surf(&b, shader_num), ct = shader_cont(&b, shader_num);
      struct shader *sh = script_get(&scripts, nm);
      struct mat m;
      memset(&m, 0, sizeof m);
      m.name = strdup(nm), m.surf = sf, m.cont = ct;
      m.texname = material_of(nm, sh, lmd, &m.flags, &m.blend, &m.alpha, &m.cull);
      if (sf & SURF_SKY)
        m.flags |= MAT_SKY;
      if (sf & SURF_NODRAW)
        m.flags |= MAT_NODRAW;
      /* its texture */
      m.tex = -1;
      if (m.texname) {
        char base[160];
        snprintf(base, sizeof base, "%s", m.texname);
        char *dot = strrchr(base, '.');
        if (dot && (!strcmp(dot, ".tga") || !strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg") || !strcmp(dot, ".png")))
          *dot = 0;
        int found = -2;
        for (int k = 0; k < tex_seen.n && found == -2; k++)
          if (!strcmp(tex_seen.a[k], base))
            found = tex_seen_i.a[k];
        if (found == -2) {
          found = -1;
          char pn[170];
          snprintf(pn, sizeof pn, "%s.p8", base);
          int ti = texpak ? pk3_find(texpak, pn) : -1;
          if (ti >= 0) {
            u32 tn;
            u8 *blob = pk3_read(texpak, ti, &tn);
            struct p8 t = load_p8(blob, tn);
            free(blob);
            if (t.ok) {
              found = textures.n;
              PUSH(textures, t);
              PUSH(texnames, strdup(base));
            }
          }
          PUSH(tex_seen, strdup(base));
          PUSH(tex_seen_i, found);
        }
        m.tex = found;
      }
      if ((m.flags & MAT_LIGHTMAP) && m.tex >= 0 && m.alpha != ALPHA_NONE) {
        m.alit = 1;
        m.flags = (m.flags & ~MAT_LIGHTMAP) | MAT_VERTEX | MAT_ALIT;
      }
      /* deformVertexes autosprite, autosprite2: quads that turn to the eye */
      if (sh)
        for (int i = 0; i < sh->lines.n; i++) {
          struct sline L = sh->lines.a[i];
          if (ieq(TOK(sh, L, 0), "deformvertexes") && L.n > 1 &&
              (ieq(TOK(sh, L, 1), "autosprite") || ieq(TOK(sh, L, 1), "autosprite2")))
            m.sprite = ieq(TOK(sh, L, 1), "autosprite") ? 1 : 2;
        }
      mi = mat_index[2 * shader_num + lmd] = mats.n;
      PUSH(mats, m);
    }
    struct mat *m = &mats.a[mi];
    if (m->flags & MAT_NODRAW)
      continue;
    if (m->sprite && surf_model[si] == 0 && stype != MST_PATCH && S->s[4] >= 4 && S->s[4] % 4 == 0) {
      struct sprite sp;
      memset(&sp, 0, sizeof sp);
      sp.mat = mi, sp.fog = fog, sp.surf = si;
      for (int k = 0; k < 3; k++)
        sp.lo[k] = 1e300, sp.hi[k] = -1e300;
      for (int k = 0; k < S->s[4]; k += 4) {
        double p[4][3], st[4][2];
        for (int i = 0; i < 4; i++) {
          const struct vert *dv = &b.verts[S->s[3] + k + i];
          for (int j = 0; j < 3; j++)
            p[i][j] = dv->xyz[j];
          st[i][0] = dv->st[0], st[i][1] = dv->st[1];
        }
        double lo[3], hi[3];
        if (m->sprite == 1) {
          double c[3], d[3];
          for (int j = 0; j < 3; j++)
            c[j] = (((p[0][j] + p[1][j]) + p[2][j]) + p[3][j]) / 4;
          for (int j = 0; j < 3; j++)
            d[j] = p[0][j] - c[j];
          double rr = norm3(d) * 0.7071;
          for (int j = 0; j < 3; j++)
            put_f(&sp.rec, c[j]);
          put_f(&sp.rec, rr);
          static const double rest[11] = {0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0};
          for (int j = 0; j < 11; j++)
            put_f(&sp.rec, rest[j]);
          put_f(&sp.rec, 1);
          for (int j = 0; j < 3; j++)
            lo[j] = c[j] - rr * 1.5, hi[j] = c[j] + rr * 1.5;
        } else {
          /* its two short sides: the nearest two corners and the other two */
          int bi_ = 0, bj = 1;
          double bd = -1;
          for (int i = 0; i < 4; i++)
            for (int j = i + 1; j < 4; j++) {
              double d[3] = {p[i][0] - p[j][0], p[i][1] - p[j][1], p[i][2] - p[j][2]};
              double l = norm3(d);
              if (bd < 0 || l < bd)
                bd = l, bi_ = i, bj = j;
            }
          int o2[2], n2 = 0;
          for (int x = 0; x < 4; x++)
            if (x != bi_ && x != bj)
              o2[n2++] = x;
          int kk = o2[0], ll = o2[1];
          double dk[3] = {p[kk][0] - p[bi_][0], p[kk][1] - p[bi_][1], p[kk][2] - p[bi_][2]};
          double dl[3] = {p[ll][0] - p[bi_][0], p[ll][1] - p[bi_][1], p[ll][2] - p[bi_][2]};
          if (norm3(dk) < norm3(dl)) {
            int t = kk;
            kk = ll, ll = t;
          }
          int o[4] = {bi_, bj, kk, ll};
          double *a = p[o[0]], *bb = p[o[1]], *c_ = p[o[2]], *d = p[o[3]];
          double e0[3], e1[3], c[3], ax[3], ab[3], cd[3];
          for (int j = 0; j < 3; j++) {
            e0[j] = (a[j] + bb[j]) / 2, e1[j] = (c_[j] + d[j]) / 2;
            c[j] = (e0[j] + e1[j]) / 2, ax[j] = (e1[j] - e0[j]) / 2;
            ab[j] = a[j] - bb[j], cd[j] = c_[j] - d[j];
          }
          double hw = (norm3(ab) + norm3(cd)) / 4;
          for (int j = 0; j < 3; j++)
            put_f(&sp.rec, c[j]);
          put_f(&sp.rec, 0);
          for (int j = 0; j < 3; j++)
            put_f(&sp.rec, ax[j]);
          put_f(&sp.rec, hw);
          for (int i = 0; i < 4; i++)
            put_f(&sp.rec, st[o[i]][0]), put_f(&sp.rec, st[o[i]][1]);
          for (int j = 0; j < 3; j++) {
            lo[j] = (e0[j] < e1[j] ? e0[j] : e1[j]) - hw;
            hi[j] = (e0[j] > e1[j] ? e0[j] : e1[j]) + hw;
          }
        }
        for (int j = 0; j < 3; j++) {
          if (lo[j] < sp.lo[j])
            sp.lo[j] = lo[j];
          if (hi[j] > sp.hi[j])
            sp.hi[j] = hi[j];
        }
      }
      PUSH(sprites, sp);
      continue;
    }
    if (stype == MST_PATCH)
      patch_triangles(&b, S, &verts, &tris);
    else {
      surface_vertices(&b, S, &verts);
      tris.n = 0;
      for (int k = 0; k + 2 < S->s[6]; k += 3) {
        T t;
        for (int j = 0; j < 3; j++)
          t.i[j] = rds32(b.l[L_DRAWINDEXES] + 4 * (S->s[5] + k + j));
        PUSH(tris, t);
      }
    }
    if (m->flags & MAT_SKY) {
      /* not drawn: the surface's bounds alone, as one triangle */
      if (!verts.n)
        continue;
      double lo[3], hi[3];
      for (int k = 0; k < 3; k++)
        lo[k] = hi[k] = verts.a[0].v[k];
      for (int i = 1; i < verts.n; i++)
        for (int k = 0; k < 3; k++) {
          if (verts.a[i].v[k] < lo[k])
            lo[k] = verts.a[i].v[k];
          if (verts.a[i].v[k] > hi[k])
            hi[k] = verts.a[i].v[k];
        }
      V a = verts.a[0], bb = verts.a[0], c = verts.a[0];
      for (int k = 0; k < 3; k++)
        a.v[k] = lo[k], bb.v[k] = hi[k], c.v[k] = lo[k];
      int ax = 0;
      for (int k = 1; k < 3; k++)
        if (hi[k] - lo[k] > hi[ax] - lo[ax])
          ax = k;
      c.v[ax] = hi[ax];
      verts.n = 0, tris.n = 0;
      PUSH(verts, a);
      PUSH(verts, bb);
      PUSH(verts, c);
      T t = {{0, 1, 2}};
      PUSH(tris, t);
    } else
      split_long(&verts, &tris, EDGE, 3.5);
    if (m->tex >= 0) {
      double tw = textures.a[m->tex].w, th = textures.a[m->tex].h;
      for (int i = 0; i < tris.n; i++) {
        const double *a = verts.a[tris.a[i].i[0]].v, *bb = verts.a[tris.a[i].i[1]].v, *c = verts.a[tris.a[i].i[2]].v;
        double u[3] = {bb[0] - a[0], bb[1] - a[1], bb[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        double axyz = 0.5 * sqrt(dot3(n, n));
        double ast = 0.5 * fabs((bb[3] - a[3]) * (c[4] - a[4]) - (c[3] - a[3]) * (bb[4] - a[4])) * tw * th;
        m->area += axyz, m->texels += ast, m->has_area = 1;
      }
    }
    int gk_lm = (m->flags & MAT_LIGHTMAP) ? lm : -1;
    const struct page *page = lm >= 0 && lm < b.nlm && (m->flags & MAT_LIGHTMAP) ? &b.lm[lm] : 0;
    int vlm = !!(m->flags & MAT_VLM);
    double *hues = page && verts.n ? vertex_hues(page, verts.a, verts.n, 0) : 0;
    double *alit = 0;
    if (m->alit && lm >= 0 && lm < b.nlm && !page && verts.n && !vlm)
      alit = vertex_hues(&b.lm[lm], verts.a, verts.n, 1);
    for (int i = 0; i < tris.n; i++) {
      const double *tv[3];
      int ix[3];
      for (int k = 0; k < 3; k++)
        ix[k] = tris.a[i].i[k], tv[k] = verts.a[ix[k]].v;
      s32 qp[3][3];
      for (int k = 0; k < 3; k++)
        for (int j = 0; j < 3; j++)
          qp[k][j] = (s32)rnd(tv[k][j] * q);
      if (!memcmp(qp[0], qp[1], 12) || !memcmp(qp[1], qp[2], 12) || !memcmp(qp[0], qp[2], 12))
        continue;
      struct gtri g;
      memset(&g, 0, sizeof g);
      double cs = ((0 + tv[0][3]) + tv[1][3]) + tv[2][3], ct = ((0 + tv[0][4]) + tv[1][4]) + tv[2][4];
      cs /= 3, ct /= 3;
      double cell0 = floor(cs / 4) * 4, cell1 = floor(ct / 4) * 4;
      for (int k = 0; k < 3; k++) {
        const double *v = tv[k];
        memcpy(g.p[k], qp[k], 12);
        double a0 = rnd((v[3] - cell0) * 4096), a1 = rnd((v[4] - cell1) * 4096);
        g.st[k][0] = (s32)(a0 < -32768 ? -32768 : a0 > 32767 ? 32767 : a0);
        g.st[k][1] = (s32)(a1 < -32768 ? -32768 : a1 > 32767 ? 32767 : a1);
        if (page) {
          double l0 = rnd(v[5] * 4096), l1 = rnd(v[6] * 4096);
          g.lmst[k][0] = (s32)(l0 < -32768 ? -32768 : l0 > 32767 ? 32767 : l0);
          g.lmst[k][1] = (s32)(l1 < -32768 ? -32768 : l1 > 32767 ? 32767 : l1);
          double rgb[3];
          const double *vh = hues ? hues + 3 * ix[k] : 0;
          double vmax = vh ? (vh[0] > vh[1] ? vh[0] : vh[1]) : 0;
          if (vh && vh[2] > vmax)
            vmax = vh[2];
          if (vh && vmax > 0)
            memcpy(rgb, vh, sizeof rgb);
          else
            sample_lightmap(page, v[5], v[6], rgb);
          double top = rgb[0];
          if (rgb[1] > top)
            top = rgb[1];
          if (rgb[2] > top)
            top = rgb[2];
          double hue[3];
          for (int j = 0; j < 3; j++)
            hue[j] = top > 0 ? rgb[j] / top : 1;
          if (vlm)
            for (int j = 0; j < 3; j++) {
              double m2 = v[10 + j] * 2 < 255.0 ? v[10 + j] * 2 : 255.0;
              hue[j] = hue[j] * m2 / 510.0;
            }
          for (int j = 0; j < 3; j++)
            g.col[k][j] = (s32)rnd(hue[j] * 128);
          g.col[k][3] = 128;
        } else {
          double c[3];
          const double *va = alit ? alit + 3 * ix[k] : v + 10;
          for (int j = 0; j < 3; j++)
            c[j] = va[j] * 2;
          double top = c[0];
          if (c[1] > top)
            top = c[1];
          if (c[2] > top)
            top = c[2];
          if (top > 255)
            for (int j = 0; j < 3; j++)
              c[j] = c[j] * 255 / top;
          if (vlm) {
            double l2[3] = {c[0], c[1], c[2]};
            if (lm >= 0 && lm < b.nlm) {
              sample_lightmap(&b.lm[lm], v[5], v[6], l2);
              for (int j = 0; j < 3; j++)
                l2[j] *= 2.0;
              double t2 = l2[0];
              if (l2[1] > t2)
                t2 = l2[1];
              if (l2[2] > t2)
                t2 = l2[2];
              if (t2 > 255)
                for (int j = 0; j < 3; j++)
                  l2[j] = l2[j] * 255 / t2;
            }
            for (int j = 0; j < 3; j++)
              c[j] = c[j] * l2[j] / 508.0;
          }
          for (int j = 0; j < 3; j++)
            g.col[k][j] = (s32)rnd(c[j]);
          g.col[k][3] = (s32)rnd(v[13] * 128 / 255);
        }
      }
      /* the triangle's normal (the BSP's faces are wound clockwise seen from the front) */
      double e1[3], e2[3];
      for (int j = 0; j < 3; j++)
        e1[j] = (double)(qp[1][j] - qp[0][j]), e2[j] = (double)(qp[2][j] - qp[0][j]);
      double nrm[3] = {e2[1] * e1[2] - e2[2] * e1[1], e2[2] * e1[0] - e2[0] * e1[2], e2[0] * e1[1] - e2[1] * e1[0]};
      double ln = sqrt(dot3(nrm, nrm));
      for (int j = 0; j < 3; j++)
        g.n[j] = ln > 0 ? nrm[j] / ln : 0;
      if (stype == MST_PLANAR) {
        for (int j = 0; j < 3; j++)
          g.okey[j] = (int)rnd(g.n[j] * 16);
        g.okey[3] = 3;
      } else {
        int ax = 0;
        for (int j = 1; j < 3; j++)
          if (fabs(g.n[j]) > fabs(g.n[ax]))
            ax = j;
        g.okey[0] = 100 + ax * 2 + (g.n[ax] < 0), g.okey[3] = 1;
      }
      for (int j = 0; j < 3; j++)
        g.c[j] = (((0 + tv[0][j]) + tv[1][j]) + tv[2][j]) / 3;
      g.key[0] = surf_model[si], g.key[1] = mi, g.key[2] = gk_lm, g.key[3] = fog;
      g.seq = gt.n, g.surf = si;
      PUSH(gt, g);
    }
    free(hues), free(alit);
  }
  free(verts.a), free(tris.a);

  /* groups and chunks */
  struct work w;
  memset(&w, 0, sizeof w);
  qsort(gt.a, (size_t)gt.n, sizeof *gt.a, gkey_cmp);
  w.t = gt.a, w.mats = mats.a, w.sclusters = scl, w.sareas = sar;
  int *idx = malloc((size_t)(gt.n ? gt.n : 1) * sizeof *idx);
  for (int i = 0; i < gt.n && !r;) {
    int j = i;
    while (j < gt.n && !memcmp(gt.a[j].key, gt.a[i].key, sizeof gt.a[i].key))
      j++;
    for (int k = i; k < j; k++)
      idx[k - i] = k;
    cut(&w, gt.a[i].key[0], gt.a[i].key[1], gt.a[i].key[2], gt.a[i].key[3], idx, j - i);
    i = j;
    if (cv_cancel)
      r = -1;
  }
  free(idx);
  for (int i = 0; i < w.recs.n; i++)
    for (int k = 0; k < 3; k++)
      w.recs.a[i].mins[k] /= q, w.recs.a[i].maxs[k] /= q;

  /* the surfaces of quads that turn to the eye: a chunk each, after the world's chunks */
  int at = w.recs.n;
  for (int i = 0; i < w.recs.n; i++)
    if (w.recs.a[i].model != 0) {
      at = i;
      break;
    }
  /* sorted by material, stably */
  for (int i = 1; i < sprites.n; i++) {
    struct sprite x = sprites.a[i];
    int j = i - 1;
    while (j >= 0 && sprites.a[j].mat > x.mat)
      sprites.a[j + 1] = sprites.a[j], j--;
    sprites.a[j + 1] = x;
  }
  for (int i = 0; i < sprites.n; i++) {
    struct sprite *sp = &sprites.a[i];
    struct chunkrec c;
    memset(&c, 0, sizeof c);
    c.off = (u32)w.blob.n;
    put(&w.blob, sp->rec.a, (u32)sp->rec.n);
    pad16(&w.blob);
    memcpy(c.mins, sp->lo, sizeof c.mins), memcpy(c.maxs, sp->hi, sizeof c.maxs);
    c.mat = sp->mat, c.lm = -1, c.flags = CH_SPRITE, c.fog = sp->fog & 255, c.qwc = (u32)sp->rec.n / 64;
    c.cone[3] = 255;
    const iset *ar = &sar[sp->surf];
    c.area = ar->n == 1 && ar->a[0] >= 0 && ar->a[0] < 255 ? ar->a[0] : 255;
    c.group = w.ngroup++;
    iset_union(&c.clusters, &scl[sp->surf]);
    PUSH(w.recs, c);
    memmove(w.recs.a + at + 1, w.recs.a + at, (size_t)(w.recs.n - 1 - at) * sizeof c);
    w.recs.a[at++] = c;
    free(sp->rec.a);
  }

  /* groups: bounds and chunk ranges */
  struct grec {
    int id, model, first, n;
    double mins[3], maxs[3];
    iset cl;
  };
  VEC(struct grec) grecs = {0};
  for (int i = 0; i < w.recs.n; i++) {
    struct chunkrec *c = &w.recs.a[i];
    if (!grecs.n || grecs.a[grecs.n - 1].id != c->group) {
      struct grec g;
      memset(&g, 0, sizeof g);
      g.id = c->group, g.model = c->model, g.first = i;
      memcpy(g.mins, c->mins, sizeof g.mins), memcpy(g.maxs, c->maxs, sizeof g.maxs);
      PUSH(grecs, g);
    }
    struct grec *g = &grecs.a[grecs.n - 1];
    g->n++;
    for (int k = 0; k < 3; k++) {
      if (c->mins[k] < g->mins[k])
        g->mins[k] = c->mins[k];
      if (c->maxs[k] > g->maxs[k])
        g->maxs[k] = c->maxs[k];
    }
    iset_union(&g->cl, &c->clusters);
  }
  int nworld = 0;
  bytes sec_model = {0};
  for (int mi = 0; mi < b.nmodels; mi++) {
    int first = -1, n = 0;
    for (int i = 0; i < grecs.n; i++)
      if (grecs.a[i].model == mi) {
        if (first < 0)
          first = i;
        n++;
      }
    for (int k = 0; k < 6; k++)
      put_f(&sec_model, model_f(&b, mi, k));
    put_u32(&sec_model, (u32)(first < 0 ? 0 : first)), put_u32(&sec_model, (u32)n);
    if (mi == 0)
      nworld = n;
  }

  /* visibility: the groups seen from each cluster, and the chunks */
  int ncl = b.nclusters;
  u32 vis_bytes = (u32)(nworld + 7) / 8;
  vis_bytes += (u32)(-(int)vis_bytes & 3);
  if (vis_bytes < 4)
    vis_bytes = 4;
  u32 rows = (u32)(ncl > 1 ? ncl : 1);
  u8 *vis = calloc(rows, vis_bytes);
  /* seen[k][c]: bit c of the BSP's row k */
#define SEEN(k, c) (b.vis[(size_t)(k) * (size_t)b.cluster_bytes + ((c) >> 3)] >> ((c) & 7) & 1)
  if (ncl) {
    for (int gi = 0; gi < nworld; gi++) {
      const iset *cl = &grecs.a[gi].cl;
      for (int k = 0; k < ncl; k++) {
        int any = 0;
        for (int j = 0; j < cl->n && !any; j++)
          any = cl->a[j] < ncl && SEEN(k, cl->a[j]);
        if (any)
          vis[(size_t)k * vis_bytes + (gi >> 3)] |= (u8)(1 << (gi & 7));
      }
    }
  } else
    memset(vis, 0xFF, rows * vis_bytes);
  u32 cvis_bytes = (u32)(w.recs.n + 7) / 8;
  cvis_bytes += (u32)(-(int)cvis_bytes & 3);
  if (cvis_bytes < 4)
    cvis_bytes = 4;
  u8 *cvis = malloc((size_t)rows * cvis_bytes);
  memset(cvis, 0xFF, (size_t)rows * cvis_bytes);
  if (ncl) {
    memset(cvis, 0, (size_t)rows * cvis_bytes);
    for (int i = 0; i < w.recs.n; i++) {
      const iset *cl = &w.recs.a[i].clusters;
      for (int k = 0; k < ncl; k++) {
        int any = w.recs.a[i].model != 0;
        for (int j = 0; j < cl->n && !any; j++)
          any = cl->a[j] < ncl && SEEN(k, cl->a[j]);
        if (any)
          cvis[(size_t)k * cvis_bytes + (i >> 3)] |= (u8)(1 << (i & 7));
      }
    }
  }
#undef SEEN

  /* ---- the file ---- */
  bytes sec_mat = {0}, sec_tex = {0}, sec_chunk = {0}, sec_group = {0}, sec_lm = {0}, sec_lmdim = {0};
  bytes sec_plane = {0}, sec_node = {0}, sec_leaf = {0}, sec_fog = {0};
  for (int i = 0; i < mats.n; i++) {
    struct mat *m = &mats.a[i];
    put_name(&sec_mat, m->name, 64);
    put_u16(&sec_mat, (u32)m->tex), put_u16(&sec_mat, (u32)m->flags);
    put_u8(&sec_mat, (u32)m->blend), put_u8(&sec_mat, (u32)m->alpha), put_u8(&sec_mat, (u32)m->cull), put_u8(&sec_mat, 0);
    put_u32(&sec_mat, (u32)m->surf), put_u32(&sec_mat, (u32)m->cont);
    put_f(&sec_mat, m->has_area && m->area != 0 ? sqrt(m->texels / m->area) : 1.0);
    put_zero(&sec_mat, 12);
  }
  for (int i = 0; i < b.nlm; i++) {
    const struct page *pg = &b.lm[i];
    put_u16(&sec_lmdim, (u32)pg->side), put_u16(&sec_lmdim, (u32)pg->side), put_u32(&sec_lmdim, (u32)sec_lm.n);
    for (int k = 0; k < pg->side * pg->side; k++) {
      const u8 *p = pg->rgb + 3 * k;
      int mx = p[0] > p[1] ? p[0] : p[1];
      if (p[2] > mx)
        mx = p[2];
      put_u8(&sec_lm, (u32)(128 + (mx < 127 ? mx : 127)));
    }
    pad16(&sec_lm);
  }
  /* the fog volumes */
  const u8 *fl = b.l[L_FOGS];
  for (u32 i = 0; i + 72 <= b.ln[L_FOGS]; i += 72) {
    char fname[65];
    memcpy(fname, fl + i, 64);
    fname[64] = 0;
    lower_slash(fname);
    s32 brush = rds32(fl + i + 64), vis_side = rds32(fl + i + 68);
    s32 first = rds32(b.l[L_BRUSHES] + 12 * brush);
    double pl[6];
    for (int k = 0; k < 6; k++)
      pl[k] = plane_f(&b, rds32(b.l[L_BRUSHSIDES] + 8 * (first + k)), 3);
    double colour[3] = {1, 1, 1}, depth = 300;
    struct shader *sh = script_get(&scripts, fname);
    for (int li = 0; sh && li < sh->lines.n; li++) {
      struct sline L = sh->lines.a[li];
      if (!ieq(TOK(sh, L, 0), "fogparms"))
        continue;
      double nums[64];
      int nn = 0;
      for (int k = 1; k < L.n; k++) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s", TOK(sh, L, k));
        for (char *p = buf; *p; p++)
          if (*p == '(' || *p == ')')
            *p = ' ';
        for (char *p = strtok(buf, " \t\r\n\f\v"); p && nn < 64; p = strtok(0, " \t\r\n\f\v")) {
          double v;
          if (pyfloat(p, &v))
            nums[nn++] = v;
        }
      }
      if (nn >= 4)
        colour[0] = nums[0], colour[1] = nums[1], colour[2] = nums[2], depth = nums[3] > 1.0 ? nums[3] : 1.0;
    }
    for (int k = 0; k < 3; k++)
      put_f(&sec_fog, colour[k]);
    put_f(&sec_fog, depth);
    if (vis_side < 0) {
      put_zero(&sec_fog, 16), put_u32(&sec_fog, 0);
    } else {
      int pn = rds32(b.l[L_BRUSHSIDES] + 8 * (first + vis_side));
      for (int k = 0; k < 4; k++)
        put_f(&sec_fog, -(double)plane_f(&b, pn, k));
      put_u32(&sec_fog, 1);
    }
    put_f(&sec_fog, -pl[0]), put_f(&sec_fog, -pl[2]), put_f(&sec_fog, -pl[4]);
    put_f(&sec_fog, pl[1]), put_f(&sec_fog, pl[3]), put_f(&sec_fog, pl[5]);
    put_zero(&sec_fog, 4);
  }
  put(&sec_plane, b.l[L_PLANES], (u32)b.nplanes * 16);
  for (int i = 0; i < b.nnodes; i++)
    for (int k = 0; k < 3; k++)
      put_u32(&sec_node, (u32)node_i(&b, i, k));
  for (int i = 0; i < b.nleafs; i++)
    put_u16(&sec_leaf, (u32)leaf_i(&b, i, 0)), put_u16(&sec_leaf, (u32)leaf_i(&b, i, 1));
  for (int i = 0; i < textures.n; i++) {
    put_name(&sec_tex, texnames.a[i], 64);
    put_u16(&sec_tex, (u32)textures.a[i].w), put_u16(&sec_tex, (u32)textures.a[i].h);
    put_u8(&sec_tex, (u32)textures.a[i].alpha), put_zero(&sec_tex, 3);
  }
  for (int i = 0; i < grecs.n; i++) {
    struct grec *g = &grecs.a[i];
    for (int k = 0; k < 3; k++)
      put_f(&sec_group, g->mins[k]);
    put_u32(&sec_group, (u32)g->first);
    for (int k = 0; k < 3; k++)
      put_f(&sec_group, g->maxs[k]);
    put_u32(&sec_group, (u32)g->n);
  }
  /* the offsets */
  enum { S_MAT, S_TEX, S_CHUNK, S_GROUP, S_MODEL, S_LM, S_LMDIM, S_VIS, S_PLANE, S_NODE, S_LEAF, S_CVIS, S_FOG, S_DATA, NSEC };
  u32 size[NSEC] = {(u32)sec_mat.n, (u32)sec_tex.n, 48u * (u32)w.recs.n, (u32)sec_group.n, (u32)sec_model.n,
                    (u32)sec_lm.n, (u32)sec_lmdim.n, rows * vis_bytes, (u32)sec_plane.n, (u32)sec_node.n,
                    (u32)sec_leaf.n, rows * cvis_bytes, (u32)sec_fog.n, (u32)w.blob.n};
  u32 off[NSEC], o = 128;
  for (int k = 0; k < NSEC; k++)
    off[k] = o, o += (size[k] + 15) & ~15u;
  u32 total = o;
  for (int i = 0; i < w.recs.n; i++) {
    struct chunkrec *c = &w.recs.a[i];
    for (int k = 0; k < 3; k++)
      put_f(&sec_chunk, c->mins[k]);
    put_u16(&sec_chunk, (u32)c->mat), put_u16(&sec_chunk, (u32)c->lm);
    for (int k = 0; k < 3; k++)
      put_f(&sec_chunk, c->maxs[k]);
    put_u16(&sec_chunk, (u32)c->nverts), put_u8(&sec_chunk, (u32)c->flags), put_u8(&sec_chunk, (u32)c->fog);
    put_u32(&sec_chunk, off[S_DATA] + c->off), put_u16(&sec_chunk, c->qwc), put_u16(&sec_chunk, (u32)c->nbatch);
    for (int k = 0; k < 3; k++)
      put_u8(&sec_chunk, (u32)c->cone[k]);
    put_u8(&sec_chunk, (u32)c->cone[3]), put_u8(&sec_chunk, (u32)c->area), put_zero(&sec_chunk, 3);
  }
  bytes head = {0};
  put(&head, "PMAP", 4);
  u32 hv[] = {11, (u32)mats.n, (u32)textures.n, (u32)w.recs.n, (u32)b.nmodels, (u32)b.nlm, (u32)ncl, vis_bytes,
              (u32)b.nplanes, (u32)b.nnodes, (u32)b.nleafs, off[S_MAT], off[S_TEX], off[S_CHUNK], off[S_MODEL],
              off[S_LM], off[S_VIS], off[S_PLANE], off[S_NODE], off[S_LEAF], off[S_DATA], total};
  for (u32 k = 0; k < sizeof hv / sizeof *hv; k++)
    put_u32(&head, hv[k]);
  put_f(&head, 1.0 / q);
  u32 hv2[] = {BATCH, (u32)grecs.n, off[S_GROUP], off[S_CVIS], cvis_bytes, off[S_LMDIM], (u32)sec_fog.n / 64, off[S_FOG]};
  for (u32 k = 0; k < sizeof hv2 / sizeof *hv2; k++)
    put_u32(&head, hv2[k]);
  put_zero(&head, 128 - (u32)head.n);
  cv_file *f = r ? 0 : cv_open(out, 1);
  if (!f)
    r = -1;
  else {
    const void *secs[NSEC] = {sec_mat.a, sec_tex.a, sec_chunk.a, sec_group.a, sec_model.a, sec_lm.a, sec_lmdim.a,
                              vis, sec_plane.a, sec_node.a, sec_leaf.a, cvis, sec_fog.a, w.blob.a};
    static const u8 zero[16];
    r = cv_write(f, head.a, 128);
    for (int k = 0; k < NSEC && !r; k++)
      if ((size[k] && cv_write(f, secs[k], size[k])) || cv_write(f, zero, (16 - size[k] % 16) % 16))
        r = -1;
    cv_close(f);
    if (r)
      cv_remove(out);
  }
  cv_progress(mapname, (u32)b.nsurf, (u32)b.nsurf);
  if (!r)
    cv_log("%s: %u bytes, %d chunks in %d groups, %d materials, %d textures, %d lightmap pages", out, total,
           w.recs.n, grecs.n, mats.n, textures.n, b.nlm);

  /* everything back */
  free(head.a), free(sec_mat.a), free(sec_tex.a), free(sec_chunk.a), free(sec_group.a), free(sec_model.a);
  free(sec_lm.a), free(sec_lmdim.a), free(sec_plane.a), free(sec_node.a), free(sec_leaf.a), free(sec_fog.a);
  free(vis), free(cvis), free(w.blob.a);
  for (int i = 0; i < w.recs.n; i++)
    free(w.recs.a[i].clusters.a);
  free(w.recs.a);
  for (int i = 0; i < grecs.n; i++)
    free(grecs.a[i].cl.a);
  free(grecs.a), free(gt.a), free(sprites.a);
  for (int i = 0; i < mats.n; i++)
    free(mats.a[i].name), free(mats.a[i].texname);
  free(mats.a);
  for (int i = 0; i < texnames.n; i++)
    free(texnames.a[i]);
  free(texnames.a), free(textures.a);
  for (int i = 0; i < tex_seen.n; i++)
    free(tex_seen.a[i]);
  free(tex_seen.a), free(tex_seen_i.a), free(mat_index);
  for (int i = 0; i < b.nsurf; i++)
    free(scl[i].a), free(sar[i].a);
  free(scl), free(sar), free(surf_model);
  for (int i = 0; i < b.nlm; i++)
    free(b.lm[i].rgb);
  free(b.lm), free(b.verts), free(b.surf);
  scripts_free(&scripts);
  pk3_close(texpak);
  free(data);
  pk3_close(paks);
  return r;
}
