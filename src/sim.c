/*
 * Simulated gameplay: the ported interpreter decides where every object is and
 * which animation frame it shows, and this draws them with hardware sprites.
 * Sprites are looked up by animation number in the ROM table built by
 * tools/animconv.py, and stream through 32 8x16 VRAM slots.
 */
#include "SMSlib.h"
#include "data_index.h"
#include "pge.h"
#include "logic.h"
#include "room.h"
#include "sim.h"

#define SPR_SLOTS  32
#define MAX_PARTS  12

unsigned char sim_sprites;
unsigned int  sim_uploads;
unsigned char sim_scroll, sim_room;

static unsigned int  slot_tile[SPR_SLOTS];
static unsigned char slot_used[SPR_SLOTS];
static unsigned char slot_next;
static unsigned char parts[MAX_PARTS * 4];

static unsigned int rd16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned char slot_for(unsigned int tid)
{
    unsigned char i, s;
    for (i = 0; i < SPR_SLOTS; i++) {
        if (slot_tile[i] == tid) { slot_used[i] = 1; return i; }
    }
    for (i = 0; i < SPR_SLOTS; i++) {          /* a slot not needed this frame */
        s = slot_next;
        slot_next = (unsigned char)((slot_next + 1) % SPR_SLOTS);
        if (!slot_used[s]) {
            SMS_mapROMBank((unsigned char)(ANIM_TILE_BANK + (tid >> 8)));
            SMS_loadTiles((const unsigned char *)(0x8000 + ((tid & 255) << 6)), s << 1, 64);
            slot_tile[s] = tid;
            slot_used[s] = 1;
            sim_uploads++;
            return s;
        }
    }
    return 0xFF;                                /* every slot is in use */
}

/* The engine draws collectibles over the foreground scenery (its blit that
 * ignores the foreground mask).  An SMS sprite cannot override a background
 * tile's priority, so clear the priority bit on the few cells under each item
 * in the room - otherwise an item tucked into foliage is simply invisible. */
static void unhide_items(unsigned char idx, unsigned char room)
{
    unsigned char it, g = 0, cx, cy, cx0, cx1, cy0, cy1;
    int x, y;
    const unsigned char *nt;
    unsigned int e;
    for (it = room_head[room]; it != 0xFF && it < MAX_PGE && g++ < MAX_PGE; it = next_in_room[it]) {
        if (logic_object_type(it) != 3) continue;          /* collectibles only */
        x = pge_live[it].pos_x + 4;
        y = pge_live[it].pos_y - 4;
        if (x < 0 || y < 0 || x > 248 || y > 216) continue;
        cx0 = (unsigned char)(x >> 3); cx1 = (unsigned char)((x + 7) >> 3);
        cy0 = (unsigned char)(y >> 3); cy1 = (unsigned char)((y + 7) >> 3);
        SMS_mapROMBank(room_bank[idx]);                   /* logic paged its own bank */
        nt = (const unsigned char *)(room_addr[idx] + 18);
        for (cy = cy0; cy <= cy1 && cy < 28; cy++) {
            for (cx = cx0; cx <= cx1 && cx < 32; cx++) {
                e = rd16(nt + ((unsigned int)cy * 32 + cx) * 2) & ~0x1000;
                SMS_setTileatXY(cx, cy, e);
            }
        }
    }
}

static void load_room(unsigned char room)
{
    unsigned char idx = room_find(0, room);
    if (idx == 0xFF) return;
    SMS_displayOff();
    room_load(idx);
    unhide_items(idx, room);
    SMS_loadSpritePalette(anim_palette);
    SMS_displayOn();
    sim_room = room;
    for (idx = 0; idx < SPR_SLOTS; idx++) slot_tile[idx] = 0xFFFF;
}

void sim_start(void)
{
    unsigned char i;
    logic_check = 0;                            /* run on past any divergence */
    logic_use_pad = 1;                          /* played, not replayed */
    logic_start();
    for (i = 0; i < SPR_SLOTS; i++) { slot_tile[i] = 0xFFFF; slot_used[i] = 0; }
    slot_next = 0;
    sim_uploads = 0;
    sim_room = 0xFF;
    SMS_useFirstHalfTilesforSprites(1);
    SMS_setSpriteMode(SPRITEMODE_TALL);
    load_room(logic_cur_room);
}

/* the controller, in the engine's own key-mask encoding:
 * 1 up, 2 down, 4 left, 8 right, 0x10/0x20/0x40 the three action keys */
static unsigned char pad_mask(void)
{
    unsigned int k = SMS_getKeysStatus();
    unsigned char m = 0;
    if (k & PORT_A_KEY_UP)    m |= 1;
    if (k & PORT_A_KEY_DOWN)  m |= 2;
    if (k & PORT_A_KEY_LEFT)  m |= 4;
    if (k & PORT_A_KEY_RIGHT) m |= 8;
    if (k & PORT_A_KEY_2)     m |= 0x20;        /* action / gun */
    if (k & PORT_A_KEY_1)     m |= 0x40;        /* run */
    if ((k & PORT_A_KEY_1) && (k & PORT_A_KEY_2)) m |= 0x10;
    return m;
}

/* after a cutscene the VDP setup and the sprite cache have to come back */
void sim_resume(void)
{
    unsigned char i;
    for (i = 0; i < SPR_SLOTS; i++) { slot_tile[i] = 0xFFFF; slot_used[i] = 0; }
    slot_next = 0;
    SMS_useFirstHalfTilesforSprites(1);
    SMS_setSpriteMode(SPRITEMODE_TALL);
    sim_room = 0xFF;                            /* forces the room to reload */
}

void sim_step(void)
{
    unsigned int off, tid;
    unsigned char it, guard;
    unsigned char n, k, s, facing, count;
    int x, y;
    const unsigned char *p;
    LivePGE *pge;
    unsigned char *pp;

    logic_pad_mask = pad_mask();
    logic_step();
    if (logic_cur_room != sim_room) load_room(logic_cur_room);

    y = pge_live[0].pos_y - 120;
    if (y < 0) y = 0;
    if (y > ROOM_SCROLL_MAX) y = ROOM_SCROLL_MAX;
    sim_scroll = (unsigned char)y;
    SMS_setBGScrollY(sim_scroll);

    for (k = 0; k < SPR_SLOTS; k++) slot_used[k] = 0;
    SMS_initSprites();
    n = 0;
    /* only the objects of this room, from the interpreter's own room list,
     * instead of scanning all of them every frame */
    guard = 0;
    for (it = (sim_room < 64) ? room_head[sim_room] : 0xFF;
         it != 0xFF && it < MAX_PGE && n < 60 && guard++ < MAX_PGE;
         it = next_in_room[it]) {
        pge = &pge_live[it];
        if (!(pge->flags & 4) || pge->room_location != sim_room) continue;
        if (pge->anim_number > ANIM_MAX) continue;
        SMS_mapROMBank(ANIM_TAB_BANK);
        /* bit 1 mirrors the sprite; bit 3 says whether this animation number
         * means a character sprite or a level object - they share numbers */
        facing = ((pge->flags & 2) >> 1) | ((pge->flags & 8) >> 2);
        off = rd16((const unsigned char *)(0x8000 + ((pge->anim_number << 2) + facing) * 2));
        if (off == 0xFFFF) continue;
        SMS_mapROMBank((unsigned char)(ANIM_BLOB_BANK + (off >> 14)));
        p = (const unsigned char *)(0x8000 + (off & 0x3FFF));
        count = *p++;
        if (count > MAX_PARTS) count = MAX_PARTS;
        /* read each part straight from ROM: uploading a slot pages another
         * bank in, so just map this one back each time - cheaper than copying
         * the whole part list into RAM first */
        for (k = 0; k < count && n < 60; k++) {
            SMS_mapROMBank((unsigned char)(ANIM_BLOB_BANK + (off >> 14)));
            pp = (unsigned char *)(p + k * 4);
            tid = (unsigned int)pp[0] | ((unsigned int)pp[1] << 8);
            x = pge->pos_x + (signed char)pp[2];
            y = pge->pos_y + (signed char)pp[3] - sim_scroll;
            if (x < 0 || x > 248 || y < 1 || y > 176) continue;
            s = slot_for(tid);
            if (s == 0xFF) continue;
            SMS_addSprite((unsigned char)x, (unsigned char)y, s << 1);
            n++;
        }
    }
    sim_sprites = n;
    SMS_copySpritestoSAT();
}
