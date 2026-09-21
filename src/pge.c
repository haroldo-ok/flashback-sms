/*
 * Builds the live object table for a level, the way the original engine's
 * pge_loadForCurrentLevel() does.  Level data lives in ROM as produced by
 * tools/levelconv.py:
 *   part A (one bank)  : u16 numPges, InitPGE[31 bytes], node_first[256] u16,
 *                        node_num[256] u16
 *   part B (n banks)   : Object[18 bytes], 910 per bank so none straddles
 * Both parts page into the same window, so the loader reads what it needs
 * from part A, switches to the objects, then switches back.
 */
#include "SMSlib.h"
#include "data_index.h"
#include "pge.h"

#define INIT_PGE_SIZE    31
#define OBJECT_SIZE      18
#define OBJECTS_PER_BANK 910

/* InitPGE field offsets */
#define I_TYPE        0
#define I_POS_X       2
#define I_POS_Y       4
#define I_NODE        6
#define I_LIFE        8
#define I_OBJECT_TYPE 18
#define I_INIT_ROOM   19
#define I_ROOM_LOC    20
#define I_INIT_FLAGS  21
#define I_SKILL       25
#define I_MIRROR_X    26
#define I_FLAGS       27

LivePGE pge_live[MAX_PGE];
unsigned int  pge_num;
unsigned int  pge_checksum;
unsigned int  pge_active;
unsigned char pge_skill = 1;

static unsigned int rd16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* pge_setupDefaultAnim(): the animation record decides two of the object's
 * flags and its starting animation frame.  Records are indexed by object type
 * through a (bank, address) table, both paged into the same window. */
void setup_default_anim(LivePGE *live, unsigned char idx_bank, unsigned char ani_bank0)
{
    const unsigned char *e;
    const unsigned char *rec;
    unsigned char bank;
    unsigned int f, frame;

    SMS_mapROMBank(idx_bank);
    e = (const unsigned char *)(0x8000 + live->obj_type * 3);
    bank = e[0];
    rec = (const unsigned char *)rd16(e + 1);
    SMS_mapROMBank(ani_bank0 + bank);

    live->anim_seq = 0;
    frame = rd16(rec + 6);                      /* anim_seq 0 */
    if (frame == 0xFFFF) return;
    f = rd16(rec);
    if (live->flags & 1) f ^= 0x8000;
    live->flags &= ~2;
    if (f & 0x8000) live->flags |= 2;
    live->flags &= ~8;
    if (rd16(rec + 4)) live->flags |= 8;
    live->anim_number = frame & 0x7FFF;
}

void pge_load_level(unsigned char level_index)
{
    const unsigned char *A = (const unsigned char *)0x8000;
    const unsigned char *p;
    unsigned char bank_a = level_bank_a[level_index];
    unsigned char bank_obj = level_bank_obj[level_index];
    unsigned char bank_aniidx = level_bank_aniidx[level_index];
    unsigned char bank_ani = level_bank_ani[level_index];
    unsigned char room0, skill, init_flags, iflags;
    unsigned int i, node, first, count, sum = 0;
    LivePGE *live;

    pge_active = 0;
    SMS_mapROMBank(bank_a);
    pge_num = rd16(A);
    if (pge_num > MAX_PGE) pge_num = MAX_PGE;
    room0 = A[2 + I_INIT_ROOM];                 /* engine: _currentRoom = pgeInit[0].init_room */

    for (i = 0; i < pge_num; i++) {
        SMS_mapROMBank(bank_a);
        p = A + 2 + i * INIT_PGE_SIZE;
        live = &pge_live[i];
        live->obj_type = rd16(p + I_TYPE);
        live->pos_x = (int)rd16(p + I_POS_X);
        live->pos_y = (int)rd16(p + I_POS_Y);
        live->anim_seq = 0;
        live->room_location = p[I_INIT_ROOM];
        live->life = (int)rd16(p + I_LIFE);
        live->counter_value = 0;
        live->collision_slot = 0xFF;
        live->next_inventory_PGE = 0xFF;
        live->current_inventory_PGE = 0xFF;
        live->ref_inventory_PGE = 0xFF;
        live->anim_number = 0;
        live->index = (unsigned char)i;
        live->flags = 0;
        live->first_obj = 0;

        skill = p[I_SKILL];
        if (skill > pge_skill) continue;

        init_flags = p[I_INIT_FLAGS];
        iflags = p[I_FLAGS];
        if (p[I_ROOM_LOC] != 0 || ((iflags & 4) && room0 == p[I_INIT_ROOM])) {
            live->flags |= 4;                   /* active this level */
            pge_active++;
        }
        if (p[I_MIRROR_X]) live->flags |= 1;
        if (init_flags & 8) live->flags |= 0x10;
        live->flags |= (init_flags & 3) << 5;
        if (iflags & 2) live->flags |= 0x80;

        node = rd16(p + I_NODE);
        first = rd16(A + 2 + pge_num * INIT_PGE_SIZE + node * 2);
        /* the object's entry point is its index within the node's object list */
        /* walk the node's object list for this object's type.  The pointer
         * moves through the bank and only remaps at a bank boundary: doing a
         * bank switch per object made loading a level take seconds. */
        {
            unsigned int n = first;
            unsigned char bank = bank_obj;
            const unsigned char *o;
            while (n >= OBJECTS_PER_BANK) { n -= OBJECTS_PER_BANK; bank++; }
            SMS_mapROMBank(bank);
            o = (const unsigned char *)(0x8000 + n * OBJECT_SIZE);
            count = 0;
            while (rd16(o) != live->obj_type) {
                if (++count > 4000) break;      /* malformed data guard */
                o += OBJECT_SIZE;
                if (++n >= OBJECTS_PER_BANK) {
                    n = 0;
                    SMS_mapROMBank(++bank);
                    o = (const unsigned char *)0x8000;
                }
            }
        }
        live->first_obj = count;

        setup_default_anim(live, bank_aniidx, bank_ani);
        sum += count + live->flags + (unsigned int)live->life + live->room_location
               + live->anim_number;
    }
    pge_checksum = sum;
}
