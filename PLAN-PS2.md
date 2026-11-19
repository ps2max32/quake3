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
