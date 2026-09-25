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
    # 3. rooms.  ONE compact tile dictionary for every map (per-map dictionaries
    # cost four bank-aligned regions each, and tiles barely differ between
    # maps), then the room records first-fit into whatever bank space is left -
    # a record is ~2 KB and skipping to a fresh bank each time wasted ~200 KB.
    # A record is u16 ntiles, palette[16], nametable[32*28*2], then ntiles x u16
    # dictionary ids: the tile for each of its VRAM slots (448-n .. 447).
    import tilepack as TP
    room_tab = []
    uniq, index = [], {}
    for r in rooms:
        t = r['tiles']
        for i in range(r['ntiles']):
            k = t[i * 32:i * 32 + 32]
            if k not in index:
                index[k] = len(uniq)
                uniq.append(k)
    room_dicts = []
    if rooms:
        remap, regions, starts = TP.build([TP.unplanar(k) for k in uniq])
        banks = []
        for ty in (0, 1, 2, 3):
            b.align_bank()
            banks.append(b.bank)
            b.data += regions[ty]
        b.align_bank()
        room_dicts.append(tuple(banks) + tuple(starts))
        # first-fit: remember the free tail of every bank the records open
        free = []          # (bank, start_offset_in_blob, bytes_left)
        placed = {}
        for r in rooms:
            key = (r['level'], r.get('alias', r['room']))
            if key not in placed:
                ids = bytearray()
                t = r['tiles']
                for i in range(r['ntiles']):
                    tid = remap[index[t[i * 32:i * 32 + 32]]]
                    ids += bytes([tid & 0xFF, tid >> 8])
                rec = bytes([r['ntiles'] & 0xFF, r['ntiles'] >> 8]) + r['palette'] + r['nametable'] + bytes(ids)
                assert len(rec) <= BANK
                spot = None
                for j, (bk, off, left) in enumerate(free):
                    if left >= len(rec):
                        spot = j
                        break
                if spot is None:
                    b.align_bank()
                    bk, off = b.bank, len(b.data)
                    b.data += bytes(BANK)
                    free.append((bk, off, BANK))
                    spot = len(free) - 1
                bk, off, left = free[spot]
                at = off + (BANK - left)
                b.data[at:at + len(rec)] = rec
                placed[key] = (bk, 0x8000 + (BANK - left))
                free[spot] = (bk, off, left - len(rec))
            room_tab.append((r['level'], r['room']) + placed[key] + (0,))
    b.align_bank()
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

    print(f'  [pack] before cutscenes: bank {b.bank}')
    # LAST: cutscene dictionary in the compact lossless form (tools/tilepack.py):
    # sorted by type, each type region bank-aligned, no per-tile index
    import tilepack as TP
    remap, regions, (start1, start2, start3) = TP.build([np.frombuffer(k, np.uint8) for k in fmv['dict']])
    fmv_banks = []
    for t in (0, 1, 2, 3):
        b.align_bank()
        fmv_banks.append(b.bank)
        b.data += regions[t]
    b.align_bank()
    dict_bank0 = fmv_banks[0]
    dict_banks = b.bank - dict_bank0
    fmv_desc = (fmv_banks[0], fmv_banks[1], fmv_banks[2], fmv_banks[3], start1, start2, start3)
    # the streams name tiles by their old ids: rewrite the upload ops
    import dictmerge as DM
    fmv['clips'] = [(cid, DM.rewrite(st, np.array(remap))) for cid, st in fmv['clips']]
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
    print(f'  [pack] after rooms and levels: bank {b.bank}')
    # 6b. all-levels sprite sets (tools/sprconv.py): one compact 8x8 dictionary,
    # per set a table of (bank, addr) per (anim, mirror), entries of
    #   u8 count, count x (u16 top, u16 bottom, i8 dx, i8 dy)
    spr_desc = (0,) * 7
    spr_tab = [(0, 0)] * 6
    spr_lo = [(0, 0)] * 6
    spr_mon = []
    spr_max = 0
    spr_palette = [0] * 16
    if a.sprsets:
        import tilepack as TP
        sp = pickle.load(open(a.sprsets, 'rb'))
        # sprite tiles stream into VRAM every frame while things animate, so
        # they are stored raw: a straight copy, no plane rebuilding
        remap, regions, starts = TP.build(sp['tiles'], raw=True)
        banks = []
        for ty in (0, 1, 2, 3):
            b.align_bank()
            banks.append(b.bank)
            b.data += regions[ty]
        b.align_bank()
        spr_desc = tuple(banks) + tuple(starts)
        spr_max = max(anim for st in sp['sets'].values() for (anim, _m) in st)
        # entry records first, so the tables can point at them
        where = {}
        for slot in sorted(sp['sets']):
            for key, parts in sorted(sp['sets'][slot].items()):
                rec = bytes([len(parts)])
                for top, bot, dx, dy in parts:
                    t, u = remap[top], remap[bot]
                    rec += bytes([t & 0xFF, t >> 8, u & 0xFF, u >> 8, dx & 0xFF, dy & 0xFF])
                if len(rec) > b.room_left():
                    b.align_bank()
                where[(slot,) + key] = (b.bank, b.addr)
                b.data += rec
        b.align_bank()
        # each set covers only the animation numbers it actually uses: the
        # monsters share one 95-entry range, so a table sized to the global
        # maximum wasted ~30 KB
        spr_tab = []
        spr_lo = []
        for slot in range(6):
            used = [anim for (sl, anim, _m) in where if sl == slot]
            lo, hi = (min(used), max(used)) if used else (0, 0)
            per = (hi - lo + 1) * 2 * 3
            if per > b.room_left():
                b.align_bank()
            spr_tab.append((b.bank, b.addr))
            spr_lo.append((lo, hi))
            tab = bytearray()
            for anim in range(lo, hi + 1):
                for mirror in (0, 1):
                    bk, ad = where.get((slot, anim, mirror), (0, 0))
                    tab += bytes([bk, ad & 0xFF, ad >> 8])
            b.data += bytes(tab)
        b.align_bank()
        for row in sp['monster_of']:
            if len(row) > b.room_left():
                b.align_bank()
            spr_mon.append((b.bank, b.addr))
            b.data += row
        b.align_bank()
        spr_palette = list(sp['palette'])
        print(f"sprite sets: {len(sp['tiles'])} 8x8 tiles, {len(where)} entries, tables for 6 sets")

    print(f'  [pack] after sprites: bank {b.bank}')
    # 6c. level-select text (tools/menuconv.py): a few shared tiles, one row of
    # cell indices per level, and a palette loaded as the sprite palette so the
    # text keeps its colours over the title picture
    menu_bank = menu_addr = 0
    menu_ntiles = menu_cells = 0
    menu_palette = [0] * 16
    if a.menu:
        mn = pickle.load(open(a.menu, 'rb'))
        blob = b''.join(mn['tiles']) + bytes(c for strip in mn['strips'] for c in strip)
        if len(blob) > b.room_left():
            b.align_bank()
        menu_bank, menu_addr = b.bank, b.addr
        b.data += blob
        menu_ntiles, menu_cells = len(mn['tiles']), mn['cells']
        menu_palette = list(mn['palette'])
        print(f'level select: {menu_ntiles} tiles + 5 strips at bank {menu_bank}')

    # 6d. music: one PSG stream per track (tools/midiconv.py), each inside a
    # single bank so the player never crosses one mid-stream
    mus_tab = []
    if a.music:
        for path in a.music:
            if path == '-':                      # slot kept, no track packed
                mus_tab.append((0, 0))
                continue
            mu = pickle.load(open(path, 'rb'))
            st = mu['stream']
            assert len(st) <= BANK
            if len(st) > b.room_left():
                b.align_bank()
            mus_tab.append((b.bank, b.addr))
            b.data += st
        b.align_bank()
        print(f'music: {sum(1 for x in mus_tab if x[0])} tracks in {len(mus_tab)} slots, '
              f'{sum(len(pickle.load(open(p_, "rb"))["stream"]) for p_ in a.music if p_ != "-")/1024:.1f} KB')

    # 6e. sound effects approximated on the PSG (tools/sfxconv.py)
    sfx_tab = []
    if a.sfx:
        sx = pickle.load(open(a.sfx, 'rb'))
        top = max(sx) + 1 if sx else 0
        for i in range(top):
            st = sx.get(i)
            if not st:
                sfx_tab.append((0, 0))
                continue
            if len(st) > b.room_left():
                b.align_bank()
            sfx_tab.append((b.bank, b.addr))
            b.data += st
        b.align_bank()
        print(f'sound effects: {sum(1 for x in sfx_tab if x[0])} of {top} entries')

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
        '#include "tiledec.h"',
        'extern const TileDict fmv_dict;',
        f'#define FMV_DICT_BANK1 {fmv_desc[1] if fmv["dict"] else 0}',
        f'#define FMV_DICT_BANK2 {fmv_desc[2] if fmv["dict"] else 0}',
        f'#define FMV_DICT_BANK3 {fmv_desc[3] if fmv["dict"] else 0}',
        f'#define FMV_DICT_START1 {fmv_desc[4] if fmv["dict"] else 0}',
        f'#define FMV_DICT_START2 {fmv_desc[5] if fmv["dict"] else 0}',
        f'#define FMV_DICT_START3 {fmv_desc[6] if fmv["dict"] else 0}',
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
        f'#define HAS_SPRSETS {1 if a.sprsets else 0}',
        f'#define HAS_MENU {1 if a.menu else 0}',
        f'#define HAS_MUSIC {1 if a.music else 0}',
        f'#define HAS_SFX {1 if a.sfx else 0}',
        f'#define LOGIC_LEVEL {a.logic_level}',      # the level the harness trace plays
        f'#define LOGIC_LEVEL {a.logic_level}',     # which level the harness checks
        f'#define NUM_SFX {len(sfx_tab)}',
        'extern const unsigned char sfx_bank[];',
        'extern const unsigned int sfx_addr[];',
        f'#define NUM_MUSIC {len(mus_tab)}',
        'extern const unsigned char music_bank[], cut_music[];',
        '#define CUT_MUSIC_COUNT 75',
        '#define TITLE_MUSIC 1',
        'extern const unsigned int music_addr[];',
        'extern const unsigned char level_cutscene[];',
        f'#define MENU_BANK {menu_bank}',
        f'#define MENU_ADDR 0x{menu_addr:04X}',
        f'#define MENU_NTILES {menu_ntiles}',
        f'#define MENU_CELLS {menu_cells}',
        'extern const unsigned char menu_palette[16];',
        f'#define SPR_MAX_ANIM {spr_max}',
        'extern const TileDict spr_dict;',
        'extern const unsigned char spr_tab_bank[6], spr_mon_bank[];',
        'extern const unsigned int spr_tab_lo[6], spr_tab_hi[6];',
        'extern const unsigned int spr_tab_addr[6], spr_mon_addr[];',
        'extern const unsigned char spr_palette[16];',
        f'#define HAS_LOGIC {1 if a.logic else 0}',
        'extern const unsigned char level_num[], level_bank_a[], level_bank_obj[];',
        'extern const unsigned char level_bank_aniidx[], level_bank_ani[], level_bank_ct[];',
        'extern const unsigned char anim_palette[16];',
        'extern const unsigned int level_npges[];',
        'extern const unsigned char fmv_clip_id[], fmv_clip_bank[];',
        'extern const unsigned int fmv_clip_addr[];',
        'extern const unsigned char room_level[], room_num[], room_bank[], room_dict[];',
        f'#define NUM_ROOM_DICTS {len(room_dicts)}',
        'extern const TileDict room_dicts[];',
        'extern const unsigned int room_addr[];',
        '#endif']
    open(f'{a.out}/data_index.h', 'w').write('\n'.join(h) + '\n')

    def arr(t, name, vals, fmt):
        return f'const {t} {name}[] = {{ ' + ', '.join(fmt(v) for v in vals) + ' };'
    hx = lambda v: f'0x{v:02X}'
    hx4 = lambda v: f'0x{v:04X}'
    c = ['#include "data_index.h"',
         'const TileDict fmv_dict = { FMV_DICT_BANK0, FMV_DICT_BANK1, FMV_DICT_BANK2, FMV_DICT_BANK3,'
         ' FMV_DICT_START1, FMV_DICT_START2, FMV_DICT_START3 };',
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
         'const TileDict spr_dict = { %d, %d, %d, %d, %d, %d, %d };' % spr_desc,
         arr('unsigned char', 'spr_tab_bank', [x[0] for x in spr_tab], str),
         arr('unsigned int', 'spr_tab_addr', [x[1] for x in spr_tab], hx4),
         arr('unsigned int', 'spr_tab_lo', [x[0] for x in spr_lo], str),
         arr('unsigned int', 'spr_tab_hi', [x[1] for x in spr_lo], str),
         arr('unsigned char', 'spr_mon_bank', [x[0] for x in spr_mon] or [0], str),
         arr('unsigned int', 'spr_mon_addr', [x[1] for x in spr_mon] or [0], hx4),
         arr('unsigned char', 'spr_palette', spr_palette, hx),
         arr('unsigned char', 'menu_palette', menu_palette, hx),
         arr('unsigned char', 'music_bank', [x[0] for x in mus_tab] or [0], str),
         arr('unsigned char', 'sfx_bank', [x[0] for x in sfx_tab] or [0], str),
         arr('unsigned int', 'sfx_addr', [x[1] for x in sfx_tab] or [0], hx4),
         # the engine's own cutscene -> music table (Cutscene::_musicTableDOS)
         arr('unsigned char', 'cut_music', [
             0x10,0x15,0x15,0xFF,0x15,0x19,0x0F,0xFF,0x15,0x04,0x15,0xFF,0xFF,0x00,0x19,0x15,
             0x15,0x0D,0x15,0x0D,0x18,0x13,0xFF,0xFF,0xFF,0x14,0x14,0x14,0x14,0x14,0xFF,0xFF,
             0x13,0x13,0x13,0x13,0x15,0x14,0x14,0x14,0x14,0x14,0x14,0x13,0x13,0x11,0xFF,0x03,
             0x0E,0x13,0x12,0xFF,0x06,0x07,0x0A,0x0A,0x15,0x05,0x13,0x02,0x15,0x09,0x17,0x08,
             0x0B,0x0C,0x14,0x14,0x14,0x14,0x14,0x14,0xFF,0xFF,0xFF], hx),
         arr('unsigned int', 'music_addr', [x[1] for x in mus_tab] or [0], hx4),
         arr('unsigned char', 'level_cutscene',
             [{0: 0x00, 1: 0x2F, 2: 0xFF, 3: 0x34, 4: 0x39, 5: 0x35, 6: 0xFF}[n] for n in [x[0] for x in lvl_tab]] or [0xFF], hx),
         arr('unsigned char', 'level_bank_ct', [x[7] for x in lvl_tab] or [0], str),
         arr('unsigned int', 'level_npges', [x[3] for x in lvl_tab] or [0], str),
         arr('unsigned char', 'room_num', [x[1] for x in room_tab] or [0], str),
         arr('unsigned char', 'room_bank', [x[2] for x in room_tab] or [0], str),
         arr('unsigned char', 'room_dict', [x[4] for x in room_tab] or [0], str),
         'const TileDict room_dicts[] = { ' + ', '.join(
             '{ %d, %d, %d, %d, %d, %d, %d }' % d for d in room_dicts) + ' };',
         arr('unsigned int', 'room_addr', [x[3] for x in room_tab] or [0], hx4)]
    open(f'{a.out}/data_index.c', 'w').write('\n'.join(c) + '\n')
    total = BANK * 2 + len(b.data)
    print(f'dictionary {len(fmv["dict"])} tiles in {dict_banks} banks; streams to bank {stream_end - 1}; '
          f'{len(room_tab)} rooms ({len(placed)} unique) to bank {b.bank - 1}')
    import json
    json.dump({'levels': list(a.levels or []), 'rooms': list(a.rooms or []),
               'fmv': a.fmv, 'title': a.title, 'sprsets': a.sprsets, 'logic': a.logic},
              open(f'{a.out}/manifest.json', 'w'), indent=1)
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


def fmv_desc_from(defs):
    return dict(bank0=defs['FMV_DICT_BANK0'], bank1=defs.get('FMV_DICT_BANK1', 0),
                bank2=defs.get('FMV_DICT_BANK2', 0), bank3=defs.get('FMV_DICT_BANK3', 0),
                start1=defs.get('FMV_DICT_START1', 1 << 30), start2=defs.get('FMV_DICT_START2', 1 << 30),
                start3=defs.get('FMV_DICT_START3', 1 << 30))


def play_clip(blob, bank, addr, desc):
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
                import tilepack as TP
                at = lambda bk, off, n: blob[(bk - BANK0) * BANK + off:(bk - BANK0) * BANK + off + n]
                tilecache[did] = unplanar(TP.fetch(at, desc, did))
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
        desc = fmv_desc_from(defines_from_header(a.out))
        for tick, disp, img in play_clip(blob, bank, addr, desc):
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
    ap.add_argument('--fmv'); ap.add_argument('--rooms', nargs='*'); ap.add_argument('--sprites'); ap.add_argument('--title'); ap.add_argument('--levels', nargs='*'); ap.add_argument('--logic'); ap.add_argument('--anim'); ap.add_argument('--sprsets'); ap.add_argument('--menu'); ap.add_argument('--music', nargs='*'); ap.add_argument('--sfx'); ap.add_argument('--logic-level', type=int, default=0); ap.add_argument('--out', default='gen')
    ap.add_argument('--capture', default='capture'); ap.add_argument('--png')
    ap.add_argument('--max-err', type=float, default=3.0)
    a = ap.parse_args()
    pack(a) if a.mode == 'pack' else verify(a)


if __name__ == '__main__':
    main()
