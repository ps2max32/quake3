#!/usr/bin/env python3
"""The PS2 renderer's VU1 programs (PLAN-PS2.md, 5.4): their loops scheduled by
maxsdk's tools/vusched, written as code/renderer_ps2/vu/q3vu.vsm and q3vu.h.

    ps2vu.py [--maxsdk DIR] [--out code/renderer_ps2/vu] [-v]

VU1 data memory (quadwords): the context at 0 (Q3VU_CTX_*: what a draw's
programs read once, at their set-up), then two buffers of Q3VU_BUF (VIF1's
double buffering, BASE Q3VU_BASE, OFFSET Q3VU_BUF). A buffer, from its start:

    0 ..          a set-up's GS registers (an A+D packet, kicked by the set-up)
    VU_A - 1      the first pass's GIF tag, written by the program
    VU_A + 3 i    vertex i of the first pass: ST, RGBAQ, XYZ2 in place (the VIF
                  put the texture coordinates, the colour and the position
                  there: tools/ps2map.py)
    VU_B - 1      the second pass's GIF tag
    VU_B + 3 i    vertex i of the second pass: the lightmap's coordinates in,
                  ST out; RGBAQ as it lies (a decal: the GS does not read it,
                  only takes Q at it); XYZ2 as the first pass's

Each program has a set-up entry (MSCAL: the context into registers, the GS
registers kicked) and ends before its batch entry, so every batch after it is
an MSCNT with the vertex count as ITOP: the map's chunks hold no program
address. A batch's count is even (the loops take two vertices a turn).

Programs:
    w2   texture and lightmap from one vertex: the vertex is transformed once
         and written twice (the lightmap in GS context 2)
    w1   one pass
    w2c, w1c   the same with a test of each vertex against the guard band and
         the depth planes (CLIPW of the position scaled into that volume): a
         triangle with a vertex outside is not drawn (ADC). For chunks across
         a plane, until they are cut on VU1.
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
CTX_QW = 32
BUF_QW = 496
CTX = {"C0": 0, "C1": 1, "C2": 2, "C3": 3, "TA": 4, "TB": 5, "CS": 6, "CN": 7}


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


def program(vs, kern, s, regs, words, two):
    name = kern.name
    used = set(regs.values())
    free = [f"vf{r:02d}" for r in range(1, 32) if f"vf{r:02d}" not in used]
    ta, tb = free[0], free[1]
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
    lo("nop", e=True)
    lo("nop")
    lo(f"b {name}_batch")
    lo("nop")
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
    defs, word = [], 0
    worst_pad = worst_slop = 0
    for kern, iis in kernels(vs):
        two = kern.name.startswith("q3w2")
        s, regs, words, pad, slop = schedule(vs, kern, iis, a.v)
        worst_pad, worst_slop = max(worst_pad, pad), max(worst_slop, slop)
        print(f"{kern.name}: {kern.u} vertices in {kern.ii} cycles ({kern.ii / kern.u:g} a vertex), {s.stages} stages, "
              f"pad {pad}, slop {slop}, {len(set(regs.values()))} VF registers")
        lines.append(f"; {kern.name}: {kern.u} vertices in {kern.ii} cycles, {s.stages} stages")
        for up, low in program(vs, kern, s, regs, words, two):
            if up == "label":
                lines.append(f"{low}:")
                if not low.endswith("_loop"):
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
