#!/usr/bin/env python3
"""Splice the video data banks into the code ROM and fix the SEGA checksum.

ihx2sms builds a 32 KiB ROM from the code (banks 0 and 1). The pre-rendered
video lives in banks 2.., which the Z80 pages into 0x8000-0xBFFF with
SMS_mapROMBank(). We append the blob verbatim so bank N of the blob lands at
ROM offset (N+2)*16384 -- exactly what tools/encode.py assumed when it
emitted frame_bank[]/frame_ofs[].
"""
import sys

BANK = 16384


def main(code_path, data_path, out_path):
    code = bytearray(open(code_path, 'rb').read())
    data = open(data_path, 'rb').read()
    if len(code) < 2 * BANK:
        code += b'\0' * (2 * BANK - len(code))
    if len(code) > 2 * BANK:
        sys.exit(f'code is {len(code)} bytes, must fit in 2 banks (32 KiB)')
    if len(data) % BANK:
        data += b'\0' * (BANK - len(data) % BANK)
    rom = bytearray(code) + bytearray(data)

    # ROM size must be a power of two for the header's size nibble
    total = len(rom)
    size = BANK
    while size < total:
        size *= 2
    rom += b'\0' * (size - total)

    # --- SEGA header checksum (offset 0x7ff0 in bank 1) -----------------
    hdr = rom.find(b'TMR SEGA', 0x7ff0, 0x8000)
    if hdr < 0:
        sys.exit('SEGA header not found')
    # checksum covers 0x0000..0x7ff0 for ROMs > 32K the range stays the same
    csum = sum(rom[0:0x7ff0]) & 0xffff
    rom[hdr + 0x0a] = csum & 0xff
    rom[hdr + 0x0b] = (csum >> 8) & 0xff
    # size nibble: 0x0c=8K 0x0d=16K 0x0e=32K 0x0f=48K 0x00=64K 0x01=128K
    # 0x02=256K 0x03=512K 0x04=1M. >1M has no standard nibble (the Sega mapper's
    # 8-bit bank register reaches 256 x 16K = 4M; emulators key off the file
    # size), so we extend the table so large FMV ROMs build with a valid header.
    nib = {0x2000: 0x0c, 0x4000: 0x0d, 0x8000: 0x0e, 0xC000: 0x0f,
           0x10000: 0x00, 0x20000: 0x01, 0x40000: 0x02, 0x80000: 0x03,
           0x100000: 0x04, 0x200000: 0x05, 0x400000: 0x06}.get(size)
    if nib is None:
        sys.exit(f'unsupported ROM size {size} (max 4 MiB)')
    rom[hdr + 0x0f] = (rom[hdr + 0x0f] & 0xf0) | nib

    open(out_path, 'wb').write(rom)
    print(f'{out_path}: {size//1024} KB '
          f'({size//BANK} banks, {len(data)//BANK} of them data), '
          f'checksum 0x{csum:04x}')


if __name__ == '__main__':
    main(*sys.argv[1:4])
