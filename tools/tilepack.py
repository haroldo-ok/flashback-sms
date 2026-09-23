#!/usr/bin/env python3
"""Lossless compact storage for 8x8 4bpp SMS tiles.

Flat-shaded and pixel-art tiles rarely use all 16 colours, so each tile is
stored in the smallest of three forms:

  type 1  two colours      8-byte mask + 1 byte (c0<<4 | c1)          9 bytes
  type 2  three/four       2 bits per pixel (row: bit0 byte, bit1 byte)
                           16 bytes + 2 bytes of four packed colours   18 bytes
  type 0  anything else    the usual 32-byte planar tile               32 bytes

No per-tile index is stored.  A dictionary is sorted by type, so a tile id
alone says which region it is in and where; each region starts on a bank and
holds a whole number of records per bank, so none straddles a bank:

  region 0: 512 records/bank, region 1: 1792 (7 x 256, 16128 bytes),
  region 2: 896 (7 x 128, 16128 bytes).  Multiples of 256/128 let the Z80 find
  the bank with a small table lookup instead of a division.

The Z80 expands types 1 and 2 into 32 planar bytes before uploading
(src/tiledec.c); `decode` below is the reference for that code.
"""
import numpy as np

SIZE = {0: 32, 1: 10, 2: 18, 3: 26}
# records per bank chosen so the Z80 finds the bank with a table and shifts
PER_BANK = {0: 512, 1: 1536, 2: 896, 3: 512}   # 512, 6x256, 7x128, 512
BANK = 16384


def planar(p64):
    out = bytearray()
    for y in range(8):
        b = [0, 0, 0, 0]
        for x in range(8):
            v = int(p64[y * 8 + x]) & 15
            for k in range(4):
                if v & (1 << k):
                    b[k] |= 0x80 >> x
        out += bytes(b)
    return bytes(out)


def unplanar(t32):
    out = np.zeros(64, np.uint8)
    for y in range(8):
        q = t32[y * 4:y * 4 + 4]
        for x in range(8):
            bit = 7 - x
            out[y * 8 + x] = (((q[0] >> bit) & 1) | (((q[1] >> bit) & 1) << 1) |
                              (((q[2] >> bit) & 1) << 2) | (((q[3] >> bit) & 1) << 3))
    return out


def _planes(p64):
    """the four bit-planes of a tile, 8 bytes each"""
    pl = planar(p64)
    return [bytes(pl[y * 4 + k] for y in range(8)) for k in range(4)]


def encode(p64):
    """Bit-plane storage.  Each of the four planes is a code in a 2-byte
    header: 0 = all zero, 1 = all one, 2+j = stored plane j, 6+j = the
    complement of stored plane j.  Only independent planes are stored, so a
    flat-shaded tile usually needs one or two.  Type = number stored (1..3);
    four independent planes are kept raw (type 0)."""
    planes = _planes(p64)
    stored, codes = [], []
    for pl in planes:
        if pl == bytes(8):
            codes.append(0); continue
        if pl == bytes([255]) * 8:
            codes.append(1); continue
        comp = bytes(255 - b for b in pl)
        if pl in stored:
            codes.append(2 + stored.index(pl)); continue
        if comp in stored:
            codes.append(6 + stored.index(comp)); continue
        stored.append(pl)
        codes.append(2 + len(stored) - 1)
    k = len(stored)
    if k == 0:                      # solid tile: keep one stored plane of zeros
        stored = [bytes(8)]
        k = 1
    if k >= 4:
        return 0, planar(p64)
    hdr = bytes([(codes[0] << 4) | codes[1], (codes[2] << 4) | codes[3]])
    return k, hdr + b''.join(stored)


def tile_type(p64):
    return encode(p64)[0]


def decode(t, rec):
    """reference expansion to 32 planar bytes (mirrors src/tiledec.c)"""
    if t == 0:
        return bytes(rec)
    codes = [rec[0] >> 4, rec[0] & 15, rec[1] >> 4, rec[1] & 15]
    st = rec[2:]
    out = bytearray(32)
    for p, c in enumerate(codes):
        for y in range(8):
            if c == 0: v = 0
            elif c == 1: v = 255
            elif c < 6: v = st[(c - 2) * 8 + y]
            else: v = 255 - st[(c - 6) * 8 + y]
            out[y * 4 + p] = v
    return bytes(out)


def build(tiles64):
    """tiles64: list of 64-index tiles in their current id order.
    Returns (remap old->new id, regions {type: bytes}, starts (start1, start2))."""
    enc = [encode(t) for t in tiles64]
    order = sorted(range(len(enc)), key=lambda i: (enc[i][0] != 0, enc[i][0] != 1, enc[i][0] != 2, i))
    # stable: type 0 first, then 1, then 2
    order = [i for t in (0, 1, 2, 3) for i in range(len(enc)) if enc[i][0] == t]
    remap = [0] * len(enc)
    for new, old in enumerate(order):
        remap[old] = new
    regions = {}
    for t in (0, 1, 2, 3):
        recs = [enc[i][1] for i in order if enc[i][0] == t]
        blob = bytearray()
        for n, r in enumerate(recs):
            if n and n % PER_BANK[t] == 0:
                blob += bytes(BANK - len(blob) % BANK) if len(blob) % BANK else b''
            blob += r
        regions[t] = bytes(blob)
    n0 = sum(1 for e in enc if e[0] == 0)
    n1 = sum(1 for e in enc if e[0] == 1)
    n2 = sum(1 for e in enc if e[0] == 2)
    return remap, regions, (n0, n0 + n1, n0 + n1 + n2)


def fetch(blob_at, desc, tid):
    """reference lookup; desc = dict(bank0..bank3, start1..start3)"""
    if tid < desc['start1']:
        return decode(0, blob_at(desc['bank0'] + tid // 512, (tid % 512) * 32, 32))
    for t, lo, hi in ((1, 'start1', 'start2'), (2, 'start2', 'start3'), (3, 'start3', None)):
        if hi is None or tid < desc[hi]:
            k = tid - desc[lo]
            n = PER_BANK[t]
            return decode(t, blob_at(desc[f'bank{t}'] + k // n, (k % n) * SIZE[t], SIZE[t]))


if __name__ == '__main__':
    # self-test: every form round-trips exactly
    rng = np.random.default_rng(1992)
    for trial in range(3000):
        n = int(rng.integers(1, 17))
        cols = rng.choice(16, n, replace=False)
        p = cols[rng.integers(0, n, 64)]
        t, rec = encode(p)
        assert len(rec) == SIZE[t], (t, len(rec))
        assert np.array_equal(unplanar(decode(t, rec)), p), (trial, n, t)
    print('tilepack self-test: 3000 random tiles round-trip exactly')
