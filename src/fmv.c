/*
 * Cutscene tile-stream player.
 *
 * The stream (tools/fmvenc.py, packed by tools/mkdata.py) is a sequence of
 * byte ops read straight from the paged ROM window.  Tile patterns live in a
 * dictionary shared by every cutscene (bank FMV_DICT_BANK0 + id/512); an
 * upload pages the dictionary bank in, copies 32 bytes to VRAM and pages the
 * stream bank back.  No op ever straddles a bank (OP_NEXTBANK), so pointers
 * into the window stay valid for SMS_VRAMmemcpy.
 */
#include "SMSlib.h"
#include "data_index.h"
#include "tiledec.h"
#include "fmv.h"

#define OP_TICK     0
#define OP_WAIT     1
#define OP_PALBG    2
#define OP_PALSPR   3
#define OP_DISPOFF  4
#define OP_DISPON   5
#define OP_CLEAR    6
#define OP_NEXTBANK 7
#define OP_END      0xFF

unsigned char fmv_active;
unsigned int  fmv_tick;
unsigned int  fmv_uploads;

static unsigned char s_bank;
static const unsigned char *s_ptr;
static unsigned char s_wait;

unsigned char fmv_find(unsigned char cutscene_id)
{
    unsigned char i;
    for (i = 0; i < FMV_NUM_CLIPS; i++)
        if (fmv_clip_id[i] == cutscene_id) return i;
    return 0xFF;
}

void fmv_start(unsigned char clip)
{
    SMS_displayOff();
    SMS_setBGScrollY(0);
    SMS_zeroBGPalette();
    s_bank = fmv_clip_bank[clip];
    s_ptr = (const unsigned char *)fmv_clip_addr[clip];
    s_wait = 0;
    fmv_tick = 0;
    fmv_uploads = 0;
    fmv_active = 1;
}

void fmv_stop(void)
{
    fmv_active = 0;
}

unsigned char fmv_step(void)
{
    unsigned char op;
    unsigned int guard;
    if (!fmv_active) return 0;
    if (s_wait) { s_wait--; fmv_tick++; return 1; }
    SMS_mapROMBank(s_bank);
    /* Bounded: a frame is a few hundred ops at most.  If the stream reads back
     * as garbage - an emulator that cannot reach the bank, a bad build - this
     * loop must not spin forever with the machine unresponsive. */
    for (guard = 0; guard < 4000; guard++) {
        op = *s_ptr++;
        if ((op & 0xC0) == 0x40) {                /* 0x40..0x7F nametable run */
            unsigned int n = ((unsigned int)(op & 0x3F) + 1) << 1;
            unsigned int cell = s_ptr[0] | ((unsigned int)s_ptr[1] << 8);
            s_ptr += 2;
            SMS_VRAMmemcpy(XYtoADDR(0, 0) + (cell << 1), s_ptr, n);
            s_ptr += n;
        } else if ((op & 0xF0) == 0x10) {         /* tile upload */
            unsigned int slot = ((unsigned int)(op & 1) << 8) | s_ptr[0];
            unsigned int id = s_ptr[1] | ((unsigned int)s_ptr[2] << 8);
            s_ptr += 3;
            tile_upload(&fmv_dict, id, slot);      /* compact dictionary */
            SMS_mapROMBank(s_bank);
            fmv_uploads++;
        } else switch (op) {
        case OP_TICK:
            fmv_tick++;
            return 1;
        case OP_WAIT:
            s_wait = *s_ptr++;
            fmv_tick++;
            return 1;
        case OP_PALBG:
            SMS_loadBGPalette(s_ptr);
            s_ptr += 16;
            break;
        case OP_PALSPR:
            SMS_loadSpritePalette(s_ptr);
            s_ptr += 16;
            break;
        case OP_DISPOFF:
            SMS_displayOff();
            break;
        case OP_DISPON:
            SMS_displayOn();
            break;
        case OP_CLEAR:
            SMS_VRAMmemset(XYtoADDR(0, 0), 0, 32 * 28 * 2);
            SMS_VRAMmemset(0x4000, 0, 32);        /* slot 0 = blank */
            break;
        case OP_NEXTBANK:
            s_bank++;
            SMS_mapROMBank(s_bank);
            s_ptr = (const unsigned char *)0x8000;
            break;
        default:                                  /* OP_END or corrupt stream */
            fmv_active = 0;
            return 0;
        }
        }
    fmv_active = 0;      /* ran off the end of the stream: stop, do not hang */
    return 0;
}
