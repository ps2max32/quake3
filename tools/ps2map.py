#!/usr/bin/env python3
"""A Quake 3 map as the PS2 renderer draws it (PLAN-PS2.md, 5.2).

    ps2map.py OUT.pmap BASEQ3_DIR MAP [--tex ps2tex.pk3] [--stats]

Reads maps/MAP.bsp and the shader scripts from BASEQ3_DIR's pak*.pk3 (a later
pak's file replaces an earlier one's) and the textures' sizes from
ps2tex.pk3 (tools/ps2pak.py), and writes OUT.pmap: the map's drawn surfaces
as chunks VU1 takes as they are. The textures themselves are the renderer's
file of images (tools/ps2bin.py), found there by name. The file is mapped and locked on the PS2; the DMAC
reads the chunks where they lie, so everything it reads is 16-byte aligned.

What is made of the BSP:
- Curves (patches) are tessellated (a chord at most --curve units off the
  curve). Edges longer than --edge units are halved (the same on both sides
  of an edge, so no cracks): dynamic lights are per vertex, and a triangle
  stays inside the guard band longer. Triangles whose texture repeats more
  than 3.5 times are halved too: texture coordinates are 4.12 fixed point.
- Surfaces of one shader, lightmap page and fog are gathered and cut by
  place into groups of about --chunk strip vertices within --extent units:
  a group is in a view or not by its bounds and by the PVS clusters it is
  seen from. A group's triangles of one direction (a flat surface's normal;
  of a curve's triangles the axis they face most) are a chunk, drawn or left
  out as a whole: left out when all of it faces away, by the cone of its
  normals (the GS draws both sides of a triangle).
- A chunk's triangles become strips, cut into batches of at most BATCH
  vertices: what one VU1 buffer holds.
- A lightmapped vertex's colour is the hue of the lightmap at it (its
  largest channel made 1.0); the lightmap pages keep the brightness (T8).
  Other vertices keep the BSP's vertex light.

File layout (little-endian; "off" are bytes from the file's start):

  head (128 bytes):
    u32 magic "PMAP", version 9 (5: a texture with alpha of its own had no lightmap; 6: no cvis; 7: no areas;
      8: lightmap pages of 128 x 128 all; 9: no chunks of quads that turn to the eye)
    u32 nmat, ntex, nchunk, nmodel, nlightmap, ncluster, vis_bytes
    u32 nplane, nnode, nleaf
    u32 off_mat, off_tex, off_chunk, off_model, off_lightmap, off_vis,
        off_plane, off_node, off_leaf, off_data, size
    f32 pos_scale          world units a position unit
    u32 batch              BATCH
    u32 ngroup, off_group
    u32 off_cvis, cvis_bytes
    u32 off_lmdim
  material (96 bytes): char name[64]; s16 tex (-1: none); u16 flags (MAT_*);
    u8 blend (BLEND_*), alpha (ALPHA_*), cull (CULL_*), pad; u32 surf, cont
    (the BSP's flags); f32 texels (of its texture's first level a world
    unit, for the mipmap level); pad to 96
  texture (72 bytes): char name[64] (the image's in the file of images);
    u16 w, h; u8 alpha (its palette has alpha of its own), pad[3]
  chunk (48 bytes): f32 mins[3]; u16 mat; s16 lightmap (-1: none);
    f32 maxs[3]; u16 nverts; u8 flags (CH_*), fog; u32 off_data; u16 qwc
    (quadwords the DMAC sends for it, tags included), nbatch; s8 cone[3] (the
    axis of its triangles' normals, length 127); u8 sine (of the widest
    angle between a normal and the axis, in 1/255; 255: no cone); u8 area
    (of the world's chunks: the area of the leafs it lies in, the room between
    doors; 255: in more than one, or none), pad[3]
  group (32 bytes): f32 mins[3]; u32 first_chunk; f32 maxs[3]; u32 nchunk (its
    chunks are of one material and lightmap page)
  model (32 bytes): f32 mins[3], maxs[3]; u32 first_group, ngroup (model 0 is
    the world; the others the map's doors and platforms)
  lmdim: nlightmap pages: u16 w, h (64 x 64, or 128 x 128 for a surface whose
    lightmap is larger than 64); u32 off (its texels in lightmap)
  lightmap: w x h bytes a page, T8: 128 + b, b the brightness (the
    largest of R, G, B; 127 and more: 127, twice the texture's colour, the
    map's overbright range). It is drawn with the CLUT of the surface's
    texture, whose entries 128 .. 255 have that brightness as their alpha
    (tools/ps2bin.py): the GS has one CLUT buffer, and two CLUTs by turns
    would be loaded at every batch
  vis: ncluster rows of vis_bytes: bit g of row k set: the world's group g is
    seen from cluster k
  cvis: ncluster rows of cvis_bytes: bit c of row k set: chunk c is seen from
    cluster k (a group is seen when one of its chunks is: a view tests the
    group first, then its chunks)
  plane (16 bytes): f32 normal[3], dist;  node (12 bytes): s32 plane,
    child[2] (negative: -(leaf + 1));  leaf (4 bytes): s16 cluster, area

A chunk's data is a DMA chain a frame CALLs: a CNT tag, the VIF codes and
data of its batches, a RET tag. A batch (n vertices; the addresses are in a
buffer of VU1's double buffering, VU_* below, shared with the programs):

    STCYCL cl 3 wl 1
    UNPACK V3-16  n -> VU_A + 0 + 3 i   texture s, t (4.12) and 4096
    UNPACK V4-8   n -> VU_A + 1 + 3 i   colour (unsigned)
    UNPACK V4-16  n -> VU_A + 2 + 3 i   position, and w: 0xA000 the vertex
                                        ends no triangle (ADC, and bit 13 for
                                        the programs that set ADC themselves),
                                        0x4000 its triangle is wound the other
                                        way
    UNPACK V3-16  n -> VU_IB + 0 + 3 i  lightmap s, t and 4096 (CH_LIGHTMAP)
    ITOP n, MSCNT

Nothing waits between batches, so VU1 works while the GS draws: the batch
after this is unpacked into the buffer of the batch before while this
program runs, and the GIF may still be reading that batch's last packet (the
GS takes its time over a large triangle). So the VIF writes nothing where a
program's last packet lies: that one (VU_B: the lightmap pass, or the one
pass of a surface without) is written by the program alone. The texture
pass's, in place over what the VIF wrote, has left before its program ends
(the next XGKICK waits for it).
"""
import argparse
import math
import os
import struct
import sys
import zipfile

import numpy as np

# ---- shared with the VU1 programs (code/renderer_ps2) ---------------------------
BATCH = 46      # vertices a batch at most
VU_A = 18       # the texture pass's first vertex in a buffer (after the pad and its GIF tag)
VU_B = 175      # the lightmap pass's packet: written by the programs alone
VU_IB = 331     # the lightmap's texture coordinates
W_NONE = 0xA000  # a position's w: the vertex ends no triangle (bit 15 is the GS's ADC; bit 13 says
                 # it to the programs that cut, which set ADC themselves)
W_REV = 0x4000   # its triangle is wound the other way

MAT_LIGHTMAP, MAT_SKY, MAT_NODRAW, MAT_DEFORM, MAT_TCMOD, MAT_SCRIPT, MAT_VERTEX, MAT_ENV = (
    1, 2, 4, 8, 16, 32, 64, 128)
# the script's first stage has rgbGen vertex and a $lightmap stage follows: the texture times
# the vertex's colour times the lightmap (banners, grates, wires)
MAT_VLM = 256
BLEND_NONE, BLEND_ADD, BLEND_ALPHA, BLEND_FILTER, BLEND_OTHER = range(5)
ALPHA_NONE, ALPHA_GT0, ALPHA_LT128, ALPHA_GE128 = range(4)
CULL_FRONT, CULL_BACK, CULL_NONE = range(3)
CH_LIGHTMAP = 1
CH_SPRITE = 2  # no vertices: off_data is qwc records of quads that turn to the eye (64 bytes each)
SURF_SKY, SURF_NODRAW = 0x4, 0x80
MST_PLANAR, MST_PATCH, MST_TRIANGLES, MST_FLARE = 1, 2, 3, 4

VIF_STCYCL, VIF_ITOP, VIF_FLUSH, VIF_MSCNT = 0x01, 0x04, 0x11, 0x17
UNPACK_V3_16, UNPACK_V4_16, UNPACK_V4_8 = 0x69, 0x6D, 0x6E
DMA_CNT, DMA_RET = 1, 6


def vif(cmd, num=0, imm=0):
    return struct.pack("<I", cmd << 24 | (num & 255) << 16 | imm & 0xFFFF)


# ---- the paks ---------------------------------------------------------------------

class Paks:
    def __init__(self, basedir):
        self.files = {}
        names = sorted(n for n in os.listdir(basedir) if n.lower().startswith("pak") and n.lower().endswith(".pk3"))
        for n in names:
            z = zipfile.ZipFile(os.path.join(basedir, n))
            for i in z.infolist():
                self.files[i.filename.lower()] = (z, i)

    def read(self, name):
        e = self.files.get(name.lower())
        return e[0].read(e[1]) if e else None

    def names(self, prefix, suffix):
        return sorted(n for n in self.files if n.startswith(prefix) and n.endswith(suffix))


# ---- shader scripts: what the map tool needs of them ---------------------------------

def tokens(text):
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            if c == "\n":
                out.append("\n")
            i += 1
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c in "{}":
            out.append(c)
            i += 1
        elif c == '"':
            j = text.find('"', i + 1)
            j = n if j < 0 else j
            out.append(text[i + 1:j])
            i = j + 1
        else:
            j = i
            while j < n and text[j] not in " \t\r\n{}":
                j += 1
            out.append(text[i:j])
            i = j
    return out


def parse_scripts(paks):
    """name -> {'lines': [[tok..]], 'stages': [[[tok..]]]}"""
    shaders = {}
    for fn in paks.names("scripts/", ".shader"):
        t = tokens(paks.read(fn).decode("latin-1"))
        i, n = 0, len(t)
        while i < n:
            while i < n and t[i] == "\n":
                i += 1
            if i >= n:
                break
            name = t[i].lower().replace("\\", "/")
            i += 1
            while i < n and t[i] == "\n":
                i += 1
            if i >= n or t[i] != "{":
                continue
            i += 1
            sh = {"lines": [], "stages": []}
            depth, line, stage = 1, [], None
            while i < n and depth:
                k = t[i]
                i += 1
                if k == "{":
                    depth += 1
                    stage = []
                    line = []
                elif k == "}":
                    if line:
                        (stage if stage is not None and depth == 2 else sh["lines"]).append(line)
                        line = []
                    if depth == 2 and stage is not None:
                        sh["stages"].append(stage)
                        stage = None
                    depth -= 1
                elif k == "\n":
                    if line:
                        (stage if stage is not None and depth == 2 else sh["lines"]).append(line)
                        line = []
                else:
                    line.append(k)
            shaders.setdefault(name, sh)  # the first definition is the one used, as the engine
    return shaders


def blend_of(args):
    a = [x.lower() for x in args]
    if a[:1] == ["add"] or a[:2] == ["gl_one", "gl_one"]:
        return BLEND_ADD
    if a[:1] == ["blend"] or a[:2] == ["gl_src_alpha", "gl_one_minus_src_alpha"]:
        return BLEND_ALPHA
    if a[:1] == ["filter"] or a[:2] in (["gl_dst_color", "gl_zero"], ["gl_zero", "gl_src_color"]):
        return BLEND_FILTER
    return BLEND_OTHER


def material_of(name, script, lightmapped):
    """(texture name or None, flags, blend, alpha, cull)"""
    flags, blend, alpha, cull, tex = 0, BLEND_NONE, ALPHA_NONE, CULL_FRONT, None
    if script is None:
        return name, (MAT_LIGHTMAP if lightmapped else MAT_VERTEX), blend, alpha, cull
    flags |= MAT_SCRIPT
    for l in script["lines"]:
        k = l[0].lower()
        if k == "cull" and len(l) > 1:
            v = l[1].lower()
            cull = CULL_NONE if v in ("none", "twosided", "disable") else CULL_BACK if v in ("back", "backside", "backsided") else CULL_FRONT
        elif k == "deformvertexes":
            flags |= MAT_DEFORM
        elif k == "skyparms":
            flags |= MAT_SKY
    base = None
    for st in script["stages"]:
        m = {"blend": None, "map": None, "alpha": ALPHA_NONE, "vertex": False}
        for l in st:
            k = l[0].lower()
            if k in ("map", "clampmap") and len(l) > 1:
                m["map"] = l[1].lower().replace("\\", "/")
            elif k == "animmap" and len(l) > 2:
                m["map"] = l[2].lower().replace("\\", "/")
            elif k == "blendfunc":
                m["blend"] = blend_of(l[1:])
            elif k == "alphafunc" and len(l) > 1:
                m["alpha"] = {"gt0": ALPHA_GT0, "lt128": ALPHA_LT128, "ge128": ALPHA_GE128}.get(l[1].lower(), ALPHA_NONE)
            elif k == "tcmod":
                flags |= MAT_TCMOD
            elif k == "tcgen" and len(l) > 1 and l[1].lower() == "environment":
                m["env"] = True
            elif k == "rgbgen" and len(l) > 1 and l[1].lower() in ("vertex", "exactvertex"):
                m["vertex"] = True
        if m["map"] == "$lightmap":
            if lightmapped:
                flags |= MAT_LIGHTMAP
            continue
        if m["map"] in (None, "$whiteimage"):
            continue
        if base is None:
            base = m
    if base:
        tex = base["map"]
        alpha = base["alpha"]
        if base.get("env"):
            flags |= MAT_ENV
        if base["vertex"] and flags & MAT_LIGHTMAP:
            flags |= MAT_VLM
        if base["vertex"] or not flags & MAT_LIGHTMAP:
            flags |= MAT_VERTEX
        if base["blend"] is not None and not flags & MAT_LIGHTMAP:
            blend = base["blend"]
        elif base["blend"] is not None and flags & MAT_LIGHTMAP and base["blend"] != BLEND_FILTER:
            # a blended stage over a lightmap stage: drawn as texture times lightmap for now
            pass
    else:
        flags |= MAT_VERTEX
    return tex, flags, blend, alpha, cull


# ---- textures of ps2tex.pk3 as the GS takes them ---------------------------------------

GL_PALETTE4_RGB8, GL_PALETTE4_RGBA8, GL_PALETTE8_RGB8, GL_PALETTE8_RGBA8 = 0x8B90, 0x8B91, 0x8B95, 0x8B96


def load_p8(blob):
    """-> dict(w, h, four, alpha, pal (n x 4, alpha as stored), levels [uint8 arrays]) or None"""
    if blob is None or blob[:4] != b"P8T1":
        return None
    width, height, pw, ph, w0, h0, levels, _pad, fmt = struct.unpack_from("<8HI", blob, 4)
    four = fmt in (GL_PALETTE4_RGB8, GL_PALETTE4_RGBA8)
    has_a = fmt in (GL_PALETTE4_RGBA8, GL_PALETTE8_RGBA8)
    ncol, bpe = (16 if four else 256), (4 if has_a else 3)
    p = 24
    src = np.frombuffer(blob, np.uint8, ncol * bpe, p).reshape(ncol, bpe)
    p += ncol * bpe
    pal = np.full((ncol, 4), 255, np.uint8)
    pal[:, :bpe] = src
    lv = []
    w, h = w0, h0
    for _ in range(levels):
        nbytes = (w * h + 1) // 2 if four else w * h
        d = np.frombuffer(blob, np.uint8, nbytes, p)
        p += nbytes
        if w >= 8 and h >= 8 and len(lv) < 7:
            lv.append(d)
        w, h = max(w // 2, 1), max(h // 2, 1)
    if not lv:
        return None
    return dict(w=w0, h=h0, four=four, alpha=bool(has_a and (pal[:, 3] < 255).any()), pal=pal, levels=lv)


# ---- the BSP ------------------------------------------------------------------------------

LUMPS = ["entities", "shaders", "planes", "nodes", "leafs", "leafsurfaces", "leafbrushes", "models", "brushes",
         "brushsides", "drawverts", "drawindexes", "fogs", "surfaces", "lightmaps", "lightgrid", "visibility"]


class Bsp:
    def __init__(self, data):
        if data[:4] != b"IBSP" or struct.unpack_from("<i", data, 4)[0] != 46:
            raise SystemExit("not a Quake 3 BSP (version 46)")
        self.l = {}
        for i, n in enumerate(LUMPS):
            o, s = struct.unpack_from("<ii", data, 8 + 8 * i)
            self.l[n] = data[o:o + s]
        l = self.l
        self.shaders = [(l["shaders"][i:i + 64].split(b"\0")[0].decode("latin-1").lower().replace("\\", "/"),
                         *struct.unpack_from("<ii", l["shaders"], i + 64)) for i in range(0, len(l["shaders"]), 72)]
        self.planes = np.frombuffer(l["planes"], "<f4").reshape(-1, 4)
        self.nodes = np.frombuffer(l["nodes"], "<i4").reshape(-1, 9)
        self.leafs = np.frombuffer(l["leafs"], "<i4").reshape(-1, 12)
        self.leafsurfaces = np.frombuffer(l["leafsurfaces"], "<i4")
        self.models = [struct.unpack_from("<6f4i", l["models"], i) for i in range(0, len(l["models"]), 40)]
        dv = np.frombuffer(l["drawverts"], np.dtype([("xyz", "<f4", 3), ("st", "<f4", 2), ("lm", "<f4", 2),
                                                     ("n", "<f4", 3), ("c", "u1", 4)]))
        self.verts = dv
        self.indexes = np.frombuffer(l["drawindexes"], "<i4")
        self.surfaces = [struct.unpack_from("<12i", l["surfaces"], i) + struct.unpack_from("<2i", l["surfaces"], i + 96)
                         for i in range(0, len(l["surfaces"]), 104)]
        self.lightmaps = np.frombuffer(l["lightmaps"], np.uint8).reshape(-1, 128, 128, 3)
        v = l["visibility"]
        if len(v) >= 8:
            self.nclusters, self.cluster_bytes = struct.unpack_from("<ii", v, 0)
            self.vis = np.frombuffer(v, np.uint8, self.nclusters * self.cluster_bytes, 8).reshape(self.nclusters, -1)
        else:
            self.nclusters, self.cluster_bytes, self.vis = 0, 0, None


# ---- geometry -------------------------------------------------------------------------------

# a vertex here: 14 floats: xyz 0-2, st 3-4, lm 5-6, normal 7-9, rgba 10-13

def surface_vertices(bsp, s):
    first, num = s[3], s[4]
    dv = bsp.verts[first:first + num]
    return np.concatenate([dv["xyz"], dv["st"], dv["lm"], dv["n"], dv["c"].astype(np.float32)], axis=1).astype(np.float64)


def bezier_rows(ctrl, n):
    """ctrl: (2k + 1, ...) control points along axis 0; n segments a span -> (k n + 1, ...)"""
    k = (ctrl.shape[0] - 1) // 2
    out = []
    for i in range(k):
        p0, p1, p2 = ctrl[2 * i], ctrl[2 * i + 1], ctrl[2 * i + 2]
        for j in range(n):
            t = j / n
            out.append(p0 * (1 - t) ** 2 + p1 * 2 * t * (1 - t) + p2 * t * t)
    out.append(ctrl[-1])
    return np.array(out)


def segs_for(ctrl, tol):
    """segments a span so that the chords stay within tol of the curves along axis 0"""
    k = (ctrl.shape[0] - 1) // 2
    worst = 0.0
    for i in range(k):
        d = ctrl[2 * i, ..., :3] - 2 * ctrl[2 * i + 1, ..., :3] + ctrl[2 * i + 2, ..., :3]
        worst = max(worst, float(np.sqrt((d * d).sum(-1)).max()))
    n = 1
    while n < 16 and worst / (4.0 * n * n) > tol:
        n *= 2
    return n


def patch_triangles(bsp, s, tol):
    w, h = s[12], s[13]
    ctrl = surface_vertices(bsp, s).reshape(h, w, 14)
    nu = segs_for(ctrl.transpose(1, 0, 2), tol)   # along the width
    nv = segs_for(ctrl, tol)                      # along the height
    g = bezier_rows(ctrl, nv)                     # rows
    g = bezier_rows(g.transpose(1, 0, 2), nu).transpose(1, 0, 2)
    gh, gw = g.shape[0], g.shape[1]
    nrm = g[..., 7:10]
    ln = np.sqrt((nrm * nrm).sum(-1, keepdims=True))
    g[..., 7:10] = nrm / np.where(ln > 0, ln, 1)
    verts = g.reshape(-1, 14)
    tris = []
    for y in range(gh - 1):
        for x in range(gw - 1):
            a, b, c, d = y * gw + x, y * gw + x + 1, (y + 1) * gw + x, (y + 1) * gw + x + 1
            tris.append((a, c, b))
            tris.append((b, c, d))
    return verts, tris


def split_long(verts, tris, edge_max, st_max):
    """halve edges longer than edge_max (positions) or with more than st_max texture repeats"""
    verts = [np.array(v) for v in verts]
    mid = {}
    out = []
    stack = list(tris)
    while stack:
        t = stack.pop()
        best, be = 0.0, -1
        for e in range(3):
            a, b = verts[t[e]], verts[t[(e + 1) % 3]]
            d = a - b
            l = math.sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / edge_max
            l = max(l, abs(d[3]) / st_max, abs(d[4]) / st_max)
            if l > best:
                best, be = l, e
        if best <= 1.0 or len(verts) > 60000:
            out.append(t)
            continue
        i0, i1, i2 = t[be], t[(be + 1) % 3], t[(be + 2) % 3]
        key = (min(i0, i1), max(i0, i1))
        m = mid.get(key)
        if m is None:
            m = len(verts)
            verts.append((verts[i0] + verts[i1]) * 0.5)
            mid[key] = m
        stack.append((i0, m, i2))
        stack.append((m, i1, i2))
    return verts, out


def stripify(tris):
    """triangles (index triples) -> strips: lists of (index, reversed) with reversed for the
    triangle a vertex ends (None for a strip's first two)"""
    edge = {}
    for ti, t in enumerate(tris):
        for e in range(3):
            a, b = t[e], t[(e + 1) % 3]
            edge.setdefault((min(a, b), max(a, b)), []).append(ti)
    used = [False] * len(tris)

    def other(ti, a, b):
        for v in tris[ti]:
            if v != a and v != b:
                return v
        return None

    def nxt(a, b, cur):
        for tj in edge.get((min(a, b), max(a, b)), ()):
            if tj != cur and not used[tj]:
                return tj
        return -1

    def rev(a, b, c, ti):
        t = tris[ti]
        for e in range(3):
            if t[e] == a and t[(e + 1) % 3] == b and t[(e + 2) % 3] == c:
                return False
        return True

    def degree(ti):
        t = tris[ti]
        return sum(1 for e in range(3) if nxt(t[e], t[(e + 1) % 3], ti) >= 0)

    strips = []
    order = sorted(range(len(tris)), key=lambda i: i)
    for start in order:
        if used[start]:
            continue
        best = None
        t = tris[start]
        for e in range(3):
            a, b, c = t[e], t[(e + 1) % 3], t[(e + 2) % 3]
            # forward from (a, b, c)
            seq, tl = [a, b, c], [start]
            mark = {start}
            cur = start
            while True:
                x, y = seq[-2], seq[-1]
                tj = -1
                for cand in edge.get((min(x, y), max(x, y)), ()):
                    if cand != cur and not used[cand] and cand not in mark:
                        tj = cand
                        break
                if tj < 0:
                    break
                seq.append(other(tj, x, y))
                tl.append(tj)
                mark.add(tj)
                cur = tj
            if best is None or len(tl) > len(best[1]):
                best = (seq, tl)
        seq, tl = best
        for tj in tl:
            used[tj] = True
        strip = [(seq[0], None), (seq[1], None)]
        for k, tj in enumerate(tl):
            strip.append((seq[k + 2], rev(seq[k], seq[k + 1], seq[k + 2], tj)))
        strips.append(strip)
    return strips


def sample_lightmap(page, s, t):
    """bilinear RGB of a page (square) at s, t in 0..1"""
    side = page.shape[0]
    x, y = s * side - 0.5, t * side - 0.5
    x0, y0 = int(math.floor(x)), int(math.floor(y))
    fx, fy = x - x0, y - y0
    c = np.zeros(3)
    for dy, wy in ((0, 1 - fy), (1, fy)):
        for dx, wx in ((0, 1 - fx), (1, fx)):
            xx, yy = min(max(x0 + dx, 0), side - 1), min(max(y0 + dy, 0), side - 1)
            c += page[yy, xx].astype(np.float64) * (wx * wy)
    return c


def repack_lightmaps(bsp):
    """The surfaces' rectangles of lightmap packed again, in the order of their place in the
    map. q3map fills its pages in the BSP's order of surfaces, so the surfaces of one room
    lie on most of the map's pages and a view draws with most of them: 16 KB of GS memory
    each. Packed by place, a view's surfaces are on few pages."""
    verts = bsp.verts.copy()
    old = bsp.lightmaps
    rects = {}  # (page, x, y, w, h) -> [surfaces]
    for si, s in enumerate(bsp.surfaces):
        if s[7] < 0 or s[4] <= 0 or s[2] not in (MST_PLANAR, MST_PATCH):
            continue
        lm = verts["lm"][s[3]:s[3] + s[4]] * 128.0 - 0.5
        # the texels the bilinear filter reads for these coordinates
        x0, y0 = (int(v) for v in np.clip(np.floor(lm.min(0)), 0, 127))
        x1, y1 = (int(v) for v in np.clip(np.floor(lm.max(0)) + 1, 0, 127))
        rects.setdefault((s[7], x0, y0, x1 - x0 + 1, y1 - y0 + 1), []).append(si)
    if not rects:
        return 0, 0

    def key(r):  # the place of its first surface: a Z-order of cells of 256 units
        s = bsp.surfaces[rects[r][0]]
        c = verts["xyz"][s[3]:s[3] + s[4]].mean(0)
        q = [int((v + 65536) // 256) for v in c]
        z = 0
        for b in range(10):
            for a in range(3):
                z |= (q[a] >> b & 1) << (3 * b + a)
        return z

    order = sorted(rects, key=key)
    pages, place = [], {}

    def pack(order, side):
        i, carry = 0, []
        while i < len(order) or carry:
            batch, area = carry, sum(r[3] * r[4] for r in carry)
            carry = []
            while i < len(order) and area + order[i][3] * order[i][4] <= 0.97 * side * side:
                batch.append(order[i])
                area += order[i][3] * order[i][4]
                i += 1
            if not batch:
                batch.append(order[i])
                i += 1
            used = np.zeros((side, side), bool)
            page = np.zeros((side, side, 3), np.uint8)
            x = y = shelf = 0
            for r in sorted(batch, key=lambda r: -r[4]):
                w, h = r[3], r[4]
                if x + w > side:
                    x, y, shelf = 0, y + shelf, 0
                if y + h > side:
                    carry.append(r)
                    continue
                page[y:y + h, x:x + w] = old[r[0], r[2]:r[2] + h, r[1]:r[1] + w]
                used[y:y + h, x:x + w] = True
                place[r] = (len(pages), x, y, side)
                x, shelf = x + w, max(shelf, h)
            # the texels no surface has take a neighbour's: the filter reads a rectangle's
            # neighbours at its edge, as it read another surface's before
            for _ in range(2):
                for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
                    src_used = np.roll(used, (dy, dx), (0, 1))
                    src = np.roll(page, (dy, dx), (0, 1))
                    take = ~used & src_used
                    page[take] = src[take]
                    used = used | take
            pages.append(page)

    # pages of 64 x 64 (4 KB of GS memory each: a view takes what it draws with, in
    # small pieces); a rectangle larger than that in a page of 128 x 128
    pack([r for r in order if r[3] <= 64 and r[4] <= 64], 64)
    pack([r for r in order if r[3] > 64 or r[4] > 64], 128)
    done = set()
    surfaces = list(bsp.surfaces)
    for r, ss in rects.items():
        p, x, y, side = place[r]
        for si in ss:
            s = surfaces[si]
            if s[3] not in done:
                done.add(s[3])
                lm = verts["lm"][s[3]:s[3] + s[4]]
                verts["lm"][s[3]:s[3] + s[4]] = (lm * 128.0 + np.array([x - r[1], y - r[2]], np.float32)) / side
            surfaces[si] = s[:7] + (p, x, y, r[3], r[4]) + s[12:]
    was = len(old)
    bsp.verts, bsp.surfaces, bsp.lightmaps = verts, surfaces, pages
    return was, len(pages)


# ---- the map -----------------------------------------------------------------------------------

def build(args):
    paks = Paks(args.basedir)
    data = paks.read(f"maps/{args.map}.bsp")
    if data is None:
        raise SystemExit(f"maps/{args.map}.bsp is in no pak of {args.basedir}")
    bsp = Bsp(data)
    lm_was = len(bsp.lightmaps)
    if not args.keep_lightmaps and lm_was:
        repack_lightmaps(bsp)
    scripts = parse_scripts(paks)
    texpak = zipfile.ZipFile(args.tex) if args.tex and os.path.exists(args.tex) else None
    texnames = {i.filename.lower(): i for i in texpak.infolist()} if texpak else {}

    # which model a surface belongs to
    surf_model = {}
    for mi, m in enumerate(bsp.models):
        for si in range(m[6], m[6] + m[7]):
            surf_model[si] = mi
    # the clusters a surface is seen in
    surf_clusters = {}
    surf_areas = {}  # the areas (the rooms between doors) of the leafs a surface is in
    for lf in bsp.leafs:
        cl = int(lf[0])
        if cl < 0:
            continue
        for k in range(int(lf[8]), int(lf[8]) + int(lf[9])):
            surf_clusters.setdefault(int(bsp.leafsurfaces[k]), set()).add(cl)
            surf_areas.setdefault(int(bsp.leafsurfaces[k]), set()).add(int(lf[1]))

    # materials: a shader of the BSP with or without a lightmap
    mats, mat_index, textures, tex_index = [], {}, [], {}

    def texture(name):
        if name is None:
            return -1
        base = name.rsplit(".", 1)[0] if name.rsplit(".", 1)[-1] in ("tga", "jpg", "jpeg", "png") else name
        if base in tex_index:
            return tex_index[base]
        t = load_p8(texpak.read(texnames[base + ".p8"])) if base + ".p8" in texnames else None
        tex_index[base] = len(textures) if t else -1
        if t:
            textures.append((base, t))
        return tex_index[base]

    def material(shader_num, lightmapped):
        key = (shader_num, lightmapped)
        if key in mat_index:
            return mat_index[key]
        name, surf, cont = bsp.shaders[shader_num]
        tex, flags, blend, alpha, cull = material_of(name, scripts.get(name), lightmapped)
        if surf & SURF_SKY:
            flags |= MAT_SKY
        if surf & SURF_NODRAW:
            flags |= MAT_NODRAW
        ti = texture(tex)
        if flags & MAT_LIGHTMAP and ti >= 0:
            if alpha != ALPHA_NONE:
                # the lightmap's pass cannot leave out the texels the test leaves out: lit by
                # the vertices. (A texture with alpha that is not tested keeps its lightmap:
                # the renderer draws it as a pass of its own, the CLUT's alpha being the
                # texture's.)
                flags = (flags & ~MAT_LIGHTMAP) | MAT_VERTEX
        mat_index[key] = len(mats)
        # deformVertexes autosprite, autosprite2: quads that turn to the eye
        sprite = 0
        for l in (scripts.get(name) or {}).get("lines", []):
            if l[0].lower() == "deformvertexes" and len(l) > 1 and l[1].lower() in ("autosprite", "autosprite2"):
                sprite = 1 if l[1].lower() == "autosprite" else 2
        mats.append(dict(name=name, tex=ti, texname=tex, flags=flags, blend=blend, alpha=alpha, cull=cull,
                         surf=surf, cont=cont, sprite=sprite))
        return mat_index[key]

    # the world's size picks the positions' unit
    allxyz = bsp.verts["xyz"]
    reach = float(np.abs(allxyz).max()) if len(allxyz) else 1.0
    q = 8.0
    while reach * q > 32000 and q > 0.25:
        q /= 2
    stats = dict(surfaces=0, skipped=0, tris_in=0, tris=0, verts=0, strips=0, batches=0, patches=0)

    # triangles by (model, material, lightmap page, fog)
    groups = {}
    sprites = []  # surfaces of quads that turn to the eye: (material, fog, clusters, areas, records, mins, maxs)
    for si, s in enumerate(bsp.surfaces):
        shader_num, fog, stype, lm = s[0], s[1], s[2], s[7]
        if stype == MST_FLARE or si not in surf_model:
            stats["skipped"] += 1
            continue
        mi = material(shader_num, lm >= 0)
        m = mats[mi]
        # the sky's surfaces stay (the renderer draws the sky where the view sees them)
        if m["flags"] & MAT_NODRAW:
            stats["skipped"] += 1
            continue
        if m["sprite"] and surf_model[si] == 0 and stype != MST_PATCH and s[4] >= 4 and s[4] % 4 == 0:
            # each four vertices a quad: its centre and size (autosprite), or the line through
            # the middles of its two short sides and its width (autosprite2: it turns about
            # that line), with its corners' texture coordinates
            dv = bsp.verts[s[3]:s[3] + s[4]]
            rec, lo, hi = b"", [], []
            for k in range(0, s[4], 4):
                p, st = dv["xyz"][k:k + 4].astype(np.float64), dv["st"][k:k + 4]
                if m["sprite"] == 1:
                    c = p.mean(0)
                    r = float(np.linalg.norm(p[0] - c)) * 0.7071
                    rec += struct.pack("<3ff3ff8f", *c, r, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1)
                    lo.append(c - r * 1.5), hi.append(c + r * 1.5)
                else:
                    # its two short sides: the nearest two corners and the other two (the
                    # corners come in any order); a and d on one long side
                    i, j = min(((i, j) for i in range(4) for j in range(i + 1, 4)),
                               key=lambda ij: float(np.linalg.norm(p[ij[0]] - p[ij[1]])))
                    k, l = (x for x in range(4) if x not in (i, j))
                    if np.linalg.norm(p[k] - p[i]) < np.linalg.norm(p[l] - p[i]):
                        k, l = l, k
                    o = [i, j, k, l]
                    a, b_, c_, d = (p[x] for x in o)
                    e0, e1 = (a + b_) / 2, (c_ + d) / 2
                    c, axis = (e0 + e1) / 2, (e1 - e0) / 2
                    hw = (float(np.linalg.norm(a - b_)) + float(np.linalg.norm(c_ - d))) / 4
                    rec += struct.pack("<3ff3ff8f", *c, 0, *axis, hw, *[float(x) for i in o for x in st[i]])
                    lo.append(np.minimum(e0, e1) - hw), hi.append(np.maximum(e0, e1) + hw)
            sprites.append((mi, fog, surf_clusters.get(si, set()), frozenset(surf_areas.get(si, set())), rec,
                            np.min(lo, axis=0), np.max(hi, axis=0)))
            stats["surfaces"] += 1
            continue
        if stype == MST_PATCH:
            verts, tris = patch_triangles(bsp, s, args.curve)
            stats["patches"] += 1
        else:
            verts = surface_vertices(bsp, s)
            idx = bsp.indexes[s[5]:s[5] + s[6]]
            tris = [(int(idx[k]), int(idx[k + 1]), int(idx[k + 2])) for k in range(0, len(idx) - 2, 3)]
        stats["surfaces"] += 1
        stats["tris_in"] += len(tris)
        if m["flags"] & MAT_SKY:
            # not drawn: the surface's bounds alone, as one triangle from its
            # lowest to its highest corner and a corner along its widest side
            # (where the view sees it, the sky is)
            lo, hi = verts[:, :3].min(0), verts[:, :3].max(0)
            a, b, c = verts[0].copy(), verts[0].copy(), verts[0].copy()
            a[:3], b[:3], c[:3] = lo, hi, lo
            c[int(np.argmax(hi - lo))] = hi[int(np.argmax(hi - lo))]
            verts, tris = np.array([a, b, c]), [(0, 1, 2)]
        else:
            verts, tris = split_long(verts, tris, args.edge, 3.5)
        if m["tex"] >= 0:
            tw, th = textures[m["tex"]][1]["w"], textures[m["tex"]][1]["h"]
            for t in tris:
                a, b, c = verts[t[0]], verts[t[1]], verts[t[2]]
                n = np.cross(b[:3] - a[:3], c[:3] - a[:3])
                axyz = 0.5 * math.sqrt(float(n @ n))
                ast = 0.5 * abs((b[3] - a[3]) * (c[4] - a[4]) - (c[3] - a[3]) * (b[4] - a[4])) * tw * th
                m["area"] = m.get("area", 0.0) + axyz
                m["texels"] = m.get("texels", 0.0) + ast
        gkey = (surf_model[si], mi, lm if m["flags"] & MAT_LIGHTMAP else -1, fog)
        clusters = surf_clusters.get(si, set())
        areas = frozenset(surf_areas.get(si, set()))
        page = bsp.lightmaps[lm] if lm >= 0 and m["flags"] & MAT_LIGHTMAP else None
        vlm = bool(m["flags"] & MAT_VLM)
        for t in tris:
            tv = [verts[i] for i in t]
            # degenerate after quantisation: left out
            qp = [tuple(int(round(v[k] * q)) for k in range(3)) for v in tv]
            if qp[0] == qp[1] or qp[1] == qp[2] or qp[0] == qp[2]:
                continue
            cs, ct = sum(v[3] for v in tv) / 3, sum(v[4] for v in tv) / 3
            cell = (math.floor(cs / 4) * 4, math.floor(ct / 4) * 4)
            out = []
            for v, p in zip(tv, qp):
                st = (int(round((v[3] - cell[0]) * 4096)), int(round((v[4] - cell[1]) * 4096)))
                st = tuple(min(max(x, -32768), 32767) for x in st)
                if page is not None:
                    lmst = (min(max(int(round(v[5] * 4096)), -32768), 32767), min(max(int(round(v[6] * 4096)), -32768), 32767))
                    rgb = sample_lightmap(page, v[5], v[6])
                    top = rgb.max()
                    hue = rgb / top if top > 0 else np.ones(3)
                    if vlm:
                        # times the vertex's colour as rgbGen vertex shows it: half of
                        # what the engine loads (twice the file's, not over 255)
                        hue = hue * np.minimum(v[10:13] * 2, 255.0) / 510.0
                    col = tuple(int(round(h * 128)) for h in hue) + (128,)
                else:
                    lmst = (0, 0)
                    c = v[10:13] * 2
                    top = c.max()
                    if top > 255:
                        c = c * 255 / top
                    if vlm:
                        # lit by its vertices in place of its lightmap (its alpha is
                        # tested): the vertex's colour halved, times the lightmap there
                        # (the engine's twice the file's, twice as bright on the screen)
                        l2 = c
                        if lm >= 0:
                            l2 = sample_lightmap(bsp.lightmaps[lm], v[5], v[6]) * 2.0
                            top = l2.max()
                            if top > 255:
                                l2 = l2 * 255 / top
                        c = c * l2 / 508.0
                    col = tuple(int(round(x)) for x in c) + (int(round(v[13] * 128 / 255)),)
                out.append((p, st, lmst, col))
            # the triangle's normal (the BSP's faces are wound clockwise seen from the front)
            e1, e2 = np.subtract(qp[1], qp[0]), np.subtract(qp[2], qp[0])
            nrm = np.cross(e2, e1).astype(np.float64)
            ln = math.sqrt(float(nrm @ nrm))
            nrm = nrm / ln if ln > 0 else np.zeros(3)
            # a chunk is of one direction, so that it can be left out when it faces away: flat
            # surfaces by their plane's normal, the others by the axis they face most
            if stype == MST_PLANAR:
                okey = tuple(int(round(x * 16)) for x in nrm)
            else:
                ax = int(np.argmax(np.abs(nrm)))
                okey = (100 + ax * 2 + (nrm[ax] < 0),)
            groups.setdefault(gkey, []).append(
                (out, clusters, tuple(sum(v[k] for v in tv) / 3 for k in range(3)), nrm, okey, areas))

    # groups: the triangles of a material and lightmap page cut by place; chunks: a group's
    # triangles of one direction
    chunks = []  # (model, mat, lm, fog, [tri], group)
    ngroup = 0

    def cut(model, mat, lm, fog, tris):
        nonlocal ngroup
        c = np.array([t[2] for t in tris])
        size = c.max(0) - c.min(0)
        # small enough, and in one place: a group spread over the map is in every view
        if len(tris) * 1.4 <= args.chunk and (size.max() <= args.extent or len(tris) <= args.least):
            by = {}
            for t in tris:
                by.setdefault(t[4], []).append(t)
            for okey in sorted(by):
                chunks.append((model, mat, lm, fog, by[okey], ngroup))
            ngroup += 1
            return
        axis = int(np.argmax(size))
        order = np.argsort(c[:, axis], kind="stable")
        half = len(tris) // 2
        cut(model, mat, lm, fog, [tris[i] for i in order[:half]])
        cut(model, mat, lm, fog, [tris[i] for i in order[half:]])

    for (model, mat, lm, fog), tris in sorted(groups.items()):
        if tris:
            cut(model, mat, lm, fog, tris)

    blob = bytearray()
    recs = []
    world_vis = []
    for model, mat, lm, fog, tris, group in chunks:
        vid, vlist, itris = {}, [], []
        clusters = set()
        nsum = np.zeros(3)
        chunk_areas = set()
        for out, cl, _, nrm, _okey, ar in tris:
            nsum += nrm
            clusters |= cl
            chunk_areas |= ar
            ids = []
            for v in out:
                i = vid.get(v)
                if i is None:
                    i = vid[v] = len(vlist)
                    vlist.append(v)
                ids.append(i)
            itris.append(tuple(ids))
        strips = stripify(itris)
        stream = []  # (vertex, w)
        for st in strips:
            for i, r in st:
                stream.append((vlist[i], W_NONE if r is None else W_REV if r else 0))
        # batches: a strip that goes on in the next one starts there with its last two vertices
        batches, pos = [], 0
        while pos < len(stream):
            b = stream[pos:pos + BATCH]
            if pos and stream[pos][1] != W_NONE:
                # mid-strip: the two vertices before, as a strip's first two
                b = [(stream[pos - 2][0], W_NONE), (stream[pos - 1][0], W_NONE)] + stream[pos:pos + BATCH - 2]
                pos += BATCH - 2
            else:
                pos += BATCH
            # a batch does not end on a strip's first or second vertex
            while b and b[-1][1] == W_NONE:
                b.pop()
                pos -= 1
            if b:
                # an even count (the programs take two vertices a turn): the last one again,
                # ending no triangle
                if len(b) & 1:
                    b.append((b[-1][0], W_NONE))
                batches.append(b)
        lit = lm >= 0
        body = bytearray()
        nv = 0
        for b in batches:
            n = len(b)
            nv += n
            body += vif(VIF_STCYCL, 0, 1 << 8 | 3)
            d = b"".join(struct.pack("<3h", v[1][0], v[1][1], 4096) for v, _ in b)
            body += vif(UNPACK_V3_16, n, 0x8000 | VU_A) + d + b"\0" * (-len(d) % 4)
            d = b"".join(struct.pack("<4B", *[min(max(c, 0), 255) for c in v[3]]) for v, _ in b)
            body += vif(UNPACK_V4_8, n, 0xC000 | (VU_A + 1)) + d
            d = b"".join(struct.pack("<3hH", v[0][0], v[0][1], v[0][2], w) for v, w in b)
            body += vif(UNPACK_V4_16, n, 0x8000 | (VU_A + 2)) + d
            if lit:
                d = b"".join(struct.pack("<3h", v[2][0], v[2][1], 4096) for v, _ in b)
                body += vif(UNPACK_V3_16, n, 0x8000 | VU_IB) + d + b"\0" * (-len(d) % 4)
            body += vif(VIF_ITOP, 0, n) + vif(VIF_MSCNT)
        body += b"\0" * (-len(body) % 16)
        qwc = len(body) // 16
        off = len(blob)
        blob += struct.pack("<IIQ", DMA_CNT << 28 | qwc, 0, 0) + body + struct.pack("<IIQ", DMA_RET << 28, 0, 0)
        p = np.array([v[0] for v in vlist], np.float64) / q
        # the cone of its triangles' normals: the axis, and the sine of the widest angle to it
        # (255: no cone, drawn always: two-sided, or its triangles face all ways)
        ln = math.sqrt(float(nsum @ nsum))
        cone = (0, 0, 0, 255)
        if ln > 0 and mats[mat]["cull"] == CULL_FRONT:
            axis = np.round(nsum / ln * 127)
            an = axis / math.sqrt(float(axis @ axis))
            cosw = min(float(t[3] @ an) for t in tris)
            if cosw > 0.2:
                sinw = math.sqrt(max(0.0, 1 - cosw * cosw)) + 0.01
                cone = (int(axis[0]), int(axis[1]), int(axis[2]), min(254, int(math.ceil(sinw * 255))))
        recs.append(dict(mins=p.min(0), maxs=p.max(0), mat=mat, lm=lm, nverts=nv, flags=CH_LIGHTMAP if lit else 0,
                         fog=fog & 255, off=off, qwc=qwc + 2, nbatch=len(batches), model=model, cone=cone,
                         # in one area, of the world: left out while a closed door shuts that area off
                         area=next(iter(chunk_areas)) if model == 0 and len(chunk_areas) == 1
                         and 0 <= next(iter(chunk_areas)) < 255 else 255,
                         group=group))
        world_vis.append(clusters)
        stats["tris"] += len(itris)
        stats["verts"] += nv
        stats["strips"] += len(strips)
        stats["batches"] += len(batches)

    # the surfaces of quads that turn to the eye: a chunk each, without vertices, after the
    # world's chunks (a view finds them as it finds those: by cluster, area and bounds)
    at = next((i for i, r in enumerate(recs) if r["model"] != 0), len(recs))
    for mi, fog, clusters, areas, rec, lo, hi in sorted(sprites, key=lambda t: t[0]):
        off = len(blob)
        blob += rec + b"\0" * (-len(rec) % 16)
        recs.insert(at, dict(mins=lo, maxs=hi, mat=mi, lm=-1, nverts=0, flags=CH_SPRITE, fog=fog & 255, off=off,
                             qwc=len(rec) // 64, nbatch=0, model=0, cone=(0, 0, 0, 255),
                             area=next(iter(areas)) if len(areas) == 1 and 0 <= next(iter(areas)) < 255 else 255,
                             group=ngroup))
        world_vis.insert(at, clusters)
        ngroup += 1
        at += 1

    # groups: bounds and chunk ranges (the chunks lie by group, the groups by model and material)
    grecs = []
    for i, r in enumerate(recs):
        if not grecs or grecs[-1]["id"] != r["group"]:
            grecs.append(dict(id=r["group"], model=r["model"], first=i, n=0, mins=r["mins"].copy(),
                              maxs=r["maxs"].copy(), clusters=set()))
        g = grecs[-1]
        g["n"] += 1
        g["mins"] = np.minimum(g["mins"], r["mins"])
        g["maxs"] = np.maximum(g["maxs"], r["maxs"])
        g["clusters"] |= world_vis[i]
    # models: group ranges
    models = []
    for mi, m in enumerate(bsp.models):
        ids = [i for i, g in enumerate(grecs) if g["model"] == mi]
        models.append((m[0:3], m[3:6], ids[0] if ids else 0, len(ids)))
    nworld = models[0][3] if models else 0

    # visibility: the groups seen from each cluster
    ncl = bsp.nclusters
    vis_bytes = (nworld + 7) // 8
    vis_bytes += -vis_bytes % 4
    vis = np.zeros((max(ncl, 1), max(vis_bytes, 4)), np.uint8)
    vis_bytes = vis.shape[1]
    if ncl:
        seen = np.unpackbits(bsp.vis, axis=1, bitorder="little")[:, :ncl].astype(bool)  # seen[k][c]
        for gi in range(nworld):
            cl = np.zeros(ncl, bool)
            cl[list(grecs[gi]["clusters"])] = True
            rows = (seen & cl[None, :]).any(1)
            vis[rows, gi >> 3] |= 1 << (gi & 7)
    else:
        vis[:] = 0xFF
    # and the chunks: a group lies in several clusters' sight, its chunks each in fewer
    cvis_bytes = (len(recs) + 7) // 8
    cvis_bytes += -cvis_bytes % 4
    cvis = np.full((max(ncl, 1), max(cvis_bytes, 4)), 0xFF, np.uint8)
    cvis_bytes = cvis.shape[1]
    if ncl:
        cmat = np.zeros((len(recs), ncl), np.float32)
        for i in range(len(recs)):
            cmat[i, list(world_vis[i])] = 1
        by_chunk = (seen.astype(np.float32) @ cmat.T) > 0  # [k][chunk]
        by_chunk[:, [i for i, r in enumerate(recs) if r["model"] != 0]] = True
        bits = np.zeros((ncl, cvis_bytes * 8), bool)
        bits[:, :len(recs)] = by_chunk
        cvis = np.packbits(bits, axis=1, bitorder="little")

    # ---- the file ---------------------------------------------------------------------
    def pad16(b):
        return b + b"\0" * (-len(b) % 16)

    sec = {}
    sec["mat"] = b"".join(struct.pack("<64shHBBBBIIf12x", m["name"].encode()[:63], m["tex"], m["flags"], m["blend"],
                                      m["alpha"], m["cull"], 0, m["surf"] & 0xFFFFFFFF, m["cont"] & 0xFFFFFFFF,
                                      math.sqrt(m["texels"] / m["area"]) if m.get("area") else 1.0)
                          for m in mats)
    lmdata, lmdim = b"", b""
    for pg in bsp.lightmaps:
        lmdim += struct.pack("<HHI", pg.shape[1], pg.shape[0], len(lmdata))
        lmdata += pad16((128 + np.minimum(pg.max(axis=2), 127)).astype(np.uint8).tobytes())
    planes = bsp.planes.astype("<f4").tobytes()
    nodes = np.ascontiguousarray(bsp.nodes[:, 0:3]).astype("<i4").tobytes()
    leafs = np.ascontiguousarray(bsp.leafs[:, 0:2]).astype("<i2").tobytes()
    order = ["mat", "tex", "chunk", "group", "model", "lightmap", "lmdim", "vis", "plane", "node", "leaf", "cvis", "data"]
    size = {"mat": len(sec["mat"]), "tex": 72 * len(textures), "chunk": 48 * len(recs), "group": 32 * len(grecs),
            "model": 32 * len(models),
            "lightmap": len(lmdata), "lmdim": len(lmdim), "vis": vis.size, "plane": len(planes), "node": len(nodes), "leaf": len(leafs),
            "cvis": cvis.size, "data": len(blob)}
    off, o = {}, 128
    for k in order:
        off[k] = o
        o += size[k] + (-size[k] % 16)
    total = o
    sec["tex"] = b"".join(struct.pack("<64sHHB3x", n.encode()[:63], t["w"], t["h"], int(t["alpha"]))
                          for n, t in textures)
    sec["chunk"] = b"".join(struct.pack("<3fHh3fHBBIHH3bBB3x", *r["mins"], r["mat"], r["lm"], *r["maxs"], r["nverts"],
                                        r["flags"], r["fog"], off["data"] + r["off"], r["qwc"], r["nbatch"],
                                        *r["cone"], r["area"])
                            for r in recs)
    sec["group"] = b"".join(struct.pack("<3fI3fI", *g["mins"], g["first"], *g["maxs"], g["n"]) for g in grecs)
    sec["model"] = b"".join(struct.pack("<6fII", *mn, *mx, f, n) for mn, mx, f, n in models)
    sec["lightmap"], sec["vis"], sec["plane"], sec["node"], sec["leaf"] = lmdata, vis.tobytes(), planes, nodes, leafs
    sec["data"] = bytes(blob)
    sec["cvis"] = cvis.tobytes()
    sec["lmdim"] = lmdim
    head = struct.pack("<4sI7I3I11IfIIIIII", b"PMAP", 10, len(mats), len(textures), len(recs), len(models),
                       len(bsp.lightmaps), ncl, vis_bytes, len(bsp.planes), len(bsp.nodes), len(bsp.leafs),
                       off["mat"], off["tex"], off["chunk"], off["model"], off["lightmap"], off["vis"], off["plane"],
                       off["node"], off["leaf"], off["data"], total, 1.0 / q, BATCH, len(grecs), off["group"],
                       off["cvis"], cvis_bytes, off["lmdim"])
    with open(args.out, "wb") as f:
        f.write(head + b"\0" * (128 - len(head)))
        for k in order:
            f.write(pad16(sec[k]))
    if args.stats:
        missing = sorted({m["texname"] for m in mats if m["tex"] < 0 and m["texname"]
                          and not m["flags"] & (MAT_NODRAW | MAT_SKY)})
        print(f"{args.map}: {total} bytes; positions 1/{q:g} unit; {stats['surfaces']} surfaces "
              f"({stats['patches']} curves, {stats['skipped']} left out), {stats['tris_in']} -> {stats['tris']} "
              f"triangles, {stats['verts']} strip vertices ({stats['verts'] / max(stats['tris'], 1):.2f} a triangle) "
              f"in {stats['strips']} strips, {stats['batches']} batches, {len(recs)} chunks in {len(grecs)} groups "
              f"({nworld} of the world); "
              f"{len(mats)} materials, {len(textures)} textures, {len(bsp.lightmaps)} "
              f"lightmap pages, {ncl} clusters; chunk data {len(blob)} bytes")
        if missing:
            print("  textures not in the texture pak:", ", ".join(missing[:12]), "..." if len(missing) > 12 else "")
        nv = np.array([r["nverts"] for r in recs])
        print(f"  chunks with a cone: {sum(1 for r in recs if r['cone'][3] < 255)}, flat ones "
              f"{sum(1 for r in recs if r['cone'][3] <= 4)}")
        if len(nv):
            print(f"  a chunk: {nv.mean():.0f} vertices (the largest {nv.max()}), "
                  f"{np.mean([r['nbatch'] for r in recs]):.1f} batches")
        if ncl and nworld:
            per = np.unpackbits(vis, axis=1).sum(1)
            per = per[per > 0]
            print(f"  groups seen from a cluster: {per.mean():.0f} on average, {per.max()} at most")
            # what a view sends for the chunks' own visibility, against the groups'
            wchunks = [i for i, r in enumerate(recs) if r["model"] == 0]
            by_chunk = by_chunk[:, wchunks]
            gbit = np.unpackbits(vis, axis=1, bitorder="little")[:, :nworld].astype(bool)
            gof = np.array([next(k for k, g in enumerate(grecs) if g["first"] <= i < g["first"] + g["n"]) for i in wchunks])
            by_group = gbit[:, gof]
            lmof = np.array([recs[i]["lm"] for i in wchunks])
            if len(bsp.lightmaps):
                onehot = np.zeros((len(wchunks), len(bsp.lightmaps)), np.float32)
                onehot[np.arange(len(wchunks))[lmof >= 0], lmof[lmof >= 0]] = 1
                pg = ((by_chunk.astype(np.float32) @ onehot) > 0).sum(1)
                pg = pg[pg > 0]
                print(f"  lightmap pages: {len(bsp.lightmaps)} (the BSP's {lm_was}); seen from a cluster: "
                      f"{pg.mean():.1f} on average, {pg.max()} at most")
            wv = nv[wchunks].astype(np.float64)
            a, b = by_group @ wv, by_chunk @ wv
            use = a > 0
            print(f"  vertices seen from a cluster: {a[use].mean():.0f} by groups, {b[use].mean():.0f} by chunks "
                  f"({100 * b[use].sum() / a[use].sum():.0f} %)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("basedir")
    ap.add_argument("map")
    ap.add_argument("--tex", default=None, help="ps2tex.pk3 (default: BASEQ3_DIR/ps2tex.pk3)")
    ap.add_argument("--curve", type=float, default=4.0, help="a curve's chords stay within this of it")
    ap.add_argument("--edge", type=float, default=384.0, help="edges longer than this are halved")
    ap.add_argument("--chunk", type=int, default=200, help="strip vertices a chunk, about")
    ap.add_argument("--extent", type=float, default=768.0, help="a chunk's triangles lie within this, unless few")
    ap.add_argument("--least", type=int, default=6, help="a chunk of this many triangles is not cut for its extent")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--keep-lightmaps", action="store_true", help="the BSP's lightmap pages as they are, not packed by place")
    a = ap.parse_args()
    if a.tex is None:
        a.tex = os.path.join(a.basedir, "ps2tex.pk3")
    build(a)


if __name__ == "__main__":
    main()
