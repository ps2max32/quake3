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
| Renderer | renderergl1 on `libs/maxsdk_gl` (OpenGL 1.1 subset on the GS, shared with `apps/maxsdk_quake1` and `maxsdk_quake2`); `code/ps2/ps2_glimp.c` points the `qgl*` at it. 640 x 448, 16-bit colour (dithered), mipmaps, trilinear; the world's static surfaces are display lists (DMA chains the frame calls) |
| Lighting | lightmaps (`r_vertexLight 0`) as brightness textures the GS multiplies by (`Cd * As`), the surface tinted by the vertex light's hue; overbright (`r_overBrightBits 1`) by the GS's 2x modulate |
| Game code | game, cgame and q3_ui built natively and linked in, each with its symbols made local but `vmMain`/`dllEntry` (`<module>_vmMain`); `Sys_LoadGameDll` returns them |
| Files | stdio on the IOP file server (`host:`), `code/ps2/ps2_file.c`; `fs_basepath host:` |
| Input | pad 0: d-pad arrows, sticks move/look, R1 attack, X jump, O crouch, square/triangle weapons, START menu |
| Memory | 32 MiB: hunk 12 MiB (q3dm1 uses 8.4), zone 3 MiB, stack 1 MiB, large static tables shrunk |

Not done yet: sound (off), cinematics (stubbed), network play; lightmap colour comes from
the vertex light.

## Branch `hires`: 1080i

This branch builds the 1920 x 1080 interlaced version (`$(OUT)` is
`build/apps/maxsdk_quake3-hires`): maxsdk_gl with `MXGL_HIRES=1`, on the maxsdk worktree of
branch `hires` (`libs/maxsdk-hires`, built into `build/libs/maxsdk-hires`). It runs, but
the display lists' 2 MiB pool does not fit next to the hires records, so the world is drawn
without them (slower).
