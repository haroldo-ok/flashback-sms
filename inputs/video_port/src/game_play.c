#include "game_play.h"
#include "conrad.h"
#include "actors.h"
#include "hud.h"
#include "audio.h"
#include "game_banks.h"
#include "sprite_patterns.h"
#include "SMSlib.h"

__sfr __at (0xbe) SMS_VDPDataPort;

int g_cur_room = 26;
unsigned char g_trigger_death_scene = 0;
static unsigned int prev_keys = 0;

static const unsigned char conrad_sprite_pal[16] = {
    0x00, /* 0: Transparent */
    0x3F, /* 1: White shoes highlight (0x3F) */
    0x3A, /* 2: Light blue jeans (0x3A) */
    0x35, /* 3: Mid blue jeans (0x35) */
    0x25, /* 4: Blue jeans shadow (0x25) */
    0x20, /* 5: Dark blue jeans shadow (0x20) */
    0x10, /* 6: Deep shadow jeans (0x10) */
    0x0B, /* 7: Brown jacket highlight (0x0B) */
    0x06, /* 8: Brown jacket shadow (0x06) */
    0x06, /* 9: Brown hair highlight (0x06) */
    0x01, /* 10: Dark brown hair (0x01) */
    0x1B, /* 11: Peach skin face & hands (0x1B) */
    0x2A, /* 12: White t-shirt shadow / grey metal (0x2A) */
    0x3F, /* 13: White t-shirt highlight (0x3F) */
    0x00, /* 14: Black belt & holster (0x00) */
    0x30  /* 15: Cyan laser bolt & shield glow (0x30) */
};

unsigned char game_is_solid(int x, int y) {
    (void)x;
    if (y >= 112) return 1;
    return 0;
}

static void load_room(int room_num) {
    const unsigned char *bank_ptr = (const unsigned char *)0x8000;
    unsigned int num_tiles;
    unsigned char x, y;
    const unsigned int *tmap;
    const unsigned char *tiles_src;
    unsigned char bank_idx;
    unsigned int i;

    g_cur_room = room_num;
    bank_idx = ROOM_BANK_26 + (room_num - 26);

    SMS_displayOff();
    SMS_mapROMBank(bank_idx);

    /* 1. Palette */
    SMS_loadBGPalette(bank_ptr);
    SMS_loadSpritePalette(conrad_sprite_pal);

    /* 2. Tiles (direct VDP write starting at tile 64 = VRAM 0x0800) */
    num_tiles = (unsigned int)bank_ptr[16] | ((unsigned int)bank_ptr[17] << 8);
    tiles_src = bank_ptr + 2336;
    SMS_setAddr(0x4000 | (64 * 32));
    for (i = 0; i < num_tiles * 32; i++) {
        SMS_VDPDataPort = tiles_src[i];
    }

    /* 3. Static Sprites (Tiles 1..32) */
    sprite_patterns_init();

    /* 4. Tilemap */
    tmap = (const unsigned int *)(bank_ptr + 32);
    for (y = 0; y < 24; y++) {
        SMS_setNextTileatXY(0, y);
        for (x = 0; x < 32; x++) {
            SMS_setTile(64 + tmap[y * 32 + x]);
        }
    }

    /* 5. Actors */
    actors_init_for_room(room_num);

    /* 6. Force Conrad frame upload */
    conrad_force_reload_frame();

    SMS_displayOn();
}

void game_play_init(void) {
    g_trigger_holocube_scene = 0;
    g_trigger_death_scene = 0;
    conrad_init(40, 112);
    load_room(26);
    hud_init();
    audio_init();
    prev_keys = SMS_getKeysStatus();
}

unsigned char game_play_update(void) {
    unsigned int keys = SMS_getKeysStatus();
    unsigned int pressed = keys & ~prev_keys;
    prev_keys = keys;

    conrad_update(keys, pressed);

    if (g_conrad.x > 244) {
        if (g_cur_room == 26) {
            g_conrad.x = 16;
            load_room(27);
        } else if (g_cur_room == 27) {
            g_conrad.x = 16;
            load_room(28);
        } else if (g_cur_room == 28) {
            g_conrad.x = 244;
        } else if (g_cur_room == 29) {
            g_conrad.x = 16;
            load_room(30);
        }
    } else if (g_conrad.x < 8) {
        if (g_cur_room == 27) {
            g_conrad.x = 236;
            load_room(26);
        } else if (g_cur_room == 28) {
            g_conrad.x = 236;
            load_room(27);
        } else if (g_cur_room == 30) {
            g_conrad.x = 236;
            load_room(29);
        } else {
            g_conrad.x = 8;
        }
    }

    actors_update();
    audio_update();

    if (g_trigger_holocube_scene) {
        g_trigger_holocube_scene = 0;
        return 2;
    }

    if (g_conrad.state == CONRAD_STATE_DIE && g_conrad.anim_frame >= 8) {
        return 3;
    }

    return 0;
}

void game_play_draw(void) {
    SMS_initSprites();
    conrad_draw();
    actors_draw();
    hud_draw();
    SMS_finalizeSprites();
}
