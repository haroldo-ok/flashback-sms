/* Live game objects (LivePGE) - first slice of the game-logic port */
#ifndef PGE_H
#define PGE_H
#define MAX_PGE 180

typedef struct {
    unsigned int  obj_type;
    int           pos_x, pos_y;
    unsigned int  anim_number;
    unsigned int  first_obj;
    int           life;
    int           counter_value;
    unsigned char anim_seq, room_location, collision_slot;
    unsigned char next_inventory_PGE, current_inventory_PGE, ref_inventory_PGE;
    unsigned char flags, index;
} LivePGE;

extern LivePGE pge_live[MAX_PGE];
extern unsigned int  pge_num;          /* objects in the loaded level */
extern unsigned int  pge_checksum;     /* checked against the original engine */
extern unsigned int  pge_active;       /* objects flagged active (flags & 4) */
extern unsigned char pge_skill;

void pge_load_level(unsigned char level_index);
void setup_default_anim(LivePGE *live, unsigned char idx_bank, unsigned char ani_bank0);
#endif
