#ifndef GAME_PLAY_H
#define GAME_PLAY_H

extern int g_cur_room;
extern unsigned char g_trigger_holocube_scene;
extern unsigned char g_trigger_death_scene;

void game_play_init(void);
unsigned char game_play_update(void);
void game_play_draw(void);
unsigned char game_is_solid(int x, int y);

#endif
