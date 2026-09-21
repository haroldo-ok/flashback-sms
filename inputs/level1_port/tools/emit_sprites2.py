#!/usr/bin/env python3
"""Emit Conrad sprite frames as pre-planarized SMS tiles (normal + x-flipped),
so the Z80 only does one SMS_loadTiles per frame change.
Bank layout per frame: [w,u8][h,u8][ntiles,u8][ntiles*32 normal][ntiles*32 flipped]
frametable.c: {bank, off} per sprite id (65535 = absent).
"""
import os, sys, re
sys.path.insert(0, os.path.dirname(__file__))
import fbextract as fb

ROOT = os.path.join(os.path.dirname(__file__), "..")
DATA = os.path.join(ROOT, "assets", "DATA")
GEN = os.path.join(ROOT, "sms", "gen")
os.makedirs(GEN, exist_ok=True)

arch = fb.read_aba(os.path.join(DATA, "DEMO_UK.ABA"))
spr = open(os.path.join(DATA, "PERSO.SPR"), "rb").read()
off = arch["PERSO.OFF"]
frames = fb.load_spr(spr, off)

def planar_tile(px8):
    """px8: 8x8 list of indices -> 32 bytes SMS planar."""
    out = bytearray(32)
    for row in range(8):
        for plane in range(4):
            b = 0
            for col in range(8):
                if px8[row][col] & (1 << plane):
                    b |= 1 << (7 - col)
            out[row * 4 + plane] = b
    return bytes(out)

def frame_tiles(pix, w, h, flip):
    tw = (w + 7) // 8
    th = (h + 7) // 8
    tiles = bytearray()
    for ty in range(th):
        for tx in range(tw):
            px8 = [[0] * 8 for _ in range(8)]
            for ry in range(8):
                for rx in range(8):
                    xx = tx * 8 + rx
                    yy = ty * 8 + ry
                    if xx < w and yy < h:
                        sx = (w - 1 - xx) if flip else xx
                        px8[ry][rx] = pix[yy * w + sx]
            tiles += planar_tile(px8)
    return bytes(tiles), tw * th

USED = set([3]) | set(range(11, 17)) | set(range(71, 83)) | set(range(239, 243)) | set(range(293, 299)) | set(range(305, 326)) | set(range(328, 346))
good = {}
for idx in sorted(frames):
    if idx not in USED:
        continue
    fr = frames[idx]
    wb, hb = fr[2], fr[3]
    dec = fb.spm_decode(fr)
    if wb & 0x40:
        # transposed storage: screen[y][x] = dec[y + x*h]
        w = hb; h = wb & 0x3F
        if len(dec) < w * h:
            continue
        pix = [dec[y + x * h] for y in range(h) for x in range(w)]
    else:
        w, h = wb, hb
        if len(dec) < w * h:
            continue
        pix = list(dec[:w * h])
    if not (4 <= w <= 48 and 4 <= h <= 56):
        continue
    good[idx] = (w, h, pix)

print("good frames:", len(good))

# pack into 16KB banks
banks = []   # list of bytearray
cur = bytearray()
table = {}
BANK0 = 40
for idx in sorted(good):
    w, h, pix = good[idx]
    fr = frames[idx]
    tn, n = frame_tiles(pix, w, h, False)
    tf, _ = frame_tiles(pix, w, h, True)
    rec = bytes([w, h, n, fr[0], fr[1]]) + tn + tf
    if len(cur) + len(rec) > 16384:
        banks.append(cur)
        cur = bytearray()
    table[idx] = (BANK0 + len(banks), len(cur))
    cur += rec
banks.append(cur)

for i, b in enumerate(banks):
    bf = open(os.path.join(GEN, "bank%d.c" % (BANK0 + i)), "w")
    bf.write('#include <stdint.h>\n#pragma constseg BANK%d\n' % (BANK0 + i))
    bf.write('static const uint8_t sprdata[%d] = {\n' % len(b))
    for i2 in range(0, len(b), 16):
        bf.write(','.join('0x%02X' % x for x in b[i2:i2 + 16]) + ',\n')
    bf.write('};\n')
    bf.close()
# remove stale extra banks
import glob
for p in glob.glob(os.path.join(GEN, "bank*.c")):
    n = int(re.search(r'bank(\d+)\.c', p).group(1))
    if n >= BANK0 + len(banks) + 1:  # +1: bank43 is the icon bank, never touch
        os.remove(p)

maxid = max(table) if table else 0
with open(os.path.join(GEN, "frametable.c"), "w") as f:
    f.write('#include "frames.h"\nconst FrameRec frameTable[NUM_FRAMES]={\n')
    for i in range(maxid + 1):
        b, o = table.get(i, (0, 65535))
        f.write('{%d,%d},\n' % (b, o))
    f.write('};\n')
with open(os.path.join(GEN, "frames.h"), "w") as f:
    f.write('#ifndef FRAMES_H\n#define FRAMES_H\n#include <stdint.h>\n')
    f.write('typedef struct { uint8_t bank; uint16_t off; } FrameRec;\n')
    f.write('#define NUM_FRAMES %d\n' % (maxid + 1))
    f.write('extern const FrameRec frameTable[NUM_FRAMES];\n#endif\n')
print("banks", len(banks), "sizes", [len(b) for b in banks], "maxid", maxid)
