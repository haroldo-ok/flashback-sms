/*
 * Music playback.  tools/midiconv.py reduces the game's MIDI score to the
 * three square voices and the noise voice and stores it as a 60 Hz stream of
 * ready-made PSG bytes:
 *
 *   0x01..0x0F n : that many PSG bytes follow, written on this frame
 *   0x80|k       : nothing happens for k frames
 *   0x00         : end of track (loops back to the start)
 *
 * The stream lives in its own ROM bank, so this maps that bank while reading;
 * callers map their own afterwards.
 */
#include "SMSlib.h"
#include "data_index.h"
#include "psg.h"

__sfr __at 0x7F psg_port;

unsigned char music_track = NO_MUSIC;
unsigned char sfx_playing;
static const unsigned char *s_ptr;
static unsigned char s_bank, s_wait;
static const unsigned char *m_ptr;
static unsigned int  m_start;
static unsigned char m_bank, m_wait;

static void silence(void)
{
    psg_port = 0x9F;        /* attenuate all four channels */
    psg_port = 0xBF;
    psg_port = 0xDF;
    psg_port = 0xFF;
}

void music_stop(void)
{
    music_track = NO_MUSIC;
    silence();
}

void music_play(unsigned char track)
{
#if HAS_MUSIC
    if (track >= NUM_MUSIC || !music_bank[track]) { music_stop(); return; }
    if (track == music_track) return;            /* already playing */
    music_track = track;
    m_bank = music_bank[track];
    m_start = music_addr[track];
    m_ptr = (const unsigned char *)m_start;
    m_wait = 0;
    silence();
#else
    (void)track;
#endif
}

/* An effect owns voice 2 and the noise voice while it plays, so the score's
 * writes to those two are dropped (a tone latch carries a second data byte). */
static void music_write(unsigned char b)
{
    static unsigned char skip_data;
    if (b & 0x80) {
        unsigned char reg = (b >> 5) & 3;
        skip_data = (!(b & 0x10) && (reg >= 2)) ? 1 : 0;
        if (sfx_playing && reg >= 2) return;
    } else if (skip_data) {
        if (sfx_playing) return;
    }
    psg_port = b;
}

unsigned int sfx_calls, sfx_started;
void sfx_play(unsigned char id)
{
#if HAS_SFX
    sfx_calls++;
    if (id >= NUM_SFX) return;
    if (!sfx_bank[id]) return;                   /* nothing for this one */
    s_bank = sfx_bank[id];
    s_ptr = (const unsigned char *)sfx_addr[id];
    s_wait = 0;
    sfx_playing = 1;
    sfx_started++;
#else
    (void)id;
#endif
}

void sfx_frame(void)
{
#if HAS_SFX
    unsigned char b, n;
    if (!sfx_playing) return;
    if (s_wait) { s_wait--; return; }
    SMS_mapROMBank(s_bank);
    for (;;) {
        b = *s_ptr++;
        if (b == 0) {                            /* done: hand the voices back */
            sfx_playing = 0;
            psg_port = 0xDF;
            psg_port = 0xFF;
            return;
        }
        if (b & 0x80) { s_wait = (b & 0x7F) - 1; return; }
        n = b;
        while (n--) psg_port = *s_ptr++;
    }
#endif
}

void music_frame(void)
{
#if HAS_MUSIC
    unsigned char b, n, guard = 0;
    if (music_track == NO_MUSIC) return;
    if (m_wait) { m_wait--; return; }
    SMS_mapROMBank(m_bank);
    for (;;) {
        b = *m_ptr++;
        if (b == 0) {                            /* end: loop */
            m_ptr = (const unsigned char *)m_start;
            if (++guard > 2) { music_stop(); return; }   /* empty track */
            continue;
        }
        if (b & 0x80) { m_wait = (b & 0x7F) - 1; return; }
        n = b;
        while (n--) music_write(*m_ptr++);
    }
#endif
}
