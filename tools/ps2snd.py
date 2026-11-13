#!/usr/bin/env python3
"""Quake 3's sounds as SPU ADPCM for the PS2.

    ps2snd.py OUT.pk3 BASEQ3_DIR [--dirs sound] [-j N]

Reads the pak*.pk3 of BASEQ3_DIR in order (a later pak's file replaces an
earlier one's) and writes for each .wav under --dirs a <name>.adp into
OUT.pk3. The PS2's sound code (code/ps2/snd_ps2.c, s_ps2hw 1) takes a
sound's .adp before the .wav: the SPU2's voices play it as it is, so it
need not be encoded on the PS2 (about 60 ms a second of sound there), and
the file is a third and a half of the 16-bit one. The music (music/) stays
as it is: it is streamed.

.adp layout (little-endian):
    char  magic[4]   "ADP1"
    u32   rate       Hz
    u32   samples    of the sound (mono; stereo files are mixed)
    then the ADPCM: blocks of 16 bytes for 28 samples each (shift and filter,
    flags, 28 nibbles), the last one flagged as the end, as maxsdk's
    mx_snd_encode makes them (ee/src/base/snd.c: this is that encoder).
"""
import argparse
import os
import struct
import sys
import zipfile
from concurrent.futures import ProcessPoolExecutor

FILT = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))


def try_block(x, f, sh, h1, h2):
    """the block's 14 data bytes for filter f and shift sh, its squared error
    and the decoder's last two samples"""
    f0, f1 = FILT[f]
    step = 1 << (12 - sh)
    half = step >> 1
    s1, s2 = h1, h2
    err = 0
    b = [0] * 14
    for i in range(28):
        pred = (s1 * f0 + s2 * f1 + 32) >> 6
        r = x[i] - pred
        q = (r + half) // step if r >= 0 else -((half - r) // step)
        q = -8 if q < -8 else 7 if q > 7 else q
        d = q * step + pred
        d = -32768 if d < -32768 else 32767 if d > 32767 else d
        e = x[i] - d
        err += e * e
        s2 = s1
        s1 = d
        b[i >> 1] |= (q & 15) << (4 if i & 1 else 0)
    return err, b, s1, s2


def encode(pcm):
    """16-bit mono samples (a list of ints) as SPU ADPCM, played once"""
    n = len(pcm)
    blocks = (n + 27) // 28
    out = bytearray(blocks * 16)
    h1 = h2 = 0
    pcm = list(pcm) + [0] * (blocks * 28 - n)
    for k in range(blocks):
        x = pcm[k * 28:k * 28 + 28]
        best = None
        for f in range(5):
            f0, f1 = FILT[f]
            # the shift for the largest residual, predicted from the source
            p1, p2, m = h1, h2, 0
            for v in x:
                r = v - ((p1 * f0 + p2 * f1 + 32) >> 6)
                if r < 0:
                    r = -r
                if r > m:
                    m = r
                p2 = p1
                p1 = v
            sh = 12
            while sh > 0 and (7 << (12 - sh)) < m:
                sh -= 1
            for s in (sh, sh - 1):
                if s < 0:
                    break
                err, b, t1, t2 = try_block(x, f, s, h1, h2)
                if best is None or err < best[0]:
                    best = (err, f << 4 | s, b, t1, t2)
        _, head, b, h1, h2 = best
        out[k * 16] = head
        out[k * 16 + 2:k * 16 + 16] = bytes(b)
    out[(blocks - 1) * 16 + 1] |= 1  # end: the voice is released
    return bytes(out)


def wav_pcm(data):
    """(rate, mono 16-bit samples) of a PCM .wav, or None"""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    at, fmt, body = 12, None, None
    while at + 8 <= len(data):
        tag, size = data[at:at + 4], struct.unpack_from("<I", data, at + 4)[0]
        if tag == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, at + 8)
        elif tag == b"data":
            body = data[at + 8:at + 8 + size]
        at += 8 + size + (size & 1)
    if not fmt or body is None or fmt[0] != 1:
        return None
    _, channels, rate, _, _, bits = fmt
    width = bits // 8
    if width not in (1, 2) or channels < 1:
        return None
    n = len(body) // (width * channels)
    if width == 2:
        v = struct.unpack_from("<%dh" % (n * channels), body)
    else:
        v = [(c - 128) << 8 for c in body[:n * channels]]
    if channels > 1:
        # the average, rounded towards zero as C does
        v = [int(sum(v[i * channels:(i + 1) * channels]) / channels) for i in range(n)]
    return rate, list(v)


def convert(job):
    name, data = job
    w = wav_pcm(data)
    if not w or not w[1]:
        return name, None
    rate, pcm = w
    return name, struct.pack("<4sII", b"ADP1", rate, len(pcm)) + encode(pcm)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("baseq3")
    ap.add_argument("--dirs", default="sound")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 1)
    a = ap.parse_args()
    dirs = tuple(d.strip("/").lower() + "/" for d in a.dirs.split(","))
    files = {}
    paks = sorted(f for f in os.listdir(a.baseq3)
                  if f.lower().startswith("pak") and f.lower().endswith(".pk3"))
    for pak in paks:
        with zipfile.ZipFile(os.path.join(a.baseq3, pak)) as z:
            for n in z.namelist():
                low = n.lower()
                if low.startswith(dirs) and low.endswith(".wav"):
                    files[low] = z.read(n)
    jobs = sorted(files.items())
    src = dst = done = 0
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED) as z, ProcessPoolExecutor(a.j) as ex:
        for name, blob in ex.map(convert, jobs, chunksize=4):
            if blob is None:
                print("skipped (not PCM):", name, file=sys.stderr)
                continue
            z.writestr(name[:-4] + ".adp", blob)
            src += len(files[name])
            dst += len(blob)
            done += 1
    print("%d sounds: %d KB of .wav, %d KB of .adp" % (done, src // 1024, dst // 1024))


if __name__ == "__main__":
    main()
