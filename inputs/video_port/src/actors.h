#ifndef ACTORS_H
#define ACTORS_H

#include "SMSlib.h"

enum {
    ACTOR_NONE = 0,
    ACTOR_HOLOCUBE,
    ACTOR_DRONE,
    ACTOR_MUTANT,
    ACTOR_PLANT,
    ACTOR_GATE,
    ACTOR_SWITCH,
    ACTOR_RECHARGER,
    ACTOR_CREDITS
};

typedef struct {
    unsigned char type;
    int x;
    int y;
    int vx;
    int vy;
    unsigned char state;
    unsigned char timer;
    unsigned char health;
    unsigned char active;
} actor_t;

typedef struct {
    int x;
    int y;
    int vx;
    unsigned char is_player;
    unsigned char active;
} bullet_t;

#define MAX_ACTORS  8
#define MAX_BULLETS 8

extern actor_t g_actors[MAX_ACTORS];
extern bullet_t g_bullets[MAX_BULLETS];
extern unsigned char g_trigger_holocube_scene;

void actors_init_for_room(int room_num);
void actors_update(void);
void actors_draw(void);
void actors_spawn_bullet(int x, int y, int vx, unsigned char is_player);
unsigned char actors_is_item_near(int x, int y);

#endif
