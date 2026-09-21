#ifndef CONRAD_H
#define CONRAD_H

#include "SMSlib.h"

#define CONRAD_TOTAL_FRAMES 357

enum {
    CONRAD_STATE_IDLE = 0,
    CONRAD_STATE_TURN,
    CONRAD_STATE_WALK,
    CONRAD_STATE_RUN,
    CONRAD_STATE_RUN_STOP,
    CONRAD_STATE_DRAW_GUN,
    CONRAD_STATE_AIM_GUN,
    CONRAD_STATE_SHOOT,
    CONRAD_STATE_HOLSTER_GUN,
    CONRAD_STATE_CROUCH,
    CONRAD_STATE_STAND_CROUCH,
    CONRAD_STATE_PICKUP,
    CONRAD_STATE_ROLL,
    CONRAD_STATE_JUMP_UP,
    CONRAD_STATE_JUMP_FORWARD,
    CONRAD_STATE_FALL,
    CONRAD_STATE_CLIMB,
    CONRAD_STATE_HURT,
    CONRAD_STATE_DIE
};

typedef struct {
    int x;            /* X pixel pos */
    int y;            /* Y pixel pos */
    int vx;           /* X velocity */
    int vy;           /* Y velocity */
    unsigned char state;
    unsigned char anim_frame;
    unsigned char anim_timer;
    unsigned char facing;      /* 0 = right, 1 = left */
    unsigned char gun_drawn;   /* 0 = no, 1 = yes */
    unsigned char on_ground;   /* 0 = airborne, 1 = grounded */
    unsigned char health;      /* 1..4 shields */
    unsigned char has_holocube;
    unsigned char has_gun;
    unsigned char has_card;
    unsigned int  score;
} conrad_t;

extern conrad_t g_conrad;

void conrad_init(int start_x, int start_y);
void conrad_update(unsigned int keys, unsigned int pressed);
void conrad_draw(void);
void conrad_load_frame_tiles(void);
void conrad_take_damage(unsigned char dmg);
void conrad_force_reload_frame(void);

#endif
