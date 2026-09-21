import struct
from PIL import Image

with open('fbdemo/DATA/PERSO.SPR', 'rb') as f:
    spr = f.read()[12:]
with open('aba_unpacked/PERSO.OFF', 'rb') as f:
    off_data = f.read()

from test_decode_all_frames import decode_spm

TOTAL_FRAMES = 357
FRAMES_PER_BANK = 51
BANK_SIZE = 16384

right_frames = []
left_frames = []

for frame_idx in range(TOTAL_FRAMES):
    if frame_idx * 6 >= len(off_data):
        break
    idx_val, off = struct.unpack('<HI', off_data[frame_idx*6 : frame_idx*6+6])
    if idx_val == 0xFFFF or off >= len(spr):
        break
    dw, dh, b2, b3 = spr[off], spr[off+1], spr[off+2], spr[off+3]
    pix = decode_spm(spr[off+4:])

    a, b = b3, b2
    sprite_mirror_y = False
    if b & 0x40:
        b &= ~0x40
        a, b = b, a
        sprite_mirror_y = True
    h, w = a, b

    grid = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            p_idx = (x * h + y) if sprite_mirror_y else (y * w + x)
            if p_idx < len(pix):
                grid[y * w + x] = pix[p_idx] & 0x0F

    # Raw box (Facing Left)
    box_raw = bytearray(16 * 40)
    ox = (16 - min(w, 16)) // 2
    oy = max(0, 40 - h)
    for y in range(min(h, 40)):
        for x in range(min(w, 16)):
            box_raw[(oy + y) * 16 + (ox + x)] = grid[y * w + x]

    # Flipped box (Facing Right)
    box_flipped = bytearray(16 * 40)
    for y in range(40):
        for x in range(16):
            box_flipped[y * 16 + (15 - x)] = box_raw[y * 16 + x]

    # Convert Right-facing (Flipped) to 4bpp planar (10 tiles = 2 cols x 5 rows)
    r_tiles = bytearray()
    for ty in range(5):
        for tx in range(2):
            for y in range(8):
                p0, p1, p2, p3 = 0, 0, 0, 0
                for x in range(8):
                    bx = tx * 8 + x
                    by = ty * 8 + y
                    c = box_flipped[by * 16 + bx]
                    bit = 7 - x
                    p0 |= ((c >> 0) & 1) << bit
                    p1 |= ((c >> 1) & 1) << bit
                    p2 |= ((c >> 2) & 1) << bit
                    p3 |= ((c >> 3) & 1) << bit
                r_tiles.extend([p0, p1, p2, p3])
    right_frames.append(bytes(r_tiles))

    # Convert Left-facing (Raw) to 4bpp planar (10 tiles = 2 cols x 5 rows)
    l_tiles = bytearray()
    for ty in range(5):
        for tx in range(2):
            for y in range(8):
                p0, p1, p2, p3 = 0, 0, 0, 0
                for x in range(8):
                    bx = tx * 8 + x
                    by = ty * 8 + y
                    c = box_raw[by * 16 + bx]
                    bit = 7 - x
                    p0 |= ((c >> 0) & 1) << bit
                    p1 |= ((c >> 1) & 1) << bit
                    p2 |= ((c >> 2) & 1) << bit
                    p3 |= ((c >> 3) & 1) << bit
                l_tiles.extend([p0, p1, p2, p3])
    left_frames.append(bytes(l_tiles))

def build_bank_blob(frames):
    blob = bytearray()
    num_banks = (len(frames) + FRAMES_PER_BANK - 1) // FRAMES_PER_BANK
    for b in range(num_banks):
        bank_data = bytearray(BANK_SIZE)
        start_f = b * FRAMES_PER_BANK
        end_f = min(start_f + FRAMES_PER_BANK, len(frames))
        for f in range(start_f, end_f):
            off = (f - start_f) * 320
            bank_data[off : off + 320] = frames[f]
        blob.extend(bank_data)
    return blob

right_blob = build_bank_blob(right_frames)
left_blob = build_bank_blob(left_frames)

full_conrad_data = right_blob + left_blob
with open("conrad_frames_all.bin", "wb") as f:
    f.write(full_conrad_data)

total_banks = len(full_conrad_data) // BANK_SIZE
print(f"Generated {len(right_frames)} frames per facing: {len(full_conrad_data)} bytes ({total_banks} banks of 16KB)")
