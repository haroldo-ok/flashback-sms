#include "video_player.h"

static unsigned char dbuf[768];

static void video_build_tilemap(void) {
    unsigned char x, y;
    unsigned int tile = CELL_TILE0;
    for (y = 0; y < 24; y++) {
        SMS_setNextTileatXY(0, y);
        if (y >= WIN_TILE_Y && y < WIN_TILE_Y + VID_TH) {
            for (x = 0; x < 32; x++) {
                if (x < VID_TW) {
                    SMS_setTile(tile++);
                } else {
                    SMS_setTile(BLANK_TILE);
                }
            }
        } else {
            for (x = 0; x < 32; x++) {
                SMS_setTile(BLANK_TILE);
            }
        }
    }
}

static void video_apply_snapshot(const cutscene_info_t *info, unsigned int snap) {
    unsigned char bank = info->snap_bank[snap];
    unsigned int off = info->snap_ofs[snap];
    unsigned int cell;
    for (cell = 0; cell < VID_CELLS; cell++) {
        unsigned int tid;
        unsigned char tbank;
        const unsigned char *p;
        SMS_mapROMBank(bank);
        p = (const unsigned char *)off;
        tid = (unsigned int)p[cell * 2] | ((unsigned int)p[cell * 2 + 1] << 8);
        tbank = (unsigned char)(info->data_bank0 + (tid >> 9));
        SMS_mapROMBank(tbank);
        SMS_loadTiles((const unsigned char *)(0x8000 + ((tid & 511) << 5)),
                      CELL_TILE0 + cell, 32);
    }
}

unsigned char video_player_play(const cutscene_info_t *info) {
    unsigned int pos;
    unsigned char cur_bank;
    const unsigned char *p;
    unsigned int cell;
    unsigned char d;
    unsigned int keys;

    static const unsigned char blank_tile[32] = {0};
    SMS_displayOff();
    SMS_initSprites();
    UNSAFE_SMS_copySpritestoSAT();
    SMS_loadTiles(blank_tile, BLANK_TILE, 32);
    SMS_loadBGPalette(info->bg_palette);
    video_build_tilemap();
    video_apply_snapshot(info, 0);
    SMS_displayOn();

    for (pos = 1; pos < info->nframes; pos++) {
        keys = SMS_getKeysStatus();
        if (keys & (PORT_A_KEY_1 | PORT_A_KEY_2)) {
            return 1; /* skipped */
        }

        if (pos % info->snapivl == 0) {
            video_apply_snapshot(info, pos / info->snapivl);
        } else {
            SMS_mapROMBank(info->frame_bank[pos]);
            {
                const unsigned char *src = (const unsigned char *)info->frame_ofs[pos];
                unsigned int len = info->frame_ofs[pos + 1] - info->frame_ofs[pos];
                unsigned int i;
                if (len > sizeof(dbuf)) len = sizeof(dbuf);
                for (i = 0; i < len; i++) dbuf[i] = src[i];
            }

            cur_bank = 0xFF;
            cell = 0;
            p = dbuf;
            for (;;) {
                unsigned char skip = *p++;
                unsigned char count = *p++;
                if (skip == 255 && count == 0) { cell += 254; continue; }
                if (skip == 0 && count == 0) break;
                cell += skip;
                while (count--) {
                    unsigned int tid = (unsigned int)p[0] | ((unsigned int)p[1] << 8);
                    unsigned char tbank = (unsigned char)(info->data_bank0 + (tid >> 9));
                    p += 2;
                    if (tbank != cur_bank) { cur_bank = tbank; SMS_mapROMBank(tbank); }
                    SMS_loadTiles((const unsigned char *)(0x8000 + ((tid & 511) << 5)),
                                  CELL_TILE0 + cell, 32);
                    cell++;
                }
            }
        }

        for (d = 0; d < info->frame_delay; d++) {
            SMS_waitForVBlank();
        }
    }
    return 0;
}
