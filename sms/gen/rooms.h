// generated
#ifndef ROOMS_H
#define ROOMS_H
#include <stdint.h>
#define NUM_ROOMS 38
typedef struct { uint8_t bank; uint16_t ntiles; uint8_t room; } RoomInfo;
extern const RoomInfo roomTable[NUM_ROOMS];
extern const int8_t roomLinks[NUM_ROOMS][4];
extern const uint8_t roomPalette[38][16];
#endif
