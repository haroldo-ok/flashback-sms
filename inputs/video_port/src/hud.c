#include "hud.h"
#include "conrad.h"
#include "SMSlib.h"

void hud_init(void) {
}

void hud_draw(void) {
    unsigned char i;

    /* Draw 4 shield crystal units */
    for (i = 0; i < 4; i++) {
        if (i < g_conrad.health) {
            SMS_addSprite(8 + i * 8, 8, 16); /* Tile 16: Full shield */
        } else {
            SMS_addSprite(8 + i * 8, 8, 17); /* Tile 17: Empty shield slot */
        }
    }

    /* Draw weapon status icon */
    if (g_conrad.gun_drawn) {
        SMS_addSprite(48, 8, 18); /* Tile 18: Gun icon */
    }

    /* Draw inventory item icon */
    if (g_conrad.has_holocube) {
        SMS_addSprite(232, 8, 19); /* Tile 19: Holocube icon */
    }
}
