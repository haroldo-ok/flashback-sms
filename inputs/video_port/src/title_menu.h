#ifndef TITLE_MENU_H
#define TITLE_MENU_H

#include "SMSlib.h"

enum {
    MENU_START_GAME = 0,
    MENU_CINEMATIC_INTRO,
    MENU_PLAY_HOLOCUBE,
    MENU_INSTRUCTIONS,
    MENU_COUNT
};

void title_menu_init(void);
unsigned char title_menu_update(void);

#endif
