/*
 * Flashback (DOS demo) for the Sega Master System - milestone 1:
 * the converted cutscenes and every room of the demo's three levels,
 * running on the real ROM layout (4 MB, data in banks 2..).
 *
 *   boot:  plays the intro as the original does (0x40 logos, 0x0D + 0x4A)
 *          button 1 skips a cutscene
 *   rooms: left/right = previous/next room, up/down = scroll,
 *          button 1 = play the next cutscene ("cinema"),
 *          button 2 = play the traced level-1 gameplay demo (sprites)
 */
#include "SMSlib.h"
#include "data_index.h"
#include "fmv.h"
#include "room.h"
#include "pge.h"
#include "logic.h"
#include "sim.h"

#define ST_CUTSCENE 0
#define ST_TITLE    1
#define ST_SIM      2

unsigned char game_state;
unsigned char seq_pos;            /* position in the intro sequence */
unsigned int  last_clip_ticks;    /* ticks the previous clip ran (tests) */
unsigned int  last_clip_uploads;
unsigned char is_pal;
unsigned int  pge_sum[3];       /* per level: checked against the original engine */
unsigned int  pge_act[3];
unsigned int  pge_cnt[3];
unsigned char pge_loaded;
unsigned char self_test;
unsigned char return_to_sim;
unsigned char start_after_clip;  /* the level intro is playing */    /* a cutscene the game asked for, mid-play */        /* boot diagnostics ran (button 1 held) */       /* levels whose tables are built */
volatile unsigned char frames_elapsed;  /* ++ in the frame interrupt */
unsigned int  ticks_behind;             /* cutscene lag in ticks (tests) */

volatile unsigned char watchdog;     /* frames since the main loop last ran */

static void frame_irq(void)
{
    frames_elapsed++;
    /* If a frame never finishes, say so instead of sitting on a black screen:
     * a red backdrop means the game is stuck, not that nothing is happening. */
    if (watchdog < 255) watchdog++;
    if (watchdog == 180) SMS_setBGPaletteColor(0, 0x03);
}

__sfr __at 0x7E vcounter_port;
__sfr __at 0xDC joypad_port;      /* player 1: bit 4 = button 1, active low */

/* 50/60 Hz: in VBlank the V counter jumps backwards - 0xDA->0xD5 on NTSC,
 * 0xF2->0xBA on PAL.  A backward jump of more than 16 lines (that is not the
 * end-of-frame wrap to 0) means PAL. */
static unsigned char detect_pal(void)
{
    unsigned char v, prev = 0;
    unsigned int i;
    for (i = 0; i < 12000; i++) {
        v = vcounter_port;
        if (v < prev && v >= 0x80 && (unsigned char)(prev - v) > 16) return 1;
        prev = v;
    }
    return 0;
}

static const unsigned char intro_seq[] = { 0x40, 0x0D, 0x4A };
#define LEVEL1_CUTSCENE 0x00
#define INTRO_LEN (sizeof intro_seq)

static void hide_sprites(void)
{
    SMS_initSprites();
    SMS_finalizeSprites();
    SMS_copySpritestoSAT();
}

/* the title screen is packed as a room of its own (level 99) */
static void enter_title(void)
{
    unsigned char idx = room_find(99, 0);
    SMS_displayOff();
    SMS_setSpriteMode(SPRITEMODE_NORMAL);
    hide_sprites();
    if (idx != 0xFF) room_load(idx);
    SMS_setBGScrollY(0);
    SMS_displayOn();
    game_state = ST_TITLE;
}

static void play_clip(unsigned char clip)
{
    fmv_start(clip);
    game_state = ST_CUTSCENE;
}

static void clip_finished(void)
{
    last_clip_ticks = fmv_tick;
    last_clip_uploads = fmv_uploads;
    fmv_stop();
    if (return_to_sim) {            /* back to the game that triggered it */
        return_to_sim = 0;
        sim_resume();
        game_state = ST_SIM;
        return;
    }
    if (start_after_clip) {         /* level 1's own intro just finished */
        start_after_clip = 0;
        sim_start();
        game_state = ST_SIM;
        return;
    }
    if (seq_pos < INTRO_LEN) {
        seq_pos++;
        if (seq_pos < INTRO_LEN) {
            unsigned char c = fmv_find(intro_seq[seq_pos]);
            if (c != 0xFF) { play_clip(c); return; }
        }
    }
    enter_title();
}

void main(void)
{
    unsigned int keys, pressed;
    unsigned char tick_acc = 0, e;

    SMS_displayOff();
    SMS_useFirstHalfTilesforSprites(1);
    SMS_setSpriteMode(SPRITEMODE_NORMAL);
    hide_sprites();
    is_pal = detect_pal();
    SMS_setFrameInterruptHandler(frame_irq);

    /* The self-test builds every level's object table and runs the ported
     * interpreter against the recorded demo - about a minute with nothing on
     * screen.  It is a BUILD option (make SELF_TEST=1), not a held button: a
     * pad that reports a button pressed at power-on would otherwise make a
     * normal boot look frozen. */
#if SELF_TEST
    self_test = 1;
    {
        unsigned char l;
        for (l = 0; l < NUM_LEVELS && l < 3; l++) {
            pge_load_level(l);
            pge_sum[l] = pge_checksum;
            pge_act[l] = pge_active;
            pge_cnt[l] = pge_num;
            pge_loaded = l + 1;
        }
#if HAS_LOGIC
        logic_start();
        while (logic_step()) { }
#endif
    }
#endif
    seq_pos = 0;
    room_index = 0;
    {   /* a build without cutscenes starts in the room viewer */
        unsigned char c0 = (FMV_NUM_CLIPS > 0) ? fmv_find(intro_seq[0]) : 0xFF;
        if (c0 == 0xFF) { seq_pos = INTRO_LEN; enter_title(); }
        else play_clip(c0);
    }

    for (;;) {
        watchdog = 0;
        SMS_waitForVBlank();
        keys = SMS_getKeysStatus();
        pressed = SMS_getKeysPressed();

        /* real frames since the last loop (a heavy tick can overrun one) */
        e = frames_elapsed; frames_elapsed = 0;
        if (!e) e = 1;
        if (e > 12) e = 12;

        if (game_state == ST_CUTSCENE) {
            /* streams are 60 Hz ticks: run 5 (NTSC) or 6 (PAL) ticks per 5
             * frames, and catch up after an overrun during the cheap wait
             * ticks that follow each ~12 fps picture change */
            unsigned char alive = 1, budget = 3;
            tick_acc += e * (is_pal ? 6 : 5);
            while (tick_acc >= 5 && alive && budget--) {
                tick_acc -= 5;
                alive = fmv_step();
            }
            ticks_behind = tick_acc / 5;
            if (tick_acc > 200) tick_acc = 200;    /* 40-tick cap; the 3-step budget stops spirals */
            if (pressed & PORT_A_KEY_1) {
                if (seq_pos < INTRO_LEN) seq_pos = INTRO_LEN - 1;   /* skip the rest of the intro */
                alive = 0;
            }
            if (!alive) { clip_finished(); tick_acc = 0; }
        } else if (game_state == ST_SIM) {
            /* the simulated game runs at 30 Hz: one step per 2 SMS frames */
            unsigned char budget = 2;
            tick_acc += e;
            while (tick_acc >= 2 && budget--) {
                tick_acc -= 2;
                sim_step();
            }
            ticks_behind = tick_acc >> 1;
            if (tick_acc > 100) tick_acc = 100;
            /* the game can ask for a cutscene (picking up an item does) */
            if (logic_cutscene != 0xFFFF) {
                unsigned char c = (FMV_NUM_CLIPS > 0) ? fmv_find((unsigned char)logic_cutscene) : 0xFF;
                logic_cutscene = 0xFFFF;
                if (c != 0xFF) {
                    return_to_sim = 1;
                    tick_acc = 0;
                    SMS_setSpriteMode(SPRITEMODE_NORMAL);
                    hide_sprites();
                    play_clip(c);
                }
            }
            /* leave with PAUSE: button 1 is the run key while playing */
            if (SMS_queryPauseRequested()) {
                SMS_resetPauseRequest();
                tick_acc = 0;
                SMS_setSpriteMode(SPRITEMODE_NORMAL);
                hide_sprites();
                enter_title();
            }
        } else {                    /* title screen: any button starts level 1 */
            if (pressed & (PORT_A_KEY_1 | PORT_A_KEY_2)) {
                unsigned char c = fmv_find(LEVEL1_CUTSCENE);
                if (c != 0xFF) { start_after_clip = 1; play_clip(c); }
                else { sim_start(); game_state = ST_SIM; }
            }
        }
    }
}

SMS_EMBED_SEGA_ROM_HEADER(9999, 0);
SMS_EMBED_SDSC_HEADER_AUTO_DATE(0, 1, "port", "Flashback SMS", "DOS demo port - milestone 1");
