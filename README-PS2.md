# Quake III Arena on maxsdk (PS2)

ioquake3 built for the PS2 with `libs/maxsdk` (clang, R5900) by `Makefile`; the port is in
`code/ps2/`, and the engine changes are under `#ifdef __PS2__`.

```
make [FRAMES=n] [ARGS="+map q3dm1"]     $(OUT)/quake3.elf (build/apps/quake3)
make run                                 copies the retail baseq3/pak*.pk3 (Q3DATA) to
                                         build/pcsx2-test/baseq3 and runs it in PCSX2
make pak                                 build/pcsx2-test/baseq3/ps2tex.pk3: the textures
                                         as 8-bit paletted ones (tools/ps2pak.py)
```

`FRAMES=n` ends the program after n frames with a picture (`host:quake3.tga`) and the test end
marker; `ARGS` is added to the command line.

| Part | How |
|---|---|
| Renderer | renderergl1 on `libs/libgl` (OpenGL 1.1 subset on the GS, shared with `apps/quake1` and `quake2`); `code/ps2/ps2_glimp.c` points the `qgl*` at it. The TV's mode (NTSC 640 x 448, PAL 640 x 512) or 1080i, beam racing, 16 or 24-bit colour (below), mipmaps, trilinear; the world's static surfaces are display lists (DMA chains the frame calls) |
| Textures | `make pak` (`tools/ps2pak.py`, Python with numpy and Pillow): each texture under `textures/` and `models/` with its own 256-colour palette (16 when that is exact) and its mipmaps, as a `.p8` in `ps2tex.pk3`; `tr_image.c` takes a mipmapped image's `.p8` before the original and uploads it with `glCompressedTexImage2D` (`GL_OES_compressed_paletted_texture`: T8 and a CLUT in GS memory, a byte a texel). `r_picmip 1` (128 x 128); without the pak (16 bits a texel) use `r_picmip 2` |
| Lighting | lightmaps (`r_vertexLight 0`) as brightness textures the GS multiplies by (`Cd * As`), the surface tinted by the vertex light's hue; overbright (`r_overBrightBits 1`) by the GS's 2x modulate |
| Game code | game, cgame and q3_ui built natively and linked in, each with its symbols made local but `vmMain`/`dllEntry` (`<module>_vmMain`); `Sys_LoadGameDll` returns them |
| Files | stdio on the IOP file server (`host:`), `code/ps2/ps2_file.c`; `fs_basepath host:` |
| Input | pad 0: d-pad arrows, sticks move/look, R1 attack, X jump, O crouch, square/triangle weapons, START menu, R3 (right stick pressed) the counters on/off (`r_ps2stats`) |
| Memory | maxsdk's virtual memory (`mx_vm_config` in `code/ps2/sys_ps2.c`), paged to `host:quake3.swp`: built into VM (`MX_MEMORY=vm`, the default; started by `mxload.elf host:quake3.elf`, as `make run` does) the program's code, data and bss are in VM too, only maxsdk, libc and libgl (`MX_CORE_OBJS`) in core; `malloc` in up to 128 MiB: hunk 56 MiB, zone 16 MiB (`Q3_MEM` in the `Makefile`; ioquake3's 128 and 48 do not fit), sounds `com_soundMegs` as ioquake3; no core heap: libgl's pinned memory (what the GS reads by DMA: arenas, display lists, the textures' smaller levels) is taken from the frames as needed (the counters' `PIN GL` shows it: about 4.8 MB on q3dm7), the textures' two larger levels are pageable (drawn from the smaller ones while the pager brings them in); stack 1 MiB. In PCSX2 only on the EE interpreter (`PCSX2_EE=interp`) |
| Sound | ioquake3's mixer at 22050 Hz 16-bit stereo into a ring, sent to maxsdk's PCM stream (`mx/snd.h`, audsrv) by `code/ps2/snd_ps2.c` as far as the mixer has painted; `s_mixPreStep 0` (each mix starts where the sending stopped) and `s_mixahead 0.14` (more than the stream's ring) |

Not done yet: cinematics (stubbed), network play; lightmap colour comes from
the vertex light.

## Video modes: 1080i, beam racing, 24-bit colour

Setup, System, Graphics sets them; Apply restarts the renderer (`vid_restart`), whose
`GLimp_Init` sets the mode (`code/ps2/ps2_glimp.c`):

- Resolution: the TV's mode or 1920x1080 (`r_availableModes`): 1080i for `r_mode -1`,
  `r_customheight 1080`. Without beam racing 1080i is 640 x 540 stretched to the screen.
- Beam Racing (in the GL Driver's place, `r_ps2beam`; libgl's `MXGL_BEAM`): one buffer of
  a field's lines, drawn in strips just ahead of the beam every field. In the TV's mode it
  leaves 3.6 MiB of GS memory for textures (without: 1.8, PAL 1.5); at most a frame a
  field (60, PAL 50); 3 MiB more of pinned memory while it is on (the records are 512
  KB, `MXGL_REC_BYTES` in the `Makefile`: a frame's stream is up to ~350 KB). `glReadPixels`
  gives zeros then (flares, `r_flares`, are off by default).
- Color Depth 32 bit (`r_colorbits 32`, `MXGL_RGB24`): 24-bit colour, with beam racing in
  the TV's mode (3.3 MiB for textures); otherwise 16-bit, dithered.
- Colored Light (`r_ps2colorlight`, on by default): the lightmaps in 8 bits a channel, their
  stage drawn a channel at a time (libgl's `mxgl_color_light`: three passes, three times
  their GS memory); no stage takes the vertex light's hue then. Off, and with beam racing:
  their brightness, the stage under them tinted with the vertex light's hue (`tr_shade.c`).
- Texture quality (`r_ps2texquality -1`, libgl's `MXGL_TEX_AUTO`; 0, 1, 2: that many levels
  always; `r_ps2texlimits 120 80 30`): full while the textures a frame uses fit 120% of GS
  memory; above that a level less for the farthest and largest until 80%, back at 100%; the
  smaller levels drawn while the swap file is read.
