#!/usr/bin/env python3
"""tools/ps2load.py <quake3.load>: the load of the PS2's units over a timedemo.

The file is written at the end of a run with ps2_load <samples a second>
(code/ps2/ps2_load.c): what the EE ran and what VIF1, VU1 and the GS did at
each sample, counted by the beam's line, and the samples themselves of the
frames from ps2_loadraw. Prints each unit's share of the time, the shares
along a field (where in the picture the drawing units work), and the
kernel's places with the most samples."""
import sys

EE = ["the game", "the renderer", "the kernel (game)", "the kernel (renderer)", "asleep (game)", "asleep (renderer)"]

def read(path):
    d = {"ph": [], "eeu": {}, "kpc": [], "raw": []}
    raw = False
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if raw:
            if p[0] == "end":
                break
            d["raw"] += [int(x, 16) for x in p]
        elif p[0] == "ph":
            i = p.index(":")
            d["ph"].append(([int(x) for x in p[2:i]], [int(x) for x in p[i + 1:]]))
        elif p[0] == "eeu":
            d["eeu"][int(p[1])] = [int(x) for x in p[2:]]
        elif p[0] == "kpc":
            d["kpc"].append((int(p[1], 16), int(p[2])))
        elif p[0] == "raw":
            raw = True
        else:
            d[p[0]] = p[1:]
    return d

def main():
    d = read(sys.argv[1])
    n = int(d["samples"][0])
    lines = int(d["lines"][0])
    print(f"{n} samples, one every {d['period_us'][0]} us; the strips start at lines {' '.join(d['strips'][1:])}")
    # a unit state's number: 1 VIF1's DMA (a pass is sent), 2 VU1 runs, 4 VU1 waits at an XGKICK, 8 the GS's FIFO is not empty
    u = [sum(d["eeu"][c][i] for c in d["eeu"]) for i in range(16)]
    share = lambda bit: 100.0 * sum(u[i] for i in range(16) if i & bit) / n
    print("the EE:")
    for c in sorted(d["eeu"]):
        print(f"  {100.0 * sum(d['eeu'][c]) / n:5.1f} %  {EE[c]}")
    wait = share(4)
    print(f"VU1: runs {share(2) - wait:.1f} %, waits for the GS at an XGKICK {wait:.1f} %, idle {100 - share(2):.1f} %")
    print(f"the GS: its FIFO not empty {share(8):.1f} %; a pass being sent (VIF1's DMA) {share(1):.1f} %; all of them idle {100.0 * u[0] / n:.1f} %")
    print(f"VU0's micro program: {100.0 * int(d['units'][5]) / n:.1f} %")
    print("\nalong a field (the beam's line since the vblank's interrupt), % of the samples there:")
    print("  line  EE asleep  pass sent  VU1 runs  VU1 waits  GS busy")
    for i, (ee, un) in enumerate(d["ph"]):
        t = sum(ee)
        if t * 400 < n:
            continue
        s = lambda bit: 100.0 * sum(un[k] for k in range(16) if k & bit) / t
        print(f"  {i * lines:4d}  {100.0 * (ee[4] + ee[5]) / t:9.1f}  {s(1):9.1f}  {s(2) - s(4):8.1f}  {s(4):9.1f}  {s(8):7.1f}")
    print("\nthe kernel's places (but its idle loop at " + d["idle"][0] + "):")
    for pc, k in sorted(d["kpc"], key=lambda x: -x[1])[:8]:
        print(f"  {pc:08x}  {100.0 * k / n:4.1f} %")
    print(f"\n{len(d['raw'])} samples of the frames themselves (a word each: the line in bits 0-10, the EE's state in 11-13,")
    print("mx_trace_units' bits in 14-23, VIF1's STAT in 24-27, the demo's frame in 28-31)")

if __name__ == "__main__":
    main()
