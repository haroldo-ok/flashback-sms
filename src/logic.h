/* Object script interpreter (game logic), checked against the original engine */
#ifndef LOGIC_H
#define LOGIC_H
extern unsigned int  logic_frame;        /* frames simulated */
extern unsigned int  logic_frames_ok;    /* frames matching the engine */
extern unsigned int  logic_first_bad;    /* first diverging frame, 0xFFFF = none */
extern unsigned char logic_bad_op;       /* first opcode not ported yet */
extern unsigned int  logic_bad_op_frame;
extern unsigned char logic_running;
extern unsigned char logic_check;      /* 0 = run on without comparing */
extern unsigned char logic_cur_room;
extern unsigned char logic_use_pad;    /* play with the controller */
extern unsigned char logic_pad_mask;
extern unsigned int  logic_cutscene;   /* 0xFFFF = none pending */
extern unsigned char room_head[64], next_in_room[];
extern unsigned int  logic_checksum, logic_expected;
void logic_start(void);
unsigned char logic_object_type(unsigned char idx);
unsigned char logic_step(void);          /* one 30 Hz game frame */
#endif
