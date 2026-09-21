#include "instructions.h"
#include "SMSlib.h"
#include "game_banks.h"
#include "audio.h"

__sfr __at (0xbe) SMS_VDPDataPort;

static unsigned int prev_keys = 0;

void instructions_init(void) {
    const unsigned char *bank_ptr = (const unsigned char *)0x8000;
    unsigned int num_tiles;
    unsigned char x, y;
    const unsigned int *tmap;
    const unsigned char *tiles_src;
    unsigned int i;

    SMS_displayOff();
    SMS_mapROMBank(BANK_INSTRU_SCREEN);

    /* 1. Palette */
    SMS_loadBGPalette(bank_ptr);

    /* 2. Tiles direct to VRAM 0x0000 */
    num_tiles = (unsigned int)bank_ptr[16] | ((unsigned int)bank_ptr[17] << 8);
    tiles_src = bank_ptr + 2336;
    SMS_setAddr(0x4000 | 0);
    for (i = 0; i < num_tiles * 32; i++) {
        SMS_VDPDataPort = tiles_src[i];
    }

    /* 3. Tilemap */
    tmap = (const unsigned int *)(bank_ptr + 32);
    for (y = 0; y < 24; y++) {
        SMS_setNextTileatXY(0, y);
        for (x = 0; x < 32; x++) {
            SMS_setTile(tmap[y * 32 + x]);
        }
    }

    SMS_displayOn();
    prev_keys = SMS_getKeysStatus();
}

unsigned char instructions_update(void) {
    unsigned int keys = SMS_getKeysStatus();
    unsigned int pressed = keys & ~prev_keys;
    prev_keys = keys;

    if (pressed & (PORT_A_KEY_1 | PORT_A_KEY_2)) {
        audio_play_sfx(SFX_MENU_SELECT);
        return 1; /* Return to title */
    }

    return 0;
}
