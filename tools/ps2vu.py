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
    VU_A + 3 i    vertex i of the first pass: ST, RGBAQ, XYZ2 in place (the VIF
                  put the texture coordinates, the colour and the position
                  there: tools/ps2map.py)
    VU_B - 1      the second pass's GIF tag
    VU_B + 3 i    vertex i of the second pass: the lightmap's coordinates in,
                  ST out; RGBAQ: the GS does not read it (a decal; it only
                  takes Q at it), so the programs that cut keep the vertex's
                  position before the divide there; XYZ2 as the first pass's

Each program has a set-up entry (MSCAL: the context into registers, the GS
registers kicked) and ends before its batch entry, so every batch after it is
an MSCNT with the vertex count as ITOP: the map's chunks hold no program
address. A batch's count is even (the loops take two vertices a turn).

Programs:
    w2   texture and lightmap from one vertex: the vertex is transformed once
         and written twice (the lightmap in GS context 2)
    w1   one pass
    w2c, w1c   the same for chunks across the near plane or the guard band:
         each vertex is tested against the guard band and the depth planes
         (CLIPW of the position scaled into that volume) and a triangle with
         a vertex outside is left out of the strip (ADC). After the batch's
         strips are kicked, q3clip cuts those triangles and sends what is
         left of each as triangles of its own.

q3clip (the end of this file): a triangle left out is found by its vertex's w
(ADC set, tools/ps2map.py's bit 13 clear). Its three positions before the
divide are in the second pass's RGBAQ slots; the texture coordinates come back
from ST * w, the colours are still integers. The polygon is cut in the
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
CTX_QW = 72
BUF_QW = 476
# the context: the matrix, the clip test's scale and offset (a frame's); a material's GIF tags
# (strips; TA3, TB3: single triangles, for q3clip); NOP: a packet that sets nothing; then
# q3clip's room: PL the six planes, CO the corners' attributes (position, texture ST, colour,
# lightmap ST: three each), WA and WB the polygon (9 vertices), R0 and R1 a triangle's
# packets, SAVE integers
CTX = {"C0": 0, "C1": 1, "C2": 2, "C3": 3, "CS": 4, "CN": 5, "TA": 6, "TB": 7, "TA3": 8, "TB3": 9, "NOP": 10}
ROOM = {"PL": 12, "CO": 18, "WA": 30, "WB": 39, "R0": 48, "R1": 58, "SAVE": 68}
# q3clip's registers: the loops' uniforms and tags are kept out of them
CLIP_VF = set(range(1, 10)) | set(range(12, 26))


def kernels(vs):
    Op, Kernel = vs.Op, vs.Kernel

    def pos(clip, two):
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
            ops.append(Op("lqadc", "L", "lq.w {O}, {off}({p})", ("O", "w"), mem=("lq", 2)))
        ops.append(Op("sqpos", "L", "sq.xyzw {O}, {off}({p})", None, [("O", "xyzw")], mem=("sq", 2)))
        if two:
            ops.append(Op("sqposb", "L", "sq.xyzw {O}, {off}({p})", None, [("O", "xyzw")], mem=("sq", FAR + 2)))
        return ops

    def st(tag, slot):
        return [
            Op("lqst" + tag, "L", "lq.xyz {S%s}, {off}({p})" % tag, ("S" + tag, "xyz"), mem=("lq", slot)),
            Op("itofst" + tag, "U", "itof12.xyz {I%s}, {S%s}" % (tag, tag), ("I" + tag, "xyz"), [("S" + tag, "xyz")]),
            Op("mulst" + tag, "U", "mulq.xyz {M%s}, {I%s}, Q" % (tag, tag), ("M" + tag, "xyz"),
               [("I" + tag, "xyz"), ("Q", "")]),
            Op("sqst" + tag, "L", "sq.xyz {M%s}, {off}({p})" % tag, None, [("M" + tag, "xyz")], mem=("sq", slot)),
        ]

    mvp = [(g, CTX[g]) for g in ("C0", "C1", "C2", "C3")]
    clipu = [(g, CTX[g]) for g in ("CS", "CN")]

    def k(name, clip, two, u):
        ops = lambda c: pos(clip, two) + st("a", 0) + (st("b", FAR) if two else [])
        return Kernel(name, ops, u, 0, STRIDE, mvp + (clipu if clip else []), {})

    # name, the kernel, the IIs to try
    return [
        (k("q3w2", False, True, 2), range(20, 33)),
        (k("q3w1", False, False, 2), range(14, 25)),
        (k("q3w2c", True, True, 1), range(14, 25)),
        (k("q3w1c", True, False, 1), range(12, 23)),
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
                pad = max(pad, (i.c // s.ii * kern.u - i.k) * STRIDE - slot)
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
    lo(f"mfir.x {ta}, vi10")
    if two:
        lo(f"mfir.x {tb}, vi10")
    lo(f"iaddiu vi07, vi08, {A - 1}")
    if two:
        lo(f"iaddiu vi06, vi08, {B - 1}")
    lo("nop")
    lo(f"sq.xyzw {ta}, {A - 1}(vi08)")
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
    lo("nop", e=True)
    lo("nop")
    lo(f"b {name}_batch")
    lo("nop")
    return out


def clipper():
    """q3clip (this file's head): vi08 the buffer, vi09 its vertices, vi14 1: two passes,
    vi15 the return. An instruction a word, but where a pair is plainly independent."""
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
    L(f"lq.xyzw vf01, {CTX['TA3']}(vi00)", f"lq.xyzw vf02, {CTX['TB3']}(vi00)")
    L(f"lq.xyz vf24, {CTX['CS']}(vi00)", f"lq.xyz vf25, {CTX['CN']}(vi00)")
    # the weights of the three corners: vf21 (1, 0, 0), vf22 (0, 1, 0), vf23 (0, 0, 1)
    L("mr32.xyzw vf23, vf00", f"sq.xyzw vf01, {R0}(vi00)", f"sq.xyzw vf02, {R1}(vi00)", "nop",
      "mr32.xyzw vf22, vf23", "nop", "nop", "nop", "mr32.xyzw vf21, vf22")
    L(f"iaddiu vi11, vi08, {A + 6}", "isubiu vi12, vi09, 2", "nop", "iblez vi12, q3clip_ret", "nop")

    lab("q3clip_scan")
    L("ilw.w vi05, 2(vi11)", "iaddiu vi01, vi00, 0x2000", "nop", "nop", "iand vi01, vi05, vi01",
      "ibgez vi05, q3clip_next", "nop", "ibne vi01, vi00, q3clip_next", "nop")
    # the corners' positions before the divide (vf09, vf12, vf13), scaled into the clip volume (vf14-16)
    L(f"lq.xyzw vf09, {FAR + 1 - 6}(vi11)", f"lq.xyzw vf12, {FAR + 1 - 3}(vi11)", f"lq.xyzw vf13, {FAR + 1}(vi11)")
    U("mulaw.xyz ACC, vf25, vf09w", "madd.xyz vf14, vf09, vf24",
      "mulaw.xyz ACC, vf25, vf12w", "madd.xyz vf15, vf12, vf24",
      "mulaw.xyz ACC, vf25, vf13w", "madd.xyz vf16, vf13, vf24")
    # by coordinate over the corners: W vf17, X vf18, Y vf19, Z vf20
    U("addw.x vf17, vf00, vf09w", "addw.y vf17, vf00, vf12w", "addw.z vf17, vf00, vf13w")
    for r, c in ((18, "x"), (19, "y"), (20, "z")):
        U(f"add{c}.x vf{r}, vf00, vf14{c}", f"add{c}.y vf{r}, vf00, vf15{c}", f"add{c}.z vf{r}, vf00, vf16{c}")
    L("iaddiu vi06, vi00, 0xe0", "iaddiu vi08, vi00, 0")
    # the planes' rows (a corner's distance, inside from 0 up): w - x, w + x, w - y, w + y,
    # w - z (far), w + z (near). All three outside one: nothing of it is seen. vi08: a bit a
    # plane with a corner outside, the first plane's the highest
    for p, (op, src) in enumerate((("sub", 18), ("add", 18), ("sub", 19), ("add", 19), ("sub", 20), ("add", 20))):
        out.append((f"{op}.xyz vf01, vf17, vf{src}", "iadd vi08, vi08, vi08"))
        gap(3)
        L("fmand vi01, vi06", f"sq.xyz vf01, {PL + p}(vi00)", "ibeq vi01, vi06, q3clip_next", "nop",
          f"ibeq vi01, vi00, q3clip_in{p}", "nop", "iaddiu vi08, vi08, 1")
        lab(f"q3clip_in{p}")
    # the corners' attributes: position, texture ST (ST * w), colour, lightmap ST
    L(f"sq.xyzw vf09, {CP}(vi00)", f"sq.xyzw vf12, {CP + 1}(vi00)", f"sq.xyzw vf13, {CP + 2}(vi00)")
    for src, dst in ((0, CSA), (FAR, CSB)):
        L(f"lq.xyz vf01, {src - 6}(vi11)", f"lq.xyz vf02, {src - 3}(vi11)", f"lq.xyz vf03, {src}(vi11)")
        U("mulw.xyz vf01, vf01, vf09w", "mulw.xyz vf02, vf02, vf12w", "mulw.xyz vf03, vf03, vf13w")
        gap(1)
        L(f"sq.xyz vf01, {dst}(vi00)", f"sq.xyz vf02, {dst + 1}(vi00)", f"sq.xyz vf03, {dst + 2}(vi00)")
    L(f"lq.xyzw vf01, {1 - 6}(vi11)", f"lq.xyzw vf02, {1 - 3}(vi11)", f"lq.xyzw vf03, 1(vi11)")
    U("itof0.xyzw vf01, vf01", "itof0.xyzw vf02, vf02", "itof0.xyzw vf03, vf03")
    gap(1)
    L(f"sq.xyzw vf01, {CC}(vi00)", f"sq.xyzw vf02, {CC + 1}(vi00)", f"sq.xyzw vf03, {CC + 2}(vi00)")
    # the polygon: the triangle. vi03 from, vi04 to, vi05 its vertices, vi10 the plane's row
    L(f"sq.xyz vf21, {WA}(vi00)", f"sq.xyz vf22, {WA + 1}(vi00)", f"sq.xyz vf23, {WA + 2}(vi00)")
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
      "isubiu vi14, vi05, 3", "nop", "ibltz vi14, q3clip_next", "nop")
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

    lab("q3clip_next")
    L("iaddiu vi11, vi11, 3", "isubiu vi12, vi12, 1", "nop", "ibne vi12, vi00, q3clip_scan", "nop")
    lab("q3clip_ret")
    L(f"ilw.x vi15, {SAVE}(vi00)", "nop", "nop", "nop", "nop", "jr vi15", "nop")

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
        worst_pad, worst_slop = max(worst_pad, pad), max(worst_slop, slop)
        print(f"{kern.name}: {kern.u} vertices in {kern.ii} cycles ({kern.ii / kern.u:g} a vertex), {s.stages} stages, "
              f"pad {pad}, slop {slop}, {len(set(regs.values()))} VF registers")
        progs.append([("comment", f"{kern.name}: {kern.u} vertices in {kern.ii} cycles, {s.stages} stages")] +
                     program(vs, kern, s, regs, words, two, kern.name.endswith("c")))
    progs.append([("comment", "q3clip: what is left of the triangles q3w2c and q3w1c left out of a batch")] +
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
    assert A + STRIDE * n + max(worst_slop, worst_pad) <= B - 1, "the first pass's slop reaches the second's tag"
    assert B + STRIDE * n + worst_slop <= BUF_QW, "the second pass's slop leaves the buffer"
    assert CTX_QW + 2 * BUF_QW <= 1024 and word <= 2048
    h = ["/* Generated by tools/ps2vu.py -- do not edit. The PS2 renderer's VU1 programs (q3vu.vsm):",
         "   entries (instruction words from q3vu_vu) and VU1's data memory (quadwords). */",
         "#ifndef Q3VU_H", "#define Q3VU_H", "",
         "extern const unsigned char q3vu_vu[], q3vu_vu_end[];", ""]
    h += [f"#define {d} {w}" for d, w in defs]
    h += ["", f"#define Q3VU_CTX_QW {CTX_QW} /* the context, at 0 */"]
    h += [f"#define Q3VU_CTX_{g} {q}" for g, q in CTX.items()]
    h += [f"#define Q3VU_ROOM_{g} {q}" for g, q in ROOM.items()]
    h += [f"#define Q3VU_BASE {CTX_QW}", f"#define Q3VU_BUF {BUF_QW}", f"#define Q3VU_A {A}", f"#define Q3VU_B {B}",
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
