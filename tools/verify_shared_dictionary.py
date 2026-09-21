#!/usr/bin/env python3
"""DEPRECATED -- use `encode_shared_dictionary.py ... --verify`, which does the
same replay against the emitted bytes and is kept in sync with the encoder.

Verify the shared-dictionary stream: replay the EMITTED banks exactly like
the SMS runtime player does and compare against the source frames.

Usage: verify_shared_dictionary.py --frames-dir F --gen GEN [--base-bank 44]
"""
import argparse
import os
import re
import sys

BANK = 16384
CELLS = 32 * 12


def parse_c(gen):
    txt = open(os.path.join(gen, 'cutscenes_data.c')).read()
    cuts = []
    for m in re.finditer(r'const cutscene_info_t cutscene_(\w+) = \{(.*?)\};', txt, re.S):
        name = m.group(1)
        nums = [int(x) for x in re.findall(r'\d+', m.group(2))]
        nframes, snapivl, nsnaps, data_bank0, delay = nums[:5]

        def arr(n):
            mm = re.search(rf'{n}_{name}\[\] = \{{(.*?)\}};', txt, re.S)
            return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+', mm.group(1))]
        cuts.append(dict(name=name, nframes=nframes, snapivl=snapivl,
                         nsnaps=nsnaps, data_bank0=data_bank0, delay=delay,
                         frame_bank=arr('frame_bank'), frame_ofs=arr('frame_ofs'),
                         snap_bank=arr('snap_bank'), snap_ofs=arr('snap_ofs')))
    return cuts


def planar_to_indices(buf):
    px = [0] * 64
    for y in range(8):
        b0, b1, b2, b3 = buf[y * 4:y * 4 + 4]
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
    return px


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--frames-dir', required=True)
    ap.add_argument('--gen', required=True)
    ap.add_argument('--base-bank', type=int, default=44)
    a = ap.parse_args()

    blob = open(os.path.join(a.gen, 'video_bank_data.bin'), 'rb').read()
    cuts = parse_c(a.gen)

    def byte(bank, winaddr):
        return blob[(bank - a.base_bank) * BANK + (winaddr - 0x8000)]

    tiles = {}
    def tile(tid):
        t = tiles.get(tid)
        if t is None:
            t = planar_to_indices(bytes(byte(a.base_bank + (tid >> 9),
                                          0x8000 + (tid & 511) * 32 + k)
                                       for k in range(32)))
            tiles[tid] = t
        return t

    total_err = 0
    total_px = 0
    worst = (0, None)
    for c in cuts:
        src = open(os.path.join(a.frames_dir, f'frames_{c["name"]}.bin'), 'rb').read()
        shown = [-1] * CELLS
        err_c = 0
        for f in range(c['nframes']):
            if f % c['snapivl'] == 0:
                sn = f // c['snapivl']
                bank, off = c['snap_bank'][sn], c['snap_ofs'][sn]
                shown = [byte(bank, off + i * 2) | (byte(bank, off + i * 2 + 1) << 8)
                         for i in range(CELLS)]
            else:
                bank, off = c['frame_bank'][f], c['frame_ofs'][f]
                i = 0
                cell = 0
                while True:
                    skip = byte(bank, off + i)
                    cnt = byte(bank, off + i + 1)
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
            # compare
            base = f * 256 * 96
            e = 0
            for cy in range(12):
                for cx in range(32):
                    px = tile(shown[cy * 32 + cx])
                    for y in range(8):
                        s = base + (cy * 8 + y) * 256 + cx * 8
                        row = src[s:s + 8]
                        for k in range(8):
                            if px[y * 8 + k] != row[k]:
                                e += 1
            err_c += e
            if e > worst[0]:
                worst = (e, (c['name'], f))
        total_err += err_c
        total_px += c['nframes'] * 256 * 96
        print(f'{c["name"]:9s} frames {c["nframes"]:4d}  pixel error {err_c:9d} '
              f'({100.0*err_c/(c["nframes"]*256*96):.3f}%)')
    print(f'TOTAL mean error {100.0*total_err/total_px:.4f}%   worst frame '
          f'{worst[1]} {100.0*worst[0]/(256*96):.2f}%')
    print(f'blob {len(blob)//1024} KB ({len(blob)//BANK} banks)')


if __name__ == '__main__':
    main()
