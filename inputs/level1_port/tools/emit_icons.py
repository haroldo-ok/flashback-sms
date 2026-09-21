#!/usr/bin/env python3
"""Emit PGE item icon (GLOBAL.ICN #12) as 4 planar SMS BG tiles -> gen/bank43.c."""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import fbextract as fb

ROOT = os.path.join(os.path.dirname(__file__), "..")
DATA = os.path.join(ROOT, "assets", "DATA")
GEN = os.path.join(ROOT, "sms", "gen")

arch = fb.read_aba(os.path.join(DATA, "DEMO_UK.ABA"))
icn = arch["GLOBAL.ICN"]

def decode_icn(src, num):
    off = int.from_bytes(src[num * 2:num * 2 + 2], "little")
    p = src[off + 2:]
    px = []
    for i in range(128):
        px.append(p[i] >> 4)
        px.append(p[i] & 15)
    return px  # 16x16 row-major

px = decode_icn(icn, 12)

def planar_tile(p8):
    out = bytearray(32)
    for row in range(8):
        for plane in range(4):
            b = 0
            for col in range(8):
                if p8[row * 8 + col] & (1 << plane):
                    b |= 1 << (7 - col)
            out[row * 4 + plane] = b
    return bytes(out)

data = bytearray()
for ty in range(2):
    for tx in range(2):
        t = [0] * 64
        for ry in range(8):
            for rx in range(8):
                t[ry * 8 + rx] = px[(ty * 8 + ry) * 16 + (tx * 8 + rx)]
        data += planar_tile(t)

with open(os.path.join(GEN, "bank43.c"), "w") as f:
    f.write('#include <stdint.h>\n#pragma constseg BANK43\n')
    f.write('static const uint8_t icondata[%d] = {\n' % len(data))
    for i in range(0, len(data), 16):
        f.write(','.join('0x%02X' % b for b in data[i:i + 16]) + ',\n')
    f.write('};\n')
print("icon tiles emitted", len(data))
