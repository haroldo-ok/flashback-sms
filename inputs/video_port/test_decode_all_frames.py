import struct
from PIL import Image

with open('fbdemo/DATA/PERSO.SPR', 'rb') as f:
    spr = f.read()[12:]
with open('aba_unpacked/PERSO.OFF', 'rb') as f:
    off_data = f.read()

def decode_spm(data_ptr):
    nibble_len = struct.unpack('>H', data_ptr[0:2])[0] * 2
    raw = data_ptr[2:]
    nibbles = []
    for b in raw[:(nibble_len + 1) // 2]:
        nibbles.append(b >> 4)
        nibbles.append(b & 0xF)
    nibbles = nibbles[:nibble_len]
    
    dst = []
    i = 0
    while i < len(nibbles):
        code = nibbles[i]
        i += 1
        if code == 0xF:
            if i >= len(nibbles): break
            color = nibbles[i]
            i += 1
            if i >= len(nibbles): break
            count = nibbles[i]
            i += 1
            if color == 0xF:
                if i >= len(nibbles): break
                count = (count << 4) | nibbles[i]
                i += 1
                if i >= len(nibbles): break
                color = nibbles[i]
                i += 1
            count += 4
            dst.extend([color] * count)
        else:
            dst.append(code)
    return dst

all_frames = {}
correct = 0
total = 0
for i in range(len(off_data) // 6):
    idx, off = struct.unpack('<HI', off_data[i*6:i*6+6])
    if idx == 0xFFFF: break
    total += 1
    if off >= len(spr):
        continue
    dw = spr[off]
    dh = spr[off+1]
    b2 = spr[off+2]
    b3 = spr[off+3]
    w = b3 if (b2 & 0x40) else (b2 & 0x3F)
    h = dh
    pix = decode_spm(spr[off+4:])
    if len(pix) >= w * h:
        correct += 1
        all_frames[idx] = (dw, dh, w, h, pix[:w*h])

print(f"Correctly decoded frames: {correct} / {total}")
