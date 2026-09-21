#!/usr/bin/env python3
"""Collapse interlaced dither flicker in the extracted FMV frames.

The DOS demo draws the big cartoon graphics with a *binary dither* (a 50%
checkerboard of two palette entries) and animates by flipping the phase of that
checkerboard every other frame.  At the original ~15 fps that reads as a shade;
replayed at 50/60 fps on the Master System it reads as a strobe, and it also
costs the encoder an update of every dithered cell on every frame.

This pass finds pixels that alternate between two palette entries
(A, B, A ...) and, when the *average* of the two colours exists in the
cutscene's own palette (within a tolerance), replaces the alternation with that
single stable colour.

Usage: deflicker.py <frames-dir> <out-dir> [--tol 48]
"""
import argparse
import os
import sys

import numpy as np

W, H = 256, 96


def sms_rgb_table():
    """SMS 2-bit-per-channel colour space: index -> (r,g,b) in 0..3."""
    return np.array([[(i & 3), ((i >> 2) & 3), ((i >> 4) & 3)] for i in range(16)],
                    dtype=np.int32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frames_dir')
    ap.add_argument('out_dir')
    ap.add_argument('--tol', type=int, default=48,
                    help='max per-channel error when snapping the average to a '
                         'palette entry (in 0..255 units)')
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)

    lut = sms_rgb_table()                       # (16,3) 0..3
    meta = [l.split() for l in open(os.path.join(a.frames_dir, 'cutscene_meta.txt'))
            if l.strip()]
    for name, nframes, delay, snapivl in meta:
        n = int(nframes)
        raw = np.frombuffer(open(os.path.join(a.frames_dir, f'frames_{name}.bin'),
                                 'rb').read()[:n * W * H],
                            dtype=np.uint8).reshape(n, H, W).astype(np.uint8)
        out = raw.copy()
        collapsed = 0
        for f in range(n - 1):
            prev = raw[f - 1] if f else raw[0]
            cur = raw[f]
            nxt = raw[f + 1]
            mask = (prev == nxt) & (cur != prev)
            if not mask.any():
                continue
            idx = np.nonzero(mask)
            A = cur[idx].astype(np.int32)         # e.g. white
            B = prev[idx].astype(np.int32)        # e.g. black
            avg = (lut[A] + lut[B]) * 0.5         # 0..3 scale
            # nearest palette entry to the average (in 0..3 units)
            d = np.abs(lut[None, :, :] - avg[:, None, :]).sum(axis=2)
            best = d.argmin(axis=1)
            ok = d[np.arange(len(best)), best] <= (a.tol / 85.0)
            sel = (idx[0][ok], idx[1][ok])
            c = best[ok].astype(np.uint8)
            out[f][sel] = c
            out[f + 1][sel] = c
            collapsed += int(ok.sum())
        open(os.path.join(a.out_dir, f'frames_{name}.bin'), 'wb').write(out.tobytes())
        open(os.path.join(a.out_dir, f'pal_{name}.bin'), 'wb').write(
            open(os.path.join(a.frames_dir, f'pal_{name}.bin'), 'rb').read())
        print(f'{name:9s} {n:4d} frames  dither pixels collapsed: {collapsed:9d}')
    # copy the metadata through
    open(os.path.join(a.out_dir, 'cutscene_meta.txt'), 'w').write(
        open(os.path.join(a.frames_dir, 'cutscene_meta.txt')).read())
    print(f'-> {a.out_dir}')


if __name__ == '__main__':
    main()
