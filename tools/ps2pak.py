#!/usr/bin/env python3
"""Quake 3's textures as 8-bit (or 4-bit) paletted textures for the PS2.

    ps2pak.py OUT.pk3 BASEQ3_DIR [--max 128] [--dirs textures,models] [-j N]

Reads the pak*.pk3 of BASEQ3_DIR in order (a later pak's file replaces an
earlier one's; a .tga is taken before a .jpg of the same name, as the renderer
does), and writes for each image under --dirs a <name>.p8 into OUT.pk3. The
renderer (tr_image.c, PS2) loads a .p8 for a mipmapped image before the
original, and uploads it with one glCompressedTexImage2D: libgl keeps it as
T8 with the texture's own CLUT, so GS memory and the core copy take a byte a
texel (16 bits before).

Each image is made the size the renderer would (sides rounded down to powers
of two: r_roundImagesDown 1), then halved on both sides until it fits --max
(GL_MAX_TEXTURE_SIZE). Its own palette (no dithering; libimagequant's of the
colours, with alpha refined by k-means in RGBA; exact when it has 256
colours or fewer) comes from that level; the levels below are
made from the full-colour level (2 x 2 box, as R_MipMap) and given the
nearest palette entry. 16 colours or fewer: a 4-bit palette.

.p8 layout (little-endian):
    char  magic[4]   "P8T1"
    u16   width, height        the image's own size (image_t width/height)
    u16   pw, ph               the sides rounded to powers of two (picmip
                               halves these, as Upload32)
    u16   w0, h0               the size of the first level stored
    u16   levels               levels stored (w0 x h0 down to 1 x 1)
    u16   pad
    u32   format               GL_PALETTE8_RGB8_OES, _RGBA8, PALETTE4_*
    then the glCompressedTexImage2D blob: the palette (16 or 256 entries,
    3 or 4 bytes each) and the levels' indices (4-bit: two a byte, the
    first in the high bits; each level whole bytes).
"""
import argparse
import io
import os
import struct
import sys
import zipfile
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

GL_PALETTE4_RGB8_OES = 0x8B90
GL_PALETTE4_RGBA8_OES = 0x8B91
GL_PALETTE8_RGB8_OES = 0x8B95
GL_PALETTE8_RGBA8_OES = 0x8B96


def floor_pow2(n):
    p = 1
    while p * 2 <= n:
        p *= 2
    return p


def mip(a):
    """one 2 x 2 box level of an h x w x 4 uint8 array (a side of 1 stays)"""
    h, w = a.shape[:2]
    a = a.astype(np.uint16)
    if h > 1:
        a = (a[0::2] + a[1::2]) >> 1
    if w > 1:
        a = (a[:, 0::2] + a[:, 1::2]) >> 1
    return a.astype(np.uint8)


def nearest(pixels, pal):
    """index of the nearest palette entry for each of n x c pixels"""
    p = pixels.astype(np.int32)
    q = pal.astype(np.int32)
    out = np.empty(len(p), np.uint8)
    for i in range(0, len(p), 4096):
        d = ((p[i:i + 4096, None, :] - q[None, :, :]) ** 2).sum(-1)
        out[i:i + 4096] = d.argmin(1)
    return out


def palette(a, alpha):
    """256 colours for the h x w x 4 texels a: libimagequant's of their
    colours; with alpha, those with alpha added by k-means in RGBA
    (libimagequant's RGBA drops the colour of texels of alpha 0, which
    additive blends show)"""
    q = Image.fromarray(np.ascontiguousarray(a[..., :3]), "RGB")
    q = q.quantize(256, method=Image.Quantize.LIBIMAGEQUANT, dither=Image.Dither.NONE)
    pal = np.array(q.getpalette(rawmode="RGB"), np.uint8).reshape(-1, 3)[:256]
    if not alpha:
        return pal
    pal = np.hstack([pal, np.full((len(pal), 1), 128)]).astype(np.float64)
    px = a.reshape(-1, 4)
    for _ in range(12):
        idx = nearest(px, pal)
        sums = np.zeros((len(pal), 4))
        np.add.at(sums, idx, px)
        cnt = np.bincount(idx, minlength=len(pal))
        used = cnt > 0
        pal[used] = sums[used] / cnt[used, None]
    return np.clip(np.rint(pal), 0, 255).astype(np.uint8)


def convert(data, maxside):
    im = Image.open(io.BytesIO(data))
    im.load()
    rgba = np.asarray(im.convert("RGBA"))
    width, height = im.size
    alpha = bool((rgba[..., 3] != 255).any())
    pw, ph = floor_pow2(width), floor_pow2(height)
    w0, h0 = pw, ph
    while w0 > maxside or h0 > maxside:
        w0, h0 = max(1, w0 // 2), max(1, h0 // 2)
    # down to w0 x h0 the way the renderer goes: a resample to the powers of
    # two, then 2 x 2 levels
    img = Image.fromarray(rgba, "RGBA")
    if (pw, ph) != (width, height):
        img = img.resize((pw, ph), Image.Resampling.BOX)
    a = np.asarray(img)
    while a.shape[1] > w0 or a.shape[0] > h0:
        a = mip(a)
    chans = 4 if alpha else 3
    flat = a.reshape(-1, 4)[:, :chans]
    uniq = np.unique(flat, axis=0)
    if len(uniq) <= 256:
        pal = uniq
    else:
        pal = palette(a, alpha)
    bits = 4 if len(pal) <= 16 else 8
    n = 16 if bits == 4 else 256
    full = np.zeros((n, chans), np.uint8)
    full[:len(pal)] = pal
    levels = []
    lv = a
    while True:
        idx = nearest(lv.reshape(-1, 4)[:, :chans], pal)
        if bits == 4:
            if len(idx) & 1:
                idx = np.append(idx, 0)
            idx = (idx[0::2] << 4 | idx[1::2]).astype(np.uint8)
        levels.append(idx.tobytes())
        if lv.shape[0] == 1 and lv.shape[1] == 1:
            break
        lv = mip(lv)
    # level 0's error against the full-colour texels, and that of 16 bits
    # (5 bits a channel: what libgl kept before)
    rec = full[nearest(a.reshape(-1, 4)[:, :chans], pal)].astype(np.float64)
    src = a.reshape(-1, 4)[:, :chans].astype(np.float64)
    mse8 = ((rec - src) ** 2).mean()
    mse16 = ((((a.reshape(-1, 4)[:, :3] >> 3) << 3 | a.reshape(-1, 4)[:, :3] >> 5) - src[:, :3]) ** 2).mean()
    fmt = {(4, 3): GL_PALETTE4_RGB8_OES, (4, 4): GL_PALETTE4_RGBA8_OES,
           (8, 3): GL_PALETTE8_RGB8_OES, (8, 4): GL_PALETTE8_RGBA8_OES}[(bits, chans)]
    head = struct.pack("<4s8HI", b"P8T1", width, height, pw, ph, w0, h0, len(levels), 0, fmt)
    return head + full.tobytes() + b"".join(levels), mse8, mse16


def job(args):
    name, data, maxside = args
    try:
        return name, convert(data, maxside), None
    except Exception as e:  # an image PIL cannot read: left to the renderer
        return name, None, str(e)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("baseq3")
    ap.add_argument("--max", type=int, default=128)
    ap.add_argument("--dirs", default="textures,models")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    o = ap.parse_args()
    dirs = tuple(d.strip("/") + "/" for d in o.dirs.split(","))
    paks = sorted(f for f in os.listdir(o.baseq3) if f.lower().endswith(".pk3") and f.lower().startswith("pak"))
    # name without extension -> (pak, member); .tga before .jpg in a pak
    src = {}
    for pak in paks:
        z = zipfile.ZipFile(os.path.join(o.baseq3, pak))
        found = {}
        for m in z.namelist():
            low = m.lower()
            if not low.startswith(dirs) or not low.endswith((".tga", ".jpg")):
                continue
            base = low.rsplit(".", 1)[0]
            if base not in found or low.endswith(".tga"):
                found[base] = (pak, m)
        src.update(found)
    jobs = []
    zips = {}
    for base, (pak, m) in sorted(src.items()):
        z = zips.setdefault(pak, zipfile.ZipFile(os.path.join(o.baseq3, pak)))
        jobs.append((base, z.read(m), o.max))
    tmp = o.out + ".tmp"
    n = bytes_out = worse = 0
    s8 = s16 = 0.0
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zo, ProcessPoolExecutor(o.j) as ex:
        for name, res, err in ex.map(job, jobs, chunksize=8):
            if err:
                print(f"skipped {name}: {err}", file=sys.stderr)
                continue
            blob, mse8, mse16 = res
            zo.writestr(name + ".p8", blob)
            n += 1
            bytes_out += len(blob)
            s8 += mse8
            s16 += mse16
            worse += mse8 > mse16
    os.replace(tmp, o.out)
    print(f"{n} textures, {bytes_out / 1048576:.1f} MiB; mean squared error of level 0: "
          f"8-bit {s8 / n:.2f}, 16-bit {s16 / n:.2f}; {worse} worse than 16-bit")


if __name__ == "__main__":
    main()
