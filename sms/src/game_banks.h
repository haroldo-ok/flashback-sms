/* ROM bank map of the merged 4 MB Flashback SMS cartridge image.
 *
 *   bank   0 ..  1   code (fixed first 32 KiB, linked normally)
 *   bank   2 .. 43   level 1 data -- compiled from sms/gen/bank*.c, each in its
 *                    own CONST segment placed at 0x<n>8000 (see the Makefile)
 *   bank  44 .. 96   FMV stream (all cutscenes, one shared tile dictionary),
 *                    appended as a raw blob: bank = 44 + blob_bank
 *   bank  97         title screen  (palette + tiles + tilemap)
 *   bank  98         instructions screen
 */
#ifndef GAME_BANKS_H
#define GAME_BANKS_H

#define BANK_VIDEO_DATA0    44
#define BANK_VIDEO_BANKS    53
#define BANK_TITLE_SCREEN   97
#define BANK_INSTRU_SCREEN  98
#define ROM_BANKS           99

/* level 1 data banks (kept identical to the level-1 port) */
#define BANK_ROOMS_START     2       /* rooms 26..63: banks 2..39            */
#define BANK_CONRAD_SPRITES0 40      /* Conrad frames: banks 40..42          */
#define BANK_ITEM_ICONS      43      /* item icons                           */
#define BANK_LEVEL1_END      43

#endif
