/* Title screen + main menu.
 *
 * The screen itself is a converted image bank (palette + tilemap + tiles) that
 * is paged into slot 2, so the menu code only has to load it and overlay the
 * menu text (8x8 font uploaded to a free VRAM slot) and the selection cursor
 * (a hardware sprite, pattern at the last VRAM tile slot).
 */
#include <stdint.h>
#include "title_menu.h"
#include "SMSlib.h"
#include "game_banks.h"
#include "audio.h"
#include "menu_font.h"

#define FONT_TILE0   384
#define CURSOR_TILE  447

/* small right-pointing cursor, sprite tile 447 (sprite tiles use the second
 * half of VRAM, i.e. SMS_useFirstHalfTilesforSprites(0)) */
static const unsigned char cursor_pattern[32] = {
    0xC0, 0x00, 0x00, 0x00,
    0xF0, 0x00, 0x00, 0x00,
    0xFC, 0x00, 0x00, 0x00,
    0xFF, 0x00, 0x00, 0x00,
    0xFF, 0x00, 0x00, 0x00,
    0xFC, 0x00, 0x00, 0x00,
    0xF0, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00
};

static unsigned char menu_selection = 0;
static unsigned int prev_keys = 0;

static void print_string (unsigned char x, unsigned char y, const char *str) {
    SMS_setNextTileatXY (x, y);
    while (*str) {
        char c = *str++;
        SMS_setTile ((c >= 32 && c < 96) ? (FONT_TILE0 + (c - 32)) : 0);
    }
}

void title_menu_init (void) {
    const unsigned char *bank_ptr = (const unsigned char *)0x8000;
    unsigned int num_tiles;
    unsigned char x, y;
    const unsigned int *tmap;

    SMS_displayOff ();
    SMS_useFirstHalfTilesforSprites (0);
    SMS_setSpriteMode (SPRITEMODE_NORMAL);
    SMS_initSprites ();
    SMS_finalizeSprites ();
    UNSAFE_SMS_copySpritestoSAT ();

    SMS_mapROMBank (BANK_TITLE_SCREEN);
    SMS_loadBGPalette (bank_ptr);
    SMS_setBackdropColor (0);
    num_tiles = (unsigned int)bank_ptr[16] | ((unsigned int)bank_ptr[17] << 8);
    SMS_loadTiles (bank_ptr + 2336, 0, (uint16_t)(num_tiles * 32));
    SMS_loadTiles (menu_font_tiles, FONT_TILE0, sizeof (menu_font_tiles));
    SMS_loadTiles (cursor_pattern, CURSOR_TILE, 32);

    tmap = (const unsigned int *)(bank_ptr + 32);
    for (y = 0; y < 24; ++y) {
        SMS_setNextTileatXY (0, y);
        for (x = 0; x < 32; ++x) SMS_setTile (tmap[y * 32 + x]);
    }

    print_string (9, 16, "1. START GAME");
    print_string (9, 18, "2. CINEMATIC INTRO");
    print_string (9, 20, "3. HOLOCUBE LOG");
    print_string (9, 22, "4. INSTRUCTIONS");

    SMS_setBGScrollX (0);
    SMS_setBGScrollY (0);
    SMS_displayOn ();
    menu_selection = 0;
    prev_keys = SMS_getKeysStatus ();
}

unsigned char title_menu_update (void) {
    unsigned int keys = SMS_getKeysStatus ();
    unsigned int pressed = keys & ~prev_keys;
    prev_keys = keys;

    if (pressed & PORT_A_KEY_UP) {
        menu_selection = menu_selection ? (menu_selection - 1) : (MENU_COUNT - 1);
        audio_play_sfx (SFX_MENU_MOVE);
    }
    if (pressed & PORT_A_KEY_DOWN) {
        menu_selection = (menu_selection + 1) % MENU_COUNT;
        audio_play_sfx (SFX_MENU_MOVE);
    }

    SMS_initSprites ();
    SMS_addSprite (56, 128 + menu_selection * 16, CURSOR_TILE);
    SMS_finalizeSprites ();

    if (pressed & (PORT_A_KEY_1 | PORT_A_KEY_2)) {
        audio_play_sfx (SFX_MENU_SELECT);
        return (unsigned char)(menu_selection + 1);
    }
    return 0;
}
