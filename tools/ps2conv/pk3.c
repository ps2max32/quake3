/* The paks: read (all pak*.pk3 of a directory in the order of their names,
   a later one's file over an earlier one's, as the game's file system), and
   written (stored, no compression). */
#include "conv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

struct entry {
  char *name; /* lower case */
  u16 pak, method;
  u32 local, csize, usize, crc;
};
struct pk3_set {
  cv_file **paks;
  int npaks;
  struct entry *e;
  int n, cap;
  char *names; /* the names' bytes */
};

static u32 le16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

static int by_name(const void *a, const void *b) {
  return strcmp(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

/* the central directory of one pak into s (its files replace ones of the
   same name) */
static int add_pak(pk3_set *s, cv_file *f) {
  u32 size = cv_size(f);
  u8 tail[22 + 1024];
  u32 tn = size < sizeof tail ? size : sizeof tail;
  if (tn < 22 || cv_seek(f, size - tn) || cv_read(f, tail, tn) != (int)tn)
    return -1;
  int at = -1;
  for (int i = (int)tn - 22; i >= 0 && at < 0; i--)
    if (le32(tail + i) == 0x06054b50)
      at = i;
  if (at < 0)
    return -1;
  u32 n = le16(tail + at + 10), cs = le32(tail + at + 12), co = le32(tail + at + 16);
  u8 *cd = malloc(cs ? cs : 1);
  if (!cd || cv_seek(f, co) || cv_read(f, cd, cs) != (int)cs) {
    free(cd);
    return -1;
  }
  int pak = s->npaks, n0 = s->n; /* the earlier paks' entries, sorted */
  const u8 *p = cd;
  for (u32 i = 0; i < n; i++) {
    if (p + 46 > cd + cs || le32(p) != 0x02014b50)
      break;
    u32 nl = le16(p + 28), xl = le16(p + 30), cl = le16(p + 32);
    if (p + 46 + nl > cd + cs)
      break;
    if (nl && p[46 + nl - 1] != '/') {
      char *name = malloc(nl + 1);
      for (u32 k = 0; k < nl; k++) {
        char c = (char)p[46 + k];
        name[k] = c >= 'A' && c <= 'Z' ? c + 32 : c == '\\' ? '/' : c;
      }
      name[nl] = 0;
      struct entry e = {name, (u16)pak, (u16)le16(p + 10), le32(p + 42), le32(p + 20), le32(p + 24), le32(p + 16)};
      /* the same name in an earlier pak: this one instead */
      struct entry *old = n0 ? bsearch(&e, s->e, (size_t)n0, sizeof *s->e, by_name) : 0;
      if (old) {
        free(old->name);
        *old = e;
      } else {
        if (s->n == s->cap) {
          s->cap = s->cap ? s->cap * 2 : 1024;
          s->e = realloc(s->e, (size_t)s->cap * sizeof *s->e);
        }
        s->e[s->n++] = e;
      }
    }
    p += 46 + nl + xl + cl;
  }
  free(cd);
  /* this pak's own new names among the old ones */
  qsort(s->e, (size_t)s->n, sizeof *s->e, by_name);
  /* a name twice in one pak: the later one's (as a lookup would find either) */
  int k = 0;
  for (int i = 0; i < s->n; i++) {
    if (k && !strcmp(s->e[k - 1].name, s->e[i].name)) {
      free(s->e[k - 1].name);
      s->e[k - 1] = s->e[i];
    } else
      s->e[k++] = s->e[i];
  }
  s->n = k;
  s->paks = realloc(s->paks, (size_t)(s->npaks + 1) * sizeof *s->paks);
  s->paks[s->npaks++] = f;
  return 0;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

pk3_set *pk3_open(const char *dir) {
  int n;
  char *list = cv_list(dir, &n);
  if (!list)
    return 0;
  char **names = malloc((size_t)(n ? n : 1) * sizeof *names);
  int k = 0;
  for (char *s = list; n--; s += strlen(s) + 1) {
    u32 l = (u32)strlen(s);
    char low[256];
    if (l >= sizeof low)
      continue;
    for (u32 i = 0; i <= l; i++)
      low[i] = s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i];
    if (!strncmp(low, "pak", 3) && l > 4 && !strcmp(low + l - 4, ".pk3"))
      names[k++] = s;
  }
  qsort(names, (size_t)k, sizeof *names, cmp_str);
  pk3_set *set = calloc(1, sizeof *set);
  for (int i = 0; i < k; i++) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, names[i]);
    cv_file *f = cv_open(path, 0);
    if (!f || add_pak(set, f)) {
      cv_log("%s: not a pk3 this reads", path);
      cv_close(f);
    }
  }
  free(names);
  free(list);
  return set;
}

pk3_set *pk3_open_file(const char *path) {
  cv_file *f = cv_open(path, 0);
  if (!f)
    return 0;
  pk3_set *set = calloc(1, sizeof *set);
  if (add_pak(set, f)) {
    cv_close(f);
    pk3_close(set);
    return 0;
  }
  return set;
}

void pk3_close(pk3_set *s) {
  if (!s)
    return;
  for (int i = 0; i < s->npaks; i++)
    cv_close(s->paks[i]);
  for (int i = 0; i < s->n; i++)
    free(s->e[i].name);
  free(s->paks);
  free(s->e);
  free(s);
}

int pk3_count(const pk3_set *s) { return s->n; }
const char *pk3_name(const pk3_set *s, int i) { return s->e[i].name; }
u32 pk3_size_of(const pk3_set *s, int i) { return s->e[i].usize; }
int pk3_pak_of(const pk3_set *s, int i) { return s->e[i].pak; }
int pk3_find(const pk3_set *s, const char *name) {
  struct entry key = {(char *)name, 0, 0, 0, 0, 0, 0};
  const struct entry *e = s->n ? bsearch(&key, s->e, (size_t)s->n, sizeof *s->e, by_name) : 0;
  return e ? (int)(e - s->e) : -1;
}

u8 *pk3_read(pk3_set *s, int i, u32 *n) {
  const struct entry *e = &s->e[i];
  cv_file *f = s->paks[e->pak];
  u8 h[30];
  if (cv_seek(f, e->local) || cv_read(f, h, 30) != 30 || le32(h) != 0x04034b50)
    return 0;
  if (cv_seek(f, e->local + 30 + le16(h + 26) + le16(h + 28)))
    return 0;
  u8 *out = malloc(e->usize + 1);
  if (!out)
    return 0;
  if (e->method == 0) {
    if (cv_read(f, out, e->usize) != (int)e->usize)
      goto bad;
  } else if (e->method == 8) {
    u8 *in = malloc(e->csize ? e->csize : 1);
    if (!in || cv_read(f, in, e->csize) != (int)e->csize) {
      free(in);
      goto bad;
    }
    z_stream z;
    memset(&z, 0, sizeof z);
    int ok = inflateInit2(&z, -15) == Z_OK;
    if (ok) {
      z.next_in = in, z.avail_in = e->csize, z.next_out = out, z.avail_out = e->usize;
      int r = inflate(&z, Z_FINISH);
      ok = (r == Z_STREAM_END || (r == Z_BUF_ERROR && !z.avail_out)) && z.total_out == e->usize;
      inflateEnd(&z);
    }
    free(in);
    if (!ok)
      goto bad;
  } else
    goto bad;
  if ((u32)crc32(0, out, e->usize) != e->crc)
    goto bad;
  out[e->usize] = 0;
  *n = e->usize;
  return out;
bad:
  free(out);
  cv_log("%s: cannot be read", e->name);
  return 0;
}

/* ---- written ---- */
struct pk3_out {
  cv_file *f;
  u32 at;
  struct {
    char *name;
    u32 off, size, crc;
  } *e;
  int n, cap;
};

static void put16(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }

pk3_out *pk3_create(const char *path) {
  cv_file *f = cv_open(path, 1);
  if (!f)
    return 0;
  pk3_out *o = calloc(1, sizeof *o);
  o->f = f;
  return o;
}

int pk3_add(pk3_out *o, const char *name, const void *data, u32 n) {
  u32 nl = (u32)strlen(name), crc = (u32)crc32(0, data, n);
  u8 h[30];
  memset(h, 0, sizeof h);
  put32(h, 0x04034b50), put16(h + 4, 10), put32(h + 14, crc), put32(h + 18, n), put32(h + 22, n), put16(h + 26, nl);
  if (cv_write(o->f, h, 30) || cv_write(o->f, name, nl) || cv_write(o->f, data, n))
    return -1;
  if (o->n == o->cap) {
    o->cap = o->cap ? o->cap * 2 : 256;
    o->e = realloc(o->e, (size_t)o->cap * sizeof *o->e);
  }
  o->e[o->n].name = strdup(name);
  o->e[o->n].off = o->at, o->e[o->n].size = n, o->e[o->n].crc = crc;
  o->n++;
  o->at += 30 + nl + n;
  return 0;
}

int pk3_finish(pk3_out *o) {
  int r = 0;
  u32 start = o->at, cs = 0;
  for (int i = 0; i < o->n; i++) {
    u32 nl = (u32)strlen(o->e[i].name);
    u8 h[46];
    memset(h, 0, sizeof h);
    put32(h, 0x02014b50), put16(h + 4, 20), put16(h + 6, 10), put32(h + 16, o->e[i].crc);
    put32(h + 20, o->e[i].size), put32(h + 24, o->e[i].size), put16(h + 28, nl), put32(h + 42, o->e[i].off);
    if (cv_write(o->f, h, 46) || cv_write(o->f, o->e[i].name, nl))
      r = -1;
    cs += 46 + nl;
    free(o->e[i].name);
  }
  u8 end[22];
  memset(end, 0, sizeof end);
  put32(end, 0x06054b50), put16(end + 8, (u32)o->n), put16(end + 10, (u32)o->n), put32(end + 12, cs);
  put32(end + 16, start);
  if (cv_write(o->f, end, 22))
    r = -1;
  cv_close(o->f);
  free(o->e);
  free(o);
  return r;
}
