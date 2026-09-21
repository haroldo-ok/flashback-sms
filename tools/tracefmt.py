#!/usr/bin/env python3
"""Read gameplay traces (fbdump replay) and measure what they ask of the SMS.

    tracefmt.py capture/trace_D0.fbt [--frames N]

Reports, per trace: sprite pieces per frame, the 8x8 sprite tiles a frame
needs, hardware sprites per scanline (the VDP shows 8 per line and drops the
rest), and the colours the characters/objects use (the sprite palette holds
15 plus transparent).
"""
import argparse, os, sys
import numpy as np

W, H = 256, 224
# record: 'G' u32 frame, u8 level, u8 room, i16 x, i16 y, pal[768], layer,
# u16 npges, npges*12 object state, u8 input, u16 npieces, pieces


def read_fbt(path, max_frames=None):
    data = np.fromfile(path, np.uint8)
    assert data[:4].tobytes() == b'FBT1', path
    p = 4
    n = 0
    while p < len(data):
        if data[p] != ord('G'):
            break
        frame = int(data[p + 1:p + 5].view('<u4')[0])
        level, room = int(data[p + 5]), int(data[p + 6])
        cx, cy = data[p + 7:p + 11].view('<i2')
        pal = data[p + 11:p + 779].reshape(256, 3)
        layer = data[p + 779:p + 779 + W * H].reshape(H, W)
        q = p + 779 + W * H
        npges = int(data[q:q + 2].view('<u2')[0])
        q += 2
        pges = data[q:q + npges * 12].reshape(npges, 12)   # per-object state
        q += npges * 12
        demo_input = int(data[q]); q += 1
        npieces = int(data[q:q + 2].view('<u2')[0])
        q += 2
        pieces = []
        for _ in range(npieces):
            variant, colmask, x, y, w, h = (int(v) for v in data[q:q + 6])
            q += 6
            pge_index, pge_flags = int(data[q]), int(data[q + 1])
            anim = int(data[q + 2:q + 4].view('<u2')[0])
            pge_x, pge_y = (int(v) for v in data[q + 4:q + 8].view('<i2'))
            q += 8
            pix = data[q:q + w * h].reshape(h, w)
            q += w * h
            pieces.append(dict(variant=variant, colmask=colmask, x=x, y=y, w=w, h=h, pix=pix,
                               pge_index=pge_index, pge_flags=pge_flags, anim=anim,
                               pge_x=pge_x, pge_y=pge_y))
        yield dict(frame=frame, level=level, room=room, conrad=(int(cx), int(cy)),
                   pal=pal, layer=layer, pieces=pieces, pges=pges, demo_input=demo_input)
        p = q
        n += 1
        if max_frames and n >= max_frames:
            return


def frame_tiles(f, scroll=0):
    """8x8 sprite cells a frame needs (piece pixels placed on the screen grid)."""
    cells = {}
    for p in f['pieces']:
        for j in range(p['h']):
            y = p['y'] + j - scroll
            if not 0 <= y < 192:
                continue
            row = p['pix'][j]
            for i in range(p['w']):
                v = row[i]
                if v:
                    x = p['x'] + i
                    if 0 <= x < 256:
                        cells.setdefault((x >> 3, y >> 3), np.zeros((8, 8), np.uint8))[y & 7, x & 7] = v | p['colmask']
    return cells


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('traces', nargs='+')
    ap.add_argument('--frames', type=int, default=400)
    a = ap.parse_args()
    for path in a.traces:
        pieces_n, tiles_n, colors = [], [], {}
        perline = []
        rooms = set()
        for f in read_fbt(path, a.frames):
            rooms.add((f['level'], f['room']))
            pieces_n.append(len(f['pieces']))
            cells = frame_tiles(f)
            tiles_n.append(len(cells))
            # hardware sprites per scanline: each cell is an 8x8 sprite
            rows = {}
            for (cx, cy) in cells:
                for k in range(8):
                    rows[cy * 8 + k] = rows.get(cy * 8 + k, 0) + 1
            perline.append(max(rows.values()) if rows else 0)
            for t in cells.values():
                for v in np.unique(t):
                    if v:
                        colors[int(v)] = colors.get(int(v), 0) + 1
        pn, tn, pl = np.array(pieces_n), np.array(tiles_n), np.array(perline)
        print(f'\n{os.path.basename(path)}: {len(pn)} frames, rooms visited {len(rooms)}')
        print(f'  pieces/frame   mean {pn.mean():5.1f} max {pn.max()}')
        print(f'  sprite tiles/frame mean {tn.mean():5.1f} max {tn.max()}  (VRAM pool + 64 sprite limit)')
        print(f'  worst sprites on one scanline: mean {pl.mean():4.1f} max {pl.max()}  (VDP shows 8)')
        print(f'  frames over 8 on a line: {100 * (pl > 8).mean():.0f}%   over 64 tiles: {100 * (tn > 64).mean():.0f}%')
        slots = sorted({v >> 4 for v in colors})
        print(f'  palette slots used by sprites: {slots}')
        print(f'  distinct sprite colour values: {len(colors)}')


if __name__ == '__main__':
    main()
