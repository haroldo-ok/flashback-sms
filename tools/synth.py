#!/usr/bin/env python3
"""Generate stand-in captures in fbdump's formats so the SMS pipeline can be
built and verified without the Flashback data.  Content is deliberately
Flashback-*shaped* (flat-shaded polygon shots in a 240x128 clip at y=50, a
caption strip below, 32-colour Amiga-style palettes that change per shot,
~12 fps; rooms built from flipped 8x8 4bpp tiles with a foreground bit), but
it is original placeholder art.

    synth.py OUTDIR
"""
import math, os, random, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, os.path.dirname(__file__))
from fbfmt import write_fbv, write_fbr

rng = random.Random(1992)


def amiga_pal(colors12):
    pal = np.zeros((256, 3), np.uint8)
    for i, c in enumerate(colors12):
        pal[i] = [((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17]
    return pal


def shot_palette(seed):
    r = random.Random(seed)
    base = [0x000]
    hue = r.random()
    for i in range(15):                                # a shaded ramp per shot
        k = (i + 1) / 15
        cr = int(15 * k * (0.5 + 0.5 * math.sin(6.28 * hue)))
        cg = int(15 * k * (0.5 + 0.5 * math.sin(6.28 * (hue + .33))))
        cb = int(15 * k * (0.5 + 0.5 * math.sin(6.28 * (hue + .66))))
        base.append((cr << 8) | (cg << 4) | cb)
    accents = [r.randrange(0x1000) for _ in range(16)]
    return base + accents


TEXT = [0x000, 0xFFF, 0xCCC, 0x888]


def clip(num_frames, seed, captions):
    font = ImageFont.load_default()
    frames, t = [], 0
    shots = [(0, seed), (num_frames // 3, seed + 1), (2 * num_frames // 3, seed + 2)]
    for f in range(num_frames):
        shot = max(s for s in shots if s[0] <= f)
        cols = shot_palette(shot[1])
        pal = amiga_pal([0] * 0xC0 + cols + TEXT)
        img = Image.new('P', (256, 224), 0xC0)
        d = ImageDraw.Draw(img)
        lf = f - shot[0]
        # background bands (sky / floor) inside the 240x128 clip
        for b in range(8):
            d.rectangle([8, 50 + b * 16, 247, 50 + b * 16 + 15], fill=0xC1 + b)
        # a few rotating, scaling convex polygons
        for k in range(4):
            cx = 128 + 70 * math.sin(lf * 0.03 + k * 1.7)
            cy = 114 + 30 * math.cos(lf * 0.05 + k)
            rad = 18 + 10 * math.sin(lf * 0.07 + k)
            n = 3 + (k % 4)
            pts = [(cx + rad * math.cos(lf * 0.08 * (k + 1) + j * 6.283 / n),
                    cy + rad * math.sin(lf * 0.08 * (k + 1) + j * 6.283 / n)) for j in range(n)]
            d.polygon(pts, fill=0xCA + k * 2)
            d.polygon([(x * .6 + cx * .4, y * .6 + cy * .4) for x, y in pts], fill=0xD0 + k)
        # clip mask: nothing outside 240x128@(8,50)
        a = np.array(img)
        mask = np.zeros_like(a, bool)
        mask[50:178, 8:248] = True
        a[~mask] = 0xC0
        for (start, end, text) in captions:
            if start <= f < end:
                ci = Image.fromarray(a, 'P')
                ImageDraw.Draw(ci).text((24, 188), text, font=font, fill=0xE1)
                a = np.array(ci)
        frames.append((t, pal, a))
        t += 83                                        # frameDelay 5 @ 60 Hz
    return frames


def rooms(n):
    tiles = []
    for i in range(180):                               # an 8x8 4bpp tileset
        tt = np.zeros((8, 8), np.uint8)
        kind = i % 6
        c1, c2 = rng.randrange(1, 16), rng.randrange(1, 16)
        for y in range(8):
            for x in range(8):
                if kind == 0: v = c1
                elif kind == 1: v = c1 if (x + y) % 4 else c2
                elif kind == 2: v = c1 if y < 3 + (x % 3) else c2
                elif kind == 3: v = c2 if (x * x + y * y) < 30 else c1
                elif kind == 4: v = c1 if (x ^ y) & 2 else c2
                else: v = rng.choice([c1, c2, c1])
                tt[y, x] = v
        tiles.append(tt)
    out = []
    for r in range(n):
        pix = np.zeros((224, 256), np.uint8)
        for ty in range(28):
            for tx in range(32):
                if ty in (8, 17, 26):                  # three floors
                    t = tiles[r % 30]
                else:
                    t = tiles[rng.randrange(len(tiles))]
                if rng.random() < .3: t = t[:, ::-1]
                if rng.random() < .2: t = t[::-1, :]
                v = t.copy()
                if ty in (6, 7) and 10 <= tx < 14:      # a foreground pillar
                    v = v | 0x80
                pix[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8] = v
        pal = amiga_pal([rng.randrange(0x1000) for _ in range(16)] * 16)
        out.append(dict(room=r, slots=(r % 4, 1, 2, 3), pal=pal, pix=pix))
    return out


def main(out):
    os.makedirs(out, exist_ok=True)
    write_fbv(f'{out}/cut_0D.fbv', clip(240, 10, [(20, 90, 'MY NAME IS CONRAD B. HART'), (120, 200, 'I HAVE LOST MY MEMORY...')]))
    write_fbv(f'{out}/cut_40.fbv', clip(180, 40, [(30, 120, 'PLACEHOLDER LOGO SEQUENCE')]))
    write_fbv(f'{out}/cut_01.fbv', clip(200, 77, []))
    write_fbr(f'{out}/rooms.fbr', rooms(12))
    print('synthetic captures written to', out)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'capture')
