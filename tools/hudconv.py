#!/usr/bin/env python3
"""Item icons, the menu font and the level texts, for the in-game item name
display and the inventory screen.

Read straight from the game's files:
  global.icn   16x16 icons, 4 bits per pixel, behind a u16 offset table
  fb_txt.fnt   8x8 glyphs, 4 bits per pixel, 32 bytes each, from ' ' (0x20)
  level*.tbn   each level part's strings, behind a u16 offset table

Colours: icons are drawn by the engine with palette slot 0xA (captured by
`fbdump hudpal`).  They are mapped onto the SPRITE palette, because the item
name shows over gameplay, where the background palette belongs to the room -
background cells can select the sprite palette.  The inventory screen loads
the same palette, so an icon looks the same in both places.  Text is one colour,
the sprite-palette entry nearest the engine's text colour (0xE6).

    hudconv.py DATA capfull/hudpal.bin gen/full_sprsets.pkl capfull/level_L*.fbl --out gen/hud.pkl
"""
import argparse, os, pickle, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from levelconv import read_fbl
import tilepack as TP

TBN = ['level1', 'level2', 'level3', 'level4_1', 'level4_2', 'level5_1', 'level5_2']


def c6_rgb(c):
    return np.array([(c & 3) * 85, ((c >> 2) & 3) * 85, ((c >> 4) & 3) * 85], np.int32)


def nearest(rgb, pal_c6):
    best, bi = 1 << 30, 1
    for i in range(1, 16):
        d = int(((c6_rgb(pal_c6[i]) - rgb) ** 2).sum())
        if d < best:
            best, bi = d, i
    return bi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('data')
    ap.add_argument('hudpal')
    ap.add_argument('sprsets')
    ap.add_argument('levels', nargs='+')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    pal = open(a.hudpal, 'rb').read()
    spal = list(pickle.load(open(a.sprsets, 'rb'))['palette'])
    rgb = lambda i: np.array(list(pal[i * 3:i * 3 + 3]), np.int32)
    # engine icon colour k -> sprite palette index (0 stays transparent)
    icon_map = [0] + [nearest(rgb(0xA0 + k), spal) for k in range(1, 16)]
    text_col = nearest(rgb(0xE6), spal)

    # which icons objects use: colliding_icon_num - 1 (the name display) and
    # icon_num (the inventory), across all levels
    used = set()
    for path in a.levels:
        lv = read_fbl(path)
        A = lv['pges']
        for i in range(lv['npges']):
            p = i * 31
            if A[p + 22]:
                used.add(A[p + 22] - 1)
            used.add(A[p + 23])
    icn = open(os.path.join(a.data, 'global.icn'), 'rb').read()
    n_icons = struct.unpack_from('<H', icn, 0)[0] // 2
    icons = {}
    for num in sorted(u for u in used if u < n_icons):
        off = struct.unpack_from('<H', icn, num * 2)[0]
        raw = icn[off + 2:off + 2 + 128]
        if len(raw) < 128:
            continue
        px = np.zeros(256, np.uint8)
        for i, b in enumerate(raw):
            px[2 * i] = icon_map[b >> 4]
            px[2 * i + 1] = icon_map[b & 15]
        img = px.reshape(16, 16)
        # four 8x8 tiles: top-left, top-right, bottom-left, bottom-right
        icons[num] = [img[y:y + 8, x:x + 8].ravel() for y in (0, 8) for x in (0, 8)]

    # the texts
    texts = []
    chars = set()
    for name in TBN:
        d = open(os.path.join(a.data, name + '.tbn'), 'rb').read()
        count = struct.unpack_from('<H', d, 0)[0] // 2
        strs = []
        for k in range(count):
            off = struct.unpack_from('<H', d, k * 2)[0]
            end = d.index(b'\0', off) if b'\0' in d[off:] else len(d)
            s = bytes(c for c in d[off:end] if 0x20 <= c < 0x7F)
            strs.append(s)
            chars |= set(s)
        texts.append(strs)

    # the glyphs the texts use, 1 bit per pixel
    fnt = open(os.path.join(a.data, 'fb_txt.fnt'), 'rb').read()
    glyphs = {}
    for c in sorted(chars | {ord(' ')}):
        g = fnt[(c - 0x20) * 32:(c - 0x20) * 32 + 32]
        rows = []
        for y in range(8):
            bits = 0
            for x in range(8):
                v = (g[y * 4 + (x >> 1)] >> (4 if x % 2 == 0 else 0)) & 15
                if v:
                    bits |= 0x80 >> x
            rows.append(bits)
        glyphs[c] = bytes(rows)

    print(f'icons: {len(icons)} used (of {n_icons}); glyphs: {len(glyphs)}; '
          f'texts: {sum(len(t) for t in texts)} strings in {len(texts)} level parts, '
          f'{sum(len(s) for t in texts for s in t)} characters')
    print(f'text colour -> sprite palette entry {text_col}')
    pickle.dump(dict(icons=icons, glyphs=glyphs, texts=texts, text_col=text_col),
                open(a.out, 'wb'))


if __name__ == '__main__':
    main()
