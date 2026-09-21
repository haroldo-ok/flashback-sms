#!/usr/bin/env python3
"""Vector-quantize a Flashback room's 8x8 cells into an SMS-sized tile codebook."""
import sys, os

def read_pgm(p):
    f = open(p, 'rb'); t = f.read().split(None, 3)
    return int(t[1]), int(t[2]), t[3]

def cells_of(d, w, h):
    out = []
    for ty in range(h // 8):
        for tx in range(w // 8):
            t = bytes(d[(ty * 8 + r) * w + tx * 8:(ty * 8 + r) * w + tx * 8 + 8] for r in range(8))
            out.append((ty, tx, t))
    return out

def dist(a, b):
    s = 0
    for i in range(64):
        s += abs(a[i] - b[i])
    return s

def quantize(cells, maxtiles):
    # exact-match dedup first
    proto = {}          # canonical tile -> id
    plist = []          # id -> tile bytes
    cellmap = []        # per cell: id
    for (ty, tx, t) in cells:
        if t not in proto:
            proto[t] = len(plist); plist.append(t)
        cellmap.append(proto[t])
    # greedy merge: repeatedly merge the nearest pair until <= maxtiles
    n = len(plist)
    if n > maxtiles:
        # precompute nearest for speed using coarse bucketing
        while n > maxtiles:
            # find nearest pair (sample for speed)
            best = None; bi = -1; bj = -1
            step = max(1, n // 200)
            for i in range(0, n, 1):
                a = plist[i]
                if a is None: continue
                for j in range(i + 1, min(i + 40, n)):
                    b = plist[j]
                    if b is None: continue
                    dd = dist(a, b)
                    if best is None or dd < best:
                        best = dd; bi = i; bj = j
            # merge j into i
            for c in range(len(cellmap)):
                if cellmap[c] == bj: cellmap[c] = bi
            plist[bj] = None
            n -= 1
    # compact ids
    remap = {}
    comp = []
    for i, t in enumerate(plist):
        if t is not None:
            remap[i] = len(comp); comp.append(t)
    cellmap = [remap[c] for c in cellmap]
    return comp, cellmap
