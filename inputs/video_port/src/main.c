#include "SMSlib.h"
#include <stdio.h>
#include "video_player.h"
#include "title_menu.h"
#include "instructions.h"
#include "game_play.h"
#include "conrad.h"
#include "audio.h"
#include "game_banks.h"
#include "sprite_patterns.h"

SMS_EMBED_SEGA_ROM_HEADER(9999,0);
SMS_EMBED_SDSC_HEADER_AUTO_DATE(1,0,"Haroldo/Anthropic","Flashback SMS","Flashback Sega Master System Port");

enum {
    MODE_BOOT = 0,
    MODE_LOGOS,
    MODE_INTRO,
    MODE_TITLE,
    MODE_GAMEPLAY,
    MODE_HOLOCUBE,
    MODE_INSTRUCTIONS,
    MODE_GAMEOVER
};

/* Global playtest inspection symbols */
volatile unsigned char cur_mode = MODE_BOOT;
volatile unsigned char cur_room = 26;
volatile unsigned char conrad_x = 40;
volatile unsigned char conrad_y = 112;
volatile unsigned char conrad_health = 4;
volatile unsigned int  game_score = 0;

void main(void) {
    unsigned char next_opt;
    unsigned char game_status;

    SMS_init();
    SMS_displayOff();
    SMS_setSpriteMode(SPRITEMODE_NORMAL);
    SMS_useFirstHalfTilesforSprites(1);
    sprite_patterns_init();
    audio_init();

    /* 1. Play Delphine Logo sequence */
    cur_mode = MODE_LOGOS;
    video_player_play(&cutscene_logos);

    /* 2. Main Game Loop */
    cur_mode = MODE_TITLE;
    title_menu_init();

    for (;;) {
        switch (cur_mode) {
        case MODE_TITLE:
            SMS_waitForVBlank();
            UNSAFE_SMS_copySpritestoSAT();
            next_opt = title_menu_update();
            if (next_opt != 0) {
                if (next_opt == 1) {
                    /* START GAME: play Level 1 debut cutscene, then enter jungle! */
                    cur_mode = MODE_GAMEPLAY;
                    video_player_play(&cutscene_debut);
                    game_play_init();
                } else if (next_opt == 2) {
                    /* Full Cinematic Intro */
                    cur_mode = MODE_INTRO;
                    video_player_play(&cutscene_intro1);
                    video_player_play(&cutscene_intro2);
                    cur_mode = MODE_TITLE;
                    title_menu_init();
                } else if (next_opt == 3) {
                    /* Play Holocube Message */
                    cur_mode = MODE_HOLOCUBE;
                    video_player_play(&cutscene_holocube);
                    cur_mode = MODE_TITLE;
                    title_menu_init();
                } else if (next_opt == 4) {
                    /* Instructions Screen */
                    cur_mode = MODE_INSTRUCTIONS;
                    instructions_init();
                }
            }
            break;

        case MODE_INSTRUCTIONS:
            SMS_waitForVBlank();
            if (instructions_update()) {
                cur_mode = MODE_TITLE;
                title_menu_init();
            }
            break;

        case MODE_GAMEPLAY:
            game_status = game_play_update();
            game_play_draw();
            SMS_waitForVBlank();
            conrad_load_frame_tiles();
            UNSAFE_SMS_copySpritestoSAT();

            /* Update playtest inspection symbols */
            cur_room = g_cur_room;
            conrad_x = g_conrad.x;
            conrad_y = g_conrad.y;
            conrad_health = g_conrad.health;
            game_score = g_conrad.score;

            if (game_status == 2) {
                /* Holocube collected in game -> play cutscene! */
                video_player_play(&cutscene_holocube);
                /* Return to current room */
                game_play_init();
                g_conrad.has_holocube = 1;
            } else if (game_status == 3) {
                /* Player died -> play disintegration cutscene */
                video_player_play(&cutscene_desinteg);
                cur_mode = MODE_TITLE;
                title_menu_init();
            }
            break;

        default:
            cur_mode = MODE_TITLE;
            title_menu_init();
            break;
        }
    }
}
