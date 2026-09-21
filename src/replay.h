/* Gameplay replay player: sprite stream produced by tools/spriteconv.py */
#ifndef REPLAY_H
#define REPLAY_H
extern unsigned char replay_active;
extern unsigned int  replay_frame;     /* game frames played (30 Hz) */
extern unsigned int  replay_uploads;   /* 8x16 sprite entries uploaded (tests) */
extern unsigned char replay_sprites;   /* sprites in the last frame (tests) */
extern unsigned char replay_scroll;
void replay_start(void);
unsigned char replay_step(void);       /* one game frame; 0 when the demo ends */
#endif
