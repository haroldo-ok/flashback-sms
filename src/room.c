/*
 * A room record is  u16 ntiles | 16-byte BG palette | 896-entry nametable |
 * ntiles*32 tile bytes, kept inside one 16 KB bank.  The 32x28 nametable is
 * the whole 256x224 Flashback room; the engine scrolls 0..32 lines.
 */
#include "SMSlib.h"
#include "data_index.h"
#include "room.h"
#include "tiledec.h"

unsigned char room_index;
unsigned char room_tiles;

void room_load(unsigned char index)
{
    const unsigned char *p;
    unsigned int n;
    room_index = index;
    SMS_mapROMBank(room_bank[index]);
    p = (const unsigned char *)room_addr[index];
    n = p[0] | ((unsigned int)p[1] << 8);
    room_tiles = (unsigned char)n;
    SMS_loadBGPalette(p + 2);
    SMS_loadTileMap(0, 0, p + 18, 32 * 28 * 2);
    /* Tiles sit at the TOP of VRAM (448-n..447) so the low tiles - the only
     * ones sprite patterns can use - stay free; the converter baked that base
     * into the nametable.  They come from the map's compact dictionary, one
     * id per VRAM slot; the dictionary pages its own banks, so this room's
     * bank is re-mapped before each id is read. */
    {
        const TileDict *dict = &room_dicts[room_dict[index]];
        const unsigned char *ids = p + 18 + 32 * 28 * 2;
        unsigned int i, id, base = 448 - n;
        unsigned char bank = room_bank[index];
        for (i = 0; i < n; i++) {
            SMS_mapROMBank(bank);
            id = ids[0] | ((unsigned int)ids[1] << 8);
            ids += 2;
            tile_upload(dict, id, base + i);
        }
        SMS_mapROMBank(bank);
    }
}

unsigned char room_find(unsigned char level, unsigned char room)
{
    unsigned char i;
    for (i = 0; i < NUM_ROOMS; i++)
        if (room_level[i] == level && room_num[i] == room) return i;
    return 0xFF;
}
