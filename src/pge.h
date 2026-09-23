/* Live game objects (LivePGE) - the ported game logic's object table */
#ifndef PGE_H
#define PGE_H
/* 0xFF means "no object" in the room and inventory lists, so index 255 cannot
 * be used: level 2 has 256 objects and its last one is not simulated. */
#define MAX_PGE 255

/* The console has 8 KB of RAM in total and this table is the biggest thing in
 * it, so each field is as narrow as the data allows.  Measured over all seven
 * level parts: object types reach 1163, life values 7178 and object
 * numbers 65535, so those stay 16-bit.  The three inventory links are NOT
 * here: hardly any object ever carries or is carried, so they live in a small
 * side table in logic.c instead of costing three bytes for all 255. */
typedef struct {
    unsigned int  obj_type;
    int           pos_x, pos_y;
    unsigned int  anim_number;
    unsigned int  first_obj;
    int           life;
    int           counter_value;
    unsigned char anim_seq, room_location, collision_slot;
    unsigned char flags, index;
} LivePGE;

extern LivePGE pge_live[MAX_PGE];
extern unsigned int  pge_num;          /* objects simulated (<= MAX_PGE) */
extern unsigned int  pge_total;        /* objects in the level data */
extern unsigned int  pge_checksum;     /* checked against the original engine */
extern unsigned int  pge_active;       /* objects flagged active (flags & 4) */
extern unsigned char pge_skill;

void pge_load_level(unsigned char level_index);
void setup_default_anim(LivePGE *live, unsigned char idx_bank, unsigned char ani_bank0);
#endif
