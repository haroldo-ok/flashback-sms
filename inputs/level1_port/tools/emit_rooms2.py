#!/usr/bin/env python3
import os, sys, glob, re
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import importlib
import sms_convert; importlib.reload(sms_convert)
from sms_convert import vq_room, sms_color, chunky_to_planar

DUMP = os.path.join(os.path.dirname(__file__), "..", "work", "dump0")
GEN = os.path.join(os.path.dirname(__file__), "..", "sms", "gen")
os.makedirs(GEN, exist_ok=True)

import fbextract
_arch = fbextract.read_aba(os.path.join(os.path.dirname(__file__), "..", "assets", "DATA", "DEMO_UK.ABA"))
_ctraw, _ok = fbextract.bk_unpack(_arch["LEVEL1.CT"])
assert _ok
def _s(b): return b if b < 128 else b - 256
links = {}
for r in range(64):
    links[r] = (_s(_ctraw[r]), _s(_ctraw[0x40+r]), _s(_ctraw[0x80+r]), _s(_ctraw[0xC0+r]))
ct = {}; cur = None
for line in open(os.path.join(DUMP, "ct.txt")):
    m = re.match(r'room (\d+) grid', line)
    if m: cur = int(m.group(1)); ct[cur] = []
    elif cur is not None and line.strip()[:1].isdigit():
        ct[cur] += [int(x) for x in line.split()]
rooms = sorted(glob.glob(os.path.join(DUMP, "room*_idx.bin")))
room_ids = [int(re.search(r'room(\d+)_idx', r).group(1)) for r in rooms]
have = set(room_ids)
bank = 2; table = []; pals = []
for rid in room_ids:
    A = np.fromfile(os.path.join(DUMP, "room%02d_idx.bin" % rid), dtype=np.uint8).reshape(224, 256)
    pal = np.fromfile(os.path.join(DUMP, "room%02d_pal.bin" % rid), dtype=np.uint8).reshape(256, 3)
    used = sorted(set(A.ravel().tolist()))
    # dedupe identical rgb then merge nearest until <=16
    groups = {}   # representative index -> list of indices
    reps = []
    for c in used:
        key = tuple(pal[c][:3])
        placed = False
        for r in reps:
            if tuple(pal[r][:3]) == key:
                groups[r].append(c); placed = True; break
        if not placed:
            reps.append(c); groups[c] = [c]
    while len(reps) > 16:
        bi = 0; bj = 1; bd = None
        for i in range(len(reps)):
            for j in range(i + 1, len(reps)):
                d = sum(abs(int(pal[reps[i]][k]) - int(pal[reps[j]][k])) for k in range(3))
                if bd is None or d < bd: bd = d; bi = i; bj = j
        groups[reps[bi]] += groups[reps[bj]]
        reps.pop(bj); 
    slot = {}
    for s_i, r in enumerate(reps):
        for c in groups[r]: slot[c] = s_i
    M = np.zeros_like(A)
    for c, s in slot.items(): M[A == c] = s
    pal16 = [sms_color(*pal[reps[i]][:3].tolist()) if i < len(reps) else 0 for i in range(16)]
    tiles, tilemap = vq_room(M, 400)
    nt = len(tiles)
    data = bytearray(nt.to_bytes(2, 'little'))
    for t in tiles: data += chunky_to_planar(t)
    for j in range(896): data += int(tilemap[j]).to_bytes(2, 'little')
    g = ct.get(rid, [0] * 112)
    data += bytes(g[:112])
    lu, ld, lr, ll = links[rid]
    data += bytes([lu if lu in have else 255, ld if ld in have else 255,
                   lr if lr in have else 255, ll if ll in have else 255])
    bf = open(os.path.join(GEN, "bank%d.c" % bank), "w")
    bf.write('#include <stdint.h>\n#pragma constseg BANK%d\n' % bank)
    bf.write('static const uint8_t roomdata[%d] = {\n' % len(data))
    for i in range(0, len(data), 16):
        bf.write(','.join('0x%02X' % b for b in data[i:i + 16]) + ',\n')
    bf.write('};\n')
    bf.close()
    table.append((rid, bank, nt)); pals.append(pal16); bank += 1
open(os.path.join(GEN, "roomtable.c"), "w").write('#include "rooms.h"\nconst RoomInfo roomTable[NUM_ROOMS]={\n' +
    ''.join('{%d,%d,%d},\n' % (b, nt, r) for r, b, nt in table) + '};\n' +
    'const int8_t roomLinks[NUM_ROOMS][4]={\n' +
    ''.join('{%d,%d,%d,%d},\n' % tuple(l if l in have else -1 for l in links[r]) for r in room_ids) + '};\n')
open(os.path.join(GEN, "roompals.c"), "w").write('#include "rooms.h"\nconst uint8_t roomPalette[NUM_ROOMS][16]={\n' +
    ''.join('{' + ','.join(str(p) for p in pl) + '},\n' for pl in pals) + '};\n')
print("banks 2..%d rooms %d" % (bank - 1, len(room_ids)))
