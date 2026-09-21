import struct, os
from PIL import Image
import numpy as np
import process_rooms

BANK_SIZE = 16384

def planar_tile(tile8x8):
    out = bytearray()
    for y in range(8):
        b = [0, 0, 0, 0]
        for x in range(8):
            v = tile8x8[y * 8 + x] & 0xF
            for p in range(4):
                if v & (1 << p):
                    b[p] |= (0x80 >> x)
        out += bytes(b)
    return bytes(out)

def pack_screen_bank(pal, tiles_planar, tilemap):
    b = bytearray(BANK_SIZE)
    b[0:16] = pal
    num_t = len(tiles_planar) // 32
    b[16] = num_t & 0xFF
    b[17] = (num_t >> 8) & 0xFF
    for i, t in enumerate(tilemap):
        b[32 + i*2] = t & 0xFF
        b[32 + i*2 + 1] = (t >> 8) & 0xFF
    b[2336 : 2336 + len(tiles_planar)] = tiles_planar
    return b

def convert_title_screen(bin_path, max_unique_tiles=320):
    data = open(bin_path, 'rb').read()
    fb = data[:256*224]
    pal_data = data[256*224:]
    
    title_pal_sms = [
        0x00, 0x3F, 0x2F, 0x1B, 0x0B, 0x3E, 0x3A, 0x39,
        0x29, 0x24, 0x10, 0x2A, 0x15, 0x14, 0x25, 0x06
    ]
    pal_rgb = []
    for c in title_pal_sms:
        r = (c & 0x03) * 85
        g = ((c >> 2) & 0x03) * 85
        b = ((c >> 4) & 0x03) * 85
        pal_rgb.extend([r, g, b])
        
    img = Image.new('RGB', (256, 224))
    pixels = []
    for idx in fb:
        pixels.append((pal_data[idx*3], pal_data[idx*3+1], pal_data[idx*3+2]))
    img.putdata(pixels)
    crop = img.crop((0, 16, 256, 208))
    
    pal_img = Image.new('P', (1, 1))
    pal_img.putpalette(pal_rgb + [0]*(768 - len(pal_rgb)))
    
    q_dither = crop.quantize(palette=pal_img, dither=Image.Dither.FLOYDSTEINBERG)
    q_arr = np.array(q_dither)
    
    tw, th = 32, 24
    raw_tiles = []
    for ty in range(th):
        for tx in range(tw):
            raw_tiles.append(q_arr[ty*8:(ty+1)*8, tx*8:(tx+1)*8].flatten())
            
    unique_tiles = []
    tilemap = []
    for t in raw_tiles:
        found = -1
        for i, ut in enumerate(unique_tiles):
            if np.array_equal(t, ut):
                found = i
                break
        if found != -1:
            tilemap.append(found)
        else:
            if len(unique_tiles) < max_unique_tiles:
                tilemap.append(len(unique_tiles))
                unique_tiles.append(t)
            else:
                best_match = 0
                best_d = 999999
                for i, ut in enumerate(unique_tiles):
                    d = np.sum(np.abs(t.astype(int) - ut.astype(int)))
                    if d < best_d:
                        best_d = d
                        best_match = i
                tilemap.append(best_match)
                
    all_tiles_bin = b''.join(planar_tile(t) for t in unique_tiles)
    pal_bytes = bytearray(title_pal_sms)
    return pal_bytes, all_tiles_bin, tilemap

def convert_instructions_screen(png_path, max_unique_tiles=320):
    img = Image.open(png_path).convert('RGB')
    
    instru_pal_sms = [
        0x00, 0x3F, 0x3B, 0x38, 0x28, 0x20, 0x10, 0x0F,
        0x03, 0x2A, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00
    ]
    pal_rgb = []
    for c in instru_pal_sms:
        r = (c & 0x03) * 85
        g = ((c >> 2) & 0x03) * 85
        b = ((c >> 4) & 0x03) * 85
        pal_rgb.extend([r, g, b])
        
    pal_img = Image.new('P', (1, 1))
    pal_img.putpalette(pal_rgb + [0]*(768 - len(pal_rgb)))
    
    q_instru = img.quantize(palette=pal_img, dither=Image.Dither.NONE)
    q_arr = np.array(q_instru)
    
    tw, th = 32, 24
    raw_tiles = []
    for ty in range(th):
        for tx in range(tw):
            raw_tiles.append(q_arr[ty*8:(ty+1)*8, tx*8:(tx+1)*8].flatten())
            
    unique_tiles = []
    tilemap = []
    for t in raw_tiles:
        found = -1
        for i, ut in enumerate(unique_tiles):
            if np.array_equal(t, ut):
                found = i
                break
        if found != -1:
            tilemap.append(found)
        else:
            if len(unique_tiles) < max_unique_tiles:
                tilemap.append(len(unique_tiles))
                unique_tiles.append(t)
            else:
                best_match = 0
                best_d = 999999
                for i, ut in enumerate(unique_tiles):
                    d = np.sum(np.abs(t.astype(int) - ut.astype(int)))
                    if d < best_d:
                        best_d = d
                        best_match = i
                tilemap.append(best_match)
                
    all_tiles_bin = b''.join(planar_tile(t) for t in unique_tiles)
    pal_bytes = bytearray(instru_pal_sms)
    return pal_bytes, all_tiles_bin, tilemap

# 1. Video banks (Banks 2..56, 55 banks = 880 KB)
video_blob = open("cutscenes_gen/all_video_banks.bin", "rb").read()
video_nbanks = len(video_blob) // BANK_SIZE
print(f"Video banks: {video_nbanks} banks (bank 2..{2 + video_nbanks - 1})")

bank_cursor = 2 + video_nbanks
all_banks = bytearray(video_blob)

# 2. Conrad Rotoscoped Frames (Banks 57..70, 14 banks = 224 KB)
conrad_raw = open("conrad_frames_all.bin", "rb").read()
conrad_bank0 = bank_cursor
conrad_nbanks = len(conrad_raw) // BANK_SIZE
all_banks += conrad_raw
bank_cursor += conrad_nbanks
print(f"Conrad banks: {conrad_nbanks} banks (bank {conrad_bank0}..{conrad_bank0+conrad_nbanks-1})")

# 3. Title Screen & Instructions Screen (Banks 71..72)
t_pal, t_tiles, t_map = convert_title_screen("menu1_pic.bin")
title_bank_idx = bank_cursor
all_banks += pack_screen_bank(t_pal, t_tiles, t_map)
bank_cursor += 1

i_pal, i_tiles, i_map = convert_instructions_screen("shots/sms_custom_instructions.png")
instru_bank_idx = bank_cursor
all_banks += pack_screen_bank(i_pal, i_tiles, i_map)
bank_cursor += 1
print(f"Title bank: {title_bank_idx}, Instru bank: {instru_bank_idx}")

# 4. Rooms 26..33 (Banks 73..80, 8 banks = 128 KB)
room_start_bank = bank_cursor
rooms_to_include = [26, 27, 28, 29, 30, 31, 32, 33]
for room_id in rooms_to_include:
    res = process_rooms.process_room(room_id, 280)
    all_banks += pack_screen_bank(res["palette"], res["tiles_bin"], res["tilemap"])
    bank_cursor += 1

print(f"Room banks: {len(rooms_to_include)} rooms (banks {room_start_bank}..{bank_cursor-1})")

with open("all_data_banks.bin", "wb") as f:
    f.write(all_banks)

print(f"Total data banks generated: {len(all_banks)//BANK_SIZE} banks ({len(all_banks)//1024} KB)")

# Update game_banks.h
with open("game_banks.h", "w") as f:
    f.write("/* Game Bank Constants for Flashback SMS */\n")
    f.write("#ifndef GAME_BANKS_H\n#define GAME_BANKS_H\n\n")
    f.write(f"#define BANK_CONRAD_SPRITES0   {conrad_bank0}\n")
    f.write(f"#define BANK_TITLE_SCREEN      {title_bank_idx}\n")
    f.write(f"#define BANK_INSTRU_SCREEN     {instru_bank_idx}\n")
    f.write(f"#define BANK_ROOMS_START       {room_start_bank}\n\n")
    for r_idx, r_num in enumerate(rooms_to_include):
        f.write(f"#define ROOM_BANK_{r_num}           {room_start_bank + r_idx}\n")
    f.write("\n#endif\n")

print("Generated game_banks.h!")
