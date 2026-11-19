#!/usr/bin/env python3
"""The PS2 renderer's images and models as two files it maps into memory
(code/renderer_ps2): nothing is copied or decoded when a map loads, a page
of them is read when it is first touched and given up without a write when
memory is short.

    ps2bin.py tex OUT.bin PS2TEX.pk3     the images of tools/ps2pak.py's pak
    ps2bin.py mdl OUT.bin BASEQ3_DIR     the MD3 models of the paks

Both files (little-endian):

    head (32 bytes): char magic[4] "PBIN"; u32 version 2; u32 kind (1 images,
        2 models); u32 count; u32 off_index; u32 size; u32 pad[2]
    index: count entries of 128 bytes, sorted by name (bytes, lower case, no
        extension, '/' between directories):
        char name[64]
        u32  off, size                 the entry's data (16-byte aligned)
      images:
        u16  w, h                      level 0's texels (powers of two)
        u16  pic_w, pic_h              the picture's own size
        u8   psm (0x13 T8, 0x14 T4), levels, alpha (its palette has alpha), pad
        u32  off_clut                  the GS's order. With alpha: 256 words
                                       (T8) or 16 (T4). Without: 256 words
                                       also for T4 (its 16 colours are the
                                       first 16 entries: a T4 CLUT at the
                                       same place), and the alpha of entries
                                       128 .. 255 is a lightmap's brightness
                                       as a blend's factor: entry 128 + i has
                                       2 i (255 for the last), 0x80 is 1.0. A map's
                                       lightmap pages (T8, tools/ps2map.py)
                                       are drawn with the CLUT of the
                                       surface's texture, whose own pass
                                       takes no alpha from it
        u32  off_level[7]              the levels, each padded to 16 bytes
                                       (T4: the first texel of a byte in its
                                       low four bits); the CLUT and the
                                       levels lie one after the other
      models:
        the file as it is in the pak (an MD3), at off

The levels of an image go down to 8 texels a side: the GS's bilinear filter
and its IMAGE transfers want no less. An image smaller than that is left
out. The pictures of the screen (gfx/2d, menu, ui, icons) have one level:
they are not drawn smaller than they are. The models' files of fewer details (name_1.md3, name_2.md3) are left
out: the renderer draws the first.
"""
import os
import struct
import sys
import zipfile

GL_PALETTE4_RGB8_OES, GL_PALETTE4_RGBA8_OES = 0x8B90, 0x8B91
GL_PALETTE8_RGB8_OES, GL_PALETTE8_RGBA8_OES = 0x8B95, 0x8B96
LEVELS = 7
# The PS2's frame has 5 bits a colour: what is drawn is cut off to them (the
# GS adds its dither's -4 .. 3 first), 4 of 255 darker on average. Every
# colour of a palette is that much brighter here (black stays black: 4 + 3
# is cut off to 0).
BRIGHTER = 4
ONE_LEVEL = ("gfx/2d/", "menu/", "ui/", "icons/")


def pad16(b):
    return b + b"\0" * (-len(b) % 16)


def image(blob, levels=LEVELS):
    """a .p8 (tools/ps2pak.py) as the index's fields and the data (at most
    levels levels of it), or None"""
    magic, width, height, pw, ph, w0, h0, nlev, _, fmt = struct.unpack_from("<4s8HI", blob)
    if magic != b"P8T1" or w0 & (w0 - 1) or h0 & (h0 - 1) or w0 > 512 or h0 > 512:
        return None
    t4 = fmt in (GL_PALETTE4_RGB8_OES, GL_PALETTE4_RGBA8_OES)
    alpha = fmt in (GL_PALETTE4_RGBA8_OES, GL_PALETTE8_RGBA8_OES)
    ncol, chans = (16 if t4 else 256), (4 if alpha else 3)
    at = 24
    pal = blob[at:at + ncol * chans]
    at += ncol * chans
    nclut = 16 if t4 and alpha else 256

    def place(i):  # the GS's order of a 256-entry CLUT
        return i if nclut == 16 else i + 8 if (i & 0x18) == 0x08 else i - 8 if (i & 0x18) == 0x10 else i

    clut = [0] * nclut
    for i in range(ncol):
        c = pal[i * chans:(i + 1) * chans]
        a = (c[3] + 1) >> 1 if alpha else 0x80
        r, g, b = (min(255, x + BRIGHTER) for x in c[:3])
        clut[place(i)] = r | g << 8 | b << 16 | a << 24
    if not alpha:
        for i in range(128):
            k = place(128 + i)
            clut[k] = (clut[k] & 0xFFFFFF) | (255 if i == 127 else 2 * i) << 24
    data = pad16(struct.pack("<%dI" % nclut, *clut))
    offs = []
    lv = 0
    while lv < nlev and lv < levels and (w0 >> lv) >= 8 and (h0 >> lv) >= 8:
        n = (w0 >> lv) * (h0 >> lv)
        n = (n + 1) // 2 if t4 else n
        src = blob[at:at + n]
        if len(src) < n:
            break
        if t4:  # the first texel of a byte: the file's high four bits, the GS's low ones
            src = bytes((b << 4 | b >> 4) & 0xFF for b in src)
        offs.append(len(data))
        data += pad16(src)
        at += n
        lv += 1
    if not offs:
        return None
    return (w0, h0, width, height, 0x14 if t4 else 0x13, len(offs), int(alpha), offs), data


def write(out, kind, entries):
    """entries: [(name, fields or None, data)]"""
    entries.sort(key=lambda e: e[0])
    off_index = 32
    off = off_index + 128 * len(entries)
    off += -off % 16
    index, blobs = [], []
    for name, f, data in entries:
        rec = struct.pack("<64sII", name[:63], off, len(data))
        if f:
            w, h, pw, ph, psm, nl, alpha, offs = f
            rec += struct.pack("<4H4BI7I", w, h, pw, ph, psm, nl, alpha, 0, off,
                               *[(off + o) for o in (offs + [0] * 7)[:7]])
        index.append(rec + b"\0" * (128 - len(rec)))
        blobs.append(pad16(data))
        off += len(blobs[-1])
    with open(out + ".tmp", "wb") as fo:
        fo.write(struct.pack("<4s5I8x", b"PBIN", 2, kind, len(entries), off_index, off))
        fo.write(b"".join(index))
        fo.write(b"\0" * (-(off_index + 128 * len(entries)) % 16))
        for b in blobs:
            fo.write(b)
    os.replace(out + ".tmp", out)
    return off


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("tex", "mdl"):
        sys.exit(__doc__)
    kind, out, src = sys.argv[1:]
    entries = []
    if kind == "tex":
        z = zipfile.ZipFile(src)
        left = 0
        for m in sorted(z.namelist()):
            if not m.lower().endswith(".p8"):
                continue
            r = image(z.read(m), 1 if m.lower().replace("\\", "/").startswith(ONE_LEVEL) else LEVELS)
            if not r:
                left += 1
                continue
            entries.append((m[:-3].lower().replace("\\", "/").encode(), r[0], r[1]))
        size = write(out, 1, entries)
        print(f"{out}: {len(entries)} images, {size / 1048576:.1f} MiB; {left} left out (smaller than 8 texels)")
    else:
        paks = sorted(f for f in os.listdir(src) if f.lower().endswith(".pk3") and f.lower().startswith("pak"))
        found = {}
        for pak in paks:  # a later pak's file replaces an earlier one's
            z = zipfile.ZipFile(os.path.join(src, pak))
            for m in z.namelist():
                low = m.lower().replace("\\", "/")
                if low.endswith(".md3") and low[-6:-4] not in ("_1", "_2"):
                    found[low[:-4]] = (z, m)
        for name, (z, m) in found.items():
            entries.append((name.encode(), None, z.read(m)))
        size = write(out, 2, entries)
        print(f"{out}: {len(entries)} models, {size / 1048576:.1f} MiB")


if __name__ == "__main__":
    main()
