#!/usr/bin/env python3
"""Shrink the shared cutscene tile dictionary by merging near-identical tiles.

The dictionary is by far the biggest thing in the ROM, and flat-shaded polygon
artwork produces many tiles that differ in a pixel or two along an edge.  This
merges a tile into an earlier one when at most --max-diff of its 64 pixels
differ, then rewrites the upload ids in every stream.

Candidates are found without comparing all pairs: tiles are bucketed by a
coarse signature (each 2x4 block reduced to its most common pixel), so only
tiles that already look alike are compared.

    dictmerge.py gen/fmv.pkl --out gen/fmv_merged.pkl --max-diff 2
"""
import argparse, os, pickle, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import fmvenc as F


def signature(t):
    """coarse 4x2 fingerprint of an 8x8 tile"""
    a = t.reshape(8, 8)
    sig = []
    for by in range(4):
        for bx in range(2):
            blk = a[by * 2:by * 2 + 2, bx * 4:bx * 4 + 4].ravel()
            vals, counts = np.unique(blk, return_counts=True)
            sig.append(int(vals[counts.argmax()]))
    return bytes(sig)


def merge(tiles, max_diff):
    arr = np.array([np.frombuffer(t, np.uint8) for t in tiles])
    buckets = {}
    remap = np.arange(len(tiles))
    keep = []
    for i in range(len(tiles)):
        sig = signature(arr[i])
        bucket = buckets.setdefault(sig, [])
        hit = None
        if bucket:
            cand = np.array(bucket)
            d = (arr[cand] != arr[i]).sum(1)
            j = int(d.argmin())
            if d[j] <= max_diff:
                hit = int(cand[j])
        if hit is None:
            remap[i] = len(keep)
            bucket.append(i)
            keep.append(i)
        else:
            remap[i] = remap[hit]
    return [tiles[i] for i in keep], remap


def rewrite(stream, remap):
    out = []
    for chunk in stream:
        b = bytearray(chunk)
        i = 0
        while i < len(b):
            op = b[i]
            if op in (F.OP_TICK, F.OP_DISPOFF, F.OP_DISPON, F.OP_CLEAR, F.OP_END, F.OP_NEXTBANK):
                n = 1
            elif op == F.OP_WAIT:
                n = 2
            elif op in (F.OP_PALBG, F.OP_PALSPR):
                n = 17
            elif op & 0xF0 == F.OP_UPLOAD:
                old = b[i + 2] | (b[i + 3] << 8)
                new = int(remap[old])
                b[i + 2], b[i + 3] = new & 0xFF, new >> 8
                n = 4
            elif op & 0xC0 == F.OP_NTRUN:
                n = 3 + 2 * ((op & 0x3F) + 1)
            else:
                raise ValueError(f'bad op {op:#x}')
            i += n
        out.append(bytes(b))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fmv')
    ap.add_argument('--out', required=True)
    ap.add_argument('--max-diff', type=int, default=2)
    a = ap.parse_args()
    d = pickle.load(open(a.fmv, 'rb'))
    tiles, remap = merge(d['dict'], a.max_diff)
    before, after = len(d['dict']), len(tiles)
    clips = [(cid, rewrite(stream, remap)) for cid, stream in d['clips']]
    pickle.dump(dict(dict=tiles, clips=clips), open(a.out, 'wb'))
    print(f'dictionary {before} -> {after} tiles '
          f'({before * 32 / 1024:.0f} KB -> {after * 32 / 1024:.0f} KB, '
          f'saved {(before - after) * 32 / 1024:.0f} KB) at max {a.max_diff} differing pixels')


if __name__ == '__main__':
    main()
