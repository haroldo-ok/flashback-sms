#!/usr/bin/env python3
"""Cut the captured "LEVEL 1..5" strips into SMS tiles for the level select.

The title screen is a picture with its text baked in, so the selector needs
text of its own.  Each strip is one row of cells; the tiles are shared between
strips (the word LEVEL is the same in all five), and they are drawn with the
SPRITE palette - background cells can select it per cell - so the text keeps
its own colours over the title picture.

    menuconv.py capfull/menutext.fbr --out gen/menu.pkl
"""
import argparse, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from fbfmt import read_fbr, to_sms_rgb
import tilepack as TP

CELLS = 10          # 80 pixels: "LEVEL n" with room to spare
X0, Y0, DY = 16, 16, 16


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    r = read_fbr(a.capture)[0]
    rgb2 = to_sms_rgb(r['pal'])
    c6 = (rgb2[:, 0] | (rgb2[:, 1] << 2) | (rgb2[:, 2] << 4)).astype(np.int32)

    used = sorted({int(c6[v]) for v in np.unique(r['pix']) if v != 0})
    pal = [0] + used[:15]
    pal += [0] * (16 - len(pal))
    lut = {c: i for i, c in enumerate(pal)}

    tiles, index, strips = [], {}, []
    for n in range(5):
        cells = []
        for cx in range(CELLS):
            block = r['pix'][Y0 + n * DY:Y0 + n * DY + 8, X0 + cx * 8:X0 + cx * 8 + 8]
            idx = np.zeros((8, 8), np.uint8)
            for y in range(8):
                for x in range(8):
                    v = int(block[y, x])
                    idx[y, x] = 0 if v == 0 else lut.get(int(c6[v]), 1)
            key = idx.tobytes()
            if key not in index:
                index[key] = len(tiles)
                tiles.append(TP.planar(idx.ravel()))
            cells.append(index[key])
        strips.append(cells)
    print(f'level select: {len(tiles)} tiles for 5 strips of {CELLS} cells, '
          f'{len(tiles) * 32} bytes; palette {len([c for c in pal if c])} colours')
    import pickle
    pickle.dump(dict(tiles=tiles, strips=strips, palette=bytes(pal), cells=CELLS),
                open(a.out, 'wb'))


if __name__ == '__main__':
    main()
