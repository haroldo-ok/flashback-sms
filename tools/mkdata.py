#!/usr/bin/env python3
"""Lay out FMV dictionary, FMV streams and rooms into ROM banks 2..255,
emit the C index, and verify the packed bytes by decoding them.

    mkdata.py pack   --fmv gen/fmv.pkl --rooms gen/rooms.pkl --out gen
    mkdata.py verify --out gen --capture capture [--png shots]
"""
import argparse, os, pickle, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import fmvenc as F

BANK = 16384
BANK0 = 2
MAX_BANKS = 256


class Blob:
    def __init__(self):
        self.data = bytearray()

    @property
    def bank(self):
        return BANK0 + len(self.data) // BANK

    @property
    def addr(self):
        return 0x8000 + len(self.data) % BANK

    def align_bank(self):
        if len(self.data) % BANK:
            self.data += bytes(BANK - len(self.data) % BANK)

    def room_left(self):
        return BANK - len(self.data) % BANK


def pack(a):
    fmv = pickle.load(open(a.fmv, 'rb')) if a.fmv else dict(dict=[], clips=[])
    rooms = []
    if a.title:                      # the title screen rides along as a room
        for r in pickle.load(open(a.title, 'rb')):
            r['level'] = 99
            rooms.append(r)
    for path in (a.rooms or []):
        lvl = int(os.path.basename(path).split('_L')[1].split('.')[0])
        for r in pickle.load(open(path, 'rb')):
            r['level'] = lvl
            rooms.append(r)
    b = Blob()
    # 3. rooms: packed first-fit, each record kept inside one bank
    room_tab = []
    placed = {}
    for r in rooms:
        key = (r['level'], r.get('alias', r['room']))
        if key not in placed:
            rec = bytes([r['ntiles'] & 0xFF, r['ntiles'] >> 8]) + r['palette'] + r['nametable'] + r['tiles']
            assert len(rec) <= BANK
            if len(rec) > b.room_left():
                b.align_bank()
            placed[key] = (b.bank, b.addr)
            b.data += rec
        room_tab.append((r['level'], r['room']) + placed[key])
    # 4. gameplay replay: sprite dictionary (64-byte 8x16 entries) + stream
    spr_bank0 = replay_bank = replay_addr = 0
    if a.sprites:
        rep = pickle.load(open(a.sprites, 'rb'))
        spr_bank0 = b.bank
        for t in rep['dict']:
            assert len(t) == 64
            b.data += t
        b.align_bank()
        replay_bank, replay_addr = b.bank, b.addr
        for chunk in rep['stream']:
            for op in split_replay_ops(chunk):
                if len(op) + 1 > b.room_left():
                    b.data += bytes([F.OP_NEXTBANK])
                    b.align_bank()
                b.data += op
        b.align_bank()
        print(f"sprite dictionary {len(rep['dict'])} entries from bank {spr_bank0}; "
              f"replay stream at bank {replay_bank} to {b.bank - 1}")
    # 5. level object tables for the game-logic port (bank-aligned parts)
    lvl_tab = []
    for path in (a.levels or []):
        lv = pickle.load(open(path, 'rb'))
        b.align_bank()
        a_bank = b.bank
        b.data += lv['partA']
        b.align_bank()
        o_bank = b.bank
        for blk in lv['objectBanks']:
            b.data += blk
        b.align_bank()
        ani_idx_bank = b.bank
        b.data += lv['aniIndex']
        b.align_bank()
        ani_bank = b.bank
        for blk in lv['aniBanks']:
            b.data += blk
        b.align_bank()
        ct_bank = b.bank
        b.data += lv['ct']
        lvl_tab.append((int(os.path.basename(path).split('_L')[1].split('.')[0]),
                        a_bank, o_bank, lv['npges'], lv['checksum'], ani_idx_bank, ani_bank, ct_bank))
    if lvl_tab:
        print('level tables: ' + ', '.join(f'L{l} partA {ab} objects {ob} ani index {ai} ani {an} ({n} pges)'
                                           for l, ab, ob, n, _, ai, an, ct in lvl_tab))
    # 6. sprites keyed by animation number, for drawing simulated objects
    anim_tile_bank = anim_tab_bank = anim_blob_bank = 0
    anim_max = 0
    anim_palette = [0] * 16
    if a.anim:
        an = pickle.load(open(a.anim, 'rb'))
        b.align_bank()
        anim_tile_bank = b.bank
        for t in an['tiles']:
            assert len(t) == 64
            b.data += t
        b.align_bank()
        anim_max = an['max_anim']
        # entry blob first (so the table can point into it), padded so that no
        # entry straddles a bank
        blob = bytearray()
        offs = {}
        for key, parts in sorted(an['entries'].items()):
            rec = bytes([len(parts)])
            for tid, dx, dy in parts:
                rec += bytes([tid & 0xFF, tid >> 8, dx & 0xFF, dy & 0xFF])
            if (len(blob) % BANK) + len(rec) > BANK:
                blob += bytes(BANK - (len(blob) % BANK))
            offs[key] = len(blob)
            blob += rec
        tab = bytearray()
        for n in range(anim_max + 1):
            for kind in (0, 1):
                for mirror in (0, 1):
                    o = offs.get((n, mirror, kind))
                    tab += (0xFFFF).to_bytes(2, 'little') if o is None else o.to_bytes(2, 'little')
        assert len(blob) < 0xFFFF, len(blob)   # table holds plain 16-bit offsets
        anim_tab_bank = b.bank
        b.data += bytes(tab)
        b.align_bank()
        anim_blob_bank = b.bank
        b.data += bytes(blob)
        b.align_bank()
        anim_palette = list(an['palette'])
        print(f'animation sprites: {len(an["tiles"])} tiles from bank {anim_tile_bank}, '
              f'table at {anim_tab_bank}, entries at {anim_blob_bank}')

    # LAST: cutscene dictionary: 512 tiles per bank, tile id -> bank BANK0+(id>>9)
    dict_bank0 = b.bank
    for k in fmv['dict']:
        b.data += F_planar(k)
    b.align_bank()
    dict_banks = b.bank - dict_bank0
    # and cutscene streams - the bulkiest data, and the only part a game can
    # do without, so it goes above everything gameplay needs: ops never straddle a bank; OP_NEXTBANK jumps to the next
    clips = []
    for cid, stream in fmv['clips']:
        start = (b.bank, b.addr)
        for chunk in stream:
            # split the tick chunk into individual ops so we can break anywhere
            for op in split_ops(chunk):
                if len(op) + 1 > b.room_left():
                    b.data += bytes([F.OP_NEXTBANK])
                    b.align_bank()
                b.data += op
        clips.append((cid,) + start)
    b.align_bank()
    stream_end = b.bank
    # 7. per-frame inputs + expected object state for the logic port
    logic_bank = 0
    if a.logic:
        lg = pickle.load(open(a.logic, 'rb'))
        b.align_bank()
        logic_bank = b.bank
        b.data += lg['blob']
        b.align_bank()
        print(f"logic harness: {lg['frames']} frames at bank {logic_bank}")
    b.align_bank()
    if b.bank > MAX_BANKS:
        sys.exit(f'data needs {b.bank} banks: exceeds the 4 MB mapper range')
    os.makedirs(a.out, exist_ok=True)
    open(f'{a.out}/bank_data.bin', 'wb').write(b.data)
    h = [
        '/* generated by tools/mkdata.py - do not edit */',
        '#ifndef DATA_INDEX_H', '#define DATA_INDEX_H',
        f'#define FMV_DICT_BANK0 {dict_bank0}',
        f'#define FMV_NUM_CLIPS {len(clips)}',
        f'#define NUM_ROOMS {len(room_tab)}',
        f'#define HAS_TITLE {1 if a.title else 0}',
        f'#define SPR_DICT_BANK0 {spr_bank0}',
        f'#define REPLAY_BANK {replay_bank}',
        f'#define REPLAY_ADDR 0x{replay_addr:04X}',
        f'#define HAS_REPLAY {1 if a.sprites else 0}',
        f'#define NUM_LEVELS {len(lvl_tab)}',
        f'#define LOGIC_BANK {logic_bank}',
        f'#define ANIM_TILE_BANK {anim_tile_bank}',
        f'#define ANIM_TAB_BANK {anim_tab_bank}',
        f'#define ANIM_BLOB_BANK {anim_blob_bank}',
        f'#define ANIM_MAX {anim_max}',
        f'#define HAS_ANIM {1 if a.anim else 0}',
        f'#define HAS_LOGIC {1 if a.logic else 0}',
        'extern const unsigned char level_num[], level_bank_a[], level_bank_obj[];',
        'extern const unsigned char level_bank_aniidx[], level_bank_ani[], level_bank_ct[];',
        'extern const unsigned char anim_palette[16];',
        'extern const unsigned int level_npges[];',
        'extern const unsigned char fmv_clip_id[], fmv_clip_bank[];',
        'extern const unsigned int fmv_clip_addr[];',
        'extern const unsigned char room_level[], room_num[], room_bank[];',
        'extern const unsigned int room_addr[];',
        '#endif']
    open(f'{a.out}/data_index.h', 'w').write('\n'.join(h) + '\n')

    def arr(t, name, vals, fmt):
        return f'const {t} {name}[] = {{ ' + ', '.join(fmt(v) for v in vals) + ' };'
    hx = lambda v: f'0x{v:02X}'
    hx4 = lambda v: f'0x{v:04X}'
    c = ['#include "data_index.h"',
         arr('unsigned char', 'fmv_clip_id', [x[0] for x in clips] or [0], hx),
         arr('unsigned char', 'fmv_clip_bank', [x[1] for x in clips] or [0], str),
         arr('unsigned int', 'fmv_clip_addr', [x[2] for x in clips] or [0], hx4),
         arr('unsigned char', 'room_level', [x[0] for x in room_tab] or [0], str),
         arr('unsigned char', 'level_num', [x[0] for x in lvl_tab] or [0], str),
         arr('unsigned char', 'level_bank_a', [x[1] for x in lvl_tab] or [0], str),
         arr('unsigned char', 'level_bank_obj', [x[2] for x in lvl_tab] or [0], str),
         arr('unsigned char', 'level_bank_aniidx', [x[5] for x in lvl_tab] or [0], str),
         arr('unsigned char', 'level_bank_ani', [x[6] for x in lvl_tab] or [0], str),
         arr('unsigned char', 'anim_palette', anim_palette, hx),
         arr('unsigned char', 'level_bank_ct', [x[7] for x in lvl_tab] or [0], str),
         arr('unsigned int', 'level_npges', [x[3] for x in lvl_tab] or [0], str),
         arr('unsigned char', 'room_num', [x[1] for x in room_tab] or [0], str),
         arr('unsigned char', 'room_bank', [x[2] for x in room_tab] or [0], str),
         arr('unsigned int', 'room_addr', [x[3] for x in room_tab] or [0], hx4)]
    open(f'{a.out}/data_index.c', 'w').write('\n'.join(c) + '\n')
    total = BANK * 2 + len(b.data)
    print(f'dictionary {len(fmv["dict"])} tiles in {dict_banks} banks; streams to bank {stream_end - 1}; '
          f'{len(room_tab)} rooms ({len(placed)} unique) to bank {b.bank - 1}')
    print(f'ROM usage {total / 1024:.0f} KB of 4096 KB ({100 * total / (4 << 20):.1f}%)')


def split_replay_ops(chunk):
    ops, i = [], 0
    while i < len(chunk):
        op = chunk[i]
        if op in (0, 0xFF): n = 1
        elif op == 1: n = 18            # room index + 16-byte sprite palette
        elif op == 2: n = 2             # scroll
        elif op == 3: n = 2 + 3 * chunk[i + 1]
        elif op == 4: n = 2 + 3 * chunk[i + 1]
        else: raise ValueError(f'bad replay op {op:#x}')
        ops.append(chunk[i:i + n]); i += n
    return ops


def F_planar(key):
    from roomconv import planar
    return planar(np.frombuffer(key, np.uint8))


def split_ops(chunk):
    ops, i = [], 0
    while i < len(chunk):
        op = chunk[i]
        if op in (F.OP_TICK, F.OP_DISPOFF, F.OP_DISPON, F.OP_CLEAR, F.OP_END):
            n = 1
        elif op == F.OP_WAIT:
            n = 2
        elif op in (F.OP_PALBG, F.OP_PALSPR):
            n = 17
        elif op & 0xF0 == F.OP_UPLOAD:
            n = 4
        elif op & 0xC0 == F.OP_NTRUN:
            n = 3 + 2 * ((op & 0x3F) + 1)
        else:
            raise ValueError(f'bad op {op:#x}')
        ops.append(chunk[i:i + n]); i += n
    return ops


# ------------------------------------------------------------------ verify --
def unplanar(t32):
    out = np.zeros(64, np.uint8)
    for y in range(8):
        for x in range(8):
            v = 0
            for k in range(4):
                if t32[y * 4 + k] & (0x80 >> x):
                    v |= 1 << k
            out[y * 8 + x] = v
    return out


def play_clip(blob, bank, addr, dict_bank=BANK0):
    """Decode a stream exactly as src/fmv.c does. Yields (tick, display_on, c6 image)."""
    def rd(pos):
        return blob[pos]
    pos = (bank - BANK0) * BANK + (addr - 0x8000)
    vram = np.zeros((448, 64), np.uint8)
    nt = np.zeros(896, np.int32)
    pals = np.zeros((2, 16), np.int32)
    disp = False
    tick = 0
    tilecache = {}
    while True:
        op = blob[pos]
        if op == F.OP_END:
            return
        if op == F.OP_NEXTBANK:
            pos = (pos // BANK + 1) * BANK; continue
        if op in (F.OP_TICK, F.OP_WAIT):
            n = 1 if op == F.OP_TICK else blob[pos + 1] + 1
            pos += 1 if op == F.OP_TICK else 2
            img = render(vram, nt, pals)
            for _ in range(n):
                yield tick, disp, img
                tick += 1
            continue
        if op == F.OP_CLEAR:
            nt[:] = 0; vram[0] = 0; pos += 1
        elif op == F.OP_DISPOFF:
            disp = False; pos += 1
        elif op == F.OP_DISPON:
            disp = True; pos += 1
        elif op in (F.OP_PALBG, F.OP_PALSPR):
            pals[op - F.OP_PALBG] = list(blob[pos + 1:pos + 17]); pos += 17
        elif op & 0xF0 == F.OP_UPLOAD:
            slot = ((op & 1) << 8) | blob[pos + 1]
            did = blob[pos + 2] | (blob[pos + 3] << 8)
            if did not in tilecache:
                o = (dict_bank - BANK0) * BANK + did * 32   # the dictionary is no
                # longer first in the blob: gameplay data comes before it
                tilecache[did] = unplanar(blob[o:o + 32])
            vram[slot] = tilecache[did]; pos += 4
        elif op & 0xC0 == F.OP_NTRUN:
            n = (op & 0x3F) + 1
            cell = blob[pos + 1] | (blob[pos + 2] << 8)
            for i in range(n):
                nt[cell + i] = blob[pos + 3 + 2 * i] | (blob[pos + 4 + 2 * i] << 8)
            pos += 3 + 2 * n
        else:
            raise ValueError(f'bad op {op:#x} at {pos:#x}')


def render(vram, nt, pals):
    e = nt[:768]
    slot = e & 0x1FF
    t = vram[slot].reshape(768, 8, 8)
    hf = (e & 0x200) != 0
    vf = (e & 0x400) != 0
    t = np.where(hf[:, None, None], t[:, :, ::-1], t)
    t = np.where(vf[:, None, None], t[:, ::-1, :], t)
    pb = ((e & 0x800) != 0).astype(np.int32)
    c6 = pals[pb[:, None, None], t]
    return c6.reshape(24, 32, 8, 8).transpose(0, 2, 1, 3).reshape(192, 256)


def c6_to_rgb(img):
    return np.stack([(img & 3) * 85, ((img >> 2) & 3) * 85, ((img >> 4) & 3) * 85], -1).astype(np.uint8)


def defines_from_header(out):
    d = {}
    for line in open(f'{out}/data_index.h'):
        p = line.split()
        if len(p) == 3 and p[0] == '#define':
            try: d[p[1]] = int(p[2], 0)
            except ValueError: pass
    return d


def verify(a):
    from fbfmt import read_fbv
    blob = open(f'{a.out}/bank_data.bin', 'rb').read()
    src = open(f'{a.out}/data_index.c').read()
    def grab(name):
        s = src[src.index(name + '[] = {') + len(name) + 6:]
        return [int(x, 0) for x in s[:s.index('}')].split(',')]
    ids, banks, addrs = grab('fmv_clip_id'), grab('fmv_clip_bank'), grab('fmv_clip_addr')
    worst = 0
    for cid, bank, addr in zip(ids, banks, addrs):
        frames = read_fbv(f'{a.capture}/cut_{cid:02X}.fbv')
        si, errs, off = 0, [], 0
        dict_bank = defines_from_header(a.out).get('FMV_DICT_BANK0', BANK0)
        for tick, disp, img in play_clip(blob, bank, addr, dict_bank):
            tms = tick * 1000 // 60
            while si + 1 < len(frames) and frames[si + 1][0] <= tms:
                si += 1
            ideal = F.composite(frames[si][1], frames[si][2])
            if disp:
                errs.append(float((img != ideal).mean() * 100))
            else:
                off += 1
            if a.png and tick in (30, 200, 600):
                from PIL import Image
                os.makedirs(a.png, exist_ok=True)
                Image.fromarray(np.concatenate([c6_to_rgb(ideal), c6_to_rgb(img)], 1)).save(
                    f'{a.png}/verify_{cid:02X}_{tick}.png')
        e = np.array(errs) if errs else np.array([100.0])
        worst = max(worst, e.mean())
        print(f'clip {cid:02X}: {len(errs) + off} ticks decoded from packed ROM data, '
              f'display-off ticks {off}, pixel err mean {e.mean():.2f}% p95 {np.percentile(e, 95):.2f}% max {e.max():.2f}%')
    if worst > a.max_err:
        sys.exit(f'FAIL: mean error {worst:.2f}% > {a.max_err}%')
    print('verify OK')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['pack', 'verify'])
    ap.add_argument('--fmv'); ap.add_argument('--rooms', nargs='*'); ap.add_argument('--sprites'); ap.add_argument('--title'); ap.add_argument('--levels', nargs='*'); ap.add_argument('--logic'); ap.add_argument('--anim'); ap.add_argument('--out', default='gen')
    ap.add_argument('--capture', default='capture'); ap.add_argument('--png')
    ap.add_argument('--max-err', type=float, default=3.0)
    a = ap.parse_args()
    pack(a) if a.mode == 'pack' else verify(a)


if __name__ == '__main__':
    main()
