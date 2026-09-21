#!/usr/bin/env python3
import os, sys, glob, re
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from sms_convert import vq_room, sms_color, chunky_to_planar

ROOT = os.path.join(os.path.dirname(__file__), "..")
DUMP = os.path.join(ROOT, "work", "dump0")
GEN = os.path.join(ROOT, "sms", "gen")
os.makedirs(GEN, exist_ok=True)

def read_pgm(p):
    f = open(p, 'rb'); t = f.read().split(None, 3); return int(t[1]), int(t[2]), t[3]

# parse ct.txt -> room -> 112 grid
ct = {}
cur = None
for line in open(os.path.join(DUMP, "ct.txt")):
    m = re.match(r'room (\d+) grid', line)
    if m: cur = int(m.group(1)); ct[cur] = []
    elif cur is not None and line.strip().startswith(tuple('0123456789')):
        ct[cur] += [int(x) for x in line.split()]

rooms = sorted(glob.glob(os.path.join(DUMP, "room*_idx.bin")))
room_ids = [int(re.search(r'room(\d+)_idx', r).group(1)) for r in rooms]
print("rooms:", room_ids)

room_table = []  # (room, bank, ntiles)
bank = 2
header = open(os.path.join(GEN, "rooms.h"), "w")
header.write("// generated\n#ifndef ROOMS_H\n#define ROOMS_H\n#include <stdint.h>\n")
header.write("#define NUM_ROOMS %d\n" % len(room_ids))
header.write("typedef struct { uint8_t bank; uint16_t ntiles; uint8_t room; } RoomInfo;\n")
header.write("extern const RoomInfo roomTable[NUM_ROOMS];\n")
header.write("extern const uint8_t roomPalette[%d][16];\n" % len(room_ids))
header.write("#endif\n")
header.close()

tab = open(os.path.join(GEN, "roomtable.c"), "w")
tab.write('#include "rooms.h"\n')
palall = open(os.path.join(GEN, "roompals.c"), "w")
palall.write('#include "rooms.h"\nconst uint8_t roomPalette[%d][16] = {\n' % len(room_ids))

for idx, rid in enumerate(room_ids):
    idxbuf = np.fromfile(os.path.join(DUMP, "room%02d_idx.bin" % rid), dtype=np.uint8).reshape(224, 256)
    pal = np.fromfile(os.path.join(DUMP, "room%02d_pal.bin" % rid), dtype=np.uint8).reshape(256, 3)
    used = sorted(set(idxbuf.ravel().tolist()))
    slot = {c: i for i, c in enumerate(used[:16])}
    # map framebuffer to slots
    A = np.zeros_like(idxbuf)
    for c, s in slot.items():
        A[idxbuf == c] = s
    pal16 = [sms_color(*pal[used[i]][:3].tolist()) if i < len(used) else 0 for i in range(16)]
    tiles, tilemap = vq_room(A, 400)
    nt = len(tiles)
    room_table.append((rid, bank, nt))
    # emit bank file
    bf = open(os.path.join(GEN, "bank%d.c" % bank), "w")
    bf.write('#include <stdint.h>\n')
    bf.write('#pragma constseg BANK%d\n' % bank)
    bf.write('const uint8_t room%02d_tiles[%d] = {\n' % (rid, nt * 32))
    data = bytearray()
    for t in tiles:
        data += chunky_to_planar(t)
    for i in range(0, len(data), 16):
        bf.write(",".join("0x%02X" % b for b in data[i:i + 16]) + ",\n")
    bf.write('};\n')
    bf.write('const uint16_t room%02d_map[896] = {\n' % rid)
    for i in range(0, 896, 16):
        bf.write(",".join(str(tilemap[j]) for j in range(i, i + 16)) + ",\n")
    bf.write('};\n')
    bf.write('const uint8_t room%02d_ct[112] = {\n' % rid)
    g = ct.get(rid, [0] * 112)
    bf.write(",".join(str(x) for x in g[:112]) + "};\n")
    bf.write('#pragma constseg\n')
    bf.close()
    palall.write("{" + ",".join(str(p) for p in pal16) + "},\n")
    bank += 1

palall.write("};\n"); palall.close()
tab.write("const RoomInfo roomTable[NUM_ROOMS]={\n")
for rid, b, nt in room_table:
    tab.write("{%d,%d,%d},\n" % (b, nt, rid))
tab.write("};\n"); tab.close()
print("emitted banks 2..%d" % (bank - 1))
