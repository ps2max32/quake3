# Quake III Arena on maxsdk (PS2)

ioquake3 built for the PS2 with `libs/maxsdk` (clang, R5900) by `Makefile`; the port is in
`code/ps2/`, and the engine changes are under `#ifdef __PS2__`.

```
make [FRAMES=n] [ARGS="+map q3dm1"]     $(OUT)/quake3.elf (build/apps/quake3)
make run                                 copies the retail baseq3/pak*.pk3 (Q3DATA) to
                                         build/pcsx2-test/baseq3 and runs it in PCSX2
```

`FRAMES=n` ends the program after n frames with a picture (`host:quake3.tga`) and the test end
marker; `ARGS` is added to the command line.

| Part | How |
|---|---|
| Renderer | renderergl1 on `libs/libgl` (OpenGL 1.1 subset on the GS, shared with `apps/quake1` and `quake2`); `code/ps2/ps2_glimp.c` points the `qgl*` at it. The TV's mode (NTSC 640 x 448, PAL 640 x 512) or 1080i (below), 16-bit colour (dithered), mipmaps, trilinear; the world's static surfaces are display lists (DMA chains the frame calls) |
| Lighting | lightmaps (`r_vertexLight 0`) as brightness textures the GS multiplies by (`Cd * As`), the surface tinted by the vertex light's hue; overbright (`r_overBrightBits 1`) by the GS's 2x modulate |
| Game code | game, cgame and q3_ui built natively and linked in, each with its symbols made local but `vmMain`/`dllEntry` (`<module>_vmMain`); `Sys_LoadGameDll` returns them |
| Files | stdio on the IOP file server (`host:`), `code/ps2/ps2_file.c`; `fs_basepath host:` |
| Input | pad 0: d-pad arrows, sticks move/look, R1 attack, X jump, O crouch, square/triangle weapons, START menu, R3 (right stick pressed) the counters on/off (`r_ps2stats`) |
| Memory | maxsdk's virtual memory (`mx_vm_config` in `code/ps2/sys_ps2.c`), paged to `host:quake3.swp`: with `make MX_MEMORY=vm` (started by `mxload.elf host:quake3.elf`) the program's code, data and bss are in VM too, only maxsdk, libc and libgl (`MX_CORE_OBJS`) in core; `malloc` in up to 128 MiB: hunk 56 MiB, zone 16 MiB (`Q3_MEM` in the `Makefile`; ioquake3's 128 and 48 do not fit), sounds `com_soundMegs` as ioquake3; a core heap of 12 MiB for libgl (what the GS reads by DMA); stack 1 MiB. In PCSX2 only on the EE interpreter (`PCSX2_EE=interp`) |
| Sound | ioquake3's mixer at 22050 Hz 16-bit stereo into a ring, sent to maxsdk's PCM stream (`mx/snd.h`, audsrv) by `code/ps2/snd_ps2.c` as far as the mixer has painted; `s_mixPreStep 0` (each mix starts where the sending stopped) and `s_mixahead 0.14` (more than the stream's ring) |

Not done yet: cinematics (stubbed), network play; lightmap colour comes from
the vertex light.

## 1080i

Setup, System, Graphics lists the TV's mode and 1920x1080 (`r_availableModes`); Apply
restarts the renderer (`vid_restart`), whose `GLimp_Init` sets 1080i for `r_mode -1`,
`r_customheight 1080` (`code/ps2/ps2_glimp.c`).
