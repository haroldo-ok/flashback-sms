#!/usr/bin/env python3
"""Build the final 4 MB Flashback SMS cartridge image.

    flashback_base.sms   (banks 0..43: linked code + level 1 data, from the
                          banked SDCC build -- bank n already sits at ROM
                          offset n*16384)
  + gen/video_bank_data.bin  (FMV stream, 52 banks; blob bank N -> ROM bank 44+N)
  + gen/title_bank.bin       (title screen,        ROM bank 96)
  + gen/instru_bank.bin      (instructions screen, ROM bank 97)
  = Flashback_SMS.sms        (padded to 4 MiB, SEGA header checksum + size
                              nibble fixed)

Usage: mkrom_merged.py <base.sms> <gen-dir> <out.sms>
"""
import os
import sys

BANK = 16384
TARGET = 4 * 1024 * 1024
BANK_VIDEO_DATA0 = 44
BANK_TITLE = 97
BANK_INSTRU = 98


def main(base_path, gen_dir, out_path):
    base = bytearray(open(base_path, 'rb').read())
    if len(base) % BANK:
        base += b'\0' * (BANK - len(base) % BANK)
    if len(base) > BANK_VIDEO_DATA0 * BANK:
        sys.exit(f'base ROM is {len(base)//BANK} banks, '
                 f'max is {BANK_VIDEO_DATA0} (banks 0..43)')

    video = open(os.path.join(gen_dir, 'video_bank_data.bin'), 'rb').read()
    title = open(os.path.join(gen_dir, 'title_bank.bin'), 'rb').read()
    instru = open(os.path.join(gen_dir, 'instru_bank.bin'), 'rb').read()
    for name, blob in (('video', video), ('title', title), ('instru', instru)):
        if len(blob) != BANK:
            if name != 'video' or len(blob) % BANK:
                sys.exit(f'{name} blob is {len(blob)} bytes (not bank aligned)')

    rom = bytearray(base) + bytearray(video) + bytearray(title) + bytearray(instru)
    used_banks = len(rom) // BANK
    if len(rom) < TARGET:
        rom += b'\0' * (TARGET - len(rom))
    elif len(rom) > TARGET:
        sys.exit(f'image {len(rom)} bytes exceeds 4 MiB')

    # --- SEGA header ---------------------------------------------------------
    hdr = rom.find(b'TMR SEGA', 0x7ff0, 0x8000)
    if hdr < 0:
        sys.exit('SEGA header not found in bank 1 (SMS_EMBED_SEGA_ROM_HEADER?)')
    csum = sum(rom[0:0x7ff0]) & 0xffff
    rom[hdr + 0x0a] = csum & 0xff
    rom[hdr + 0x0b] = (csum >> 8) & 0xff
    rom[hdr + 0x0f] = (rom[hdr + 0x0f] & 0xf0) | 0x06      # 4 MB (32 Mbit)

    open(out_path, 'wb').write(rom)

    # --- report -------------------------------------------------------------
    vb = len(video) // BANK
    print(f'{out_path}: {len(rom)} bytes (4 MiB, {len(rom)//BANK} banks)')
    print(f'  banks   0..43  code + level 1 data   ({len(base)//BANK} banks, '
          f'{len(base)//1024} KB)')
    print(f'  banks  {BANK_VIDEO_DATA0}..{BANK_VIDEO_DATA0 + vb - 1}  FMV stream '
          f'({vb} banks, {len(video)//1024} KB)')
    print(f'  bank   {BANK_TITLE}     title screen')
    print(f'  bank   {BANK_INSTRU}     instructions screen')
    print(f'  banks {used_banks}..255    unused padding')
    print(f'  checksum 0x{csum:04x}, size nibble 0x06 (4 MB)')


if __name__ == '__main__':
    main(*sys.argv[1:4])
