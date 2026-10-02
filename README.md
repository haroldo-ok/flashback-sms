# Flashback → Sega Master System

A port of the DOS version of *Flashback* (Delphine Software, 1992) to the Sega
Master System: all five levels in one 4 MB cartridge image, with the original's
cutscenes, rooms, sprites, game logic, music and sound effects converted from
the game's own data files.

The game logic is a port of the open-source REminiscence engine's object-script
interpreter to the Z80, checked frame by frame against the original engine
running on the same inputs.

**The game's data is not included.**  You need your own copy of the DOS game;
the build reads its files and produces the ROM.

## Status

Playable, with known gaps:

* All five levels, chosen from a level select on the title screen.
* Conrad walks, runs, jumps, draws and fires the gun, picks up and uses items,
  and rides lifts; monsters and level machinery run on the ported scripts.
* 98–99 % of the script steps in every level are ported opcodes.  An unported
  step is skipped, so a script keeps running, but the machinery that relies on
  it may misbehave.
* **The simulation runs at roughly a third of the original's speed** (see
  *Performance*).  It is correct but slow.
* The intro is the logos only, and the mission briefings and the ending are not
  in the ROM: they do not fit (see *What fits*).

## Controls

| | |
|---|---|
| D-pad | walk; up/down climb, crouch, take ledges |
| Button 1 | run while moving; **button 1 + up** jumps (from a standstill or after running); **button 1 + up/down** rides a lift while standing on it |
| Button 2 | action: draw and fire the gun, pick up, operate |
| Buttons 1 + 2 | use the current inventory item |
| PAUSE | open the inventory |

In the inventory: left/right choose an item, button 2 makes it the current item
and returns, button 1 or PAUSE returns, both buttons quit to the title screen.
On the title screen, up/down choose the level and any button starts it; button 1
skips a cutscene.

The engine reads the pad through its own `pge_getInput`, which never sees a
diagonal: with both axes held it keeps the last purely horizontal direction.
So up plus a direction keeps you running, as in the original.

## What's in the ROM

| | |
|---|---|
| Cutscenes | 27 clips: the logos, the level intros, and the short in-game clips the level scripts can request (item pickups, lifts, teleports, taxis) |
| Rooms | 193 rooms from the five levels' maps, plus the title screen |
| Level data | object tables, script records and animation data for all seven level parts |
| Sprites | six sets — Conrad, the four monster types, and the level objects — 3,255 animation frames sharing 23,068 8×8 tiles |
| Sound | 11 music tracks and 56 sound effects |
| Item display | 30 icons, 27 font glyphs and 128 level texts |
| Code | 32,079 of 32,768 bytes; 7,853 of 8,192 bytes of RAM |
| ROM | 4,096 KB of 4,096 — completely full |

## Building

Needs SDCC ≥ 4.2 and devkitSMS (`~/.devkitsms`, with `~/.devkitsms/bin` on the
path), a C++ compiler for the extractor, and Python 3 with numpy and Pillow.

```
tools/rebuild_full.sh /path/to/flashback/data   # extract and convert everything
make pack                                        # pack the ROM data
make                                             # -> flashback.sms (4 MB)
make check                                       # the verification gate
```

`tools/rebuild_full.sh` runs the extractor on your copy of the game and every
converter after it; it takes a while (the cutscene captures are large).  Once
`gen/` is populated, `make pack && make` rebuilds the ROM in seconds.

`make check` builds a separate test ROM — with the verification data in and the
item display out, since the cartridge cannot hold both — runs it in the bundled
emulator, compares the results with the reference decoders and the original
engine, then rebuilds the shipping ROM.

## How it works

### Extraction
`tools/fbdump` links the REminiscence engine (GPL) headless, with hooks on every
sprite blit and every drawn frame.  It captures what the original engine
renders — cutscene frames, rooms, every sprite of every animation, the title
screen, menu text — and dumps level tables, sound samples and palettes, so the
converters never guess at a format.  The same hooks trace the engine's object
state frame by frame, which is the oracle for the logic port.

### Graphics
Everything is tiled and losslessly compressed (`tools/tilepack.py`): a tile
keeps only its non-constant bit-planes, a plane may be the complement of
another, and the Z80 rebuilds the exact tile before uploading it.  Flat-shaded
artwork compresses well — cutscene tiles shrink by 46 %, rooms by 31 % and
sprites by 33 % — and this is the main reason five levels fit.  The expanders
are in assembly: in C they took over half the CPU at cutscene cuts.

* **Cutscenes** (`tools/fmvenc.py`, `src/fmv.c`) are streams of tile uploads and
  tile-map writes over a shared tile dictionary, with VRAM used as a cache.
* **Rooms** (`tools/roomconv.py`, `src/room.c`) are a tile map plus the
  dictionary ids of their tiles, loaded with the display off.
* **Sprites** (`tools/animconv.py`, `tools/sprconv.py`, `src/sim.c`) are
  rendered from the simulated state: each object's animation number, mirror
  flag and sprite set select a list of 8×16 hardware sprites, which stream
  through a 22-slot cache in VRAM.
* Collectibles are drawn by the original over scenery; an SMS sprite cannot
  override a background tile's priority, so the priority bit is cleared on the
  cells under each item when a room loads.

### Game logic
`src/logic.c` ports the frame loop, the object-script interpreter and its
opcodes, the message queue, the collision grid and slots, room tracking,
inventory lists, hit detection (melee and gun) and the animation-driven sounds.
`src/pge.c` builds each level's live object table exactly as the engine does.

The SMS has 8 KB of RAM, so the live object table holds 255 objects in a
narrowed record, with the inventory links kept in a small side table.  Level 2
has 256 objects; its 256th is the level's controller object, which other
scripts rely on, so it moves into the slot of an object that is never loaded at
this difficulty.

### Sound
The PSG has three square voices and one noise voice, while the original has
MIDI music and sampled effects, so both are re-created:

* **Music** (`tools/midiconv.py`, `src/psg.c`): each MIDI score is reduced to
  three voices — the loudest three sounding notes, each note keeping the voice it
  already has — with percussion on the noise voice, stored as a 60 Hz stream of
  ready-made PSG writes.  Cutscenes play the score the game assigns them.
* **Effects** (`tools/sfxconv.py`): each sample's noisiness, pitch and loudness
  envelope become a tone or noise burst with the same shape in time.  Most
  effects are triggered by animations (a sound id in each animation's header),
  as in the original.

### Item display and inventory
`tools/hudconv.py` reads the icons, the menu font and every level's texts from
`global.icn`, `fb_txt.fnt` and the `.tbn` files.  Standing on an item shows its
icon and name where the original draws them, the current item's icon sits at the
top right, and PAUSE opens the inventory (`src/hud.c`).  They use the sprite
palette, which background cells can select, so they look right over any room.

### Bank layout
Everything gameplay needs is packed into the lower banks and the cutscenes,
the bulkiest data and the only part the game can do without, go above them.

## Verification

* **Against the original engine:** the recorded demo's first 504 frames on
  level 1 reproduce the engine's state for every object, every frame.  The level
  object tables match the engine for all seven parts.
* **Scripted runs:** `fbdump DATA script OUT LEVEL KEYFILE` plays a level from
  its own start through the original engine with a chosen key sequence (one mask
  byte per frame), and the port checks itself against that trace with
  `mkdata.py pack ... --logic TRACE --logic-level N` and `make SELF_TEST=1`.
  This verified the level-2 gun draw (170 frames) and the level-4 lift ride
  (315 frames).
* **Against reference decoders:** sampled cutscene frames and the title screen
  match pixel for pixel.
* **Emulator** (`tools/smstest`): a headless SMS emulator with the 8-sprites-
  per-line limit, background priority, PAUSE, sound-chip logging (`psglog`), a
  cycle-sampling profiler (`profstart`/`profstop`/`profdump`), and RAM that powers
  on with garbage rather than zeros.

## Performance
A simulated frame takes about 6.6 frames of CPU against a budget of 2, so the
game runs at roughly a third of full speed.  Profiling (`tools/smstest`) puts
most of the cost in the interpreter's collision preparation, script walk and
animation setup, spread across SDCC's stack-frame addressing rather than any
one hot loop.  The wins so far came from doing less work, such as walking the
room's own object list instead of every object, not from tightening code.

## What fits, and what does not
The SMS mapper tops out at 4 MB and the ROM is full.  Left out: the second and
third parts of the intro and level 4's long approach cutscene (the three
longest clips), the mission briefings and the ending, and 15 of the 26 music
tracks (only those the included cutscenes can request are packed).  Six of the
cutscenes the level scripts can ask for were also missing from the source data
used.  The code area is also nearly full (97.9 %).

## Lessons worth keeping
* **Check against the original, don't reason about it.**  Every stubborn bug —
  the level-2 gun, the lift, the missing sound effects — fell to running the
  original engine on the same inputs and finding the first frame where the
  port differs.  Reasoning from symptoms produced fixes that broke other things.
* **Feed test inputs the way the game does.**  Driving the engine through its
  demo-playback path changed the current room while the level loaded, so the
  start room's objects were never activated, and the lift could not be ridden
  in testing even though it worked in the game.
* **Indexes that are fine on level 1 are not proof.**  Several bugs were per-level
  data addressed with a pinned index, or tile bases that agreed only because
  level 1's rooms all hit the same cap.
* **A sticky "unsupported" flag looks exactly like a freeze.**  An unported opcode
  once halted every object for the rest of the session, with the screen intact.
* **Measure CPU time, don't infer it.**  Profiling by disabling parts of the frame
  changes what the game simulates, so the timing means nothing.
* **Edits that silently miss are dangerous.**  A slot-count change that never
  applied let sprite uploads overwrite the item icon; scripted edits now fail
  loudly when their target is absent.

## Known gaps
* Simulation speed, as above.
* The remaining unported opcodes: mostly level-specific machinery.
* The collision-grid overlay holds 24 modified spans and the collision slots
  160 (the engine allows more); a busy level could exceed them and diverge.
* PAL (50 Hz) consoles are untested; cutscenes adjust, gameplay may run slow.
* Not tested on real hardware.  It has been played in PicoDrive and in the bundled
  emulator.

## Licence
`tools/fbdump` links the REminiscence engine, which is GPL-licensed, and the
Z80 interpreter in `src/logic.c` is a port of that engine's code, so the GPL's
terms are worth checking before distributing either.  *Flashback* and its data
belong to their rights holders and are not distributed here; a ROM built from
them contains the game's converted graphics, levels and sound.
