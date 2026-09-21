/* Cutscene tile-stream player (format: tools/fmvenc.py) */
#ifndef FMV_H
#define FMV_H

extern unsigned char fmv_active;     /* 1 while a clip is playing */
extern unsigned int  fmv_tick;       /* ticks consumed in the current clip */
extern unsigned int  fmv_uploads;    /* tiles uploaded in the current clip (tests) */

unsigned char fmv_find(unsigned char cutscene_id);   /* -> clip index or 0xFF */
void fmv_start(unsigned char clip);
unsigned char fmv_step(void);        /* one 60 Hz tick; returns 0 at end of clip */
void fmv_stop(void);

#endif
