# q3view: the PS2 renderer's world of a map, flown along a recorded camera
# (q3view.c's head; PLAN-PS2.md, phase 1).
#
#   make [OUT=<dir>]     $(OUT)/q3view.elf (build/apps/q3view), an MXOS program
#   make map MAP=q3dm6   $(OUT)/$(MAP).pmap from the paks in Q3DATA

HERE     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
Q3       := $(abspath $(HERE)/../..)
PS2MAX   ?= $(abspath $(Q3)/../..)
MAXSDK   ?= $(PS2MAX)/libs/maxsdk
OUT      ?= $(PS2MAX)/build/apps/q3view
REN      := $(Q3)/code/renderer_ps2
VSM_DIRS := $(REN)/vu
all: $(OUT)/q3view.elf
include $(MAXSDK)/mk/maxsdk.mk

CFLAGS += -I$(REN) -mvu0 -ffast-math -cl-single-precision-constant
OBJS   := $(OUT)/q3view.o $(OUT)/rp_world.o

$(OUT)/q3view.o: $(HERE)/q3view.c $(REN)/rp_world.h $(MX_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/rp_world.o: $(REN)/rp_world.c $(REN)/rp_world.h $(REN)/vu/q3vu.h $(MX_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/q3view.elf: $(OBJS) $(VU_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD)
	$(call link,$@,$(MX_CRT0) $(OBJS) $(VU_OBJS) $(MX_LIB))

Q3DATA ?= $(PS2MAX)/build/pcsx2-test/baseq3
MAP    ?= q3dm6
map:
	@mkdir -p $(OUT)
	python3 $(Q3)/tools/ps2map.py $(OUT)/$(MAP).pmap "$(Q3DATA)" $(MAP) --stats

clean:
	rm -rf $(OUT)/q3view.elf $(OBJS) $(OUT)/vu

.PHONY: all map clean
