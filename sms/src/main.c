/* Flashback: The Quest for Identity -- Sega Master System, merged 4 MB build.
 *
 *  * Level 1 (Titan Jungle) gameplay from the level-1 port: real DOS room data
 *    (backgrounds, collision grids, room links, Conrad sprite frames, item
 *    icons), full moveset, ledge grab, room transitions, item grabbing.
 *  * FMV cutscene engine from the video port, re-encoded so that all seven
 *    cutscenes share ONE dictionary of tiles (tools/encode_shared_dictionary.py).
 *  * Picking an item up plays the cutscene that belongs to it: the holocube
 *    plays the Holocube Message, every other object plays the object pickup
 *    scene. Walking into the last room of the demo plays the disintegration
 *    scene and returns to the title screen.
 *
 * Bank map (see game_banks.h): 0-1 code | 2-43 level 1 data (C sources)
 * | 44-95 FMV stream (shared dictionary) | 96 title | 97 instructions.
 */
#include <stdint.h>
#include <string.h>
#include <SMSlib.h>
#include "rooms.h"
#include "frames.h"
#include "items.h"
#include "cutscenes_data.h"
#include "video_player.h"
#include "title_menu.h"
#include "instructions.h"
#include "audio.h"
#include "game_banks.h"

SMS_EMBED_SEGA_ROM_HEADER(9999, 0);
SMS_EMBED_SDSC_HEADER_AUTO_DATE(1, 0, "port", "Flashback SMS",
                                "Flashback DOS demo: level 1 + FMV cutscenes");

#define ROOM_W 32
#define CT_W 16
#define CON_W 16
#define CON_H 40
#define WORLD_H 216
#define SPRITE_TILE_BASE 400
#define ICON_TILE_BASE 444
#define SPAWN_ROOM 27
#define DEMO_LAST_ROOM 63

/* ---------------------------------------------------------------- modes --- */
enum {
    MODE_LOGOS = 1,
    MODE_INTRO = 2,
    MODE_TITLE = 3,
    MODE_GAMEPLAY = 4,
    MODE_CUTSCENE = 5,
    MODE_INSTRUCTIONS = 6,
    MODE_GAMEOVER = 7
};

/* cutscene indices (order of the table below) */
enum {
    SC_LOGO = 0, SC_INTRO1, SC_INTRO2, SC_HOLOCUBE, SC_DEBUT, SC_OBJET, SC_DESINTEG
};

static const cutscene_info_t * const scenes[] = {
    &cutscene_logos, &cutscene_intro1, &cutscene_intro2, &cutscene_holocube,
    &cutscene_debut, &cutscene_objet, &cutscene_desinteg
};

/* ------------------------------------------------ state visible to tests --- */
volatile uint8_t cur_mode = MODE_LOGOS;
volatile uint8_t cur_scene = 0;      /* 1-based cutscene number while playing   */
volatile uint8_t demo_done = 0;      /* 1 once the demo end was reached         */
volatile uint8_t dbg_state = 0;      /* 0 idle 1 walk 2 run 3 jump 4 crouch
                                        5 shoot 6 crouchshoot 7 hang 8 climb    */
volatile uint8_t cur_room = SPAWN_ROOM;   /* current level-1 room          */
volatile uint8_t dbg_room = SPAWN_ROOM;   /* same value, historic name      */
volatile uint8_t dbg_inv = 0;        /* items picked up                         */
volatile int16_t dbg_px = 0, dbg_py = 0;
volatile uint8_t dbg_w = 0, dbg_h = 0, dbg_t = 0, dbg_flip = 0;
volatile uint8_t last_pickup_video = 0;   /* 1-based cutscene the last pickup played */
volatile uint8_t warp_room = 0;      /* test hook: warp to this room next frame   */

/* ------------------------------------------------------------ level 1 ------ */
static uint8_t curRoomIdx = 0;
static int8_t lkU = -1, lkD = -1, lkR = -1, lkL = -1;
static int8_t scrollY = 0;
static uint8_t ctGrid[112], gridL[112], gridR[112];

/* player (PGE init #0: room 27) */
int16_t px = 32, py = 32;                  /* player feet position (testable) */
static int8_t vx = 0, vy = 0;
static uint8_t onGround = 0;
static uint8_t facing = 1;
static uint8_t lastFlip = 0;
static uint8_t state = 0;
static uint8_t animTick = 0, animIdx = 0;
static uint8_t curFrame = 255;
static uint8_t hanging = 0, hangDir = 1, climbing = 0, climbTick = 0;
static int16_t hangTop = 0;

/* current room item */
uint8_t itemIdx = 255;                    /* current room item (testable)  */
int16_t itemX, itemY;
static uint16_t mapSave[4];
static uint8_t itemTX, itemTY;
static uint32_t grabbedMask = 0;

/* a cutscene requested by the game loop (0 = none) */
static uint8_t pending_scene = 0;
static uint8_t pending_end = 0;

static const uint8_t animIdle[]   = { 3 };
static const uint8_t animWalk[]   = { 11, 12, 13, 14, 15, 16 };
static const uint8_t animRun[]    = { 74, 76, 78, 80, 82 };
static const uint8_t animJump[]   = { 241 };
static const uint8_t animCrouch[] = { 295, 297 };
static const uint8_t animShoot[]  = { 312 };
static const uint8_t animCShoot[] = { 306 };
static const uint8_t animHang[]   = { 331, 334 };
static const uint8_t animClimb[]  = { 341, 343, 345 };

static int8_t findRoom (uint8_t room) {
    uint8_t i;
    for (i = 0; i < NUM_ROOMS; ++i)
        if (roomTable[i].room == room) return (int8_t)i;
    return -1;
}

static void loadGrid (uint8_t ridx, uint8_t *dst) {
    const uint8_t *base;
    uint16_t nt = roomTable[ridx].ntiles;
    SMS_mapROMBank (roomTable[ridx].bank);
    base = (const uint8_t *)0x8000;
    memcpy (dst, base + 2 + (uint32_t)nt * 32 + 896 * 2, 112);
}

/* Show the first not-yet-grabbed item of the room and remember how to hide it
 * again once it has been picked up. */
static void setupItem (void) {
    uint8_t i;
    itemIdx = 255;
    for (i = 0; i < NUM_ITEMS; ++i) {
        if ((itemTable[i].room == cur_room) &&
            !(grabbedMask & ((uint32_t)1 << i))) { itemIdx = i; break; }
    }
    if (itemIdx == 255) return;
    itemX = itemTable[itemIdx].x;
    itemY = itemTable[itemIdx].y;
    itemTX = (uint8_t)(itemX >> 3);
    itemTY = (uint8_t)((itemY - 16) >> 3);
    {
        const uint8_t *base;
        uint16_t nt = roomTable[curRoomIdx].ntiles;
        uint32_t mo;
        SMS_mapROMBank (roomTable[curRoomIdx].bank);
        base = (const uint8_t *)0x8000;
        mo = 2 + (uint32_t)nt * 32 + (uint32_t)(itemTY * 32 + itemTX) * 2;
        mapSave[0] = base[mo] | (base[mo + 1] << 8);
        mapSave[1] = base[mo + 2] | (base[mo + 3] << 8);
        mo += 64;
        mapSave[2] = base[mo] | (base[mo + 1] << 8);
        mapSave[3] = base[mo + 2] | (base[mo + 3] << 8);
        SMS_mapROMBank (BANK_ITEM_ICONS);
        SMS_loadTiles ((const uint8_t *)0x8000 +
                       (uint16_t)itemTable[itemIdx].slot * 128, ICON_TILE_BASE, 128);
        {
            /* object icons use the sprite palette (the DOS icons were drawn
             * for the character/object palette, not the jungle background) */
            static const uint16_t iconmap[4] = {
                ICON_TILE_BASE     | TILE_USE_SPRITE_PALETTE,
                (ICON_TILE_BASE+1) | TILE_USE_SPRITE_PALETTE,
                (ICON_TILE_BASE+2) | TILE_USE_SPRITE_PALETTE,
                (ICON_TILE_BASE+3) | TILE_USE_SPRITE_PALETTE };
            SMS_loadTileMapArea (itemTX, itemTY, iconmap, 2, 2);
        }
    }
}

static void loadRoom (uint8_t room) {
    int8_t idx = findRoom (room);
    const uint8_t *base;
    uint16_t nt;
    if (idx < 0) return;
    curRoomIdx = (uint8_t)idx;
    cur_room = room;
    dbg_room = room;
    SMS_displayOff ();
    SMS_mapROMBank (roomTable[idx].bank);
    base = (const uint8_t *)0x8000;
    nt = base[0] | (base[1] << 8);
    SMS_loadBGPalette (roomPalette[idx]);
    SMS_setBackdropColor (0);
    SMS_loadTiles (base + 2, 0, (uint16_t)(nt * 32));
    SMS_loadTileMap (0, 0, base + 2 + (uint32_t)nt * 32, 896 * 2);
    memcpy (ctGrid, base + 2 + (uint32_t)nt * 32 + 896 * 2, 112);
    {
        const uint8_t *lk = base + 2 + (uint32_t)nt * 32 + 896 * 2 + 112;
        lkU = (int8_t)lk[0]; lkD = (int8_t)lk[1];
        lkR = (int8_t)lk[2]; lkL = (int8_t)lk[3];
    }
    {
        int8_t li = (lkL >= 0) ? findRoom ((uint8_t)lkL) : -1;
        int8_t ri = (lkR >= 0) ? findRoom ((uint8_t)lkR) : -1;
        if (li >= 0) loadGrid ((uint8_t)li, gridL); else memset (gridL, 0, 112);
        if (ri >= 0) loadGrid ((uint8_t)ri, gridR); else memset (gridR, 0, 112);
    }
    scrollY = 0;
    SMS_setBGScrollX (0);
    SMS_setBGScrollY (0);
    setupItem ();
    SMS_displayOn ();
}

/* collision: sample the grid cell at pixel (x,y); cell = 16x36, rows 1..6 */
static uint8_t solidAt (int16_t x, int16_t y) {
    const uint8_t *g = ctGrid;
    int8_t gy;
    if (x < 0) {
        if (lkL < 0) return 1;
        g = gridL; x += 256;
    } else if (x >= 256) {
        if (lkR < 0) return 1;
        g = gridR; x -= 256;
    }
    if (y < 0) {
        if (lkU < 0) return 1;
        y += WORLD_H;
    } else if (y >= WORLD_H) {
        if (lkD < 0) return 1;
        y -= WORLD_H;
    }
    gy = (int8_t)(y / 36);
    if (gy < 1) return 0;
    if (gy > 6) return (lkD >= 0) ? 0 : 1;
    return g[gy * CT_W + (x >> 4)] != 0;
}

static uint8_t curW = 0, curH = 0, curT = 0;
static int8_t curDx = 0, curDy = 0;

static void uploadFrame (uint8_t fid, uint8_t flip) {
    const FrameRec *fr = &frameTable[fid];
    const uint8_t *src;
    uint8_t w, h, nt;
    if (fr->off == 65535) return;
    SMS_mapROMBank (fr->bank);
    src = (const uint8_t *)0x8000 + fr->off;
    w = src[0]; h = src[1]; nt = src[2];
    curDx = (int8_t)src[3]; curDy = (int8_t)src[4];
    src += 5;
    if (flip) src += (uint16_t)nt * 32;
    SMS_loadTiles (src, SPRITE_TILE_BASE, (uint16_t)nt * 32);
    curFrame = fid; curW = w; curH = h; curT = nt; lastFlip = flip;
    dbg_w = w; dbg_h = h; dbg_t = nt; dbg_flip = flip;
}

static void drawConrad (void) {
    uint8_t tw = (curW + 7) >> 3, i, x, y;
    int16_t sx = px + (CON_W - (int16_t)curW) / 2;
    int16_t sy = py + CON_H - curH;
    SMS_initSprites ();
    for (i = 0; i < curT; ++i) {
        x = (uint8_t)(i % tw) << 3;
        y = (uint8_t)(i / tw) << 3;
        SMS_addSpriteClipping (sx + x, sy + y, SPRITE_TILE_BASE + i);
    }
    SMS_finalizeSprites ();
}

/* ------------------------------------------------------- game transitions -- */

/* Start / restart the jungle level. */
static void game_start (void) {
    SMS_useFirstHalfTilesforSprites (0);
    curFrame = 255;
    lastFlip = 255;
    hanging = 0; climbing = 0; onGround = 0; vy = 0; vx = 0;
    state = 0; animIdx = 0; animTick = 0;
    pending_scene = 0;
    loadRoom (SPAWN_ROOM);
}

/* Coming back from a cutscene: the video player owns VRAM while it runs, so
 * rebuild the room (tiles, tilemap, palette, collision grids, item icon) and
 * force the player sprite frame to be re-uploaded. */
static void game_resume (void) {
    SMS_useFirstHalfTilesforSprites (0);
    curFrame = 255;
    lastFlip = 255;
    state = 0;
    loadRoom (cur_room);
    SMS_initSprites ();
    SMS_finalizeSprites ();
}

/* -------------------------------------------------- one frame of gameplay -- */
/* Returns 1 when a cutscene has been requested (pending_scene != 0). */
static uint8_t game_frame (void) {
    uint8_t keys;

    /* test hook: the playtest harness can jump straight to a room */
    if (warp_room) { loadRoom (warp_room); warp_room = 0; }

    SMS_waitForVBlank ();
    SMS_copySpritestoSAT ();
    keys = SMS_getKeysStatus ();
    audio_update ();

    /* input -> intent */
    vx = 0;
    if (keys & PORT_A_KEY_LEFT)  { vx = -1; facing = 0; }
    if (keys & PORT_A_KEY_RIGHT) { vx =  1; facing = 1; }

    if (hanging) {
        vx = 0;
        if (keys & (PORT_A_KEY_UP | PORT_A_KEY_1)) {
            hanging = 0; climbing = 1; climbTick = 0;
            py = hangTop - CON_H;
            px = hangDir ? (int16_t)(px + 8) : (int16_t)(px - 8);
            onGround = 1; vy = 0;
            state = 8;
        } else if (keys & PORT_A_KEY_DOWN) {
            hanging = 0; onGround = 0; vy = 0; state = 3;
        } else {
            state = 7;
        }
    } else if (climbing) {
        vx = 0; state = 8;
        if (++climbTick >= 12) { climbing = 0; state = 0; }
    } else if (!onGround) {
        state = 3;
        vy += 1; if (vy > 3) vy = 3;
    } else if ((keys & PORT_A_KEY_DOWN) && (keys & PORT_A_KEY_2)) {
        state = 6; vx = 0;
    } else if (keys & PORT_A_KEY_DOWN) {
        state = 4; vx = 0;
    } else if ((keys & PORT_A_KEY_2) && vx != 0) {
        state = 2;
    } else if (keys & PORT_A_KEY_2) {
        state = 5; vx = 0;
    } else if (vx != 0) {
        state = 1;
    } else {
        state = 0;
    }
    dbg_state = state;

    /* horizontal move with wall collision + room transitions */
    if (!hanging && !climbing) {
        int16_t nx = px + vx * ((keys & PORT_A_KEY_2) ? 2 : 1);
        if (!onGround && vx != 0) {
            int16_t fx = (vx > 0) ? (int16_t)(nx + CON_W) : nx;
            int16_t hy = py + 8;
            int16_t gy = hy / 36;
            if (gy >= 1 && gy <= 6 &&
                solidAt (fx, hy) && !solidAt (fx, (int16_t)(hy - 36))) {
                hanging = 1; hangDir = (vx > 0);
                hangTop = (int16_t)gy * 36;
                {
                    int16_t cx = (int16_t)(fx >> 4) * 16;
                    px = hangDir ? (int16_t)(cx - CON_W) : (int16_t)(cx + 16);
                }
                uploadFrame (331, facing);
                py = (int16_t)(hangTop - CON_H + curH);
                vx = 0; nx = px;
            }
        }
        if (vx > 0 && (solidAt (nx + CON_W, py + CON_H - 4) ||
                       solidAt (nx + CON_W, py + 10))) nx = px;
        if (vx < 0 && (solidAt (nx, py + CON_H - 4) ||
                       solidAt (nx, py + 10))) nx = px;
        if (nx > 252 && lkR >= 0) {
            loadRoom ((uint8_t)lkR); nx -= 256;
        } else if (nx < -4 && lkL >= 0) {
            loadRoom ((uint8_t)lkL); nx += 256;
        } else {
            if (nx < 0 && lkL < 0) nx = 0;
            if (nx > 255 - CON_W && lkR < 0) nx = 255 - CON_W;
        }
        px = nx;

        /* vertical */
        py += onGround ? 0 : vy;
        if (!onGround) {
            if (vy > 0 && (solidAt (px + 2, py + CON_H) ||
                           solidAt (px + CON_W - 2, py + CON_H))) {
                onGround = 1; vy = 0;
                if (py + CON_H > WORLD_H) py = WORLD_H - CON_H;
            } else if (py + CON_H >= WORLD_H + 36 && lkD >= 0) {
                loadRoom ((uint8_t)lkD); py -= WORLD_H;
            }
        } else {
            if (keys & PORT_A_KEY_1) { onGround = 0; vy = -10; audio_play_sfx (SFX_ROLL_JUMP); }
            else if (!solidAt (px + 2, py + CON_H) &&
                     !solidAt (px + CON_W - 2, py + CON_H)) { onGround = 0; vy = 0; }
        }
        if (py < 0) {
            if (lkU >= 0) { loadRoom ((uint8_t)lkU); py += WORLD_H; }
            else py = 0;
        }
        if (lkD < 0 && py > WORLD_H - CON_H) { py = WORLD_H - CON_H; onGround = 1; vy = 0; }
    }
    dbg_px = px; dbg_py = py;

    /* camera */
    {
        int8_t target = (int8_t)(py + CON_H / 2 - 96);
        if (target < 0) target = 0;
        if (target > 24) target = 24;
        if (target != scrollY) {
            scrollY += (target > scrollY) ? 1 : -1;
            SMS_setBGScrollY (scrollY);
        }
    }

    /* item grab: button 2 while standing over the icon */
    if (itemIdx != 255 && (keys & PORT_A_KEY_2) &&
        px + CON_W > itemX && px < itemX + 16 &&
        py + CON_H > itemY - 16 && py < itemY) {
        uint8_t grabbed = itemIdx;
        grabbedMask |= (uint32_t)1 << grabbed;
        ++dbg_inv;
        SMS_loadTileMapArea (itemTX, itemTY, mapSave, 2, 2);
        itemIdx = 255;
        audio_play_sfx (SFX_PICKUP);
        /* the cutscene that belongs to this object */
        pending_scene = (itemTable[grabbed].icon == 12) ? SC_HOLOCUBE : SC_OBJET;
        last_pickup_video = (uint8_t)(pending_scene + 1);
        return 1;
    }

    /* end of the demo: the last room of the level -------------------------- */
    if (cur_room == DEMO_LAST_ROOM && !demo_done) {
        demo_done = 1;
        pending_scene = SC_DESINTEG;
        pending_end = 1;
        return 1;
    }

    /* animation frame */
    {
        const uint8_t *seq; uint8_t len;
        uint8_t flip = facing;
        switch (state) {
        case 1: seq = animWalk; len = 6; break;
        case 2: seq = animRun; len = 5; break;
        case 3: seq = animJump; len = 1; break;
        case 4: seq = animCrouch; len = 2; break;
        case 5: seq = animShoot; len = 1; break;
        case 6: seq = animCShoot; len = 1; break;
        case 7: seq = animHang; len = 2; break;
        case 8: seq = animClimb; len = 3; break;
        default: seq = animIdle; len = 1; break;
        }
        if (++animTick >= (state == 2 ? 2 : 4)) {
            animTick = 0;
            animIdx = (uint8_t)((animIdx + 1) % len);
        }
        if (seq[animIdx] != curFrame || flip != lastFlip)
            uploadFrame (seq[animIdx], flip);
    }
    drawConrad ();
    return 0;
}

/* ------------------------------------------------------------------ main --- */
static const uint8_t sprite_pal[16] = {           /* quantised _conradPal1 */
    0, 42, 53, 37, 37, 32, 16, 6, 5, 1, 2, 23, 21, 42, 21, 63
};

void main (void) {
    unsigned char opt;

    SMS_init ();
    SMS_displayOff ();
    SMS_setSpriteMode (SPRITEMODE_NORMAL);
    SMS_useFirstHalfTilesforSprites (0);
    SMS_loadSpritePalette (sprite_pal);
    audio_init ();

    /* 1. Delphine Software logo (FMV) */
    cur_mode = MODE_LOGOS;
    cur_scene = SC_LOGO + 1;
    video_player_play (&cutscene_logos);
    cur_scene = 0;

    /* 2. Title screen */
    title_menu_init ();
    cur_mode = MODE_TITLE;

    for (;;) {
        switch (cur_mode) {
        case MODE_TITLE:
            SMS_waitForVBlank ();
            SMS_copySpritestoSAT ();
            audio_update ();
            opt = title_menu_update ();
            if (opt == 1) {                       /* START GAME */
                cur_mode = MODE_CUTSCENE;
                cur_scene = SC_DEBUT + 1;
                video_player_play (&cutscene_debut);
                cur_scene = 0;
                game_start ();
                cur_mode = MODE_GAMEPLAY;
            } else if (opt == 2) {                /* CINEMATIC INTRO */
                cur_mode = MODE_INTRO;
                cur_scene = SC_INTRO1 + 1;
                video_player_play (&cutscene_intro1);
                cur_scene = SC_INTRO2 + 1;
                video_player_play (&cutscene_intro2);
                cur_scene = 0;
                title_menu_init ();
                cur_mode = MODE_TITLE;
            } else if (opt == 3) {                /* HOLOCUBE LOG */
                cur_mode = MODE_CUTSCENE;
                cur_scene = SC_HOLOCUBE + 1;
                video_player_play (&cutscene_holocube);
                cur_scene = 0;
                title_menu_init ();
                cur_mode = MODE_TITLE;
            } else if (opt == 4) {                /* INSTRUCTIONS */
                cur_mode = MODE_INSTRUCTIONS;
                instructions_init ();
            }
            break;

        case MODE_INSTRUCTIONS:
            SMS_waitForVBlank ();
            SMS_copySpritestoSAT ();
            audio_update ();
            if (instructions_update ()) {
                title_menu_init ();
                cur_mode = MODE_TITLE;
            }
            break;

        case MODE_INTRO:
            break;

        case MODE_GAMEPLAY:
            if (game_frame ()) {
                cur_mode = MODE_CUTSCENE;
                cur_scene = (uint8_t)(pending_scene + 1);
                video_player_play (scenes[pending_scene]);
                cur_scene = 0;
                pending_scene = 0;
                if (pending_end) {                /* demo finished */
                    pending_end = 0;
                    title_menu_init ();
                    cur_mode = MODE_TITLE;
                } else {
                    game_resume ();
                    cur_mode = MODE_GAMEPLAY;
                }
            }
            break;

        default:
            break;
        }
    }
}
