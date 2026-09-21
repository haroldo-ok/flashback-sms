# Flashback (DOS demo) -> Sega Master System port — sources

Level 1 of the DOS demo (fbdemous.zip / dosgames.com) ported to the SMS with
devkitSMS + SDCC. No placeholders: backgrounds, sprites, palettes, collision
grids, room links and item icons are all converted from the real game data.

## Layout

- `sms/src/main.c`   – game code (player moveset, physics, ledge grab, room
  transitions, item grabbing, VRAM/bank management)
- `sms/gen/`         – generated C data: room banks 2–39 (tiles + tilemaps +
  CT grid + links), sprite banks 40–42 (Conrad frames, normal + x-flipped),
  icon bank 43, plus roomtable/roompals/frametable/itemtable
- `sms/Makefile`     – banked ROM build (banks 2–43, contiguous pages)
- `sms/tests/`       – scripted emulator playtest (smstest)
- `tools/`           – Python converters that produced `sms/gen` from the DOS
  data (`emit_rooms3.py` backgrounds, `emit_sprites2.py` sprites,
  `emit_icons.py` item icons, `fbextract.py` ABA/BNQ/LEV/SPR unpackers,
  `sms_convert.py` palette quantisation)

## Building

Toolchain: SDCC (>= 4.2) with devkitSMS (SMSlib/PSGlib, crt0_sms.rel,
ihx2sms). Then:

    cd sms && make

produces `game.sms` (704 KB banked ROM).

## Regenerating the data

The converters expect the DOS demo data in `assets/DATA` (DEMO_UK.ABA,
LEVEL1.LEV, LEVEL1.CT, PERSO.SPR, GLOBAL.ICN, PERSO.OFF, ... from
fbdemous.zip), plus the engine-accurate raster dumps (`work/dump0`,
`work/tileraw`) captured from an instrumented build of cyxx/REminiscence
(the RE clone was used as the format/logic oracle; its instrumented
`video.cpp`/`dump.cpp`/`game.cpp` dumped per-room decoded rasters, palettes
and sprite draws). Then:

    python3 tools/emit_rooms3.py    # room banks 2–39 + roomtable/roompals
    python3 tools/emit_sprites2.py  # sprite banks 40–42 + frametable
    python3 tools/emit_icons.py     # icon bank 43 + itemtable

## VRAM map (448 BG tile slots)

- 0–399   room background tiles (per-room bank)
- 400–443 player sprite frame (uploaded on frame change, <= 15 tiles)
- 444–447 current room item icon

## Controls

D-pad move, button 1 jump, button 2 run/shoot/grab; Up or 1 while hanging
climbs a ledge, Down drops.
