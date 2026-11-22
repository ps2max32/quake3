# ps2conv for the PC (the PS2's is linked into the converter program on
# maxsdk). It needs zlib; the JPEG decoder is quake3's jpeg-9f, as on the
# PS2. OUT is where it goes.
HERE := $(dir $(lastword $(MAKEFILE_LIST)))
OUT  ?= $(HERE)../../../../build/apps/quake3/ps2conv
JPEG := $(HERE)../../code/thirdparty/jpeg-9f
SRCS := main io pk3 snd bin map pak
JSRCS := $(filter-out $(JPEG)/jmemansi.c $(JPEG)/jmemdos.c $(JPEG)/jmemmac.c $(JPEG)/jmemname.c,$(wildcard $(JPEG)/j*.c))
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
$(OUT)/ps2conv: $(SRCS:%=$(HERE)%.c) $(HERE)conv.h $(OUT)/libjpeg.a
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I$(JPEG) -o $@ $(SRCS:%=$(HERE)%.c) $(OUT)/libjpeg.a -lz -lm
$(OUT)/libjpeg.a: $(JSRCS)
	@mkdir -p $(OUT)/jpeg
	cd $(OUT)/jpeg && $(CC) -O2 -w -c $(abspath $(JSRCS))
	ar rcs $@ $(OUT)/jpeg/*.o
