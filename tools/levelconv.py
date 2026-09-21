#!/usr/bin/env python3
"""Level object tables (fbdump level) -> ROM-friendly form for the Z80 port.

A level is stored in two parts, each starting at a bank boundary so the Z80
can index into it with a bank number plus an offset:

  part A : u16 numPges, InitPGE[numPges] (31 bytes), node_first[256] u16,
           node_num[256] u16                              (fits one 16K bank)
  part B : Object[] (18 bytes each), 910 records per bank so that no record
           straddles a bank boundary

The engine's own initial live state is carried alongside so the port can be
checked against it.

    levelconv.py capture/level_L0.fbl --out gen/level_L0.pkl
"""
import argparse, os, pickle, struct, sys

INIT_PGE_SIZE = 31
OBJECT_SIZE = 18
OBJECTS_PER_BANK = 16384 // OBJECT_SIZE          # 910


def read_fbl(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'FBL1', path
    npges, nodes, nobjects = struct.unpack_from('<HHH', d, 4)
    p = 10
    pges = d[p:p + npges * INIT_PGE_SIZE]; p += npges * INIT_PGE_SIZE
    node_first = d[p:p + 512]; p += 512
    node_num = d[p:p + 512]; p += 512
    objects = d[p:p + nobjects * OBJECT_SIZE]; p += nobjects * OBJECT_SIZE
    live_first = list(struct.unpack_from(f'<{npges}H', d, p)); p += 2 * npges
    live_flags = list(d[p:p + npges]); p += npges
    live_life = list(struct.unpack_from(f'<{npges}h', d, p)); p += 2 * npges
    live_room = list(d[p:p + npges]); p += npges
    live_anim = list(struct.unpack_from(f'<{npges}H', d, p)); p += 2 * npges
    ct = d[p:p + 0x1D00]; p += 0x1D00
    ani_size = struct.unpack_from('<I', d, p)[0]; p += 4
    ani = d[p:p + ani_size]
    skills = [pges[i * INIT_PGE_SIZE + 25] for i in range(npges)]
    return dict(npges=npges, nodes=nodes, nobjects=nobjects, pges=pges,
                node_first=node_first, node_num=node_num, objects=objects,
                live_first=live_first, live_flags=live_flags, live_life=live_life,
                live_room=live_room, live_anim=live_anim, ani=ani, ct=ct, skills=skills)


def pack_ani(ani):
    """Rebuild the animation table so no record straddles a 16K bank.

    A record's length comes from where the next one starts (the blob stores
    them back to back), which is robust: the frame lists do not all end with a
    terminator.  Types that share an offset share the packed record too.

    Returns (type count, index bytes: u8 bank + u16 addr per type, bank blobs).
    """
    ntypes = struct.unpack_from('<H', ani, 2)[0] // 2
    offs = [2 + struct.unpack_from('<H', ani, 2 + n * 2)[0] for n in range(ntypes)]
    bounds = sorted(set(offs)) + [len(ani)]
    length = {o: bounds[i + 1] - o for i, o in enumerate(bounds[:-1])}
    banks, cur, placed = [], bytearray(), {}
    for o in sorted(set(offs)):
        r = ani[o:o + length[o]]
        assert len(r) <= 16384, len(r)
        if len(cur) + len(r) > 16384:
            banks.append(bytes(cur) + bytes(16384 - len(cur)))
            cur = bytearray()
        placed[o] = (len(banks), 0x8000 + len(cur))
        cur += r
    banks.append(bytes(cur) + bytes(16384 - len(cur)))
    idx = bytearray()
    for o in offs:
        bank, addr = placed[o]
        idx += bytes([bank]) + struct.pack('<H', addr)
    return ntypes, bytes(idx), banks


def expected_checksum(lv, skill=1):
    """Sum over the objects the engine actually initialises (skill <= level):
    must match what the Z80 loader computes."""
    s = 0
    n = 0
    for i in range(lv['npges']):
        if lv['skills'][i] <= skill:
            s = (s + lv['live_first'][i] + lv['live_flags'][i]
                 + (lv['live_life'][i] & 0xFFFF) + lv['live_room'][i]
                 + lv['live_anim'][i]) & 0xFFFF
            n += 1
    active = sum(1 for i in range(lv['npges'])
                 if lv['skills'][i] <= skill and (lv['live_flags'][i] & 4))
    return s, n, active


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fbl')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    lv = read_fbl(a.fbl)
    partA = struct.pack('<H', lv['npges']) + lv['pges'] + lv['node_first'] + lv['node_num']
    assert len(partA) <= 16384, len(partA)
    banks = []
    for i in range(0, lv['nobjects'], OBJECTS_PER_BANK):
        chunk = lv['objects'][i * OBJECT_SIZE:(i + OBJECTS_PER_BANK) * OBJECT_SIZE]
        banks.append(chunk + bytes(16384 - len(chunk)))
    ntypes, ani_index, ani_banks = pack_ani(lv['ani'])
    chk, n, active = expected_checksum(lv)
    print(f"{os.path.basename(a.fbl)}: {lv['npges']} pges, {lv['nobjects']} objects "
          f"-> part A {len(partA)} bytes, part B {len(banks)} banks; "
          f"engine initialises {n} pges ({active} active), checksum 0x{chk:04X}; "
          f"{ntypes} animation records in {len(ani_banks)} banks")
    assert len(lv['ct']) == 0x1D00
    pickle.dump(dict(partA=partA, objectBanks=banks, npges=lv['npges'], ct=lv['ct'],
                     aniIndex=ani_index, aniBanks=ani_banks, ntypes=ntypes,
                     checksum=chk, init_count=n, active=active), open(a.out, 'wb'))


if __name__ == '__main__':
    main()
