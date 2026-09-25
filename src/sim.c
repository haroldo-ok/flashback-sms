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
#include "psg.h"
#include "tiledec.h"

#define SPR_SLOTS  24
#define MAX_PARTS  40

/* level parts 4_1/4_2 and 5_1/5_2 share one map, so the rooms of a part live
 * under the map's own number */
static const unsigned char part_map[7] = { 0, 1, 2, 3, 3, 5, 5 };

unsigned char sim_sprites;
unsigned int  sim_uploads;
unsigned char sim_scroll, sim_room;

static unsigned int  slot_tile[SPR_SLOTS];
static unsigned char slot_used[SPR_SLOTS];
static unsigned char slot_next;

static unsigned int rd16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* an 8x16 sprite is two 8x8 tiles from the shared compact dictionary; the
 * VRAM slot cache is keyed on the pair */
static unsigned int slot_bot[SPR_SLOTS];
static unsigned int sf_bot;

/* the miss path of slot_for: upload into a slot not needed this frame */
static unsigned char slot_alloc(unsigned int top, unsigned int bot)
{
    unsigned char i, s;
    for (i = 0; i < SPR_SLOTS; i++) {
        s = slot_next;
        if (++slot_next == SPR_SLOTS) slot_next = 0;
        if (!slot_used[s]) {
            tile_upload(&spr_dict, top, s << 1);
            tile_upload(&spr_dict, bot, (s << 1) + 1);
            slot_tile[s] = top;
            slot_bot[s] = bot;
            slot_used[s] = 1;
            sim_uploads++;
            return s;
        }
    }
    return 0xFF;                                /* every slot is in use */
}

#if SPR_SLOTS != 24
#error slot_for below hard-codes 24 slots
#endif

/* The VRAM slot already holding this (top, bottom) pair, marked used; else
 * slot_alloc.  In asm: the C search compared two 16-bit keys through ix
 * slots, ~150 cycles per slot for every sprite part drawn.  This tests the
 * top key's low byte first, ~45 cycles per slot. */
static unsigned char slot_for(unsigned int top, unsigned int bot) __naked
{
    (void)top; (void)bot;
    __asm
    ld   (_sf_bot), de
    ex   de, hl                 ; de = top
    ld   hl, #_slot_tile
    ld   b, #24
00001$:
    ld   a, (hl)
    inc  hl
    cp   a, e
    jr   z, 00004$
00005$:
    inc  hl
    djnz 00001$
    ex   de, hl                 ; miss: hl = top, de = bot
    ld   de, (_sf_bot)
    jp   _slot_alloc
00004$:
    ld   a, (hl)
    cp   a, d
    jr   nz, 00005$
    ld   a, #24                 ; top matches: slot = 24 - b
    sub  a, b
    ld   c, a
    push hl
    push de
    ld   l, c
    ld   h, #0
    add  hl, hl
    ld   de, #_slot_bot
    add  hl, de
    ld   de, (_sf_bot)
    ld   a, (hl)
    cp   a, e
    jr   nz, 00006$
    inc  hl
    ld   a, (hl)
    cp   a, d
    jr   nz, 00006$
    pop  de
    pop  hl
    ld   hl, #_slot_used
    ld   b, #0
    add  hl, bc
    ld   (hl), #1
    ld   a, c
    ret
00006$:
    pop  de
    pop  hl
    jr   00005$
    __endasm;
}

/* Draw dp_count sprite parts of one object, starting at `pp` in ROM bank
 * dp_bank: each part is (u16 top, u16 bottom, i8 dx, i8 dy) and is drawn at
 * (dp_px + dx, dp_py + dy) when any of it is on screen (x 0..248, y -15..191:
 * the VDP clips an 8x16 sprite at the bottom edge and wraps one at the top),
 * stopping at 60 sprites (dp_n).  Uploading a new tile maps other banks, so
 * the parts' bank is mapped again for each part.  In asm - it runs for every
 * sprite part of every frame. */
static unsigned char dp_count, dp_bank, dp_n, dp_x, dp_y;
static int dp_px, dp_py;
static const unsigned char *dp_ptr;

static void draw_parts(const unsigned char *pp) __naked
{
    (void)pp;
    __asm
    ld   a, (_dp_count)
    or   a, a
    ret  z
    ld   (_dp_ptr), hl
00001$:
    ld   a, (_dp_n)
    cp   a, #60
    ret  nc
    ld   a, (_dp_bank)
    ld   (_ROM_bank_to_be_mapped_on_slot2), a
    ld   hl, (_dp_ptr)
    ld   de, #4
    add  hl, de
    ld   a, (hl)                ; dx
    ld   e, a
    rlca
    sbc  a, a
    ld   d, a
    inc  hl
    ld   c, (hl)                ; dy
    ld   hl, (_dp_px)
    add  hl, de
    ld   a, h
    or   a, a
    jr   nz, 00009$             ; x < 0 or > 255
    ld   a, l
    cp   a, #249
    jr   nc, 00009$             ; x > 248
    ld   (_dp_x), a
    ld   a, c
    ld   e, a
    rlca
    sbc  a, a
    ld   d, a
    ld   hl, (_dp_py)
    add  hl, de
    ld   a, h
    or   a, a
    jr   z, 00002$
    inc  a
    jr   nz, 00009$             ; y < -256 or > 255
    ld   a, l
    cp   a, #241
    jr   c, 00009$              ; y < -15: entirely above the screen
    jr   00003$
00002$:
    ld   a, l
    cp   a, #192
    jr   nc, 00009$             ; y > 191: entirely below the screen
00003$:
    ld   (_dp_y), a
    ld   hl, (_dp_ptr)
    ld   e, (hl)
    inc  hl
    ld   d, (hl)                ; top
    inc  hl
    ld   c, (hl)
    inc  hl
    ld   b, (hl)                ; bottom
    ex   de, hl
    ld   e, c
    ld   d, b
    call _slot_for              ; a = VRAM slot, 0xFF if none free
    cp   a, #0xff
    jr   z, 00009$
    add  a, a
    ld   e, a                   ; tile = slot * 2
    ld   a, (_dp_x)
    ld   d, a
    ld   a, (_dp_y)
    ld   l, a
    ld   h, #0
    call _SMS_addSprite_f
    ld   hl, #_dp_n
    inc  (hl)
00009$:
    ld   hl, (_dp_ptr)
    ld   de, #6
    add  hl, de
    ld   (_dp_ptr), hl
    ld   hl, #_dp_count
    dec  (hl)
    jr   nz, 00001$
    ret
    __endasm;
}

#if MAX_PARTS != 40
#error draw_objects below hard-codes MAX_PARTS 40
#endif

/* Draw the active objects of the current room, from the interpreter's own
 * room list, into hardware sprites (at most 60, counted in dp_n).  Per
 * object, in asm; the C it implements:
 *   skip unless flags bit 2 (active) and room_location == sim_room
 *   set = flags & 8 ? 5 (level objects)
 *       : anim in 0x22F..0x28D ? 1 + the level's monster type for this object
 *                                (0xFF: no sprites, skip)
 *       : 0 (Conrad)
 *   skip unless spr_tab_lo[set] <= anim <= spr_tab_hi[set]
 *   entry = spr_tab_addr[set] + ((anim - lo) * 2 + mirror) * 3 in bank
 *   spr_tab_bank[set]: u8 bank (0 = none), u16 address of u8 count + parts
 *   draw_parts(min(count, 40) parts) at (pos_x, pos_y - sim_scroll)
 * mirror is flags bit 1 (the effective mirror, not the facing bit). */
static unsigned char do_it, do_guard;

static void draw_objects(void) __naked
{
    __asm
    push ix
    ld   a, (_sim_room)
    cp   a, #64
    jp   nc, 00099$
    ld   e, a
    ld   d, #0
    ld   hl, #_room_head
    add  hl, de
    ld   a, #255
    ld   (_do_guard), a
    ld   a, (hl)
00001$:
    cp   a, #0xff
    jp   z, 00099$
    ld   (_do_it), a
    ld   a, (_dp_n)
    cp   a, #60
    jp   nc, 00099$
    ld   hl, #_do_guard
    ld   a, (hl)
    or   a, a
    jp   z, 00099$
    dec  (hl)
    ld   a, (_do_it)
    ld   l, a
    ld   h, #0
    ld   e, l
    ld   d, h
    add  hl, hl
    add  hl, hl
    add  hl, hl
    add  hl, hl
    add  hl, de
    add  hl, de
    add  hl, de
    ld   de, #_pge_live
    add  hl, de
    push hl
    pop  ix                     ; ix = the object
    bit  2, 17 (ix)
    jp   z, 00090$
    ld   a, (_sim_room)
    cp   a, 15 (ix)
    jp   nz, 00090$
    bit  3, 17 (ix)             ; which sprite set
    jr   z, 00002$
    ld   a, #5
    jr   00005$
00002$:
    ld   l, 6 (ix)
    ld   h, 7 (ix)
    ld   de, #0x22f
    or   a, a
    sbc  hl, de
    jr   c, 00004$
    ld   de, #0x5f
    or   a, a
    sbc  hl, de
    jr   nc, 00004$
    ld   a, (_logic_level)      ; a monster: its type for this level part
    ld   e, a
    ld   d, #0
    ld   hl, #_spr_mon_bank
    add  hl, de
    ld   a, (hl)
    ld   (_ROM_bank_to_be_mapped_on_slot2), a
    ld   hl, #_spr_mon_addr
    add  hl, de
    add  hl, de
    ld   a, (hl)
    inc  hl
    ld   h, (hl)
    ld   l, a
    ld   e, 18 (ix)
    ld   d, #0
    add  hl, de
    ld   a, (hl)
    cp   a, #0xff
    jp   z, 00090$
    inc  a
    jr   00005$
00004$:
    xor  a, a                   ; Conrad
00005$:
    ld   e, a
    ld   d, #0                  ; de = set
    ld   hl, #_spr_tab_lo
    add  hl, de
    add  hl, de
    ld   c, (hl)
    inc  hl
    ld   b, (hl)                ; lo
    ld   l, 6 (ix)
    ld   h, 7 (ix)
    or   a, a
    sbc  hl, bc
    jp   c, 00090$              ; anim < lo
    push hl                     ; anim - lo
    ld   hl, #_spr_tab_hi
    add  hl, de
    add  hl, de
    ld   a, (hl)
    inc  hl
    ld   h, (hl)
    ld   l, a
    ld   c, 6 (ix)
    ld   b, 7 (ix)
    or   a, a
    sbc  hl, bc
    pop  hl
    jp   c, 00090$              ; anim > hi
    add  hl, hl
    bit  1, 17 (ix)
    jr   z, 00006$
    inc  hl
00006$:
    ld   c, l
    ld   b, h
    add  hl, hl
    add  hl, bc                 ; entry * 3
    push hl
    ld   hl, #_spr_tab_bank
    add  hl, de
    ld   a, (hl)
    ld   (_ROM_bank_to_be_mapped_on_slot2), a
    ld   hl, #_spr_tab_addr
    add  hl, de
    add  hl, de
    ld   a, (hl)
    inc  hl
    ld   h, (hl)
    ld   l, a
    pop  bc
    add  hl, bc
    ld   a, (hl)                ; bank of the parts
    or   a, a
    jr   z, 00090$
    ld   (_dp_bank), a
    inc  hl
    ld   e, (hl)
    inc  hl
    ld   d, (hl)
    ld   (_ROM_bank_to_be_mapped_on_slot2), a
    ex   de, hl
    ld   a, (hl)                ; part count
    inc  hl
    cp   a, #41
    jr   c, 00007$
    ld   a, #40
00007$:
    ld   (_dp_count), a
    ld   c, 2 (ix)
    ld   b, 3 (ix)
    ld   (_dp_px), bc
    ld   c, 4 (ix)
    ld   b, 5 (ix)
    ld   a, (_sim_scroll)
    push hl
    ld   l, c
    ld   h, b
    ld   c, a
    ld   b, #0
    or   a, a
    sbc  hl, bc
    ld   (_dp_py), hl
    pop  hl
    call _draw_parts
00090$:
    ld   a, (_do_it)
    ld   e, a
    ld   d, #0
    ld   hl, #_next_in_room
    add  hl, de
    ld   a, (hl)
    jp   00001$
00099$:
    pop  ix
    ret
    __endasm;
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

/* The camera.  The room is 224 lines and the screen 192, so the view scrolls
 * by up to ROOM_SCROLL_MAX.  It follows the floor Conrad is on (the engine
 * stands characters at y 70, 142 and 214), not his pos_y: that moves with
 * every animation frame's own offset - walking bobs, stepping down a ledge
 * rises before it drops - and following it made the screen jitter.  The
 * floor only changes once he is well away from it, and the view then glides
 * there; a new room starts at its target at once. */
#define CAM_STEP 2                      /* lines per game tick */
static int cam_floor;
static unsigned char cam_snap;

static int nearest_floor(int y)
{
    if (y < 106) return 70;
    if (y < 178) return 142;
    return 214;
}

static void update_scroll(void)
{
    int y = pge_live[0].pos_y, target;
    if (cam_snap || y < cam_floor - 48 || y > cam_floor + 48) cam_floor = nearest_floor(y);
    target = cam_floor - 120;
    if (target < 0) target = 0;
    if (target > ROOM_SCROLL_MAX) target = ROOM_SCROLL_MAX;
    if (cam_snap) sim_scroll = (unsigned char)target;
    else if (sim_scroll + CAM_STEP <= target) sim_scroll += CAM_STEP;
    else if (sim_scroll >= target + CAM_STEP) sim_scroll -= CAM_STEP;
    else sim_scroll = (unsigned char)target;
    cam_snap = 0;
    SMS_setBGScrollY(sim_scroll);
}

static void load_room(unsigned char room)
{
    unsigned char idx = room_find(part_map[logic_level], room);
    if (idx == 0xFF) return;
    SMS_displayOff();
    room_load(idx);
    unhide_items(idx, room);
    SMS_loadSpritePalette(spr_palette);
    SMS_displayOn();
    sim_room = room;
    cam_snap = 1;
    for (idx = 0; idx < SPR_SLOTS; idx++) { slot_tile[idx] = 0xFFFF; slot_bot[idx] = 0xFFFF; }
}

void sim_start(unsigned char level_index)
{
    unsigned char i;
    music_stop();                               /* no in-game score yet */
    logic_check = 0;                            /* run on past any divergence */
    logic_use_pad = 1;                          /* played, not replayed */
    logic_start(level_index);
    for (i = 0; i < SPR_SLOTS; i++) { slot_tile[i] = 0xFFFF; slot_bot[i] = 0xFFFF; slot_used[i] = 0; }
    slot_next = 0;
    sim_uploads = 0;
    sim_room = 0xFF;
    input_take_held();                          /* the press that started the level */
    SMS_useFirstHalfTilesforSprites(1);
    SMS_setSpriteMode(SPRITEMODE_TALL);
    load_room(logic_cur_room);
}

/* the controller, in the engine's own key-mask encoding:
 * 1 up, 2 down, 4 left, 8 right, 0x10/0x20/0x40 the three action keys */
static unsigned char pad_mask(void)
{
    /* a button down at any frame since the last tick counts: a tick takes
     * over 2 frames, and a quick tap could fall between two reads */
    unsigned int k = SMS_getKeysStatus() | input_take_held();
    unsigned char m = 0;
    if (k & PORT_A_KEY_UP)    m |= 1;
    if (k & PORT_A_KEY_DOWN)  m |= 2;
    if (k & PORT_A_KEY_LEFT)  m |= 4;
    if (k & PORT_A_KEY_RIGHT) m |= 8;
    /* Exactly ONE modifier at a time: the scripts compare the whole mask, so
     * both buttons together must mean the third key alone (0x10) - lifts and
     * other machinery are worked with that key plus up/down.  Setting all
     * three bits matched nothing at all. */
    if ((k & PORT_A_KEY_1) && (k & PORT_A_KEY_2)) m |= 0x10;   /* use / third key */
    else if (k & PORT_A_KEY_2) m |= 0x20;                      /* action, gun */
    else if (k & PORT_A_KEY_1) m |= 0x40;                      /* run */
    return m;
}

/* after a cutscene the VDP setup and the sprite cache have to come back */
void sim_resume(void)
{
    unsigned char i;
    for (i = 0; i < SPR_SLOTS; i++) { slot_tile[i] = 0xFFFF; slot_bot[i] = 0xFFFF; slot_used[i] = 0; }
    slot_next = 0;
    input_take_held();                          /* nothing held during the cutscene */
    SMS_useFirstHalfTilesforSprites(1);
    SMS_setSpriteMode(SPRITEMODE_TALL);
    sim_room = 0xFF;                            /* forces the room to reload */
}

void sim_step(void)
{
    unsigned char n, k;

    logic_pad_mask = pad_mask();
    logic_step();
    if (logic_cur_room != sim_room) load_room(logic_cur_room);
    update_scroll();

    for (k = 0; k < SPR_SLOTS; k++) slot_used[k] = 0;
    SMS_initSprites();
    dp_n = 0;
    draw_objects();
    n = dp_n;
    sim_sprites = n;
    SMS_copySpritestoSAT();
}
