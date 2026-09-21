import struct

# Sega Master System Sprite Palette (16 colors):
# 0: Transparent
# 1: White shoes highlight (0x3F)
# 2: Light blue jeans (0x3A)
# 3: Mid blue jeans (0x35)
# 4: Blue jeans shadow (0x25)
# 5: Dark blue jeans shadow (0x20)
# 6: Deep shadow jeans (0x10)
# 7: Brown jacket highlight (0x0B)
# 8: Brown jacket shadow (0x06)
# 9: Brown hair highlight (0x06)
# 10: Dark brown hair (0x01)
# 11: Peach skin face & hands (0x1B)
# 12: White t-shirt shadow / grey metal (0x2A)
# 13: White t-shirt highlight (0x3F)
# 14: Black belt & holster (0x00)
# 15: Cyan laser bolt & shield glow (0x30)

def make_tile_from_strings(rows, col_map):
    b = bytearray(32)
    for y in range(8):
        row = rows[y]
        p0, p1, p2, p3 = 0, 0, 0, 0
        for x in range(8):
            ch = row[x]
            col = col_map.get(ch, 0)
            bit = 7 - x
            p0 |= ((col >> 0) & 1) << bit
            p1 |= ((col >> 1) & 1) << bit
            p2 |= ((col >> 2) & 1) << bit
            p3 |= ((col >> 3) & 1) << bit
        b[y*4 + 0] = p0
        b[y*4 + 1] = p1
        b[y*4 + 2] = p2
        b[y*4 + 3] = p3
    return bytes(b)

smap = {
    '.': 0, 'W': 1, 'C': 2, 'B': 3, 'D': 4,
    'J': 7, 'K': 8, 'H': 10, 'S': 11, 'G': 12, 'L': 15
}

static_tiles = {}

# Tile 1: Title Menu Cursor Arrow
static_tiles[1] = make_tile_from_strings([
    "W.......",
    "WW......",
    "WWW.....",
    "WWWW....",
    "WWW.....",
    "WW......",
    "W.......",
    "........"
], smap)

# Tile 16: HUD Full Shield Crystal Unit
static_tiles[16] = make_tile_from_strings([
    "WWWWWWWW",
    "WCCCCLLW",
    "WCCCCCCW",
    "WCCCCCCW",
    "WCCCCCCW",
    "WCCCCCCW",
    "WCCCCCCW",
    "WWWWWWWW"
], smap)

# Tile 17: HUD Empty Shield Slot
static_tiles[17] = make_tile_from_strings([
    "GGGGGGGG",
    "G......G",
    "G......G",
    "G......G",
    "G......G",
    "G......G",
    "G......G",
    "GGGGGGGG"
], smap)

# Tile 18: HUD Gun Icon
static_tiles[18] = make_tile_from_strings([
    "........",
    ".WWWW...",
    ".WGGW...",
    ".WGGWWWW",
    ".WGGGGGW",
    "..WWWWWW",
    "...WGGW.",
    "...WWWW."
], smap)

# Tile 19: HUD Holocube Icon / Item
static_tiles[19] = make_tile_from_strings([
    "..WWWW..",
    ".WCCCCW.",
    "WCCCCCCW",
    "WCCWWCCW",
    "WCCWWCCW",
    "WCCCCCCW",
    ".WCCCCW.",
    "..WWWW.."
], smap)

# Tile 20-21: Security Drone (16x8)
static_tiles[20] = make_tile_from_strings([ # Drone Left
    "..GGGG..",
    ".GGWWGG.",
    "GGWCCWGG",
    "GGWCCWGG",
    "GGWWWWGG",
    ".GGGGGG.",
    "..DDDD..",
    "...CC..."
], smap)

static_tiles[21] = make_tile_from_strings([ # Drone Right
    "..GGGG..",
    ".GGWWGG.",
    "GGWCCWGG",
    "GGWCCWGG",
    "GGWWWWGG",
    ".GGGGGG.",
    "..DDDD..",
    "...CC..."
], smap)

# Tile 22-25: Mutant Guard (16x16)
static_tiles[22] = make_tile_from_strings([ # Top-Left
    "...KKKK.",
    "..KKWKK.",
    ".KKKKKK.",
    ".KKKKKK.",
    "..KKKK..",
    ".KKDDKK.",
    ".KKDDKK.",
    "KKDDDDKK"
], smap)

static_tiles[23] = make_tile_from_strings([ # Top-Right
    ".KKKK...",
    ".KKWKK..",
    ".KKKKKK.",
    ".KKKKKK.",
    "..KKKK..",
    ".KKDDKK.",
    ".KKDDKK.",
    "KKDDDDKK"
], smap)

static_tiles[24] = make_tile_from_strings([ # Bottom-Left
    ".KKDDKK.",
    ".KKDDKK.",
    "..D..D..",
    "..D..D..",
    "..D..D..",
    "..D..D..",
    ".DD..DD.",
    "DDD..DDD"
], smap)

static_tiles[25] = make_tile_from_strings([ # Bottom-Right
    ".KKDDKK.",
    ".KKDDKK.",
    "..D..D..",
    "..D..D..",
    "..D..D..",
    "..D..D..",
    ".DD..DD.",
    "DDD..DDD"
], smap)

# Tile 26: Carnivorous Plant (8x8)
static_tiles[26] = make_tile_from_strings([
    "J..JJ..J",
    "JJ.JJ.JJ",
    "JJJJJJJJ",
    ".JJJJJJ.",
    ".JHHHHJ.",
    ".JHHHHJ.",
    "..JJJJ..",
    "...JJ..."
], smap)

# Tile 28: Laser Security Gate (8x8)
static_tiles[28] = make_tile_from_strings([
    "...CC...",
    "..CCCC..",
    ".CCWWCC.",
    ".CCWWCC.",
    ".CCWWCC.",
    ".CCWWCC.",
    "..CCCC..",
    "...CC..."
], smap)

# Tile 29: Floor Switch (8x8)
static_tiles[29] = make_tile_from_strings([
    "........",
    "........",
    "........",
    "........",
    "........",
    ".WWWWWW.",
    "WCCCCCCW",
    "GGGGGGGG"
], smap)

# Tile 30: Shield Recharger (8x8)
static_tiles[30] = make_tile_from_strings([
    ".WWWWWW.",
    "WCCCCCCW",
    "WCWWWWWW",
    "WCCCCCCW",
    "WCWWWWWW",
    "WCCCCCCW",
    "WCCCCCCW",
    "WWWWWWWW"
], smap)

# Tile 32: Laser Projectile Bolt (8x8)
static_tiles[32] = make_tile_from_strings([
    "........",
    "........",
    ".CCCCCC.",
    "CCCCCCCC",
    "CCCCCCCC",
    ".CCCCCC.",
    "........",
    "........"
], smap)

with open("src/sprite_patterns.h", "w") as f:
    f.write("#ifndef SPRITE_PATTERNS_H\n#define SPRITE_PATTERNS_H\n\nvoid sprite_patterns_init(void);\n\n#endif\n")

with open("src/sprite_patterns.c", "w") as f:
    f.write('#include "sprite_patterns.h"\n#include "SMSlib.h"\n\n')
    f.write('__sfr __at (0xbe) SMS_VDPDataPort;\n\n')
    for tid, tdata in sorted(static_tiles.items()):
        f.write(f'static const unsigned char spr_tile_{tid}[32] = {{\n')
        for i in range(0, 32, 8):
            f.write('    ' + ', '.join(f'0x{b:02x}' for b in tdata[i:i+8]) + ',\n')
        f.write('};\n\n')
    
    f.write('static void load_tile_direct(unsigned char tile_num, const unsigned char *src) {\n')
    f.write('    unsigned char i;\n')
    f.write('    SMS_setAddr(0x4000 | ((unsigned int)tile_num * 32));\n')
    f.write('    for (i = 0; i < 32; i++) {\n')
    f.write('        SMS_VDPDataPort = src[i];\n')
    f.write('    }\n')
    f.write('}\n\n')

    f.write('void sprite_patterns_init(void) {\n')
    for tid in sorted(static_tiles.keys()):
        f.write(f'    load_tile_direct({tid}, spr_tile_{tid});\n')
    f.write('}\n')

print("Generated src/sprite_patterns.c and src/sprite_patterns.h with updated palette")
