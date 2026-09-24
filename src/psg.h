/* PSG music: 60 Hz streams of ready-made sound chip writes (tools/midiconv.py) */
#ifndef PSG_H
#define PSG_H
void music_play(unsigned char track);   /* NO_MUSIC (0xFF) = silence */
void music_stop(void);
void music_frame(void);                 /* once per displayed frame */
void sfx_play(unsigned char id);        /* a sound effect, on voice 2 + noise */
void sfx_frame(void);
extern unsigned char sfx_playing;
extern unsigned char music_track;       /* what is playing, 0xFF = nothing */
#define NO_MUSIC 0xFF
#endif
