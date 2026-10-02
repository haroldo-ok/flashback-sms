/*
 * As in the original: standing on something that can be picked up or used
 * shows its icon and name at the top of the screen, and the inventory screen
 * lists what Conrad carries.  Icons, glyphs and the level texts come from the
 * game's own files (tools/hudconv.py), coloured with the sprite palette so the
 * name display can sit over any room: background cells may select it.
 *
 * VRAM: the current item's icon uses tiles 44..47 and the name display
 * 48..63, the top of the sprite pattern area (the sprite cache keeps 0..43).  The inventory screen owns the whole
 * screen, so it uses tiles from 256 up and the room is reloaded afterwards.
 */
#include "SMSlib.h"
#include "data_index.h"
#include "pge.h"
#include "logic.h"
#include "room.h"
#include "sim.h"
#include "hud.h"

#if HAS_HUD
#define HUD_T0       48          /* icon: 48..51, glyphs: 52..63 */
#define HUD_GLYPH_T  52
#define HUD_MAX_G    12
#define NT_FLAGS     0x1800      /* sprite palette + in front of sprites */

static unsigned char hud_obj = 0xFF, hud_row = 0xFF;
static unsigned char tbuf[32];

static const unsigned char *hud(unsigned int off) { return (const unsigned char *)(0x8000 + off); }

/* one 1-bit glyph -> a planar tile in colour `col` */
static void glyph_tile(unsigned char g, unsigned char col, unsigned int vram)
{
    const unsigned char *src;
    unsigned char y, k, bits, *d = tbuf;
    SMS_mapROMBank(HUD_BANK);
    src = hud(HUD_GLYPHS + g * 8);
    for (y = 0; y < 8; y++) {
        bits = src[y];
        for (k = 0; k < 4; k++) *d++ = (col & (1 << k)) ? bits : 0;
    }
    SMS_loadTiles(tbuf, vram, 32);
}

/* icon number -> its four tiles at vram..vram+3; returns 0 if not converted */
static unsigned char icon_tiles(unsigned char icon, unsigned int vram)
{
    unsigned char k;
    SMS_mapROMBank(HUD_BANK);
    if (icon >= 128) return 0;
    k = hud(96)[icon];
    if (k == 0xFF) return 0;
    SMS_loadTiles(hud(HUD_ICONS + (unsigned int)k * 128), vram, 128);
    return 1;
}

/* a level text: the string for text_num in the current level part */
static const unsigned char *text_of(unsigned int num)
{
    unsigned int tab, off;
    SMS_mapROMBank(HUD_BANK);
    tab = hud(224 + 2 * logic_level)[0] | ((unsigned int)hud(224 + 2 * logic_level)[1] << 8);
    off = hud(tab + 2 * num)[0] | ((unsigned int)hud(tab + 2 * num)[1] << 8);
    return hud(off);
}

/* put back the room's own cells: `n` cells from (col, row) */
static void restore_cells(unsigned char col, unsigned char row, unsigned char n)
{
    const unsigned char *nt;
    unsigned int e, i;
    if (sim_room_idx == 0xFF || row >= 28) return;
    SMS_mapROMBank(room_bank[sim_room_idx]);
    nt = (const unsigned char *)(room_addr[sim_room_idx] + 18);
    SMS_setNextTileatXY(col, row);
    for (i = 0; i < n && col + i < 32; i++) {
        e = nt[(row * 32 + col + i) * 2] | ((unsigned int)nt[(row * 32 + col + i) * 2 + 1] << 8);
        SMS_setTile(e);
    }
}

/* Layout as the engine draws it (drawLevelTexts, drawCurrentInventoryItem):
 * the touched item's icon at (80,8), its name centred in the first 176 pixels
 * at y=26, and the current item's icon at (232,8) - relative to the visible
 * top, which moves with the vertical scroll. */
#define TOUCH_COL  10            /* x = 80  */
#define CUR_COL    29            /* x = 232 */
#define CUR_T0     44            /* the current item's icon: tiles 44..47 */

static unsigned char name_col, name_len;
static unsigned char cur_item = 0xFF, cur_row = 0xFF;

static void icon_cells(unsigned char col, unsigned char row, unsigned int t)
{
    SMS_setNextTileatXY(col, row);
    SMS_setTile(t | NT_FLAGS); SMS_setTile((t + 1) | NT_FLAGS);
    SMS_setNextTileatXY(col, row + 1);
    SMS_setTile((t + 2) | NT_FLAGS); SMS_setTile((t + 3) | NT_FLAGS);
}

static void touch_clear(void)
{
    if (hud_row == 0xFF) return;
    restore_cells(TOUCH_COL, hud_row + 1, 2);
    restore_cells(TOUCH_COL, hud_row + 2, 2);
    if (name_len) restore_cells(name_col, hud_row + 3, name_len);
    name_len = 0;
}

static void cur_clear(void)
{
    if (cur_row == 0xFF) return;
    restore_cells(CUR_COL, cur_row + 1, 2);
    restore_cells(CUR_COL, cur_row + 2, 2);
}

void hud_reset(void)
{
    hud_obj = 0xFF;
    hud_row = 0xFF;
    cur_item = 0xFF;
    cur_row = 0xFF;
    name_len = 0;
}

void hud_update(void)
{
    unsigned char o = logic_touching();
    unsigned char row = sim_scroll >> 3;
    unsigned char ci = logic_inv_first();
    unsigned char i, n, g, gi, used[HUD_MAX_G], nused = 0;
    const unsigned char *s;
    unsigned char cells[22];

    /* the current item, top right */
    if (ci != cur_item || row != cur_row) {
        cur_clear();
        cur_item = ci;
        cur_row = 0xFF;
        if (ci != 0xFF && icon_tiles(logic_field8(ci, 23), CUR_T0)) {
            icon_cells(CUR_COL, row + 1, CUR_T0);
            cur_row = row;
        }
    }

    /* what Conrad is standing on */
    if (o == hud_obj && (o == 0xFF || row == hud_row)) return;
    touch_clear();
    hud_obj = o;
    hud_row = 0xFF;
    if (o == 0xFF) return;
    hud_row = row;
    if (icon_tiles(logic_field8(o, 22) - 1, HUD_T0))      /* colliding_icon_num - 1 */
        icon_cells(TOUCH_COL, row + 1, HUD_T0);
    s = text_of(logic_field16(o, 29));
    for (n = 0; n < 22; n++) {
        SMS_mapROMBank(HUD_BANK);
        if (!s[n]) break;
        g = (s[n] >= 0x20 && s[n] < 0x80) ? hud(0)[s[n] - 0x20] : 0xFF;
        if (g == 0xFF) { cells[n] = 0xFF; continue; }
        for (gi = 0; gi < nused && used[gi] != g; gi++) ;
        if (gi == nused) {
            if (nused == HUD_MAX_G) { cells[n] = 0xFF; continue; }
            used[nused] = g;
            glyph_tile(g, HUD_TEXT_COL, HUD_GLYPH_T + nused);
            nused++;
        }
        cells[n] = gi;
    }
    name_len = n;
    name_col = (unsigned char)((22 - n) >> 1);        /* centred in 176 pixels */
    SMS_setNextTileatXY(name_col, row + 3);
    for (i = 0; i < n; i++)
        SMS_setTile((HUD_GLYPH_T + (cells[i] == 0xFF ? 0 : cells[i])) | NT_FLAGS);
}

/* ------------------------------ inventory ------------------------------ */
#define INV_T0     256           /* free while the inventory owns the screen */
#define INV_MAX    8
static unsigned char inv_items[INV_MAX], inv_n, inv_sel;

static void inv_draw(void)
{
    unsigned char i, n, g, x, y;
    const unsigned char *s;
    unsigned int t = INV_T0 + 2;                  /* 256 blank, 257 cursor bar */
    SMS_displayOff();
    SMS_VRAMmemsetW(XYtoADDR(0, 0), INV_T0, 32 * 28 * 2);
    /* each item: its icon as 2x2 cells, four columns apart, on rows 8-9 */
    for (i = 0; i < inv_n; i++) {
        x = 2 + i * 4;
        if (icon_tiles(logic_field8(inv_items[i], 23), t)) {
            SMS_setNextTileatXY(x, 8);  SMS_setTile(t | 0x0800); SMS_setTile((t + 1) | 0x0800);
            SMS_setNextTileatXY(x, 9);  SMS_setTile((t + 2) | 0x0800); SMS_setTile((t + 3) | 0x0800);
        }
        t += 4;
    }
    /* the cursor under the selected item, and its name below */
    SMS_setNextTileatXY(2 + inv_sel * 4, 11);
    SMS_setTile((INV_T0 + 1) | 0x0800); SMS_setTile((INV_T0 + 1) | 0x0800);
    if (inv_n) {
        s = text_of(logic_field16(inv_items[inv_sel], 29));
        for (n = 0; n < 28; n++) {
            SMS_mapROMBank(HUD_BANK);
            if (!s[n]) break;
            g = (s[n] >= 0x20 && s[n] < 0x80) ? hud(0)[s[n] - 0x20] : 0xFF;
            y = 14;
            if (g != 0xFF) {
                glyph_tile(g, HUD_TEXT_COL, t);
                SMS_setNextTileatXY(2 + n, y);
                SMS_setTile(t | 0x0800);
                t++;
            }
        }
    }
    SMS_displayOn();
}

void inv_open(void)
{
    unsigned char it = logic_inv_first(), guard = 0;
    unsigned char k;
    inv_n = 0;
    inv_sel = 0;
    while (it != 0xFF && inv_n < INV_MAX && guard++ < 64) {
        inv_items[inv_n++] = it;
        it = logic_inv_next(it);
    }
    SMS_displayOff();
    SMS_setSpriteMode(SPRITEMODE_NORMAL);
    SMS_initSprites();
    SMS_copySpritestoSAT();
    SMS_setBGScrollY(0);
    SMS_loadBGPalette(spr_palette);               /* icons and text keep their colours */
    SMS_loadSpritePalette(spr_palette);
    for (k = 0; k < 32; k++) tbuf[k] = 0;
    SMS_loadTiles(tbuf, INV_T0, 32);              /* blank */
    for (k = 0; k < 32; k++) tbuf[k] = (k & 3) < 4 && ((HUD_TEXT_COL >> (k & 3)) & 1) ? 0xFF : 0;
    SMS_loadTiles(tbuf, INV_T0 + 1, 32);          /* cursor bar */
    inv_draw();
}

unsigned char inv_step(unsigned int pressed)
{
    if ((pressed & PORT_A_KEY_1) && (pressed & PORT_A_KEY_2)) return 2;
    if ((pressed & PORT_A_KEY_LEFT) && inv_sel) { inv_sel--; inv_draw(); }
    else if ((pressed & PORT_A_KEY_RIGHT) && inv_sel + 1 < inv_n) { inv_sel++; inv_draw(); }
    else if (pressed & PORT_A_KEY_2) {            /* make it the current item */
        if (inv_n) logic_select_item(inv_items[inv_sel]);
        return 1;
    }
    else if (pressed & PORT_A_KEY_1) return 1;
    return 0;
}
#else
void hud_reset(void) {}
void hud_update(void) {}
void inv_open(void) {}
unsigned char inv_step(unsigned int pressed) { (void)pressed; return 1; }
#endif
