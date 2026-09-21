#include "conrad.h"
#include "audio.h"
#include "game_banks.h"
#include "actors.h"

__sfr __at (0xbe) SMS_VDPDataPort;

conrad_t g_conrad;
static unsigned int last_uploaded_frame = 0xFFFF;
static unsigned char last_uploaded_facing = 0xFF;

/* Fast unrolled VDP OTIR upload (320 bytes = 10 4bpp tiles) */
static void fast_upload_320_bytes(const unsigned char *src) __z88dk_fastcall {
    (void)src;
    __asm
        ld c, #0xbe
        .rept 320
        outi
        .endm
    __endasm;
}

void conrad_force_reload_frame(void) {
    last_uploaded_frame = 0xFFFF;
    last_uploaded_facing = 0xFF;
}

/* Authentic Flashback Rotoscoped Animation Frame Sequences */
static const unsigned int anim_idle[]        = { 0 };
static const unsigned int anim_turn[]        = { 1, 2, 3, 4 };
static const unsigned int anim_walk[]        = { 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22 };
static const unsigned int anim_run[]         = { 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82 };
static const unsigned int anim_run_stop[]    = { 61, 62, 63, 83, 84, 85 };
static const unsigned int anim_draw_gun[]    = { 86, 87, 88, 89, 90, 91, 92, 93 };
static const unsigned int anim_aim_gun[]     = { 93 };
static const unsigned int anim_shoot[]       = { 93, 92, 91, 92, 93 };
static const unsigned int anim_holster_gun[] = { 109, 110, 111, 113, 114, 115, 116, 117, 118, 119, 0 };
static const unsigned int anim_crouch[]      = { 173, 174, 175, 176, 177, 178, 179, 180 };
static const unsigned int anim_stand_crouch[]= { 181, 182, 183 };
static const unsigned int anim_pickup[]      = { 299, 300, 301, 302, 303, 302, 301, 300, 299 };
static const unsigned int anim_roll[]        = { 148, 150, 152, 154, 156, 158, 160, 162, 164, 166, 168, 170, 172 };
static const unsigned int anim_jump_up[]     = { 95, 96, 97, 98, 99, 100, 101, 102, 103, 104 };
static const unsigned int anim_jump_forward[]= { 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209 };
static const unsigned int anim_fall[]        = { 239, 240, 241, 242 };
static const unsigned int anim_climb[]       = { 184, 185, 186, 187, 188, 189, 190 };
static const unsigned int anim_hurt[]        = { 257, 258, 259, 260, 261, 262, 263, 264 };
static const unsigned int anim_die[]         = { 257, 258, 260, 262, 264, 266, 268, 270, 272 };

void conrad_init(int start_x, int start_y) {
    g_conrad.x = start_x;
    g_conrad.y = start_y;
    g_conrad.vx = 0;
    g_conrad.vy = 0;
    g_conrad.state = CONRAD_STATE_IDLE;
    g_conrad.anim_frame = 0;
    g_conrad.anim_timer = 0;
    g_conrad.facing = 0; /* facing right */
    g_conrad.gun_drawn = 0;
    g_conrad.on_ground = 1;
    g_conrad.health = 4;
    g_conrad.has_holocube = 0;
    g_conrad.has_gun = 1;
    g_conrad.has_card = 0;
    g_conrad.score = 0;
    last_uploaded_frame = 0xFFFF;
    last_uploaded_facing = 0xFF;
}

void conrad_take_damage(unsigned char dmg) {
    if (g_conrad.state == CONRAD_STATE_DIE) return;

    if (g_conrad.health > dmg) {
        g_conrad.health -= dmg;
        g_conrad.state = CONRAD_STATE_HURT;
        g_conrad.anim_frame = 0;
        g_conrad.anim_timer = 0;
        g_conrad.vx = g_conrad.facing ? 2 : -2;
        audio_play_sfx(SFX_DAMAGE);
    } else {
        g_conrad.health = 0;
        g_conrad.state = CONRAD_STATE_DIE;
        g_conrad.anim_frame = 0;
        g_conrad.anim_timer = 0;
        g_conrad.vx = 0;
        audio_play_sfx(SFX_DAMAGE);
    }
}

void conrad_update(unsigned int keys, unsigned int pressed) {
    /* 1. Defeat state */
    if (g_conrad.state == CONRAD_STATE_DIE) {
        g_conrad.vx = 0;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 6) {
            g_conrad.anim_timer = 0;
            if (g_conrad.anim_frame < (sizeof(anim_die)/sizeof(anim_die[0])) - 1) {
                g_conrad.anim_frame++;
            }
        }
        return;
    }

    /* 2. Hurt recoil state */
    if (g_conrad.state == CONRAD_STATE_HURT) {
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 3) {
            g_conrad.anim_timer = 0;
            g_conrad.anim_frame++;
            if (g_conrad.anim_frame >= (sizeof(anim_hurt)/sizeof(anim_hurt[0]))) {
                g_conrad.state = CONRAD_STATE_IDLE;
                g_conrad.anim_frame = 0;
                g_conrad.vx = 0;
            }
        }
        g_conrad.x += g_conrad.vx;
        return;
    }

    /* 3. Main Moveset State Machine */
    switch (g_conrad.state) {
    case CONRAD_STATE_IDLE:
        g_conrad.vx = 0;

        /* Pickup floor item / Action button */
        if (pressed & PORT_A_KEY_1) {
            g_conrad.state = CONRAD_STATE_PICKUP;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            break;
        }

        /* Weapon draw / holster toggle */
        if (pressed & PORT_A_KEY_2) {
            if (g_conrad.gun_drawn) {
                g_conrad.state = CONRAD_STATE_HOLSTER_GUN;
            } else {
                g_conrad.state = CONRAD_STATE_DRAW_GUN;
            }
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            break;
        }

        /* Vertical high reach / jump */
        if (pressed & PORT_A_KEY_UP) {
            g_conrad.state = CONRAD_STATE_JUMP_UP;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            g_conrad.vy = -6;
            g_conrad.on_ground = 0;
            audio_play_sfx(SFX_ROLL_JUMP);
            break;
        }

        /* Crouch, Roll, or Item Pickup */
        if (keys & PORT_A_KEY_DOWN) {
            if (pressed & PORT_A_KEY_1) {
                g_conrad.state = CONRAD_STATE_PICKUP;
                g_conrad.anim_frame = 0;
                g_conrad.anim_timer = 0;
                break;
            }
            if ((keys & PORT_A_KEY_LEFT) || (keys & PORT_A_KEY_RIGHT)) {
                g_conrad.facing = (keys & PORT_A_KEY_LEFT) ? 1 : 0;
                g_conrad.state = CONRAD_STATE_ROLL;
                g_conrad.anim_frame = 0;
                g_conrad.anim_timer = 0;
                audio_play_sfx(SFX_ROLL_JUMP);
            } else {
                g_conrad.state = CONRAD_STATE_CROUCH;
                g_conrad.anim_frame = 0;
                g_conrad.anim_timer = 0;
            }
            break;
        }

        /* Walk / Run Left */
        if (keys & PORT_A_KEY_LEFT) {
            if (g_conrad.facing == 0) {
                g_conrad.state = CONRAD_STATE_TURN;
                g_conrad.anim_frame = 0;
                g_conrad.anim_timer = 0;
                g_conrad.facing = 1;
                break;
            }
            if (keys & PORT_A_KEY_1) {
                g_conrad.state = CONRAD_STATE_RUN;
            } else {
                g_conrad.state = CONRAD_STATE_WALK;
            }
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            break;
        }

        /* Walk / Run Right */
        if (keys & PORT_A_KEY_RIGHT) {
            if (g_conrad.facing == 1) {
                g_conrad.state = CONRAD_STATE_TURN;
                g_conrad.anim_frame = 0;
                g_conrad.anim_timer = 0;
                g_conrad.facing = 0;
                break;
            }
            if (keys & PORT_A_KEY_1) {
                g_conrad.state = CONRAD_STATE_RUN;
            } else {
                g_conrad.state = CONRAD_STATE_WALK;
            }
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            break;
        }
        break;

    case CONRAD_STATE_TURN:
        g_conrad.vx = 0;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 2) {
            g_conrad.anim_timer = 0;
            g_conrad.anim_frame++;
            if (g_conrad.anim_frame >= (sizeof(anim_turn)/sizeof(anim_turn[0]))) {
                if ((g_conrad.facing == 1 && (keys & PORT_A_KEY_LEFT)) ||
                    (g_conrad.facing == 0 && (keys & PORT_A_KEY_RIGHT))) {
                    g_conrad.state = (keys & PORT_A_KEY_1) ? CONRAD_STATE_RUN : CONRAD_STATE_WALK;
                } else {
                    g_conrad.state = CONRAD_STATE_IDLE;
                }
                g_conrad.anim_frame = 0;
            }
        }
        break;

    case CONRAD_STATE_WALK:
        g_conrad.vx = g_conrad.facing ? -2 : 2;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 3) {
            g_conrad.anim_timer = 0;
            g_conrad.anim_frame = (g_conrad.anim_frame + 1) % 12;
            if (g_conrad.anim_frame == 0 || g_conrad.anim_frame == 6) {
                audio_play_sfx(SFX_FOOTSTEP);
            }
        }
        if (keys & PORT_A_KEY_1) {
            g_conrad.state = CONRAD_STATE_RUN;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            break;
        }
        if (keys & PORT_A_KEY_DOWN) {
            g_conrad.state = CONRAD_STATE_ROLL;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            audio_play_sfx(SFX_ROLL_JUMP);
            break;
        }
        if (pressed & PORT_A_KEY_UP) {
            g_conrad.state = CONRAD_STATE_JUMP_UP;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            g_conrad.vy = -6;
            g_conrad.on_ground = 0;
            audio_play_sfx(SFX_ROLL_JUMP);
            break;
        }
        if (!(keys & (PORT_A_KEY_LEFT | PORT_A_KEY_RIGHT))) {
            g_conrad.state = CONRAD_STATE_IDLE;
            g_conrad.anim_frame = 0;
            g_conrad.vx = 0;
        }
        break;

    case CONRAD_STATE_RUN:
        g_conrad.vx = g_conrad.facing ? -4 : 4;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 2) {
            g_conrad.anim_timer = 0;
            g_conrad.anim_frame = (g_conrad.anim_frame + 1) % 12;
            if (g_conrad.anim_frame == 0 || g_conrad.anim_frame == 6) {
                audio_play_sfx(SFX_FOOTSTEP);
            }
        }
        if (pressed & PORT_A_KEY_UP) {
            g_conrad.state = CONRAD_STATE_JUMP_FORWARD;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            g_conrad.vy = -5;
            g_conrad.on_ground = 0;
            audio_play_sfx(SFX_ROLL_JUMP);
            break;
        }
        if (keys & PORT_A_KEY_DOWN) {
            g_conrad.state = CONRAD_STATE_ROLL;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            audio_play_sfx(SFX_ROLL_JUMP);
            break;
        }
        if (!(keys & (PORT_A_KEY_LEFT | PORT_A_KEY_RIGHT))) {
            g_conrad.state = CONRAD_STATE_RUN_STOP;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
        }
        break;

    case CONRAD_STATE_RUN_STOP:
        g_conrad.vx = g_conrad.facing ? -1 : 1;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 2) {
            g_conrad.anim_timer = 0;
            g_conrad.anim_frame++;
            if (g_conrad.anim_frame >= (sizeof(anim_run_stop)/sizeof(anim_run_stop[0]))) {
                g_conrad.state = CONRAD_STATE_IDLE;
                g_conrad.anim_frame = 0;
                g_conrad.vx = 0;
            }
        }
        break;

    case CONRAD_STATE_DRAW_GUN:
        g_conrad.vx = 0;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 2) {
            g_conrad.anim_timer = 0;
            g_conrad.anim_frame++;
            if (g_conrad.anim_frame >= (sizeof(anim_draw_gun)/sizeof(anim_draw_gun[0]))) {
                g_conrad.gun_drawn = 1;
                g_conrad.state = CONRAD_STATE_AIM_GUN;
                g_conrad.anim_frame = 0;
            }
        }
        break;

    case CONRAD_STATE_AIM_GUN:
        g_conrad.vx = 0;
        if (pressed & PORT_A_KEY_1) {
            g_conrad.state = CONRAD_STATE_SHOOT;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            audio_play_sfx(SFX_LASER_SHOT);
            actors_spawn_bullet(g_conrad.facing ? (g_conrad.x - 8) : (g_conrad.x + 16),
                               g_conrad.y + 12,
                               g_conrad.facing ? -6 : 6, 1);
            break;
        }
        if (pressed & PORT_A_KEY_2) {
            g_conrad.state = CONRAD_STATE_HOLSTER_GUN;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            break;
        }
        if (keys & (PORT_A_KEY_LEFT | PORT_A_KEY_RIGHT)) {
            g_conrad.facing = (keys & PORT_A_KEY_LEFT) ? 1 : 0;
        }
        break;

    case CONRAD_STATE_SHOOT:
        g_conrad.vx = 0;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 2) {
            g_conrad.anim_timer = 0;
            g_conrad.anim_frame++;
            if (g_conrad.anim_frame >= (sizeof(anim_shoot)/sizeof(anim_shoot[0]))) {
                g_conrad.state = CONRAD_STATE_AIM_GUN;
                g_conrad.anim_frame = 0;
            }
        }
        break;

    case CONRAD_STATE_HOLSTER_GUN:
        g_conrad.vx = 0;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 2) {
            g_conrad.anim_timer = 0;
            g_conrad.anim_frame++;
            if (g_conrad.anim_frame >= (sizeof(anim_holster_gun)/sizeof(anim_holster_gun[0]))) {
                g_conrad.gun_drawn = 0;
                g_conrad.state = CONRAD_STATE_IDLE;
                g_conrad.anim_frame = 0;
            }
        }
        break;

    case CONRAD_STATE_CROUCH:
        g_conrad.vx = 0;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 2) {
            g_conrad.anim_timer = 0;
            if (g_conrad.anim_frame < 7) {
                g_conrad.anim_frame++;
            }
        }
        if (pressed & PORT_A_KEY_1) {
            g_conrad.state = CONRAD_STATE_PICKUP;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            break;
        }
        if (pressed & (PORT_A_KEY_LEFT | PORT_A_KEY_RIGHT)) {
            g_conrad.facing = (keys & PORT_A_KEY_LEFT) ? 1 : 0;
            g_conrad.state = CONRAD_STATE_ROLL;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
            audio_play_sfx(SFX_ROLL_JUMP);
            break;
        }
        if (!(keys & PORT_A_KEY_DOWN)) {
            g_conrad.state = CONRAD_STATE_STAND_CROUCH;
            g_conrad.anim_frame = 0;
            g_conrad.anim_timer = 0;
        }
        break;

    case CONRAD_STATE_STAND_CROUCH:
        g_conrad.vx = 0;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 2) {
            g_conrad.anim_timer = 0;
            g_conrad.anim_frame++;
            if (g_conrad.anim_frame >= (sizeof(anim_stand_crouch)/sizeof(anim_stand_crouch[0]))) {
                g_conrad.state = CONRAD_STATE_IDLE;
                g_conrad.anim_frame = 0;
            }
        }
        break;

    case CONRAD_STATE_PICKUP:
        g_conrad.vx = 0;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 3) {
            g_conrad.anim_timer = 0;
            g_conrad.anim_frame++;
            if (g_conrad.anim_frame >= (sizeof(anim_pickup)/sizeof(anim_pickup[0]))) {
                g_conrad.state = (keys & PORT_A_KEY_DOWN) ? CONRAD_STATE_CROUCH : CONRAD_STATE_IDLE;
                g_conrad.anim_frame = 0;
            }
        }
        break;

    case CONRAD_STATE_ROLL:
        g_conrad.vx = g_conrad.facing ? -3 : 3;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 2) {
            g_conrad.anim_timer = 0;
            g_conrad.anim_frame++;
            if (g_conrad.anim_frame >= (sizeof(anim_roll)/sizeof(anim_roll[0]))) {
                g_conrad.state = (keys & PORT_A_KEY_DOWN) ? CONRAD_STATE_CROUCH : CONRAD_STATE_IDLE;
                g_conrad.anim_frame = 0;
            }
        }
        break;

    case CONRAD_STATE_JUMP_UP:
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 3) {
            g_conrad.anim_timer = 0;
            if (g_conrad.anim_frame < 9) {
                g_conrad.anim_frame++;
            }
        }
        g_conrad.vy += 1;
        if (g_conrad.vy > 6) g_conrad.vy = 6;
        break;

    case CONRAD_STATE_JUMP_FORWARD:
        g_conrad.vx = g_conrad.facing ? -3 : 3;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 3) {
            g_conrad.anim_timer = 0;
            if (g_conrad.anim_frame < 15) {
                g_conrad.anim_frame++;
            }
        }
        g_conrad.vy += 1;
        if (g_conrad.vy > 6) g_conrad.vy = 6;
        break;

    case CONRAD_STATE_FALL:
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 3) {
            g_conrad.anim_timer = 0;
            if (g_conrad.anim_frame < (sizeof(anim_fall)/sizeof(anim_fall[0])) - 1) {
                g_conrad.anim_frame++;
            }
        }
        g_conrad.vy += 1;
        if (g_conrad.vy > 6) g_conrad.vy = 6;
        break;

    case CONRAD_STATE_CLIMB:
        g_conrad.vx = 0;
        g_conrad.anim_timer++;
        if (g_conrad.anim_timer >= 3) {
            g_conrad.anim_timer = 0;
            g_conrad.anim_frame++;
            if (g_conrad.anim_frame >= (sizeof(anim_climb)/sizeof(anim_climb[0]))) {
                g_conrad.state = CONRAD_STATE_IDLE;
                g_conrad.anim_frame = 0;
            }
        }
        break;

    default:
        break;
    }

    /* Physics & Ground collision */
    g_conrad.x += g_conrad.vx;
    g_conrad.y += g_conrad.vy;

    if (g_conrad.y >= 112) {
        g_conrad.y = 112;
        g_conrad.vy = 0;
        g_conrad.on_ground = 1;
        if (g_conrad.state == CONRAD_STATE_JUMP_UP || 
            g_conrad.state == CONRAD_STATE_JUMP_FORWARD ||
            g_conrad.state == CONRAD_STATE_FALL) {
            g_conrad.state = CONRAD_STATE_IDLE;
            g_conrad.anim_frame = 0;
        }
    }
}

void conrad_load_frame_tiles(void) {
    unsigned char cur_state = g_conrad.state;
    unsigned int frame_id = 0;
    unsigned int bank_off;
    unsigned char bank_idx;
    const unsigned char *src;

    /* Map current state and anim_frame to authentic rotoscoped frame ID */
    switch (cur_state) {
    case CONRAD_STATE_IDLE:
        frame_id = anim_idle[0];
        break;
    case CONRAD_STATE_TURN:
        frame_id = anim_turn[g_conrad.anim_frame % (sizeof(anim_turn)/sizeof(anim_turn[0]))];
        break;
    case CONRAD_STATE_WALK:
        frame_id = anim_walk[g_conrad.anim_frame % (sizeof(anim_walk)/sizeof(anim_walk[0]))];
        break;
    case CONRAD_STATE_RUN:
        frame_id = anim_run[g_conrad.anim_frame % (sizeof(anim_run)/sizeof(anim_run[0]))];
        break;
    case CONRAD_STATE_RUN_STOP:
        frame_id = anim_run_stop[g_conrad.anim_frame % (sizeof(anim_run_stop)/sizeof(anim_run_stop[0]))];
        break;
    case CONRAD_STATE_DRAW_GUN:
        frame_id = anim_draw_gun[g_conrad.anim_frame % (sizeof(anim_draw_gun)/sizeof(anim_draw_gun[0]))];
        break;
    case CONRAD_STATE_AIM_GUN:
        frame_id = anim_aim_gun[0];
        break;
    case CONRAD_STATE_SHOOT:
        frame_id = anim_shoot[g_conrad.anim_frame % (sizeof(anim_shoot)/sizeof(anim_shoot[0]))];
        break;
    case CONRAD_STATE_HOLSTER_GUN:
        frame_id = anim_holster_gun[g_conrad.anim_frame % (sizeof(anim_holster_gun)/sizeof(anim_holster_gun[0]))];
        break;
    case CONRAD_STATE_CROUCH:
        frame_id = anim_crouch[g_conrad.anim_frame % (sizeof(anim_crouch)/sizeof(anim_crouch[0]))];
        break;
    case CONRAD_STATE_STAND_CROUCH:
        frame_id = anim_stand_crouch[g_conrad.anim_frame % (sizeof(anim_stand_crouch)/sizeof(anim_stand_crouch[0]))];
        break;
    case CONRAD_STATE_PICKUP:
        frame_id = anim_pickup[g_conrad.anim_frame % (sizeof(anim_pickup)/sizeof(anim_pickup[0]))];
        break;
    case CONRAD_STATE_ROLL:
        frame_id = anim_roll[g_conrad.anim_frame % (sizeof(anim_roll)/sizeof(anim_roll[0]))];
        break;
    case CONRAD_STATE_JUMP_UP:
        frame_id = anim_jump_up[g_conrad.anim_frame % (sizeof(anim_jump_up)/sizeof(anim_jump_up[0]))];
        break;
    case CONRAD_STATE_JUMP_FORWARD:
        frame_id = anim_jump_forward[g_conrad.anim_frame % (sizeof(anim_jump_forward)/sizeof(anim_jump_forward[0]))];
        break;
    case CONRAD_STATE_FALL:
        frame_id = anim_fall[g_conrad.anim_frame % (sizeof(anim_fall)/sizeof(anim_fall[0]))];
        break;
    case CONRAD_STATE_CLIMB:
        frame_id = anim_climb[g_conrad.anim_frame % (sizeof(anim_climb)/sizeof(anim_climb[0]))];
        break;
    case CONRAD_STATE_HURT:
        frame_id = anim_hurt[g_conrad.anim_frame % (sizeof(anim_hurt)/sizeof(anim_hurt[0]))];
        break;
    case CONRAD_STATE_DIE:
        frame_id = anim_die[g_conrad.anim_frame % (sizeof(anim_die)/sizeof(anim_die[0]))];
        break;
    default:
        frame_id = 0;
        break;
    }

    if (frame_id >= CONRAD_TOTAL_FRAMES) frame_id = 0;
    if (frame_id == last_uploaded_frame && g_conrad.facing == last_uploaded_facing) return;
    last_uploaded_frame = frame_id;
    last_uploaded_facing = g_conrad.facing;

    /* 51 frames per 16KB bank. Right facing = banks 57..63 (7 banks), Left facing = banks 64..70 (7 banks) */
    bank_idx = BANK_CONRAD_SPRITES0 + (g_conrad.facing ? 7 : 0) + (unsigned char)(frame_id / 51);
    bank_off = (frame_id % 51) * 320;

    SMS_mapROMBank(bank_idx);
    src = (const unsigned char *)(0x8000 + bank_off);
    
    SMS_setAddr(0x4000); /* Tile 0 in VRAM */
    fast_upload_320_bytes(src);
}

void conrad_draw(void) {
    /* 16x40 metasprite: 2 columns x 5 rows = 10 sprites (tiles 0..9) */
    SMS_addSprite(g_conrad.x,     g_conrad.y,      0);
    SMS_addSprite(g_conrad.x + 8, g_conrad.y,      1);
    SMS_addSprite(g_conrad.x,     g_conrad.y + 8,  2);
    SMS_addSprite(g_conrad.x + 8, g_conrad.y + 8,  3);
    SMS_addSprite(g_conrad.x,     g_conrad.y + 16, 4);
    SMS_addSprite(g_conrad.x + 8, g_conrad.y + 16, 5);
    SMS_addSprite(g_conrad.x,     g_conrad.y + 24, 6);
    SMS_addSprite(g_conrad.x + 8, g_conrad.y + 24, 7);
    SMS_addSprite(g_conrad.x,     g_conrad.y + 32, 8);
    SMS_addSprite(g_conrad.x + 8, g_conrad.y + 32, 9);
}
