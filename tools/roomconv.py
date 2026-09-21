#!/usr/bin/env python3
"""Flashback room layers (fbdump rooms.fbr) -> SMS tilesets + 32x28 nametables.

A DOS-demo room is already composed of 8x8 4bpp tiles with H/V flip, so the
256x224 layer maps 1:1 onto the SMS nametable (which is 32x28 cells; 24 rows
are visible and the engine scrolls vertically by 0..32 px to follow Conrad).

* colours: pixel values index the engine palette (slot 0/8 = map palette 1,
  slot 9 = map palette 2).  Snapped to SMS 6-bit, reduced to the BG palette.
* bit 0x80 = foreground -> SMS tile priority bit (drawn over sprites; colour 0
  of a priority tile stays transparent to sprites).
* tiles are H/V-flip deduplicated; if a room still needs more than --max-tiles
  patterns, the closest pairs are merged (lossy, reported).

    roomconv.py capture/rooms.fbr --out gen/rooms.pkl [--max-tiles 384]
"""
import argparse, os, pickle, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from fbfmt import read_fbr, to_sms_rgb

_c = np.arange(64)
_rgb = np.stack([_c & 3, (_c >> 2) & 3, (_c >> 4) & 3], 1)
DIST = ((_rgb[:, None, :] - _rgb[None, :, :]) ** 2).sum(2)


def variants(p):
    return [p, p[:, ::-1], p[::-1, :], p[::-1, ::-1]]


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


def build_palette(c6img, n=16):
    """Index 0 is reserved and never used by a tile: on the SMS a priority
    (foreground) tile only hides a sprite where its pixel is not colour 0, so
    letting index 0 appear in artwork would make sprites show through
    foreground scenery.  Rooms therefore get 15 colours, in indices 1..15."""
    cols, counts = np.unique(c6img, return_counts=True)
    order = [int(c) for c, _ in sorted(zip(cols, counts), key=lambda x: -x[1])]
    pal = order[:n - 1]
    lost = order[n - 1:]
    pal = [0] + pal + [0] * (n - 1 - len(pal))
    return np.array(pal, np.int32), len(lost)


def convert_room(r, max_tiles, base=448 - 384):
    pix = r['pix']
    rgb2 = to_sms_rgb(r['pal'])
    c6lut = (rgb2[:, 0] | (rgb2[:, 1] << 2) | (rgb2[:, 2] << 4)).astype(np.int32)
    c6 = c6lut[pix]
    pal, lost = build_palette(c6)
    idx = (DIST[:, pal[1:]].argmin(1) + 1)[c6].astype(np.uint8)   # (224,256) 1..15
    cells = idx.reshape(28, 8, 32, 8).transpose(0, 2, 1, 3).reshape(896, 8, 8)
    prio = ((pix & 0x80) != 0).reshape(28, 8, 32, 8).transpose(0, 2, 1, 3).reshape(896, 64).mean(1) > 0.5

    keys, flips = [], []
    for c in cells:
        vs = [v.tobytes() for v in variants(c)]
        k = min(vs)
        keys.append(k); flips.append(vs.index(k))
    uniq = sorted(set(keys), key=keys.index)
    merged = 0
    if len(uniq) > max_tiles:
        # usage-weighted greedy merge in SMS colour space, flip-aware
        N = len(uniq)
        pats = np.array([np.frombuffer(k, np.uint8) for k in uniq]).reshape(N, 8, 8)
        col = pal[pats]                                  # (N,8,8) c6
        use = np.array([0] * N, np.int64)
        kidx = {k: i for i, k in enumerate(uniq)}
        for k in keys:
            use[kidx[k]] += 1
        D = np.full((N, N), 1 << 30, np.int64)
        DF = np.zeros((N, N), np.int8)
        A = col.reshape(N, 64)
        for f, fl in enumerate([col, col[:, :, ::-1], col[:, ::-1, :], col[:, ::-1, ::-1]]):
            B = fl.reshape(N, 64)
            for r0 in range(0, N, 64):
                d = DIST[A[r0:r0 + 64, None, :], B[None, :, :]].sum(-1)
                better = d < D[r0:r0 + 64]
                D[r0:r0 + 64] = np.where(better, d, D[r0:r0 + 64])
                DF[r0:r0 + 64] = np.where(better, f, DF[r0:r0 + 64])
        np.fill_diagonal(D, 1 << 30)
        alive = np.ones(N, bool)
        parent = {}
        while alive.sum() > max_tiles:
            Dm = np.where(alive[None, :], D, 1 << 30)
            nn = Dm.argmin(1)
            cost = np.where(alive, use * Dm[np.arange(N), nn], 1 << 62)
            i = int(cost.argmin()); j = int(nn[i])
            # variant DF[i,j] of pattern j ~= pattern i
            parent[i] = (j, int(DF[i, j]))
            alive[i] = False
            use[j] += use[i]
            merged += 1
        def resolve(i, f):
            while i in parent:
                j, f2 = parent[i]
                f ^= f2; i = j
            return i, f
        newkeys, newflips = [], []
        for k, f in zip(keys, flips):
            i, f2 = resolve(kidx[k], f)
            newkeys.append(uniq[i]); newflips.append(f2)
        keys, flips = newkeys, newflips
        uniq = [uniq[i] for i in range(N) if alive[i]]
    tid = {k: i for i, k in enumerate(uniq)}
    tiles = b''.join(planar(np.frombuffer(k, np.uint8)) for k in uniq)
    FL = [(0, 0), (1, 0), (0, 1), (1, 1)]
    nt = bytearray()
    for i in range(896):
        h, v = FL[flips[i]]
        e = (tid[keys[i]] + base) | (h << 9) | (v << 10) | (int(prio[i]) << 12)
        nt += bytes([e & 0xFF, e >> 8])
    sms_pal = bytes(int(c) for c in pal)
    # reconstruction error (for the report)
    rec = np.zeros((896, 8, 8), np.uint8)
    for i in range(896):
        p = np.frombuffer(keys[i], np.uint8).reshape(8, 8)
        rec[i] = variants(p)[flips[i]]
    rec = rec.reshape(28, 32, 8, 8).transpose(0, 2, 1, 3).reshape(224, 256)
    err = float((pal[rec] != c6).mean() * 100)
    return dict(rec=rec, c6=c6, pal=pal, room=r['room'], ntiles=len(uniq), tiles=tiles, nametable=bytes(nt),
                palette=sms_pal, err=err, lost_colors=lost, merged=merged)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fbr')
    ap.add_argument('--out', required=True)
    ap.add_argument('--max-tiles', type=int, default=384)
    ap.add_argument('--png')
    a = ap.parse_args()
    out = []
    seen = {}
    for r in read_fbr(a.fbr):
        h = hash(r['pix'].tobytes() + r['pal'].tobytes())
        if h in seen:                       # several room numbers share one image
            c = dict(seen[h]); c['room'] = r['room']; c['alias'] = seen[h]['room']
            out.append(c)
            continue
        c = convert_room(r, a.max_tiles, 448 - a.max_tiles)
        seen[h] = c
        if a.png:
            from PIL import Image
            rgb = lambda im: np.stack([(im & 3) * 85, ((im >> 2) & 3) * 85, ((im >> 4) & 3) * 85], -1).astype(np.uint8)
            os.makedirs(a.png, exist_ok=True)
            Image.fromarray(np.concatenate([rgb(c['c6']), rgb(c['pal'][c['rec']])], 1)).save(f"{a.png}/room_{os.path.basename(a.fbr)[6:8]}_{c['room']:02d}.png")
        print(f"room {c['room']:2d}: {c['ntiles']:3d} tiles, merged {c['merged']:3d}, "
              f"colours dropped {c['lost_colors']}, pixel err {c['err']:.2f}%")
        out.append(c)
    os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
    for c in out:
        c.pop('rec', None); c.pop('c6', None)
    pickle.dump(out, open(a.out, 'wb'))


if __name__ == '__main__':
    main()
