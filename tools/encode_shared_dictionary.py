#!/usr/bin/env python3
"""Re-encode all Flashback SMS cutscenes into ONE tile stream with a SHARED
dictionary of tiles.

Why a shared dictionary: in the original video build each cutscene carried its
own private tile set. The seven cutscenes share a lot of imagery (the intro
shots, the Holocube console, the starfields), so a single global dictionary
removes the duplicates -- the same footage then fits in fewer banks and there is
room to keep the encode loss-less.

Input : raw index frames (256x96, palette indices 0..15) produced by
        tools/extract_video_frames.py, one file per cutscene, plus palettes and
        cutscene_meta.txt (name nframes frame_delay snapivl).
Output: <out>/video_bank_data.bin  -- tiles + snapshots + deltas, 512 tiles per
                                     16 KiB bank, ROM-absolute bank numbering
                                     (blob bank N lands at ROM bank base_bank+N)
        <out>/cutscenes_data.{c,h} -- the runtime tables

Stream format (identical to the runtime player's contract):
    delta, per position: <skip u8> <count u8> <count x u16 tile id LE> ...
                         <255> <0> = advance 254 cells, keep going
                         <0>   <0> = end of this position
    snapshot (every `--snapivl` positions): VID_CELLS x u16 tile id LE

The encoder is *exact*: unlike the general-purpose smsvideo encoder it does not
drop changed cells, because on the SMS the player's per-VBlank quota already
spreads heavy positions over several frames -- there is no need to lose pixels
to stay inside the VBlank budget. Use --thresh/--maxupd only if you want a
smaller stream and accept the error; --verify replays the emitted bytes against
the ideal frames and prints the pixel error.

Usage:
  encode_shared_dictionary.py --frames-dir DIR --out gen --base-bank 44 \
        [--snapivl 8] [--thresh 0] [--maxupd 0] [--verify]
"""
import argparse
import os
import struct
import sys

BANK = 16384
VID_TW, VID_TH = 32, 12
CELLS = VID_TW * VID_TH
RAW_LOOKUP = []                     # tile id -> 64-byte tile pixels


def read_frames(path, n):
    data = open(path, 'rb').read()
    need = 256 * 96 * n
    if len(data) < need:
        sys.exit(f'{path}: {len(data)} bytes, need {need}')
    return data


def cell_ids(data, n):
    """raw index frames -> n rows of CELLS tiles (as 64-byte keys)."""
    ids = []
    for f in range(n):
        base = f * 256 * 96
        row = [0] * CELLS
        for ty in range(VID_TH):
            for tx in range(VID_TW):
                cell = bytearray(64)
                for yy in range(8):
                    src = base + (ty * 8 + yy) * 256 + tx * 8
                    cell[yy * 8:yy * 8 + 8] = data[src:src + 8]
                row[ty * VID_TW + tx] = bytes(cell)
        ids.append(row)
    return ids


def pixdiff(a, b):
    return sum(1 for i in range(64) if a[i] != b[i])


def raw_of(tid):
    return RAW_LOOKUP[tid]


def build_streams(ids, raw, snapivl, thresh, maxupd):
    """Replay the encoder/decoder contract over the shared dictionary ids.
    Returns (deltas, snaps, updates_per_position)."""
    deltas, snaps, nupd = [], [], []
    shown = [-1] * CELLS
    for f in range(len(ids)):
        cur, craw = ids[f], raw[f]
        if f % snapivl == 0:
            # Snapshot positions carry a *resync* copy of the whole window, but
            # they still get a normal delta: the runtime advances one position
            # per displayed frame and cannot afford a full 12 KB repaint while
            # a cutscene is playing (the reference player only repaints in
            # seek()).  The delta chain below is therefore self-sufficient --
            # every position is reproduced from the previous one.
            if f == 0:
                shown = list(cur)          # position 0 == snapshot 0
            snaps.append(list(cur))
        cand = [i for i in range(CELLS) if cur[i] != shown[i]]
        if thresh <= 0:
            changed = cand
        else:
            changed = [i for i in cand
                       if pixdiff(craw[i], raw_of(shown[i])) > thresh]
        if maxupd and len(changed) > maxupd:
            changed.sort(key=lambda i: -pixdiff(craw[i], raw_of(shown[i])))
            changed = sorted(changed[:maxupd])
        for i in changed:
            shown[i] = cur[i]
        nupd.append(len(changed))
        # emit RLE runs of consecutive changed cells
        buf = bytearray()
        i = 0
        cursor = 0
        while i < len(changed):
            j = i
            while (j + 1 < len(changed) and changed[j + 1] == changed[j] + 1
                   and (j - i + 1) < 255):
                j += 1
            skip = changed[i] - cursor
            while skip >= 255:
                buf += bytes((255, 0))
                skip -= 254
            buf.append(skip)
            buf.append(j - i + 1)
            for k in range(i, j + 1):
                buf += struct.pack('<H', cur[changed[k]])
            cursor = changed[j] + 1
            i = j + 1
        buf += bytes((0, 0))
        deltas.append(bytes(buf))
    return deltas, snaps, nupd


def planar(tile64):
    out = bytearray()
    for y in range(8):
        b = [0, 0, 0, 0]
        for x in range(8):
            v = tile64[y * 8 + x] & 0xF
            for p in range(4):
                if v & (1 << p):
                    b[p] |= 0x80 >> x
        out += bytes(b)
    return bytes(out)


def pack(tilepix, per_cut):
    """Pack the dictionary (512 tiles/bank), then snapshots and deltas.
    Nothing straddles a bank. Returns (blob, per-cutscene (snap_loc, frame_loc))
    where each loc is a list of (blob_bank, window_address)."""
    tiledata = b''.join(planar(t) for t in tilepix)
    banks = [bytearray(tiledata[i * 32:(i + 512) * 32])
             for i in range(0, len(tilepix), 512)]
    if not banks:
        banks.append(bytearray())

    def place(blob):
        if len(banks[-1]) + len(blob) > BANK:
            banks.append(bytearray())
        b = len(banks) - 1
        addr = 0x8000 + len(banks[-1])
        banks[-1] += blob
        return b, addr

    out = []
    for snaps, deltas in per_cut:
        loc_s = [place(struct.pack('<%dH' % len(s), *s)) for s in snaps]
        loc_f = [place(d) for d in deltas]
        out.append((loc_s, loc_f))
    blob = b''.join(bytes(b) + b'\0' * (BANK - len(b)) for b in banks)
    return blob, out


def carr(name, data, typ, per, fmt):
    s = [f'const {typ} {name}[] = {{']
    for i in range(0, len(data), per):
        s.append('  ' + ','.join(fmt % v for v in data[i:i + per]) + ',')
    return '\n'.join(s + ['};', ''])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--frames-dir', required=True)
    ap.add_argument('--out', default='gen')
    ap.add_argument('--base-bank', type=int, default=44)
    ap.add_argument('--snapivl', type=int, default=8)
    ap.add_argument('--thresh', type=int, default=0)
    ap.add_argument('--maxupd', type=int, default=0)
    ap.add_argument('--verify', action='store_true',
                    help='replay the emitted bytes and report pixel error')
    a = ap.parse_args()

    meta = []
    for line in open(os.path.join(a.frames_dir, 'cutscene_meta.txt')):
        parts = line.split()
        if parts:
            meta.append((parts[0], int(parts[1]), int(parts[2]), int(parts[3])))

    # --- 1. load the frames and build the GLOBAL dictionary -------------------
    global RAW_LOOKUP
    dic, tilepix, cuts = {}, [], []
    for name, nframes, delay, snapivl in meta:
        data = read_frames(os.path.join(a.frames_dir, f'frames_{name}.bin'), nframes)
        ids = cell_ids(data, nframes)
        ids2, raw = [], []
        for row in ids:
            r, w = [], []
            for t in row:
                tid = dic.get(t)
                if tid is None:
                    tid = len(tilepix)
                    dic[t] = tid
                    tilepix.append(t)
                r.append(tid)
                w.append(t)
            ids2.append(r)
            raw.append(w)
        pal = list(open(os.path.join(a.frames_dir, f'pal_{name}.bin'),
                        'rb').read()[:16])
        cuts.append(dict(name=name, nframes=nframes, delay=delay, snapivl=snapivl,
                         ids=ids2, raw=raw, pal=pal))
        print(f'  {name:9s} {nframes:4d} frames  delay {delay}')

    RAW_LOOKUP = tilepix
    print(f'global dictionary: {len(tilepix)} tiles ({len(tilepix)*32/1024:.1f} KB)')
    if len(tilepix) > 65535:
        sys.exit('dictionary > 65535 tiles -- cannot address with 16-bit ids')

    # --- 2. per-cutscene streams over the shared dictionary ------------------
    all_upd = []
    for c in cuts:
        deltas, snaps, nupd = build_streams(c['ids'], c['raw'], a.snapivl,
                                            a.thresh, a.maxupd)
        c['deltas'], c['snaps'] = deltas, snaps
        all_upd += nupd
        print(f'  {c["name"]:9s} tiles/position mean {sum(nupd)/len(nupd):5.1f} '
              f'max {max(nupd):3d}  delta bytes mean '
              f'{sum(len(d) for d in deltas)//len(deltas):4d} '
              f'max {max(len(d) for d in deltas):4d}')

    blob, locs = pack(tilepix, [(c['snaps'], c['deltas']) for c in cuts])
    print(f'blob: {len(blob)//1024} KB = {len(blob)//BANK} banks '
          f'({(len(tilepix)*32 + BANK - 1)//BANK} tile banks)  '
          f'mean tiles/position {sum(all_upd)/len(all_upd):.1f} max {max(all_upd)}')

    # --- 3. emit the C tables ------------------------------------------------
    os.makedirs(a.out, exist_ok=True)
    h = ['/* generated by tools/encode_shared_dictionary.py -- do not edit */',
         '#ifndef CUTSCENES_DATA_H', '#define CUTSCENES_DATA_H', '',
         'typedef struct {',
         '    unsigned int nframes;',
         '    unsigned int snapivl;',
         '    unsigned int nsnaps;',
         '    unsigned char data_bank0;',
         '    const unsigned char *bg_palette;',
         '    const unsigned char *frame_bank;',
         '    const unsigned int *frame_ofs;',
         '    const unsigned char *snap_bank;',
         '    const unsigned int *snap_ofs;',
         '    unsigned char frame_delay;',
         '} cutscene_info_t;', '']
    body = ['']
    base = a.base_bank
    for c, (loc_s, loc_f) in zip(cuts, locs):
        n = c['name']
        h += [f'extern const unsigned char bg_palette_{n}[16];',
              f'extern const unsigned char frame_bank_{n}[{c["nframes"]}];',
              f'extern const unsigned int  frame_ofs_{n}[{c["nframes"] + 1}];',
              f'extern const unsigned char snap_bank_{n}[{len(c["snaps"])}];',
              f'extern const unsigned int  snap_ofs_{n}[{len(c["snaps"])}];',
              f'extern const cutscene_info_t cutscene_{n};', '']
        body.append(f'/* --- cutscene: {n} ({c["nframes"]} frames, '
                    f'{len(c["snaps"])} snapshots, delay {c["delay"]}) --- */')
        body.append(carr(f'bg_palette_{n}', c['pal'], 'unsigned char', 16, '0x%02x'))
        body.append(carr(f'frame_bank_{n}', [base + b for b, _ in loc_f],
                         'unsigned char', 16, '0x%02x'))
        ofs = [o for _, o in loc_f]
        ofs.append(ofs[-1] + len(c['deltas'][-1]))          # sentinel
        body.append(carr(f'frame_ofs_{n}', ofs, 'unsigned int', 8, '0x%04x'))
        body.append(carr(f'snap_bank_{n}', [base + b for b, _ in loc_s],
                         'unsigned char', 16, '0x%02x'))
        body.append(carr(f'snap_ofs_{n}', [o for _, o in loc_s],
                         'unsigned int', 8, '0x%04x'))
        body.append(f'const cutscene_info_t cutscene_{n} = {{')
        body.append(f'    {c["nframes"]}, {a.snapivl}, {len(c["snaps"])}, {base},')
        body.append(f'    bg_palette_{n}, frame_bank_{n}, frame_ofs_{n}, '
                    f'snap_bank_{n}, snap_ofs_{n},\n    {c["delay"]}\n}};\n')
    h.append('#endif')
    open(os.path.join(a.out, 'cutscenes_data.h'), 'w').write('\n'.join(h) + '\n')
    open(os.path.join(a.out, 'cutscenes_data.c'), 'w').write(
        '/* generated by tools/encode_shared_dictionary.py -- do not edit */\n'
        '#include "cutscenes_data.h"\n' + '\n'.join(body) + '\n')
    open(os.path.join(a.out, 'video_bank_data.bin'), 'wb').write(blob)
    print(f'wrote {a.out}/cutscenes_data.h, cutscenes_data.c, video_bank_data.bin')

    # --- 4. optional byte-exact verification ---------------------------------
    if a.verify:
        verify(a, cuts, blob, locs, tilepix, base)


def verify(a, cuts, blob, locs, tilepix, base):
    def byte(bank, addr):
        """bank is blob-relative (0 == the first bank of the stream)."""
        return blob[bank * BANK + (addr - 0x8000)]

    tilecache = {}

    def tile(tid):
        t = tilecache.get(tid)
        if t is None:
            packed = bytes(byte(tid >> 9,
                                0x8000 + (tid & 511) * 32 + k) for k in range(32))
            px = [0] * 64
            for y in range(8):
                b0, b1, b2, b3 = packed[y * 4:y * 4 + 4]
                for x in range(8):
                    bit = 0x80 >> x
                    v = 0
                    if b0 & bit:
                        v |= 1
                    if b1 & bit:
                        v |= 2
                    if b2 & bit:
                        v |= 4
                    if b3 & bit:
                        v |= 8
                    px[y * 8 + x] = v
            t = bytes(px)
            tilecache[tid] = t
        return t

    total_err = 0
    total_px = 0
    for c, (loc_s, loc_f) in zip(cuts, locs):
        src = open(os.path.join(a.frames_dir, f'frames_{c["name"]}.bin'), 'rb').read()
        # runtime contract: start from snapshot 0, then apply the delta of every
        # position in turn (no mid-cutscene repaints)
        bank, off = loc_s[0]
        shown = [byte(bank, off + i * 2) | (byte(bank, off + i * 2 + 1) << 8)
                 for i in range(CELLS)]
        err_c = 0
        snap_bad = 0
        for f in range(c['nframes']):
            if f:
                bank, off = loc_f[f]
                i = cell = 0
                while True:
                    skip, cnt = byte(bank, off + i), byte(bank, off + i + 1)
                    i += 2
                    if skip == 255 and cnt == 0:
                        cell += 254
                        continue
                    if skip == 0 and cnt == 0:
                        break
                    cell += skip
                    for _ in range(cnt):
                        shown[cell] = byte(bank, off + i) | (byte(bank, off + i + 1) << 8)
                        i += 2
                        cell += 1
            if f and (f % a.snapivl) == 0:
                # the snapshot must agree with the state the delta chain has
                # reached -- otherwise a runtime repaint would jump
                bank, off = loc_s[f // a.snapivl]
                ref = [byte(bank, off + i * 2) | (byte(bank, off + i * 2 + 1) << 8)
                       for i in range(CELLS)]
                snap_bad += sum(1 for i in range(CELLS) if ref[i] != shown[i])
            idealf = src[f * 256 * 96:(f + 1) * 256 * 96]
            e = 0
            for cy in range(VID_TH):
                for cx in range(VID_TW):
                    px = tile(shown[cy * VID_TW + cx])
                    for y in range(8):
                        s = (cy * 8 + y) * 256 + cx * 8
                        row = idealf[s:s + 8]
                        e += sum(1 for k in range(8) if px[y * 8 + k] != row[k])
            err_c += e
        total_err += err_c
        total_px += c['nframes'] * 256 * 96
        print(f'  verify {c["name"]:9s} pixel error {err_c:9d} '
              f'({100.0*err_c/(c["nframes"]*256*96):.4f}%)  '
              f'delta chain vs snapshots: {snap_bad} cell(s) apart')
    print(f'verify TOTAL: mean error {100.0*total_err/total_px:.4f}%  '
          f'({len(blob)//1024} KB stream)')


if __name__ == '__main__':
    main()
