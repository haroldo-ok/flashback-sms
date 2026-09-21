#!/usr/bin/env python3
"""Decode the FMV cutscene banks of the 'video' build back into ideal frames.

The video build stores, per cutscene:
  * a tile dictionary (512 tiles per 16 KiB bank, 32 bytes/tile, 4bpp planar)
  * per-frame deltas (RLE over cell indices) and periodic snapshots (full window)
  * a 16-byte BG palette per cutscene

This script replays that stream exactly like src/video_player.c does and writes
one raw index frame sequence per cutscene (256x96 = 32x12 cells) so the material
can be re-encoded with a *shared* dictionary.

Usage: extract_video_frames.py <all_data_banks.bin> <src/cutscenes_data.c> <outdir>
"""
import os
import re
import sys

BANK = 16384
VID_TW, VID_TH = 32, 12
CELLS = VID_TW * VID_TH


def parse_tables(path):
    txt = open(path).read()
    out = []

    def arr(name, cut):
        m = re.search(rf'{name}_{cut}\[\] = \{{(.*?)\}};', txt, re.S)
        if not m:
            return None
        return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', m.group(1))]

    # cutscene_info_t blocks, in file order
    for m in re.finditer(r'const cutscene_info_t cutscene_(\w+) = \{(.*?)\};', txt, re.S):
        cut = m.group(1)
        nums = [int(x) for x in re.findall(r'\d+', m.group(2))]
        nframes, snapivl, nsnaps, data_bank0, frame_delay = nums[:5]
        out.append(dict(
            name=cut, nframes=nframes, snapivl=snapivl, nsnaps=nsnaps,
            data_bank0=data_bank0, frame_delay=frame_delay,
            pal=arr('bg_palette', cut),
            frame_bank=arr('frame_bank', cut),
            frame_ofs=arr('frame_ofs', cut),
            snap_bank=arr('snap_bank', cut),
            snap_ofs=arr('snap_ofs', cut),
        ))
    return out


def planar_to_indices(buf):
    """32 bytes 4bpp planar -> 64 palette indices."""
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


class Blob:
    def __init__(self, path):
        self.data = open(path, 'rb').read()

    def byte(self, bank, addr_off):
        """bank is a ROM bank (2 == first byte of the file); addr_off is the
        offset inside the 0x8000 window."""
        return self.data[(bank - 2) * BANK + addr_off]


def read_delta(blob, bank, off):
    off = off - 0x8000          # window address -> offset inside the bank
    i = 0
    out = bytearray()
    while True:
        skip = blob.byte(bank, off + i)
        cnt = blob.byte(bank, off + i + 1)
        i += 2
        out.append(skip)
        out.append(cnt)
        if skip == 255 and cnt == 0:
            continue
        if skip == 0 and cnt == 0:
            return bytes(out)
        for _ in range(cnt):
            out.append(blob.byte(bank, off + i))
            out.append(blob.byte(bank, off + i + 1))
            i += 2


def tile_pixels(blob, data_bank0, tid):
    bank = data_bank0 + (tid >> 9)
    off = (tid & 511) * 32
    return planar_to_indices(bytes(blob.byte(bank, off + k) for k in range(32)))


def main():
    banks_path, csrc_path, outdir = sys.argv[1:4]
    os.makedirs(outdir, exist_ok=True)
    blob = Blob(banks_path)
    cuts = parse_tables(csrc_path)
    grand_total = 0
    for c in cuts:
        name = c['name']
        frames = []
        shown = None
        for f in range(c['nframes']):
            if f % c['snapivl'] == 0:
                sn = f // c['snapivl']
                bank, off = c['snap_bank'][sn], c['snap_ofs'][sn] - 0x8000
                shown = [blob.byte(bank, off + i * 2) |
                         (blob.byte(bank, off + i * 2 + 1) << 8) for i in range(CELLS)]
            else:
                d = read_delta(blob, c['frame_bank'][f], c['frame_ofs'][f])
                k = 0
                cell = 0
                while True:
                    skip, cnt = d[k], d[k + 1]
                    k += 2
                    if skip == 255 and cnt == 0:
                        cell += 254
                        continue
                    if skip == 0 and cnt == 0:
                        break
                    cell += skip
                    for _ in range(cnt):
                        shown[cell] = d[k] | (d[k + 1] << 8)
                        k += 2
                        cell += 1
            frames.append(list(shown))

        # rasterise every frame with the cutscene's own palette indices
        nwritten = 0
        path = os.path.join(outdir, f'frames_{name}.bin')
        with open(path, 'wb') as fh:
            for cells in frames:
                img = bytearray(256 * 96)
                for cy in range(VID_TH):
                    for cx in range(VID_TW):
                        tid = cells[cy * VID_TW + cx]
                        px = tile_pixels(blob, c['data_bank0'], tid)
                        for y in range(8):
                            base = (cy * 8 + y) * 256 + cx * 8
                            img[base:base + 8] = bytes(px[y * 8:y * 8 + 8])
                fh.write(img)
                nwritten += 1
        pal = bytes(c['pal'] or [0] * 16)
        with open(os.path.join(outdir, f'pal_{name}.bin'), 'wb') as fh:
            fh.write(pal)
        grand_total += nwritten
        print(f'{name:9s} {nwritten:4d} frames  delay {c["frame_delay"]}  '
              f'snapivl {c["snapivl"]}  {nwritten*256*96/1024:8.1f} KB raw')
    print(f'total {grand_total} frames -> {outdir}')
    with open(os.path.join(outdir, 'cutscene_meta.txt'), 'w') as fh:
        for c in cuts:
            fh.write(f'{c["name"]} {c["nframes"]} {c["frame_delay"]} {c["snapivl"]}\n')


if __name__ == '__main__':
    main()
