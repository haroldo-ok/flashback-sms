#ifndef VIDEO_PLAYER_H
#define VIDEO_PLAYER_H

#include "SMSlib.h"
#include "cutscenes_data.h"

#define CELL_TILE0  64
#define BLANK_TILE  60
#define WIN_TILE_X  0
#define WIN_TILE_Y  6
#define VID_TW      32
#define VID_TH      12
#define VID_CELLS   (VID_TW * VID_TH)

/* Plays a cutscene; returns 1 if skipped by player, 0 if finished */
unsigned char video_player_play(const cutscene_info_t *info);

#endif
