#include "actors.h"
#include "conrad.h"
#include "audio.h"
#include "SMSlib.h"

actor_t g_actors[MAX_ACTORS];
bullet_t g_bullets[MAX_BULLETS];
unsigned char g_trigger_holocube_scene = 0;

void actors_spawn_bullet(int x, int y, int vx, unsigned char is_player) {
    unsigned char i;
    for (i = 0; i < MAX_BULLETS; i++) {
        if (!g_bullets[i].active) {
            g_bullets[i].x = x;
            g_bullets[i].y = y;
            g_bullets[i].vx = vx;
            g_bullets[i].is_player = is_player;
            g_bullets[i].active = 1;
            break;
        }
    }
}

unsigned char actors_is_item_near(int x, int y) {
    unsigned char i;
    for (i = 0; i < MAX_ACTORS; i++) {
        if (!g_actors[i].active) continue;
        if (g_actors[i].type == ACTOR_HOLOCUBE || g_actors[i].type == ACTOR_CREDITS ||
            g_actors[i].type == ACTOR_SWITCH || g_actors[i].type == ACTOR_RECHARGER) {
            if (x + 24 >= g_actors[i].x && x <= g_actors[i].x + 24 &&
                y <= g_actors[i].y + 16 && y + 48 >= g_actors[i].y) {
                return 1;
            }
        }
    }
    return 0;
}

void actors_init_for_room(int room_num) {
    unsigned char i;
    for (i = 0; i < MAX_ACTORS; i++) g_actors[i].active = 0;
    for (i = 0; i < MAX_BULLETS; i++) g_bullets[i].active = 0;

    switch (room_num) {
    case 26:
        if (!g_conrad.has_holocube) {
            g_actors[0].type = ACTOR_HOLOCUBE;
            g_actors[0].x = 160;
            g_actors[0].y = 144;
            g_actors[0].active = 1;
        }
        g_actors[1].type = ACTOR_DRONE;
        g_actors[1].x = 180;
        g_actors[1].y = 48;
        g_actors[1].vx = -1;
        g_actors[1].health = 2;
        g_actors[1].active = 1;
        break;

    case 27:
        g_actors[0].type = ACTOR_PLANT;
        g_actors[0].x = 120;
        g_actors[0].y = 144;
        g_actors[0].health = 1;
        g_actors[0].active = 1;

        g_actors[1].type = ACTOR_RECHARGER;
        g_actors[1].x = 210;
        g_actors[1].y = 144;
        g_actors[1].active = 1;
        break;

    case 28:
        g_actors[0].type = ACTOR_SWITCH;
        g_actors[0].x = 80;
        g_actors[0].y = 144;
        g_actors[0].active = 1;

        g_actors[1].type = ACTOR_GATE;
        g_actors[1].x = 190;
        g_actors[1].y = 120;
        g_actors[1].state = 1;
        g_actors[1].active = 1;
        break;

    case 29:
        g_actors[0].type = ACTOR_MUTANT;
        g_actors[0].x = 170;
        g_actors[0].y = 136;
        g_actors[0].vx = -1;
        g_actors[0].health = 3;
        g_actors[0].active = 1;

        g_actors[1].type = ACTOR_CREDITS;
        g_actors[1].x = 60;
        g_actors[1].y = 144;
        g_actors[1].active = 1;
        break;

    default:
        break;
    }
}

void actors_update(void) {
    unsigned char i, j;

    /* 1. Update Projectiles */
    for (i = 0; i < MAX_BULLETS; i++) {
        if (!g_bullets[i].active) continue;

        g_bullets[i].x += g_bullets[i].vx;
        if (g_bullets[i].x < 0 || g_bullets[i].x > 256) {
            g_bullets[i].active = 0;
            continue;
        }

        if (g_bullets[i].is_player) {
            for (j = 0; j < MAX_ACTORS; j++) {
                if (!g_actors[j].active) continue;
                if (g_actors[j].type == ACTOR_DRONE || g_actors[j].type == ACTOR_MUTANT || g_actors[j].type == ACTOR_PLANT) {
                    if (g_bullets[i].x >= g_actors[j].x - 8 && g_bullets[i].x <= g_actors[j].x + 16 &&
                        g_bullets[i].y >= g_actors[j].y - 8 && g_bullets[i].y <= g_actors[j].y + 24) {
                        g_bullets[i].active = 0;
                        if (g_actors[j].health > 1) {
                            g_actors[j].health--;
                            audio_play_sfx(SFX_DAMAGE);
                        } else {
                            g_actors[j].active = 0;
                            g_conrad.score += 100;
                            audio_play_sfx(SFX_DAMAGE);
                        }
                        break;
                    }
                }
            }
        } else {
            if (g_bullets[i].x >= g_conrad.x - 4 && g_bullets[i].x <= g_conrad.x + 16 &&
                g_bullets[i].y >= g_conrad.y && g_bullets[i].y <= g_conrad.y + 40) {
                g_bullets[i].active = 0;
                conrad_take_damage(1);
            }
        }
    }

    /* 2. Update Interactive Actors and Enemies */
    for (i = 0; i < MAX_ACTORS; i++) {
        if (!g_actors[i].active) continue;

        switch (g_actors[i].type) {
        case ACTOR_HOLOCUBE:
            /* Conrad touching or standing near Holocube */
            if (g_conrad.x + 20 >= g_actors[i].x && g_conrad.x <= g_actors[i].x + 20 &&
                g_conrad.y <= g_actors[i].y + 16 && g_conrad.y + 48 >= g_actors[i].y) {
                g_actors[i].active = 0;
                g_conrad.has_holocube = 1;
                g_conrad.score += 500;
                audio_play_sfx(SFX_PICKUP);
                g_trigger_holocube_scene = 1;
            }
            break;

        case ACTOR_RECHARGER:
            if (g_conrad.x + 20 >= g_actors[i].x && g_conrad.x <= g_actors[i].x + 20 &&
                g_conrad.y <= g_actors[i].y + 16 && g_conrad.y + 48 >= g_actors[i].y) {
                if (g_conrad.health < 4) {
                    g_conrad.health = 4;
                    audio_play_sfx(SFX_PICKUP);
                }
            }
            break;

        case ACTOR_CREDITS:
            if (g_conrad.x + 20 >= g_actors[i].x && g_conrad.x <= g_actors[i].x + 20 &&
                g_conrad.y <= g_actors[i].y + 16 && g_conrad.y + 48 >= g_actors[i].y) {
                g_actors[i].active = 0;
                g_conrad.score += 250;
                audio_play_sfx(SFX_PICKUP);
            }
            break;

        case ACTOR_DRONE:
            g_actors[i].x += g_actors[i].vx;
            if (g_actors[i].x < 40) { g_actors[i].x = 40; g_actors[i].vx = 1; }
            if (g_actors[i].x > 220) { g_actors[i].x = 220; g_actors[i].vx = -1; }

            g_actors[i].timer++;
            if (g_actors[i].timer >= 60) {
                g_actors[i].timer = 0;
                if (g_actors[i].x >= g_conrad.x - 30 && g_actors[i].x <= g_conrad.x + 30) {
                    actors_spawn_bullet(g_actors[i].x + 4, g_actors[i].y + 12, (g_conrad.x < g_actors[i].x) ? -3 : 3, 0);
                    audio_play_sfx(SFX_LASER_SHOT);
                }
            }
            break;

        case ACTOR_MUTANT:
            g_actors[i].x += g_actors[i].vx;
            if (g_actors[i].x < 100) { g_actors[i].x = 100; g_actors[i].vx = 1; }
            if (g_actors[i].x > 220) { g_actors[i].x = 220; g_actors[i].vx = -1; }

            g_actors[i].timer++;
            if (g_actors[i].timer >= 45) {
                g_actors[i].timer = 0;
                if ((g_actors[i].vx < 0 && g_conrad.x < g_actors[i].x) ||
                    (g_actors[i].vx > 0 && g_conrad.x > g_actors[i].x)) {
                    actors_spawn_bullet(g_actors[i].x + 4, g_actors[i].y + 10, g_actors[i].vx * 4, 0);
                    audio_play_sfx(SFX_LASER_SHOT);
                }
            }
            break;

        case ACTOR_PLANT:
            if (g_conrad.x + 16 >= g_actors[i].x - 8 && g_conrad.x <= g_actors[i].x + 20 &&
                g_conrad.y <= g_actors[i].y + 16 && g_conrad.y + 44 >= g_actors[i].y) {
                conrad_take_damage(1);
            }
            break;

        case ACTOR_SWITCH:
            if (g_conrad.x + 20 >= g_actors[i].x && g_conrad.x <= g_actors[i].x + 20 &&
                g_conrad.y <= g_actors[i].y + 16 && g_conrad.y + 48 >= g_actors[i].y) {
                for (j = 0; j < MAX_ACTORS; j++) {
                    if (g_actors[j].active && g_actors[j].type == ACTOR_GATE) {
                        if (g_actors[j].state == 1) {
                            g_actors[j].state = 0;
                            audio_play_sfx(SFX_SWITCH);
                        }
                    }
                }
            }
            break;

        default:
            break;
        }
    }
}

void actors_draw(void) {
    unsigned char i;

    for (i = 0; i < MAX_BULLETS; i++) {
        if (!g_bullets[i].active) continue;
        SMS_addSprite(g_bullets[i].x, g_bullets[i].y, 32); /* Tile 32: Laser bolt */
    }

    for (i = 0; i < MAX_ACTORS; i++) {
        if (!g_actors[i].active) continue;

        switch (g_actors[i].type) {
        case ACTOR_HOLOCUBE:
            SMS_addSprite(g_actors[i].x, g_actors[i].y, 19); /* Tile 19: Holocube */
            break;

        case ACTOR_DRONE:
            SMS_addSprite(g_actors[i].x,     g_actors[i].y, 20); /* Tile 20-21: Drone */
            SMS_addSprite(g_actors[i].x + 8, g_actors[i].y, 21);
            break;

        case ACTOR_MUTANT:
            SMS_addSprite(g_actors[i].x,     g_actors[i].y,      22); /* Tile 22-25: Mutant */
            SMS_addSprite(g_actors[i].x + 8, g_actors[i].y,      23);
            SMS_addSprite(g_actors[i].x,     g_actors[i].y + 8,  24);
            SMS_addSprite(g_actors[i].x + 8, g_actors[i].y + 8,  25);
            break;

        case ACTOR_PLANT:
            SMS_addSprite(g_actors[i].x, g_actors[i].y, 26); /* Tile 26: Plant */
            break;

        case ACTOR_GATE:
            if (g_actors[i].state == 1) {
                SMS_addSprite(g_actors[i].x, g_actors[i].y,      28); /* Tile 28: Laser beam */
                SMS_addSprite(g_actors[i].x, g_actors[i].y + 8,  28);
                SMS_addSprite(g_actors[i].x, g_actors[i].y + 16, 28);
                SMS_addSprite(g_actors[i].x, g_actors[i].y + 24, 28);
            }
            break;

        case ACTOR_SWITCH:
            SMS_addSprite(g_actors[i].x, g_actors[i].y, 29); /* Tile 29: Switch */
            break;

        case ACTOR_RECHARGER:
            SMS_addSprite(g_actors[i].x, g_actors[i].y, 30); /* Tile 30: Recharger */
            break;

        case ACTOR_CREDITS:
            SMS_addSprite(g_actors[i].x, g_actors[i].y, 19);
            break;

        default:
            break;
        }
    }
}
