import sys

BANK = 16384
TARGET_SIZE = 4 * 1024 * 1024 # 4 Megabytes = 4,194,304 bytes

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

    # Pad to exactly 4 MB
    if len(rom) < TARGET_SIZE:
        rom += b'\0' * (TARGET_SIZE - len(rom))
    elif len(rom) > TARGET_SIZE:
        sys.exit(f'ROM size {len(rom)} exceeds 4 MB')

    size = len(rom)

    # --- SEGA header checksum (offset 0x7ff0 in bank 1) -----------------
    hdr = rom.find(b'TMR SEGA', 0x7ff0, 0x8000)
    if hdr < 0:
        sys.exit('SEGA header not found')
    csum = sum(rom[0:0x7ff0]) & 0xffff
    rom[hdr + 0x0a] = csum & 0xff
    rom[hdr + 0x0b] = (csum >> 8) & 0xff
    
    # Size nibble for 4 MB is 0x06
    nib = 0x06
    rom[hdr + 0x0f] = (rom[hdr + 0x0f] & 0xf0) | nib

    open(out_path, 'wb').write(rom)
    print(f'{out_path}: {size//1024} KB '
          f'({size//BANK} banks, {len(data)//BANK} of them data), '
          f'checksum 0x{csum:04x}')

if __name__ == '__main__':
    main(*sys.argv[1:4])
