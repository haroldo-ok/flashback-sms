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
#include "psg.h"

#define ST_CUTSCENE 0
#define ST_TITLE    1
#define ST_SIM      2

unsigned char game_state;
unsigned char seq_pos;            /* position in the intro sequence */
unsigned int  last_clip_ticks;    /* ticks the previous clip ran (tests) */
unsigned int  last_clip_uploads;
unsigned char is_pal;
unsigned int  pge_sum[NUM_LEVELS];  /* per level: checked against the original engine */
unsigned int  pge_act[NUM_LEVELS];
unsigned int  pge_cnt[NUM_LEVELS];
unsigned char pge_loaded;
unsigned char self_test;
unsigned char return_to_sim;
unsigned char start_after_clip;
unsigned char level_sel;         /* 0..4: the level chosen on the title screen */
/* levels 4 and 5 are two parts each; a game starts at the first of them */
static const unsigned char level_start[5] = { 0, 1, 2, 3, 5 };
#define NUM_SEL 5        /* skip button seen released during this clip */  /* the level intro is playing */    /* a cutscene the game asked for, mid-play */        /* boot diagnostics ran (button 1 held) */       /* levels whose tables are built */
volatile unsigned char frames_elapsed;  /* ++ in the frame interrupt */
unsigned int  ticks_behind;             /* cutscene lag in ticks (tests) */

volatile unsigned char watchdog;     /* frames since the main loop last ran */

/* The pad as the frame interrupt saw it (SMSlib reads it just before calling
 * us), kept until the main loop takes it.  Reading the pad only when the
 * loop gets round to it lost presses: one pass can outlast a whole tap (a
 * cutscene picture change takes 15+ frames, a game tick over 2). */
static volatile unsigned int keys_pressed_latch;   /* new presses */
static volatile unsigned int keys_held_latch;      /* down at any point */

/* frames after the title appears during which presses are ignored: a burst
 * of presses aimed at the intro should not fall through and start a level */
#define TITLE_GUARD 15
static unsigned char title_guard;

/* the buttons newly pressed since the last call */
static unsigned int input_take_presses(void)
{
    unsigned int p;
    __critical { p = keys_pressed_latch; keys_pressed_latch = 0; }
    return p;
}

unsigned int input_take_held(void)
{
    unsigned int h;
    __critical { h = keys_held_latch; keys_held_latch = 0; }
    return h;
}

static void frame_irq(void)
{
    keys_pressed_latch |= SMS_getKeysPressed();
    keys_held_latch |= SMS_getKeysStatus();
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
#define INTRO_LEN (sizeof intro_seq)

static void hide_sprites(void)
{
    SMS_initSprites();
    SMS_finalizeSprites();
    SMS_copySpritestoSAT();
}

#if HAS_MENU
/* Draw the chosen level's text over the title picture.  The tiles go into the
 * low VRAM slots (free here: the title screen uses no sprites) and the cells
 * select the sprite palette, so the text keeps its own colours. */
#define MENU_ROW 23
#define MENU_COL 2
static void draw_level_text(void)
{
    const unsigned char *cells;
    unsigned char i;
    SMS_mapROMBank(MENU_BANK);
    cells = (const unsigned char *)(MENU_ADDR + MENU_NTILES * 32 + level_sel * MENU_CELLS);
    SMS_setNextTileatXY(MENU_COL, MENU_ROW);
    for (i = 0; i < MENU_CELLS; i++) SMS_setTile(cells[i] | 0x0800);   /* sprite palette */
}
#endif

/* the title screen is packed as a room of its own (level 99) */
static void enter_title(void)
{
    unsigned char idx = room_find(99, 0);
    SMS_displayOff();
    SMS_setSpriteMode(SPRITEMODE_NORMAL);
    hide_sprites();
    if (idx != 0xFF) room_load(idx);
    SMS_setBGScrollY(0);
#if HAS_MUSIC
    music_play(TITLE_MUSIC);           /* the menu theme */
#endif
#if HAS_MENU
    SMS_mapROMBank(MENU_BANK);
    SMS_loadTiles((const unsigned char *)MENU_ADDR, 0, MENU_NTILES * 32);
    SMS_loadSpritePalette(menu_palette);
    draw_level_text();
#endif
    SMS_displayOn();
    game_state = ST_TITLE;
    /* presses made while the intro ended and this loaded were meant for the
     * intro: without this, mashing the skip button started the game too */
    input_take_presses();
    title_guard = TITLE_GUARD;
}

/* discard_presses: the press that led here (starting a level, the game
 * asking for a cutscene) must not also skip the clip.  The boot's intro keeps
 * them, so a press made while the ROM starts up skips it. */
static void play_clip(unsigned char clip, unsigned char discard_presses)
{
    if (discard_presses) input_take_presses();
#if HAS_MUSIC
    {   /* the score the game itself uses for this cutscene */
        unsigned char id = fmv_clip_id[clip];
        music_play(id < CUT_MUSIC_COUNT ? cut_music[id] : NO_MUSIC);
    }
#endif
    fmv_start(clip);
    game_state = ST_CUTSCENE;
}

/* the next clip of the intro that is actually in this build */
static unsigned char next_intro_clip(void)
{
    unsigned char c;
    while (seq_pos < INTRO_LEN) {
        c = fmv_find(intro_seq[seq_pos]);
        if (c != 0xFF) return c;
        seq_pos++;                       /* not built in: skip it */
    }
    return 0xFF;
}

static void clip_finished(void)
{
    last_clip_ticks = fmv_tick;
    last_clip_uploads = fmv_uploads;
    fmv_stop();
    if (return_to_sim) {            /* back to the game that triggered it */
        return_to_sim = 0;
        music_stop();               /* the cutscene's score ends with it */
        sim_resume();
        game_state = ST_SIM;
        return;
    }
    if (start_after_clip) {         /* level 1's own intro just finished */
        start_after_clip = 0;
        sim_start(level_start[level_sel]);
        game_state = ST_SIM;
        return;
    }
    if (seq_pos < INTRO_LEN) {
        unsigned char c;
        seq_pos++;
        c = next_intro_clip();
        if (c != 0xFF) { play_clip(c, 0); return; }
    }
    enter_title();
}

void main(void)
{
    unsigned int pressed;
    unsigned char tick_acc = 0, e;

    SMS_displayOff();
    SMS_useFirstHalfTilesforSprites(1);
    SMS_setSpriteMode(SPRITEMODE_NORMAL);
    hide_sprites();
    /* installed first, so a press during the PAL check (the first ~12
     * frames) is latched and skips the intro like any other */
    SMS_setFrameInterruptHandler(frame_irq);
    is_pal = detect_pal();

    /* The self-test builds every level's object table and runs the ported
     * interpreter against the recorded demo - about a minute with nothing on
     * screen.  It is a BUILD option (make SELF_TEST=1), not a held button: a
     * pad that reports a button pressed at power-on would otherwise make a
     * normal boot look frozen. */
#if SELF_TEST
    self_test = 1;
    {
        unsigned char l;
        for (l = 0; l < NUM_LEVELS; l++) {
            pge_load_level(l);
            pge_sum[l] = pge_checksum;
            pge_act[l] = pge_active;
            pge_cnt[l] = pge_num;
            pge_loaded = l + 1;
        }
#if HAS_LOGIC
        logic_start(LOGIC_LEVEL);
        while (logic_step()) { }
#endif
    }
    frames_elapsed = 0;   /* the harness ran for a minute: that is not cutscene lag */
#endif
    seq_pos = 0;
    room_index = 0;
    {   /* a build without cutscenes starts in the room viewer */
        unsigned char c0 = (FMV_NUM_CLIPS > 0) ? next_intro_clip() : 0xFF;
        if (c0 == 0xFF) { seq_pos = INTRO_LEN; enter_title(); }
        else play_clip(c0, 0);
    }

    for (;;) {
        /* A game tick still takes longer than its 2 frames, so one is nearly
         * always due: waiting for the next vblank first only wasted up to a
         * frame per pass.  Wait only when there is nothing to run yet. */
        unsigned char waited = 0, n;
        watchdog = 0;
        if (game_state != ST_SIM || tick_acc + frames_elapsed < 2) {
            SMS_waitForVBlank();
            waited = 1;
        }
        pressed = input_take_presses();

        /* real frames since the last loop (a heavy tick can overrun one) */
        e = frames_elapsed; frames_elapsed = 0;
        if (!e && waited) e = 1;
        if (e > 12) e = 12;
        /* the score and effects advance one 60 Hz step per frame that passed:
         * the loop no longer runs once per frame, and their tempo must not
         * follow the game's speed */
        for (n = e; n; n--) { music_frame(); sfx_frame(); }

        /* PAUSE only means something while playing; one pressed on the title
         * or during a cutscene would otherwise quit the game on its first frame */
        if (game_state != ST_SIM && SMS_queryPauseRequested()) SMS_resetPauseRequest();

        if (game_state == ST_CUTSCENE) {
            /* streams are 60 Hz ticks: run 5 (NTSC) or 6 (PAL) ticks per 5
             * frames, and catch up after an overrun during the cheap wait
             * ticks that follow each ~12 fps picture change */
            /* either button skips, as either starts a level on the title */
            unsigned char alive = 1, budget = 3, skip = (pressed & (PORT_A_KEY_1 | PORT_A_KEY_2)) != 0;
            tick_acc += e * (is_pal ? 6 : 5);
            while (tick_acc >= 5 && alive && budget-- && !skip) {
                tick_acc -= 5;
                alive = fmv_step();
                /* a press latched while that tick ran (a picture change can
                 * take 15+ frames) skips at once rather than a pass later */
                if (keys_pressed_latch & (PORT_A_KEY_1 | PORT_A_KEY_2)) skip = 1;
            }
            ticks_behind = tick_acc / 5;
            if (tick_acc > 200) tick_acc = 200;    /* 40-tick cap; the 3-step budget stops spirals */
            if (skip) {
                input_take_presses();               /* it must not also start the next screen */
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
            /* a small backlog only: a bigger one plays back as a fast-forward
             * burst as soon as a lighter room lets the game catch up */
            if (tick_acc > 4) tick_acc = 4;
            /* the game can ask for a cutscene (picking up an item does) */
            if (logic_cutscene != 0xFFFF) {
                unsigned char c = (FMV_NUM_CLIPS > 0) ? fmv_find((unsigned char)logic_cutscene) : 0xFF;
                logic_cutscene = 0xFFFF;
                if (c != 0xFF) {
                    return_to_sim = 1;
                    tick_acc = 0;
                    SMS_setSpriteMode(SPRITEMODE_NORMAL);
                    hide_sprites();
                    play_clip(c, 1);
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
        } else {                    /* title screen: pick a level, any button starts it */
            if (title_guard) {
                title_guard = (title_guard > e) ? title_guard - e : 0;
                pressed = 0;
            }
#if HAS_MENU
            if ((pressed & PORT_A_KEY_UP) && level_sel) { level_sel--; draw_level_text(); }
            else if ((pressed & PORT_A_KEY_DOWN) && level_sel + 1 < NUM_SEL
                     && level_start[level_sel + 1] < NUM_LEVELS) { level_sel++; draw_level_text(); }
#endif
            if (pressed & (PORT_A_KEY_1 | PORT_A_KEY_2)) {
                unsigned char lv = level_start[level_sel];
                unsigned char c = fmv_find(level_cutscene[lv]);
                if (c != 0xFF) { start_after_clip = 1; play_clip(c, 1); }
                else { sim_start(level_start[level_sel]); game_state = ST_SIM; }
            }
        }
    }
}

SMS_EMBED_SEGA_ROM_HEADER(9999, 0);
SMS_EMBED_SDSC_HEADER_AUTO_DATE(0, 1, "port", "Flashback SMS", "DOS demo port - milestone 1");
