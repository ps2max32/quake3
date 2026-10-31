# Quake III Arena (ioquake3) on maxsdk: the PS2 port.
#
#   make [OUT=<dir>]     $(OUT)/quake3.elf
#   make run             quake3.elf in PCSX2 with the game data in
#                        build/pcsx2-test/baseq3 (Q3DATA: the retail
#                        baseq3 directory, copied there once)
#
# The engine (qcommon, server, client, botlib, renderergl1) is linked with
# the game modules (game, cgame, q3_ui) built natively: each module is
# linked into one object whose symbols are local except its vmMain and
# dllEntry, renamed <module>_vmMain and <module>_dllEntry, so the three
# copies of q_shared.c and bg_*.c do not clash (code/ps2/sys_ps2.c finds
# them in Sys_LoadGameDll). The renderer draws through libs/maxsdk_gl: an
# OpenGL 1.1 subset on the GS (code/ps2/ps2_glimp.c sets the qgl* to it).

HERE     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PS2MAX   ?= $(abspath $(HERE)/../..)
MAXSDK   ?= $(PS2MAX)/libs/maxsdk
OUT      ?= $(PS2MAX)/build/apps/maxsdk_quake3
all: $(OUT)/quake3.elf
MXGL     ?= $(PS2MAX)/libs/maxsdk_gl
include $(MXGL)/mk/mxgl.mk
include $(MAXSDK)/mk/maxsdk.mk

PICOLIBC ?= $(PS2MAX)/build/libs/picolibc/install
LIBMXC    = $(MAXSDK_OUT)/libmxc.a
LIBRT     = $(MAXSDK_OUT)/librt.a
GLUE      = $(MAXSDK_OUT)/libc/glue.c.o
LIBC_LIBS = $(GLUE) $(LIBMXC) $(PICOLIBC)/lib/libc.a $(MX_LIB) $(LIBRT)

C        := $(HERE)/code
JPEG     := $(C)/thirdparty/jpeg-9f
ZLIB     := $(C)/thirdparty/zlib-1.3.1
SDLINC   := $(C)/thirdparty/SDL2-2.32.8/include

# Memory sizes for 32 MiB (code/qcommon/common.c)
Q3_MEM   ?= -DDEF_COMHUNKMEGS=10 -DDEF_COMZONEMEGS=3
# VU0 macro mode (-mvu0: vector code on VU0) and fast math everywhere;
# unsuffixed literals are float (-cl-single-precision-constant, clang's
# twin of gcc's -fsingle-precision-constant) and the math.h functions go to
# float versions (code/ps2/ps2_math.h): the FPU has no doubles
Q3CFLAGS  = $(filter-out -ffreestanding -Wall -Wextra -Wno-unused-parameter -std=gnu11,$(CFLAGS)) \
            -std=gnu99 -w -MMD -mvu0 -ffast-math -fapprox-func -cl-single-precision-constant -nostdlibinc -isystem $(PICOLIBC)/include -D__PS2__ -DNDEBUG \
            -DPRODUCT_VERSION=\"1.36-ps2\" -DUSE_INTERNAL_JPEG -DNO_GZIP \
            -DUSE_INTERNAL_SDL_HEADERS $(Q3_MEM) \
            -I$(JPEG) -I$(ZLIB) -I$(SDLINC) -I$(C)/ps2 -I$(OUT)/ps2 -I$(MXGL_INC) -include $(C)/ps2/ps2_stdio.h -include $(C)/ps2/ps2_math.h

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
PS2SRC   := $(basename $(notdir $(wildcard $(C)/ps2/*.c)))
JPEGSRC  := $(basename $(notdir $(wildcard $(JPEG)/j*.c)))
ZLIBSRC  := $(basename $(notdir $(wildcard $(ZLIB)/*.c)))

ENGINE_OBJS := $(patsubst %,$(OUT)/qcommon/%.o,$(QCOMMON)) \
               $(patsubst %,$(OUT)/server/%.o,$(SERVER)) \
               $(patsubst %,$(OUT)/botlib/%.o,$(BOTLIB)) \
               $(patsubst %,$(OUT)/client/%.o,$(CLIENT)) \
               $(patsubst %,$(OUT)/renderercommon/%.o,$(RCOMMON)) \
               $(patsubst %,$(OUT)/renderergl1/%.o,$(RGL1)) \
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

# $(call module,name,sources)
define module
$(OUT)/$(1).o: $(patsubst %,$(OUT)/mod/$(1)/%.o,$(2))
	$(LLVM)/ld.lld -m elf32ltsmipn32 -r -o $$@.r $$^
	$(LLVM)/llvm-objcopy --keep-global-symbol=vmMain --keep-global-symbol=dllEntry $$@.r $$@.l
	$(LLVM)/llvm-objcopy --redefine-sym=vmMain=$(1)_vmMain --redefine-sym=dllEntry=$(1)_dllEntry \
	  $$@.l $$@
	rm -f $$@.r $$@.l
endef
$(eval $(call module,qagame,$(GAME_SRC)))
$(eval $(call module,cgame,$(CGAME_SRC)))
$(eval $(call module,ui,$(UI_SRC)))

MODULES := $(OUT)/qagame.o $(OUT)/cgame.o $(OUT)/ui.o

# FRAMES: end after that many frames with a picture (host:quake3.tga);
# ARGS: more of the command line (e.g. ARGS="+map q3dm1"); GLSHOT: the
# same picture at that rendered frame (loading screens included). They go into
# $(OUT)/ps2/runcfg.h, rewritten only when they change.
RUNCFG := $(OUT)/ps2/runcfg.h
$(shell mkdir -p $(OUT)/ps2; printf '#define PS2_FRAMES %s\n#define PS2_ARGS "%s"\n#define PS2_GLSHOT %s\n' \
  '$(or $(FRAMES),0)' '$(ARGS)' '$(or $(GLSHOT),0)' > $(RUNCFG).new; \
  cmp -s $(RUNCFG).new $(RUNCFG) || mv $(RUNCFG).new $(RUNCFG); rm -f $(RUNCFG).new)

$(OUT)/quake3.elf: $(ENGINE_OBJS) $(MODULES) $(MXGL_OBJS) $(HLSL_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,--defsym=__stack_size=0x100000 $(MX_CRT0) $(ENGINE_OBJS) $(MODULES) $(MXGL_OBJS) $(HLSL_OBJS) $(LIBC_LIBS))

# ---- running --------------------------------------------------------------------
Q3DATA   ?= /mnt/truenas_data_exthdd/spellen/PC/Quake III Arena/baseq3
RUNDIR   := $(PS2MAX)/build/pcsx2-test
run: $(OUT)/quake3.elf
	@mkdir -p $(RUNDIR)/baseq3
	@for f in "$(Q3DATA)"/pak*.pk3; do \
	  [ -f "$(RUNDIR)/baseq3/$$(basename "$$f")" ] || cp -v "$$f" $(RUNDIR)/baseq3/; done
	~/.claude/skills/ps2-pcsx2-test/run.sh $(OUT)/quake3.elf $(or $(TIMEOUT),600)

clean:
	rm -rf $(OUT)

.PHONY: all run clean

-include $(shell find $(OUT) -name '*.d' 2>/dev/null)
