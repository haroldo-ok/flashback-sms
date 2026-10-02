/* The item name display and the inventory screen */
#ifndef HUD_H
#define HUD_H
void hud_reset(void);                   /* the room's tile map was just reloaded */
void hud_update(void);                  /* once per game frame while playing */
void inv_open(void);
unsigned char inv_step(unsigned int pressed);  /* 0 = stay, 1 = back to play, 2 = quit to title */
#endif
