#!/usr/bin/env python3
"""The PS2 renderer's VU1 programs (PLAN-PS2.md, 5.4): their loops scheduled by
maxsdk's tools/vusched, written as code/renderer_ps2/vu/q3vu.vsm and q3vu.h.

    ps2vu.py [--maxsdk DIR] [--out code/renderer_ps2/vu] [-v]

VU1 data memory (quadwords): the context at 0 (Q3VU_CTX_*: what a draw's
programs read once, at their set-up, and the room of the programs that cut),
then two buffers of Q3VU_BUF (VIF1's double buffering, BASE Q3VU_CTX_QW,
OFFSET Q3VU_BUF). A buffer, from its start:

    0 ..          a set-up's GS registers (an A+D packet, kicked by the set-up)
    VU_A - 1      the first pass's GIF tag, written by the program
    VU_A + 3 i    vertex i as the VIF wrote it (the texture coordinates, the
                  colour and the position: tools/ps2map.py); the first pass's
                  ST, RGBAQ, XYZ2 in place when there are two passes
    VU_B - 1      the last pass's GIF tag
    VU_B + 3 i    vertex i of the last pass, written by the program alone:
                  the lightmap's ST; RGBAQ: the GS does not read it (a decal;
                  it only takes Q at it), so the programs that cut keep the
                  vertex's position before the divide there; XYZ2 as the
                  first pass's. With one pass: that pass
    VU_IB + 3 i   the lightmap's coordinates as the VIF wrote them

No batch waits for the one before, so VU1 works while the GS draws. The VIF
unpacks batch k + 2 into the buffer of batch k as soon as program k + 1 has
started, and the GIF may still be sending batch k's last packet then: so
that packet lies where the VIF writes nothing (VU_B). The packet before it
has left by then: its program's last XGKICK waited for it. q3w1c alone
draws its one pass in place and waits for it to leave (an XGKICK of the
packet that sets nothing): what it leaves out is cut from that place.
q3m's packet and what the VIF writes for it lie elsewhere than these: between
a run of q3m's and one of the others the stream waits (a FLUSH, rp_list.c).

Each program has a set-up entry (MSCAL: the context into registers, the GS
registers kicked) and ends before its batch entry, so every batch after it is
an MSCNT with the vertex count as ITOP: the map's chunks hold no program
address. A batch's count is even (the loops take two vertices a turn).

Programs:
    w2   texture and lightmap from one vertex: the vertex is transformed once
         and written twice (the lightmap in GS context 2)
    w1   one pass
         Both leave out (ADC) a triangle whose three vertices are all above
         or all below a window of the picture's lines (the context's SS, SN:
         the strip of beam racing a pass draws): the GS takes about 40 EE
         cycles to set up a flat triangle and 70 a textured one, drawn or
         not, and a chunk is in the records of every strip it touches.
    w2c, w1c   the same for chunks across the near plane or the guard band:
         each vertex is tested against the guard band and the depth planes
         (CLIPW of the position scaled into that volume) and a triangle with
         a vertex outside is left out of the strip (ADC). After the batch's
         strips are kicked, q3clip cuts those triangles and sends what is
         left of each as triangles of its own.

    m    an MD3 model's surface, a piece of it a batch (code/renderer_ps2/
         rp_model.c cuts a surface into pieces of at most M_VERTS vertices):
         the piece's vertices each once (texture coordinates, normal, the new
         frame's position, the old frame's: M_VERT + 4 i), blended, lit
         (ambient + directed * N.L, the directed light's colour times its
         direction as three uniforms) and written in place as ST, RGBAQ,
         XYZ2; then the piece's triangles (M_IDX: their number, then a
         quadword each: its vertices' offsets 4 i) gathered into the packet
         at M_OUT, three vertices each (the first two ADC), but those that
         face away: the sign of the screen's cross product of its edges
         against the context's CULL (x: 0x20 leaves out those whose product
         is negative, 0 the others, 0x40 none). Nothing is cut: for models
         inside the guard band.
    w1t, w1tc   one pass of a shader's stage: w1 and w1c with the texture
         coordinates through a matrix (TM0, TM1, TM2: tcMod) and the colour
         as the vertex's times CMUL plus CADD (a stage's own colour: CMUL 0)
    mc   the same for a model across the near plane or the guard band (the
         view weapon always is): each vertex is tested as w2c's and its
         position before the divide kept in its fourth quadword; a triangle
         with a vertex outside is not culled but put in the packet with ADC
         on all three (the vertices' offsets in their XYZ2's w), and after
         the packet is kicked q3mclip cuts those.
    wd, wdc   a dynamic light's pass over the world's batches: w1 and w1c with
         the texture coordinates from the position (DCTX: the light's spot,
         a texture, laid along the surface) and the colour as the light's
         times 1 - z^2, z how far the vertex is off the spot's plane.
    me, mec   m and mc with the texture coordinates of tcGen environment: the
         reflection of the eye's direction (one for the model: ECTX) in the
         vertex's normal, in place of the vertex's own.

q3clip (the end of this file): a triangle left out is found by its vertex's w
(ADC set, tools/ps2map.py's bit 13 clear). Its three positions before the
divide are in the second pass's RGBAQ slots; the texture coordinates come back
from ST * w, the colours are still integers. q3mclip, the models': each triangle
in the packet (M_IDX's y: its vertices) whose third vertex has ADC (w bit
15; the three w's: the vertices' offsets), the corners where the loop left
them. Both hand the corners to the same two routines: q3cut_test (is any of
it inside) and q3cut_poly. The polygon is cut in the
triangle's own coordinates: a vertex is three weights (1 quadword), a plane
three distances (the corners'), so the six planes are six rows and a cut vertex
is cut once for all its attributes, when it is written. An edge is always cut
from its inside vertex, so two triangles cut the edge they share at the same
point. Each triangle of the polygon's fan goes out as a packet of its own (two:
the lightmap's), and a third XGKICK (a packet that sets nothing) waits until
they have left before the next one is built in the same place.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ps2map  # noqa: E402  the buffer's layout is shared with the map tool

STRIDE = 3
FAR = ps2map.VU_B - ps2map.VU_A   # a slot of the second pass, from the first's
FARI = ps2map.VU_IB - ps2map.VU_A # and of the lightmap's coordinates as the VIF wrote them
CTX_QW = 72
BUF_QW = 476
# the context: the matrix, the clip test's scale and offset (a view's); SS, SN: the same for
# the window of lines a pass draws (y * SS + w * SN within -w .. w is inside); a material's GIF tags
# (strips; TA3, TB3: single triangles, for q3clip); NOP: a packet that sets nothing; then
# q3clip's room: PL the six planes, CO the corners' attributes (position, texture ST, colour,
# lightmap ST: three each), WA and WB the polygon (9 vertices), R0 and R1 a triangle's
# packets, SAVE integers
CTX = {"C0": 0, "C1": 1, "C2": 2, "C3": 3, "CS": 4, "CN": 5, "TA": 6, "TB": 7, "TA3": 8, "TB3": 9, "NOP": 10,
       "SS": 69, "SN": 70, "CULL": 71}
ROOM = {"PL": 12, "CO": 18, "WA": 30, "WB": 39, "R0": 48, "R1": 58, "SAVE": 68}
# q3m's buffer: the packet (its tag before it), the entries, the vertices; its uniforms where
# its set-up finds them (q3clip's polygon room: free while no triangle is cut)
M_OUT, M_IDX, M_VERT = 18, 306, 352
M_VERTS, M_TRIS = 28, 32
MCTX = {"LERP": 30, "AMB": 31, "DX": 32, "DY": 33, "DZ": 34, "C255": 35}
# q3me's more: the eye's direction from the model (in its space) for tcGen environment, each of
# its three numbers times (-1, 0, 1) / 127^2 (the normal's length is 127); E0: (0.5 - 0.5 y,
# 0.5 + 0.5 z, 1) of it, plus the stage's scroll
ECTX = {"EX": 36, "EY": 37, "EZ": 38, "E0": 39}
# a stage's, in the same room: the texture coordinates' matrix by columns (s' = TM0.x s + TM1.x
# t + TM2.x, t' the y's; TM2.z 1), the colour's factor and what is added (0 .. 255)
TCTX = {"TM0": 30, "TM1": 31, "TM2": 32, "CMUL": 33, "CADD": 34}
# a dynamic light's, in the same room: the position (as the VIF wrote it) to the light's own
# coordinates by columns (s' = DM0.x x + DM1.x y + DM2.x z + DM3.x: the spot's texture
# coordinates in x and y, and in z how far off the spot's plane, in radii); DLC its colour
DCTX = {"DM0": 30, "DM1": 31, "DM2": 32, "DM3": 33, "DLC": 34}
# q3clip's registers: the loops' uniforms and tags are kept out of them
CLIP_VF = set(range(1, 10)) | set(range(12, 21))


def kernels(vs):
    Op, Kernel = vs.Op, vs.Kernel

    def pos(clip, two, c=0):
        ops = [
            Op("lqpos", "L", "lq.xyz {V}, {off}({p})", ("V", "xyz"), mem=("lq", 2)),
            Op("itof", "U", "itof0.xyz {F}, {V}", ("F", "xyz"), [("V", "xyz")]),
            Op("mx", "U", "mulax.xyzw ACC, {C0}, {F}x", ("ACC", ""), [("F", "x")]),
            Op("my", "U", "madday.xyzw ACC, {C1}, {F}y", ("ACC", ""), [("ACC", ""), ("F", "y")]),
            Op("mz", "U", "maddaz.xyzw ACC, {C2}, {F}z", ("ACC", ""), [("ACC", ""), ("F", "z")]),
            Op("mw", "U", "maddw.xyzw {P}, {C3}, vf00w", ("P", "xyzw"), [("ACC", "")]),
            Op("div", "L", "div Q, vf00w, {P}w", ("Q", ""), [("P", "w")], lat=vs.DIV_LAT, res="DIV"),
            Op("mulq", "U", "mulq.xyz {T}, {P}, Q", ("T", "xyz"), [("P", "xyz"), ("Q", "")]),
            Op("ftoi", "U", "ftoi4.xyz {O}, {T}", ("O", "xyz"), [("T", "xyz")]),
        ]
        if clip:
            ops += [
                Op("ca", "U", "mulaw.xyz ACC, {CN}, {P}w", ("ACC", ""), [("P", "w")]),
                Op("cb", "U", "madd.xyz {PC}, {P}, {CS}", ("PC", "xyz"), [("ACC", ""), ("P", "xyz")]),
                Op("clip", "U", "clipw.xyz {PC}xyz, {P}w", ("CF", ""), [("PC", "xyz"), ("P", "w")],
                   lat=vs.CLIP_LAT),
                Op("fc", "L", "fcand vi01, 0x3ffff", ("VI:fc", ""), [("CF", "")], lat=1),
                Op("adc", "L", "iaddiu vi02, vi01, 0x7fff", ("VI:adc", ""), [("VI:fc", "")], lat=1),
                Op("lsf", "L", "ilw.w vi11, {off}({p})", ("VI:sf", ""), mem=("lq", 2), lat=1),
                Op("or", "L", "ior vi12, vi02, vi11", ("VI:w", ""), [("VI:adc", ""), ("VI:sf", "")], lat=1),
                Op("mfir", "L", "mfir.w {O}, vi12", ("O", "w"), [("VI:w", "")]),
                # for q3clip: bit 13 of vi13 stays set until a triangle is left out here (w is
                # 0x7fff | flags, or 0x8000 | flags with a vertex outside: bit 13 then is
                # ps2map's "ends no triangle"); the position before the divide
                Op("acc", "L", "iand vi13, vi13, vi12", None, [("VI:w", "")], lat=1),
                Op("sqp", "L", "sq.xyzw {P}, {off}({p})", None, [("P", "xyzw")], mem=("sq", FAR + 1)),
            ]
        else:
            # all three of a triangle's vertices below the window (+y: bits 2, 8, 14 of the
            # clip flags) or all above it (-y: 3, 9, 15): ADC
            r, q = ("vi02", "vi11") if c == 0 else ("vi03", "vi12")
            ops += [
                Op("sa", "U", "mulaw.xy ACC, {SN}, {P}w", ("ACC", ""), [("P", "w")]),
                Op("sb", "U", "madd.xy {PS}, {P}, {SS}", ("PS", "xy"), [("ACC", ""), ("P", "xy")]),
                Op("clip", "U", "clipw.xyz {PS}xyz, {P}w", ("CF", ""), [("PS", "xy"), ("P", "w")], lat=vs.CLIP_LAT),
                Op("fa", "L", "fcor vi01, 0xffbefb", ("VI:fc", "a"), [("CF", "")], lat=1),
                Op("ma", "L", f"iaddiu {r}, vi01, 0x7fff", (f"VI:r{c}", ""), [("VI:fc", "a")], lat=1),
                Op("fb", "L", "fcor vi01, 0xff7df7", ("VI:fc", "b"), [("CF", ""), (f"VI:r{c}", "")], lat=1),
                Op("mb", "L", f"iadd {r}, {r}, vi01", (f"VI:s{c}", ""), [("VI:fc", "b"), (f"VI:r{c}", "")], lat=1),
                Op("lsf", "L", f"ilw.w {q}, {{off}}({{p}})", (f"VI:f{c}", ""), mem=("lq", 2), lat=1),
                Op("or", "L", f"ior {r}, {r}, {q}", (f"VI:w{c}", ""), [(f"VI:s{c}", ""), (f"VI:f{c}", "")], lat=1),
                Op("mfir", "L", f"mfir.w {{O}}, {r}", ("O", "w"), [(f"VI:w{c}", "")]),
            ]
        # the position: in place and in the last pass's packet; one pass that is not cut: there alone
        if two or clip:
            ops.append(Op("sqpos", "L", "sq.xyzw {O}, {off}({p})", None, [("O", "xyzw")], mem=("sq", 2)))
        if two or not clip:
            ops.append(Op("sqposb", "L", "sq.xyzw {O}, {off}({p})", None, [("O", "xyzw")], mem=("sq", FAR + 2)))
        return ops

    def st(tag, src, dst, tm=False):
        ops = [
            Op("lqst" + tag, "L", "lq.xyz {S%s}, {off}({p})" % tag, ("S" + tag, "xyz"), mem=("lq", src)),
            Op("itofst" + tag, "U", "itof12.xyz {I%s}, {S%s}" % (tag, tag), ("I" + tag, "xyz"), [("S" + tag, "xyz")]),
        ]
        g = "I" + tag
        if tm:  # (s, t, 1) through the matrix's columns
            ops += [
                Op("tma", "U", "mulax.xyz ACC, {TM0}, {I%s}x" % tag, ("ACC", ""), [("I" + tag, "x")]),
                Op("tmb", "U", "madday.xyz ACC, {TM1}, {I%s}y" % tag, ("ACC", ""), [("ACC", ""), ("I" + tag, "y")]),
                Op("tmc", "U", "maddz.xyz {G%s}, {TM2}, {I%s}z" % (tag, tag), ("G" + tag, "xyz"),
                   [("ACC", ""), ("I" + tag, "z")]),
            ]
            g = "G" + tag
        return ops + [
            Op("mulst" + tag, "U", "mulq.xyz {M%s}, {%s}, Q" % (tag, g), ("M" + tag, "xyz"), [(g, "xyz"), ("Q", "")]),
            Op("sqst" + tag, "L", "sq.xyz {M%s}, {off}({p})" % tag, None, [("M" + tag, "xyz")], mem=("sq", dst)),
        ]

    # one pass, not in place: the colour goes with it
    col = [Op("lqcol", "L", "lq.xyzw {K}, {off}({p})", ("K", "xyzw"), mem=("lq", 1)),
           Op("sqcol", "L", "sq.xyzw {K}, {off}({p})", None, [("K", "xyzw")], mem=("sq", FAR + 1))]

    # a stage's colour: the vertex's times CMUL plus CADD, to slot dst
    def colt(dst):
        return [
            Op("lqcol", "L", "lq.xyzw {K}, {off}({p})", ("K", "xyzw"), mem=("lq", 1)),
            Op("kf", "U", "itof0.xyzw {KF}, {K}", ("KF", "xyzw"), [("K", "xyzw")]),
            Op("ka", "U", "mulaw.xyzw ACC, {CADD}, vf00w", ("ACC", ""), []),
            Op("kb", "U", "madd.xyzw {KC}, {KF}, {CMUL}", ("KC", "xyzw"), [("ACC", ""), ("KF", "xyzw")]),
            Op("ki", "U", "ftoi0.xyzw {KO}, {KC}", ("KO", "xyzw"), [("KC", "xyzw")]),
            Op("sqcol", "L", "sq.xyzw {KO}, {off}({p})", None, [("KO", "xyzw")], mem=("sq", dst)),
        ]

    # a dynamic light's pass: the spot's coordinates from the position, the colour by how
    # far the vertex is off the spot's plane (1 - z^2, not below 0)
    def dlight(dst):
        return [
            Op("da", "U", "mulax.xyz ACC, {DM0}, {F}x", ("ACC", ""), [("F", "x")]),
            Op("db", "U", "madday.xyz ACC, {DM1}, {F}y", ("ACC", ""), [("ACC", ""), ("F", "y")]),
            Op("dc", "U", "maddaz.xyz ACC, {DM2}, {F}z", ("ACC", ""), [("ACC", ""), ("F", "z")]),
            Op("dd", "U", "maddw.xyz {G}, {DM3}, vf00w", ("G", "xyz"), [("ACC", "")]),
            Op("ds", "U", "mulq.xy {M}, {G}, Q", ("M", "xy"), [("G", "xy"), ("Q", "")]),
            Op("dq", "U", "addq.z {M}, vf00, Q", ("M", "z"), [("Q", "")]),
            Op("sqst", "L", "sq.xyz {M}, {off}({p})", None, [("M", "xyz")], mem=("sq", dst)),
            Op("dz", "U", "mul.z {G2}, {G}, {G}", ("G2", "z"), [("G", "z")]),
            Op("ka", "U", "mulaw.xyzw ACC, {DLC}, vf00w", ("ACC", ""), []),
            Op("kb", "U", "msubz.xyzw {KC}, {DLC}, {G2}z", ("KC", "xyzw"), [("ACC", ""), ("G2", "z")]),
            Op("km", "U", "maxx.xyzw {KM}, {KC}, vf00x", ("KM", "xyzw"), [("KC", "xyzw")]),
            Op("ki", "U", "ftoi0.xyzw {KO}, {KM}", ("KO", "xyzw"), [("KM", "xyzw")]),
            Op("sqcol", "L", "sq.xyzw {KO}, {off}({p})", None, [("KO", "xyzw")], mem=("sq", dst + 1)),
        ]

    mvp = [(g, CTX[g]) for g in ("C0", "C1", "C2", "C3")]
    clipu = [(g, CTX[g]) for g in ("CS", "CN")]
    stripu = [(g, CTX[g]) for g in ("SS", "SN")]

    stageu = [(g, q) for g, q in TCTX.items()]

    def kd(name, clip, u):
        ops = lambda c: pos(clip, False, c) + dlight(0 if clip else FAR)
        return Kernel(name, ops, u, 0, STRIDE, mvp + (clipu if clip else stripu) + [(g, q) for g, q in DCTX.items()], {})

    def k(name, clip, two, u, stage=False):
        if two:
            ops = lambda c: pos(clip, two, c) + st("a", 0, 0) + st("b", FARI, FAR)
        elif clip:
            ops = lambda c: pos(clip, two, c) + st("a", 0, 0, stage) + (colt(1) if stage else [])
        else:
            ops = lambda c: pos(clip, two, c) + st("a", 0, FAR, stage) + (colt(FAR + 1) if stage else col)
        return Kernel(name, ops, u, 0, STRIDE, mvp + (clipu if clip else stripu) + (stageu if stage else []), {})

    def model(clip, env=False):
        ops = [] if env else [
            Op("lqs", "L", "lq.xyz {S}, {off}({p})", ("S", "xyz"), mem=("lq", 0)),
        ]
        ops += [
            Op("lqn", "L", "lq.xyz {N}, {off}({p})", ("N", "xyz"), mem=("lq", 1)),
            Op("lqa", "L", "lq.xyz {A}, {off}({p})", ("A", "xyz"), mem=("lq", 2)),
            Op("lqb", "L", "lq.xyz {B}, {off}({p})", ("B", "xyz"), mem=("lq", 3)),
            # the position: A + (B - A) * the old frame's share
            Op("ia", "U", "itof0.xyz {FA}, {A}", ("FA", "xyz"), [("A", "xyz")]),
            Op("ib", "U", "itof0.xyz {FB}, {B}", ("FB", "xyz"), [("B", "xyz")]),
            Op("sub", "U", "sub.xyz {D}, {FB}, {FA}", ("D", "xyz"), [("FB", "xyz"), ("FA", "xyz")]),
            Op("mul", "U", "mulx.xyz {E}, {D}, {LERP}x", ("E", "xyz"), [("D", "xyz")]),
            Op("add", "U", "add.xyz {V}, {FA}, {E}", ("V", "xyz"), [("FA", "xyz"), ("E", "xyz")]),
            Op("mx", "U", "mulax.xyzw ACC, {C0}, {V}x", ("ACC", ""), [("V", "x")]),
            Op("my", "U", "madday.xyzw ACC, {C1}, {V}y", ("ACC", ""), [("ACC", ""), ("V", "y")]),
            Op("mz", "U", "maddaz.xyzw ACC, {C2}, {V}z", ("ACC", ""), [("ACC", ""), ("V", "z")]),
            Op("mw", "U", "maddw.xyzw {P}, {C3}, vf00w", ("P", "xyzw"), [("ACC", "")]),
            Op("div", "L", "div Q, vf00w, {P}w", ("Q", ""), [("P", "w")], lat=vs.DIV_LAT, res="DIV"),
            Op("mulq", "U", "mulq.xyz {T}, {P}, Q", ("T", "xyz"), [("P", "xyz"), ("Q", "")]),
            Op("ftoi", "U", "ftoi4.xyz {O}, {T}", ("O", "xyz"), [("T", "xyz")]),
            # the colour: ambient + (directed * its direction) . N, not below the ambient
            Op("in", "U", "itof0.xyz {FN}, {N}", ("FN", "xyz"), [("N", "xyz")]),
            Op("la", "U", "mulaw.xyzw ACC, {AMB}, vf00w", ("ACC", ""), []),
            Op("lx", "U", "maddax.xyzw ACC, {DX}, {FN}x", ("ACC", ""), [("ACC", ""), ("FN", "x")]),
            Op("ly", "U", "madday.xyzw ACC, {DY}, {FN}y", ("ACC", ""), [("ACC", ""), ("FN", "y")]),
            Op("lz", "U", "maddz.xyzw {C}, {DZ}, {FN}z", ("C", "xyzw"), [("ACC", ""), ("FN", "z")]),
            Op("cmax", "U", "max.xyzw {CA}, {C}, {AMB}", ("CA", "xyzw"), [("C", "xyzw")]),
            Op("cmin", "U", "mini.xyzw {CB}, {CA}, {C255}", ("CB", "xyzw"), [("CA", "xyzw")]),
            Op("cint", "U", "ftoi0.xyzw {CO}, {CB}", ("CO", "xyzw"), [("CB", "xyzw")]),
        ]
        if env:
            # tcGen environment, the eye's direction V one for the model: d = N . V (in all of
            # ED, by signs), (s, t, 1) = E0 + (N.y d, -N.z d, 0): the outer product's first half
            ops += [
                Op("ex", "U", "mulax.xyz ACC, {EX}, {FN}x", ("ACC", ""), [("FN", "x")]),
                Op("ey", "U", "madday.xyz ACC, {EY}, {FN}y", ("ACC", ""), [("ACC", ""), ("FN", "y")]),
                Op("ez", "U", "maddz.xyz {ED}, {EZ}, {FN}z", ("ED", "xyz"), [("ACC", ""), ("FN", "z")]),
                Op("eo", "U", "opmula.xyz ACC, {FN}, {ED}", ("ACC", ""), [("FN", "xyz"), ("ED", "xyz")]),
                Op("ee", "U", "maddw.xyz {FS}, {E0}, vf00w", ("FS", "xyz"), [("ACC", "")]),
            ]
        else:
            ops.append(Op("is", "U", "itof12.xyz {FS}, {S}", ("FS", "xyz"), [("S", "xyz")]))
        ops += [
            Op("ms", "U", "mulq.xyz {SQ}, {FS}, Q", ("SQ", "xyz"), [("FS", "xyz"), ("Q", "")]),
            # in place: each over the input it came from
            Op("sqs", "L", "sq.xyz {SQ}, {off}({p})", None, [("SQ", "xyz")], mem=("sq", 0)),
            Op("sqc", "L", "sq.xyzw {CO}, {off}({p})", None, [("CO", "xyzw")], mem=("sq", 1)),
        ]
        if clip:
            # the vertex outside: bit 15 of its XYZ2's w, for the gather
            ops += [
                Op("ca", "U", "mulaw.xyz ACC, {CN}, {P}w", ("ACC", ""), [("P", "w")]),
                Op("cb", "U", "madd.xyz {PC}, {P}, {CS}", ("PC", "xyz"), [("ACC", ""), ("P", "xyz")]),
                Op("clip", "U", "clipw.xyz {PC}xyz, {P}w", ("CF", ""), [("PC", "xyz"), ("P", "w")], lat=vs.CLIP_LAT),
                Op("fc", "L", "fcand vi01, 0x3f", ("VI:fc", ""), [("CF", "")], lat=1),
                Op("adc", "L", "iaddiu vi02, vi01, 0x7fff", ("VI:adc", ""), [("VI:fc", "")], lat=1),
                Op("mfir", "L", "mfir.w {O}, vi02", ("O", "w"), [("VI:adc", "")]),
                Op("sqp", "L", "sq.xyzw {O}, {off}({p})", None, [("O", "xyzw")], mem=("sq", 2)),
                # for q3mclip: the position before the divide, over the old frame's
                Op("sqw", "L", "sq.xyzw {P}, {off}({p})", None, [("P", "xyzw")], mem=("sq", 3)),
            ]
        else:
            ops.append(Op("sqp", "L", "sq.xyz {O}, {off}({p})", None, [("O", "xyz")], mem=("sq", 2)))
        mu = [(g, q) for g, q in MCTX.items()] + ([(g, q) for g, q in ECTX.items()] if env else [])
        # q3mec: the model's ten first, in the registers q3mclip leaves alone (the uniforms
        # get vf31 downwards); the context's can be loaded again after it
        return Kernel(("q3me" if env else "q3m") + ("c" if clip else ""), lambda c: ops, 1, 0, 4,
                      (mu + mvp if clip and env else mvp + mu) + (clipu if clip else []), {})

    # name, the kernel, the IIs to try
    return [
        (model(False), range(21, 40)),
        (model(True), range(24, 44)),
        (model(False, True), range(21, 46)),
        (model(True, True), range(24, 50)),
        (k("q3w2", False, True, 2), range(24, 44)),
        (k("q3w1", False, False, 2), range(20, 40)),
        (k("q3w2c", True, True, 1), range(14, 25)),
        (k("q3w1c", True, False, 1), range(12, 23)),
        (k("q3w1t", False, False, 2, True), range(30, 56)),
        (k("q3w1tc", True, False, 1, True), range(18, 34)),
        (kd("q3wd", False, 2), range(30, 60)),
        (kd("q3wdc", True, 1), range(18, 36)),
    ]


def schedule(vs, kern, iis, verbose):
    for ii in iis:
        kern.ii = ii
        s = vs.Schedule(kern)
        try:
            if not s.solve():
                continue
            regs = vs.alloc(s)
            text, words = vs.emit(s, regs)
        except SystemExit as e:
            if verbose:
                print(f"  {kern.name} II {ii}: {e}")
            continue
        cyc, stalls, errors = vs.check(s, regs, words)
        # the second pass's stores are outside what vusched takes for the vertices: its pad
        # (the quadwords before a pass's first vertex that the first turns store into) is
        # worked out here for both passes
        pad = 0
        for i in s.insts:
            if i.op.mem and i.op.mem[0] == "sq" and i.c // s.ii:
                slot = i.op.mem[1] - (FAR if i.op.mem[1] >= FAR else 0)
                pad = max(pad, (i.c // s.ii * kern.u - i.k) * kern.stride - slot)
        slop = vs.slop_qw(s)
        rest = []
        for e in errors:
            m = re.match(r"store of iteration (-?\d+) at (\d+) outside", e)
            if m and int(m.group(2)) >= 1000 + FAR - pad:
                continue
            rest.append(e)
        # vusched keeps an upper and a lower instruction of a word from writing one register
        # (VU1 drops the lower one's result: lq.w and mfir.w put ADC into the register FTOI4
        # writes X, Y and Z of); the same over the registers as allocated, where two groups
        # may share one
        for up, low in words:
            mu = re.match(r"\S+ (vf\d\d)", up)
            ml = re.match(r"(?:lq|mfir|move|mr32|mfp)\S* (vf\d\d)", low)
            if mu and ml and mu.group(1) == ml.group(1):
                rest.append(f"upper and lower write {mu.group(1)} in one word")
        if stalls or rest:
            if verbose:
                print(f"  {kern.name} II {ii}: {stalls} stalls, {len(rest)} errors: {rest[:3]}")
            continue
        return s, regs, words, pad, slop
    raise SystemExit(f"{kern.name}: no schedule")


def program(vs, kern, s, regs, words, two, clip):
    name = kern.name
    used = set(regs.values())
    keep = CLIP_VF if clip else set()
    free = [f"vf{r:02d}" for r in range(1, 32) if f"vf{r:02d}" not in used and r not in keep]
    ta, tb = free[0], free[1]
    if clip:
        # what a batch leaves for the next one is not q3clip's
        kept = {regs[g] for g, _ in kern.uniforms}
        assert not kept & {f"vf{r:02d}" for r in CLIP_VF}, f"{name}: a uniform in q3clip's registers"
    A, B = ps2map.VU_A, ps2map.VU_B
    slop = (s.stages - 1) * kern.u * STRIDE
    out, entry = [], {}

    def lo(text, e=False):
        out.append(("nop[e]" if e else "nop", text))

    entry[name + "_setup"] = None
    out.append(("label", name + "_setup"))
    for g, qw in kern.uniforms:
        lo(f"lq.xyzw {regs[g]}, {qw}(vi00)")
    lo(f"lq.xyzw {ta}, {CTX['TA']}(vi00)")
    if two:
        lo(f"lq.xyzw {tb}, {CTX['TB']}(vi00)")
    lo("xtop vi08")
    lo("xgkick vi08")
    lo("nop", e=True)
    lo("nop")
    out.append(("label", name + "_batch"))
    lo("xtop vi08")
    lo("xitop vi09")
    lo(f"iaddiu vi04, vi08, {A}")
    lo("iadd vi05, vi09, vi09")
    lo("iadd vi05, vi05, vi09")
    lo("iadd vi05, vi05, vi04")
    lo(f"iaddiu vi05, vi05, {slop}")
    lo("iaddiu vi10, vi09, 0x7fff")
    lo("iaddiu vi10, vi10, 1")      # the count with EOP
    if clip:
        # vi13: all ones; and so the w of what the loop's first and last turns read outside
        # the batch's vertices (the turns before vertex 0 and after the last one)
        lo("isubiu vi13, vi00, 1")
        for j in range(1, s.stages):
            lo(f"isw.w vi13, {A - 3 * j + 2}(vi08)")
        for j in range(s.stages - 1):
            lo(f"isw.w vi13, {3 * j + 2 - slop}(vi05)")
    out.append(("label", name + "_loop"))
    for up, low in words:
        out.append((up, low))
    # one pass that is not cut: its packet is the last pass's place
    first = A if two or clip else B
    lo(f"mfir.x {ta}, vi10")
    if two:
        lo(f"mfir.x {tb}, vi10")
    lo(f"iaddiu vi07, vi08, {first - 1}")
    if two:
        lo(f"iaddiu vi06, vi08, {B - 1}")
    lo("nop")
    lo(f"sq.xyzw {ta}, {first - 1}(vi08)")
    if two:
        lo(f"sq.xyzw {tb}, {B - 1}(vi08)")
    lo("nop")
    lo("nop")
    lo("nop")
    lo("xgkick vi07")
    if two:
        lo("xgkick vi06")
    if clip:
        lo("iaddiu vi01, vi00, 0x2000")
        lo("iand vi01, vi13, vi01")
        lo("nop")
        lo(f"ibne vi01, vi00, {name}_whole")
        lo("nop")
        lo("bal vi15, q3clip")
        lo(f"iaddiu vi14, vi00, {1 if two else 0}")
        out.append(("label", name + "_whole"))
        if not two:
            # its pass, in place where the VIF writes, has left before the program ends
            lo(f"iaddiu vi01, vi00, {CTX['NOP']}")
            lo("xgkick vi01")
    lo("nop", e=True)
    lo("nop")
    lo(f"b {name}_batch")
    lo("nop")
    return out


def program_m(vs, kern, s, regs, words):
    """q3m, q3mc: the set-up, then a batch: the piece's vertices by the loop, the gather, the kick."""
    name, clip = kern.name, kern.name.endswith("c")
    used = set(regs.values())
    free = [f"vf{r:02d}" for r in range(1, 32) if f"vf{r:02d}" not in used]
    # too few (q3mec): the loop's own registers too, but its uniforms' (the loop is over
    # when the gather starts)
    if len(free) < 9:
        kept = {regs[g] for g, _ in kern.uniforms}
        free += [f"vf{r:02d}" for r in range(1, 32) if f"vf{r:02d}" in used and f"vf{r:02d}" not in kept]
    if clip:
        # the tag is kept over q3mclip: not in its registers
        tag = [f for f in free if int(f[2:]) not in CLIP_VF][0]
        free.remove(tag)
        ga, gb, gc = free[0], free[1], free[2]
        assert len(free) >= 8, f"{name}: {len(free)} free VF registers for the gather"
        # nor the uniforms, but those it can load again (the context's first quadwords;
        # the models' own lie in q3clip's room)
        again = [(g, qw) for g, qw in kern.uniforms if int(regs[g][2:]) in CLIP_VF]
        assert all(qw < ROOM["PL"] for _, qw in again), f"{name}: a model uniform in q3clip's registers"
    else:
        tag = free.pop(0)
        ga, gb, gc = free[0], free[1], free[2]
    slop = (s.stages - 1) * kern.u * kern.stride
    out = []

    def lo(text, e=False):
        out.append(("nop[e]" if e else "nop", text))

    out.append(("label", name + "_setup"))
    for g, qw in kern.uniforms:
        lo(f"lq.xyzw {regs[g]}, {qw}(vi00)")
    lo(f"lq.xyzw {tag}, {CTX['TA']}(vi00)")
    lo("xtop vi08")
    lo("xgkick vi08")
    lo("nop", e=True)
    lo("nop")
    out.append(("label", name + "_batch"))
    lo("xtop vi08")
    lo("xitop vi09")
    lo(f"iaddiu vi04, vi08, {M_VERT}")
    lo("iadd vi05, vi09, vi09")
    lo("iadd vi05, vi05, vi05")
    lo("iadd vi05, vi05, vi04")
    lo(f"iaddiu vi05, vi05, {slop}")
    out.append(("label", name + "_loop"))
    for up, low in words:
        out.append((up, low))
    # the triangles: vi04 the next, vi05 their end, vi06 the packet's next vertex, vi07 the
    # vertices, vi11 0x8000 (ADC), vi12 the packet's vertices, vi13 0x20 (the MAC flag's sign
    # of z), vi15 the cull (CTX CULL); clip: vi09 the three vertices' tests (0x7fff inside,
    # 0x8000 outside) together
    pa, pb, pc_ = free[3], free[4], free[5]
    ea, eb = free[6], free[7]
    lo(f"ilw.x vi13, {M_IDX}(vi08)")
    lo(f"iaddiu vi04, vi08, {M_IDX + 1}")
    lo(f"iaddiu vi06, vi08, {M_OUT}")
    lo(f"iaddiu vi07, vi08, {M_VERT}")
    lo("iaddiu vi11, vi00, 0x7fff")
    lo("iaddiu vi11, vi11, 1")
    lo("iadd vi05, vi13, vi04")
    lo("iaddiu vi12, vi00, 0")
    lo(f"ilw.x vi15, {CTX['CULL']}(vi00)")
    lo("iaddiu vi13, vi00, 0x20")
    out.append(("label", name + "_gather"))
    ent = ("vi01", "vi02", "vi03")
    for k, f in enumerate("xyz"):
        lo(f"ilw.{f} {ent[k]}, 0(vi04)")
    if clip:
        lo("iaddiu vi09, vi00, 0")
    for k, (e, pk) in enumerate(zip(ent, (pa, pb, pc_))):
        lo(f"iadd vi10, {e}, vi07")
        lo(f"lq.xyzw {ga}, 0(vi10)")
        lo(f"lq.xyzw {gb}, 1(vi10)")
        lo(f"lq.xyzw {pk}, 2(vi10)")
        if clip:
            lo("ilw.w vi14, 2(vi10)")
        lo(f"sq.xyzw {ga}, {3 * k}(vi06)")
        lo(f"sq.xyzw {gb}, {3 * k + 1}(vi06)")
        lo(f"sq.xyzw {pk}, {3 * k + 2}(vi06)")
        if clip:
            # the vertex's offset for q3mclip; ADC on the first two
            lo("ior vi09, vi09, vi14")
            if k < 2:
                lo(f"ior vi10, {e}, vi11")
            else:
                lo("iand vi10, vi09, vi11")
                lo(f"ior vi10, vi10, {e}")
            lo(f"isw.w vi10, {3 * k + 2}(vi06)")
        else:
            lo(f"isw.w {'vi11' if k < 2 else 'vi00'}, {3 * k + 2}(vi06)")
    if clip:
        # a vertex outside: kept for q3mclip, not culled (its positions are not the screen's)
        lo("iand vi09, vi09, vi11")
        lo("nop")
        lo(f"ibne vi09, vi00, {name}_keep")
        lo("nop")
    # the screen's cross product of the edges a -> b, a -> c: z = ea.x eb.y - eb.x ea.y
    out.append((f"itof4.xy {pa}, {pa}", "nop"))
    out.append((f"itof4.xy {pb}, {pb}", "nop"))
    out.append((f"itof4.xy {pc_}, {pc_}", "nop"))
    out.append((f"sub.xy {ea}, {pb}, {pa}", "nop"))
    out.append((f"sub.xy {eb}, {pc_}, {pa}", "nop"))
    out.append((f"opmula.xyz ACC, {ea}, {eb}", "nop"))
    out.append((f"opmsub.xyz {ea}, {eb}, {ea}", "nop"))
    lo("nop")
    lo("nop")
    lo("nop")
    lo("fmand vi10, vi13")
    lo("nop")
    lo(f"ibeq vi10, vi15, {name}_drop")
    lo("nop")
    out.append(("label", name + "_keep"))
    lo("iaddiu vi06, vi06, 9")
    lo("iaddiu vi12, vi12, 3")
    out.append(("label", name + "_drop"))
    lo("iaddiu vi04, vi04, 1")
    lo("nop")
    lo(f"ibne vi04, vi05, {name}_gather")
    lo("nop")
    if clip:
        # the packet's vertices, for q3mclip
        lo(f"isw.y vi12, {M_IDX}(vi08)")
    lo("iaddiu vi12, vi12, 0x7fff")
    lo("iaddiu vi12, vi12, 1")        # with EOP
    lo(f"mfir.x {tag}, vi12")
    lo(f"iaddiu vi07, vi08, {M_OUT - 1}")
    lo("nop")
    lo("nop")
    lo(f"sq.xyzw {tag}, {M_OUT - 1}(vi08)")
    lo("nop")
    lo("nop")
    lo("nop")
    lo("xgkick vi07")
    if clip:
        lo("bal vi15, q3mclip")
        lo("nop")
        for g, qw in again:
            lo(f"lq.xyzw {regs[g]}, {qw}(vi00)")
    lo("nop", e=True)
    lo("nop")
    lo(f"b {name}_batch")
    lo("nop")
    return out, pad_of(s, kern), slop


def pad_of(s, kern):
    pad = 0
    for i in s.insts:
        if i.op.mem and i.op.mem[0] == "sq" and i.c // s.ii:
            pad = max(pad, (i.c // s.ii * kern.u - i.k) * kern.stride - i.op.mem[1])
    return pad


def clipper():
    """q3clip (this file's head): vi08 the buffer, vi09 its vertices, vi14 1: two passes,
    vi15 the return; q3mclip, the models'. An instruction a word, but where a pair is
    plainly independent."""
    A = ps2map.VU_A
    PL, CO, WA, WB, R0, R1, SAVE = (ROOM[k] for k in ("PL", "CO", "WA", "WB", "R0", "R1", "SAVE"))
    CP, CSA, CC, CSB = CO, CO + 3, CO + 6, CO + 9
    assert R1 == R0 + 10 and SAVE < CTX_QW
    out = []

    def L(*texts):
        for t in texts:
            out.append(("nop", t))

    def U(*texts):
        for t in texts:
            out.append((t, "nop"))

    def lab(n):
        out.append(("label", n))

    def gap(n):
        L(*["nop"] * n)

    lab("q3clip")
    L(f"isw.x vi15, {SAVE}(vi00)", f"isw.y vi14, {SAVE}(vi00)")
    L(f"lq.xyzw vf01, {CTX['TA3']}(vi00)", f"lq.xyzw vf02, {CTX['TB3']}(vi00)", "nop", "nop",
      f"sq.xyzw vf01, {R0}(vi00)", f"sq.xyzw vf02, {R1}(vi00)")
    L(f"iaddiu vi11, vi08, {A + 6}", "isubiu vi12, vi09, 2", "nop", "iblez vi12, q3clip_ret", "nop")

    def attributes(st, col):
        """the corners' texture coordinates (ST * w) and colours into the room: st [(the
        three loads, the room's place)], col the colours' loads"""
        for loads, dst in st:
            L(*[f"lq.xyz vf{r:02d}, {a}" for r, a in zip((1, 2, 3), loads)])
            U("mulw.xyz vf01, vf01, vf09w", "mulw.xyz vf02, vf02, vf12w", "mulw.xyz vf03, vf03, vf13w")
            gap(1)
            L(f"sq.xyz vf01, {dst}(vi00)", f"sq.xyz vf02, {dst + 1}(vi00)", f"sq.xyz vf03, {dst + 2}(vi00)")
        L(*[f"lq.xyzw vf{r:02d}, {a}" for r, a in zip((1, 2, 3), col)])
        U("itof0.xyzw vf01, vf01", "itof0.xyzw vf02, vf02", "itof0.xyzw vf03, vf03")
        gap(1)
        L(f"sq.xyzw vf01, {CC}(vi00)", f"sq.xyzw vf02, {CC + 1}(vi00)", f"sq.xyzw vf03, {CC + 2}(vi00)")

    lab("q3clip_scan")
    L("ilw.w vi05, 2(vi11)", "iaddiu vi01, vi00, 0x2000", "nop", "nop", "iand vi01, vi05, vi01",
      "ibgez vi05, q3clip_next", "nop", "ibne vi01, vi00, q3clip_next", "nop")
    # the corners' positions before the divide (vf09, vf12, vf13)
    L(f"lq.xyzw vf09, {FAR + 1 - 6}(vi11)", f"lq.xyzw vf12, {FAR + 1 - 3}(vi11)", f"lq.xyzw vf13, {FAR + 1}(vi11)")
    L("bal vi15, q3cut_test", "nop", "nop", "ibeq vi01, vi00, q3clip_next", "nop")
    attributes([([f"{-6}(vi11)", f"{-3}(vi11)", "0(vi11)"], CSA),
                ([f"{FAR - 6}(vi11)", f"{FAR - 3}(vi11)", f"{FAR}(vi11)"], CSB)],
               [f"{1 - 6}(vi11)", f"{1 - 3}(vi11)", "1(vi11)"])
    L("bal vi15, q3cut_poly", "nop")
    lab("q3clip_next")
    L("iaddiu vi11, vi11, 3", "isubiu vi12, vi12, 1", "nop", "ibne vi12, vi00, q3clip_scan", "nop")
    lab("q3clip_ret")
    L(f"ilw.x vi15, {SAVE}(vi00)", "nop", "nop", "nop", "nop", "jr vi15", "nop")

    # q3mclip: a model's piece (q3mc), after its packet is kicked. vi15 the return. The
    # packet's vertex 3 t + 2 ends triangle t: left out when its w has bit 15; the w's of
    # the three are their vertices' offsets (vi02, vi03, vi04: their places)
    lab("q3mclip")
    L(f"isw.x vi15, {SAVE}(vi00)", f"isw.y vi00, {SAVE}(vi00)", f"lq.xyzw vf01, {CTX['TA3']}(vi00)", "xtop vi08", "nop",
      f"ilw.y vi12, {M_IDX}(vi08)", f"sq.xyzw vf01, {R0}(vi00)", f"iaddiu vi11, vi08, {M_OUT + 6}", "nop",
      "iblez vi12, q3clip_ret", "nop")
    # vi12: the packet's vertices, three a triangle
    lab("q3mclip_scan")
    L("ilw.w vi05, 2(vi11)", "ilw.w vi02, -4(vi11)", "ilw.w vi03, -1(vi11)", "iaddiu vi01, vi00, 0x7fff", "xtop vi07",
      "ibgez vi05, q3mclip_next", f"iaddiu vi07, vi07, {M_VERT}")
    L("iand vi02, vi02, vi01", "iand vi03, vi03, vi01", "iand vi04, vi05, vi01", "iadd vi02, vi02, vi07",
      "iadd vi03, vi03, vi07", "iadd vi04, vi04, vi07", "nop")
    L("lq.xyzw vf09, 3(vi02)", "lq.xyzw vf12, 3(vi03)", "lq.xyzw vf13, 3(vi04)")
    L("bal vi15, q3cut_test", "nop", "nop", "ibeq vi01, vi00, q3mclip_next", "nop")
    attributes([(["0(vi02)", "0(vi03)", "0(vi04)"], CSA)], ["1(vi02)", "1(vi03)", "1(vi04)"])
    L("bal vi15, q3cut_poly", "nop")
    lab("q3mclip_next")
    L("iaddiu vi11, vi11, 9", "isubiu vi12, vi12, 3", "nop", "ibgtz vi12, q3mclip_scan", "nop")
    L("b q3clip_ret", "nop")

    # q3cut_test: the corners' positions before the divide in vf09, vf12, vf13; vi15 the
    # return. vi01 0: nothing of the triangle is seen; else vi08: a bit a plane with a
    # corner outside, the first plane's the highest (for q3cut_poly), and the planes' rows.
    # Keeps vi02 .. vi05, vi07, vi09 .. vi15
    lab("q3cut_test")
    # First against the picture's sides and the window of lines the pass draws (SS, SN:
    # x * SS + w * SN within -w .. w is inside): all three corners outside one of the
    # four and nothing of it is drawn in this pass. A wall beside the eye is across the
    # guard band in every strip's pass, and beside the picture or outside the strip in
    # most; cutting it is hundreds of cycles and three packets.
    L(f"lq.xyz vf17, {CTX['SS']}(vi00)", f"lq.xyz vf18, {CTX['SN']}(vi00)", "nop", "nop", "nop")
    U("mulaw.xyz ACC, vf18, vf09w", "madd.xyz vf14, vf09, vf17",
      "mulaw.xyz ACC, vf18, vf12w", "madd.xyz vf15, vf12, vf17",
      "mulaw.xyz ACC, vf18, vf13w", "madd.xyz vf16, vf13, vf17")
    U("addw.x vf17, vf00, vf09w", "addw.y vf17, vf00, vf12w", "addw.z vf17, vf00, vf13w")
    for r, c in ((18, "x"), (19, "y")):
        U(f"add{c}.x vf{r}, vf00, vf14{c}", f"add{c}.y vf{r}, vf00, vf15{c}", f"add{c}.z vf{r}, vf00, vf16{c}")
    L("iaddiu vi06, vi00, 0xe0")
    for op, src in (("sub", 18), ("add", 18), ("sub", 19), ("add", 19)):
        out.append((f"{op}.xyz vf01, vf17, vf{src}", "nop"))
        gap(3)
        L("fmand vi01, vi06", "nop", "ibeq vi01, vi06, q3cut_out", "nop")
    # scaled into the clip volume (vf14-16)
    L(f"lq.xyz vf17, {CTX['CS']}(vi00)", f"lq.xyz vf18, {CTX['CN']}(vi00)", "nop", "nop", "nop")
    U("mulaw.xyz ACC, vf18, vf09w", "madd.xyz vf14, vf09, vf17",
      "mulaw.xyz ACC, vf18, vf12w", "madd.xyz vf15, vf12, vf17",
      "mulaw.xyz ACC, vf18, vf13w", "madd.xyz vf16, vf13, vf17")
    # by coordinate over the corners: W vf17, X vf18, Y vf19, Z vf20
    U("addw.x vf17, vf00, vf09w", "addw.y vf17, vf00, vf12w", "addw.z vf17, vf00, vf13w")
    for r, c in ((18, "x"), (19, "y"), (20, "z")):
        U(f"add{c}.x vf{r}, vf00, vf14{c}", f"add{c}.y vf{r}, vf00, vf15{c}", f"add{c}.z vf{r}, vf00, vf16{c}")
    L("iaddiu vi06, vi00, 0xe0", "iaddiu vi08, vi00, 0")
    # the planes' rows (a corner's distance, inside from 0 up): w - x, w + x, w - y, w + y,
    # w - z (far), w + z (near). All three outside one: nothing of it is seen
    for p, (op, src) in enumerate((("sub", 18), ("add", 18), ("sub", 19), ("add", 19), ("sub", 20), ("add", 20))):
        out.append((f"{op}.xyz vf01, vf17, vf{src}", "iadd vi08, vi08, vi08"))
        gap(3)
        L("fmand vi01, vi06", f"sq.xyz vf01, {PL + p}(vi00)", "ibeq vi01, vi06, q3cut_out", "nop",
          f"ibeq vi01, vi00, q3clip_in{p}", "nop", "iaddiu vi08, vi08, 1")
        lab(f"q3clip_in{p}")
    L("iaddiu vi01, vi00, 1", "jr vi15", "nop")
    lab("q3cut_out")
    L("iaddiu vi01, vi00, 0", "jr vi15", "nop")

    # q3cut_poly: after q3cut_test said yes and the corners' attributes are in the room
    # (CSA, CC; CSB with two passes); vi15 the return. Keeps vi11, vi12
    lab("q3cut_poly")
    L(f"sq.xyzw vf09, {CP}(vi00)", f"sq.xyzw vf12, {CP + 1}(vi00)", f"sq.xyzw vf13, {CP + 2}(vi00)")
    # the polygon: the triangle, its corners' weights (1, 0, 0), (0, 1, 0), (0, 0, 1).
    # vi03 from, vi04 to, vi05 its vertices, vi10 the plane's row
    L("mr32.xyzw vf03, vf00", "nop", "nop", "nop", "mr32.xyzw vf02, vf03", "nop", "nop", "nop", "mr32.xyzw vf01, vf02",
      "nop", "nop", "nop")
    L(f"sq.xyz vf01, {WA}(vi00)", f"sq.xyz vf02, {WA + 1}(vi00)", f"sq.xyz vf03, {WA + 2}(vi00)")
    L(f"iaddiu vi03, vi00, {WA}", f"iaddiu vi04, vi00, {WB}", "iaddiu vi05, vi00, 3", f"iaddiu vi10, vi00, {PL}")

    lab("q3clip_plane")
    L("iaddiu vi01, vi00, 0x20", "iand vi01, vi08, vi01", "nop", "ibeq vi01, vi00, q3clip_noplane", "nop")
    # the row's three distances into x of vf14, vf15, vf16
    L("lq.xyz vf14, 0(vi10)", "nop", "nop", "nop", "mr32.xyzw vf15, vf14", "nop", "nop", "nop",
      "mr32.xyzw vf16, vf15")
    # the vertex before the first: the last (vf03 its weights, vf04 x its distance, vi13 outside)
    L("iadd vi07, vi03, vi05", "isubiu vi07, vi07, 1", "nop", "nop", "lq.xyz vf03, 0(vi07)", "nop", "nop", "nop")
    U("mulax.x ACC, vf14, vf03x", "madday.x ACC, vf15, vf03y", "maddz.x vf04, vf16, vf03z")
    gap(3)
    L("fsand vi13, 2", "iaddiu vi07, vi03, 0", "iaddiu vi01, vi04, 0", "iaddiu vi06, vi00, 0", "iaddiu vi02, vi05, 0")
    lab("q3clip_edge")
    L("lq.xyz vf01, 0(vi07)", "nop", "nop", "nop")
    U("mulax.x ACC, vf14, vf01x", "madday.x ACC, vf15, vf01y", "maddz.x vf02, vf16, vf01z")
    gap(3)
    L("fsand vi09, 2", "nop", "ibeq vi09, vi13, q3clip_same", "nop")
    # the plane is between them: vf05, vf06 the inside one, vf07, vf08 the other
    L("ibne vi13, vi00, q3clip_ainv", "nop")
    L("move.xyz vf05, vf03", "move.x vf06, vf04", "move.xyz vf07, vf01", "b q3clip_cut", "move.x vf08, vf02")
    lab("q3clip_ainv")
    L("move.xyz vf05, vf01", "move.x vf06, vf02", "move.xyz vf07, vf03", "move.x vf08, vf04")
    lab("q3clip_cut")
    # at most 9 vertices (a triangle cut by six planes has no more; rounding could make one)
    L("isubiu vi14, vi06, 9", "nop", "ibgez vi14, q3clip_same", "nop")
    gap(2)
    U("sub.x vf08, vf06, vf08", "sub.xyz vf07, vf07, vf05")
    gap(3)
    L("div Q, vf06x, vf08x", "waitq")
    U("mulq.xyz vf07, vf07, Q", "add.xyz vf07, vf07, vf05")
    gap(3)
    L("sq.xyz vf07, 0(vi01)", "iaddiu vi01, vi01, 1", "iaddiu vi06, vi06, 1")
    lab("q3clip_same")
    L("isubiu vi14, vi06, 9", "nop", "ibne vi09, vi00, q3clip_gone", "nop", "ibgez vi14, q3clip_gone", "nop")
    L("sq.xyz vf01, 0(vi01)", "iaddiu vi01, vi01, 1", "iaddiu vi06, vi06, 1")
    lab("q3clip_gone")
    L("move.xyz vf03, vf01", "move.x vf04, vf02", "iaddiu vi13, vi09, 0", "iaddiu vi07, vi07, 1",
      "isubiu vi02, vi02, 1", "nop", "ibne vi02, vi00, q3clip_edge", "nop")
    # the next plane cuts what this one made
    L("iaddiu vi05, vi06, 0", "iaddiu vi14, vi03, 0", "iaddiu vi03, vi04, 0", "iaddiu vi04, vi14, 0",
      "isubiu vi14, vi05, 3", "nop", "ibltz vi14, q3cut_done", "nop")
    lab("q3clip_noplane")
    L("iadd vi08, vi08, vi08", "iaddiu vi10, vi10, 1", f"isubiu vi14, vi10, {PL + 6}", "nop",
      "ibne vi14, vi00, q3clip_plane", "nop")

    # the polygon as a fan: (0, k, k + 1), a triangle a packet. vi06 vertex k, vi02 left
    L("iaddiu vi06, vi03, 1", "isubiu vi02, vi05, 2")
    lab("q3clip_tri")
    L("iaddiu vi07, vi03, 0", "bal vi13, q3clip_put", f"iaddiu vi01, vi00, {R0 + 1}")
    L("iaddiu vi07, vi06, 0", "bal vi13, q3clip_put", f"iaddiu vi01, vi00, {R0 + 4}")
    L("iaddiu vi07, vi06, 1", "bal vi13, q3clip_put", f"iaddiu vi01, vi00, {R0 + 7}")
    L(f"ilw.y vi14, {SAVE}(vi00)", f"iaddiu vi01, vi00, {R0}", f"iaddiu vi07, vi00, {R1}",
      f"iaddiu vi10, vi00, {CTX['NOP']}", "nop", "xgkick vi01", "ibeq vi14, vi00, q3clip_one", "nop", "xgkick vi07")
    lab("q3clip_one")
    # the packet that sets nothing: this XGKICK waits until the ones before have left R0 and R1
    L("xgkick vi10", "iaddiu vi06, vi06, 1", "isubiu vi02, vi02, 1", "nop", "ibne vi02, vi00, q3clip_tri", "nop")

    lab("q3cut_done")
    L("jr vi15", "nop")

    # the polygon's vertex at vi07 (its weights) into the packets at vi01; keeps vi02, vi03, vi06
    lab("q3clip_put")
    L("lq.xyz vf01, 0(vi07)")
    for at, lanes, dst in ((CP, "xyzw", 5), (CSA, "xyz", 6), (CC, "xyzw", 7), (CSB, "xyz", 8)):
        L(f"lq.{lanes} vf02, {at}(vi00)", f"lq.{lanes} vf03, {at + 1}(vi00)", f"lq.{lanes} vf04, {at + 2}(vi00)", "nop")
        U(f"mulax.{lanes} ACC, vf02, vf01x", f"madday.{lanes} ACC, vf03, vf01y", f"maddz.{lanes} vf{dst:02d}, vf04, vf01z")
        gap(1)
    gap(3)
    L("div Q, vf00w, vf05w", "waitq")
    U("mulq.xyz vf05, vf05, Q", "mulq.xyz vf06, vf06, Q", "mulq.xyz vf08, vf08, Q", "ftoi0.xyzw vf07, vf07",
      "ftoi4.xyz vf05, vf05")
    L("sq.xyz vf06, 0(vi01)", "sq.xyz vf08, 10(vi01)", "sq.xyzw vf07, 1(vi01)", "sq.xyzw vf07, 11(vi01)",
      "mfir.w vf05, vi00", "nop", "nop", "nop", "sq.xyzw vf05, 2(vi01)", "sq.xyzw vf05, 12(vi01)", "jr vi13", "nop")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--maxsdk", default=os.path.join(HERE, "../../../libs/maxsdk"))
    ap.add_argument("--out", default=os.path.join(HERE, "../code/renderer_ps2/vu"))
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(a.maxsdk, "tools/vusched"))
    import vusched as vs

    lines = ["; q3vu: the PS2 renderer's VU1 programs. Generated by tools/ps2vu.py (its head says what",
             "; they do; the loops are scheduled by maxsdk's tools/vusched) -- do not edit.",
             '\t.section .vutext, "ax", @progbits', "\t.globl q3vu_vu", "q3vu_vu:"]
    defs, word, progs = [], 0, []
    worst_pad = worst_slop = 0
    for kern, iis in kernels(vs):
        two = kern.name.startswith("q3w2")
        s, regs, words, pad, slop = schedule(vs, kern, iis, a.v)
        print(f"{kern.name}: {kern.u} vertices in {kern.ii} cycles ({kern.ii / kern.u:g} a vertex), {s.stages} stages, "
              f"pad {pad}, slop {slop}, {len(set(regs.values()))} VF registers")
        if kern.name in ("q3m", "q3mc", "q3me", "q3mec"):
            prog, mpad, mslop = program_m(vs, kern, s, regs, words)
            # its stray stores stay between the entries and the buffer's end
            assert M_OUT + 9 * M_TRIS <= M_IDX and M_IDX + 1 + M_TRIS + mpad <= M_VERT
            assert M_VERT + 4 * M_VERTS + mslop <= BUF_QW, f"q3m: slop {mslop} leaves the buffer"
            progs.append([("comment", f"{kern.name}: a vertex in {kern.ii} cycles, {s.stages} stages; then the gather")] + prog)
            continue
        worst_pad, worst_slop = max(worst_pad, pad), max(worst_slop, slop)
        progs.append([("comment", f"{kern.name}: {kern.u} vertices in {kern.ii} cycles, {s.stages} stages")] +
                     program(vs, kern, s, regs, words, two, kern.name.endswith("c")))
    progs.append([("comment", "q3clip, q3mclip: what is left of the triangles the programs that cut left out")] +
                 clipper())
    for prog in progs:
        for up, low in prog:
            if up == "comment":
                lines.append(f"; {low}")
                continue
            if up == "label":
                lines.append(f"{low}:")
                if low.endswith(("_setup", "_batch")):
                    defs.append((low.upper(), word))
                continue
            lo_t, _, lo_c = low.partition(";")
            up_t, _, up_c = up.partition(";")
            com = "; ".join(x.strip() for x in (up_c, lo_c) if x.strip())
            lines.append(f"\t{up_t.strip():<32}{lo_t.strip():<34}" + (f"; {com}" if com else ""))
            word += 1
    lines += ["\t.globl q3vu_vu_end", "q3vu_vu_end:", ""]
    A, B, n = ps2map.VU_A, ps2map.VU_B, ps2map.BATCH
    # what the loops store outside their vertices must miss everything else
    assert worst_pad <= A - 1, f"pad {worst_pad}: VU_A too low"
    IB = ps2map.VU_IB
    assert A + STRIDE * n + max(worst_slop, worst_pad) <= B - 1, "the first pass's slop reaches the second's tag"
    assert B + STRIDE * n + worst_slop <= IB, "the last pass's slop reaches the lightmap's coordinates"
    assert IB + STRIDE * n <= BUF_QW, "the lightmap's coordinates leave the buffer"
    assert CTX_QW + 2 * BUF_QW <= 1024 and word <= 2048
    h = ["/* Generated by tools/ps2vu.py -- do not edit. The PS2 renderer's VU1 programs (q3vu.vsm):",
         "   entries (instruction words from q3vu_vu) and VU1's data memory (quadwords). */",
         "#ifndef Q3VU_H", "#define Q3VU_H", "",
         "extern const unsigned char q3vu_vu[], q3vu_vu_end[];", ""]
    h += [f"#define {d} {w}" for d, w in defs]
    h += ["", f"#define Q3VU_CTX_QW {CTX_QW} /* the context, at 0 */"]
    h += [f"#define Q3VU_CTX_{g} {q}" for g, q in CTX.items()]
    h += [f"#define Q3VU_ROOM_{g} {q}" for g, q in ROOM.items()]
    h += [f"#define Q3VU_M_{g} {q} /* q3m's uniforms */" for g, q in MCTX.items()]
    h += [f"#define Q3VU_M_{g} {q} /* q3me's more */" for g, q in ECTX.items()]
    h += [f"#define Q3VU_T_{g} {q} /* q3w1t's */" for g, q in TCTX.items()]
    h += [f"#define Q3VU_D_{g} {q} /* q3wd's */" for g, q in DCTX.items()]
    h += [f"#define Q3VU_M_OUT {M_OUT}", f"#define Q3VU_M_IDX {M_IDX}", f"#define Q3VU_M_VERT {M_VERT}",
          f"#define Q3VU_M_VERTS {M_VERTS} /* a piece's vertices at most */",
          f"#define Q3VU_M_TRIS {M_TRIS} /* and its triangles */"]
    h += [f"#define Q3VU_BASE {CTX_QW}", f"#define Q3VU_BUF {BUF_QW}", f"#define Q3VU_A {A}", f"#define Q3VU_B {B}",
          f"#define Q3VU_IB {IB}",
          f"#define Q3VU_BATCH {n}",
          f"#define Q3VU_REGS_QW {A - 1} /* a set-up's packet at most */",
          "", "#endif", ""]
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "q3vu.vsm"), "w") as f:
        f.write("\n".join(lines))
    with open(os.path.join(a.out, "q3vu.h"), "w") as f:
        f.write("\n".join(h))
    print(f"q3vu: {word} words; pad {worst_pad}, slop {worst_slop}")


if __name__ == "__main__":
    main()
