/* ps2conv on the PC: what the Makefile's `pak` and map targets run.

     ps2conv pak OUT.pk3 GAMEDIR [MAX] the textures (was tools/ps2pak.py; MAX 128 or 256)
     ps2conv snd OUT.pk3 GAMEDIR       the sounds (was tools/ps2snd.py)
     ps2conv tex OUT.bin PS2TEX.pk3    the file of images (was tools/ps2bin.py tex)
     ps2conv mdl OUT.bin GAMEDIR       the file of models (was tools/ps2bin.py mdl)
     ps2conv map OUT.pmap GAMEDIR MAP PS2TEX.pk3   a map (was tools/ps2map.py) */
#include "conv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if ((argc == 4 || argc == 5) && !strcmp(argv[1], "pak"))
    return cv_pak(argv[2], argv[3], argc == 5 ? atoi(argv[4]) : 128) ? 1 : 0;
  if (argc == 4 && !strcmp(argv[1], "snd"))
    return cv_snd(argv[2], argv[3]) ? 1 : 0;
  if (argc == 4 && (!strcmp(argv[1], "tex") || !strcmp(argv[1], "mdl")))
    return cv_bin(argv[2], argv[1][0] == 't' ? 1 : 2, argv[3]) ? 1 : 0;
  if (argc == 6 && !strcmp(argv[1], "map"))
    return cv_map(argv[2], argv[3], argv[4], argv[5]) ? 1 : 0;
  fprintf(stderr, "usage: ps2conv snd OUT.pk3 GAMEDIR | tex OUT.bin PS2TEX.pk3 | mdl OUT.bin GAMEDIR | map OUT.pmap GAMEDIR MAP PS2TEX.pk3\n");
  return 64;
}
