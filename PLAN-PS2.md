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
`ps2tex.pk3`'s T8 as now. A map without a `.pmap` is refused with a message
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
| clip variants of `w2`, `w1`, `m` | chunks and models across the near plane or the guard band: the three-pass clipper of libgl's `mxglc.i`. Chunks inside take the fast entries, which test nothing |
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
