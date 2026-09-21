/* Flashback -> Sega Master System (devkitSMS). Level 1 (DOS demo data).
 * Backgrounds: real DOS data -> VQ tiles. Collision: real CT grid + room links.
 * Player: real ANI sprite frames, full room navigation, PGE item grabbing.
 */
#include <stdint.h>
#include <string.h>
#include <SMSlib.h>
#include "rooms.h"
#include "frames.h"
#include "items.h"

SMS_EMBED_SEGA_ROM_HEADER(9999, 0);
SMS_EMBED_SDSC_HEADER_AUTO_DATE(1, 0, "port", "Flashback SMS", "Flashback DOS ported to SMS");

#define ROOM_W 32
#define CT_W 16
#define CON_W 16
#define CON_H 40
#define WORLD_H 216
#define SPRITE_TILE_BASE 400
#define ICON_TILE_BASE 444

/* exported for playtest inspection */
volatile uint8_t dbg_state = 0;   /* 0 idle 1 walk 2 run 3 jump 4 crouch 5 shoot 6 crouchshoot */
volatile uint8_t dbg_room = 27;
volatile uint8_t dbg_inv = 0;
volatile int16_t dbg_px = 0, dbg_py = 0;
volatile uint8_t dbg_w = 0, dbg_h = 0, dbg_t = 0, dbg_flip = 0;

static uint8_t curRoomIdx = 0;
static uint8_t curRoom = 27;
static int8_t lkU = -1, lkD = -1, lkR = -1, lkL = -1;
static int8_t scrollY = 0;
static uint8_t ctGrid[112], gridL[112], gridR[112];

/* player (PGE init #0: room 27 at (32,70) feet coords) */
static int16_t px = 32, py = 32;
static int8_t vx = 0, vy = 0;
static uint8_t onGround = 0;
static uint8_t facing = 1;      /* 1 right, 0 left */
static uint8_t lastFlip = 0;
static uint8_t state = 0;
static uint8_t animTick = 0, animIdx = 0;
static uint8_t curFrame = 255;
static uint8_t hanging = 0, hangDir = 1, climbing = 0, climbTick = 0;
static int16_t hangTop = 0;

/* current room item (first PGE icon entry) */
static uint8_t itemIdx = 255;   /* index into itemTable */
static int16_t itemX, itemY;
static uint16_t mapSave[4];
static uint8_t itemTX, itemTY;
static uint32_t grabbedMask = 0;

static const uint8_t animIdle[]  = { 3 };
static const uint8_t animWalk[]  = { 11, 12, 13, 14, 15, 16 };
static const uint8_t animRun[]   = { 74, 76, 78, 80, 82 };
static const uint8_t animJump[]  = { 241 };
static const uint8_t animCrouch[]= { 295, 297 };
static const uint8_t animShoot[] = { 312 };
static const uint8_t animCShoot[]= { 306 };
static const uint8_t animHang[]  = { 331, 334 };
static const uint8_t animClimb[] = { 341, 343, 345 };

static int8_t findRoom(uint8_t room) {
    uint8_t i;
    for (i = 0; i < NUM_ROOMS; ++i)
        if (roomTable[i].room == room) return (int8_t)i;
    return -1;
}

static void loadGrid(uint8_t ridx, uint8_t *dst) {
    const uint8_t *base;
    uint16_t nt = roomTable[ridx].ntiles;
    SMS_mapROMBank(roomTable[ridx].bank);
    base = (const uint8_t *)0x8000;
    memcpy(dst, base + 2 + (uint32_t)nt * 32 + 896 * 2, 112);
}

static void setupItem(void) {
    uint8_t i;
    itemIdx = 255;
    for (i = 0; i < NUM_ITEMS; ++i) {
        if (itemTable[i].room == curRoom && !(grabbedMask & ((uint32_t)1 << i))) { itemIdx = i; break; }
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
        SMS_mapROMBank(roomTable[curRoomIdx].bank);
        base = (const uint8_t *)0x8000;
        mo = 2 + (uint32_t)nt * 32 + (uint32_t)(itemTY * 32 + itemTX) * 2;
        mapSave[0] = base[mo] | (base[mo + 1] << 8);
        mapSave[1] = base[mo + 2] | (base[mo + 3] << 8);
        mo += 64;
        mapSave[2] = base[mo] | (base[mo + 1] << 8);
        mapSave[3] = base[mo + 2] | (base[mo + 3] << 8);
        SMS_mapROMBank(43);
        SMS_loadTiles((const uint8_t *)0x8000 + (uint16_t)itemTable[itemIdx].slot * 128, ICON_TILE_BASE, 128);
        {
            static const uint16_t iconmap[4] = { ICON_TILE_BASE, ICON_TILE_BASE + 1,
                                                 ICON_TILE_BASE + 2, ICON_TILE_BASE + 3 };
            SMS_loadTileMapArea(itemTX, itemTY, iconmap, 2, 2);
        }
    }
}

static void loadRoom(uint8_t room) {
    int8_t idx = findRoom(room);
    const uint8_t *base;
    uint16_t nt;
    if (idx < 0) return;
    curRoomIdx = (uint8_t)idx;
    curRoom = room;
    dbg_room = room;
    SMS_displayOff();
    SMS_mapROMBank(roomTable[idx].bank);
    base = (const uint8_t *)0x8000;
    nt = base[0] | (base[1] << 8);
    SMS_loadBGPalette(roomPalette[idx]);
    SMS_setBackdropColor(0);
    SMS_loadTiles(base + 2, 0, (uint16_t)(nt * 32));
    SMS_loadTileMap(0, 0, base + 2 + (uint32_t)nt * 32, 896 * 2);
    memcpy(ctGrid, base + 2 + (uint32_t)nt * 32 + 896 * 2, 112);
    {
        const uint8_t *lk = base + 2 + (uint32_t)nt * 32 + 896 * 2 + 112;
        lkU = (int8_t)lk[0]; lkD = (int8_t)lk[1]; lkR = (int8_t)lk[2]; lkL = (int8_t)lk[3];
    }
    {
        int8_t li = (lkL >= 0) ? findRoom((uint8_t)lkL) : -1;
        int8_t ri = (lkR >= 0) ? findRoom((uint8_t)lkR) : -1;
        if (li >= 0) loadGrid((uint8_t)li, gridL); else memset(gridL, 0, 112);
        if (ri >= 0) loadGrid((uint8_t)ri, gridR); else memset(gridR, 0, 112);
    }
    scrollY = 0;
    SMS_setBGScrollX(0);
    SMS_setBGScrollY(0);
    setupItem();
    SMS_displayOn();
}

/* collision: sample grid cell at pixel (x,y); cell = 16x36, rows 1..6 valid */
static uint8_t solidAt(int16_t x, int16_t y) {
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
    if (gy < 1) return 0;                 /* ceiling handled by up-room transition */
    if (gy > 6) return (lkD >= 0) ? 0 : 1;
    return g[gy * CT_W + (x >> 4)] != 0;
}

static uint8_t curW = 0, curH = 0, curT = 0;
static int8_t curDx = 0, curDy = 0;
static void uploadFrame(uint8_t fid, uint8_t flip) {
    const FrameRec *fr = &frameTable[fid];
    const uint8_t *src;
    uint8_t w, h, nt;
    if (fr->off == 65535) return;
    SMS_mapROMBank(fr->bank);
    src = (const uint8_t *)0x8000 + fr->off;
    w = src[0]; h = src[1]; nt = src[2];
    curDx = (int8_t)src[3]; curDy = (int8_t)src[4];
    src += 5;
    if (flip) src += (uint16_t)nt * 32;
    SMS_loadTiles(src, SPRITE_TILE_BASE, (uint16_t)nt * 32);
    curFrame = fid; curW = w; curH = h; curT = nt; lastFlip = flip;
    dbg_w = w; dbg_h = h; dbg_t = nt; dbg_flip = flip;
}

static void drawConrad(void) {
    uint8_t tw = (curW + 7) >> 3, i, x, y;
    /* anchor: body centered on the 16px collision box, feet at its bottom */
    int16_t sx = px + (CON_W - (int16_t)curW) / 2;
    int16_t sy = py + CON_H - curH;
    SMS_initSprites();
    for (i = 0; i < curT; ++i) {
        x = (uint8_t)(i % tw) << 3;
        y = (uint8_t)(i / tw) << 3;
        SMS_addSpriteClipping(sx + x, sy + y, SPRITE_TILE_BASE + i);
    }
    SMS_finalizeSprites();
}

int main(void) {
    uint8_t keys;
    SMS_init();
    SMS_displayOff();
    SMS_setSpriteMode(SPRITEMODE_NORMAL);
    SMS_useFirstHalfTilesforSprites(0);
    /* sprite palette = real _conradPal1 (REminiscence staticres.cpp) quantized to SMS 2-bit/ch */
    {
        static const uint8_t spal[16] = { 0, 42, 53, 37, 37, 32, 16, 6, 5, 1, 2, 23, 21, 42, 21, 63 };
        SMS_loadSpritePalette(spal);
    }
    loadRoom(27);
    curFrame = 255; lastFlip = (uint8_t)(facing ^ 1);
    SMS_displayOn();
    for (;;) {
        SMS_waitForVBlank();
        SMS_copySpritestoSAT();
        keys = SMS_getKeysStatus();

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
            state = 2;                               /* run */
        } else if (keys & PORT_A_KEY_2) {
            state = 5; vx = 0;                       /* shoot */
        } else if (vx != 0) {
            state = 1;
        } else {
            state = 0;
        }
        dbg_state = state;

        /* horizontal move with wall collision + room transitions */
        if (!hanging && !climbing) {
            int16_t nx = px + vx * ((keys & PORT_A_KEY_2) ? 2 : 1);
            /* ledge grab: airborne, hand hits an edge open above and at feet */
            if (!onGround && vx != 0) {
                int16_t fx = (vx > 0) ? (int16_t)(nx + CON_W) : nx;
                int16_t hy = py + 8;
                int16_t gy = hy / 36;
                if (gy >= 1 && gy <= 6 &&
                    solidAt(fx, hy) && !solidAt(fx, (int16_t)(hy - 36))) {
                    hanging = 1; hangDir = (vx > 0);
                    hangTop = (int16_t)gy * 36;
                    {
                        int16_t cx = (int16_t)(fx >> 4) * 16;
                        px = hangDir ? (int16_t)(cx - CON_W) : (int16_t)(cx + 16);
                    }
                    uploadFrame(331, facing);
                    py = (int16_t)(hangTop - CON_H + curH);
                    vx = 0; nx = px;
                }
            }
            if (vx > 0 && (solidAt(nx + CON_W, py + CON_H - 4) || solidAt(nx + CON_W, py + 10))) nx = px;
            if (vx < 0 && (solidAt(nx, py + CON_H - 4) || solidAt(nx, py + 10))) nx = px;
            if (nx > 252 && lkR >= 0) {
                loadRoom((uint8_t)lkR); nx -= 256;
            } else if (nx < -4 && lkL >= 0) {
                loadRoom((uint8_t)lkL); nx += 256;
            } else {
                if (nx < 0 && lkL < 0) nx = 0;
                if (nx > 255 - CON_W && lkR < 0) nx = 255 - CON_W;
            }
            px = nx;
        /* vertical */
        py += onGround ? 0 : vy;
        if (!onGround) {
            if (vy > 0 && (solidAt(px + 2, py + CON_H) || solidAt(px + CON_W - 2, py + CON_H))) {
                onGround = 1; vy = 0;
                if (py + CON_H > WORLD_H) py = WORLD_H - CON_H;
            } else if (py + CON_H >= WORLD_H + 36 && lkD >= 0) {
                loadRoom((uint8_t)lkD); py -= WORLD_H;
            }
        } else {
            if (keys & PORT_A_KEY_1) { onGround = 0; vy = -10; }  /* jump */ 
            else if (!solidAt(px + 2, py + CON_H) && !solidAt(px + CON_W - 2, py + CON_H)) { onGround = 0; vy = 0; }
        }
        if (py < 0) {
            if (lkU >= 0) { loadRoom((uint8_t)lkU); py += WORLD_H; }
            else py = 0;
        }
        if (lkD < 0 && py > WORLD_H - CON_H) { py = WORLD_H - CON_H; onGround = 1; vy = 0; }
        }
        dbg_px = px; dbg_py = py;

        /* camera vertical follow (screen shows 192 of 216+40) */
        {
            int8_t target = py + CON_H / 2 - 96;
            if (target < 0) target = 0; if (target > 24) target = 24;
            if (target != scrollY) { scrollY += (target > scrollY) ? 1 : -1; SMS_setBGScrollY(scrollY); }
        }

        /* item grab */
        if (itemIdx != 255 && (keys & PORT_A_KEY_2) &&
            px + CON_W > itemX && px < itemX + 16 && py + CON_H > itemY - 16 && py < itemY) {
            grabbedMask |= (uint32_t)1 << itemIdx;
            ++dbg_inv;
            SMS_loadTileMapArea(itemTX, itemTY, mapSave, 2, 2);
            itemIdx = 255;
        }

        /* animation frame */
        {
            const uint8_t *seq; uint8_t len;
            uint8_t flip = facing;  /* raw art faces left; mirror when facing right */
            switch (state) {
            case 1: seq = animWalk; len = 6; break;
            case 2: seq = animRun; len = 5; break;
            case 3: seq = animJump; len = 1; break;
            case 4: seq = animCrouch; len = 2; break;
            case 5: seq = animShoot; len = 1; break;
            case 6: seq = animCShoot; len = 1; break;
            case 7: seq = animHang; len = 2; break;
            case 8: seq = animClimb; len = 3; break;
            default: seq = animIdle; len = 1;
            }
            if (++animTick >= (state == 2 ? 2 : 4)) { animTick = 0; animIdx = (uint8_t)((animIdx + 1) % len); }
            if (seq[animIdx] != curFrame || flip != lastFlip) uploadFrame(seq[animIdx], flip);
        }
        drawConrad();
    }
    return 0;
}
