/* video_player.h -- FMV cutscene player for the merged Flashback SMS build.
 *
 * The cutscene stream is produced by tools/encode_shared_dictionary.py: all
 * seven cutscenes share ONE dictionary of tiles, and each cutscene has
 * per-position deltas (RLE cell updates) plus a snapshot every SNAPIVL
 * positions. Deltas are resumable and rate-limited to VIDEO_QUOTA tile uploads
 * per VBlank so playback never overruns the display.
 */
#ifndef VIDEO_PLAYER_H
#define VIDEO_PLAYER_H

#include "SMSlib.h"
#include "cutscenes_data.h"

#define CELL_TILE0   64          /* first VRAM slot owned by the 32x12 cell map */
#define BLANK_TILE   60          /* letterbox tile                              */
#define WIN_TILE_Y   6           /* window starts at tile row 6 (y = 48)        */
#define VID_TW       32
#define VID_TH       12
#define VID_CELLS    (VID_TW * VID_TH)

/* Tile uploads per VBlank. A paced 32-byte copy is ~968 cycles; 32 of them is
 * ~52% of an NTSC frame, so this fits a real machine and still lets the heavy
 * cutscenes (Holocube averages ~160 changed cells/frame) converge. */
#define VIDEO_QUOTA  32

/* Test/debug: position (frame index) the player is currently on, and whether a
 * delta is still being uploaded for it (0 = the picture for video_pos is on
 * screen exactly as the encoder meant it). */
extern volatile unsigned int video_pos;
extern volatile unsigned char video_active;
extern volatile unsigned char video_hold_extra;   /* 0 unless a test pokes it */

/* Plays a cutscene. Returns 1 if the player pressed 1/2 to skip it, 0 if the
 * cutscene ran to the end. */
unsigned char video_player_play (const cutscene_info_t *info);

#endif
