#!/usr/bin/env python3
"""All-levels sprites -> ROM-ready sets sharing one compact 8x8 tile dictionary.

Input: gen/full_sprites.pkl from animconv.py --sets, whose entries are keyed
(set, anim, mirror) with parts (8x16 tile id, dx, dy).  Sets:
  0 Conrad, 1..4 the four monster types, 10 the (global) level objects.

Output (for mkdata.py --sprsets):
  * tiles: 8x8 tiles (64 palette indices each), deduplicated - an 8x16 sprite
    is its top and bottom half, and halves repeat far more than whole sprites
  * sets: {set_slot: {(anim, mirror): [(top_id, bottom_id, dx, dy), ...]}}
    with set_slot 0..5 (objects are slot 5)
  * monster_of: per level part, per object, which monster set (0..3) it uses,
    or 0xFF - worked out exactly as the engine's loadMonsterSprites does
  * palette: the sprite palette

Entries with more than MAX_PARTS sprites are dropped: they are large pieces
of machinery drawn as objects, and could not be shown with the SMS's 64
hardware sprites (8 per line) anyway.
"""
import argparse, os, pickle, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import tilepack as TP
from levelconv import read_fbl

MAX_PARTS = 40
SET_SLOT = {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 10: 5}

# the engine's _monsterListLevels: (object node, monster set) pairs per level part
MONSTER_LISTS = [
    {0x22: 0, 0x23: 0},
    {0x22: 0, 0x23: 0, 0x4B: 0, 0x49: 1, 0x4D: 1, 0x76: 2},
    {0x76: 2},
    {0x4D: 1, 0x76: 2},
    {0x76: 2, 0xAC: 2, 0xD7: 3},
    {0xB0: 3, 0xD7: 3},
    {0xB0: 3, 0xD7: 3, 0xD8: 3},
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sprites')
    ap.add_argument('--levels', nargs='+', required=True, help='level_L*.fbl in part order')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    d = pickle.load(open(a.sprites, 'rb'))

    halves, hindex = [], {}
    def half_id(t32):
        k = bytes(t32)
        if k not in hindex:
            hindex[k] = len(halves)
            halves.append(k)
        return hindex[k]

    sets = {}
    dropped = 0
    for (setid, anim, mirror), parts in d['entries'].items():
        if len(parts) > MAX_PARTS:
            dropped += 1
            continue
        out = []
        for tid, dx, dy in parts:
            t = d['tiles'][tid]
            out.append((half_id(t[:32]), half_id(t[32:]), dx, dy))
        sets.setdefault(SET_SLOT[setid], {})[(anim, mirror)] = out

    tiles64 = [TP.unplanar(h) for h in halves]
    raw = len(tiles64) * 32
    packed = sum(TP.SIZE[TP.tile_type(t)] for t in tiles64)

    monster_of = []
    for part, path in enumerate(a.levels):
        lv = read_fbl(path)
        mlist = MONSTER_LISTS[part]
        row = []
        for i in range(lv['npges']):
            p = i * 31
            node = struct.unpack_from('<H', lv['pges'], p + 6)[0]
            otype = lv['pges'][p + 18]
            if (node == 0x49 or otype == 10) and node in mlist:
                row.append(mlist[node])
            else:
                row.append(0xFF)
        monster_of.append(bytes(row))

    n_entries = sum(len(v) for v in sets.values())
    print(f'{n_entries} entries in {len(sets)} sets ({dropped} oversized dropped); '
          f'{len(halves)} distinct 8x8 tiles: {raw/1024:.0f} KB raw -> {packed/1024:.0f} KB compact')
    for slot in sorted(sets):
        print(f'  set {slot}: {len(sets[slot])} entries')
    pickle.dump(dict(tiles=tiles64, sets=sets, monster_of=monster_of,
                     palette=d['palette']), open(a.out, 'wb'))


if __name__ == '__main__':
    main()
