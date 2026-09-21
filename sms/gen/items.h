#ifndef ITEMS_H
#define ITEMS_H
#include <stdint.h>
typedef struct { uint8_t room; uint8_t x; uint8_t y; uint8_t icon; uint8_t slot; } ItemRec;
#define NUM_ITEMS 15
extern const ItemRec itemTable[NUM_ITEMS];
#endif
