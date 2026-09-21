#include "title_menu.h"
#include "SMSlib.h"
#include "game_banks.h"
#include "audio.h"
#include "menu_font.h"

__sfr __at (0xbe) SMS_VDPDataPort;

static unsigned char menu_selection = 0;
static unsigned int prev_keys = 0;

static void print_string(unsigned char x, unsigned char y, const char *str) {
    SMS_setNextTileatXY(x, y);
    while (*str) {
        char c = *str++;
        if (c >= 32 && c < 96) {
            SMS_setTile(384 + (c - 32));
        } else {
            SMS_setTile(0);
        }
    }
}

void title_menu_init(void) {
    const unsigned char *bank_ptr = (const unsigned char *)0x8000;
    unsigned int num_tiles;
    unsigned char x, y;
    const unsigned int *tmap;
    const unsigned char *tiles_src;
    unsigned int i;

    SMS_displayOff();
    SMS_mapROMBank(BANK_TITLE_SCREEN);

    /* 1. Palette */
    SMS_loadBGPalette(bank_ptr);

    /* 2. Tiles direct to VRAM 0x0000 */
    num_tiles = (unsigned int)bank_ptr[16] | ((unsigned int)bank_ptr[17] << 8);
    tiles_src = bank_ptr + 2336;
    SMS_setAddr(0x4000 | 0);
    for (i = 0; i < num_tiles * 32; i++) {
        SMS_VDPDataPort = tiles_src[i];
    }

    /* 3. Load font tiles into VRAM starting at tile 384 */
    SMS_setAddr(0x4000 | (384 * 32));
    for (i = 0; i < sizeof(menu_font_tiles); i++) {
        SMS_VDPDataPort = menu_font_tiles[i];
    }

    /* 4. Tilemap */
    tmap = (const unsigned int *)(bank_ptr + 32);
    for (y = 0; y < 24; y++) {
        SMS_setNextTileatXY(0, y);
        for (x = 0; x < 32; x++) {
            SMS_setTile(tmap[y * 32 + x]);
        }
    }

    /* 5. Print menu items */
    print_string(9, 16, "1. START GAME");
    print_string(9, 18, "2. CINEMATIC INTRO");
    print_string(9, 20, "3. HOLOCUBE LOG");
    print_string(9, 22, "4. INSTRUCTIONS");

    SMS_displayOn();
    menu_selection = 0;
    prev_keys = SMS_getKeysStatus();
}

unsigned char title_menu_update(void) {
    unsigned int keys = SMS_getKeysStatus();
    unsigned int pressed = keys & ~prev_keys;
    prev_keys = keys;

    if (pressed & PORT_A_KEY_UP) {
        if (menu_selection > 0) menu_selection--;
        else menu_selection = MENU_COUNT - 1;
        audio_play_sfx(SFX_MENU_MOVE);
    }
    if (pressed & PORT_A_KEY_DOWN) {
        if (menu_selection < MENU_COUNT - 1) menu_selection++;
        else menu_selection = 0;
        audio_play_sfx(SFX_MENU_MOVE);
    }

    /* Draw UI cursor sprite next to selected menu item */
    SMS_initSprites();
    SMS_addSprite(56, 128 + menu_selection * 16, 1);
    SMS_finalizeSprites();

    if (pressed & (PORT_A_KEY_1 | PORT_A_KEY_2)) {
        audio_play_sfx(SFX_MENU_SELECT);
        return menu_selection + 1; /* 1..4 */
    }

    return 0; /* Still in menu */
}
