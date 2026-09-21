/* Draws the objects the ported game logic simulates */
#ifndef SIM_H
#define SIM_H
extern unsigned char sim_sprites;   /* sprites drawn last frame (tests) */
extern unsigned int  sim_uploads;
extern unsigned char sim_scroll, sim_room;
void sim_start(void);
void sim_step(void);
void sim_resume(void);              /* re-enter after a cutscene */                /* one 30 Hz game frame */
#endif
