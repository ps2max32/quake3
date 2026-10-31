# Quake III Arena on maxsdk (PS2)

ioquake3 built for the PS2 with `libs/maxsdk` (clang, R5900) by `Makefile`; the port is in
`code/ps2/`, and the engine changes are under `#ifdef __PS2__`.

```
make [FRAMES=n] [ARGS="+map q3dm1"]     $(OUT)/quake3.elf (build/apps/maxsdk_quake3)
make run                                 copies the retail baseq3/pak*.pk3 (Q3DATA) to
                                         build/pcsx2-test/baseq3 and runs it in PCSX2
```

`FRAMES=n` ends the program after n frames with a picture (`host:quake3.tga`) and the test end
marker; `ARGS` is added to the command line.

| Part | How |
|---|---|
| Renderer | renderergl1 on `libs/maxsdk_gl` (OpenGL 1.1 subset on the GS, shared with `apps/maxsdk_quake1` and `maxsdk_quake2`); `code/ps2/ps2_glimp.c` points the `qgl*` at it. The TV's mode (NTSC 640 x 448, PAL 640 x 512) or 1080i (below), 16-bit colour (dithered), mipmaps, trilinear; the world's static surfaces are display lists (DMA chains the frame calls) |
| Lighting | lightmaps (`r_vertexLight 0`) as brightness textures the GS multiplies by (`Cd * As`), the surface tinted by the vertex light's hue; overbright (`r_overBrightBits 1`) by the GS's 2x modulate |
| Game code | game, cgame and q3_ui built natively and linked in, each with its symbols made local but `vmMain`/`dllEntry` (`<module>_vmMain`); `Sys_LoadGameDll` returns them |
| Files | stdio on the IOP file server (`host:`), `code/ps2/ps2_file.c`; `fs_basepath host:` |
| Input | pad 0: d-pad arrows, sticks move/look, R1 attack, X jump, O crouch, square/triangle weapons, START menu |
| Memory | 32 MiB: hunk 12 MiB (q3dm1 uses 8.4), zone 3 MiB, sounds 1 MiB (`com_soundMegs 1` is 512 chunks, `client/snd_mem.c`), stack 1 MiB, large static tables shrunk |
| Sound | ioquake3's mixer at 22050 Hz 16-bit stereo into a ring, sent to maxsdk's PCM stream (`mx/snd.h`, audsrv) by `code/ps2/snd_ps2.c` as far as the mixer has painted; `s_mixPreStep 0` (each mix starts where the sending stopped) and `s_mixahead 0.14` (more than the stream's ring) |

Not done yet: cinematics (stubbed), network play; lightmap colour comes from
the vertex light.

## 1080i

Setup, System, Graphics lists the TV's mode and 1920x1080 (`r_availableModes`); Apply
restarts the renderer (`vid_restart`), whose `GLimp_Init` sets 1080i for `r_mode -1`,
`r_customheight 1080` (`code/ps2/ps2_glimp.c`). 1080i takes 2 MiB of memory while it is
on, and the display lists' 2 MiB pool goes for it.

Not enough memory yet: with the sound on, the heap after `Com_Init` has about 1.4 MiB left
in 1080i (3.4 in the TV's mode), and `q3dm1` does not load (the menus run). The display
lists' pool (2 MiB) no longer fits in the TV's mode either since the sound came in, so the
world is drawn without lists there too.
