/* Compact tile storage (tools/tilepack.py): bit-planes, types sorted by id */
#ifndef TILEDEC_H
#define TILEDEC_H
typedef struct {
    unsigned char bank0, bank1, bank2, bank3;  /* first bank of each type region */
    unsigned int  start1, start2, start3;      /* first id of types 1, 2, 3 */
} TileDict;
/* Upload tile `id` of dictionary `d` into VRAM tile `vram_tile`.  Pages ROM
 * banks: callers re-map their own bank afterwards. */
void tile_upload(const TileDict *d, unsigned int id, unsigned int vram_tile);
#endif
