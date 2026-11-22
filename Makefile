# Quake III Arena (ioquake3) on maxsdk: the PS2 port.
#
#   make [OUT=<dir>]     $(OUT)/quake3.elf
#   make run             quake3.elf in PCSX2 with the game data in
#                        build/pcsx2-test/baseq3 (Q3DATA: the retail
#                        baseq3 directory, copied there once)
#   make pak             build/pcsx2-test/baseq3/ps2tex.pk3: the textures
#                        as 8-bit paletted ones, and ps2snd.pk3: the
#                        sounds as SPU ADPCM; for REN=ps2 also ps2tex.bin
#                        and ps2mdl.bin, the images and the models as files
#                        it maps (all by tools/ps2conv on the PC)
#   make map MAP=<name>  build/pcsx2-test/baseq3/maps/<name>.pmap: a map as
#                        REN=ps2 draws it (tools/ps2conv)
#   make convert         $(OUT)/CONVERT.ELF: tools/ps2conv on the PS2, the
#                        game's setup= (all of the above, made of the
#                        user's data, with a progress screen)
#
# The engine (qcommon, server, client, botlib, renderergl1) is linked with
# the game modules (game, cgame, q3_ui) built natively: each module is
# linked into one object whose symbols are local except its vmMain and
# dllEntry, renamed <module>_vmMain and <module>_dllEntry, so the three
# copies of q_shared.c and bg_*.c do not clash (code/ps2/sys_ps2.c finds
# them in Sys_LoadGameDll).
#
# REN: the renderer. gl1 (the default): renderergl1 through libs/libgl, an
# OpenGL 1.1 subset on the GS (code/ps2/ps2_glimp.c sets the qgl* to it).
# ps2: code/renderer_ps2, the PS2's own on maxsdk (PLAN-PS2.md), built in
# $(OUT)-ps2; it needs make pak's files and make map's of the map played.

HERE     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PS2MAX   ?= $(abspath $(HERE)/../..)
MAXSDK   ?= $(PS2MAX)/libs/maxsdk
REN      ?= gl1
OUT      ?= $(PS2MAX)/build/apps/quake3$(if $(filter-out gl1,$(REN)),-$(REN))
all: $(OUT)/quake3.elf
ifeq ($(REN),gl1)
MXGL     ?= $(PS2MAX)/libs/libgl
# beam racing's records: a frame of Quake III's stream is up to ~350 KB
# (demo four; three quarters of a record), more than libgl's default
MXGL_REC_BYTES ?= 524288
# the frames' vertices: demo four's heaviest frame takes 266 KB of the default 512 KB's
# positions part (300 KB); the models' streams (q3model.hlsl) come on top
MXGL_ARENA_BYTES ?= 655360
# the stages' colours on VU1 (code/ps2/q3stage.hlsl, glShaderBinary)
HLSL_DIRS  += $(HERE)/code/ps2
MXGL_PROGS := q3stage q3model
# with clipping on VU1 (libs/libgl, MXGL_VU1CLIP=1): q3stagec.vsm, the same by hand
ifeq ($(MXGL_VU1CLIP),1)
MXGL_VSM_DIR   := $(HERE)/code/ps2
MXGL_VSM_PROGS := q3stagec
endif
include $(MXGL)/mk/mxgl.mk
# q3model.hlsl keeps more values than VU1 has registers (twelve uniforms and
# the vertex's): four quadwords of data memory for the compiler's spills
$(OUT)/hlsl/q3model.vu.ll: VU_OPT_FLAGS += -vu-spill-qw=4
REN_CFLAGS := -I$(MXGL_INC) $(MXGL_DEFS)
else
REN_CFLAGS := -DPS2_REN_PS2
# its VU1 programs (tools/ps2vu.py writes code/renderer_ps2/vu/q3vu.vsm)
VSM_DIRS   += $(HERE)/code/renderer_ps2/vu
endif
# the program in VM (ld/maxsdk-vm.ld), started by mxlink; MX_MEMORY=phys:
# all of it in physical memory (32 MiB, no VM in PCSX2's recompiler)
MX_MEMORY ?= vm
include $(MAXSDK)/mk/maxsdk.mk
# in VM: Quake III in VM, libgl in core
ifeq ($(REN),gl1)
MX_FAST_OBJS = $(MXGL_OBJS)
endif

PICOLIBC ?= $(PS2MAX)/build/libs/picolibc/install
LIBMXC    = $(MAXSDK_OUT)/libmxc.a
LIBRT     = $(MAXSDK_OUT)/librt.a
GLUE      = $(MAXSDK_OUT)/libc/glue.c.o
LIBC_LIBS = $(GLUE) $(LIBMXC) $(PICOLIBC)/lib/libc.a $(MX_LIB) $(LIBRT)

C        := $(HERE)/code
JPEG     := $(C)/thirdparty/jpeg-9f
ZLIB     := $(C)/thirdparty/zlib-1.3.1
SDLINC   := $(C)/thirdparty/SDL2-2.32.8/include

# Hunk and zone (code/qcommon/common.c): Quake III 1.32's 56 and 16 MiB, in
# maxsdk's virtual memory; ioquake3's 128 and 48 do not fit in its 128 MiB
Q3_MEM   ?= -DDEF_COMHUNKMEGS=56 -DDEF_COMZONEMEGS=16
# VU0 macro mode (-mvu0: vector code on VU0) and fast math everywhere;
# unsuffixed literals are float (-cl-single-precision-constant, clang's
# twin of gcc's -fsingle-precision-constant) and the math.h functions go to
# float versions (code/ps2/ps2_math.h): the FPU has no doubles
Q3CFLAGS  = $(filter-out -ffreestanding -Wall -Wextra -Wno-unused-parameter -std=gnu11,$(CFLAGS)) \
            -std=gnu99 -w -MMD -mvu0 -ffast-math -fapprox-func -cl-single-precision-constant -nostdlibinc -isystem $(PICOLIBC)/include -D__PS2__ -DNDEBUG \
            -DPRODUCT_VERSION=\"1.36-ps2\" -DUSE_INTERNAL_JPEG -DNO_GZIP \
            -DUSE_INTERNAL_SDL_HEADERS $(Q3_MEM) \
            -I$(JPEG) -I$(ZLIB) -I$(SDLINC) -I$(C)/ps2 -I$(OUT)/ps2 $(REN_CFLAGS) -include $(C)/ps2/ps2_stdio.h -include $(C)/ps2/ps2_math.h

# ORDER=<file>: the functions named there come first in the fast text, in
# that order (a function a section; ld.lld --symbol-ordering-file): the ones
# a frame runs most next to each other, whatever else the program holds
ifneq ($(ORDER),)
Q3CFLAGS += -ffunction-sections
Q3_ORDER := --symbol-ordering-file=$(ORDER) --no-warn-symbol-ordering
endif

QCOMMON  := cm_load cm_patch cm_polylib cm_test cm_trace cmd common cvar files md4 md5 msg \
            net_chan huffman q_math q_shared unzip ioapi vm vm_interpreted
SERVER   := sv_bot sv_client sv_ccmds sv_game sv_init sv_main sv_net_chan sv_snapshot sv_world
BOTLIB   := $(basename $(notdir $(wildcard $(C)/botlib/*.c)))
CLIENT   := cl_cgame cl_console cl_input cl_keys cl_main cl_net_chan cl_parse cl_scrn \
            cl_ui cl_avi snd_adpcm snd_dma snd_mem snd_mix snd_wavelet snd_main snd_codec \
            snd_codec_wav
RCOMMON  := tr_font tr_image_bmp tr_image_jpg tr_image_pcx tr_image_png tr_image_pvr \
            tr_image_tga tr_noise puff
RGL1     := $(filter-out tr_altivec,$(basename $(notdir $(wildcard $(C)/renderergl1/*.c))))
RPS2     := $(basename $(notdir $(wildcard $(C)/renderer_ps2/*.c)))
PS2SRC   := $(basename $(notdir $(wildcard $(C)/ps2/*.c)))
ifeq ($(REN),gl1)
REN_OBJS := $(patsubst %,$(OUT)/renderercommon/%.o,$(RCOMMON)) $(patsubst %,$(OUT)/renderergl1/%.o,$(RGL1))
else
REN_OBJS := $(patsubst %,$(OUT)/renderer_ps2/%.o,$(RPS2))
PS2SRC   := $(filter-out ps2_glimp,$(PS2SRC))
endif
JPEGSRC  := $(basename $(notdir $(wildcard $(JPEG)/j*.c)))
ZLIBSRC  := $(basename $(notdir $(wildcard $(ZLIB)/*.c)))

ENGINE_OBJS := $(patsubst %,$(OUT)/qcommon/%.o,$(QCOMMON)) \
               $(patsubst %,$(OUT)/server/%.o,$(SERVER)) \
               $(patsubst %,$(OUT)/botlib/%.o,$(BOTLIB)) \
               $(patsubst %,$(OUT)/client/%.o,$(CLIENT)) \
               $(REN_OBJS) \
               $(patsubst %,$(OUT)/ps2/%.o,$(PS2SRC)) \
               $(patsubst %,$(OUT)/jpeg/%.o,$(JPEGSRC)) \
               $(patsubst %,$(OUT)/zlib/%.o,$(ZLIBSRC))

# $(call srcdir,objdir,srcdir,defines)
define srcdir
$(OUT)/$(1)/%.o: $(2)/%.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(Q3CFLAGS) $(3) -c $$< -o $$@
endef
$(eval $(call srcdir,qcommon,$(C)/qcommon,-DBOTLIB))
$(eval $(call srcdir,server,$(C)/server,-DBOTLIB))
$(eval $(call srcdir,botlib,$(C)/botlib,-DBOTLIB))
$(eval $(call srcdir,client,$(C)/client,-DBOTLIB))
$(eval $(call srcdir,renderercommon,$(C)/renderercommon,))
$(eval $(call srcdir,renderergl1,$(C)/renderergl1,))
$(eval $(call srcdir,renderer_ps2,$(C)/renderer_ps2,))
$(eval $(call srcdir,ps2,$(C)/ps2,-DBOTLIB))
$(eval $(call srcdir,jpeg,$(JPEG),))
$(eval $(call srcdir,zlib,$(ZLIB),))

# ---- the game modules -----------------------------------------------------------
MOD_SHARED := qcommon/q_math qcommon/q_shared
GAME_SRC   := $(addprefix game/,g_main ai_chat ai_cmd ai_dmnet ai_dmq3 ai_main ai_team ai_vcmd \
              bg_misc bg_pmove bg_slidemove bg_lib g_active g_arenas g_bot g_client g_cmds \
              g_combat g_items g_mem g_misc g_missile g_mover g_session g_spawn g_svcmds \
              g_target g_team g_trigger g_utils g_weapon g_syscalls) $(MOD_SHARED)
CGAME_SRC  := $(addprefix cgame/,cg_main cg_consolecmds cg_draw cg_drawtools cg_effects cg_ents \
              cg_event cg_info cg_localents cg_marks cg_particles cg_players cg_playerstate \
              cg_predict cg_scoreboard cg_servercmds cg_snapshot cg_view cg_weapons cg_syscalls) \
              $(addprefix game/,bg_misc bg_pmove bg_slidemove bg_lib) $(MOD_SHARED)
UI_SRC     := $(addprefix q3_ui/,ui_main ui_addbots ui_atoms ui_cdkey ui_cinematics ui_confirm \
              ui_connect ui_controls2 ui_credits ui_demo2 ui_display ui_gameinfo ui_ingame \
              ui_loadconfig ui_menu ui_mfield ui_mods ui_network ui_options ui_playermodel \
              ui_players ui_playersettings ui_preferences ui_qmenu ui_removebots ui_saveconfig \
              ui_serverinfo ui_servers2 ui_setup ui_sound ui_sparena ui_specifyserver ui_splevel \
              ui_sppostgame ui_spskill ui_startserver ui_team ui_teamorders ui_video) \
              ui/ui_syscalls $(addprefix game/,bg_misc bg_lib) $(MOD_SHARED)

$(eval $(call srcdir,mod/qagame,$(C),-DQAGAME))
$(eval $(call srcdir,mod/cgame,$(C),-DCGAME))
$(eval $(call srcdir,mod/ui,$(C),-DUI))

# $(call module,name,sources): a module as one object, with only its entry
# points seen from outside, and symbols at the ends of its data and its zeroed
# data: sys_ps2.c gives it its data as linked each time it is loaded (the
# object must have one .data and one .bss and no small data: the test)
define module
$(OUT)/$(1).o: $(patsubst %,$(OUT)/mod/$(1)/%.o,$(2))
	$(LLVM)/ld.lld -m elf32ltsmipn32 -r -o $$@.r $$^
	$(LLVM)/llvm-objcopy --keep-global-symbol=vmMain --keep-global-symbol=dllEntry $$@.r $$@.l
	$(LLVM)/llvm-objcopy --redefine-sym=vmMain=$(1)_vmMain --redefine-sym=dllEntry=$(1)_dllEntry \
	  --add-symbol $(1)_data_start=.data:0,global \
	  --add-symbol $(1)_data_end=.data:$$$$($(LLVM)/llvm-size -A $$@.l | awk '$$$$1 == ".data" { print $$$$2 }'),global \
	  --add-symbol $(1)_bss_start=.bss:0,global \
	  --add-symbol $(1)_bss_end=.bss:$$$$($(LLVM)/llvm-size -A $$@.l | awk '$$$$1 == ".bss" { print $$$$2 }'),global \
	  $$@.l $$@
	$(if $(3),$(LLVM)/llvm-objcopy $(3) $$@)
	test "$$$$($(LLVM)/llvm-size -A $$@ | grep -c -E '^\.(data|bss|sdata|sbss)')" = 2
	rm -f $$@.r $$@.l
endef
$(eval $(call module,qagame,$(GAME_SRC)))
# cgame's data and zeroed data (cg, cgs, cg_entities, its cvars: 1.5 MB,
# read every frame) in the core's fast part, under its large TLB pages: no
# refills (190 a frame of them in VM); the module's fresh data at each load
# works on them where they are. CGAME_FAST= for VM.
ifneq ($(REN),gl1)
CGAME_FAST ?= --rename-section .bss=.bss.mx.fast --rename-section .data=.data.mx.fast
endif
$(eval $(call module,cgame,$(CGAME_SRC),$(CGAME_FAST)))
$(eval $(call module,ui,$(UI_SRC)))

MODULES := $(OUT)/qagame.o $(OUT)/cgame.o $(OUT)/ui.o

# REN=ps2: the code a frame of play runs stays core (no TLB refills; its
# data is paged): the renderer, cgame, collision, and what of the engine and
# the client every frame goes through. Chosen with EXTRA_CFLAGS=-DQ3_VM_HIST=1
# (the refills by page) and a profile (PROF=1); CORE_TEXT= for none of it.
ifneq ($(REN),gl1)
CORE_TEXT ?= $(REN_OBJS) $(OUT)/cgame.o \
             $(patsubst %,$(OUT)/qcommon/%.o,cm_load cm_patch cm_test cm_trace cmd common cvar huffman msg \
               net_chan q_math q_shared unzip vm) \
             $(patsubst %,$(OUT)/client/%.o,cl_cgame cl_console cl_input cl_main cl_parse cl_scrn snd_dma snd_main) \
             $(patsubst %,$(OUT)/ps2/%.o,snd_ps2 sys_ps2) \
             $(patsubst %,$(OUT)/zlib/%.o,crc32 inffast inflate)
MX_FAST_TEXT_OBJS = $(CORE_TEXT)
endif

# FRAMES: end after that many frames with a picture (host:quake3.tga);
# ARGS: more of the command line (e.g. ARGS="+map q3dm1"); GLSHOT: the
# same picture at that rendered frame (loading screens included); PROF=n:
# a profile (mx/prof.h, tools/prof.py) of the frames from n to FRAMES, or
# without FRAMES of a timedemo's frames from its frame n to the end of the
# run; PERF=1: the EE's performance counters over the same frames;
# AUTOQUIT=ms: the run ends after that long whatever happens. They go into
# $(OUT)/ps2/runcfg.h, rewritten only when they change. A timedemo that
# quits by itself: ARGS="+set timedemo 1 +set nextdemo quit +demo four";
# every run ends with the timedemo frames' cycles (code/ps2/sys_ps2.c).
RUNCFG := $(OUT)/ps2/runcfg.h
$(shell mkdir -p $(OUT)/ps2; printf '#define PS2_FRAMES %s\n#define PS2_ARGS "%s"\n#define PS2_GLSHOT %s\n#define PS2_PROF %s\n#define PS2_PERF %s\n#define PS2_AUTOQUIT %s\n' \
  '$(or $(FRAMES),0)' '$(ARGS)' '$(or $(GLSHOT),0)' '$(or $(PROF),0)' '$(or $(PERF),0)' '$(or $(AUTOQUIT),0)' > $(RUNCFG).new; \
  cmp -s $(RUNCFG).new $(RUNCFG) || mv $(RUNCFG).new $(RUNCFG); rm -f $(RUNCFG).new)

REN_VU := $(if $(filter-out gl1,$(REN)),$(VU_OBJS))
$(OUT)/quake3.elf: $(ENGINE_OBJS) $(MODULES) $(MXGL_OBJS) $(REN_VU) $(HLSL_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS) $(ORDER)
	$(call link,$@,$(Q3_ORDER) --defsym=__mx_stack_size=0x100000 $(MX_CRT0) $(ENGINE_OBJS) $(MODULES) $(MXGL_OBJS) $(REN_VU) $(HLSL_OBJS) $(LIBC_LIBS))

# ---- running --------------------------------------------------------------------
Q3DATA   ?= /mnt/truenas_data_exthdd/spellen/PC/Quake III Arena/baseq3
RUNDIR   := $(PS2MAX)/build/pcsx2-test
run: $(OUT)/quake3.elf
	@mkdir -p $(RUNDIR)/baseq3
	@for f in "$(Q3DATA)"/pak*.pk3; do \
	  [ -f "$(RUNDIR)/baseq3/$$(basename "$$f")" ] || cp -v "$$f" $(RUNDIR)/baseq3/; done
ifeq ($(MX_MEMORY),vm)
	cp $(OUT)/quake3.elf $(RUNDIR)/quake3.elf && echo "host:quake3.elf" > $(RUNDIR)/mxload.txt
	PCSX2_EE=interp $(PCSX2_RUN) $(MX_LOADER) $(or $(TIMEOUT),1800)
else
	$(PCSX2_RUN) $(OUT)/quake3.elf $(or $(TIMEOUT),600)
endif

# The game's data made for the PS2 by tools/ps2conv (C: the same code the
# PS2 runs, below): the textures as 8-bit paletted ones (the renderer takes a
# mipmapped image's .p8 before the original, TEXMAX texels a side at most)
# and the sounds as ADPCM, pks next to the paks
PS2CONV := $(PS2MAX)/build/apps/quake3/ps2conv/ps2conv
$(PS2CONV): $(wildcard $(HERE)/tools/ps2conv/*.[ch])
	$(MAKE) -C $(HERE)/tools/ps2conv
TEXMAX ?= 128
pak: $(RUNDIR)/baseq3/ps2tex.pk3 $(RUNDIR)/baseq3/ps2snd.pk3 $(RUNDIR)/baseq3/ps2tex.bin $(RUNDIR)/baseq3/ps2mdl.bin
$(RUNDIR)/baseq3/ps2tex.pk3: $(PS2CONV)
	@mkdir -p $(RUNDIR)/baseq3
	$(PS2CONV) pak $@ "$(Q3DATA)" $(TEXMAX)
$(RUNDIR)/baseq3/ps2snd.pk3: $(PS2CONV)
	@mkdir -p $(RUNDIR)/baseq3
	$(PS2CONV) snd $@ "$(Q3DATA)"

# REN=ps2's files: the images and the models as it maps them, a map as it
# draws it
$(RUNDIR)/baseq3/ps2tex.bin: $(RUNDIR)/baseq3/ps2tex.pk3 $(PS2CONV)
	$(PS2CONV) tex $@ $<
$(RUNDIR)/baseq3/ps2mdl.bin: $(PS2CONV)
	@mkdir -p $(RUNDIR)/baseq3
	$(PS2CONV) mdl $@ "$(Q3DATA)"
MAP ?= q3dm6
map: $(RUNDIR)/baseq3/maps/$(MAP).pmap
$(RUNDIR)/baseq3/maps/$(MAP).pmap: $(PS2CONV) $(RUNDIR)/baseq3/ps2tex.pk3
	@mkdir -p $(dir $@)
	$(PS2CONV) map $@ "$(Q3DATA)" $(MAP) $(RUNDIR)/baseq3/ps2tex.pk3

# CONVERT.ELF: the same converter on the PS2, the game's setup= (game.ini):
# the user's data made there at the first start, on the store's install
# screen (MXSHELL's drawing: its `app` target builds it)
MXSHELL ?= $(PS2MAX)/apps/mxshell
CONV_JPEG := $(filter-out %/jmemansi.c %/jmemdos.c %/jmemmac.c %/jmemname.c,$(wildcard $(JPEG)/j*.c))
# Its version, in the stamp it writes: a hash of the sources that decide
# what it makes (not ps2.c and io.c: its screen and its files), so that any
# change to them makes the data again at the game's next start
CONV_MADE := $(addprefix $(HERE)/tools/ps2conv/,conv.h pk3.c snd.c bin.c map.c pak.c) $(CONV_JPEG)
CONV_HASH := $(OUT)/ps2conv/hash.c
$(CONV_HASH): FORCE
	@mkdir -p $(dir $@)
	@line="const char cv_hash[] = \"$$(cat $(CONV_MADE) | sha256sum | cut -c1-16)\";"; \
	  [ "$$(cat $@ 2>/dev/null)" = "$$line" ] || echo "$$line" > $@
FORCE:
CONV_SRCS := $(addprefix $(HERE)/tools/ps2conv/,ps2.c io.c pk3.c snd.c bin.c map.c pak.c) $(CONV_JPEG) $(CONV_HASH)
convert: $(OUT)/CONVERT.ELF
$(OUT)/CONVERT.ELF: $(wildcard $(HERE)/tools/ps2conv/*.[ch]) $(CONV_HASH)
	$(MAKE) -C $(MXSHELL) app APP=q3convert APP_SRCS="$(CONV_SRCS)" \
	        APP_CFLAGS="-DCV_PS2 -I$(HERE)/tools/ps2conv -I$(JPEG) -Wno-unused-parameter"
	@mkdir -p $(dir $@)
	$(LLVM)/llvm-strip -o $@ $(PS2MAX)/build/apps/mxshell/app/q3convert.elf

# The store's packages (ps2max32/store docs/index-format.md, "The package"),
# released by the CI on a tag v*: quake3-gl (renderergl1 on libgl) and
# quake3-ps2 (the PS2's own renderer), each <id>/ with quake3.elf,
# CONVERT.ELF (its setup=) and code/ps2/game.ini; no game data
PKG_DIR := $(OUT)/pkg
package:
	$(MAKE) REN=gl1 OUT=$(OUT)/gl $(OUT)/gl/quake3.elf
	$(MAKE) REN=ps2 OUT=$(OUT)/ps2 $(OUT)/ps2/quake3.elf
	$(MAKE) OUT=$(OUT) $(OUT)/CONVERT.ELF
	for v in gl ps2; do \
	  d=$(PKG_DIR)/quake3-$$v; rm -rf $$d && mkdir -p $$d && \
	  $(LLVM)/llvm-strip -o $$d/quake3.elf $(OUT)/$$v/quake3.elf && \
	  cp $(OUT)/CONVERT.ELF $(HERE)/code/ps2/game.ini $$d/ && \
	  (cd $(PKG_DIR) && rm -f $(OUT)/quake3-$$v-ps2max.zip && python3 -c 'import sys, os, zipfile; z = zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED); [z.write(os.path.join(d, f)) for d, _, fs in sorted(os.walk(sys.argv[2])) for f in sorted(fs)]; z.close()' $(OUT)/quake3-$$v-ps2max.zip quake3-$$v) || exit 1; \
	done

clean:
	rm -rf $(OUT)

.PHONY: all run pak map clean package convert

-include $(shell find $(OUT) -name '*.d' 2>/dev/null)
