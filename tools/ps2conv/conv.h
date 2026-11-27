/* ps2conv: Quake 3's data made into what the PS2 renderer reads (the
   textures as paletted .p8, the sounds as SPU ADPCM, the files of images
   and models, the maps), from the pak*.pk3 of a game directory. One source
   for the PC (tools/ps2conv/Makefile: ps2conv, the Makefile's `pak` and map
   targets) and the PS2 (ps2.c: CONVERT.ELF, the game's setup= program, which
   converts the user's data before the game's first start, with a progress
   screen and Stop).

   The formats are described in the parts that write them:
   pak.c  .p8 (ps2tex.pk3)      snd.c  .adp (ps2snd.pk3)
   bin.c  PBIN (ps2tex.bin, ps2mdl.bin)     map.c  PMAP (maps/<map>.pmap)

   Files go through the functions below (io.c: stdio on the PC, maxsdk's
   mx_fio on the PS2). */
#ifndef PS2CONV_H
#define PS2CONV_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;

/* ---- files (io.c) ---- */
typedef struct cv_file cv_file;
cv_file *cv_open(const char *path, int write); /* 0 when it fails */
int cv_read(cv_file *f, void *buf, u32 n);      /* bytes read, < 0 an error */
int cv_write(cv_file *f, const void *buf, u32 n); /* 0, or < 0 */
int cv_seek(cv_file *f, u32 off);
u32 cv_size(cv_file *f);
void cv_close(cv_file *f);
/* the names of a directory's files, one after the other with a 0 after each
   (malloc'd; the caller frees it), their number in *n; 0 when it fails */
char *cv_list(const char *dir, int *n);
void cv_mkdir(const char *path);
void cv_remove(const char *path);
/* progress and messages: what is being done (a part's name), done of total */
extern void (*cv_progress)(const char *what, u32 done, u32 total);
void cv_log(const char *fmt, ...);
extern volatile int cv_cancel; /* set by the caller: the work stops, < 0 */

/* ---- a game directory's paks (pk3.c) ---- */
/* the files of all pak*.pk3 of dir, a later pak's file over an earlier
   one's (as the game's file system), names in lower case */
typedef struct pk3_set pk3_set;
pk3_set *pk3_open(const char *dir);
/* Whether a file of a game's directory is one of its paks, as the game's file
   system takes them: any .pk3 but the ones this converter writes (ps2*.pk3) */
int pk3_is_pak(const char *name);
/* Paks' names compared as the game's file system orders them (case aside) */
int pk3_name_cmp(const char *a, const char *b);
void pk3_close(pk3_set *s);
int pk3_count(const pk3_set *s);
const char *pk3_name(const pk3_set *s, int i); /* sorted by name */
u32 pk3_size_of(const pk3_set *s, int i);
int pk3_pak_of(const pk3_set *s, int i); /* which pak, in their order */
int pk3_find(const pk3_set *s, const char *lower_name); /* -1: none */
/* the file's bytes (malloc'd, one 0 after them), its size in *n; 0 when it fails */
u8 *pk3_read(pk3_set *s, int i, u32 *n);

/* one pk3 file of its own (ps2tex.pk3 read by bin.c and map.c) */
pk3_set *pk3_open_file(const char *path);

/* a pk3 written: its files stored, not compressed (the PS2 reads them
   as they are) */
typedef struct pk3_out pk3_out;
pk3_out *pk3_create(const char *path);
int pk3_add(pk3_out *o, const char *name, const void *data, u32 n);
int pk3_finish(pk3_out *o); /* the directory written, closed: 0, or < 0 */

/* ---- the parts ---- */
int cv_snd(const char *out_pk3, const char *gamedir);
/* kind 1: the images of a ps2tex.pk3 (src); 2: the models of a game directory */
int cv_bin(const char *out, int kind, const char *src);
/* the textures as .p8 (max: the largest side, 128 or 256) */
int cv_pak(const char *out_pk3, const char *gamedir, int max);
/* a map (maps/<map>.bsp of the game's paks) with the textures' sizes of ps2tex.pk3 */
int cv_map(const char *out, const char *gamedir, const char *map, const char *texpk3);

#endif
