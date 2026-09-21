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

def process_room(room_num, max_unique_tiles=320, win_w=256, win_h=192, y_offset=16):
    bin_path = f"level1_rooms/room_{room_num:02d}.bin"
    if not os.path.exists(bin_path):
        return None
    data = open(bin_path, "rb").read()
    fb = data[:256*224]
    pal_data = data[256*224:]
    
    # 1. Palette
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
            
    # 3. Deduplicate / cluster tiles
    unique_tiles = []
    tilemap = []
    
    for t in raw_tiles:
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
                best_match = 0
                best_d = 999
                for i, ut in enumerate(unique_tiles):
                    d = tile_diff(t, ut)
                    if d < best_d:
                        best_d = d
                        best_match = i
                tilemap.append(best_match)
                
    # 4. Extract collision grid (32x24 blocks or 16x12 blocks)
    # A block is solid if non-zero foreground pixels exist in solid pattern
    solid_map = []
    for ty in range(th):
        for tx in range(tw):
            # Check if tile has substantial non-black pixels and forms a platform
            tid = tilemap[ty * tw + tx]
            tile = unique_tiles[tid]
            nonzeros = sum(1 for p in tile if p != 0)
            is_solid = 1 if nonzeros >= 28 and ty >= 5 else 0
            solid_map.append(is_solid)
            
    all_tiles_bin = b''.join(planar_tile(t) for t in unique_tiles)
    print(f"Room {room_num:02d}: {len(unique_tiles)} tiles ({len(all_tiles_bin)} bytes), 768 tilemap entries")
    return {
        "room_num": room_num,
        "ntiles": len(unique_tiles),
        "palette": pal_bytes,
        "tiles_bin": all_tiles_bin,
        "tilemap": tilemap,
        "solid_map": solid_map
    }

room_list = [26, 27, 28, 29, 30, 31, 32, 33]
room_data_results = []
for r in room_list:
    res = process_room(r, 280)
    if res:
        room_data_results.append(res)
