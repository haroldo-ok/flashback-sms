/*
 * Plays back a traced Flashback level: the background is a converted room,
 * and everything the original engine blitted (Conrad, monsters, objects) is
 * drawn with 8x16 hardware sprites whose patterns stream through VRAM tiles
 * 0..63 (room tiles live above that).  One stream frame = one 30 Hz game
 * frame = two SMS frames.
 */
#include "SMSlib.h"
#include "data_index.h"
#include "replay.h"
#include "room.h"

#define OP_END_FRAME 0
#define OP_ROOM      1
#define OP_SCROLL    2
#define OP_UPLOAD    3
#define OP_SPRITES   4
#define OP_NEXTBANK  7
#define OP_END       0xFF

unsigned char replay_active;
unsigned int  replay_frame;
unsigned int  replay_uploads;
unsigned char replay_sprites;
unsigned char replay_scroll;

static unsigned char s_bank;
static const unsigned char *s_ptr;

void replay_start(void)
{
#if HAS_REPLAY
    s_bank = REPLAY_BANK;
    s_ptr = (const unsigned char *)REPLAY_ADDR;
    replay_frame = 0;
    replay_uploads = 0;
    replay_sprites = 0;
    replay_scroll = 0;
    replay_active = 1;
    SMS_displayOff();
    SMS_useFirstHalfTilesforSprites(1);       /* sprite patterns = VRAM tiles 0..255 */
    SMS_setSpriteMode(SPRITEMODE_TALL);       /* 8x16 sprites, even tile index */
    SMS_initSprites();
    SMS_copySpritestoSAT();
    SMS_displayOn();
#endif
}

unsigned char replay_step(void)
{
    unsigned char op, n;
    if (!replay_active) return 0;
    SMS_mapROMBank(s_bank);
    for (;;) {
        op = *s_ptr++;
        switch (op) {
        case OP_END_FRAME:
            replay_frame++;
            return 1;
        case OP_ROOM: {
            /* copy the palette out FIRST: room_load() pages the room's bank
             * into the same window s_ptr points into */
            unsigned char pal[16], i, idx;
            idx = *s_ptr++;
            for (i = 0; i < 16; i++) pal[i] = *s_ptr++;
            SMS_displayOff();
            room_load(idx);
            SMS_mapROMBank(s_bank);
            SMS_loadSpritePalette(pal);
            SMS_displayOn();
            break;
        }
        case OP_SCROLL:
            replay_scroll = *s_ptr++;
            SMS_setBGScrollY(replay_scroll);
            break;
        case OP_UPLOAD:
            n = *s_ptr++;
            replay_sprites = replay_sprites;  /* (unchanged here) */
            while (n--) {
                unsigned char slot = s_ptr[0];
                unsigned int id = s_ptr[1] | ((unsigned int)s_ptr[2] << 8);
                s_ptr += 3;
                /* dictionary entries are 64 bytes = the two tiles of an 8x16 sprite */
                SMS_mapROMBank(SPR_DICT_BANK0 + (id >> 8));
                SMS_loadTiles((const unsigned char *)(0x8000 + ((id & 255) << 6)), slot << 1, 64);
                SMS_mapROMBank(s_bank);
                replay_uploads++;
            }
            break;
        case OP_SPRITES:
            n = *s_ptr++;
            replay_sprites = n;
            SMS_initSprites();
            while (n--) {
                SMS_addSprite(s_ptr[1], s_ptr[0], s_ptr[2] << 1);
                s_ptr += 3;
            }
            SMS_copySpritestoSAT();
            break;
        case OP_NEXTBANK:
            s_bank++;
            SMS_mapROMBank(s_bank);
            s_ptr = (const unsigned char *)0x8000;
            break;
        default:                              /* OP_END */
            replay_active = 0;
            return 0;
        }
    }
}
