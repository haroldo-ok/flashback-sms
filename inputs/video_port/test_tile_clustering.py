import struct, os, sys

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

def tile_diff(t1, t2):
    return sum(1 for a, b in zip(t1, t2) if a != b)

def convert_picture_quantized(bin_path, out_prefix, max_unique_tiles=350, win_w=256, win_h=192, y_offset=16):
    data = open(bin_path, "rb").read()
    fb = data[:256*224]
    pal_data = data[256*224:]
    
    # 1. Palette of 16 colors
    color_freq = {}
    for y in range(win_h):
        sy = y + y_offset
        for x in range(win_w):
            idx = fb[sy * 256 + x]
            r = (pal_data[idx*3] + 42) // 85
            g = (pal_data[idx*3+1] + 42) // 85
            b = (pal_data[idx*3+2] + 42) // 85
            if r > 3: r = 3
            if g > 3: g = 3
            if b > 3: b = 3
            c = (r, g, b)
            color_freq[c] = color_freq.get(c, 0) + 1
            
    pal_list = [(0, 0, 0)]
    sorted_cols = [c for c, _ in sorted(color_freq.items(), key=lambda x: x[1], reverse=True) if c != (0,0,0)]
    pal_list.extend(sorted_cols[:15])
    while len(pal_list) < 16:
        pal_list.append((0, 0, 0))
    pal_list = pal_list[:16]
    
    pal_bytes = bytearray([r | (g << 2) | (b << 4) for (r, g, b) in pal_list])
    
    # 2. Pixel grid
    grid = []
    for y in range(win_h):
        sy = y + y_offset
        row = []
        for x in range(win_w):
            idx = fb[sy * 256 + x]
            r = (pal_data[idx*3] + 42) // 85
            g = (pal_data[idx*3+1] + 42) // 85
            b = (pal_data[idx*3+2] + 42) // 85
            if r > 3: r = 3
            if g > 3: g = 3
            if b > 3: b = 3
            
            best_idx = 0
            best_dist = 999999
            for p_idx, (pr, pg, pb) in enumerate(pal_list):
                dist = (r-pr)**2 + (g-pg)**2 + (b-pb)**2
                if dist < best_dist:
                    best_dist = dist
                    best_idx = p_idx
            row.append(best_idx)
        grid.append(row)
        
    tw = win_w // 8
    th = win_h // 8
    raw_tiles = []
    for ty in range(th):
        for tx in range(tw):
            tile8x8 = []
            for py in range(8):
                for px in range(8):
                    tile8x8.append(grid[ty * 8 + py][tx * 8 + px])
            raw_tiles.append(tile8x8)
            
    # 3. Cluster / deduplicate tiles
    unique_tiles = []
    tilemap = []
    
    for t in raw_tiles:
        # Check exact match first
        found = -1
        for i, ut in enumerate(unique_tiles):
            if t == ut:
                found = i
                break
        if found != -1:
            tilemap.append(found)
        else:
            if len(unique_tiles) < max_unique_tiles:
                tilemap.append(len(unique_tiles))
                unique_tiles.append(t)
            else:
                # Find closest tile
                best_match = 0
                best_d = 999
                for i, ut in enumerate(unique_tiles):
                    d = tile_diff(t, ut)
                    if d < best_d:
                        best_d = d
                        best_match = i
                tilemap.append(best_match)
                
    print(f"Quantized {out_prefix}: {len(unique_tiles)} tiles, {len(tilemap)} map entries")
    
    all_tiles_bin = b''.join(planar_tile(t) for t in unique_tiles)
    
    c_text = f"/* Generated picture {out_prefix} */\n"
    c_text += f"#include \"{out_prefix}.h\"\n\n"
    c_text += f"const unsigned char {out_prefix}_palette[16] = {{\n  "
    c_text += ", ".join(f"0x{b:02x}" for b in pal_bytes) + "\n};\n\n"
    c_text += f"const unsigned char {out_prefix}_tiles[{len(all_tiles_bin)}] = {{\n"
    for i in range(0, len(all_tiles_bin), 16):
        c_text += "  " + ", ".join(f"0x{b:02x}" for b in all_tiles_bin[i:i+16]) + ",\n"
    c_text += "};\n\n"
    c_text += f"const unsigned int {out_prefix}_tilemap[{len(tilemap)}] = {{\n"
    for i in range(0, len(tilemap), 16):
        c_text += "  " + ", ".join(f"{t}" for t in tilemap[i:i+16]) + ",\n"
    c_text += "};\n"
    
    h_text = f"/* Header for picture {out_prefix} */\n#ifndef {out_prefix.upper()}_H\n#define {out_prefix.upper()}_H\n\n"
    h_text += f"#define {out_prefix.upper()}_NUM_TILES {len(unique_tiles)}\n"
    h_text += f"extern const unsigned char {out_prefix}_palette[16];\n"
    h_text += f"extern const unsigned char {out_prefix}_tiles[{len(all_tiles_bin)}];\n"
    h_text += f"extern const unsigned int {out_prefix}_tilemap[{len(tilemap)}];\n\n#endif\n"
    
    return c_text, h_text, all_tiles_bin, pal_bytes, tilemap

c1, h1, _, _, _ = convert_picture_quantized("menu1_pic.bin", "title_screen", 350, 256, 192, 16)
with open("generated_assets/title_screen.c", "w") as f: f.write(c1)
with open("generated_assets/title_screen.h", "w") as f: f.write(h1)

c2, h2, _, _, _ = convert_picture_quantized("instru_pic.bin", "instru_screen", 350, 256, 192, 16)
with open("generated_assets/instru_screen.c", "w") as f: f.write(c2)
with open("generated_assets/instru_screen.h", "w") as f: f.write(h2)
