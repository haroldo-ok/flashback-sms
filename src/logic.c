/*
 * Game logic: the object script interpreter, ported from the original
 * engine's pge_process()/pge_execute().
 *
 * This is deliberately self-checking.  The ROM carries, for every 30 Hz frame
 * of the recorded demo, the input byte the engine consumed and a checksum of
 * every object's state afterwards.  Each simulated frame is compared with it,
 * so the port reports exactly where it first diverges from the original, and
 * which opcode it could not execute.
 *
 * Implemented so far: the frame loop, message queue, animation advance, the
 * object-script walk and dispatch, and a first tranche of opcodes.  Opcodes
 * that are not ported yet stop the run and are reported (logic_bad_op).
 */
#include "SMSlib.h"
#include "data_index.h"
#include "pge.h"
#include "logic.h"
#include "psg.h"

/* the asm routines below hard-code these LivePGE offsets (16-bit ints) */
#ifdef __SDCC
#define LAYOUT_CHECK(name, cond) typedef char name[(cond) ? 1 : -1]
#else
#define LAYOUT_CHECK(name, cond) typedef char name[1]
#endif
LAYOUT_CHECK(lp_size,  sizeof(LivePGE) == 19);
LAYOUT_CHECK(lp_type,  __builtin_offsetof(LivePGE, obj_type) == 0);
LAYOUT_CHECK(lp_x,     __builtin_offsetof(LivePGE, pos_x) == 2);
LAYOUT_CHECK(lp_y,     __builtin_offsetof(LivePGE, pos_y) == 4);
LAYOUT_CHECK(lp_anim,  __builtin_offsetof(LivePGE, anim_number) == 6);
LAYOUT_CHECK(lp_seq,   __builtin_offsetof(LivePGE, anim_seq) == 14);
LAYOUT_CHECK(lp_flags, __builtin_offsetof(LivePGE, flags) == 17);

#define OBJECT_SIZE      18
#define OBJECTS_PER_BANK 910
#define INIT_PGE_SIZE    31
#define I_NODE           6
#define I_DATA           10
#define I_FLAGS          27

/* 16-bit division is very slow on a Z80, and the collision code divides room
 * coordinates by 72 and 36 constantly.  Both ranges are tiny, so compare. */
static int div72(int v)
{
    if (v < 0) return (v > -72) ? 0 : ((v > -144) ? -1 : -2);
    if (v < 72) return 0;
    if (v < 144) return 1;
    if (v < 216) return 2;
    return 3;
}

static int div36(int v)
{
    if (v < 0) return (v > -36) ? 0 : ((v > -72) ? -1 : -2);
    if (v < 36) return 0;
    if (v < 72) return 1;
    if (v < 108) return 2;
    if (v < 144) return 3;
    if (v < 180) return 4;
    if (v < 216) return 5;
    return 6;
}

unsigned int  logic_frame;
unsigned int  logic_frames_ok;      /* frames matching the original engine */
unsigned int  logic_first_bad;      /* first frame that differs (0xFFFF = none) */
unsigned char logic_bad_op;         /* first opcode that is not ported yet */
unsigned int  logic_bad_op_frame;
unsigned char logic_running;
unsigned char logic_check = 1;
unsigned char logic_use_pad;     /* 1 = take input from the controller */
unsigned char logic_pad_mask;    /* the engine's _pge_inpKeysMask */
unsigned int  logic_cutscene = 0xFFFF;   /* a cutscene the game asked for */
unsigned int prof_objs, prof_ani, prof_ct, prof_init, prof_prep, prof_slots;   /* compare against the engine and stop when it differs */
unsigned int  logic_checksum;
unsigned int  logic_expected;

static unsigned char s_bank_a, s_bank_obj, s_bank_aniidx, s_bank_ani, s_bank_logic;
static unsigned int  s_total_frames;
static unsigned int  s_node_tab;       /* part A: the per-node first-object table */
unsigned char logic_cur_room;
unsigned char logic_level;
unsigned char col_peak_pos, col_peak_slot, msg_peak, ov_peak;       /* level part being played (0..6) */
#define s_room logic_cur_room
static unsigned char s_inp;             /* _pge_inpKeysMask */
static unsigned char s_facing, s_pge_room;
static unsigned char s_last_lr;   /* last horizontal-only direction */
static unsigned char s_load_map;   /* the engine's _loadMap */
static int s_gun_var;              /* the engine's _pge_opGunVar */
static unsigned long s_rand;

/* message queue: one list head per object, entries in a small pool */
#define MSG_POOL 64
static unsigned char msg_head[MAX_PGE];
static unsigned char msg_next[MSG_POOL], msg_src[MSG_POOL], msg_num[MSG_POOL];
static unsigned char msg_free;

unsigned char room_head[64];            /* _pge_liveTable1; the renderer
                                         * walks these instead of every object */
unsigned char next_in_room[MAX_PGE];
static unsigned char in_list[MAX_PGE];  /* which room list each object is on */

static unsigned char init_field8(unsigned char idx, unsigned char off);
static const unsigned char *map_ani(unsigned int obj_type);
static void msg_send(unsigned char src, unsigned char dst, unsigned char num);

/* ------------------------------------------------------------ collision --
 * A collision slot records that an object occupies a cell of the 16x7 room
 * grid; slots sharing a cell are chained, which is how objects find each
 * other.  Slot storage is the engine's, shrunk to fit SMS RAM. */
#define COL_SLOTS 160
static unsigned int  col_ct_pos[COL_SLOTS];
static unsigned char col_live[COL_SLOTS];      /* object index */
static unsigned char col_index[COL_SLOTS];     /* next cell of the same object */
static unsigned char col_prev[COL_SLOTS];      /* previous slot in this cell */
static unsigned char col_table[COL_SLOTS];     /* head slot per occupied cell */
static unsigned char col_cur_pos, col_cur_slot;

static unsigned char col_left_room, col_right_room;
static int s_grid_x, s_grid_y;                 /* current object's grid cell */

#define CT_UP 0x00
#define CT_DOWN 0x40
#define CT_RIGHT 0x80
#define CT_LEFT 0xC0

static unsigned char s_bank_ct;

/* Objects can WRITE into the collision grid (pge_updateCollisionState), and
 * those writes last for the rest of the level - but the grid lives in ROM.
 * Modified spans are therefore kept in RAM and consulted by every grid read,
 * which is the engine's own slot-2 mechanism seen from the other side. */
#define OVERLAY_SPANS 16
#define OVERLAY_LEN   16
static unsigned int  ov_off[OVERLAY_SPANS];
static unsigned char ov_len[OVERLAY_SPANS];
static unsigned char ov_val[OVERLAY_SPANS][OVERLAY_LEN];
static unsigned char ov_count;

static signed char ct_s(unsigned int off)
{
    SMS_mapROMBank(s_bank_ct);
    return (signed char)*(const unsigned char *)(0x8000 + off);
}

/* a cell of the room grid, overlay first */
static signed char grid_read(unsigned int idx)
{
    unsigned char i;
    for (i = 0; i < ov_count; i++) {
        if (idx >= ov_off[i] && idx < ov_off[i] + ov_len[i])
            return (signed char)ov_val[i][idx - ov_off[i]];
    }
    return ct_s(0x100 + idx);
}

static int col_update_state(LivePGE *pge, int dy, unsigned char value)
{
    unsigned char len = init_field8(pge->index, 28);
    unsigned int base;
    unsigned char i, k;
    int gy, gx;
    if (pge->room_location >= 0x40 || len == 0 || len > OVERLAY_LEN) return 1;
    gy = (div36(pge->pos_y) & ~1) + dy;
    gx = (pge->pos_x + 8) >> 4;
    base = (unsigned int)(0x70 * pge->room_location + gx + gy * 16);
    if (s_facing) base -= (len - 1);
    for (i = 0; i < ov_count; i++) {
        if (ov_off[i] == base) {
            ov_len[i] = len;
            for (k = 0; k < len; k++) ov_val[i][k] = value;
            return 1;
        }
    }
    if (ov_count < OVERLAY_SPANS) {
        ov_off[ov_count] = base;
        ov_len[ov_count] = len;
        for (k = 0; k < len; k++) ov_val[ov_count][k] = value;
        ov_count++;
    }
    return 1;
}

/* one bit per (pos & 63) in use: a clear bit means col_find_slot can skip its
 * scan.  Positions from other rooms share bits, which only costs a scan. */
static unsigned char col_used[8];

static void col_clear_state(void)
{
    unsigned char i;
    col_cur_pos = 0;
    col_cur_slot = 0;
    for (i = 0; i < 8; i++) col_used[i] = 0;
}

static const unsigned char bit_of[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };

static unsigned int col_get_grid_pos(LivePGE *pge, int dx)
{
    int x = pge->pos_x + dx;
    int y = pge->pos_y;
    signed char c = (signed char)pge->room_location;
    if (c < 0) return 0xFFFF;
    if (x < 0)          { c = ct_s(CT_LEFT + c);  if (c < 0) return 0xFFFF; x += 256; }
    else if (x >= 256)  { c = ct_s(CT_RIGHT + c); if (c < 0) return 0xFFFF; x -= 256; }
    else if (y < 0)     { c = ct_s(CT_UP + c);    if (c < 0) return 0xFFFF; y += 216; }
    else if (y >= 216)  { c = ct_s(CT_DOWN + c);  if (c < 0) return 0xFFFF; y -= 216; }
    x = (x + 8) >> 4;
    y = div72(y - 8);
    if (x < 0 || x > 15 || y < 0 || y > 2) return 0xFFFF;
    return (unsigned int)(y * 16 + x + c * 64);
}

/* the index i with col_ct_pos[col_table[i]] == pos, or -1.  In asm because
 * SDCC kept every temporary of this loop in ix slots, ~450 cycles a step. */
static int col_find_slot(unsigned int pos) __naked
{
    (void)pos;
    __asm
    ex   de, hl                 ; de = pos
    ld   a, e
    and  a, #0x3f
    ld   c, a
    rrca
    rrca
    rrca
    and  a, #0x07
    ld   hl, #_col_used
    add  a, l
    ld   l, a
    adc  a, h
    sub  a, l
    ld   h, a
    ld   b, (hl)                ; the col_used byte
    ld   a, c
    and  a, #0x07
    ld   hl, #_bit_of
    add  a, l
    ld   l, a
    adc  a, h
    sub  a, l
    ld   h, a
    ld   a, (hl)
    and  a, b
    jr   z, 00009$
    ld   a, (_col_cur_pos)
    or   a, a
    jr   z, 00009$
    ld   b, a
    ld   c, #0
    ld   iy, #_col_table
00001$:
    ld   l, 0 (iy)
    ld   h, #0
    add  hl, hl
    ld   a, l
    add  a, #<(_col_ct_pos)
    ld   l, a
    ld   a, h
    adc  a, #>(_col_ct_pos)
    ld   h, a
    ld   a, (hl)
    cp   a, e
    jr   nz, 00002$
    inc  hl
    ld   a, (hl)
    cp   a, d
    jr   z, 00008$
00002$:
    inc  iy
    inc  c
    djnz 00001$
00009$:
    ld   de, #0xffff
    ret
00008$:
    ld   e, c
    ld   d, #0
    ret
    __endasm;
}

#if COL_SLOTS != 160
#error col_prepare_piege_state below hard-codes 160 collision slots
#endif
LAYOUT_CHECK(lp_room,  __builtin_offsetof(LivePGE, room_location) == 15);
LAYOUT_CHECK(lp_cslot, __builtin_offsetof(LivePGE, collision_slot) == 16);
LAYOUT_CHECK(lp_index, __builtin_offsetof(LivePGE, index) == 18);

/* pge_prepareCollisionState for one object: record each of its
 * collision_data_len cells (one grid cell apart, left to right) in a slot,
 * chaining slots that share a cell.  In asm - it runs for every cell of every
 * object each frame and SDCC's version was ~4k cycles a call.  Step by step:
 *   len = init_field8(index, 28); if (!len) { collision_slot = 0xFF; return; }
 *   fast = room >= 0 && 0 <= pos_y < 216 && (row = div72(pos_y - 8)) in 0..2
 *   link = &collision_slot; x = pos_x
 *   per cell c:  if (col_cur_slot >= 160) return;  slot2 = col_cur_slot++
 *     pos = fast && 0 <= x < 256 ? row * 16 + room * 64 + ((x + 8) >> 4)
 *                                : col_get_grid_pos(pge, c * 16)
 *     if (pos == 0xFFFF) { *link = 0xFF; return; }
 *     col_ct_pos[slot2] = pos; col_live[slot2] = index; col_index[slot2] = 0xFF
 *     if ((f = col_find_slot(pos)) >= 0) {
 *         prev = col_table[f]; col_prev[slot2] = prev; col_table[f] = slot2
 *         *link = f; if (flags & 0x80) flags |= 4
 *         if (prev != 0xFF && (pge_live[col_live[prev]].flags & 0x80)) ... |= 4
 *     } else {
 *         col_prev[slot2] = 0xFF; if (col_cur_pos >= 160) return
 *         col_table[col_cur_pos] = slot2; mark pos & 63 in col_used
 *         update the peaks; *link = col_cur_pos++
 *     }
 *     link = &col_index[slot2]; x += 16                                    */
static unsigned char cp_len, cp_fast, cp_slot2;
static unsigned char *cp_link;
static unsigned int cp_pos, cp_row, cp_dx;
static int cp_x;

static void col_prepare_piege_state(LivePGE *pge) __naked
{
    (void)pge;
    __asm
    push ix
    push hl
    pop  ix                     ; ix = pge
    ld   a, 18 (ix)
    ld   l, #28
    call _init_field8
    or   a, a
    jr   nz, 00001$
    ld   16 (ix), #0xff
    pop  ix
    ret
00001$:
    ld   (_cp_len), a
    xor  a, a
    ld   (_cp_fast), a
    bit  7, 15 (ix)
    jr   nz, 00002$             ; room < 0
    ld   l, 4 (ix)
    ld   h, 5 (ix)
    bit  7, h
    jr   nz, 00002$             ; pos_y < 0
    ld   de, #216
    or   a, a
    sbc  hl, de
    jr   nc, 00002$             ; pos_y >= 216
    add  hl, de
    ld   de, #-8
    add  hl, de
    call _div72                 ; de = row
    ld   a, d
    or   a, a
    jr   nz, 00002$
    ld   a, e
    cp   a, #3
    jr   nc, 00002$
    ld   l, a
    ld   h, #0
    add  hl, hl
    add  hl, hl
    add  hl, hl
    add  hl, hl                 ; row * 16
    ex   de, hl
    ld   l, 15 (ix)
    ld   h, #0
    add  hl, hl
    add  hl, hl
    add  hl, hl
    add  hl, hl
    add  hl, hl
    add  hl, hl                 ; room * 64
    add  hl, de
    ld   (_cp_row), hl
    ld   a, #1
    ld   (_cp_fast), a
00002$:
    push ix
    pop  hl
    ld   de, #16
    add  hl, de
    ld   (_cp_link), hl         ; &collision_slot
    ld   l, 2 (ix)
    ld   h, 3 (ix)
    ld   (_cp_x), hl
    ld   hl, #0
    ld   (_cp_dx), hl
00010$:                         ; per cell
    ld   a, (_col_cur_slot)
    cp   a, #160
    jp   nc, 00099$
    ld   (_cp_slot2), a
    inc  a
    ld   (_col_cur_slot), a
    ld   a, (_cp_fast)
    or   a, a
    jr   z, 00011$
    ld   hl, (_cp_x)
    ld   a, h
    or   a, a
    jr   nz, 00011$             ; x outside 0..255
    ld   a, l
    add  a, #8
    rra                         ; 9-bit (x + 8) >> 1
    srl  a
    srl  a
    srl  a                      ; (x + 8) >> 4
    ld   hl, (_cp_row)
    add  a, l
    ld   l, a
    jr   nc, 00012$
    inc  h
    jr   00012$
00011$:
    push ix
    pop  hl
    ld   de, (_cp_dx)
    call _col_get_grid_pos      ; de = pos
    ex   de, hl
    ld   a, l
    and  a, h
    inc  a
    jr   nz, 00012$
    ld   hl, (_cp_link)         ; off the map: end the chain
    ld   (hl), #0xff
    jp   00099$
00012$:
    ld   (_cp_pos), hl
    ex   de, hl                 ; de = pos
    ld   a, (_cp_slot2)
    ld   c, a
    ld   b, #0
    ld   hl, #_col_ct_pos
    add  hl, bc
    add  hl, bc
    ld   (hl), e
    inc  hl
    ld   (hl), d
    ld   hl, #_col_live
    add  hl, bc
    ld   a, 18 (ix)
    ld   (hl), a
    ld   hl, #_col_index
    add  hl, bc
    ld   (hl), #0xff
    ex   de, hl                 ; hl = pos
    call _col_find_slot         ; de = found or -1
    bit  7, d
    jr   nz, 00020$
    ld   c, e                   ; found
    ld   b, #0
    ld   hl, #_col_table
    add  hl, bc
    ld   d, (hl)                ; d = prev
    ld   a, (_cp_slot2)
    ld   (hl), a                ; col_table[found] = slot2
    ld   c, a
    ld   hl, #_col_prev
    add  hl, bc
    ld   (hl), d
    ld   hl, (_cp_link)
    ld   (hl), e
    bit  7, 17 (ix)
    jr   z, 00013$
    set  2, 17 (ix)
00013$:
    ld   a, d
    inc  a
    jr   z, 00030$              ; no previous occupant
    ld   c, d
    ld   hl, #_col_live
    add  hl, bc
    ld   l, (hl)
    ld   h, #0
    ld   e, l
    ld   d, h
    add  hl, hl
    add  hl, hl
    add  hl, hl
    add  hl, hl
    add  hl, de
    add  hl, de
    add  hl, de                 ; index * 19
    ld   de, #(_pge_live + 17)
    add  hl, de
    bit  7, (hl)
    jr   z, 00030$
    set  2, (hl)
    jr   00030$
00020$:                         ; a new cell
    ld   a, (_cp_slot2)
    ld   c, a
    ld   b, #0
    ld   hl, #_col_prev
    add  hl, bc
    ld   (hl), #0xff
    ld   a, (_col_cur_pos)
    cp   a, #160
    jr   nc, 00099$
    ld   e, a                   ; e = col_cur_pos
    ld   c, a
    ld   hl, #_col_table
    add  hl, bc
    ld   a, (_cp_slot2)
    ld   (hl), a
    ld   a, (_cp_pos)
    and  a, #63
    ld   d, a
    rrca
    rrca
    rrca
    and  a, #7
    ld   c, a
    ld   hl, #_col_used
    add  hl, bc
    push hl
    ld   a, d
    and  a, #7
    ld   c, a
    ld   hl, #_bit_of
    add  hl, bc
    ld   a, (hl)
    pop  hl
    or   a, (hl)
    ld   (hl), a
    ld   a, (_col_peak_pos)
    cp   a, e
    jr   nc, 00021$
    ld   a, e
    ld   (_col_peak_pos), a
00021$:
    ld   a, (_col_cur_slot)
    ld   d, a
    ld   a, (_col_peak_slot)
    cp   a, d
    jr   nc, 00022$
    ld   a, d
    ld   (_col_peak_slot), a
00022$:
    ld   hl, (_cp_link)
    ld   (hl), e
    ld   a, e
    inc  a
    ld   (_col_cur_pos), a
00030$:                         ; next cell
    ld   a, (_cp_slot2)
    ld   c, a
    ld   b, #0
    ld   hl, #_col_index
    add  hl, bc
    ld   (_cp_link), hl
    ld   de, #16
    ld   hl, (_cp_x)
    add  hl, de
    ld   (_cp_x), hl
    ld   hl, (_cp_dx)
    add  hl, de
    ld   (_cp_dx), hl
    ld   hl, #_cp_len
    dec  (hl)
    jp   nz, 00010$
00099$:
    pop  ix
    ret
    __endasm;
}

static void col_prepare_room_state(void)
{
    col_left_room = (unsigned char)ct_s(CT_LEFT + s_room);
    col_right_room = (unsigned char)ct_s(CT_RIGHT + s_room);
}

/* the collision grid value next to an object */
static int col_get_grid_data(LivePGE *pge, int dy, int dx)
{
    int gx, gy;
    signed char next_room;
    if (s_facing) dx = -dx;
    gy = s_grid_y + dy;
    gx = s_grid_x + dx;
    if (gx < 0) {
        next_room = ct_s(CT_LEFT + pge->room_location);
        if (next_room < 0) return 1;
        return grid_read(gx + 16 + gy * 16 + next_room * 0x70);
    } else if (gx >= 16) {
        next_room = ct_s(CT_RIGHT + pge->room_location);
        if (next_room < 0) return 1;
        return grid_read(gx - 16 + gy * 16 + next_room * 0x70);
    } else if (gy < 1) {
        next_room = ct_s(CT_UP + pge->room_location);
        if (next_room < 0) return 1;
        return grid_read(gx + (gy + 6) * 16 + next_room * 0x70);
    } else if (gy >= 7) {
        next_room = ct_s(CT_DOWN + pge->room_location);
        if (next_room < 0) return 1;
        return grid_read(gx + (gy - 6) * 16 + next_room * 0x70);
    }
    return grid_read(gx + gy * 16 + pge->room_location * 0x70);
}

/* walk the objects sharing this object's cells; `mode` picks the engine's
 * comparison callback (0 = by animation y, 1 = by object type, 2 = by index) */
static unsigned int s_compare_var1;

/* the object sharing one of this object's cells (col_findPiege) */
static unsigned char col_find_piege(LivePGE *pge, unsigned int want)
{
    unsigned char cs, other, guard = 0;
    if (pge->collision_slot == 0xFF || pge->collision_slot >= COL_SLOTS) return 0xFF;
    cs = col_table[pge->collision_slot];
    while (cs != 0xFF && guard++ < COL_SLOTS) {
        other = col_live[cs];
        if (other != pge->index) {
            if (want == 0xFFFF || want == init_field8(other, 18)) return other;
        }
        cs = col_prev[cs];
    }
    return 0xFF;
}

/* Object indices come from level data and from the scripts, so they can be
 * out of range - 0xFF means "none" in several fields.  Indexing pge_live with
 * one of those writes far outside the array and takes the machine down with
 * it, which is what a freeze with no watchdog red screen looks like. */
static unsigned char bad_pge(unsigned char i)
{
    return (i >= pge_num || i >= MAX_PGE);
}

/* Inventory links (who carries what) as a small side table: only Conrad and
 * the few items being carried ever need them, so three bytes for every one of
 * the 255 objects would be RAM this machine does not have. */
#define INV_SLOTS 40
static unsigned char inv_owner[INV_SLOTS];
static unsigned char inv_cur_v[INV_SLOTS], inv_next_v[INV_SLOTS], inv_ref_v[INV_SLOTS];

static unsigned char inv_find(unsigned char idx)
{
    unsigned char i;
    for (i = 0; i < INV_SLOTS; i++)
        if (inv_owner[i] == idx) return i;
    return 0xFF;
}

static unsigned char inv_make(unsigned char idx)
{
    unsigned char i = inv_find(idx);
    if (i != 0xFF) return i;
    for (i = 0; i < INV_SLOTS; i++)
        if (inv_owner[i] == 0xFF) {
            inv_owner[i] = idx;
            inv_cur_v[i] = inv_next_v[i] = inv_ref_v[i] = 0xFF;
            return i;
        }
    return 0xFF;                       /* pool full: the link is dropped */
}

static unsigned char inv_cur_get(unsigned char idx)
{ unsigned char i = inv_find(idx); return (i == 0xFF) ? 0xFF : inv_cur_v[i]; }
static unsigned char inv_next_get(unsigned char idx)
{ unsigned char i = inv_find(idx); return (i == 0xFF) ? 0xFF : inv_next_v[i]; }
static unsigned char inv_ref_get(unsigned char idx)
{ unsigned char i = inv_find(idx); return (i == 0xFF) ? 0xFF : inv_ref_v[i]; }
static void inv_cur_set(unsigned char idx, unsigned char v)
{ unsigned char i = inv_make(idx); if (i != 0xFF) inv_cur_v[i] = v; }
static void inv_next_set(unsigned char idx, unsigned char v)
{ unsigned char i = inv_make(idx); if (i != 0xFF) inv_next_v[i] = v; }
static void inv_ref_set(unsigned char idx, unsigned char v)
{ unsigned char i = inv_make(idx); if (i != 0xFF) inv_ref_v[i] = v; }

/* inventory list helpers */
static unsigned char inv_prev_item(unsigned char pge, unsigned char last)
{
    if (bad_pge(pge)) return pge;
    unsigned char di = pge, n = inv_cur_get(pge), guard = 0;
    while (n != 0xFF && guard++ < MAX_PGE) {
        if (n == last) break;
        di = n;
        n = inv_next_get(di);
    }
    return di;
}

static void inv_remove(unsigned char p1, unsigned char p2, unsigned char p3)
{
    if (bad_pge(p1) || bad_pge(p2) || bad_pge(p3)) return;
    inv_ref_set(p2, 0xFF);
    if (p3 == p1) {
        inv_cur_set(p3, inv_next_get(p2));
    } else {
        inv_next_set(p1, inv_next_get(p2));
    }
    inv_next_set(p2, 0xFF);
}

static void inv_update(unsigned char p1, unsigned char p2)
{
    unsigned char ax;
    if (bad_pge(p1) || bad_pge(p2)) return;
#if TEST_NO_INVENTORY
    return;                 /* test build: does the pickup still freeze? */
#endif
    if (inv_ref_get(p2) != 0xFF) {       /* reorder */
        unsigned char bx = inv_ref_get(p2);
        unsigned char di = inv_prev_item(bx, p2);
        if (di == bx) {
            if (inv_cur_get(di) == p2) inv_remove(di, p2, bx);
        } else {
            if (inv_next_get(di) == p2) inv_remove(di, p2, bx);
        }
    }
    ax = inv_prev_item(p1, 0xFF);
    inv_ref_set(p2, p1);
    if (ax == p1) {
        inv_next_set(p2, inv_cur_get(ax));
        inv_cur_set(ax, p2);
    } else {
        inv_next_set(p2, inv_next_get(ax));
        inv_next_set(ax, p2);
    }
}

/* The collision tests the lift and machinery opcodes need (modes 3..6):
 * rarer than modes 0..2, so they stay in C.  Same walk as col_test below;
 * mode 5 = by animation y among objects of type b_arg (the engine does NOT
 * skip the object itself here), 6 = anything that is not me, 3/4 = message
 * each object facing the same (3) or the other (4) way. */
static int col_test_ext(LivePGE *pge, int num, unsigned char mode, int b_arg)
{
    unsigned char slot = pge->collision_slot, slot_bak, cs;
    unsigned char other, guard = 0, guard2;
    while (slot != 0xFF && guard++ < COL_SLOTS) {
        if (slot >= COL_SLOTS) return 0;
        cs = col_table[slot];
        slot_bak = slot;
        slot = 0xFF;
        guard2 = 0;
        while (cs != 0xFF && guard2++ < COL_SLOTS) {
            other = col_live[cs];
            if (mode == 5) {
                if (init_field8(other, 18) == (unsigned char)b_arg) {
                    const unsigned char *rec = map_ani(pge_live[other].obj_type);
                    if (rec[3] == (unsigned char)num) return 1;
                }
            } else if (mode == 6) {
                if (other != pge->index) return 1;
            } else if (other != pge->index) {
                unsigned char same = ((pge_live[other].flags & 1) == (pge->flags & 1));
                if (same == (mode == 3)) {
                    s_compare_var1 = 1;
                    /* the engine sends this FROM the object running the
                     * test TO the one it collided with, not the reverse */
                    msg_send(pge->index, other, (unsigned char)num);
                }
            }
            if (other == pge->index) slot = col_index[cs];
            cs = col_prev[cs];
            if (slot == slot_bak) return 0;
        }
    }
    return 0;
}

/* col_test (the engine's collision-test callbacks): walk the objects that
 * share any of this object's cells.  ct_mode 0 = one whose animation frame
 * has y == num, 1 = one of object type num (10 = a live monster), 2 = send
 * message num to each of them.  In asm, loop state in statics; the C:
 *   slot = collision_slot
 *   while (slot != 0xFF && 160 iterations) {
 *       if (slot >= 160) return 0
 *       cs = col_table[slot]; bak = slot; slot = 0xFF
 *       while (cs != 0xFF && 160 iterations) {
 *           other = col_live[cs]; mode test (may return 1)
 *           if (other == index) slot = col_index[cs]
 *           cs = col_prev[cs]; if (slot == bak) return 0
 *       }
 *   }
 *   return 0                                                              */
static unsigned char ct_mode, ct_idx, ct_slot, ct_bak, ct_cs, ct_other, ct_g1, ct_g2;
static int ct_num;

static void col_test_send(void)
{
    msg_send(ct_idx, ct_other, (unsigned char)ct_num);
    s_compare_var1 = 0xFFFF;
}

static int col_test(LivePGE *pge, int num) __naked
{
    (void)pge; (void)num;
    __asm
    ld   (_ct_num), de
    ld   de, #16
    add  hl, de
    ld   a, (hl)
    ld   (_ct_slot), a          ; collision_slot
    inc  hl
    inc  hl
    ld   a, (hl)
    ld   (_ct_idx), a           ; index
    ld   a, #160
    ld   (_ct_g1), a
00001$:                         ; per cell of this object
    ld   a, (_ct_slot)
    cp   a, #0xff
    jp   z, 00090$
    ld   hl, #_ct_g1
    ld   b, a
    ld   a, (hl)
    or   a, a
    jp   z, 00090$
    dec  (hl)
    ld   a, b
    cp   a, #160
    jp   nc, 00090$
    ld   (_ct_bak), a
    ld   e, a
    ld   d, #0
    ld   hl, #_col_table
    add  hl, de
    ld   a, (hl)
    ld   (_ct_cs), a
    ld   a, #0xff
    ld   (_ct_slot), a
    ld   a, #160
    ld   (_ct_g2), a
00002$:                         ; per object in that cell
    ld   a, (_ct_cs)
    cp   a, #0xff
    jr   z, 00001$
    ld   hl, #_ct_g2
    ld   b, a
    ld   a, (hl)
    or   a, a
    jr   z, 00001$
    dec  (hl)
    ld   e, b
    ld   d, #0
    ld   hl, #_col_live
    add  hl, de
    ld   a, (hl)
    ld   (_ct_other), a
    ld   a, (_ct_mode)
    or   a, a
    jr   nz, 00010$
    ld   a, (_ct_other)         ; mode 0: by animation y
    ld   b, a
    ld   a, (_ct_idx)
    cp   a, b
    jr   z, 00030$
    ld   l, b
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
    ld   a, (hl)
    inc  hl
    ld   h, (hl)
    ld   l, a                   ; obj_type
    call _map_ani
    ld   hl, #3
    add  hl, de
    ld   a, (_ct_num)
    cp   a, (hl)
    jp   z, 00091$
    jr   00030$
00010$:
    dec  a
    jr   nz, 00020$
    ld   a, (_ct_other)         ; mode 1: by object type
    ld   l, #18
    call _init_field8
    ld   c, a
    ld   hl, (_ct_num)
    ld   a, h
    or   a, a
    jr   nz, 00012$
    ld   a, l
    cp   a, #10
    jr   nz, 00012$
    ld   a, c                   ; num 10: a monster that is still alive
    cp   a, #10
    jr   nz, 00030$
    ld   a, (_ct_other)
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
    ld   de, #(_pge_live + 11)
    add  hl, de
    bit  7, (hl)                ; life < 0
    jp   z, 00091$
    jr   00030$
00012$:
    ld   a, (_ct_num)
    cp   a, c
    jp   z, 00091$
    jr   00030$
00020$:                         ; mode 2: message every other object
    ld   a, (_ct_other)
    ld   b, a
    ld   a, (_ct_idx)
    cp   a, b
    jr   z, 00030$
    call _col_test_send
00030$:
    ld   a, (_ct_other)
    ld   b, a
    ld   a, (_ct_idx)
    cp   a, b
    jr   nz, 00031$
    ld   a, (_ct_cs)            ; our own slot: its next cell
    ld   e, a
    ld   d, #0
    ld   hl, #_col_index
    add  hl, de
    ld   a, (hl)
    ld   (_ct_slot), a
00031$:
    ld   a, (_ct_cs)
    ld   e, a
    ld   d, #0
    ld   hl, #_col_prev
    add  hl, de
    ld   a, (hl)
    ld   (_ct_cs), a
    ld   a, (_ct_bak)
    ld   b, a
    ld   a, (_ct_slot)
    cp   a, b
    jr   z, 00090$
    jp   00002$
00090$:
    ld   de, #0
    ret
00091$:
    ld   de, #1
    ret
    __endasm;
}

/* An object record is read in place from ROM rather than copied: most records
 * fail their first condition, so copying all 18 bytes was mostly wasted.
 * The Z80 is little-endian with no alignment rules, so words load directly. */
#define OB_TYPE(p)      (*(const unsigned int *)(p))
#define OB_DX(p)        ((signed char)(p)[2])
#define OB_DY(p)        ((signed char)(p)[3])
#define OB_INIT_TYPE(p) (*(const unsigned int *)((p) + 4))
#define OB_OP2(p)       ((p)[6])
#define OB_OP1(p)       ((p)[7])
#define OB_FLAGS(p)     ((p)[8])
#define OB_OP3(p)       ((p)[9])
#define OB_INIT_NUM(p)  (*(const unsigned int *)((p) + 10))
#define OB_ARG1(p)      (*(const int *)((p) + 12))
#define OB_ARG2(p)      (*(const int *)((p) + 14))
#define OB_ARG3(p)      (*(const int *)((p) + 16))

static unsigned int rd16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* Script walks read consecutive records, so keep the position of the last one
 * and step from it instead of recomputing bank and offset (a 16-bit multiply). */
static unsigned int  ro_n, ro_k;
static unsigned char ro_bank, ro_valid;
static const unsigned char *ro_p;

/* maps the record's bank (left in ro_bank) and returns its address */
static const unsigned char *object_ptr(unsigned int n)
{
    if (ro_valid && n == ro_n + 1) {
        if (++ro_k == OBJECTS_PER_BANK) { ro_k = 0; ro_bank++; ro_p = (const unsigned char *)0x8000; }
        else ro_p += OBJECT_SIZE;
    } else if (!ro_valid || n != ro_n) {
        unsigned int k = n;
        unsigned char bank = s_bank_obj;
        while (k >= OBJECTS_PER_BANK) { k -= OBJECTS_PER_BANK; bank++; }
        ro_k = k;
        ro_bank = bank;
        ro_p = (const unsigned char *)(0x8000 + k * OBJECT_SIZE);
        ro_valid = 1;
    }
    ro_n = n;
    SMS_mapROMBank(ro_bank);
    return ro_p;
}

/* an object's InitPGE field (part A bank) */
static unsigned int init_field16(unsigned char idx, unsigned char off)
{
    SMS_mapROMBank(s_bank_a);
    return *(const unsigned int *)(0x8002 + (((unsigned int)idx << 5) - idx) + off);
}

static unsigned char init_field8(unsigned char idx, unsigned char off)
{
    SMS_mapROMBank(s_bank_a);
    return *(const unsigned char *)(0x8002 + (((unsigned int)idx << 5) - idx) + off);
}

/* animation record for an object type: header + the frame for a sequence */
static const unsigned char *map_ani(unsigned int obj_type)
{
    const unsigned char *e;
    unsigned char bank;
    const unsigned char *rec;
    SMS_mapROMBank(s_bank_aniidx);
    e = (const unsigned char *)(0x8000 + (obj_type << 1) + obj_type);
    bank = e[0];
    rec = (const unsigned char *)*(const unsigned int *)(e + 1);
    SMS_mapROMBank(s_bank_ani + bank);
    return rec;
}

static unsigned int rnd(void)
{
    unsigned long n = s_rand * 2;
    if (!(s_rand & 0x80000000UL)) n ^= 0x1D872B41UL;
    s_rand = n;
    return (unsigned int)(n & 0xFFFF);
}

/* ------------------------------------------------------------- messages -- */
static void msg_clear(unsigned char idx)
{
    unsigned char e = msg_head[idx], nx;
    if (e == 0xFF) return;
    msg_head[idx] = 0xFF;
    while (e != 0xFF) {
        nx = msg_next[e];
        msg_next[e] = msg_free;
        msg_src[e] = 0;
        msg_num[e] = 0;
        msg_free = e;
        e = nx;
    }
}

static void msg_send(unsigned char src, unsigned char dst, unsigned char num)
{
    unsigned char e;
    LivePGE *pge;
    if (bad_pge(src) || bad_pge(dst)) return;
    pge = &pge_live[dst];
    if (!(pge->flags & 4)) {
        if (!(init_field8(dst, I_FLAGS) & 1)) return;
        pge->flags |= 4;
    }
    if (num <= 4 && pge->room_location != pge_live[src].room_location) return;
    if (msg_free == 0xFF) return;                 /* pool exhausted */
    e = msg_free;
    msg_free = msg_next[e];
    msg_next[e] = msg_head[dst];
    msg_src[e] = src;
    msg_num[e] = num;
    msg_head[dst] = e;
}

/* ------------------------------------------------------------- opcodes --- */
static const unsigned char mod_keys[3] = { 0x40, 0x10, 0x20 };

/* Can this object still react to that message?  Its script is scanned from
 * its current entry point for a condition that tests for the message. */
static unsigned int col_hit_helper(unsigned char other, int msg_num)
{
    unsigned int node, first, count, i;
    const unsigned char *ob;
    node = init_field16(other, I_NODE);
    SMS_mapROMBank(s_bank_a);
    first = *(const unsigned int *)(s_node_tab + node * 2);
    count = *(const unsigned int *)(s_node_tab + 512 + node * 2);
    i = pge_live[other].first_obj;
    while (i < count) {
        ob = object_ptr(first + i);
        if (OB_TYPE(ob) != pge_live[other].obj_type) break;
        if (OB_OP2(ob) == 0x6B) {
            if (OB_ARG2(ob) == 0 && (msg_num == 1 || msg_num == 2)) return 0xFFFF;
            if (OB_ARG2(ob) == 1 && (msg_num == 3 || msg_num == 4)) return 0xFFFF;
        } else if (OB_OP2(ob) == 0x22 && OB_ARG2(ob) == msg_num) return 0xFFFF;
        if (OB_OP1(ob) == 0x6B) {
            if (OB_ARG1(ob) == 0 && (msg_num == 1 || msg_num == 2)) return 0xFFFF;
            if (OB_ARG1(ob) == 1 && (msg_num == 3 || msg_num == 4)) return 0xFFFF;
        } else if (OB_OP1(ob) == 0x22 && OB_ARG1(ob) == msg_num) return 0xFFFF;
        ++i;
    }
    return 0;
}

/* col_detectHit(): sweep the grid cells in front of (or behind) the object and
 * score the objects of the wanted type found there.  `same_dir` picks the
 * engine's callback 2 (same facing) or 3 (opposite facing). */
static int col_detect_hit(LivePGE *pge, int msg_num, int object_type,
                          unsigned char same_dir, int arg_a)
{
    int pos_dx, pos_dy, var8, varA, thr, gx, gy, score = 0;
    signed char room = (signed char)pge->room_location;
    int slot;
    unsigned char cs, other, hguard;
    if (room < 0 || room >= 0x40) return 0;
    thr = (int)init_field16(pge->index, I_DATA);
    if (thr > 0) { pos_dx = -1; pos_dy = -1; }
    else         { pos_dx = 1;  pos_dy = 1; thr = -thr; }
    if (s_facing) pos_dx = -pos_dx;
    gx = (pge->pos_x + 8) >> 4;
    gy = div72(pge->pos_y);
    if (gy < 0 || gy > 2) return 0;
    gy *= 16;
    var8 = 0;
    varA = 0;
    if (arg_a != 0) { var8 = pos_dy; gx += pos_dx; varA = 1; }
    while (varA <= thr) {
        if (gx < 0)   { room = ct_s(CT_LEFT + room);  if (room < 0) break; gx += 16; }
        if (gx >= 16) { room = ct_s(CT_RIGHT + room); if (room < 0) break; gx -= 16; }
        slot = col_find_slot((unsigned int)(gy + gx + room * 64));
        if (slot >= 0) {
            cs = col_table[slot];
            hguard = 0;
            while (cs != 0xFF && hguard++ < COL_SLOTS) {
                other = col_live[cs];
                if (other != pge->index && (pge_live[other].flags & 4) &&
                    init_field8(other, 18) == (unsigned char)object_type) {
                    unsigned char same = ((pge_live[other].flags & 1) == (pge->flags & 1));
                    if (same == same_dir && col_hit_helper(other, msg_num) == 0) score += 1;
                }
                cs = col_prev[cs];
            }
        }
        if (col_get_grid_data(pge, 1, var8) != 0) break;
        gx += pos_dx;
        ++varA;
        var8 += pos_dy;
    }
    return score;
}

/* col_detectGunHit(): a shot sweeps the cells in front of the shooter until it
 * meets solid ground or something it can hit; whatever it hits is sent the
 * message that matches the shooter's facing. */
static int col_detect_gun_hit(LivePGE *pge, int arg2, int arg4, unsigned char with_destructible)
{
    int pos_dx, pos_dy, var8, varA, thr, gx, gy, r;
    signed char room = (signed char)pge->room_location;
    int slot;
    unsigned char cs, other, id, ot, hguard;
    if (room < 0 || room >= 0x40) return 0;
    thr = (int)init_field16(pge->index, I_DATA);
    if (thr > 0) { pos_dx = -1; pos_dy = -1; }
    else         { pos_dx = 1;  pos_dy = 1; thr = -thr; }
    if (s_facing) pos_dx = -pos_dx;
    gx = (pge->pos_x + 8) >> 4;
    gy = div72(pge->pos_y - 8);
    if (gy < 0 || gy > 2) return 0;
    gy *= 16;
    var8 = pos_dy;                 /* the engine calls this with argA = 1 */
    varA = 1;
    gx += pos_dx;
    while (varA <= thr) {
        if (gx < 0)    { room = ct_s(CT_LEFT + room);  if (room < 0) return 0; gx += 16; }
        if (gx >= 16)  { room = ct_s(CT_RIGHT + room); if (room < 0) return 0; gx -= 16; }
        slot = col_find_slot((unsigned int)(room * 64 + gx + gy));
        if (slot >= 0) {
            cs = col_table[slot];
            hguard = 0;
            while (cs != 0xFF && hguard++ < COL_SLOTS) {
                other = col_live[cs];
                if (other != pge->index && (pge_live[other].flags & 4)) {
                    ot = init_field8(other, 18);
                    if (ot == 1 || ot == 10 || (with_destructible && ot == 12)) {
                        if ((pge_live[other].flags & 1) != (pge->flags & 1)) id = (arg4 == 0) ? 3 : 4;
                        else                                                id = (arg4 == 0) ? 1 : 2;
                        if (col_hit_helper(other, id) != 0) {
                            msg_send(pge->index, other, id);
                            return 1;
                        }
                    }
                }
                cs = col_prev[cs];
            }
        }
        r = col_get_grid_data(pge, 1, var8);
        if (r != 0 && (!(r & 2) || arg2 != 1)) break;
        gx += pos_dx;
        ++varA;
        var8 += pos_dy;
    }
    return 0;
}

/* pge_op_dropObject(): put the object where the other one is, facing the same
 * way, and take it out of whatever inventory holds it */
static void do_drop(LivePGE *pge, unsigned char src_index)
{
    LivePGE *src;
    if (bad_pge(src_index)) return;
    src = &pge_live[src_index];
    pge->pos_x = src->pos_x;
    pge->pos_y = src->pos_y;
    pge->room_location = src->room_location;
    pge->flags &= ~1;
    if (src->flags & 1) pge->flags |= 1;
    if (inv_ref_get(pge->index) != 0xFF) {
        unsigned char bx = inv_ref_get(pge->index);
        unsigned char di = inv_prev_item(bx, pge->index);
        if (di == bx) {
            if (inv_cur_get(di) == pge->index) inv_remove(di, pge->index, bx);
        } else {
            if (inv_next_get(di) == pge->index) inv_remove(di, pge->index, bx);
        }
    }
}

/* returns the opcode result; sets logic_bad_op for anything not ported */
static int exec_op(unsigned char op, LivePGE *pge, int a, int b)
{
    unsigned char e;
    switch (op) {
    /* input tests: the pad mask must equal the pattern exactly.  "forward" and
     * "backward" depend on which way the object is facing. */
    case 0x01: return (s_inp == 1) ? 0xFFFF : 0;                      /* up */
    case 0x02: return (s_inp == (s_facing ? 4 : 8)) ? 0xFFFF : 0;     /* backward */
    case 0x03: return (s_inp == 2) ? 0xFFFF : 0;                      /* down */
    case 0x04: return (s_inp == (s_facing ? 8 : 4)) ? 0xFFFF : 0;     /* forward */
    case 0x05: return (s_inp == (mod_keys[a & 3] | 1)) ? 0xFFFF : 0;
    case 0x06: return (s_inp == (mod_keys[a & 3] | (s_facing ? 4 : 8))) ? 0xFFFF : 0;
    case 0x07: return (s_inp == (mod_keys[a & 3] | 2)) ? 0xFFFF : 0;
    case 0x08: return (s_inp == (mod_keys[a & 3] | (s_facing ? 8 : 4))) ? 0xFFFF : 0;
    case 0x09: return (s_inp == 0) ? 0xFFFF : 0;                      /* idle */
    case 0x35: return (s_inp == mod_keys[a & 3]) ? 0xFFFF : 0;        /* modifier alone */
    case 0x0A:                                    /* isInpNoMod */
        return (((s_inp & 0x0F) | mod_keys[a & 3]) == s_inp) ? 0xFFFF : 0;
    case 0x22:                                    /* hasPiegeSentMessage */
        for (e = msg_head[pge->index]; e != 0xFF; e = msg_next[e])
            if (msg_num[e] == a) return 0xFFFF;
        return 0;
    case 0x23:                                    /* sendMessageData0 */
        msg_send(pge->index, (unsigned char)init_field16(pge->index, I_DATA), (unsigned char)a);
        return 0xFFFF;
    case 0x3E:                                    /* setPiegeCounter */
        pge->counter_value = a;
        return 1;
    case 0x3F:                                    /* decPiegeCounter */
        pge->counter_value -= 1;
        return (a == pge->counter_value) ? 0xFFFF : 0;
    case 0x44:                                    /* loadPiegeCounter */
        pge->counter_value = (int)init_field16(pge->index, I_DATA + 2 * (a & 3));
        return 1;
    case 0x4B:                                    /* killPiege */
        pge->room_location = 0xFE;
        pge->flags &= ~4;
        return 0xFFFF;
    case 0x4C:                                    /* isInCurrentRoom */
        return (pge->room_location == s_room) ? 1 : 0;
    case 0x4D:                                    /* isNotInCurrentRoom */
        return (pge->room_location == s_room) ? 0 : 1;
    case 0x59:                                    /* decLifeCounter */
        if (bad_pge((unsigned char)a)) return 1;
        pge->life = pge_live[a].life - 1;
        return 1;
    case 0x61:                                    /* isInRandomRange */
        if (a != 0 && (rnd() % (unsigned int)a) == 0) return 1;
        return 0;
    case 0x6B:                                    /* isMessageReceived */
        for (e = msg_head[pge->index]; e != 0xFF; e = msg_next[e]) {
            if (a == 0) { if (msg_num[e] == 1 || msg_num[e] == 2) return 1; }
            else        { if (msg_num[e] == 3 || msg_num[e] == 4) return 1; }
        }
        return 0;
    case 0x6F:                                    /* testAndAckMessage */
        for (e = msg_head[pge->index]; e != 0xFF; e = msg_next[e])
            if (msg_num[e] == a) { msg_send(pge->index, msg_src[e], 12); return 1; }
        return 0;
    /* the grid-collision family: read the collision cell dy rows away and
     * dx columns to the side, either as a value or as a yes/no test */
    case 0x0B: return col_get_grid_data(pge, 0, -a);
    case 0x0C: return col_get_grid_data(pge, 0, 0);
    case 0x0D: return col_get_grid_data(pge, 0, a);
    case 0x0E: return col_get_grid_data(pge, 1, -a);
    case 0x0F: return col_get_grid_data(pge, 1, 0);
    case 0x10: return col_get_grid_data(pge, 1, a);
    case 0x11: return col_get_grid_data(pge, 2, -a);
    case 0x12: return col_get_grid_data(pge, 2, 0);
    case 0x13: return col_get_grid_data(pge, 2, a);
    case 0x14: return col_get_grid_data(pge, 0, -a) ? 0 : 0xFFFF;
    case 0x15: return col_get_grid_data(pge, 0, 0) ? 0 : 0xFFFF;
    case 0x16: return col_get_grid_data(pge, 0, a) ? 0 : 0xFFFF;
    case 0x17: return col_get_grid_data(pge, 1, -a) ? 0 : 0xFFFF;
    case 0x18: return col_get_grid_data(pge, 1, 0) ? 0 : 0xFFFF;
    case 0x19: return col_get_grid_data(pge, 1, a) ? 0 : 0xFFFF;
    case 0x1A: return col_get_grid_data(pge, 2, -a) ? 0 : 0xFFFF;
    case 0x1B: return col_get_grid_data(pge, 2, 0) ? 0 : 0xFFFF;
    case 0x1C: return col_get_grid_data(pge, 2, a) ? 0 : 0xFFFF;
    /* ledges and steps: something here, nothing beyond, nothing above */
    case 0x1D: return (col_get_grid_data(pge, 0, a) && !col_get_grid_data(pge, 0, a + 1)
                       && !col_get_grid_data(pge, -1, a)) ? 0xFFFF : 0;
    case 0x1E: return (col_get_grid_data(pge, 2, a) && !col_get_grid_data(pge, 2, a + 1)
                       && !col_get_grid_data(pge, 1, a)) ? 0xFFFF : 0;
    case 0x1F: return (col_get_grid_data(pge, 0, a) && !col_get_grid_data(pge, 0, a - 1)
                       && !col_get_grid_data(pge, -1, a)) ? 0xFFFF : 0;
    case 0x20: return (col_get_grid_data(pge, 2, a) && !col_get_grid_data(pge, 2, a - 1)
                       && !col_get_grid_data(pge, 1, a)) ? 0xFFFF : 0;
    case 0x21: return (col_get_grid_data(pge, 2, a - 1) && !col_get_grid_data(pge, 2, a)
                       && !col_get_grid_data(pge, 1, a - 1)) ? 0xFFFF : 0;
    case 0x28: return (col_get_grid_data(pge, 1, a - 1) && !col_get_grid_data(pge, 2, a)) ? 0xFFFF : 0;
    case 0x29: return (col_get_grid_data(pge, 1, a - 1) && !col_get_grid_data(pge, 1, a)) ? 0xFFFF : 0;
    case 0x2A: return (!col_get_grid_data(pge, 1, a - 1) && col_get_grid_data(pge, 1, a)) ? 0xFFFF : 0;
    case 0x24:                                    /* sendMessageData1..3 */
    case 0x25:
    case 0x26:
        msg_send(pge->index, (unsigned char)init_field16(pge->index, I_DATA + 2 * (op - 0x23)),
                 (unsigned char)a);
        return 0xFFFF;
    case 0x27:                                    /* isPiegeDead */
        return (pge->life <= 0) ? 1 : 0;
    case 0x2E:                                    /* nop */
        return 1;
    case 0x88:                                    /* adjustPos: snap to the grid */
        pge->pos_x &= 0xFFF0;
        if (pge->pos_y != 70 && pge->pos_y != 142 && pge->pos_y != 214)
            pge->pos_y = (div72(pge->pos_y) + 1) * 72 - 2;
        return 0xFFFF;
    case 0x8A:                                    /* setGunVar */
        s_gun_var = a;
        return 0xFFFF;
    case 0x8B:                                    /* compareGunVar */
        return (s_gun_var == a) ? 0xFFFF : 0;
    case 0x3D:                                    /* collide test by animation y */
        ct_mode = 0;
        return col_test(pge, a);
    case 0x50:                                    /* collide test by object type */
        ct_mode = 1;
        return col_test(pge, a);
    case 0x7E:                                    /* collide test by index */
        s_compare_var1 = 0;
        ct_mode = 2;
        col_test(pge, a);
        return (int)s_compare_var1;
    case 0x7F: {                                  /* nothing collectible in the way */
        unsigned char slot = pge->collision_slot, cs, other, g1 = 0, g2;
        while (slot != 0xFF && g1++ < COL_SLOTS) {
            if (slot >= COL_SLOTS) break;
            cs = col_table[slot];
            slot = 0xFF;
            g2 = 0;
            while (cs != 0xFF && g2++ < COL_SLOTS) {
                other = col_live[cs];
                if (other != pge->index) {
                    if (init_field8(other, 18) == 3 &&        /* collectible */
                        pge->index != inv_ref_get(other)) return 0;
                } else {
                    slot = col_index[cs];
                }
                cs = col_prev[cs];
            }
        }
        return 0xFFFF;
    }
    case 0x6D: {                                  /* is the thing we touch this item */
        unsigned char cs, other, icon = 0, icon_guard = 0;
        if (pge->collision_slot != 0xFF && pge->collision_slot < COL_SLOTS) {
            cs = col_table[pge->collision_slot];
            while (cs != 0xFF && icon_guard++ < COL_SLOTS) {
                other = col_live[cs];
                if (init_field8(other, 18) == 3) {     /* collectible */
                    icon = init_field8(other, 22);     /* colliding_icon_num */
                    break;
                }
                cs = col_prev[cs];
            }
        }
        return (icon == (unsigned char)a) ? 1 : 0;
    }
    case 0x6E:                                    /* enterInvMessage (elevator) */
        for (e = msg_head[pge->index]; e != 0xFF; e = msg_next[e])
            if (msg_num[e] == a) { inv_update(msg_src[e], pge->index); return 0xFFFF; }
        return 0;
    case 0x73: {                                  /* pick up what we collide with */
        unsigned char other = col_find_piege(pge, (unsigned int)a);
        if (other != 0xFF) { inv_update(other, pge->index); return 0xFFFF; }
        return 0;
    }
    case 0x2F: {                                  /* pickupObject */
        unsigned char other = col_find_piege(pge, 3);
        if (other != 0xFF) { msg_send(pge->index, other, (unsigned char)a); return 0xFFFF; }
        return 0;
    }
    case 0x4A: {                                  /* killInventoryPiege */
        unsigned char inv;
        pge->room_location = 0xFE;
        pge->flags &= ~4;
        if (bad_pge((unsigned char)a)) return 1;
        inv = inv_prev_item((unsigned char)a, pge->index);
        if (inv == (unsigned char)a) {
            if (pge->index != inv_cur_get(inv)) return 1;
        } else {
            if (pge->index != inv_next_get(inv)) return 1;
        }
        inv_remove(inv, pge->index, (unsigned char)a);
        return 1;
    }
    case 0x4E: {                                  /* scrollPosY: carry the inventory */
        unsigned char it = inv_cur_get(pge->index), g = 0;
        pge->pos_y += a;
        while (it != 0xFF && !bad_pge(it) && g++ < MAX_PGE) {
            pge_live[it].pos_y += a;
            it = inv_next_get(it);
        }
        return 1;
    }
    case 0x57:                                    /* setPiegeDefaultAnim */
        pge->room_location = (unsigned char)init_field16(pge->index, I_DATA + 2 * (a & 3));
        if (init_field8(pge->index, 18) == 1) s_load_map = 1;   /* Conrad */
        setup_default_anim(pge, s_bank_aniidx, s_bank_ani);
        return 1;
    case 0x66: {                                  /* subFromCredits */
        unsigned char who = (unsigned char)init_field16(pge->index, I_DATA + 2 * (a & 3));
        int val = (int)init_field16(pge->index, I_DATA + 2 * ((a & 3) + 1));
        if (!bad_pge(who)) pge_live[who].life -= val;
        return 1;
    }
    case 0x67:                                    /* is the ground ahead solid-ish */
        return (col_get_grid_data(pge, 1, -a) & 2) ? 0xFFFF : 0;
    case 0x71:                                    /* exitInvMessage */
        for (e = msg_head[pge->index]; e != 0xFF; e = msg_next[e]) {
            if (msg_num[e] == a) {
                unsigned char r = inv_ref_get(pge->index);
                if (r != 0xFF && !bad_pge(r)) {
                    unsigned char di = inv_prev_item(r, pge->index);
                    if (di == r) {
                        if (inv_cur_get(di) == pge->index) inv_remove(di, pge->index, r);
                    } else {
                        if (inv_next_get(di) == pge->index) inv_remove(di, pge->index, r);
                    }
                }
                return 1;
            }
        }
        return 0;
    case 0x4F:                                    /* death cutscene (none here) */
    case 0x7B:                                    /* displayText (no text yet) */
    case 0x86:
        return 1;
    case 0x30:                                    /* addItemToInventory */
        if (bad_pge((unsigned char)a)) return 0xFFFF;
        inv_update((unsigned char)a, pge->index);
        pge->room_location = 0xFF;
        return 0xFFFF;
    case 0x31:                                    /* dropObject */
        do_drop(pge, (unsigned char)a);
        return 0xFFFF;
    case 0x60:                                    /* locateMessage: drop where the sender is */
        for (e = msg_head[pge->index]; e != 0xFF; e = msg_next[e]) {
            if (msg_num[e] == a) { do_drop(pge, msg_src[e]); return 1; }
        }
        return 0;
    case 0x32:                                    /* removeItemFromInventory */
        if (inv_cur_get(pge->index) != 0xFF)
            msg_send(pge->index, inv_cur_get(pge->index), (unsigned char)a);
        return 1;
    case 0x33:                                    /* canUseCurrentInventoryItem */
        if (inv_cur_get(0) != 0xFF &&
            init_field8(inv_cur_get(0), 24) == (unsigned char)a) return 1;
        return 0;
    case 0x34:                                    /* use object in front */
        if (((s_inp & 0x0F) | mod_keys[0]) == s_inp && col_get_grid_data(pge, 2, -a) == 0)
            return 0xFFFF;
        return 0;
    case 0x36:                                    /* setCollisionState1 */
        return col_update_state(pge, a, 1);
    case 0x37:                                    /* setCollisionState0 */
        return col_update_state(pge, a, 0);
    case 0x43:                                    /* removePiegeIfNotNear */
        if ((init_field8(pge->index, I_FLAGS) & 4) && !(s_room & 0x80) &&
            pge->room_location < 0x40 &&
            (pge->room_location == s_room ||
             pge->room_location == (unsigned char)ct_s(CT_UP + s_room) ||
             pge->room_location == (unsigned char)ct_s(CT_DOWN + s_room) ||
             pge->room_location == (unsigned char)ct_s(CT_RIGHT + s_room) ||
             pge->room_location == (unsigned char)ct_s(CT_LEFT + s_room))) {
            return 1;                             /* near enough: keep it */
        }
        pge->flags &= ~4;
        pge->collision_slot = 0xFF;
        return 1;
    case 0x78:                                    /* testConradLeft */
    case 0x79: {                                  /* testConradRight */
        LivePGE *c = &pge_live[0];
        unsigned char right = (op == 0x79);
        int dx;
        if (div72(pge->pos_y) != div72(c->pos_y - 8)) return 0;
        if (pge->room_location == c->room_location) {
            if (a == 0) {
                if (right) {
                    if (s_facing) { if (pge->pos_x > c->pos_x) return 0xFFFF; }
                    else          { if (pge->pos_x <= c->pos_x) return 0xFFFF; }
                } else {
                    if (s_facing) { if (pge->pos_x < c->pos_x) return 0xFFFF; }
                    else          { if (pge->pos_x > c->pos_x) return 0xFFFF; }
                }
                return 0;
            }
            if (right) dx = s_facing ? (pge->pos_x - c->pos_x) : (c->pos_x - pge->pos_x);
            else       dx = s_facing ? (c->pos_x - pge->pos_x) : (pge->pos_x - c->pos_x);
            return (dx > 0 && dx < a * 16) ? 0xFFFF : 0;
        }
        if (a == 0 && pge->room_location < 0x40) {
            unsigned char link;
            if (right) link = s_facing ? CT_LEFT : CT_RIGHT;
            else       link = s_facing ? CT_RIGHT : CT_LEFT;
            if (c->room_location == (unsigned char)ct_s(link + pge->room_location)) return 0xFFFF;
        }
        return 0;
    }
    case 0x62:                                    /* hit test, opposite facing */
        return col_detect_hit(pge, a, b, 0, 0);
    case 0x63:                                    /* hit test, same facing */
        return col_detect_hit(pge, a, b, 1, 0);
    case 0x5A:                                    /* queue a cutscene */
        logic_cutscene = (unsigned int)a;
        return 1;
    case 0x5F: {                                  /* walk along the ground ahead */
        signed char room = (signed char)pge->room_location;
        int dx, cx, gx, gy, r;
        if (room < 0 || room >= 0x40) return 0;
        cx = (int)init_field16(pge->index, I_DATA);
        if (cx <= 0) { dx = 1; cx = -cx; } else dx = -1;
        if (s_facing) dx = -dx;
        gx = (pge->pos_x + 8) >> 4;
        gy = 0;
        do {
            r = col_get_grid_data(pge, 1, -gy);
            if (r != 0 && (!(r & 2) || a != 1)) {
                pge->room_location = (unsigned char)room;
                pge->pos_x = gx * 16;
                return 1;
            }
            if (gx < 0) {
                room = ct_s(CT_LEFT + room);
                if (room < 0 || room >= 0x40) return 0;
                gx += 16;
            } else if (gx > 15) {
                room = ct_s(CT_RIGHT + room);
                if (room < 0 || room >= 0x40) return 0;
                gx -= 16;
            }
            gx += dx;
            ++gy;
        } while (gy <= cx);
        return 0;
    }
    case 0x64:                                    /* gun shot */
        return col_detect_gun_hit(pge, a, b, 1);
    case 0x3C:                                    /* collide by animation Y, of a type */
        return col_test_ext(pge, a, 5, b) ? 1 : 0;
    case 0x45:                                    /* collide by object number */
        return col_test_ext(pge, a, 6, 0) ? 1 : 0;
    case 0x74:                                    /* collides4u */
        return col_get_grid_data(pge, 4, -a) ? 0xFFFF : 0;
    case 0x75:                                    /* doesNotCollide4u */
        return col_get_grid_data(pge, 4, -a) ? 0 : 0xFFFF;
    case 0x7A:                                    /* collides2u1u */
        if (col_get_grid_data(pge, 1, -a) == 0 && col_get_grid_data(pge, 2, -(a + 1)))
            return 0xFFFF;
        return 0;
    case 0x46:                                    /* collide, facing the other way */
        s_compare_var1 = 0;
        col_test_ext(pge, a, 4, 0);
        return s_compare_var1;
    case 0x47:                                    /* collide, facing the same way */
        s_compare_var1 = 0;
        col_test_ext(pge, a, 3, 0);
        return s_compare_var1;
    case 0x65: {                                  /* addToCredits */
        unsigned char who = (unsigned char)init_field16(pge->index, I_DATA + 2 * (a & 3));
        int val = (int)init_field16(pge->index, I_DATA + 2 * ((a & 3) + 1));
        if (!bad_pge(who)) pge_live[who].life += val;
        return 1;
    }
    case 0x68:                                    /* setCollisionState2 */
        return col_update_state(pge, (unsigned char)a, 2);
    case 0x76: {                                  /* isBelowConrad */
        signed char r = (signed char)pge->room_location;
        if (pge_live[0].room_location == pge->room_location)
            return (div72(pge_live[0].pos_y - 8) < div72(pge->pos_y)) ? 0xFFFF : 0;
        if (r >= 0 && r < 0x40 && pge_live[0].room_location == (unsigned char)ct_s(CT_UP + r))
            return 0xFFFF;
        return 0;
    }
    case 0x77: {                                  /* isAboveConrad */
        signed char r = (signed char)pge->room_location;
        if (pge_live[0].room_location == pge->room_location)
            return (div72(pge_live[0].pos_y - 8) > div72(pge->pos_y)) ? 0xFFFF : 0;
        if (r >= 0 && r < 0x40 && pge_live[0].room_location == (unsigned char)ct_s(CT_DOWN + r))
            return 0xFFFF;
        return 0;
    }
    case 0x83: {                                  /* hasInventoryItem */
        unsigned char it = inv_cur_get(0);
        while (it != 0xFF) {
            if (init_field8(it, 24) == (unsigned char)a) return 0xFFFF;   /* object_id */
            it = inv_next_get(it);
        }
        return 0;
    }
    case 0x7C: {                                  /* message whatever we touch */
        unsigned char other = col_find_piege(pge, 3);
        if (other == 0xFF) other = col_find_piege(pge, 5);
        if (other == 0xFF) other = col_find_piege(pge, 9);
        if (other == 0xFF) other = col_find_piege(pge, 0xFFFF);
        if (other != 0xFF) msg_send(pge->index, other, (unsigned char)a);
        return 0;
    }
    case 0x7D:                                    /* playSound */
        sfx_play((unsigned char)a);
        return 0xFFFF;
    case 0x87:                                    /* playSoundGroup */
        sfx_play((unsigned char)init_field16(pge->index, I_DATA + 2 * (a & 3)));
        return 0xFFFF;
    default:
        /* An opcode that is not ported yet stops the verification harness, so
         * the first divergence is reported.  While PLAYING it must not: the
         * flag used to be sticky, which halted every object from then on -
         * the game looked frozen although the machine was running fine. */
        if (!logic_bad_op) { logic_bad_op = op; logic_bad_op_frame = logic_frame; }
        if (!logic_check) logic_bad_op = 0;
        return 0;
    }
}

LAYOUT_CHECK(lp_first, __builtin_offsetof(LivePGE, first_obj) == 8);
LAYOUT_CHECK(lp_life,  __builtin_offsetof(LivePGE, life) == 10);

/* Run one script record `ob` (in ROM bank ex_bank) for `pge`: up to three
 * opcodes, each must succeed; then the record becomes the object's state.
 * An opcode may map other banks or walk another script, so the record's bank
 * is mapped again after each one.  Returns 0 (a condition failed, or an
 * opcode is not ported) or 0xFFFF.  In asm; the C it implements:
 *   if (op1 && (!(exec_op(op1, pge, arg1, 0) & 0xFF) || logic_bad_op)) return 0
 *   if (op2 && (!(exec_op(op2, pge, arg2, arg1) & 0xFF) || logic_bad_op)) return 0
 *   if (op3) { exec_op(op3, pge, arg3, 0); if (logic_bad_op) return 0; }
 *   obj_type = init_type; first_obj = init_num; anim_seq = 0
 *   fl = ob flags: 1 turn around, 2 life--, 4 life++, 8 life = -1
 *   pos_x += facing ? -dx : dx; pos_y += dy
 * (the engine passes the FIRST argument as op2's second parameter) */
static unsigned char ex_bank;
static const unsigned char *ex_ob;

static int pge_execute(LivePGE *pge, const unsigned char *ob) __naked
{
    (void)pge; (void)ob;
    __asm
    push ix
    push hl
    pop  ix                     ; ix = pge
    ld   (_ex_ob), de
    ex   de, hl
    ld   de, #7
    add  hl, de
    ld   a, (hl)                ; op1
    or   a, a
    jr   z, 00002$
    ld   de, #5
    add  hl, de                 ; ob + 12
    ld   c, (hl)
    inc  hl
    ld   b, (hl)                ; arg1
    ld   hl, #0
    push hl
    push bc
    push ix
    pop  de
    call _exec_op
    ld   a, e
    or   a, a
    jp   z, 00090$
    ld   a, (_logic_bad_op)
    or   a, a
    jp   nz, 00090$
    ld   a, (_ex_bank)
    ld   (_ROM_bank_to_be_mapped_on_slot2), a
00002$:
    ld   hl, (_ex_ob)
    ld   de, #6
    add  hl, de
    ld   a, (hl)                ; op2
    or   a, a
    jr   z, 00004$
    ld   de, #6
    add  hl, de                 ; ob + 12
    ld   c, (hl)
    inc  hl
    ld   b, (hl)                ; arg1
    inc  hl
    ld   e, (hl)
    inc  hl
    ld   d, (hl)                ; arg2
    push bc
    push de
    push ix
    pop  de
    call _exec_op
    ld   a, e
    or   a, a
    jp   z, 00090$
    ld   a, (_logic_bad_op)
    or   a, a
    jp   nz, 00090$
    ld   a, (_ex_bank)
    ld   (_ROM_bank_to_be_mapped_on_slot2), a
00004$:
    ld   hl, (_ex_ob)
    ld   de, #9
    add  hl, de
    ld   a, (hl)                ; op3
    or   a, a
    jr   z, 00006$
    ld   de, #7
    add  hl, de                 ; ob + 16
    ld   c, (hl)
    inc  hl
    ld   b, (hl)                ; arg3
    ld   hl, #0
    push hl
    push bc
    push ix
    pop  de
    call _exec_op
    ld   a, (_logic_bad_op)
    or   a, a
    jp   nz, 00090$
    ld   a, (_ex_bank)
    ld   (_ROM_bank_to_be_mapped_on_slot2), a
00006$:
    ld   hl, (_ex_ob)
    ld   de, #4
    add  hl, de
    ld   a, (hl)
    ld   0 (ix), a
    inc  hl
    ld   a, (hl)
    ld   1 (ix), a              ; obj_type = init_obj_type
    ld   de, #5
    add  hl, de                 ; ob + 10
    ld   a, (hl)
    ld   8 (ix), a
    inc  hl
    ld   a, (hl)
    ld   9 (ix), a              ; first_obj = init_obj_number
    ld   14 (ix), #0            ; anim_seq
    ld   hl, (_ex_ob)
    ld   de, #8
    add  hl, de
    ld   c, (hl)                ; record flags
    bit  0, c
    jr   z, 00010$
    ld   a, 17 (ix)
    xor  a, #1
    ld   17 (ix), a
00010$:
    ld   l, 10 (ix)
    ld   h, 11 (ix)             ; life
    bit  1, c
    jr   z, 00011$
    dec  hl
00011$:
    bit  2, c
    jr   z, 00012$
    inc  hl
00012$:
    bit  3, c
    jr   z, 00013$
    ld   hl, #0xffff
00013$:
    ld   10 (ix), l
    ld   11 (ix), h
    ld   hl, (_ex_ob)
    inc  hl
    inc  hl
    ld   a, (hl)                ; dx
    ld   e, a
    rlca
    sbc  a, a
    ld   d, a
    inc  hl
    ld   c, (hl)                ; dy
    ld   l, 2 (ix)
    ld   h, 3 (ix)
    bit  0, 17 (ix)
    jr   z, 00014$
    or   a, a
    sbc  hl, de
    jr   00015$
00014$:
    add  hl, de
00015$:
    ld   2 (ix), l
    ld   3 (ix), h
    ld   a, c
    ld   e, a
    rlca
    sbc  a, a
    ld   d, a
    ld   l, 4 (ix)
    ld   h, 5 (ix)
    add  hl, de
    ld   4 (ix), l
    ld   5 (ix), h
    ld   de, #0xffff
    pop  ix
    ret
00090$:
    ld   de, #0
    pop  ix
    ret
    __endasm;
}

/* In asm: SDCC's version was ~150 instructions.  The C it implements:
 *   rec = map_ani(obj_type); if (rd16(rec) < anim_seq) anim_seq = 0;
 *   fr = rec + 6 + anim_seq * 4; fr0 = rd16(fr); if (fr0 == 0xFFFF) return;
 *   fl = fr0 ^ (facing ? 0x8000 : 0); pos_x += facing ? -dx : dx; pos_y += dy;
 *   flags bit 1 = fl bit 15, bit 3 = (rd16(rec + 4) != 0);
 *   anim_number = fr0 & 0x7FFF;          (dx, dy = signed fr[2], fr[3]) */
static void pge_setup_anim(LivePGE *pge) __naked
{
    (void)pge;
    __asm
    push ix
    push hl
    pop  ix                     ; ix = pge
    ld   l, 0 (ix)
    ld   h, 1 (ix)
    call _map_ani               ; de = rec (bank mapped)
    ex   de, hl                 ; hl = rec
    inc  hl
    ld   a, (hl)                ; sequence count, high byte
    dec  hl
    or   a, a
    jr   nz, 00001$             ; count >= 256 > anim_seq
    ld   a, (hl)
    cp   a, 14 (ix)
    jr   nc, 00001$             ; count >= anim_seq
    ld   14 (ix), #0
00001$:
    push hl                     ; rec
    ld   e, 14 (ix)
    ld   d, #0
    ex   de, hl
    add  hl, hl
    add  hl, hl
    add  hl, de
    ld   de, #6
    add  hl, de                 ; hl = fr
    ld   c, (hl)
    inc  hl
    ld   b, (hl)                ; bc = fr0
    inc  hl
    ld   a, c
    and  a, b
    inc  a
    jr   z, 00009$              ; fr0 == 0xFFFF
    ld   e, (hl)                ; dx
    inc  hl
    ld   d, (hl)                ; dy
    ld   6 (ix), c              ; anim_number = fr0 & 0x7FFF
    ld   a, b
    and  a, #0x7f
    ld   7 (ix), a
    ld   a, e                   ; hl = (int)dx
    ld   l, a
    rlca
    sbc  a, a
    ld   h, a
    bit  0, 17 (ix)
    jr   z, 00002$
    ld   a, b                   ; facing: fl ^= 0x8000, pos_x -= dx
    xor  a, #0x80
    ld   b, a
    ld   a, 2 (ix)
    sub  a, l
    ld   2 (ix), a
    ld   a, 3 (ix)
    sbc  a, h
    ld   3 (ix), a
    jr   00003$
00002$:
    ld   a, 2 (ix)
    add  a, l
    ld   2 (ix), a
    ld   a, 3 (ix)
    adc  a, h
    ld   3 (ix), a
00003$:
    ld   a, d                   ; pos_y += dy
    ld   l, a
    rlca
    sbc  a, a
    ld   h, a
    ld   a, 4 (ix)
    add  a, l
    ld   4 (ix), a
    ld   a, 5 (ix)
    adc  a, h
    ld   5 (ix), a
    ld   a, 17 (ix)
    and  a, #0xf5
    bit  7, b
    jr   z, 00004$
    or   a, #0x02
00004$:
    ld   c, a
    pop  hl                     ; rec
    inc  hl
    inc  hl
    inc  hl
    inc  hl
    ld   a, (hl)
    inc  hl
    or   a, (hl)
    ld   a, c
    jr   z, 00005$
    or   a, #0x08
00005$:
    ld   17 (ix), a
    pop  ix
    ret
00009$:
    pop  hl
    pop  ix
    ret
    __endasm;
}

/* pge_addToCurrentRoomList(): move an object between the per-room lists.
 * in_list[] records which list each object is actually on: some opcodes set
 * room_location directly (picking an item up, dropping it), so the object's
 * room and its list can disagree.  Removing by the recorded list instead of
 * the assumed one keeps an object from ending up in two lists, which made a
 * cycle and hung the frame that walked it. */
static void room_list_move(unsigned char idx, unsigned char from_room)
{
    unsigned char cur, prev = 0xFF, guard = 0;
    unsigned char to_room = pge_live[idx].room_location;
    unsigned char on = in_list[idx];
    (void)from_room;
    if (on == to_room) return;
    if (on < 64) {                              /* unlink from its real list */
        cur = room_head[on];
        while (cur != 0xFF && cur != idx && guard++ < MAX_PGE) {
            prev = cur;
            cur = next_in_room[cur];
        }
        if (cur == idx) {
            if (prev == 0xFF) room_head[on] = next_in_room[idx];
            else next_in_room[prev] = next_in_room[idx];
        }
    }
    next_in_room[idx] = 0xFF;
    in_list[idx] = 0xFF;
    if (to_room < 64) {
        next_in_room[idx] = room_head[to_room];
        room_head[to_room] = idx;
        in_list[idx] = to_room;
    }
}

/* activate the objects of a room that ask for it */
static void activate_room(unsigned char room, unsigned char monsters_too, int min_y)
{
    unsigned char it;
    if (room >= 0x40) return;
    for (it = room_head[room]; it != 0xFF && !bad_pge(it); it = next_in_room[it]) {
        if (!(init_field8(it, I_FLAGS) & 4)) continue;
        if (!monsters_too) {
            if (init_field8(it, 18) == 10) continue;          /* monster */
            if (pge_live[it].pos_y < min_y) continue;
        }
        pge_live[it].flags |= 4;
    }
}

/* pge_setupOtherPieges(): wrap an object across a room edge and, for Conrad,
 * make that room the current one */
static void pge_setup_other_pieges(LivePGE *pge)
{
    unsigned char dir = 0xFF;                 /* which CT link to follow */
    signed char room;
    unsigned char from_room = s_pge_room;
    if (pge->pos_x <= -10)      { pge->pos_x += 256; dir = CT_LEFT; }
    else if (pge->pos_x >= 256) { pge->pos_x -= 256; dir = CT_RIGHT; }
    else if (pge->pos_y < 0)    { pge->pos_y += 216; dir = CT_UP; }
    else if (pge->pos_y >= 216) { pge->pos_y -= 216; dir = CT_DOWN; }
    if (dir != 0xFF) {
        room = (signed char)pge->room_location;
        if (room >= 0) {
            room = ct_s(dir + room);
            pge->room_location = (unsigned char)room;
        }
        if (init_field8(pge->index, 18) == 1) {               /* Conrad */
            s_room = (unsigned char)room;
            col_prepare_room_state();
            s_load_map = 1;
            if (s_room < 0x40) {
                activate_room(s_room, 1, 0);
                room = ct_s(CT_UP + s_room);
                if (room >= 0) activate_room((unsigned char)room, 0, 48);
                room = ct_s(CT_DOWN + s_room);
                if (room >= 0) activate_room((unsigned char)room, 0, 176);
            }
        }
    }
    room_list_move(pge->index, from_room);
}

/* pge_messageAck(): a pending message can make the object jump straight to the
 * end of its current animation, which is how it reacts on the next frame */
static void pge_message_ack(LivePGE *pge)
{
    unsigned int node, first, i, guard;
    const unsigned char *ob;
    unsigned char e;
    unsigned char hit = 0;

    node = init_field16(pge->index, I_NODE);
    SMS_mapROMBank(s_bank_a);
    first = *(const unsigned int *)(s_node_tab + node * 2);
    i = first + pge->first_obj;
    for (guard = 0; guard < 4000; guard++) {   /* never spin on unreadable data */
        unsigned char op1, op2;
        int a1, a2;
        ob = object_ptr(i);
        if (OB_TYPE(ob) != pge->obj_type) return;
        op1 = OB_OP1(ob); op2 = OB_OP2(ob);
        a1 = OB_ARG1(ob); a2 = OB_ARG2(ob);
        for (e = msg_head[pge->index]; e != 0xFF; e = msg_next[e]) {
            unsigned char m = msg_num[e];
            if (op2 == 0x6B) {
                if (a2 == 0 && (m == 1 || m == 2)) { hit = 1; break; }
                if (a2 == 1 && (m == 3 || m == 4)) { hit = 1; break; }
            } else if (m == a2 && (op2 == 0x22 || op2 == 0x6F)) { hit = 1; break; }
            if (op1 == 0x6B) {
                if (a1 == 0 && (m == 1 || m == 2)) { hit = 1; break; }
                if (a1 == 1 && (m == 3 || m == 4)) { hit = 1; break; }
            } else if (m == a1 && (op1 == 0x22 || op1 == 0x6F)) { hit = 1; break; }
        }
        if (hit) break;
        ++i;
    }
    {   /* run out the rest of the current animation in one go */
        const unsigned char *rec = map_ani(pge->obj_type);
        unsigned char dh = (unsigned char)rd16(rec);
        unsigned char dl = pge->anim_seq;
        const unsigned char *fr = rec + 6 + dl * 4;
        while (dh > dl) {
            if (rd16(fr) != 0xFFFF) {
                if (s_facing) pge->pos_x -= (signed char)fr[2];
                else          pge->pos_x += (signed char)fr[2];
                pge->pos_y += (signed char)fr[3];
            }
            fr += 4;
            ++dl;
        }
        pge->anim_seq = dh;
        s_grid_y = div36(pge->pos_y) & ~1;
        s_grid_x = (pge->pos_x + 8) >> 4;
    }
}

/* One object's frame: react to pending messages, and once its animation has
 * run out walk its script from first_obj for the first record whose
 * conditions hold; then advance the animation.  In asm; the C it implements:
 *   s_facing = flags & 1; s_pge_room = room_location
 *   if (msg_head[index] != 0xFF) pge_message_ack(pge)
 *   if (rd16(map_ani(obj_type)) <= anim_seq) {
 *       first = node_table[init_field16(index, I_NODE)] + first_obj
 *       for (guard = 4000; guard; guard--, first++) {
 *           ob = object_ptr(first)
 *           if (type(ob) != obj_type) { msg_clear(index); return; }
 *           ex_bank = ro_bank
 *           if (pge_execute(pge, ob)) { pge_setup_other_pieges(pge); break; }
 *           if (logic_bad_op) return
 *       }
 *   }
 *   pge_setup_anim(pge); anim_seq++; msg_clear(index)                     */
static unsigned int pp_first, pp_guard;

static void pge_process(LivePGE *pge) __naked
{
    (void)pge;
    __asm
    push ix
    push hl
    pop  ix
    ld   a, 17 (ix)
    and  a, #1
    ld   (_s_facing), a
    ld   a, 15 (ix)
    ld   (_s_pge_room), a
    ld   e, 18 (ix)
    ld   d, #0
    ld   hl, #_msg_head
    add  hl, de
    ld   a, (hl)
    inc  a
    jr   z, 00001$
    push ix
    pop  hl
    call _pge_message_ack
00001$:
    ld   l, 0 (ix)
    ld   h, 1 (ix)
    call _map_ani               ; de = animation record
    ex   de, hl
    ld   e, (hl)
    inc  hl
    ld   a, (hl)
    or   a, a
    jr   nz, 00020$             ; sequence count >= 256: still animating
    ld   a, 14 (ix)
    cp   a, e
    jr   c, 00020$              ; anim_seq < count: still animating
    ld   a, 18 (ix)
    ld   l, #6                  ; I_NODE
    call _init_field16          ; de = node
    ld   a, (_s_bank_a)
    ld   (_ROM_bank_to_be_mapped_on_slot2), a
    ex   de, hl
    add  hl, hl
    ld   de, (_s_node_tab)
    add  hl, de
    ld   e, (hl)
    inc  hl
    ld   d, (hl)
    ld   l, 8 (ix)
    ld   h, 9 (ix)
    add  hl, de
    ld   (_pp_first), hl
    ld   hl, #4000
    ld   (_pp_guard), hl
00010$:
    ld   hl, (_pp_first)
    call _object_ptr            ; de = record
    ld   a, (de)
    cp   a, 0 (ix)
    jr   nz, 00015$
    inc  de
    ld   a, (de)
    cp   a, 1 (ix)
    jr   nz, 00015$
    dec  de
    ld   a, (_ro_bank)
    ld   (_ex_bank), a
    push ix
    pop  hl
    call _pge_execute
    ld   a, e
    or   a, d
    jr   z, 00012$
    push ix
    pop  hl
    call _pge_setup_other_pieges
    jr   00020$
00012$:
    ld   a, (_logic_bad_op)
    or   a, a
    jr   nz, 00099$
    ld   hl, (_pp_first)
    inc  hl
    ld   (_pp_first), hl
    ld   hl, (_pp_guard)
    dec  hl
    ld   (_pp_guard), hl
    ld   a, h
    or   a, l
    jr   nz, 00010$
    jr   00020$
00015$:                         ; past this node's records
    ld   a, 18 (ix)
    call _msg_clear
    jr   00099$
00020$:
    push ix
    pop  hl
    call _pge_setup_anim
    inc  14 (ix)
    ld   a, 18 (ix)
    call _msg_clear
00099$:
    pop  ix
    ret
    __endasm;
}

/* --------------------------------------------------------------- frame --- */
static unsigned int state_checksum(void)
{
    unsigned int i, s = 0;
    LivePGE *p = pge_live;
    for (i = 0; i < pge_num; i++, p++) {
        s += (unsigned int)p->pos_x + (unsigned int)p->pos_y + p->anim_number
             + p->obj_type + p->flags + p->room_location + p->anim_seq
             + (p->first_obj & 0xFF);
    }
    return s;
}

void logic_start(unsigned char level_index)
{
    unsigned int i;
    /* every one of these is per level: with them pinned to level 1 the other
     * levels ran on level 1's collision map and walked through their floors */
    s_bank_a = level_bank_a[level_index];
    s_bank_obj = level_bank_obj[level_index];
    s_bank_aniidx = level_bank_aniidx[level_index];
    s_bank_ani = level_bank_ani[level_index];
    s_bank_ct = level_bank_ct[level_index];
    s_bank_logic = LOGIC_BANK;
    logic_level = level_num[level_index];     /* which level part is running */
    pge_load_level(level_index);
    s_node_tab = 0x8002 + pge_total * INIT_PGE_SIZE;
    ro_valid = 0;
    SMS_mapROMBank(s_bank_logic);
    s_total_frames = rd16((const unsigned char *)0x8000);
    /* the engine moves Conrad to the demo's start point when replaying a demo;
     * when playing, leave him where the level itself starts him */
    if (!logic_use_pad) {
        pge_live[0].room_location = *(const unsigned char *)0x8004;
        pge_live[0].pos_x = *(const unsigned char *)0x8005;
        pge_live[0].pos_y = *(const unsigned char *)0x8006;
    }
    /* the engine's current room is still the level's default here: it only
     * follows Conrad once the room-change logic runs (not ported yet) */
    s_room = init_field8(0, 19);
    s_load_map = 1;        /* the room follows Conrad at the end of the frame */
    s_last_lr = 0;
    s_rand = 0;
    s_inp = 0;
    msg_free = 0;
    for (i = 0; i < MSG_POOL; i++) { msg_next[i] = (i + 1 < MSG_POOL) ? i + 1 : 0xFF; }
    for (i = 0; i < MAX_PGE; i++) { msg_head[i] = 0xFF; next_in_room[i] = 0xFF; in_list[i] = 0xFF; }
    for (i = 0; i < INV_SLOTS; i++) inv_owner[i] = 0xFF;
    for (i = 0; i < 64; i++) room_head[i] = 0xFF;
    for (i = 0; i < pge_num; i++) {
        if (init_field8((unsigned char)i, 25) <= pge_skill) {   /* skill */
            unsigned char r = pge_live[i].room_location;
            if (r < 64) {
                next_in_room[i] = room_head[r];
                room_head[r] = (unsigned char)i;
                in_list[i] = r;
            }
        }
    }
    ov_count = 0;
    s_gun_var = 0;
    logic_frame = 0;
    logic_frames_ok = 0;
    logic_first_bad = 0xFFFF;
    logic_bad_op = 0;
    logic_bad_op_frame = 0;
    logic_running = 1;
}

/* Next object at or after `p` with flags bit 2 (active), or NULL once
 * s_scan_left objects have been passed.  In C each skipped object cost ~270
 * cycles (the loop state lives in ix slots), and both per-frame loops skip
 * most of the table. */
static unsigned char s_scan_left;

static LivePGE *scan_active(LivePGE *p) __naked
{
    (void)p;
    __asm
    ld   a, (_s_scan_left)
    or   a, a
    jr   z, 00003$
    ld   b, a
    ld   de, #17
    add  hl, de
    ld   de, #19
00001$:
    bit  2, (hl)
    jr   nz, 00002$
    add  hl, de
    djnz 00001$
    xor  a, a
    ld   (_s_scan_left), a
00003$:
    ld   de, #0
    ret
00002$:
    ld   a, b
    ld   (_s_scan_left), a
    ld   de, #-17
    add  hl, de
    ex   de, hl
    ret
    __endasm;
}

/* an object's type from the level data (3 = collectible), for the renderer */
unsigned char logic_object_type(unsigned char idx)
{
    if (idx >= pge_num || idx >= MAX_PGE) return 0xFF;
    return init_field8(idx, 18);
}

/* The frame's object loops (pge_prepare, then pge_process for each active
 * object), in asm - each loop step was ~270 cycles of ix traffic in C.
 *   1. the current room's objects: col_prepare_piege_state, and activate those
 *      whose InitPGE flags ask for it (flags bit 2)
 *   2. every other active object: col_prepare_piege_state
 *   3. col_prepare_room_state, then for every active object in index order:
 *      s_grid_y = div36(pos_y) & ~1, s_grid_x = (pos_x + 8) >> 4, pge_process;
 *      an unported opcode (logic_bad_op) stops the frame
 * An object activated during loop 3 is still processed this frame if its
 * index is higher, as in the engine. */
static LivePGE *lo_p;
static unsigned char lo_it;

static void logic_objects(void) __naked
{
    __asm
    ld   a, (_logic_cur_room)
    cp   a, #64
    jr   nc, 00020$
    ld   e, a
    ld   d, #0
    ld   hl, #_room_head
    add  hl, de
    ld   a, (hl)
00010$:                         ; 1. the current room's list
    cp   a, #0xff
    jr   z, 00020$
    ld   c, a
    ld   a, (_pge_num)
    ld   b, a
    ld   a, c
    cp   a, b
    jr   nc, 00020$             ; index past the table (bad_pge)
    ld   (_lo_it), a
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
    add  hl, de                 ; it * 19
    ld   de, #_pge_live
    add  hl, de
    ld   (_lo_p), hl
    call _col_prepare_piege_state
    ld   hl, (_lo_p)
    ld   de, #17
    add  hl, de
    bit  2, (hl)
    jr   nz, 00012$
    ld   a, (_lo_it)
    ld   l, #27                 ; I_FLAGS
    call _init_field8
    and  a, #4
    jr   z, 00012$
    ld   hl, (_lo_p)
    ld   de, #17
    add  hl, de
    set  2, (hl)
00012$:
    ld   a, (_lo_it)
    ld   e, a
    ld   d, #0
    ld   hl, #_next_in_room
    add  hl, de
    ld   a, (hl)
    jr   00010$
00020$:                         ; 2. active objects elsewhere
    ld   a, (_pge_num)
    ld   (_s_scan_left), a
    ld   hl, #_pge_live
00021$:
    call _scan_active
    ld   a, d
    or   a, e
    jr   z, 00030$
    ld   (_lo_p), de
    ld   hl, #15
    add  hl, de
    ld   a, (_logic_cur_room)
    cp   a, (hl)
    jr   z, 00022$
    ex   de, hl
    call _col_prepare_piege_state
00022$:
    ld   hl, #_s_scan_left
    dec  (hl)
    ld   hl, (_lo_p)
    ld   de, #19
    add  hl, de
    jr   00021$
00030$:
    call _col_prepare_room_state
    ld   a, (_pge_num)
    ld   (_s_scan_left), a
    ld   hl, #_pge_live
00031$:                         ; 3. process every active object
    call _scan_active
    ld   a, d
    or   a, e
    ret  z
    ld   (_lo_p), de
    ex   de, hl
    inc  hl
    inc  hl
    inc  hl
    inc  hl
    ld   a, (hl)
    inc  hl
    ld   h, (hl)
    ld   l, a                   ; pos_y
    call _div36
    ld   a, e
    and  a, #0xfe
    ld   (_s_grid_y), a
    ld   a, d
    ld   (_s_grid_y + 1), a
    ld   hl, (_lo_p)
    inc  hl
    inc  hl
    ld   a, (hl)
    inc  hl
    ld   h, (hl)
    ld   l, a                   ; pos_x
    ld   de, #8
    add  hl, de
    sra  h
    rr   l
    sra  h
    rr   l
    sra  h
    rr   l
    sra  h
    rr   l
    ld   (_s_grid_x), hl
    ld   hl, (_lo_p)
    call _pge_process
    ld   a, (_logic_bad_op)
    or   a, a
    ret  nz
    ld   hl, #_s_scan_left
    dec  (hl)
    ld   hl, (_lo_p)
    ld   de, #19
    add  hl, de
    jr   00031$
    __endasm;
}

unsigned char logic_step(void)
{
    const unsigned char *p;
    if (!logic_running) return 0;
    if (logic_use_pad) {
        /* pge_getInput(): the engine never sees a diagonal.  With both axes
         * held it keeps the modifiers and the last purely horizontal
         * direction, which is what the scripts are written against. */
        unsigned char dir = logic_pad_mask & 0x0F;
        if ((dir & 0x0C) && (dir & 0x03)) dir = s_last_lr;
        else s_last_lr = dir;
        s_inp = dir | (logic_pad_mask & 0xF0);
    } else {
        if (logic_frame >= s_total_frames) { logic_running = 0; return 0; }
        SMS_mapROMBank(s_bank_logic);           /* replaying the recorded demo */
        p = (const unsigned char *)(0x8008 + logic_frame * 3);
        s_inp = p[0];
        logic_expected = rd16(p + 1);
    }

    /* pge_prepare(): rebuild the collision slots for this room */
    col_clear_state();
    logic_objects();
    /* end of frame: the engine makes Conrad's room the current one here,
     * which is why the first frame still runs in the level's default room */
    if (s_load_map) {
        s_room = pge_live[0].room_location;
        s_load_map = 0;
    }
    if (!logic_check) { logic_frame++; return 1; }  /* playing: no checksum needed */
    logic_checksum = state_checksum();
    if (logic_checksum == logic_expected && !logic_bad_op) {
        logic_frames_ok++;
    } else if (logic_first_bad == 0xFFFF) {
        logic_first_bad = logic_frame;
        logic_running = 0;                 /* stop at the first divergence */
    }
    logic_frame++;
    return logic_running;
}
