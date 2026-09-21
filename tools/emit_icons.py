#!/usr/bin/env python3
"""Emit every PGE floor-object icon from the DOS demo into one icon bank.

Replaces the level-1 port's icon bank, which only contained GLOBAL.ICN #12 in
slot 0: any item whose icon index was not 0 loaded whatever bytes happened to
follow (usually zeroes). Here the whole icon set is emitted, so item number and
icon number are the same thing and slot = icon works for every item.

Input : assets/DATA/DEMO_UK.ABA  (GLOBAL.ICN: LE16 offset table + 16x16 4bpp)
Output: sms/gen/bank43.c   --  icon N at byte offset N*128 (4 planar tiles, 2x2)

Usage: emit_icons.py <DEMO_UK.ABA> <out/bank43.c> [count]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import fbextract as fb          # noqa: E402  (from the level-1 port's tools)


def decode_icn(src, num):
    off = int.from_bytes(src[num * 2:num * 2 + 2], 'little')
    p = src[off + 2:off + 2 + 128]
    px = []
    for b in p:
        px.append(b >> 4)
        px.append(b & 15)
    return px                   # 16x16 row-major, 0..15


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


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    aba = sys.argv[1]
    out = sys.argv[2]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    arch = fb.read_aba(aba)
    icn = arch['GLOBAL.ICN']
    n = min(count, len(icn) // 2)

    data = bytearray()
    for num in range(n):
        px = decode_icn(icn, num)
        for ty in range(2):
            for tx in range(2):
                t = [0] * 64
                for ry in range(8):
                    for rx in range(8):
                        t[ry * 8 + rx] = px[(ty * 8 + ry) * 16 + (tx * 8 + rx)]
                data += planar_tile(t)

    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out, 'w') as fh:
        fh.write('#include <stdint.h>\n#pragma constseg BANK43\n')
        fh.write('/* %d floor-object icons, icon N at offset N*128 (4 tiles, 2x2) */\n' % n)
        fh.write('static const uint8_t icondata[%d] = {\n' % len(data))
        for i in range(0, len(data), 16):
            fh.write(','.join('0x%02X' % b for b in data[i:i + 16]) + ',\n')
        fh.write('};\n')
    print(f'{out}: {n} icons, {len(data)} bytes (slot = icon number)')


if __name__ == '__main__':
    main()
