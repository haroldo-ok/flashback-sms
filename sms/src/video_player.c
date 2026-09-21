/* video_player.c -- see video_player.h.
 *
 * Stream format (per position):
 *   delta :  <skip u8> <count u8> <count x u16 tile id LE> ... <0> <0>
 *            255/0 = "advance 254 cells, keep going"
 *   snapshot: VID_CELLS x u16 tile id LE   (full window state)
 * Tiles live 512 per 16 KiB bank: bank = data_bank0 + (tid >> 9),
 * address = 0x8000 + ((tid & 511) << 5).
 */
#include "video_player.h"

volatile unsigned int video_pos;        /* frame index currently being shown */
volatile unsigned char video_active;    /* 1 while a delta is still uploading  */
volatile unsigned char video_hold_extra; /* test hook: extra VBlanks per position */

static unsigned char dbuf[1024];      /* one delta, copied out of the ROM bank */

/* --- resumable delta state ------------------------------------------------ */
static unsigned char dactive;         /* a delta is being applied             */
static unsigned int  dpos;            /* cursor into dbuf                     */
static unsigned int  dcell;           /* cell the next tile is written to     */
static unsigned char drun;            /* tiles left in the current run        */

static const unsigned char blank_tile[32] = { 0 };

static void video_build_tilemap (void) {
    unsigned char x, y;
    unsigned int tile = CELL_TILE0;
    for (y = 0; y < 24; ++y) {
        SMS_setNextTileatXY (0, y);
        if ((y >= WIN_TILE_Y) && (y < WIN_TILE_Y + VID_TH)) {
            for (x = 0; x < VID_TW; ++x) SMS_setTile (tile++);
        } else {
            for (x = 0; x < 32; ++x) SMS_setTile (BLANK_TILE);
        }
    }
}

/* Paint a whole snapshot into the cell map.  Runs with the display OFF.
 *
 * IMPORTANT: a snapshot entry and a tile can live in *different* ROM banks, so
 * the snapshot bank has to be mapped again before every entry is read -- the
 * tile upload that uses the previous entry leaves a tile bank mapped.  Reading
 * p[] while a tile bank is mapped silently yields garbage tile ids, which shows
 * up as a recognisable but corrupted picture (only cells whose tile happens to
 * sit in the snapshot's own bank survive). */
static void video_apply_snapshot (const cutscene_info_t *info, unsigned int snap) {
    unsigned char sbank = info->snap_bank[snap];
    const unsigned char *p = (const unsigned char *)info->snap_ofs[snap];
    unsigned int cell;
    for (cell = 0; cell < VID_CELLS; ++cell) {
        unsigned int tid;
        unsigned char tbank;
        SMS_mapROMBank (sbank);
        tid = (unsigned int)p[cell * 2] | ((unsigned int)p[cell * 2 + 1] << 8);
        tbank = (unsigned char)(info->data_bank0 + (tid >> 9));
        SMS_mapROMBank (tbank);
        SMS_loadTiles ((const unsigned char *)(0x8000 + ((tid & 511) << 5)),
                       CELL_TILE0 + cell, 32);
    }
}

/* Copy one delta out of its bank into RAM (the tile uploads page other banks,
 * so the delta cannot stay in the paged window while it is being applied). */
static void video_load_delta (const cutscene_info_t *info, unsigned int pos) {
    const unsigned char *p;
    unsigned int n = 0;
    SMS_mapROMBank (info->frame_bank[pos]);
    p = (const unsigned char *)info->frame_ofs[pos];
    for (;;) {
        unsigned char skip = p[n];
        unsigned char cnt = p[n + 1];
        n += 2;
        if ((skip == 255) && (cnt == 0)) continue;
        if ((skip == 0) && (cnt == 0)) break;
        n += (unsigned int)cnt * 2;
    }
    if (n > sizeof (dbuf)) n = sizeof (dbuf);
    {
        unsigned int i;
        for (i = 0; i < n; ++i) dbuf[i] = p[i];
    }
    dpos = 0;
    dcell = 0;
    drun = 0;
    dactive = 1;
    video_active = 1;
}

/* Write at most VIDEO_QUOTA tiles of the active delta. Returns 1 while the
 * delta still has work left (call again after the next VBlank). */
static unsigned char video_apply_budget (const cutscene_info_t *info) {
    unsigned int written = 0;
    unsigned char cur_bank = 0xFF;
    while (dactive && (written < VIDEO_QUOTA)) {
        if (drun == 0) {
            unsigned char skip = dbuf[dpos];
            unsigned char cnt = dbuf[dpos + 1];
            dpos += 2;
            if ((skip == 255) && (cnt == 0)) { dcell += 254; continue; }
            if ((skip == 0) && (cnt == 0)) { dactive = 0; video_active = 0; break; }
            dcell += skip;
            drun = cnt;
            if (cnt == 0) continue;
        }
        {
            unsigned int tid = (unsigned int)dbuf[dpos] |
                               ((unsigned int)dbuf[dpos + 1] << 8);
            unsigned char tbank = (unsigned char)(info->data_bank0 + (tid >> 9));
            dpos += 2;
            if (tbank != cur_bank) { cur_bank = tbank; SMS_mapROMBank (tbank); }
            SMS_loadTiles ((const unsigned char *)(0x8000 + ((tid & 511) << 5)),
                           CELL_TILE0 + dcell, 32);
            ++dcell;
            --drun;
            ++written;
        }
    }
    return dactive;
}

/* One step of a cutscene: wait for VBlank, push the next slice of the delta,
 * and report a *newly pressed* 1/2 (holding a button from before the cutscene
 * started does not skip it). */
static unsigned int prev_keys;

static unsigned char video_tick (const cutscene_info_t *info) {
    unsigned int keys = SMS_getKeysStatus ();
    unsigned char pressed = (unsigned char)(keys & ~prev_keys &
                                            (PORT_A_KEY_1 | PORT_A_KEY_2));
    prev_keys = keys;
    SMS_waitForVBlank ();
    video_apply_budget (info);
    return pressed;
}

unsigned char video_player_play (const cutscene_info_t *info) {
    unsigned int pos;
    unsigned char d;

    /* Announce "painting the opening snapshot" so a test can tell the very
     * first position of a cutscene from the previous cutscene's last one. */
    video_pos = 0xFFFF;
    video_active = 0;

    /* VRAM setup (display off: the full-window repaint is cheap and safe) */
    SMS_displayOff ();
    SMS_initSprites ();
    UNSAFE_SMS_copySpritestoSAT ();
    SMS_useFirstHalfTilesforSprites (1);
    SMS_setSpriteMode (SPRITEMODE_NORMAL);
    SMS_loadTiles (blank_tile, BLANK_TILE, 32);
    SMS_loadBGPalette (info->bg_palette);
    SMS_setBackdropColor (0);
    video_build_tilemap ();
    video_apply_snapshot (info, 0);
    SMS_setBGScrollX (0);
    SMS_setBGScrollY (0);
    SMS_displayOn ();

    prev_keys = SMS_getKeysStatus ();       /* ignore a button held right now */

    /* Position 0 (the snapshot painted above) keeps its full display time, so
     * the first frame of a cutscene is actually visible -- and so a test can
     * capture it: video_pos stays 0 and video_active stays 0 while nothing is
     * being uploaded. */
    video_pos = 0;
    video_active = 0;
    for (d = info->frame_delay + video_hold_extra; d > 0; --d) {
        unsigned int keys = SMS_getKeysStatus ();
        unsigned char pressed = (unsigned char)(keys & ~prev_keys &
                                                (PORT_A_KEY_1 | PORT_A_KEY_2));
        prev_keys = keys;
        SMS_waitForVBlank ();
        if (pressed) return 1;             /* skipped */
    }

    for (pos = 1; pos < info->nframes; ++pos) {
        unsigned char ticks = 0;
        video_pos = pos;
        video_load_delta (info, pos);
        while (dactive) {                      /* 1..n VBlanks to apply the delta */
            if (video_tick (info)) return 1;   /* skipped */
            ++ticks;
        }
        /* Hold the position for the remaining part of its display time, but
         * always at least one full VBlank so the *completed* picture is really
         * shown (a heavy delta that needed several VBlanks already spent that
         * time, so the cutscene keeps the pace of the original demo). */
        for (d = ((info->frame_delay > ticks) ? info->frame_delay - ticks : 1)
                 + video_hold_extra;
             d > 0; --d) {
            unsigned int keys = SMS_getKeysStatus ();
            unsigned char pressed = (unsigned char)(keys & ~prev_keys &
                                                    (PORT_A_KEY_1 | PORT_A_KEY_2));
            prev_keys = keys;
            SMS_waitForVBlank ();
            if (pressed) return 1;             /* skipped */
        }
    }
    return 0;
}
