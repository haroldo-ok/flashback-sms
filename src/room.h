/* Room background loader (format: tools/roomconv.py, packed by tools/mkdata.py) */
#ifndef ROOM_H
#define ROOM_H
#define ROOM_MAX_TILES 384         /* VRAM tiles 64..447; 0..63 are sprite patterns */
#define ROOM_SCROLL_MAX 32         /* 224-line room on a 192-line screen */
extern unsigned char room_index;   /* index into room_* tables */
extern unsigned char room_tiles;   /* tile count of the loaded room (tests) */
void room_load(unsigned char index);      /* display must be off */
unsigned char room_find(unsigned char level, unsigned char room);
#endif
