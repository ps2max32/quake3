/* ps2conv's files: stdio and dirent on the PC, maxsdk's mx_fio on the PS2 */
#include "conv.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void progress_none(const char *what, u32 done, u32 total) { (void)what, (void)done, (void)total; }
void (*cv_progress)(const char *what, u32 done, u32 total) = progress_none;
volatile int cv_cancel;

#ifdef CV_PS2
#include <mx/debug.h>
#include <mx/mx.h>
#include <mx/sif.h>

struct cv_file {
  int fd;
};
cv_file *cv_open(const char *path, int write) {
  int fd = mx_fio_open(path, write ? MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC : MX_FIO_RDONLY);
  if (fd < 0)
    return 0;
  cv_file *f = malloc(sizeof *f);
  if (!f) {
    mx_fio_close(fd);
    return 0;
  }
  f->fd = fd;
  return f;
}
int cv_read(cv_file *f, void *buf, u32 n) {
  u32 got = 0;
  while (got < n) {
    int r = mx_fio_read(f->fd, (u8 *)buf + got, n - got);
    if (r < 0)
      return r;
    if (r == 0)
      break;
    got += (u32)r;
  }
  return (int)got;
}
int cv_write(cv_file *f, const void *buf, u32 n) { return mx_fio_write(f->fd, buf, n) == 0 ? 0 : -1; }
int cv_seek(cv_file *f, u32 off) { return mx_fio_lseek(f->fd, (s32)off, MX_FIO_SEEK_SET) == (int)off ? 0 : -1; }
u32 cv_size(cv_file *f) {
  int at = mx_fio_lseek(f->fd, 0, MX_FIO_SEEK_CUR);
  int n = mx_fio_lseek(f->fd, 0, MX_FIO_SEEK_END);
  mx_fio_lseek(f->fd, at, MX_FIO_SEEK_SET);
  return n < 0 ? 0 : (u32)n;
}
void cv_close(cv_file *f) {
  if (f) {
    mx_fio_close(f->fd);
    free(f);
  }
}
char *cv_list(const char *dir, int *n) {
  int d = mx_fio_dopen(dir);
  if (d < 0)
    return 0;
  u32 cap = 4096, used = 0;
  char *out = malloc(cap);
  char name[256];
  u32 size;
  *n = 0;
  while (out && mx_fio_dread(d, name, sizeof name, &size) > 0) {
    u32 l = (u32)strlen(name) + 1;
    if (used + l > cap) {
      char *m = realloc(out, cap * 2);
      if (!m) {
        free(out);
        out = 0;
        break;
      }
      out = m, cap *= 2;
    }
    memcpy(out + used, name, l);
    used += l, (*n)++;
  }
  mx_fio_dclose(d);
  return out;
}
void cv_mkdir(const char *path) { mx_fio_mkdir(path, 0777); }
void cv_remove(const char *path) { mx_fio_remove(path); }
void cv_log(const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  mx_vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  mx_printf("ps2conv: %s\n", b);
}
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

struct cv_file {
  FILE *f;
};
cv_file *cv_open(const char *path, int write) {
  FILE *fp = fopen(path, write ? "wb" : "rb");
  if (!fp)
    return 0;
  cv_file *f = malloc(sizeof *f);
  f->f = fp;
  return f;
}
int cv_read(cv_file *f, void *buf, u32 n) {
  size_t r = fread(buf, 1, n, f->f);
  return ferror(f->f) ? -1 : (int)r;
}
int cv_write(cv_file *f, const void *buf, u32 n) { return fwrite(buf, 1, n, f->f) == n ? 0 : -1; }
int cv_seek(cv_file *f, u32 off) { return fseek(f->f, (long)off, SEEK_SET); }
u32 cv_size(cv_file *f) {
  long at = ftell(f->f);
  fseek(f->f, 0, SEEK_END);
  long n = ftell(f->f);
  fseek(f->f, at, SEEK_SET);
  return (u32)n;
}
void cv_close(cv_file *f) {
  if (f) {
    fclose(f->f);
    free(f);
  }
}
char *cv_list(const char *dir, int *n) {
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  size_t cap = 4096, used = 0;
  char *out = malloc(cap);
  struct dirent *e;
  *n = 0;
  while ((e = readdir(d))) {
    size_t l = strlen(e->d_name) + 1;
    if (used + l > cap)
      out = realloc(out, cap *= 2);
    memcpy(out + used, e->d_name, l);
    used += l, (*n)++;
  }
  closedir(d);
  return out;
}
void cv_mkdir(const char *path) { mkdir(path, 0777); }
void cv_remove(const char *path) { unlink(path); }
void cv_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("ps2conv: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}
#endif
