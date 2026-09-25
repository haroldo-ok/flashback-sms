/* Draws the objects the ported game logic simulates */
#ifndef SIM_H
#define SIM_H
extern unsigned char sim_sprites;   /* sprites drawn last frame (tests) */
extern unsigned int  sim_uploads;
extern unsigned char sim_scroll, sim_room;
void sim_start(unsigned char level_index);
void sim_step(void);
/* main.c: buttons down at any frame since the last call (latched each frame) */
unsigned int input_take_held(void);
void sim_resume(void);              /* re-enter after a cutscene */                /* one 30 Hz game frame */
#endif
