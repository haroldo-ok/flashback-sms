#!/usr/bin/env python3
"""Build a sprite table keyed by animation number and mirror flag, for drawing objects that the
ported game logic simulates (rather than replaying recorded frames).

For every (animation number, facing) pair the engine drew, the least-clipped
example is taken from the trace and stored as 8x16 sprite tiles positioned
relative to the object's own position, so the runtime can draw any object from
its simulated state alone:

    for each active object: entry = anim_table[anim_number][facing]
                            for tile in entry: sprite at (pos_x+dx, pos_y+dy)

    animconv.py capture/trace_D0.fbt --out gen/anim_D0.pkl
"""
import argparse, os, pickle, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from tracefmt import read_fbt
from fbfmt import to_sms_rgb

_c = np.arange(64)
_rgb = np.stack([_c & 3, (_c >> 2) & 3, (_c >> 4) & 3], 1)
DIST = ((_rgb[:, None, :] - _rgb[None, :, :]) ** 2).sum(2)


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--out', required=True)
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--sets', action='store_true', help='full-game sprite dump keyed by set')
    a = ap.parse_args()

    best = {}          # (anim, facing) -> (area, pixels(c6), dx, dy)
    hist = {}
    for f in read_fbt(a.trace, a.frames or None):
        # full-game sprite dump: the frame's room byte carries the sprite set
        # (0 Conrad, 1..4 monsters, 10+level objects); objects are the same
        # global sprites in every level, so they collapse to one set (10)
        setid = f['room'] if a.sets else None
        if setid is not None and setid >= 10:
            if setid != 10:
                continue

        rgb2 = to_sms_rgb(f['pal'])
        c6lut = (rgb2[:, 0] | (rgb2[:, 1] << 2) | (rgb2[:, 2] << 4)).astype(np.int32)
        for p in f['pieces']:
            # characters (sprite data) and objects (level sprites) are numbered
            # in the SAME space, so the kind is part of the key
            key = ((setid, p['anim'], (p['pge_flags'] & 2) >> 1) if setid is not None
                   else (p['anim'], (p['pge_flags'] & 2) >> 1, (p['pge_flags'] & 8) >> 3))
            area = int((p['pix'] != 0).sum())
            if area == 0:
                continue
            for v in np.unique(p['pix'][p['pix'] != 0]):
                cv = int(c6lut[int(v) | p['colmask']])
                hist[cv] = hist.get(cv, 0) + 1
            if key not in best or area > best[key][0]:
                img = np.where(p['pix'] == 0, -1, c6lut[p['pix'].astype(np.int32) | p['colmask']])
                best[key] = (area, img, p['x'] - p['pge_x'], p['y'] - p['pge_y'])

    pal = [c for c, _ in sorted(hist.items(), key=lambda x: -x[1])][:15]
    pal = np.array([0] + pal + [0] * (15 - len(pal)), np.int32)

    tiles, tile_id = [], {}
    entries = {}
    for key, (area, img, dx, dy) in sorted(best.items()):
        h, w = img.shape
        th, tw = (h + 15) // 16, (w + 7) // 8
        pad = np.full((th * 16, tw * 8), -1, np.int32)
        pad[:h, :w] = img
        idx = np.where(pad < 0, 0, (DIST[:, pal[1:]].argmin(1) + 1)[np.clip(pad, 0, 63)])
        parts = []
        for j in range(th):
            for i in range(tw):
                t = idx[j * 16:j * 16 + 16, i * 8:i * 8 + 8].astype(np.uint8)
                if not t.any():
                    continue
                k = t.tobytes()
                if k not in tile_id:
                    tile_id[k] = len(tiles)
                    tiles.append(planar(t.ravel()[:64]) + planar(t.ravel()[64:]))
                ox, oy = dx + i * 8, dy + j * 16
                if -128 <= ox <= 127 and -128 <= oy <= 127:
                    parts.append((tile_id[k], ox, oy))
        if parts:
            entries[key] = parts

    anims = sorted({(k[1] if a.sets else k[0]) for k in entries})
    sizes = [len(v) for v in entries.values()]
    print(f'{len(entries)} (anim, mirror, kind) entries over {len(anims)} animation numbers, '
          f'max anim number {max(anims)}')
    print(f'{len(tiles)} distinct 8x16 sprites = {len(tiles) * 64} bytes; '
          f'sprites per entry mean {np.mean(sizes):.1f} max {max(sizes)}')
    pickle.dump(dict(entries=entries, tiles=tiles, palette=bytes(int(c) for c in pal),
                     max_anim=max(anims)), open(a.out, 'wb'))


if __name__ == '__main__':
    main()
