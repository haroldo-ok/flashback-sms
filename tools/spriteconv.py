#!/usr/bin/env python3
"""Gameplay trace -> SMS sprite tiles + a replay command stream.

The engine draws Conrad, monsters and objects as software blits into one
layer.  On the SMS those become hardware sprites:

* the screen is cut into 8x8 cells; every cell a frame needs becomes one 8x8
  hardware sprite (the VDP allows 64, and shows 8 per scanline)
* cell patterns are deduplicated into a global dictionary in ROM, and VRAM
  slots 0..63 are a cache the encoder simulates, exactly like the cutscene
  player does for backgrounds
* each room gets a sprite palette of 15 colours (index 0 is transparent),
  built from the colours its characters and objects actually use
* when more than 8 sprites land on a scanline the VDP drops the extras, so
  the sprite order is rotated each frame except for Conrad, who stays first and
  is never dropped; the encoder reports how often this happens
* the background scrolls 0..32 lines to keep Conrad on screen

    spriteconv.py capture/trace_D0.fbt --rooms gen/rooms_L0.pkl --level 0 \
                  --out gen/replay_D0.pkl [--frames N]
"""
import argparse, os, pickle, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from tracefmt import read_fbt
from fbfmt import to_sms_rgb

SPRITE_SLOTS = 32          # 8x16 sprites: 32 tile PAIRS = VRAM tiles 0..63
MAX_SPRITES = 64           # VDP sprite limit
MAX_UPLOADS = 32           # tile uploads per game frame (30 Hz -> 2 SMS frames)
SCREEN_H = 192
SCROLL_MAX = 32

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


def frame_canvas(f, scroll):
    """paste every piece, in engine order -> (192,256) engine colour values"""
    c = np.zeros((SCREEN_H, 256), np.uint8)
    for p in f['pieces']:
        y0 = p['y'] - scroll
        x0 = p['x']
        sy0, sy1 = max(0, -y0), min(p['h'], SCREEN_H - y0)
        sx0, sx1 = max(0, -x0), min(p['w'], 256 - x0)
        if sy1 <= sy0 or sx1 <= sx0:
            continue
        sub = p['pix'][sy0:sy1, sx0:sx1]
        dst = c[y0 + sy0:y0 + sy1, x0 + sx0:x0 + sx1]
        m = sub != 0
        dst[m] = (sub[m] | p['colmask'])
    return c


def scroll_for(f, prev):
    """keep Conrad in view; move at most 4 lines per frame"""
    ys = [p['y'] + p['h'] // 2 for p in f['pieces'] if p['colmask'] == 0x40]
    target = 0 if not ys else int(np.clip(np.mean(ys) - SCREEN_H // 2, 0, SCROLL_MAX))
    return int(np.clip(target, prev - 4, prev + 4))


def main():
    global MAX_UPLOADS, SPRITE_SLOTS
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--rooms', required=True)
    ap.add_argument('--level', type=int, required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--max-uploads', type=int, default=MAX_UPLOADS)
    ap.add_argument('--slots', type=int, default=SPRITE_SLOTS)
    a = ap.parse_args()

    MAX_UPLOADS, SPRITE_SLOTS = a.max_uploads, a.slots
    rooms = pickle.load(open(a.rooms, 'rb'))
    room_order = {r['room']: i for i, r in enumerate(rooms)}

    frames = list(read_fbt(a.trace, a.frames or None))
    print(f'{len(frames)} game frames')

    # ---- pass 1: a sprite palette per room, from the colours actually used --
    per_room = {}
    scroll = 0
    for f in frames:
        scroll = scroll_for(f, scroll)
        c = frame_canvas(f, scroll)
        rgb2 = to_sms_rgb(f['pal'])
        c6lut = (rgb2[:, 0] | (rgb2[:, 1] << 2) | (rgb2[:, 2] << 4)).astype(np.int32)
        vals, counts = np.unique(c[c != 0], return_counts=True)
        d = per_room.setdefault(f['room'], {})
        for v, n in zip(vals, counts):
            d[int(c6lut[v])] = d.get(int(c6lut[v]), 0) + int(n)
    palettes = {}
    for room, hist in per_room.items():
        cols = [c for c, _ in sorted(hist.items(), key=lambda x: -x[1])][:15]
        palettes[room] = np.array([0] + cols + [0] * (15 - len(cols)), np.int32)
        lost = len(hist) - len(cols)
        if lost:
            print(f'  room {room}: {len(hist)} sprite colours -> 15 ({lost} merged)')

    # ---- pass 2: cells, dictionary, VRAM cache, sprite lists ----------------
    dict_tiles, dict_id = [], {}
    slot_pat = [None] * SPRITE_SLOTS
    slot_of, slot_used = {}, np.zeros(SPRITE_SLOTS, np.int64)
    stream = []
    cur_room = None
    scroll = 0
    stats = dict(cells=[], drops=0, overline=0, uploads=[], missing=0, err=[])
    for fi, f in enumerate(frames):
        ops = bytearray()
        scroll = scroll_for(f, scroll)
        if f['room'] != cur_room:
            cur_room = f['room']
            slot_pat = [None] * SPRITE_SLOTS      # room load clears sprite VRAM use
            slot_of = {}
            ops += bytes([1, room_order[cur_room]])
            ops += bytes(int(x) for x in palettes[cur_room])   # sprite palette for this room
        ops += bytes([2, scroll])
        pal = palettes[cur_room]
        rgb2 = to_sms_rgb(f['pal'])
        c6lut = (rgb2[:, 0] | (rgb2[:, 1] << 2) | (rgb2[:, 2] << 4)).astype(np.int32)
        lut = (DIST[:, pal[1:]].argmin(1) + 1)            # c6 -> palette index 1..15
        # tiles are cut relative to each piece's own origin, so a piece that
        # only moves needs no upload at all (the VDP positions sprites by pixel)
        want = []                                  # (y, x, pattern) back to front
        for p in f['pieces']:
            py, px = p['y'] - scroll, p['x']
            q = np.where(p['pix'] == 0, 0, lut[c6lut[p['pix'] | p['colmask']]]).astype(np.uint8)
            th, tw = (p['h'] + 15) // 16, (p['w'] + 7) // 8   # 8x16 sprites
            pad = np.zeros((th * 16, tw * 8), np.uint8)
            pad[:p['h'], :p['w']] = q
            for j in range(th):
                for i in range(tw):
                    t = pad[j * 16:j * 16 + 16, i * 8:i * 8 + 8]
                    if not t.any():
                        continue
                    y, x = py + j * 16, px + i * 8
                    if y <= -16 or y >= SCREEN_H or x <= -8 or x >= 256:
                        continue
                    want.append((y, x, t.tobytes(), p['colmask'] == 0x40))
        stats['cells'].append(len(want))
        # the VDP gives priority to earlier sprites, so emit front-to-back:
        # Conrad first (never dropped), then the rest rotated each frame
        want = want[::-1]
        head = [w for w in want if w[3]]
        tail = [w for w in want if not w[3]]
        rot = fi % max(1, len(tail))
        order = head + tail[rot:] + tail[:rot]
        if len(order) > MAX_SPRITES:
            stats['drops'] += len(order) - MAX_SPRITES
            order = order[:MAX_SPRITES]
        # VRAM slot cache
        ups, sprites, used_now = [], [], set()
        for (y, x, pat, _is_conrad) in order:
            if pat not in dict_id:
                dict_id[pat] = len(dict_tiles)
                dict_tiles.append(pat)
            s = slot_of.get(pat)
            if s is None:
                if len(ups) >= MAX_UPLOADS:
                    stats['missing'] += 1
                    continue
                free = [i for i in range(SPRITE_SLOTS) if slot_pat[i] is None]
                if free:
                    s = free[0]
                else:                       # evict least recently used slot
                    cand = [i for i in range(SPRITE_SLOTS) if i not in used_now]
                    if not cand:
                        stats['missing'] += 1
                        continue
                    s = min(cand, key=lambda i: slot_used[i])
                    del slot_of[slot_pat[s]]
                slot_pat[s] = pat
                slot_of[pat] = s
                ups.append((s, dict_id[pat]))
            slot_used[s] = fi
            used_now.add(s)
            sprites.append((y, x, s))
        stats['uploads'].append(len(ups))
        # per-scanline check (the VDP shows 8; later sprites are dropped)
        rows = {}
        shown = []
        for (y, x, s) in sprites:
            full = all(rows.get(y + k, 0) < 8 for k in range(8))
            for k in range(8):
                rows[y + k] = rows.get(y + k, 0) + 1
            if full:
                shown.append((y, x, s))
        if len(shown) != len(sprites):
            stats['overline'] += 1
        stats.setdefault('dropped', []).append(len(sprites) - len(shown))
        if ups:
            ops += bytes([3, len(ups)])
            for s, tid in ups:
                ops += bytes([s, tid & 0xFF, tid >> 8])
        # the SAT stores y-1, so a sprite at y=0 would be written as 255 and
        # land on line 256: the hardware cannot show sprites on the top line
        sprites = [(y, x, s) for (y, x, s) in sprites if 1 <= y < SCREEN_H and 0 <= x < 256]
        ops += bytes([4, len(sprites)])
        for (y, x, s) in sprites:
            ops += bytes([y, x, s])
        ops += bytes([0])
        stream.append(bytes(ops))
    stream.append(bytes([0xFF]))

    cells_n = np.array(stats['cells'])
    ups_n = np.array(stats['uploads'])
    print(f"sprites/frame mean {cells_n.mean():.1f} max {cells_n.max()}; "
          f"uploads/frame mean {ups_n.mean():.1f} max {ups_n.max()}")
    dr = np.array(stats.get('dropped', [0]))
    print(f"sprites hidden by the 8-per-line limit: mean {dr.mean():.1f}/frame max {dr.max()}")
    print(f"frames losing sprites to the 8-per-line limit: {100 * stats['overline'] / len(frames):.0f}%; "
          f"cells skipped for VRAM/upload budget: {stats['missing']}; over the 64-sprite limit: {stats['drops']}")
    print(f"sprite dictionary: {len(dict_tiles)} tiles = {len(dict_tiles) * 32} bytes; "
          f"stream {sum(map(len, stream))} bytes")
    pickle.dump(dict(dict=[planar(np.frombuffer(t, np.uint8)[:64]) + planar(np.frombuffer(t, np.uint8)[64:])
                           for t in dict_tiles],
                     palettes={k: bytes(int(x) for x in v) for k, v in palettes.items()},
                     room_order=room_order, level=a.level, stream=stream),
                open(a.out, 'wb'))


if __name__ == '__main__':
    main()
