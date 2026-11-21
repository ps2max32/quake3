# Quake III at 1080i, 60 fps: a renderer made for the PS2

Goal: `demo four` and real play at one game frame a field in 1080i beam racing
(59.94 fps, no late passes), at high quality (lightmaps, dynamic lights, marks,
full curves, all shader stages), as an MXOS program on a retail PS2 (32 MB).
60 is the most beam racing shows; "as many fps as possible" means the margin
under a field's time, which is what keeps 60 locked in heavy scenes.

Status: phase 0 done (its results are in section 2), phase 1 in work. Every "now" number below was measured on a
PS2 with `demo four`; every "target" is an estimate until its phase's gate
has been measured.

## 1. Bottom line

- The hardware is not the limit. A frame is about 13,000 vertices and 10,000
  triangles with the passes counted. VU1 does that in about 1 ms with a
  pipelined program; today a pass takes up to 9 ms without drawing one pixel.
- The EE is the limit, and not by arithmetic: 7 million instructions and
  175,000 cache misses a frame at 0.5 instructions a cycle. A draw costs about
  30,000 cycles whatever its size (cold code and data), and there are 240.
- So no optimisation of the present path gets there, and a VU1 path beside it
  does not either (tried: section 3). The renderer is replaced whole, front
  end included, by one that does no work per surface, per vertex or per draw
  on the EE. The game (game, cgame, ui, server, collision) stays ioquake3's.
- Expected, as estimates: the EE's frame from 11.1 M cycles to 3-4 M (a field
  is 4.92 M); a pass's geometry from 9 ms to 1-1.5 ms; the world's fill as it
  is (it fits on average). What stays tight is fill in the worst effect
  frames, which gets a governor (5.5), and real play with bots (phase 6).

## 2. What was measured

`demo four`, 1260 frames, PS2. HQ = lightmaps, dlights, marks, LOD 0.

| | SD defaults | SD fast settings | 1080i beam, HQ |
|---|---|---|---|
| fps | 22.0 | 50.6 | 27.5 |
| EE cycles a frame | 12.6 M | 6.0 M | 11.1 M |
| instructions a frame | 6.3 M | 3.0 M | 7.1 M |
| I-cache / D-cache misses a frame | 101 k / 129 k | 40 k / 66 k | 67 k / 107 k |
| TLB refills a frame (about 180 cycles each) | 3,100 | 1,700 | 3,100 |
| vertices / triangles a frame, passes counted (peak) | 9,700 / 7,100 (12,900 / 9,900) | 3,700 / 2,600 | as SD defaults |
| libgl draws a frame (peak) | 238 (341) | 98 | |
| VIF1 / VU1 / GS busy | 15 % / 11 % / 7 % | 14 % / 10 % / 10 % | 70 % / 34-48 % / 36-45 % |

Where the 1080i HQ frame goes (profile): libgl 28 %, renderergl1 25 %, kernel
(idle on swap, TLB refills) 15-22 %, everything else (game, cgame, client,
sound, libc, collision) about 25 %; cgame alone is 2.3 %.

| 1080i beam, HQ | fps | late passes | longest pass |
|---|---|---|---|
| all | 27.5 | 27 % | 12-17 ms |
| all, the GS draws one pixel a strip | 27.2 | 8 % | 8.9 ms |
| no entities | 34.4 | 2.7 % | 7.5 ms |
| no world | 47.3 | 6.4 % | 10.6 ms |
| no world, no entities | 56.7 | | |

Other facts the plan rests on:

- A libgl draw is about 19,000 cycles (state 4,600, triangles 7,000, transform
  2,700, send 2,500, LOD and matrix 2,200) and the renderer adds 10-15,000.
  The HUD's 14 batches cost 32,500 cycles each for about 70 vertices.
- Blended pixels outside the world's lists: 0.7-8 M a frame, 24 M in the worst
  frames (dlit surfaces drawn again, smoke, blood, explosions). A 1080i field
  is 1.04 M pixels.
- Memory: hunk 19.0 MB used of 56, zone 0.4 of 16; pinned 8-9 MB; the pool is
  990 frames of 32 KB. In play it swaps 0.2-0.7 MB/s each way; a page costs
  5.5 ms in over the network, about 60 ms from USB.
- maxsdk's own measurements (`libs/maxsdk/docs/vu1.md`, `vif.md`,
  `gif-gs.md`): hand-scheduled VU1 kernels with the VIF and the GS: textured
  12.7 cycles a vertex, lit and textured 15.4, three lights with ADC clipping
  18.6; UNPACK V3-16 + V2-32 + V4-8 7.6 cycles a vertex; GS set-up 8 cycles a
  triangle; fill 0.42 cycles a pixel textured bilinear, 0.31 point sampled,
  0.16 flat; scratchpad stores 1.26 cycles a quadword against 6.2 uncached.

### Phase 0 results (2026-11-16)

`demo four`, frames 2-1259, the settings of `misc/ps2/q3hq.cfg` (the whole HUD
with its 3D icons on), both builds with the same measuring code
(`code/ps2/sys_ps2.c`), one PS2 (SCPH-90006), swap on `host:`.

| A frame | `REN=gl1`, 1080i beam racing | `REN=ps2` (draws nothing) |
|---|---|---|
| EE cycles, mean | 12.77 M (43.3 ms, 23.0 fps) | 1.58 M (5.36 ms, 186.6 fps) |
| median / 95 % / 99 % / longest | 41.9 / 72.2 / 101.9 / 289.8 ms | 4.29 / 9.69 / 29.7 / 57.4 ms |
| frames over a 59.94 Hz field | 1237 of 1258 | 37 of 1258 |
| instructions | 7.30 M | 1.15 M |
| I-cache / D-cache misses | 87.7 k / 120.2 k | 4.8 k / 19.5 k |
| TLB refills (of code) | 3,462 (965) | 1,165 (337) |
| swap ins / outs over the demo | 475 / 383 (4.9 s) | 0 / 0 |
| waiting for the IOP over the demo | 7.2 s | 1.33 s (261 waits) |
| pinned frames of 960 | 392 | 103 |
| beam racing | 4567 of 9557 passes late; longest 14.0 / 16.4 / 10.8 ms; record 290 KB | |
| load to the first frame | about 27 s | 13.6 s |

- Drawing is 88 % of today's frame (11.2 M of 12.8 M cycles). The game side
  is 1.58 M cycles, under the 2.5 M of phase 0's gate: phase 1 goes ahead as
  planned, and 3.3 M cycles of a field are left for the renderer and margin.
- The game side's profile: the kernel 25 % (mostly the 1.33 s of IOP waits,
  and the TLB refills: 1,165 a frame are 0.2 M cycles), collision 19 %
  (`CM_Trace` for shadows, marks, missiles, local entities), cgame 19 %,
  ioquake3's sound bookkeeping 8 % (`S_AddLoopSounds`,
  `S_Base_ClearLoopingSounds`: loops over every entity each frame), the
  syscall layer to cgame 8 %, the demo's messages 3 %.
- Its long frames are waits for the IOP: a sound is copied into SPU RAM
  when it first plays (10-57 ms). Without those waits the mean is 1.27 M
  cycles. For phase 5b: the sounds a map registers go into SPU RAM at load.
- cgame hands the renderer, a frame: 68 models, 26 sprites, 1 dynamic
  light, 178 2D pictures, 3.6 small scenes (the HUD's 3D icons), 4.5 mark
  requests.
- The game side alone does not swap and pins 103 frames (3.2 MB).

### Phase 1, first results (2026-11-16)

`tools/q3view` flies the timedemo's 1259 cameras through q3dm6 with
`code/renderer_ps2/rp_world.c`, a frame a field, in 1080i beam racing (three
strips, the whole record in every pass), on a PS2 (SCPH-90006). The world
only: no sky, models or effects. Triangles across the near plane or the
guard band are left out for now, not cut.

| A frame (a field) | Measured |
|---|---|
| The EE building it | 0.38 M cycles (1.29 ms), the longest 2.5 ms; 0.17 M before the chunks by direction |
| Stream | 856 quadwords (the largest record 28 KB; libgl's 290 KB) |
| Groups seen from the cluster / in view | 247 / 98 of 565 |
| Chunks drawn / facing away | 290 / 125 |
| Strip vertices to VU1 | 5,564 (17,344 at most) in 295 batches |
| A pass without drawing (XYZ3) | 0.9 ms: 2.7 ms a field for the three; the longest field 5.6 ms |
| The three passes with drawing | 8.6 ms a field, the longest 15.1 ms; 237 of 3778 passes late |

- The frame rate is the field rate: the EE has 15 ms of a field left.
- VIF1, VU1 and the GIF take about 50 EE cycles a vertex for both passes
  with these small batches (19 vertices on average); the loops alone 11 to
  15. Without the chunks by direction it was 35 at 59 vertices a batch.
- The GS's rasterising is what fills a pass. It is not hidden pixels: the
  overdraw is 0.9 to 1.3 triangles a pixel in a picture made with
  `rp_debug` 128 (cameras 100, 600, 1000) and 1.1 to 1.9 when the same chunks
  are rasterised on the PC; leaving out every group without a visible pixel
  would take 5 to 10 % of that, so occluders are not worth their cost (tried
  with the map's solid boxes: 5 groups of 90 hidden a frame).
- What a pixel costs (maxsdk's `probe/p41_scissor.c`, triangles the size of
  the screen, Z test and write): 0.32 EE cycles flat, 0.34 textured
  bilinear, and nothing when the scissor leaves it out. So a pass over the
  whole 1920 x 540 field is about 0.33 M cycles, 1.1 ms; texture and lightmap
  hardly matter (the same run without textures: 0.8 ms a field less;
  without the lightmap pass: 0.7 ms less); and the world's three (the
  passes' clear, the texture, the lightmap, each over 1 to 1.5 screens) are
  4 to 5 ms a field.
- Leaving out a triangle that faces away is needed for the picture too:
  Quake's surfaces have one side, and the GS draws both.
- Two CLUTs by turns do not work: the GS has one CLUT buffer for both
  contexts. A texture's CLUT now has the lightmaps' brightness ramp in its
  alpha, and the lightmap (T8) is drawn with its texture's CLUT.

What follows from it:

1. Phase 2 (maxsdk's passes) is needed as planned, both parts: a strip's own
   record (the three replays are 2.7 ms a field), and more strips whose
   passes may start early (the mean is 8.6 ms of a 16.7 ms field, but a strip
   has 5.5 ms and the strips are not alike).
2. A screen of pixels costs 1.1 ms a pass whatever is done, so what is drawn
   over the world (sky, models, effects, the HUD) has about six screens of
   fill a field. Triangles that face away in curves and models are still to
   be culled per triangle on VU1 (by the triangle's plane, kept with the
   vertex that ends it): for the picture, and for the pixels.
3. The front end's 0.38 M cycles is plain C over every chunk's cone: a flat
   chunk's test is one dot product, to be made so (and on VU0).

### Phase 2, first results (2026-11-16)

maxsdk's passes are queued now (a pass may start as soon as the beam has
left its Z strip and the pass before is done; it is left out only when it
has not started when the beam comes to its strip), and a strip has a record
of its own (`mx_hires_strip`): the renderer puts a chunk into the records of
the strips its bounds touch on the screen. The same flight (q3dm6, the
world only, 1259 fields, a PS2):

| Passes | Left out | Still drawn when the beam came | A field's passes | The EE a frame |
|---|---|---|---|---|
| Before: three strips, kicked at fixed lines, the whole record each | 237 | not counted | about 8.6 ms | 0.38 M |
| Three strips queued, the whole record each | 17 | 426 | 9.4 ms | 0.49 M |
| Three strips queued, a record a strip | 0 | 241 | 8.5 ms (the longest 17.5) | 0.76 M |
| Nine strips of 64 lines queued, a record a strip | 65 | 457 | 10.6 ms | 1.33 M |

- Three strips with their own records are the choice: no pass is left out
  (a strip left out shows the Z of the pass before: the worst glitch). A
  chunk is in 1.8 strips on average (5,564 vertices drawn, 10,128 sent a
  field); nine strips put it in 4.5 and cost more than they give.
- The 241 passes that still ended after the beam had come were stray
  triangles, on the PS2 only (its picture, read back, against PCSX2's): the
  programs wrote a vertex's ADC in the same word as FTOI4 wrote its X, Y and
  Z into the same register (VU1 drops the lower instruction's result then),
  and a buffer was unpacked into while the GIF still read it (the GS is slow
  over a large triangle; a VIF FLUSH before each batch's start waits). With
  both put right the PS2's picture is PCSX2's (mean difference 0.5 of 255:
  the dither), and the same flight is:

| Passes | Left out | Still drawn when the beam came | A field's passes | The EE a frame |
|---|---|---|---|---|
| Three strips queued, the whole record each | 0 | 28 | 7.9 ms (the longest 15.3) | 0.44 M |
| Three strips queued, a record a strip | 0 | 8 | 6.9 ms (the longest 13.8; 29 fields over 12 ms) | 0.66 M |
| The same, the GS drawing nothing | 0 | 0 | 2.2 ms (the longest 3.9) | |
- The front end went from 0.38 M to 0.76 M cycles: the strips of each chunk
  (eight corners projected) and a stream a strip. To be made cheaper with
  the cone tests (VU0, a flat chunk's plane).
- A VIF1/VU1/GIF pass costs about 28 EE cycles a vertex and 360 a batch
  (from two runs with 59 and 19 vertices a batch).

### Phase 3, first results (2026-11-17)

The renderer is in the game: the world, the game's shaders (scripts or an
image alone), sprites and polys, the screen's pictures and MD3 models with
the light grid's light. A timedemo of `demo four`, 1080i beam racing,
`misc/ps2/q3hq.cfg` (which now also sets the view weapon on: the measured
console's saved settings had it off, in the libgl build's runs too), on a
PS2 with host swap:

| Build | Timedemo | The EE a frame (waits for a record left out) | Frames over a field | A field's passes | Passes still drawn when the beam came |
|---|---|---|---|---|---|
| libgl (phase 0; no view weapon) | 23.0 fps | 12.77 M cycles | | | 48 % late |
| World, sprites, polys, pictures; no models | 59.9 fps (every field a frame) | 3.10 M (10.5 ms), median 9.7 ms | 64 of 1258 | 5.7 ms | 47 of 6694 |
| With the models (no view weapon) | 54.9 fps | 4.51 M, median 12.8 ms | 338 | 6.3 ms | 144 of 7537 |
| With the models and the view weapon | 52.5 fps | 5.07 M (17.2 ms), median 15.0 ms | 475 | 7.4 ms | 508 of 7691 |

What is in those numbers and not drawing:

- **Swap.** With the models the run swaps: 156 pages in (0.8 s) and 272 out
  (1.8 s) of the run's 24 s. The models' files are whole in the hunk and their
  frames are made ready beside them; the images are all in memory. Phase 5
  (files mapped where they are read from, nothing written that is not used)
  removes it; without those waits the run is at about 58 fps.
- **The view weapon is built on the EE** (its sphere crosses the near
  plane, so it goes the polygons' way): 0.56 M cycles a frame for 170
  triangles drawn. It gets a program of its own: `q3m` with a test of each
  vertex, a triangle with a vertex outside left out (they are below the
  screen's edge).
- **A profile** of the run without models (36 % of the time waiting for a
  record): the renderer 3.4 ms a frame (the world's cull 1.1 ms, the list's
  end 0.9 ms, pictures 0.6 ms), the kernel 2.0 ms (1,800 TLB refills a
  frame), cgame 1.1 ms, collision and the rest of the engine 2.7 ms.

The world alone, the viewer's flight (1259 fields), after the cut triangles
are drawn and the textures are a cache:

| | A field's passes | The longest | Still drawn when the beam came |
|---|---|---|---|
| All | 8.2 ms | 15.5 ms (108 fields over 12 ms) | 36 of 3777 |
| The GS drawing nothing | 2.8 ms | 5.2 ms | 0 |

- The cut triangles are large and near: drawing them is 0.7 ms a field
  that the holes saved before; the programs that cut and their scan are
  0.6 ms.
- **Where a pass's time goes**, 80 heavy cameras (630 to 710), each variant
  a flight: nothing drawn 3.9 ms; one flat pass 6.3 ms; with the texture
  7.6 ms; the lightmap pass alone over flat 10.6 ms; both 12.0 ms. So a
  layer of the world costs the GS about 2.7 ms flat and 4 ms textured, the
  lightmap pass as much as the texture pass, and the overdraw there is 1.0
  (the picture of it): about 0.6 EE cycles a pixel flat and 0.9 textured,
  two to three times the probe's screen-size triangle. MIPMAP levels change
  nothing here.
- **It is not the pixels.** The same triangles over a quarter of the
  pixels (the view drawn half as wide and high): 10.4 ms in place of 11.9.
  And it is not VU1 waiting for the GS: without the FLUSH before each batch
  (a program's last packet now lies where the VIF writes nothing, so the
  next batch's unpack cannot touch a packet still being sent; batches of 46
  vertices, map file version 4) the times are the same to a tenth of a
  millisecond. Nor triangles outside a pass's strip: the programs leave
  those out now (a test of each vertex against the strip's lines), which
  costs VU1 0.14 ms and saves the GS 0.25.
- **A fit over the flight's 1258 fields** (each field's vertices sent
  against its passes' time):

  | | A field, whatever is in it | A vertex sent |
  |---|---|---|
  | Nothing drawn (XYZ3) | 1.8 ms: the clear of the three strips (1.1 ms) and the programs' upload | 40 cycles |
  | One flat pass | 2.9 ms | 55 cycles |
  | Texture and lightmap | 5.1 ms | 105 cycles |

  So a field of the world is about 1.8 ms that are always there, 3.3 ms of
  pixels (two textured layers) and 105 EE cycles for every vertex sent: 40
  to get it through VIF1, VU1 and the GIF, 65 for the GS to set up its
  triangle twice. The world sends 8,000 vertices a field on average and
  13,000 in the heavy ones, 1.84 times what it draws (a chunk in every
  strip it touches) and 1.96 for a triangle (a BSP face's vertices are its
  own: its lightmap coordinates). What is left to cut there: curves in
  fewer triangles when they are far, and the clear.

**The same day, later.** With what follows the same timedemo is 58.9 fps
(21.4 s for 1260 frames; the EE 3.78 M cycles a frame, median 11.3 ms; 180
of 1258 frames over a field; the longest 76 ms; 13 pages swapped in and 69
out in the run):

- **The swap was the engine's temp memory.** A load reads megabytes of files
  (the BSP, sounds, scripts) into the hunk's temp side and frees them; the
  pages stayed as they were written, and the pager sent them to the swap
  file a page at a time (7 ms each, in the frame that wanted memory) during
  the first seconds of play: 8.3 MB "unused highwater". Temp memory now
  comes from a heap of pages whose freed pages are dropped
  (`Hunk_AllocateTempMemory`, PS2).
- **Images and models are mapped files** (`tools/ps2bin.py`: `ps2tex.bin`,
  `ps2mdl.bin` beside the paks): no copy in the hunk, nothing decoded at a
  map's start (cgame's start 6.5 s -> 4.9 s), pages that cost no write to
  give up. What a map's start needs is read then. A model frame's vertices
  are made ready for VU1 when first drawn, in a cache of 4 MB; when the
  file's page is not in memory the pager is asked and the draw uses the
  block it had: a frame never waits for the file.
- **The view weapon is on VU1** (`q3mc`: a triangle with a vertex outside
  is left out; they are below the screen's edge).
- **The EE**: a plane against a box without branches and the strips by
  planes (the world's front end 0.72 M -> 0.53 M cycles), the world's runs
  made at load, `Sys_Milliseconds` without a 64-bit division.
- maxsdk's VM writer thread (`clean_frames`) was tried against the swap
  and did not help: faults wait for the VM lock while it writes.
- **The world's later stages** are passes of one more program (`q3w1t`,
  `q3w1tc`: a texture matrix and a colour, the vertex's times one plus
  another, as uniforms): a script's stages after the map's own pass, every
  stage of a material without a lightmap.
- **GS memory is the textures' limit.** Beside the 1920-wide picture 1.9 MB
  is left; with the stages' images a frame drew with 1.17 MB of them, 1.78 MB
  at most, and entries of frames in flight cannot make room: 152 images found
  none in the run. Now: an image is of 16 colours when that stays within
  37 dB of the full-colour one (705 of 1919; `tools/ps2pak.py --t4`), half
  the memory and half the bytes to send; the map draws with the file of
  images' textures (one copy in GS memory of an image that is also a
  stage's or a model's; the `.pmap` has no texels, 3.2 -> 2.0 MB), a 16-colour
  one too: its CLUT has 256 entries, the colours in the first 16, the
  lightmaps' brightness in the alpha of the upper half, where the lightmap
  pages' texels point; the screen's pictures have one level. A frame draws
  with 0.93 MB (1.46 MB at most), none without room, 7.9 MB sent in the run
  (16.7 MB before).
- **TLB refills**: 1,670 a frame (1 ms) were 32 entries shared by some 200
  pages of 32 KiB in use, the hottest refilled 35 to 45 times a frame
  (`Q3_VM_HIST`: the refills by page): code 510 a frame, the program's
  statics 646, the hunk 305, the mapped files 59. The code a frame of play
  runs (the renderer, cgame, collision, the client's and the engine's
  every-frame parts: 650 KB of 1.9 MB) now stays core (`MX_FAST_TEXT_OBJS`
  in the Makefile): 670 refills a frame, the EE 3.51 M -> 3.24 M cycles a
  frame, 120 -> 85 of 1258 frames over a field, and no frame of memory
  less (the core is counted in MiB and had the room). All code in core
  (`MX_VM_TEXT=core`) gives 616 a frame for 2 MiB of frames, and more swap.

A census at a run's end (`Q3_VM_CENSUS`): 16.9 MB of the program's own
memory resident (the client's `cl` 1.9 MB, cgame's entities 0.9 MB, the
hunk and zone 8 MB), 7.5 MB of mapped files, 7.3 MB pinned (the map's
chunks 1.2, the models' frames 1.9 and tables 1.0, the frame's chains 0.8,
the records 0.6).

Left of phase 3: the models' stages after a shader's first (11,000 skipped
in the run), the map's doors and platforms, the sky, beams and lightning, marks,
dynamic lights, texture coordinates that move on world surfaces, strips in
a model's pieces.

### The late passes (2026-11-17)

About 600 of 6,600 passes a run were still being drawn when the beam came
(a strip that shows half drawn). maxsdk's pass log (`mx_hires_log`;
`+set r_ps2passes 1` writes `quake3.pass`: each pass's frame, wait, time,
and what its strip held) shows that a pass is late when the field's passes
together are long (median 15.9 ms against 11.8 ms for all fields), not
because it waited behind another. The timedemo with parts left out
(`r_ps2draw`, `r_ps2debug`), a field's passes together:

| Variant | Mean | 90 % | Late |
|---|---|---|---|
| Everything | 11.9 ms | 15.8 ms | 621 |
| The GS draws nothing | 5.7 ms | 7.3 ms | 0 |
| The same, without models | 3.3 ms | 4.6 ms | 0 |
| A quarter of the pixels, the same triangles | 11.9 ms | 15.7 ms | 593 |
| No textures | 11.3 ms | 14.9 ms | 376 |
| No lightmap pass | 9.1 ms | 11.9 ms | 51 |
| No models | 9.3 ms | 13.1 ms | 116 |
| No view weapon | 10.4 ms | 14.3 ms | 258 |
| The world alone | 8.5 ms | 12.5 ms | 45 |
| Five strips of 128 lines | 13.3 ms | 16.7 ms | 616 |

- The GS is the limit, by triangles, not pixels: a quarter of the pixels
  changes nothing, the GS drawing nothing leaves no pass late.
- The models send 14,000 vertices a frame over the strips (three a
  triangle, both faces), the world 7,200: VIF1 and VU1 spend 2.3 ms a field
  on them, the GS 0.3 ms more. Next: back faces left out on VU1 and the
  pieces as strips.
- The lightmap pass is 2.9 ms a field: every world triangle set up twice.
- More, lower strips cost more than they give (a chunk is in more strips).

Back faces of the models are now left out in `q3m`'s gather (a quadword a
triangle, the sign of the screen's cross product against the shader's
cull): the picture is the same (4 pixels of 2 million differ), the GS gets
half the models' triangles, and the passes are as long as before (11.95 ms
a field, 620 late). So the models' cost is not the GS's triangles. A fit of
each pass's time to what its strip held (`quake3.pass`, now with the
models' vertices and batches and the draws) gives about 30 us a program
set-up when the GS draws (7 us when it draws nothing) and almost nothing a
model vertex: a pass of 59 set-ups spends about 1.8 ms on them. The
set-ups (each waits for the program before: FLUSHE, a run's GS registers,
TEX0 with its CLUT) are the next thing to cut: fewer runs a strip (the
world's runs by material are split into a cut and an uncut list, each with
its own set-up), and set-ups that do not wait.

Tried next, the same timedemo: the runs' GS registers left out (rp_debug
65536: TEX0, CLUT, tests and blends of every set-up) 12.4 ms a field; no
CLUT loads (1024) 11.9 ms; as it is 11.9 ms. So the set-ups' GS registers
and CLUT loads cost nothing measurable; the fit's set-ups stand for the
amount of world in a strip. What is left as the GS's cost (6 ms a field of
the 11.9; 5.8 ms when it draws nothing) is its triangles of the world,
both passes, at small pixel counts: about 16,000 set-ups a frame.

**Do VU1 and the GS overlap?** rp_debug 131072 sends every list twice,
the second time as XYZ3 (VIF1 and VU1 work twice, the GS draws once; the
picture is the same to the pixel). The same timedemo, a field's passes:

| | Once | Twice | Added |
|---|---|---|---|
| The GS drawing nothing (VIF1, VU1, GIF alone) | 5.8 ms | 9.3 ms | 3.5 ms |
| The GS drawing | 11.9 ms | 14.5 ms | 2.6 ms |

Three quarters of VU1's added time shows in the passes: VU1 and the GS
mostly take turns. With them overlapped a field would be near the GS's
own share (about 12 - 0.75 x 5.8 = 7.6 ms), where the run without the
lightmap pass (9.1 ms) already had only 51 late passes. So the next step
is the overlap: set-ups that do not wait for the program before (FLUSHE:
the context is shared), no FLUSH between the models' programs and the
others, larger batches.

The view weapon now goes through `q3mc` with the cut: a triangle with a
vertex outside the guard band is left out of the piece's packet and cut
after it (`q3mclip`, the world's routines), where it was dropped before
(holes in the weapon near the eye). Models across a plane no longer go the
EE's way.

### Strips still being drawn at a place (2026-11-19)

A run that goes to a place and stands there (`+devmap`, `+setviewpos`,
`r_ps2beamat`: the counts from that view on), q3dm1 in a corridor beside a
wall, 512 fields on the PS2:

| | A field's passes | Still drawn when the beam came |
|---|---|---|
| Everything | 14.5 ms | 1025 of 1537 (two strips of three, every field) |
| No sky | 11.6 ms | 0 |
| No lightmap pass | 10.7 ms | 0 |
| No models | 12.8 ms | 513 |
| The GS draws nothing | 5.1 ms | 0 |
| Nothing drawn | 0.13 ms | 0 |

- The picture from GS memory is PCSX2's: nothing is drawn wrong there. What
  shows on the TV is strips half drawn.
- **The sky** was drawn over the whole of every strip in which a sky
  surface is in the PVS and the frustum, seen or not: 2.9 ms here, with no
  sky in the picture. It is now drawn in the part of a strip that its sky
  surfaces' boxes cover, cut to the frustum (0 late here, 12.0 ms), and
  after the world's surfaces that write depth. The depth test alone gives
  almost nothing (14.3 ms): a pixel that fails it costs the GS about what
  one drawn costs. The sky in 32 x 32 quads a side for 8 x 8: 14.2 ms (the
  GS's page breaks are a small part).
- **A pass had the time of one strip** (the Z of a pass was the strip after
  it, in the picture): 5.3 to 6.4 ms, whatever the other strips left. Here
  the passes took 5.6, 6.0 and 3.4 ms: two late every field with 2.3 ms
  unused. With a Z strip of the passes' own (maxsdk's `mx_hires_own_z`,
  `r_ps2ownz`, now the default) a pass has the field less its strip's time.

The timedemo with that, the same day:

| | Still drawn when the beam came | The EE a frame | Images that found no room |
|---|---|---|---|
| The Z in the strip after | 680 of 6532 | 10.9 ms | 0 |
| A Z strip of its own, three strips | 161 of 6538 | 11.1 ms | 91 |
| The same, five strips of 128 lines | 752 of 10850 | 11.8 ms | 45 |

The Z strip takes 720 KB of GS memory: 1.2 MB is left for the images, and
the demo's frames want up to 1.5 MB. When a frame's images do not fit, the
models' and the effects' are placed without their finest level (but the
view weapon's) until the frames have fitted for two seconds, and any image
that finds no room is placed without its finest one or two. Five strips of
128 lines leave 1.4 MB but every pass plays more of the world.

**Later that night**, each measured with the same timedemo and the two
standing views of q3dm1 (a corridor beside a wall, a view into the court):

| | The corridor | The court | The timedemo's strips still drawn when the beam came |
|---|---|---|---|
| Before | 14.5 ms | 15.6 ms | 680 of 6532 |
| The sky where it is seen; the Z strip | 11.3 ms | 14.0 ms | 67 to 129 (run to run) |
| A triangle to be cut tested against the pass's window first | 10.6 ms | 12.7 ms | 60 to 103 |

- **What the programs that cut cost.** The world alone with the GS drawing
  nothing: 3.46 ms a field in the corridor, 2.36 ms with nothing cut, 3.43
  with every chunk through the programs that cut (so nearly all of that
  view's chunks are across a plane); the court 4.54 and 2.81. A chunk across
  the guard band is in every strip's record, and each pass cut all of its
  triangles with a vertex outside: `q3cut_test` now leaves out those whose
  corners are all beyond one side of the picture or of the pass's lines
  (2.96 and 3.46 ms).
- **The chunks' own visibility** (map file version 7): 249 draws a frame for
  281 in the timedemo, 4 to 22 % fewer vertices a cluster before the frustum.
- **The models** are 1.4 ms of the corridor's 4.9 ms with the GS drawing
  nothing. Their later stages (the view weapon's and those of models within
  600 units) add 2,000 vertices a frame to their 11,900.
- **Not kept**: lightmaps as vertex colours where the light is linear over a
  triangle (5 to 10 % of the lightmapped area within 4 of 255); taking and
  freeing memory at the end of a map's load so that play finds free frames
  (42 swap outs for 76, but 60 swap ins for 10); the models' files of fewer
  details (a player's parts have 80 and 60 % of the vertices, the file of
  models would be 56 MB for 26).
- **The EE's frames over a field** (about 100 of 1258) are not the
  renderer's: a profile has 38 % of the time waiting, the IOP's waits are
  2.3 s of the demo's 21 s: the pager (76 pages to the swap file in 510 ms,
  102 from files in 590 ms, 52 faults of the game's own thread) and the
  sounds' first copies to SPU RAM (17 ms each; 2.7 MB of sounds for SPU
  RAM's 1.8 MB, their ADPCM in the heap).

**The morning after**, the same measures:

| | The timedemo's passes a field | Still drawn when the beam came | The EE a frame (median) | Images a frame |
|---|---|---|---|---|
| The night's end | 7.15 ms | 44 to 105 of 6540 | 10.2 ms | 772 KB |
| Areas, lightmaps by place in pages of 64 x 64 | 6.81 ms | 19 to 57 | 10.9 ms | 671 KB |
| Less of the EE a run | 6.79 ms | 19 to 54 | 10.6 ms | |

Standing views (ms a field; none with a strip still drawn): q3dm1's
corridor 9.2 and court 11.2 (14.5 and 15.6 the evening before), q3dm12 13.6
(16.6), q3dm0 at a closed door 9.4 to 10.3 (13.0), q3dm17 9.4, q3tourney2
9.0, q3dm11 8.9, q3dm6 5.6.

- **A closed door** shuts its area off (the game's area mask, a chunk's
  area in the map file): the view of one in q3dm0 13.1 -> 9.6 ms.
- **Lightmaps.** q3map fills its pages in the BSP's order of surfaces and a
  view drew with most of a map's pages, 16 KB each: 448 KB in a view of
  q3dm12, where 6 images a frame then found no room. The map tool packs the
  surfaces' rectangles again by place, into pages of 64 x 64: 244 KB there,
  and none without room. The groups of a material and a page are smaller
  and a view leaves more of them out (that view: 22,000 vertices over the
  strips for 31,800), but there are more runs: 147 a frame in the timedemo
  for 101, and the EE's frame was 0.7 ms longer until a run's state was
  copied only as far as its program reads it.
- **An image with alpha on a lightmapped surface** went two ways through
  VU1 (the night's fix of the lost lightmaps): it has a CLUT of its own for
  that use now, and goes once (q3dm12: 16.6 -> 15.8 ms).
- **One frame ahead** (`r_ps2ahead`, maxsdk's `mx_hires_ahead`): the game is
  faster than the fields and two frames waited to be shown; what it drew
  was on the screen two fields later than it could be. With one the
  timedemo repeats 52 fields of 2190 where it repeated 10.
- The lamps' glows and bulbs turn to the eye (map file version 10).
- **Runs that seemed to hang did not.** Four of some 170 runs on the PS2
  ran into the runner's timeout (two standing views, two timedemos). Each
  had ended by itself: its picture was written, and the kernel's status said
  ready, with the program's return counted and its status 0, while the
  runner still waited. The last seconds of the program's output, the end
  marker with them, had not reached the host. That is the host link's, not
  the game's: a run's end should be taken from the kernel's status when the
  marker does not come.
- **The strips that were still late** were all in one scene of the
  timedemo (frames 416 to 424: a player bursts before the eye). Its passes
  took 25 ms a field: 12,000 vertices a strip, and a hundred sprites of
  blood that filled the screen up to six times. A million pixels of blended
  sprites cost the GS 1.9 ms. The quads have a limit now (`r_ps2fill`, 1:
  automatic): what the field's time leaves after the rest of the frame, by
  the passes' measured times; over it the sprites that fill the most for
  what shows of them (grown and faded) are left out. The timedemo, two
  runs: no strip still drawn, no pass left out (14 to 27 and 4 to 5
  without), 264 to 508 sprites left out in 56 frames; light scenes keep up
  to 4.6 times the screen.
- **Marks** (bullet holes, burns, blood on the walls, the players'
  shadows) were not drawn: the renderer answered the game's question for
  the parts of a square on the world with none. The map's surfaces are not
  on the EE, so the answer comes from the collision world: short rays along
  the projection at the square's corners, or at a grid of 5 x 5 points when
  a corner is off the world, of which the cells on the world are kept (as
  rectangles where it is flat). A ray is 25 us on the PS2 and the shadows
  ask every frame (4.5 squares a frame in the timedemo): 1.1 ms a frame.
  For squares of a shadow's size and larger the rays are those of a lattice
  on the square's plane, each asked once: 0.57 ms a frame in the timedemo's
  first pass over the map, none where a shadow was before. A level's marks
  are 190 polygons a frame, two thirds of them not in view: those are left
  out before their stages are worked out. With marks the timedemo's frame
  is 0.6 ms longer (18.0 for 17.4 ms with the wait for the field, 48 more
  fields repeated of 2240); `r_ps2marks 0` for none. A mark on a wall that
  is drawn behind a clip brush (ornate pillars) is not found.
- A shader of the game without a script (`icons/noammo`) is the screen's,
  blended, as in the engine: it was a model's, opaque (a white square over
  the weapon without ammunition).
- **tcGen environment on models** (the shine of the health orbs, armour,
  ammunition, powerups, the weapons' glass: 92 model shaders): the stage
  was drawn with the model's own texture coordinates, or left out. Two more
  VU1 programs (`q3me`, `q3mec`: the models' with the reflection of the
  eye's direction in the vertex's normal as coordinates; the direction is
  one for the model, the engine's is each vertex's): 25 cycles a vertex for
  21. Far off, where one stage is drawn, an item whose first stage is its
  shine shows its skin. No difference in the timedemo's passes.
- **Dynamic lights on models**: a rocket, a shot's flash add to a model's
  directed light and turn its direction, as the engine's do (16 at the
  light's radius, by the square of the distance).
- **Dynamic lights on the world.** A light's spot is a texture laid on the
  chunks near it by position (`q3wd`, `q3wdc`: the texture coordinates are
  the vertex's position along two of the world's axes, the vertex's alpha
  falls off along the third, the axis the chunk faces most: on a floor or
  a wall the spot is exact in every pixel), and the picture there is
  multiplied by 1 + the spot's alpha, as the engine's lights multiply what
  is there (adding the light's colour was a haze over dark walls). The GS
  has no product of colours, so the world's light is white; the models
  take its colour. Pixels outside the spot fail the alpha test. Cost in
  the timedemo with every light (1.1 a frame, 21 chunks each): 0.5 ms a
  field and 68 strips late; so a light is taken for the pixels of its
  sphere on the screen and shares the sprites' room (`r_ps2fill`): 80 % of
  the lights are drawn, 0.38 ms a field, no strip late. `r_ps2dlight n`:
  n lights at most (4), 0 none.
- **The view's width.** The game takes its 90 degrees across the 1920 x
  1080 picture, which leaves 59 up and down for the 74 of a 4:3 screen:
  the weapons are mostly below the picture's edge (the lightning gun
  nearly all of it). The PC's ioquake3 at 1920 x 1080 has the same view:
  it is the game's, not the renderer's (`cg_fov 106` is the 4:3 view's
  height).

**Against the PC** (the same day). The reference for the picture is the
PC's ioquake3 with its OpenGL 1 renderer, built from this tree (CMake, the
client alone, SDL2's headers from `code/thirdparty`) and run off screen
(Xvfb, Mesa) at 1920 x 1080, full screen: only then the engine has its
overbright bit and a screenshot the gamma's doubling, which is the look the
PS2 renderer follows. The same place by `+devmap`, `+setviewpos`,
`+screenshot`; the paks are the retail ones. libgl's renderer on the PS2
(gl1) is no reference: it has its own limits.

- **Brightness.** The PS2's picture was 6 to 10 of 255 darker everywhere,
  the shadows a third: the frame has 5 bits a colour and the GS cuts what
  it draws off to them (after its dither's -4 .. 3): 4 darker on average,
  and a lit surface is drawn twice (the texture, then times the lightmap).
  Now every palette of the file of images is 4 brighter (black stays
  black), and the lightmap's pass adds 6 (as the fog's colour with the
  fog's share at 0: the pass has no colour of its own; 4 for the 5 bits, 2
  for the product's rounding down, measured). World, models, weapon and
  HUD are within 1 to 3 of the PC's now; the picture read from the PS2's
  GS memory is PCSX2's.
- **A shader's vertex colour times its lightmap** (`rgbGen vertex` on the
  texture and a `$lightmap` stage: banners, grates, wires; 11 shaders) was
  drawn by the vertex colour alone, 2.5 times too bright: the map tool
  multiplies the two.
- **Textures are half as sharp**: the images are at most 128 texels a side
  (256 on the PC: `tools/ps2pak.py --max`), for the 1.2 MB of GS memory
  that beam racing's frame and Z strip leave. A frame draws with 690 KB of
  them; at 256 it would be four times that. What could be done: the finest
  level only for the few textures near the eye, by the distance of the
  chunks that use them (the file of images grows fourfold and its pages
  are read from the USB stick as they are needed).
- **Walls that were black** (q3ctf1's blue walls): `tools/ps2pak.py`
  resampled an image whose sides are not powers of two with Pillow, which
  multiplies an RGBA image's colour by its alpha first: where the alpha is
  0 the colour came out black. On these walls the alpha is where they
  shine, not where they are seen through. Colour and alpha are resampled
  each by itself now.
- **Textures of 256 texels** (the user's question: what do they give, with
  GS memory as a cache that is refilled). A file of images with levels of
  256 (`ps2pak.py --max 256`, 62 MiB for 24) and:
  - *Everything from 256, the cache as it was*: the views want 1.7 to 1.9
    MB of 1.2, and the textures that find no room are drawn from their
    small levels, whichever they are: sharper floors, a banner more blurred
    than before.
  - *Only the levels a view needs* (a texture from the level its nearest
    chunk is drawn with; the levels as pieces of the cache each by
    themselves): at 1920 pixels across nearly every texture of a room
    needs its level of 256 (two texels a pixel within 1000 units), so
    little is saved, and the lookups of the pieces cost the EE 1 ms a
    frame. Not kept.
  - *A pool for the first levels* (kept, `r_ps2texfine`, off by default):
    a level of 256 x 256 is 8 pages of GS memory in a row, which the cache
    seldom has free between its small pieces. So the cache holds every
    image from its level of 128, exactly as before, and the first level of
    the textures whose texels are largest on the screen (nearest first;
    one that has a place keeps it) is in a pool of its own, in units of 4
    pages. With 8 units (256 KB, taken from the cache) the four to eight
    textures nearest the eye are as sharp as the PC's: the floor under the
    player, the wall beside him. Standing views fit. The timedemo (the PS2,
    a file with the 256 levels of three maps): 20.0 ms a frame for 18.4,
    no strip late; 34 MB of images sent for 17; 146 swap ins and 252 outs
    for 43 and 117 (the first levels' data is three times the textures'
    memory on the EE: most of the cost), and 1343 images that found no room
    in the smaller cache for 93.
    A first level is read from the file when its image first gets a place
    in the pool (by the pager, ahead; not at the map's load), and stays in
    memory: letting it go once it is in GS memory made the frame 22.1 ms
    (819 pages read again from the file, 6 ms each). With 4 units (two to
    four textures): 19.5 ms, 516 images without room.
  - What stands in the way of more: the EE's 32 MB (the paging), then GS
    memory (1.2 MB beside beam racing's frame and Z), not the DMA. And
    MXOS's window for mapped files is 64 MiB: the whole file of 256 levels
    does not fit beside the models' (asked of MXOS: the program's
    `map_size`, 128 MiB).
  - *Paging without waiting*: a level of a mapped file of 32 KiB or more
    may be paged out, as long as a coarser level is there to draw with
    meanwhile; the rest (the smaller levels, an image's last level, the
    CLUTs) is pinned while the image is loaded (q3dm1: 2.5 MB of a file of
    128, 2.9 MB of one of 256; the full game's timedemo: 3.5 MB). When a
    frame wants a level that is not in memory, the coarser one is sent and
    the pager asked for the finer, which a later frame sends: the game
    never waits for the file (it waited 523 ms in the timedemo before).
    The pinned levels are sent without being locked each frame.
  - *Sending in the passes* (`r_ps2texstream`, pages of GS memory taken
    from the cache, off by default; `r_ps2texstreamkb`, the most a frame
    sends there): an image that finds no room in the cache is not drawn
    coarser but sent in the strips' records, right before the first run
    that draws with it in each strip. The runs' order is known when the
    frame's list ends, so the area is planned then (Belady's choice over
    the passes' order: this strip's runs, then the next strip's from the
    first on), and a strip sends only what the strip before did not leave
    there. The KB a frame streams follow the time the passes leave of 80 %
    of a field (a quarter of it, about 1 us a KB), and drop to the least
    for half a second after a pass the beam overtook.
    - q3dm1's view (256 levels, 48 pages, no limit): no image coarser for
      room, none without room; 1.2 MB a frame streamed, sent 1.6 times
      (2.1 MB a field, 2.3 ms more of the passes: 1 GB/s). The floor and
      the walls as sharp as the PC's. With the limit the governor's bias
      goes down slowly (1.5 at the picture).
    - The full game's timedemo (a file of 256 levels, 37 dB): 23.6 ms a
      frame with 48 pages (437 KB a frame streamed, sent as 600; one pass
      overtaken by the beam in 8743), 23.1 ms with 32 pages (8 overtaken),
      22.6 ms without (none); 19.0 ms for the file of 128 levels at 35 dB.
      Its passes take 5 to 7 ms a strip already, so there is little to
      spare: without the limit (1 MB a frame) 620 passes were overtaken.
    - A pass left out would leave the next strip with what the strip
      before it did not send: drawn with the wrong texture for a field
      (none was left out in these runs with the limit).
    - The uploads go by DIRECTHL: a later frame's once record over PATH3
      in intermittent mode lets DIRECT in between its slices (EE User's
      Manual 7.1.3). A texture is sent without a FLUSH first when no run
      of the strip drew from its pages (the pass begins with one).
  - *CLUTs below the picture*: 540 lines at 16 bits leave the lower 36
    lines of the framebuffer's last row of pages unwritten (with a Z strip
    of its own): 120 runs of 4 blocks at 1920 (`mx_hires_spare`), each a
    16 x 16 CLUT. A CLUT keeps its place while frames draw with it; with
    none free the cache has it. The full game: all 74 CLUTs a frame there,
    1454 images without room for 2075 (1920, 256 levels).
  - *Half the width* (`r_ps2width 960`: the display shows each pixel
    twice; the textures keep the levels of 1920, the lines as sharp as
    before): framebuffer and Z strip take half, the cache 330 pages for
    152. Full game, 256 levels: 19.3 ms a frame (22.6 at 1920), 9 images
    without room, the passes 4.6 ms a field; the same as 1920 with the
    file of 128. Z in the strip (374 pages) makes passes late; 9 strips of
    64 lines cost VIF1 and VU1 twice the work (draws across strips, each
    run's set-up in every strip): late passes even at 960.
  - *The width from frame to frame* (`r_ps2width 0`, maxsdk's
    `mx_hires_widths`): three layouts of the strips (whole width; half at
    the buffer's start; half at its end) so that a record's passes never
    draw over strips the beam has still to show of the record before
    (half-front follows anything, whole follows whole or half-end). At
    half the width the rest of the buffer and the Z strip go to GS memory
    (the cache takes them) once the frames at the whole width are done,
    and come back (`mx_vcache_release`: the entries there placed again
    elsewhere) before a frame at the whole width. Half when textures are
    placed coarser than their distance asks or the passes take 80 % of a
    field; whole again after 2 s that would fit. Full game, 256 levels:
    20.6 ms (nearly all at half), 21.2 with the stream area; a change every
    30 frames (a test, `r_ps2widthflip`): about 5 frames until the whole
    width is back.
  - *Models in the strips piece by piece*: a model's pieces (VU1 batches)
    each by its own box in the two frames, so a piece is drawn in the
    strips it touches only: 7 % fewer vertices through VU1 at 3 strips, 18
    % at 9.
  - Found on the way: a frame was taken for done while an older record's
    last pass was still read (a record whose passes were all left out
    retired at once): maxsdk's hires now counts frames done up to the
    oldest record still played; and a frame without textures in the
    stream area called the chains of the frame before (now freed): VU1 ran
    in garbage and the passes stopped. VU1 left running by a program is
    not reset by the kernel for the next one (asked of MXOS): mx_hires_init
    resets it.
  - *The numbers on the screen* (`r_ps2hud 1`): width, frames a second,
    the share of frames at half the width, the passes' time and the ones
    late, the cache's memory used and wanted, the governor's bias, what a
    frame sends and streams, the CLUT slots, pages read in.
  - *The beam's margin* (`r_ps2margin`, maxsdk's `mx_hires_margin`, 8
    lines): a pass starts that many lines after the beam left its strip.
    At 960 wide the strips' last lines showed black and surfaces not
    drawn yet, and the picture's bottom was cut off: maxsdk reckoned
    every strip's line 22 lines too early (it read the picture's place
    from the registers of the display while off, since the display stays
    off until the first frame), and a pass clears its strip first, at
    half the width in 6 lines of the beam. 1920 hid it: the clear takes
    twice as long. Measured on the TV with maxsdk's `examples/beam` (the
    passes start early on purpose, a magenta band on a scale of lines
    shows where the beam was) in every TV mode; maxsdk's `docs/beam.md`.
  - *Every texture at its level, through the stream area* (a test,
    `r_ps2texforce 1`: none coarser for want of room; what the cache has
    no room for is sent in the passes whatever their time, or not
    drawn), at 1920 with the levels of 256, timedemo four: the frames
    are no slower (median 21.8 ms for 22.8) and no image is left out,
    but a frame draws with 1.8 MB (3.2 MB at most) for a cache of 0.8
    MB, so 1.1 MB a frame goes through the area, 1.7 MB a field; the
    passes take 9.5 ms a field in the mean and more than a field where
    it is busy: 989 of 8422 passes were still drawing when the beam
    came, 164 left out. A larger area takes the cache's pages and makes
    it worse. With the Z in the framebuffer (`r_ps2ownz 0`: 720 KB more
    for the cache, half the streaming) a pass has one strip's time
    only: 470 passes overtaken even without the test, 1242 with it. So
    1920 with the levels of 256 does not fit a field; 960 with 256 or
    1920 with 128 do. For comparison, same demo: 960, levels of 128, no
    area: median 18.5 ms, 1069 of 1258 frames in one field. Open: the
    test with a Z strip of its own hung at the screenshot.
  - Found on the way: at the width from frame to frame, textures in the
    cache's first pages turned black and the view weapon flickered. The
    frame's layout was chosen when its record opened (the stream's first
    half), not at `mx_frame_begin_n`: until then `mx_hires_frame` gave the
    whole width's FRAME, which the Z clear before the view weapon and the
    HUD's models writes (for its mask), so the clear and all after it in
    the strip went through a buffer twice as wide: Z over the cache, the
    colour nowhere seen. maxsdk now chooses the layout when the frame
    begins. Found by reading back all of GS memory at a screenshot and
    looking at it as a picture.
- **Fog** (15 of the maps have fog volumes: pits, mostly). The map file
  (version 11) has the volumes: colour, the depth at which the fog is
  opaque, the plane of its top. A volume's surfaces are drawn once more
  with the fog's colour over them, as the engine does: a texture of the
  fog's strength (the root of depth / opaque depth, times the share of the
  eye's ray that is in the fog) with coordinates from the vertex's
  position, by the program that lays the dynamic lights' spot on the world
  (`q3wd`): no new program. The share is the engine's t / (t - eye) made
  straight, since the program's coordinates are linear in the position.
  What stands in the fog (models, sprites) is not fogged yet.
- **tcGen environment on the world** (q3dm0: 32 of its 113 shaders; the
  shining walls, floors and pillars of the "base" maps; the usual script is
  the shine, the texture blended over it, the lightmap). The shine was
  drawn with the surface's own texture coordinates: blotches. The world's
  vertices have no normals, but a chunk is of one direction: with the
  chunk's direction for the surface and the eye's distance taken at the
  chunk's middle, the engine's coordinates are linear in the vertex's
  position, so the stage is drawn by the program that lays a texture on
  the world by position (`q3wd`), a run a chunk. In PCSX2 as the PC's; not
  yet run on the PS2.
- **Coloured light** (q3dm9: an orange pool on a large wall was grey).
  The lightmap the PS2 draws is brightness alone and its colour is the
  vertices': the colour was the lightmap's at the vertex itself, which
  misses a light that falls between the vertices of a large surface. It
  is now the sum of the lightmap's texels nearer to that vertex than to
  the surface's others (the map tool; the maps are to be made again).
- **Water was white and hid its floor** (q3ctf2, and every `liquids`
  script with `blendFunc GL_DST_COLOR GL_ONE`: 63 stages in the game's
  scripts). That blend is the picture times 1 + the texture; it was drawn
  as the picture plus the texture, which on a dark floor is the texture
  alone, three layers of it. The GS has no product of two colours, but it
  has the picture times 1 + alpha (the blend of the dynamic lights' spot):
  the stage is drawn so, with the texture's brightness as its alpha (the
  CLUT the filter stages have). The hue of the texture is lost (the water
  is 8 of 255 less blue than the PC's), its brightness is the PC's.
  And the `$lightmap` stage after such stages was twice too bright: the
  engine's frame is half the picture (overbright doubles it at the end), so
  a lightmap stage over what is in the frame multiplies by the lightmap's
  value itself, where over a stage of the surface (drawn at half) it comes
  out as twice it. The pass after stages that are all products now has a
  CLUT of half the brightness. q3ctf2's pool against the PC (mean of a
  region, 0-255): near 28 55 54 for the PC's 29 57 56 (it was 40 91 93),
  far 63 91 98 for 55 91 104 (it was 142 158 161).
- **More images than GS memory holds** (q3ctf2 and q3tourney4 from their
  spawn points: 1.4 to 1.7 MB of images at full size for the cache's 1.2).
  The images drawn last found no room: the view weapon, the HUD's head and
  icons, a few walls (white). Now, while a frame's images are more than fit,
  a texture of the world is placed from the level its nearest surface in
  the view is drawn with (the GS takes that level there anyway), and no
  finer; and while an image still finds no room, the levels go coarser
  sooner than the distance asks, a quarter of a level a frame, to two
  levels at most. Back down when the frame's images would take under 93 %
  of the memory with a quarter less (computed from the textures' sizes,
  not tried), and after one try that was too much not again for that view.
  A texture keeps the level of the frame before within half a level, so
  one at a boundary is not sent again each frame. Both views: no image
  without room (744 and 3829 in 370 frames before), the far walls a little
  less sharp. It does nothing while the images fit (the timedemo).
  The views are what they are: at 1920 across most of a view's textures
  are near enough for their first level, so the far ones give 100 KB of
  the 300 that lack; lightmap pages are a quarter of the memory (packed by
  place already), and the cache's own loss is 10 %.
- **The console's font took 65 KB of GS memory** in every frame with text
  on it (256 x 256, 8 bits): one colour with 242 values of alpha, which
  the texture tool's palette of 16 could not hold, as it began from the
  colours alone (one). The palette's free entries now go to the texels
  farthest from the ones there are: the font, the menu's font and 40 small
  pictures (the HUD's icons, the crosshairs) are 4-bit (half the memory),
  30 more pictures with alpha have a better palette of 256.
- **Girders and grates were not lit** (q3dm19: gold girders at full
  brightness in a dark hall; 50 of the game's scripts are a texture whose
  alpha is tested with a `$lightmap` stage after it). Such a surface has no
  lightmap pass on the PS2 (the pass cannot leave out the texels the test
  leaves out) and is lit by its vertices, but its stage is `rgbGen
  identity`: it was drawn unlit. The map tool now marks these materials
  and gives their vertices the lightmap's light around each (the mean of
  the surface's texels nearest to it; the BSP's own vertex colours are
  another light), and the stages before the script's `$lightmap` stage
  take the vertex's colour. The maps are to be made again (same version:
  a flag more).
- **Heads, torches and skulls on the walls twice too bright** (q3dm2's
  wall head: 92 50 12 for the PC's 51 28 8). They are models built into
  the map with `rgbGen vertex`, which the engine draws at half the vertex's
  colour (its identityLight; only `exactVertex`, and a surface without a
  script, takes the colour whole). The world's stages took it whole.
- **A fog's own face was a sheet over it** (q3ctf1: the ground fog a
  white floor; q3dm4: from inside the pit's fog the hall outside it a grey
  wall). The fog's brush has a face whose script has no stage: the engine
  draws it with the fog's pass alone. It was drawn by a pass of its own,
  untextured, and the fog's pass over that. A script's material with no
  stage, texture or lightmap now has no pass of its own; the fog's pass
  over the face is what it was (seen from inside, the haze over what lies
  outside the fog; from outside nothing). q3dm4's view against the PC: 65
  59 52 for 67 61 55 (was 88 83 76).
- **The fog seen from above was too thin** (q3ctf1's ground fog 53 50 50
  for the PC's 74 71 74). Its strength goes by the share of the eye's ray
  that is in the fog, t / (t - eye): made straight as t / (128 - eye) it
  was right only for a vertex 128 units under the fog's top. The
  coordinate is now t in eye's heights over the fog, which is linear in
  the position, and the fog's texture has u / (1 + u) down it (its last
  row is all of the ray, for an eye in the fog). q3ctf1: 70 67 71.
- **Fog on what stands in it**: a model whose middle is in a fog volume is
  drawn once more in the fog's colour, as strong as the world's surfaces
  have it there (one strength for the model: it is small against a fog's
  depth; the engine's is each vertex's). Not yet the sprites.
- **q3dm1's throat was black** (the cheeks of the mouth: a scrolling
  layer with `rgbGen vertex`, the skin blended over it with its own colour,
  the lightmap last). Before a late lightmap every stage takes the
  vertices' colour, which is the lightmap's hue; but a material whose first
  stage is `rgbGen vertex` has that light in the colours too, and it is
  dark there: the skin over it came out as dark. A stage of its own colour
  on such a material goes without the vertices' colour (three scripts). The
  cheeks have the PC's brightness (109 for 113) but not the red of their
  light (the hue alone is not in the map's vertices for these).
- **The shine of `alphaGen lightingSpecular`** (88 scripts, in 23 of the 35
  maps: the shining walls and trims of the "base" maps; `blendFunc
  GL_DST_COLOR GL_SRC_ALPHA` over the lightmap, so the lightmap times the
  texture plus its alpha, and the alpha is the fourth power of the cosine
  between the eye and the reflection of a light at a fixed place of the
  map). It was not drawn: those walls were a quarter darker than the PC's
  (q3dm12: 71 for 102). It is a pass of its own over the map's, a chunk at
  a time, by the program that lays a texture on the world by position: the
  eye's direction mirrored in the chunk's makes the cosine one between two
  unit vectors, whose difference is made straight around the chunk's
  middle; two of its parts are the coordinates in a texture of the fourth
  power, the third the program's fall-off. The picture times 1 + that (the
  dynamic lights' blend), so the texture brighter by its shine, where the
  engine adds the shine by the texture's alpha: about the same for these
  grey textures. Chunks without shine at their middle have no pass. q3dm12:
  81 and 94 for the PC's 83 and 102. `r_ps2debug 16777216` leaves it out.
- **Where the EE's time goes now** (a profile of the timedemo on the PS2,
  kernel 42, 18.9 ms a frame with the profiler): a fifth of it the EE
  waits (the kernel's semaphore wait: for the field, and 1.15 s of the
  run's 23 for pages: 104 read from the mapped files at 5.8 ms each, 77
  written to the swap file); the renderer 35 % (6.6 ms a frame: the world's
  cull 0.9 ms, a model's draw 0.55, the quads' areas for the fill limit
  0.53, the list's end 0.43, placing images 0.4), the rest of the kernel
  6 % (the TLB refills), collision 6.6 %, memcpy and memset 5.6 %, sound
  2.5 %. The median frame is one field; the mean is 1.9 ms more by the
  frames that take two (one in nine) and the waits for pages. Nothing in
  it is large: the next gain is in the frames that take two fields, not in
  a function.
- **The frames that take two fields are waits for pages.** The timedemo's
  report now has the frames by the fields they took: 1150 one, 91 two, 15
  three, 2 more; the late ones single frames in 85 places, 1.46 ms a frame
  over one field in all (so 18.5 ms for 16.7). In those frames the VM
  wrote 75 pages to the swap file (0.49 s), read 104 from the mapped files
  (0.62 s) and the main thread waited 0.38 s for the VM's lock while the
  pager read: a wait of 6 ms costs a field. The run starts after the map's
  load, whose dirty pages go out one by one as the demo brings in
  textures and models' frames. Making room at the end of the load from the
  program (4 MB asked for, read and given up as cold) made it worse: the
  pages that went were mostly the files' (99 swap outs for 75, 41 swap ins
  for 10). It is the VM's to solve (asked of MXOS: a call that writes the
  dirty pages out after a load; no lock held through the pager's I/O).
- **Mirrors** (q3dm0's; a portal script at a misc_portal_surface without a
  camera of its own): the scene once more from the eye mirrored in the
  mirror's plane, before the view (world, models, sky; the plane as its
  near plane, the PVS from the mirror's room), its depth taken away again,
  then the mirror's chunks as depth alone (`RP_ALPHA_DEPTH`: the GS's test
  fails every pixel and keeps its depth), so the view leaves the mirrored
  picture there and the mirror's stages are laid over it. The player's own
  body (RF_THIRD_PERSON) is drawn in it only. Not in it: sprites, marks,
  lights, fog. Its view is cut to what the mirror's opening shows (planes
  from the mirrored eye through the mirror's edges). On the PS2 it is too
  much when the mirror fills the picture (q3dm0 from 160 units: a field's
  passes 16.7 ms, 992 of 2415 left out, the game at a few frames a second):
  off by default, `r_ps2mirror 1` turns it on. A mirror at the size of a
  quarter of the picture or less would be in budget. Portals with a camera elsewhere (q3dm0's
  teleporter, q3dm7, q3dm11) still show their script only.
- **Marks: fewer rays.** A small mark (a bullet's) on a flat wall takes two
  rays: the first corner's gives the wall's plane, and when the opposite
  corner's ray ends on it the other two corners are where theirs meet it
  (426 of the timedemo's marks). Shadows (48 across, asked every frame) by
  a lattice of 24 for 12: a running player's comes to new points less
  often. A frame has 64 rays for small marks (`r_ps2markrays`; 8 left out
  in the timedemo). The timedemo's rays 19648 -> 9524, 688 -> 402 ms.
- **mx_vm_clean** (kernel 43, asked of MXOS) at the end of a level's load
  (`r_ps2clean`): the load's written pages go to the swap file then (424
  pages, 2.5 s) and stay in memory; the timedemo's swap outs 77 -> 2, the
  lock waits 407 -> 215 ms, 18.67 -> 18.47 ms a frame. The late frames left
  are mostly the 104 pages read from the mapped files. (On a kernel before
  43 the call does nothing.)
- **Textures under memory pressure: measured, left as it is.** The file of
  images is mapped (32 KiB pages); an image's CLUT and levels lie together,
  the images by name; a map reads its images at its load and the pages stay
  (clean: given up without a write when memory is short, read again on the
  next use, then the game waits). In the timedemo no level was sent while
  its pages were away. A layout with the first levels apart (packed on
  pages of their own, the small levels together; an image drawn from its
  second level while its first is away, the first levels not used for ten
  seconds told cold) was tried on the PS2: the map's images then take about
  twice the pages (an image's small levels and first level each share a
  page with other images'), and the timedemo swapped ten times as much (99
  to 110 swap ins for 9; 18.9-19.0 ms a frame for 18.6). Not kept. For the
  file of 256 levels the first level is apart already in effect (the cache
  holds images from the 128 level; the pool reads the 256 level ahead).
- **Also measured on the PS2 and not kept:** the level's models read at its
  load (5 MB, 0.7 s): 47 file pages in the timedemo for 104, but 107 swap
  ins for 9 (memory is full: what the models take, the game's own pages
  give), 18.71 ms for 18.72. The sounds as a mapped file (ps2snd.bin, 12 MB,
  in place of the 2.7 MB a level copied from the pak): the load 1.9 s
  shorter, but the sounds copied to SPU RAM during play (23 that SPU RAM
  had let go) are then read from the file: 132 file pages for 105, 18.69 to
  18.73 ms for 18.58.
- **Portals to another place and mirrors: correct in PCSX2, NOT on the
  PS2.** The view through a portal uses the engine's transform (surface to
  camera, the camera's roll), the camera's PVS, the camera's plane as the
  near plane, the opening's planes, and alphaGen portal (the haze by the
  eye's distance; opaque when no view was drawn); a view only while the
  opening covers a quarter of the picture. In PCSX2 q3dm0's teleporter
  shows the room with the machine as on the PC. On the PS2 (a SCPH-90006)
  standing at q3dm0's portal and mirror hung the console three times (a
  reset by hand) and the picture flickered heavily (beam racing's strips
  late). `r_ps2mirror` is 0 by default; the cause is not found.
- **The sky had no clouds**, one colour: its layers were drawn with the
  texture level chosen for a surface 16,000 units away, the smallest. They
  are drawn with the first level (a cloud's texel is pixels wide).

### The units' load, frames by the strip, the Z in the picture (2026-11-20)

`ps2_load <hz>` (`code/ps2/ps2_load.c`) samples, over a timedemo, what the EE
runs (the game, the renderer, the kernel, or the kernel's idle loop: asleep)
and what the drawing units do (VIF1's DMA, VU1 running or waiting at an
XGKICK, the GS's FIFO, VU0), each sample with the beam's line; the run
writes `quake3.load`, which `tools/ps2load.py` prints. Timedemo four, the
full game with sound, 1080i at 1920 wide, a SCPH-90006:

| | The EE busy | of it the renderer, the game, the kernel | A pass being sent | VU1 runs | VU1 waits for the GS | The GS's FIFO not empty | VU0 |
|---|---|---|---|---|---|---|---|
| Before (a Z strip, a frame a field, textures 256 and the stream area) | 85 % | 59, 19, 8 | 74 % | 14 % | 47 % | 60 % | 0 |
| Now, textures 128 (`r_ps2mode 0`) | 96 % | 60, 27, 9 | 71 % | 17 % | 48 % | 56 % | 0 |
| Now, textures 256 (`r_ps2mode 1`) | 93 % | 60, 24, 9 | 72 % | 17 % | 49 % | 57 % | 0 |

- At 1920 wide the EE and the GS are both loaded: the EE is full, a pass is
  on its way for seven tenths of the time. At 960 wide (`r_ps2width 960`)
  the drawing units are idle for 55 % of it (a pass 44 %, the GS 31 %).
  VU1 mostly waits at an XGKICK for the GS; VU0 runs nothing.
- What is left on the EE is not work another unit can take: of its time
  17 % is FPU arithmetic, the rest the lists, the textures' cache, the 2D
  pictures and the game.
- A field's passes together, from the log of the passes (`r_ps2passes`): at
  1920 the median is 11.9 ms, 90 % 15.1 ms, 99 % 17.0 ms, the longest 18.5
  ms, of a field's 16.7; at 960 7.4, 10.3, 13.0 and 14.4 ms. A fit to what a
  field held: 6.5 ms and 1.8 ms a million pixels of quads and 14 us a
  set-up (300 set-ups a field); the vertices do not show. Without the
  lightmap's layer (`r_ps2debug 2`) 2 ms less.
- The sampler costs 5 % of the frame rate at 2000 samples a second, and a
  timer that calls a function which returns at once 3.4 %: a timer's call
  into the program takes the kernel about 17 us.

What was changed, each measured with the same timedemo at 1920 wide (frames
a second; passes still drawn when the beam came, of about 9200):

| | | |
|---|---|---|
| This morning: a Z strip of the passes' own, a frame from a field's first pass, textures 256, stream area 48 (the numbers on the screen on) | 40.1 | 0 |
| Those settings with the code of now | 42.8 | 0 |
| The Z in the picture (`r_ps2ownz 0`), a frame from the next strip's pass (`r_ps2swap 1`), textures 128, three strips | 62.2 | 166 |
| Four strips, the last two as one: `r_ps2mode 0` (two runs) | 62.2 | 4, 8 |
| The same with textures 256: `r_ps2mode 1` (two runs) | 55.1, 55.4 | 34, 21 |
| For comparison at 960 wide: `r_ps2mode 0` | 63.5 | 0 |
| and `r_ps2mode 1` | 58.6 | 0 |

- **A frame from the next strip** (maxsdk's `mx_hires_swap_strips`): a
  record that waits is played from the next pass on, not from a field's
  first. The EE never waits for a field (four fifths of the demo's frames
  begin within one), at the price of the two frames' difference at a
  strip's edge.
- **The Z in the picture**: at 1920 wide the textures have 242 pages (1.9
  MB) for 104 and a stream area of 48. A pass then has the time the beam is
  in the strips that are neither its own nor its Z. Of three strips that is
  one strip's, without borrowing. Five strips of 128 lines borrow but cost
  the EE 0.6 ms a frame (the draws' strips, the lists a strip), and the
  fifth is the picture's last 28 lines. So the last two are one
  (`mx_hires_merge_last`): four strips, the last of 156 lines with its Z in
  strip 0 and the first 28 lines of strip 1, a pass with two strips' time
  and what the others leave. At 960 wide no pass is overtaken (thirteen
  runs); at 1920 a few in the demo's heaviest moments, where the passes of
  a field take more than a field.
- **Textures of 256 at 1920**: 1.9 MB does not hold a frame's images at
  their full size (2.0 MB in the mean, 3.9 at most): in 1146 of 1258 frames
  some are placed coarser (2346 without their finest level, 172 found no
  room). At 960 wide they fit.
- **From the EE to VU1**: a model's pieces were each tested against the
  strips' lines (0.7 ms a frame) to spare VU1 a fourteenth of the models'
  vertices; the programs leave out the triangles outside the pass's strip
  anyway. Now a model's surface is drawn in every strip the model is in
  (`r_ps2debug 67108864`: as before).
- **The sound** took 1.5 ms a frame: a level's sounds are 2.7 MB for SPU
  RAM's 1.8, so sounds leave it and are copied again while the game plays,
  the EE asleep for the IOP's 3 ms a 4 KB (a second a run); and two loops a
  frame over all 1024 entities' looping sounds. A thread makes the copies
  now (`s_ps2async`; the sound starts when its copy is there), the loops
  look at the live entries.
- Tried and not kept:
  - The quads' and the strip tests' camera transforms as vector code on VU0
    (`-mvu0`: one multiply-add chain for three dot products): 61.5 frames a
    second for 63.5 (at 960). Bringing the numbers from the FPU to VU0 and
    back costs more than the products.
  - A set-up's GS registers kicked by the run's first batch, after its
    vertices, not by the set-up before them (`tools/ps2vu.py`: the packet
    copied before the batch's own; a second program a set-up, for the turn
    of VIF1's buffers that the set-up's kick had kept safe): the passes as
    long as before. A set-up's 14 us is VIF1's and VU1's own work, not the
    wait for the GS.
  - The world's runs and chunks nearest first: the passes as long as before.
  - A lit run's chunks all through the program that cuts where one of them
    is: 7 set-ups a field less of 163. A margin of 4 lines for 8. The
    world's chunks in their group's strips (`r_ps2debug 134217728`). None
    changes the passes overtaken.
  - The textures between two levels (`r_ps2trilinear 1`, at 960): the GS's
    alone, a fifth to a third more of a pass; `2`: only while the passes
    are short. Off by default.
- A profile of the frame now (16.9 ms with the profiler): the renderer 8.1
  ms (the lists 1.9, the quads 1.4, the world 1.3, the models 1.3), the
  textures' cache 1.3, the game 2.3, collision 1.0, memcpy and memset 1.0
  (95 KB a frame in 600 copies: a run's state 37 KB, the scene's polygons
  18, its entities 13), the kernel 1.5, asleep 0.7. With the passes not
  played (a broken build) the EE's frame was 12.8 ms for 16.1: what the
  passes cost the EE itself (their interrupts, the DMA on the bus).
- **Large triangles are no load for the GS.** Standing with the nose against
  a wall (q3dm6, the wall over the whole picture, 1920 wide): the passes
  take 5.9 ms a field, for 8.5 ms with a pillar over half the picture and
  the room behind it: two layers of 1 million pixels at 0.5 EE cycles a
  pixel, the GS's best. And what of a triangle lies outside the picture
  costs nothing: with the guard band at the picture's edge (`r_ps2guardx
  1000`, `r_ps2guardy 300`: VU1 cuts every triangle there) the passes are
  as long (8.6 ms for 8.5) and the picture the same. So cutting large
  triangles up on VU1 would gain nothing. (The strips seen flickering at a
  wall on 2026-11-18 were, by this, the beam's line of that day: each pass
  started 22 lines early and cleared its strip's last lines under the
  beam, which shows most on a wall: docs/beam.md in maxsdk.)
- The passes' clear of the Z alone (FRAME's mask over the colour): as long
  as with the colour. Without the lightmap's layer the mean field is 9.8 ms
  for 11.9, but the heaviest are 16 to 17.6 ms all the same: those are
  fields of many models and sprites, not of the world's two layers.
- Leaving out more while the passes are near a field (over 80 %: the marks
  with the sprites, a quarter of the screen for the quads' least; over 90 %
  or after a pass overtaken: a tenth, and the models' first stage alone):
  the second level was on in four fifths of the timedemo's frames, for 3
  and 4 passes overtaken where there were 4 and 8 (textures 256: 16 and 18
  for 21 and 34). The heavy fields are heavy of the scene itself. Not kept.
- Six minutes of play (q3dm1 and q3dm2 with bots, 1920 wide,
  textures 256; the EE at 60 to 140 frames a second): the passes 10.3 ms a
  field in the mean, 2197 of 87,918 still drawn when the beam came and 88
  left out. More than the timedemo's 0.3 %.
- Open at 1920: the passes overtaken in the heaviest moments (the GS at its
  limit: what is left to take away there is the work itself, as the quads'
  fill limit and the lights' do already).

- Two remedies for them, built as settings and tried by the user on the TV
  (2026-11-20, evening; maxsdk `mx_hires_skip_late`, and `mx_hires_widths`
  without a Z strip of the passes' own). Timedemo four, a SCPH-90006, textures
  128 and 256:

  | Setting | fps | Passes overtaken | Left out | Textures' memory |
  |---|---|---|---|---|
  | as it was | 61.9 / 55.5 | 6 / 16 to 24 | 0 | 1936 KB |
  | `r_ps2ownz 1` (five strips, `r_ps2skip 1`) | 59.9 / 51.7 | 0 / 0 | 3 / 2, all ahead | 1456 KB |
  | `r_ps2width 0` (half the width for a time) | 62.4 / 55.9 | 0 / 5 | 0 | 1920 KB |

  - `r_ps2skip`: a pass that would still be drawn when the beam comes (by
    its strip's last pass and an eighth more) is left out; never twice in a
    row for a strip. The frame rate lost is the fifth pass's and the smaller
    cache's (1755 textures evicted for 1319).
  - `r_ps2width 0` with the Z in the picture: the half width is drawn into
    the same buffer (the left half of its lines), so GS memory does not
    change and any width follows any other; the passes still queued when
    the width changes play the record they were for. Half at a pass
    overtaken or left out, or when the passes take over 95 % of a field
    (`r_ps2halfat`); a second at least; back when the whole width would
    take under 80 %. In the timedemo 44 % of the frames at half, 18 changes
    in 21 s. Per frame the passes take 8 to 15 ms of a field at the whole
    width (textures 256; up to 18) and 5 to 9 at half: 1.65 times.
  - The user played both on the TV: both look good, both stay in the
    code, and `r_ps2width 0` is the default since (the timedemo with the
    defaults: 61.1 fps and 1 pass overtaken, textures 256: 54.8 fps and 2).
  - Open from those two play sessions: with `r_ps2ownz 1` the game hung
    after ten minutes (no crash report, no stuck pass in the log; on a
    kernel tried from RAM, so the cause is open: the new code or the
    kernel). With `r_ps2width 0` the console's lines stopped reaching the
    host after 105 s, a second level had no bots, and then the PS2 no
    longer answered for some minutes. Neither was seen in the timedemos,
    which are a minute long: a longer run without a player is the next
    test.

- The game modules' data (2026-11-20, late). qagame, cgame and ui are
  linked into the program, so their data was not new when a level was
  loaded, as a library's is that the engine loads anew, and the game's code
  counts on that. Seen from the second or third level of one run on: the
  loading screen's icons showed skins (cgame's lists of icons are never
  emptied, their shaders were of a registration gone), and no bots came
  ("client 0 already setup": the bots' states in the game's pool, which is
  not zeroed; then "AAS not initialized": the time of the bots' last frame
  is a static, so the bot library got no frames). Now the Makefile's module
  rule marks each module's .data and .bss, and `Sys_LoadGameDll` puts the
  data back as linked and zeroes the rest at every load. Tested on a SCPH-90006
  without a player: three single player levels in one run, bots in each;
  the icons right at the third load (`r_ps2shotload n`: the picture of the
  n-th load's screen). Ten timedemos in one run (five minutes) ended as
  they should, the console's lines all there: the hang and the lost console
  of the play sessions did not come back there.

- The units' load with the defaults of now (`r_ps2width 0`; the sampler at
  2 kHz, timedemo four, textures 128 / 256): the EE busy 95 / 93 %; a pass
  being sent 60 / 58 % of the time (71 % when all frames were 1920 wide);
  VU1 running 55 / 53 %, of it waiting at an XGKICK 38 / 36 % (48 %); the
  GS's FIFO not empty 45 / 43 % (56 %); VU0 0 %.
- `r_ps2ownz 1` ten timedemos long without a player, on the card's kernel:
  ended as it should; of 97,525 passes none overtaken and 41 left out
  ahead. So the hang of the play session with it is not reproduced there
  (that session ran on a kernel tried from RAM).

- The timedemo with the defaults, repeated (a SCPH-90006): textures 128 five
  runs 62.4 to 62.6 fps, 0 or 1 pass overtaken of 9200; textures 256 three
  runs 55.7 to 56.1 fps, 0 to 2 overtaken; none left out.

- VU0 for the world's culling (2026-11-20, late): the boxes of the groups
  and the chunks against the frustum, the guard band and the lines between
  the strips, four planes at a time from the box's centre and extent
  (`box_vu0` in rp_world.c; the compiler's vectors are VU0's macro
  instructions, `-mvu0`). The boxes are read where they lie in the map's
  file (16-byte aligned), so no number goes between the FPU and VU0 but the
  signs at the end: what made the first trial slower. Timedemo four, three
  runs each way: 62.8 to 62.9 fps for 62.2 to 62.4 (1 %). `r_ps2debug
  536870912` does both and counts the boxes judged differently: 0 of
  635,592. The sampler does not see it (it counts VU0's micro programs).
  The profile does: the culling (`rp_world_cull` and `rp_strip_mask`)
  is 3.6 % of the EE's samples with VU0 and 5.7 % by the FPU (1604 and
  2540 of 44,000): a third less.

- Is the GS's idle time (its FIFO empty 55 % of the time) room for more?
  `r_ps2trilinear 2` (between two levels only while the passes are short)
  with the defaults, timedemo four: the governor found room in 68 of 1258
  frames (textures 256: 30), and 9 passes were overtaken where there were 0
  or 1. The idle time is the beam's: a pass waits for its strip, and where
  the passes are long the GS has nothing to spare. Trilinear stays off.

- Which units work at the same time (the same samples, counted together;
  textures 128 / 256, of the run's time): the EE alone, no pass on its way
  38 / 39 %; the EE and the GS, VU1 waiting at an XGKICK 36 / 33 %; the EE
  and VU1, the GS's FIFO empty 14 / 13 %; the EE and the GS, VU1 free (2D,
  textures) 4 / 4 %; all three 3 / 3 %; VU1 or the GS with the EE asleep 3
  / 4 %; nothing 2 / 3 %. So the EE works through 95 % of the passes' time,
  but VU1 and the GS take turns: within a pass the GS is busy 73 % of the
  time and VU1 computes 30 %, both at once 6 %. The world is few vertices
  and large faces (5,300 vertices a frame over the strips), where VU1 is
  done at once and waits for the GS; the models are many vertices and
  small triangles (14,900, 65 % of what VU1 takes), where VU1 computes and
  the GS has next to nothing. Measured at 1920 wide (`r_ps2width 1920`):
  with everything VU1 computes with the GS's FIFO empty 14.0 % of the time
  and waits for the GS 48.3 %; without the models (`r_ps2draw 5`) 4.0 and
  45.9 %; without the world (`r_ps2draw 3`) 10.0 and 15.3 % (the game's
  quads and the view weapon fill too). So what there is to gain where the
  models' vertices could be computed beside the fill is 10 of a pass's 71
  % of the time. VU1 can be one packet ahead of the GS and
  no more (a second XGKICK waits for the first to have left), so a model's
  vertices beside a wall's fill would need walls and models in turns batch
  by batch, and a change of program and texture costs VIF1 and VU1 14 us
  for a model batch's 10 us of computing. Tried run by run (a switch, taken
  out again: the first scene's models that write depth, 22 runs a frame,
  each between two of the world's runs, not after them all; 42 waits
  between the programs' families a frame for 10), two runs each way at
  1920 wide: a field's passes 7.35 ms for 7.35, VU1 computing with the
  GS's FIFO empty 13.7 % for 13.7, both at once 3.6 % for 3.4. The order
  changes nothing: a run's set-up kicks its registers, and that XGKICK
  waits for the packet before it.
- VU0 runs no micro program and has none to run here: all work a vertex is
  VU1's, and what the EE has left of vector arithmetic is work an object
  (the culling, the polygons, matrices, the models' light: a good tenth of
  its time), which VU0 does as macro instructions inside the EE's code.
  Nor can it take VU1's work (the models, where the GS waits for VU1):
  what VU0 computes leaves it through the EE alone, the unit that is full
  (a frame's models are some 700 KB of GS packets; by reckoning, not
  measured).
- **A build's frame rate moves by up to 5 % with where its code and data
  come to lie.** The same source with a few lines more or less: 64.0, 63.3,
  62.5, 61.9 and 60.7 fps (each the same in every run). So two builds are
  only compared where the difference is far larger; everything below is
  measured in one build, by a switch at run time, the two ways in turns.
- The game's polygons once to the eye's coordinates: `rp_poly_area` (the
  pixels a quad fills, for the frame's limit) leaves each vertex's place
  from the eye, and every stage's draw takes it from there (`rp_poly_cam`;
  a mark's step towards the eye is a factor on it), where `rp_poly` worked
  it out again a stage; the view's numbers for the tests once a view. And
  by VU0, four vertices at a time (the compiler's vectors, the numbers in
  and out through memory). One build, three runs each: 16.46 ms a frame
  with VU0, 16.57 by the FPU (`r_ps2debug 8388608`), 16.64 by the FPU
  twice as before (`8396800`): 0.5 % for the once, 0.6 % more for VU0.
- The processor's counters (`ps2_perf 1`: at run time, the same program;
  a part of the frame each, `ps2_perf_region`), timedemo four, a frame of
  4.8 M cycles: 2.7 M instructions, 19,600 misses of the instruction cache,
  57,400 of the data cache, 840 TLB refills. By part (cycles, instructions,
  instruction and data cache misses):

  | | | | | |
  |---|---|---|---|---|
  | the game and the rest | 1,478,000 | 801,000 | 5,600 | 18,900 |
  | the models | 883,000 | 439,000 | 6,500 | 8,600 |
  | the strips' records (`rp_list_end`) | 512,000 | 429,000 | 700 | 4,200 |
  | the world's runs | 460,000 | 190,000 | 2,200 | 6,600 |
  | the pictures (421 a frame) | 404,000 | 199,000 | 800 | 5,700 |
  | the quads' draws | 313,000 | 178,000 | 1,700 | 2,700 |
  | the world's cull | 272,000 | 203,000 | 200 | 2,800 |
  | the quads' areas | 168,000 | 110,000 | 200 | 2,500 |
  | entities and polygons taken | 124,000 | 62,000 | 300 | 2,900 |
  | marks | 102,000 | 41,000 | 300 | 1,300 |

  The cycles over the instructions are about 25 a miss: two fifths of the
  EE's frame is waiting for its caches (16 KB of instructions, 8 KB of
  data).
- **cgame's calls straight into the engine.** cgame is linked into the
  program, but each of its calls went through `VM_DllSyscall` (sixteen
  arguments copied from the variable list) and the engine's switch: 421
  pictures, 180 polygons, 69 entities, the traces, the sounds' positions
  and 85 cvar updates a frame. The traps of the seventeen calls made most
  call the engine's functions (`ps2_cg_*` in cl_cgame.c; `ps2_nodirect 1`:
  through the system call, for comparing). One build, in turns: 15.55 and
  15.56 ms a frame for 16.59, 16.55 and 16.53: 6 %. What still goes
  through the switch is 5 calls a frame and less each (the run's report
  counts them). qagame's calls (the bots, a game played) are as they were.
- The hottest functions first in the fast text, next to each other (`make
  ORDER=<file>`: a function a section, `ld.lld --symbol-ordering-file`;
  the list from a profile's samples): 63.8 fps three times for 60.8 without
  it, of the same source; but another source's ordered build 62.5 for 63.3
  without. Within what a build's place does anyway: not the default.
- The models' runs between the world's, a trial that is out of the code
  again: see above (the passes as long, VU1 and the GS as little at once).
- Textures 256 (`r_ps2mode 1`) with the direct calls, the same build in
  turns: 56.6 and 56.8 fps (17.67 and 17.58 ms a frame) for 54.1 (18.46
  ms) through the system call: 4.6 %.
- What the models cost the EE (the timedemo with `r_ps2draw 5`, no models:
  82 fps for 64): their part of a frame is 831 thousand of 4.76 million
  cycles for 27 models drawn (35 surfaces, 159 pieces), spread evenly over
  `rp_model_draw` and what it calls: no one place to take it from. With
  them the EE also sleeps 153 thousand cycles a frame more, waiting for the
  passes, and the picture is at half width in 433 to 553 of the 1258 frames
  for 180 without.
- No program of VU0's own for the cull: the box test is 1.5 % of the EE's
  time, the walk over groups and chunks around it the rest. `memcpy` and
  `memset` are 549 and 593 calls a frame (101 and 86 KB): the cache's
  misses, not the calls.
- The screen's pictures (421 a frame, 7 % of the EE's time): the profile's
  hot instructions were the first loads of the renderer's own variables,
  each a cache miss after the game's code between two letters. What a
  picture reads lies in one cache line of the core's fast part (`h2d` in
  rp_main.c, `pc` in rp_poly.c), a picture is written straight into the
  frame's memory (no copy, no line fetched for it), a line of text takes
  its strips once, and the entry points leave out the counters' and the
  sampler's marks when no run measures (`ps2_inst`). One build, in turns
  (the old way behind a switch, taken out again): 15.12 and 15.17 ms a
  frame for 15.82 and 15.78: 4.3 %. The scene's counters in one line too
  (`scn`): within what a build's place does.
- An image placed in the frame before is taken as it was
  (`rp_image_place`'s first lines; maxsdk's `mx_vcache_entry` and
  `mx_vcache_again`: a use by the entry kept, without the lookup and
  without its places written again): 64 images a frame. Placing was 9 % of
  the EE's time. `r_ps2debug 512`: every image asked of the cache each
  frame. One build, in turns: 14.55 and 14.64 ms a frame for 15.24: 4.6 %.
  The timedemo with both: 68.7 and 68.3 fps in that build, 70.3 in the
  one without the old ways' code; textures 256 (`r_ps2mode 1`): 60.0 fps.
- The kernel's look-ups of residency (`mx_mem_resident`, some 800 cycles
  each): 165 a frame, 126 thousand cycles, most of them on a model's frame
  header and tags only to count waits that were read anyway. Now asked in
  a measured run only (`ps2_inst`), and the next animation frame's
  vertices once a frame of it: 2 a frame. Two builds differing in that
  alone, in turns: 68.8 -> 71.6 fps, textures 256 59.5 -> 61.4.
- Smaller: each strip's walk over the runs read a 380-byte run for its
  mask (a miss a run a strip): the masks in an array of their own, the
  runs' heads in the core's fast part (`rh`, `rmask` in rp_list.c). The
  world's runs of groups sorted by pass at load (a pass walks its own).
  `rp_image_place`'s globals in one line (`tp`), an image's fields it
  reads in its first line. cgame's traces skip a player's box that the
  swept box misses (collision about 40 thousand cycles a frame less).
- The placement of the code is worth more than most of these: the same
  source linked in four random orders of its hot functions gave 66.8 to
  73.3 fps; the EE's counters put it on the I-cache (16.0 against 19.6
  thousand misses a frame, the D-cache alike). Over those four orders the
  new source against the old: 70.5 for 68.8 fps on average. An order by
  the call graph (`ld.lld --call-graph-ordering-file`, cdsort, the graph
  from the code's jal and the profile's samples) gave 71.4 to 72.3 for
  both sources: a candidate for a fixed order, not measured enough yet.
- The whole: the timedemo 73.1 fps, textures 256 62.6 fps (kernel 46,
  without the VM's scan at each vblank: 2 % of a frame).
- TLB refills, 751 a frame (about 196 cycles each), down to 334 by taking
  the data a frame reads out of paged memory: the core's fast part and
  pinned memory have large entries of their own (`Q3_VM_HIST` for which
  pages). cgame's data and zeroed data whole (1.5 MB: cg, cgs,
  cg_entities, its cvars; `CGAME_FAST` in the Makefile renames the
  module's sections, its fresh data at each load works on them there):
  190 a frame less. The collision data (brushes, planes, nodes, leafs,
  patches; not the entities' text and the PVS) from a pinned heap instead
  of the hunk (`CM_PS2_Alloc` in cm_load.c, reset with the map): 80 less.
  The cvars, the looping sounds, the renderer's draws, mark samples, the
  quads' eye coordinates, the sky's points and some smaller ones
  (`MX_FAST_BSS`, 440 KB): 85 less. 64 frames more out of the pool, and
  fewer swap ins (101 -> 70 in the timedemo): what is read every frame was
  resident anyway. Two builds in turns: 73.5 -> 76.0 fps, textures 256
  62.3 -> 64.0. The demo played twice in one run (the module and the
  collision data loaded again): 76.7 and 77.9 fps by the engine's count.
  What is left is spread over some 270 pages of 5 to 10 refills a frame
  each (the renderer's run states and images, the scene's arrays, the
  texture cache's tables in libmxk).
- The world's draws are chains in the map's locked VM: each CALL of one
  asked the kernel for its physical address (`mx_paddr_of` of VM: some 700
  cycles), every draw again in every strip, some 170 thousand cycles a
  frame (the profile's piece_of, va_page, mx_vm_first_piece, in_vm). A
  locked page stays at its frame: each chunk's address is taken once at
  the map's load (`chunk_pa` in rp_world.c), the draws keep addresses
  (`rp_draw_pa`, `calls_pa` in rp_list.c). Two builds in turns: 75.8 ->
  81.6 fps, textures 256 63.9 -> 68.4; the picture of the timedemo's frame
  600 the same, pixel for pixel.
- What the EE writes for DMA does not go through the D-cache already
  (`rp_dyn` is uncached accelerated, the records are built in the
  scratchpad); what is left cached is small (the polygons' batch before
  `rp_dyn`, a model frame's block, the sound stream's stage). Reading
  through the UCAB does not pay here: any store empties it.

## 3. Why the earlier attempts stalled

| Attempt | Result | Why |
|---|---|---|
| Micro-optimising libgl's draw (`set_state`, `draw_tris_`, `-Os`, prefetch, deferred state) | ±1-4 % | the cost is the number of draws times cold code, not one function |
| Display lists called together | 16.5 -> 17.7 fps | fewer state changes, but every surface still goes through the front end, the sort and `glCallList` |
| Models on VU1 | 19.9 -> 21.5 | one kind of draw of many |
| Fast settings | 50.6 fps SD | fewer draws: the proof that draws are the cost, at the price of the quality |
| Native VU1 world beside libgl (branch ps2-native, removed) | 24.5-27.1 against 27.5 | the front end stayed; only 50-100 surfaces a frame took the new path (dlit ones fell out); state per group came from libgl; three kinds of stream mixed in one record (depth mismatches at EQUAL, record overflows read as drawing bugs); time moved, the frame did not shrink |
| VU1 clipping in libgl, FLUSHE for FLUSH, strip sorting, sprite fill limit | none | VU1 and the GS were not the limit of the frame rate |

What follows for this plan:

1. One renderer owns the whole stream. No libgl in the program, no mixed paths.
2. The front end goes too: nothing a surface, nothing a vertex on the EE.
3. A dynamic light, fog or a moving texture must not move a surface to a slow
   path: they are variants of the same VU1 programs.
4. A pass must be cheap by construction (no FLUSH, large batches, only what the
   strip shows), not by tuning.
5. The game's own cost (the frame without world and entities is 17.6 ms, about
   9 ms of it work) is measured alone, first: it decides how much is left.

## 4. Budgets

**The EE, a field of 4.92 M cycles** (1080i50: 5.90 M, 20 % more room):

| Part | Now (1080i HQ) | Target |
|---|---|---|
| Renderer front end and back end, libgl | about 5.9 M | 1.0 M (5.3) |
| Game, cgame, client, sound, collision | about 2.0-2.8 M | 1.5 M (phase 0 measures it) |
| Kernel: swap waits, TLB refills, interrupts | about 2.0 M | 0.4 M (no swap in play, hot code and data out of VM) |
| Margin | | 1.0 M and up |

**A pass.** Three strips: 5.5 ms each (1.62 M cycles), now filled by the whole
record every time. Per pass, estimated from maxsdk's measured rates for a peak
frame (12,900 vertex-passes, 9,900 triangles, 5.5 pixel writes a pixel):

| | Now | Target |
|---|---|---|
| VIF and VU1 | up to 9 ms | 13,000 x 30 cycles = 0.39 M = 1.3 ms unbinned; about half with strips binned |
| GS triangle set-up | in the above | 9,900 x 8 = 0.08 M = 0.3 ms |
| Fill, a strip of 369 k pixels | 2-2.5 ms average | 369 k x 5.5 x 0.42 = 0.85 M = 2.9 ms at today's overdraw; less front to back |

About 4.5 ms of 5.5 at the peak without binning, about 3.5 with. The 30 cycles
a vertex-pass is twice the measured kernels, as margin for small batches. Not
measured yet: fill with T8 textures and mipmaps, and what a pixel that fails
the depth test costs (phase 1 probes both).

**Memory.** Nothing leaves memory in play: the resident set during a map is
at most about 24 MB (5.7).

## 5. Design

### 5.1 The cut

`code/renderer_ps2/`: a new implementation of ioquake3's `refexport_t` (the
about 35 calls between the engine and its renderer: register a shader, a model, a
skin; load the world; add entities, polys and lights to a scene; render it;
2D pictures). It draws on maxsdk's frame stream (`<mx/frame.h>`,
`<mx/hires.h>`, `<mx/vcache.h>`) with hand-written VSM. renderergl1 and libgl
are not linked (`make REN=ps2`; `REN=gl1`, the default, is the libgl build).
The shader script parser and the `RB_Calc*` deform and
texture-coordinate functions are taken over as sources for the fallback (5.4).

Everything else stays ioquake3: game, cgame, ui, server, botlib, collision,
files, sound (`snd_ps2.c`). The libgl build stays buildable as the reference
until this one has passed it.

### 5.2 Data made on the PC

`tools/ps2map.py` beside `ps2pak.py` and `ps2snd.py`; `make pak` runs it for
every map into `ps2map.pk3`. The files are stored uncompressed and 16-byte
aligned, mapped read-only (`mx_vm_map_file`) and locked while the map is
loaded, so the DMAC reads them where they are: no copy in the hunk, no swap.

A map (`maps/<name>.pmap`):

- **Materials**: each shader as a list of passes (5.4), its textures by name.
- **Chunks**: the surfaces of one material and lightmap page, split by place
  into pieces of up to about 200 vertices, as triangle strips, ready as VIF
  packets: position V3-16 with the chunk's scale and offset in its header (ROW
  offset mode), texture ST and lightmap ST V2-16, colour V4-8. A chunk's
  header: bounds, the cone of its normals, its material, its vertex count, the
  fog volume, its DMA chain. A curve (patch) is a chunk with two or three
  levels of detail.
- **Visibility**: for each PVS cluster the list of chunks it can see, roughly
  front to back from the cluster, with the area (door) each belongs to.
- **Lightmaps** as brightness (T8) and, per vertex, the hue of the lightmap
  around it, fitted from the RGB lightmap; a surface is cut finer where the
  hue changes fast (coloured lights).
- **Sky**: the dome or box as chunks.
- The collision lumps stay in the BSP, which `cm_load.c` reads as before.

Models (`.md3`) keep their vertices as stored (shorts and the normal's two
angles: VU1 reads them already); the tool adds their strips. Textures are
`ps2tex.pk3`'s T8 and T4, as the file `tools/ps2bin.py` makes of them. A map without a `.pmap` is refused with a message
(for now the tool is the only way in; converting on the PS2 can come once
the renderer has proved itself).

### 5.3 A frame on the EE

1. The camera's cluster (one walk down the BSP), its chunk list.
2. For each chunk: door open, bounds against the frustum (VU0 macro code, the
   headers one after the other in memory, prefetched). The result is out,
   inside, or across the near plane or the guard band; the cone says back
   facing (out), mixed (cull variant) or front facing.
3. For each chunk kept: the strips its bounds touch on the screen; a CALL of
   its chain into those strips' lists, with its material's registers when the
   material changes. A patch picks its level by distance.
4. Entities: sphere against the frustum, the light grid at its origin, the
   matrix; a CALL of the model's strips with the two frames' addresses and the
   blend in the uniforms.
5. cgame's polys, sprites, beams and marks: copied as floats into the record
   (a dynamic batch).
6. 2D: sprites built in the scratchpad, sent as they are (PATH2).

No sort (the chunk lists are in order; blended passes go in a second list by
the shader's sort value), no tess arrays, no per-surface test, no vertex
touched except in 5. About 1,000 bounds tests, 300-600 CALLs, 60 materials
and 40 entities a frame.

### 5.4 The stream and the VU1 programs

One format for everything VU1 draws: a header (program entry, uniforms),
vertices by UNPACK, MSCAL once and MSCNT after, the GS registers of the
material as A+D in the packet VU1 kicks (PATH1 keeps them in order: no FLUSH
anywhere in a frame). All programs stay in micro memory, loaded at the start
of every record (a record is replayed: what an earlier one loaded says
nothing).

| Program | For |
|---|---|
| `w2` | the lightmapped world: a vertex read and transformed once, written twice: the texture in GS context 1 with depth writes, the lightmap in context 2 (beam racing's no-Z context) as `Cd * As` at the same XYZ, so depth always matches |
| `w1` | one-pass surfaces and every further stage: a texture matrix (all affine `tcMod`s as one 2 x 3 a frame), `col * cmul + cadd` (`rgbGen`/`alphaGen` wave, const, entity, vertex), `tcGen environment`; entry points with `tcMod turb`, with fog (the GS's own, `PRIM.FGE`, F from the fog plane: no fog pass), with dynamic lights |
| `m` | MD3: the two frames blended, the normal decoded, ambient and directed light, optional environment coordinates (today's `q3model.hlsl`, by hand) |
| `d` | dynamic batches: float vertices from the EE |
| clip variants of `w2`, `w1`, `m` | chunks and models across the near plane or the guard band: the loop leaves a triangle with a vertex outside out of the strip, and after the batch `q3clip` cuts those (the polygon as weights of the triangle's corners, a quadword a vertex; each piece a packet of its own). Chunks inside take the fast entries, which test nothing |
| cull variants | chunks whose normals face both ways: back faces get ADC |

Dynamic lights do not make a surface dynamic: the chunks a light's sphere
touches are drawn with the light entry of their program (up to four lights in
the uniforms, added per vertex into the pass's colour). No extra pass, no fill.
The map tool keeps world triangles small enough for that (5.2).

Coverage: a stage no program draws (`deformVertexes` other than autosprite,
unusual `tcGen`) is built on the EE with ioquake3's `RB_Calc*` and sent as a
dynamic batch. So the picture is complete from the first day and stages move
to VU1 in the order the counters show them. A counter on screen and in the
log names every fallback.

### 5.5 Beam racing (maxsdk, `<mx/hires.h>`)

Changes in maxsdk, each with its example and test, on master before Quake uses
them:

- **A record per strip.** A pass CALLs its strip's list, which CALLs chunks
  (two levels, what the DMAC has). VU1 and the GS then see a strip's chunks,
  not the frame three times.
- **More, lower strips with passes that may start early.** Pass k may run from
  the moment the beam has left strip k + 1 until it enters strip k. With three
  strips those windows touch end to end (5.5 ms each, a slow strip is late even
  when the field as a whole had time); with five strips of 128 lines or nine of
  64 each pass has a window of 3/5 or 7/9 of a field, and a heavy strip borrows
  from light ones. The passes are queued and each starts when the one before
  is done and its window is open. Nine strips of 64 lines take the buffer of
  today (576 lines); five of 128 take 640 lines, 250 KB of texture room.
- **Dynamic resolution, for every game**: a width per record, 1920 or 960
  shown at twice the width (same buffer, same FBW, the left half drawn; the
  display registers are the kernel's, so the switch at the field's start is
  maxsdk's and mx-kernel's, not the game's). A game says which widths it
  allows and maxsdk picks by the passes' measured times, or the game picks
  itself (the fill governor's last step). libgl's games get it by a flag.
- The stats per strip (time, late) readable each frame.

The fill governor in the renderer: it adds up the screen area of the blended
sprites and polys it is about to send (it has their corners) and, over a
limit, first drops the oldest smoke and trail puffs' layers, then draws
that record 960 wide. Dynamic lights cost no fill (5.4); the world's chunks
come front to back.

### 5.6 Quality

| | This plan | Not |
|---|---|---|
| Lighting | brightness lightmap times a per-vertex hue from the RGB lightmap, overbright by the GS's 2x | RGB lightmaps as three passes: the fill does not fit a strip |
| Dynamic lights | per vertex on VU1, coloured | the projected light texture as an extra blended pass |
| Fog | the GS's per-vertex fog | a fog pass |
| Textures | 128 x 128 T8 with mipmaps, bilinear; 1.9 MiB of GS memory in 1080i | 256 x 256: does not fit beside a 1920-wide buffer |
| Output | 1920 x 1080i, 16-bit with the dither matrix changed each field | 24-bit: does not fit in 1080i |
| Curves, marks, shadows, sky layers, all stages | on | |

Portals and mirrors render their view first, as a second scene in the same
record (phase 4).

### 5.7 Memory and the VM

- The renderer's code, its tables and the stream are core or pinned: reached
  through the one large TLB entry, never a refill, never paged.
- The map and models are mapped files, locked: about 1-2 MB a map, not in the
  hunk and not in swap. The hunk loses renderergl1's world and libgl's lists
  (2 MiB pool), beam racing's 512 KB records become about 128 KB (CALLs,
  uniforms and dynamic batches), the arenas go.
- `MX_VM_TEXT=core` for the game's code once the above has freed its 2.6 MB
  (26 % of the refills are code).
- `s_rawsamples` (16 MB of bss) and the mixer are left out of SPU builds.
- Gate: `tools/vmsim.py` on a trace of a demo: no swap in or out between the
  first and last frame.
- For the MXOS side (asked there, not worked around here): larger TLB pages
  where a region allows, the VM fault log.

### 5.8 The game side

- Phase 0's stub renderer gives the cost of everything that is not drawing.
  Its profile sets this list; known already: the HUD's models (each a scene of
  its own: lagometer and 3D icons off was 39 -> 46 fps in SD), blob shadows
  and marks (`CM_MarkFragments` every frame a player), sound loads.
- Small scenes (`RDF_NOWORLDMODEL`) are a matrix and a CALL here.
- Server and bots (phase 6): `sv_fps 20` puts a whole server frame in one field
  of three. Measured first; then the work is spread at its own boundaries (the
  bots' thinking by turns over the fields, the entity loop in the next), not
  by preempting it: collision is not reentrant.

## 6. Phases

Each phase ends with a run on the PS2 (1080i beam racing, host swap until
phase 5) that ends by itself and prints its numbers; a phase is done when its
gate is met, and a missed gate is reported with what was measured.

| Phase | Builds | Gate |
|---|---|---|
| 0. Ground | The run ends by itself (`Q3_AUTOQUIT`, the beam report and the cut-off counter back); reference pictures of fixed demo frames from the libgl build; `REN=ps2` as a `refexport_t` that registers and draws nothing, with its profile and counters; a VM trace of the demo; the demo's camera per frame dumped to a file | The game side's cycles a frame are known. Above 2.5 M: phase 5b moves before phase 3 |
| 1. The world | `tools/ps2map.py`; `w2`, `w1`, their clip and cull variants in VSM; `q3view`, an MXOS program that flies the demo's camera through the map with 5.3's front end; probes: fill with T8 and mipmaps, depth-fail pixels | World only, HQ, 1080i beam: EE at most 0.4 M cycles a frame; no late pass; longest pass at most 3.5 ms; pictures match the reference's world |
| 2. Beam racing | 5.5 in maxsdk, tried in `q3view` with 3, 5 and 9 strips | The strip count chosen by measurement; the longest pass at most 60 % of its window |
| 3. The game | `code/renderer_ps2`: registration, materials from shader scripts, textures on `vcache`, `m`, `d`, the fallback, 2D, small scenes | `demo four` complete against the reference pictures; EE at most 3.9 M cycles in 95 % of the frames; timedemo above 60 |
| 4. Effects and quality | Dynamic lights, fog, sky layers, marks, shadows, portals; the fill governor | No late passes over the demo; the frames 960 wide under 2 % |
| 5. Memory | 5.7 | No swap in play; TLB refills under 800 a frame |
| 5b. Game side | 5.8's list from the stub's profile | Game side at most 1.5 M cycles |
| 6. Play | Bots and the server spread over fields; menus; every map of the game loaded and flown through automatically; load times | A bot match holds 60 with at most 1 % of the fields repeated |
| 7. Back to the libraries | What proved itself as general: the stream format and binning for libgl, the programs as HLSL when the compiler can express them | |

Phases 0 and 1 decide the rest: after them the two unknowns (what the game
itself costs, what a pass costs when built right) are numbers.

## 7. Risks

| Risk | Sign | Answer |
|---|---|---|
| Fill in effect-heavy frames (up to 24 M blended pixels now) | late passes in phase 4 | governor; 960-wide records; smoke point sampled |
| The game side is above 2.5 M cycles | phase 0 | 5b first: the stub's profile, a link order by heat (+2 % measured before), code in core; if still too much, 1080i50 (5.9 M a field) |
| Bots | phase 6's first measurement | spread over fields; fewer bots thinking a field |
| Depth-fail pixels cost as much as drawn ones | phase 1's probe | front-to-back order does not help: a depth-only first pass of the world (flat, 0.16 a pixel) |
| VU1 clipper glitches | pictures against the reference | only chunks across a plane use it; the reference pictures name the frame |
| 1.9 MiB of textures in 1080i | texture quality steps down | T4 where 16 colours are exact; fewer mip levels far away (as libgl's quality control) |
| Third-party maps | a map without `.pmap` | the tool; later a conversion on the PS2 |

## 8. Rules for the work

- Check the record's size and the cut-off counter before reading any picture.
- Only committed library builds; a maxsdk change lands with its test first.
- Runs end by themselves and print one summary; settings in a cfg.
- On the PS2 with host swap until phase 5 has removed the swap.
- Pictures are compared by program against the reference frames, not by eye on
  the TV; the TV is for motion and flicker at a phase's end.
- A fallback is counted and named on screen: nothing is slow in silence.
