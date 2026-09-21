#!/usr/bin/env python3
"""Faithful room emitter using the instrumented-engine dumps (work/tileraw):
exact tile pool + per-layer cell lists (with flip bits). Cells are emitted as
original tiles + SMS hardware flip bits when they reproduce the oracle raster;
otherwise (SGD overlap, masks, multi-layer) a custom baked tile is used.
Bank layout: [u16 nt][nt*32 tiles][896*2 map u16][112 ct][4 links]
"""
import os, sys, re, glob, struct
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import fbextract as fb
from sms_convert import sms_color

ROOT = os.path.join(os.path.dirname(__file__), "..")
DATA = os.path.join(ROOT, "assets", "DATA")
DUMP = os.path.join(ROOT, "work", "dump0")
TILE = os.path.join(ROOT, "work", "tileraw")
GEN = os.path.join(ROOT, "sms", "gen")

arch = fb.read_aba(os.path.join(DATA, "DEMO_UK.ABA"))
lev = open(os.path.join(DATA, "LEVEL1.LEV"), "rb").read()
ctraw, ok = fb.bk_unpack(arch["LEVEL1.CT"])
assert ok
def s8(b): return b if b < 128 else b - 256

ct = {}; cur = None
for line in open(os.path.join(DUMP, "ct.txt")):
    m = re.match(r'room (\d+) grid', line)
    if m: cur = int(m.group(1)); ct[cur] = []
    elif cur is not None and line.strip()[:1].isdigit():
        ct[cur] += [int(x) for x in line.split()]

rooms = sorted(glob.glob(os.path.join(DUMP, "room*_idx.bin")))
room_ids = [int(re.search(r'room(\d+)_idx', r).group(1)) for r in rooms]
have = set(room_ids)

def tile_pixels(t):
    rows = []
    for y in range(8):
        row = []
        for b in t[y*8:y*8+8]:
            row.append(b >> 4); row.append(b & 15)
        rows.append(row)
    return rows

def flipx(rows): return [r[::-1] for r in rows]
def flipy(rows): return rows[::-1]

def planar(rows, slotmap):
    out = bytearray(32)
    for row in range(8):
        for plane in range(4):
            b = 0
            for col in range(8):
                if slotmap[rows[row][col]] & (1 << plane):
                    b |= 1 << (7 - col)
            out[row*4+plane] = b
    return bytes(out)

bank = 2; table = []; pals = []; stats = []
for rid in room_ids:
    C = np.fromfile(os.path.join(DUMP, "room%02d_idx.bin" % rid), dtype=np.uint8).reshape(224, 256)
    pal = np.fromfile(os.path.join(DUMP, "room%02d_pal.bin" % rid), dtype=np.uint8).reshape(256, 3)
    off = int.from_bytes(lev[rid*4:rid*4+4], "big")
    blob, ok = fb.bk_unpack(lev[off:])
    assert ok
    sgdflag = (len(blob) > 1 and blob[1] != 0)
    d = open(os.path.join(TILE, "room%02d.bin" % rid), "rb").read()
    nt_pool = struct.unpack('<I', d[0:4])[0]
    pool = [d[4+i*32:4+(i+1)*32] for i in range(nt_pool)]
    pos = 4 + nt_pool*32
    layers = []
    while pos < len(d):
        tag = d[pos]; pos += 1
        cells = [int.from_bytes(d[pos+i*2:pos+i*2+2], 'big') for i in range(896)]
        pos += 896*2
        layers.append((tag, cells))
    # palette merge
    used = sorted(set(C.ravel().tolist()))
    groups = {}; reps = []
    for c in used:
        key = tuple(pal[c][:3]); placed = False
        for r in reps:
            if tuple(pal[r][:3]) == key:
                groups[r].append(c); placed = True; break
        if not placed:
            reps.append(c); groups[c] = [c]
    while len(reps) > 16:
        bi = bj = 0; bd = None
        for i in range(len(reps)):
            for j2 in range(i+1, len(reps)):
                dd = sum(abs(int(pal[reps[i]][k]) - int(pal[reps[j2]][k])) for k in range(3))
                if bd is None or dd < bd: bd = dd; bi = i; bj = j2
        groups[reps[bi]] += groups[reps[bj]]; reps.pop(bj)
    slotmap = [0]*256
    for si, r in enumerate(reps):
        for c in groups[r]: slotmap[c] = si
    pal16 = [sms_color(*pal[reps[i]][:3].tolist()) if i < len(reps) else 0 for i in range(16)]

    tiles = [b"\x00"*32]
    pcode = {}
    mapa = [0]*896
    n_struct = 0
    pending = {}  # key -> list of cell indices
    for j in range(896):
        ty, tx = divmod(j, 32)
        cell = C[ty*8:ty*8+8, tx*8:tx*8+8]
        cands = []
        for tag, cells in layers:
            d3 = cells[j]
            d0 = d3 & 0x7FF
            if d0 != 0 and sgdflag and tag == 1:
                d0 -= 0x380
            if d0 <= 0 or d0 >= nt_pool:
                continue
            xf = bool(d3 & (1 << 11)); yf = bool(d3 & (1 << 12))
            cands.append((d0, xf, yf))
        done = False
        if len(cands) == 1:
            d0, xf, yf = cands[0]
            rows = tile_pixels(pool[d0])
            if xf: rows = flipx(rows)
            if yf: rows = flipy(rows)
            if all(rows[yy][xx] == cell[yy, xx] for yy in range(8) for xx in range(8)):
                if d0 not in pcode:
                    pcode[d0] = len(tiles)
                    tiles.append(planar(tile_pixels(pool[d0]), slotmap))
                e = pcode[d0]
                if xf: e |= 0x0200
                if yf: e |= 0x0400
                mapa[j] = e; done = True; n_struct += 1
        if not done:
            if not cell.any():
                mapa[j] = 0
            else:
                key = cell.tobytes()
                pending.setdefault(key, []).append(j)
    # budget custom tiles: keep most frequent uniques exact, map the rest to nearest kept
    BUDGET = 400 - len(tiles)
    keys = sorted(pending.keys(), key=lambda k: -len(pending[k]))
    kept = keys[:BUDGET]
    rest = keys[BUDGET:]
    kidx = {k: len(tiles)+i for i, k in enumerate(kept)}
    for k in kept:
        tiles.append(planar(np.frombuffer(k, dtype=np.uint8).reshape(8, 8).tolist(), slotmap))
    for k, js in pending.items():
        idx = kidx.get(k)
        if idx is None:
            if kept:
                v = np.frombuffer(k, dtype=np.uint8)
                km = np.stack([np.frombuffer(x, dtype=np.uint8) for x in kept])
                dist = (km != v).sum(axis=1)
                idx = kidx[kept[int(dist.argmin())]]
            else:
                idx = 0
        for j in js:
            mapa[j] = idx
    nt = len(tiles)
    stats.append((rid, nt, n_struct, len(kept), len(rest)))
    if nt > 400:
        print("OVER", rid, nt); sys.exit(1)
    data = bytearray(nt.to_bytes(2, 'little'))
    for t in tiles: data += t
    for j in range(896): data += mapa[j].to_bytes(2, 'little')
    g = ct.get(rid, [0]*112)
    data += bytes(g[:112])
    lu, ld, lr, ll = (s8(ctraw[rid]), s8(ctraw[0x40+rid]), s8(ctraw[0x80+rid]), s8(ctraw[0xC0+rid]))
    data += bytes([lu if lu in have else 255, ld if ld in have else 255,
                   lr if lr in have else 255, ll if ll in have else 255])
    bf = open(os.path.join(GEN, "bank%d.c" % bank), "w")
    bf.write('#include <stdint.h>\n#pragma constseg BANK%d\n' % bank)
    bf.write('static const uint8_t roomdata[%d] = {\n' % len(data))
    for i in range(0, len(data), 16):
        bf.write(','.join('0x%02X' % b for b in data[i:i+16]) + ',\n')
    bf.write('};\n'); bf.close()
    table.append((rid, bank, nt)); pals.append(pal16); bank += 1

open(os.path.join(GEN, "roomtable.c"), "w").write(
    '#include "rooms.h"\nconst RoomInfo roomTable[NUM_ROOMS]={\n' +
    ''.join('{%d,%d,%d},\n' % (b, nt, r) for r, b, nt in table) + '};\n' +
    'const int8_t roomLinks[NUM_ROOMS][4]={\n' +
    ''.join('{%d,%d,%d,%d},\n' % tuple(l if l in have else -1 for l in
            (s8(ctraw[r]), s8(ctraw[0x40+r]), s8(ctraw[0x80+r]), s8(ctraw[0xC0+r]))) for r in room_ids) + '};\n')
open(os.path.join(GEN, "roompals.c"), "w").write(
    '#include "rooms.h"\nconst uint8_t roomPalette[NUM_ROOMS][16]={\n' +
    ''.join('{' + ','.join(str(p) for p in pl) + '},\n' for pl in pals) + '};\n')
print("banks 2..%d" % (bank-1))
tot_s = sum(x[2] for x in stats)
print("structural cells", tot_s, "custom kept/approx", sum(x[3] for x in stats), sum(x[4] for x in stats))
for st in stats: print(st)
