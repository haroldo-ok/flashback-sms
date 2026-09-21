#ifndef FRAMES_H
#define FRAMES_H
#include <stdint.h>
typedef struct { uint8_t bank; uint16_t off; } FrameRec;
#define NUM_FRAMES 346
extern const FrameRec frameTable[NUM_FRAMES];
#endif
