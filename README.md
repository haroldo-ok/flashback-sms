# Flashback (DOS demo) → Sega Master System

**Status: milestone 3 in progress.** The cutscenes, the room backgrounds of the
demo's three levels, and a *replay* of real level-1 gameplay (Conrad, monsters
and objects as hardware sprites) all run from a 4 MB ROM, checked pixel-exact
against a host model of the same ROM bytes.  The **first slice of the game logic** now runs on the
Z80: each level's live object table is built from the level data exactly as
the original engine builds it, and the result is checked against the engine.
The rest of the logic (the object script interpreter, collision, input) is not
ported yet, so the gameplay you see is still the recorded replay, and there is
no sound.

## Build
```
make fbdump                        # headless extractor (links REminiscence, GPL)
make capture DEMO=path/to/DATA     # real engine renders -> capture/*.fbv, rooms_L*.fbr
make convert                       # cutscene encoder + room converter + bank packer
make verify                        # decode the packed bytes, compare with the captures
make                               # -> flashback.sms (4096 KB)
make check                         # emulator playtest + pixel-exact comparison
```
Needs SDCC ≥ 4.2 and devkitSMS (`~/.devkitsms`), plus Python 3 with numpy and Pillow.

## What's in the ROM
| | |
|---|---|
| Cutscenes | all 34 in the demo. They share one tile dictionary (45,804 tiles, 1.47 MB), and VRAM acts as a cache for 768 cells. Mean error from ideal is 0.0–2.3% per clip. |
| Rooms | 163 room slots, 97 unique images, from levels 1, 3 and 5. The full 256×224 room fills the 32×28 nametable and scrolls vertically. |
| Simulated gameplay | the ported logic drives the sprite renderer: 3820 8x16 sprites (245 KB) keyed by animation number, streaming through 32 VRAM slots.  This replaced the recorded replay, which is no longer in the ROM. |
| Level data | object tables, node maps and animation records for all three levels (~50 KB each, bank-safe so no record straddles a 16K bank) |
| Code | 5.8 KB of the 32 KB code area; 4.2 KB of the 8 KB RAM (mostly the 180-entry live object table) |
| Space left | ~190 KB (the cutscene dictionary is merged at 4 pixels, saving 532 KB; cutscene error 0.96% mean) |

## Controls (viewer)
Boot plays the original intro sequence (0x40, then 0x0D + 0x4A); button 1 skips it.
In the room viewer: left/right changes room, up/down scrolls, button 1 plays the
next cutscene, and button 2 runs the ported game logic and draws what it
simulates (hold button 1 to leave).

## How gameplay graphics were derived
`fbdump replay` runs the original engine headless on the game's own recorded
demo inputs, with two hooks added to the (GPL) engine: one on every sprite blit
and one per drawn game frame.  Each trace holds the engine's composited layer,
its palette, and every sprite it drew, so the SMS converters never guess and
the same traces become the oracle for the game-logic port.

Findings that shaped the design:
* tiles are cut relative to each sprite's own origin, not the screen grid, so a
  sprite that merely moves costs no upload (uploads 11 -> 9 per frame)
* 8x16 sprites halved the sprite count to 18/frame and cut budget skips 179 -> 18
* objects, not characters, cause the 8-per-line overflow; with characters alone
  level 1 exceeds 8 on only 3% of frames
* sprite order rotates each frame so overflow flickers instead of vanishing,
  with Conrad pinned first; sprites are lost on 8% of frames, 0.2 per frame
* room palette index 0 is reserved so foreground tiles reliably hide sprites
  (it cost no colours: every room uses 15 or fewer)

## Game logic so far
`src/pge.c` ports `pge_loadForCurrentLevel()` and `pge_setupDefaultAnim()`: it
walks the level's InitPGE records, derives each object's flags, finds its entry
point in the node's object list, and takes the starting animation frame from
the animation record.  For every level the Z80's object count, active-object
count and a checksum over (entry point, flags, life, room, animation number)
match the original engine exactly.

## The interpreter, so far
`src/logic.c` ports the frame loop, the message queue, the animation advance,
the object-script walk and the opcode dispatch, plus a first tranche of
opcodes.  It is self-checking: the ROM carries, for every 30 Hz frame of the
recorded demo, the input byte the engine consumed and a checksum of every
object's state afterwards, so the port reports exactly where it first differs
from the original and which opcode it could not run.

The collision system is ported too: the 16x7 room grid from the level's CT
table, the collision slots that chain objects sharing a grid cell, grid
lookups across room edges, and the collide tests the opcodes use.

Objects can also write into the collision grid, and those writes last for the
rest of the level - but on the SMS the grid is in ROM, so modified spans are
kept in a small RAM overlay that every grid read consults.  Inventory lists
(pick up, reorder, drop) are ported as well.

Room tracking is ported: objects wrap across room edges through the CT links,
the per-room lists are maintained, and Conrad's room becomes the current one
at the *end* of the frame (the engine's `_loadMap`), which is why the first
frame still runs in the level's default room.  Entering a room also reactivates
the objects there, and the ones just above and below it.

A pending message also makes an object jump to the end of its current
animation before its script runs (`pge_messageAck`), which is how objects
react on the frame after they are hit; that is ported too.

Hit detection is ported too, both the melee form (`col_detectHit`) and the gun
(`col_detectGunHit`): sweeping the grid cells in front of or behind an object
until the shot meets solid ground or something it can hit, and sending
whatever it hits the message that matches the shooter's facing.

Current state: **the first 504 frames of the level-1 demo match the original
engine exactly** - every one of the 107 objects, on position, animation
number, object type, flags, room, animation sequence and entry point.  That is
about 17 seconds of real gameplay reproduced tick for tick.  At frame 504
every opcode the demo reaches is ported, so the remaining difference is
behavioural, to be chased the same way as the earlier ones.

Opcodes ported so far (~60 of the ~75 this demo reaches): the full input-test
group (0x01-0x0A, 0x35), the whole grid-collision family (0x0B-0x21, 0x28-0x2A),
message tests and sends (0x22-0x26, 0x6B, 0x6F), counters (0x3E, 0x3F, 0x44,
0x59), the Conrad-direction tests (0x78, 0x79), the collide tests (0x3D, 0x50,
0x7E, 0x7F), collision grid writes (0x36, 0x37), inventory (0x30, 0x73), the
gun variable (0x8A, 0x8B), inventory use and drop (0x31-0x34, 0x6D, 0x6E),
the hit tests (0x62, 0x63), the gun (0x64), the ground sweep (0x5F), kill and
room tests (0x4B, 0x4C), locate-message drop (0x60), grid snap (0x88),
touch-message (0x7C), cutscene queue (0x5A), sound as no-ops with the engine's return values
(0x7D, 0x87), 0x27, 0x2E, 0x43, 0x4D, 0x61 and 0x83.  `pge_execute`'s tail is ported too:
an object record can turn the object around, change its life and move it by
its own dx/dy.

## Sizing the interpreter port (measured, not estimated)
The engine's opcode table has 101 entries.  Counting which ones *actually
execute* (a hook in `pge_execute`, reported by `fbdump replay`) gives the real
scope, and it is most of the table:

| demo | frames | distinct opcodes | executions |
|---|---|---|---|
| level 1 | 1954 | 75 | 40,592 |
| level 5 | 2047 | 87 | 72,024 |
| level 3 | 2046 | 84 | 31,694 |

The requirement grows with playtime rather than concentrating in a few
opcodes - level 1 needs 22 to reach frame 25, 45 by frame 200, 75 by the end -
so the port has to be staged by first use and checked continuously, not built
from a "top N opcodes" shortcut.

To support that, each traced frame now also carries the state the interpreter
must reproduce: for every object its position, animation number, object type,
flags, room, animation sequence and entry point, plus the demo input byte for
that frame.  That is the tick-by-tick oracle the Z80 interpreter will be
checked against.

## A bug worth remembering
Firing the gun and picking up an item both "froze" the game, with the picture
intact and no watchdog red screen.  Neither was a hang: when the interpreter
meets an opcode that is not ported yet it sets a flag so the verification
harness can report the first divergence - and that flag was sticky and global,
so from then on **no object was processed at all**.  The machine kept running
and drawing the frame it had reached.  Shooting and picking things up are
exactly the actions whose scripts reach unported opcodes.

The flag now stops the harness only; while playing, an unported opcode is
skipped and the game carries on.  The opcodes level 1 actually needs were then
ported by checking the level's own object data against the implemented set -
27 were missing, including `pickupObject` itself.

## The full build: all five levels
The default build is the whole game's five levels in one 4 MB ROM (4,080 KB of
4,096 used), chosen from the title screen:

* the level select draws "LEVEL 1..5" with the game's own menu font, captured
  from the engine and drawn as background cells using the sprite palette,
  because the title screen's own text is part of its picture
* up/down choose, any button starts, and the level's own intro cutscene plays
  first where the game has one
* 193 rooms, object tables for all seven level parts, six sprite sets (Conrad,
  four monster types and the shared level objects), 27 cutscenes

Everything is stored losslessly compressed (tools/tilepack.py): tiles keep
only their non-constant bit-planes, a plane may be the complement of another,
and the Z80 rebuilds each tile exactly before uploading it.  That takes 46% off
the cutscene tiles, 31% off rooms and 33% off sprites.  The cutscene dictionary
is the reason it fits at all.

What did not fit: the second and third parts of the game intro and level 4's
36-second approach - the three longest clips - so the intro is the logos, and
the mission briefings and ending are out.  Level 2 has 256 objects and the live
table holds 255, so its last object is not simulated.

## The demo build
The default build is a self-contained demo of level 1:

* the intro cutscenes, then the game's own title screen (captured from the
  engine and converted like a room: 447 tiles, 10% pixel error on a
  photographic image)
* any button starts level 1, after level 1's own intro cutscene (clip 0x00)
* the level itself, its objects and animations; PAUSE returns to the title
* the other levels and the room viewer are not built in
* the cutscenes that level 1's own scripts ask for (opcode 0x5A) - the ones
  that play when you pick certain items up - are included: 15 of the 21 it
  can request.  The other six (0x03, 0x08, 0x13, 0x17, 0x1E, 0x1F) are not in
  the demo's data at all, so a request for one of those is skipped.
* items are visible and can be picked up.  The engine draws collectibles
  with its blit that ignores the foreground mask, so they sit on top of
  scenery; an SMS sprite cannot override a background tile's priority, so the
  priority bit is cleared on the few cells under each item when a room loads.
  Without that, an item tucked into foliage - the cube near the start of
  level 1 is one - was drawn but completely hidden.
* `DICT_MERGE ?= 0`: the cutscene tile merge is off.  It saved ~530 KB but
  flat-shaded polygon artwork is exactly the case it damages most, so the
  cutscenes are stored unmerged.

The whole thing is 2.7 MB (a 4 MB ROM image).

## Playing it
From the title screen any button starts the game.  The d-pad moves, button 2
is the action key, button 1 is the run key, both together are the third key,
and the console's PAUSE button returns to the title.

The input goes through the engine's own `pge_getInput`, which never sees a
diagonal: with both axes held it keeps the modifiers and the last purely
horizontal direction.  So the jump is **button 1 + up with no direction
held** - Conrad leaps forward the way he is facing, and that same command is
the running jump (build up speed with a direction, release it, then button 1 +
up).  Holding up *together* with a direction just keeps you running, exactly
as in the original.

A cutscene the game itself asks for (opcode 0x5A) is played by the front end
and returns to the game afterwards.  Playing starts where the level itself
starts (room 27), not where the recorded demo started.

## Simulated gameplay
`src/sim.c` draws the objects the interpreter simulates.  Every animation frame the engine can draw - all 1771 (animation, mirror)
pairs over 923 animation numbers, in both mirrored and unmirrored form, rendered straight from the game's sprite
resources rather than harvested from a recorded demo - is stored once as 8x16
sprite tiles positioned relative to the object's own position, so an object
can be drawn from its simulated state alone.  Building the table from a demo
was a false start: free play immediately reaches frames the demo never showed,
and Conrad simply vanished.  Two further traps: sprites are mirrored by flag bit 1 (0x02), the *effective*
mirror computed from the facing and the frame's own flip, not by the facing
bit itself - keying on the facing bit made the character face the wrong way.
And characters and level objects are numbered in the SAME animation space but
come from different resources, so the kind (flag bit 3) is part of the key
too; without it, items were drawn as frames of the player.  The renderer keeps 32 sprite slots in VRAM
and streams patterns in as animations change.  The recorded replay has been
removed from the ROM: this supersedes it.

Boot: the ROM goes straight to the intro.  The self-test (level tables plus
the interpreter checked against the recorded demo) is a **build option**,
`make SELF_TEST=1`, used by `make check`.  It used to be a button held at
boot, which was a bad idea: a pad that reports a button pressed at power-on
made a normal boot sit on a black screen for a minute and look frozen.

**Bank layout matters on real emulators.**  Everything gameplay needs - rooms,
level tables, sprites - is packed into the LOW banks, and the cutscene
dictionary and streams (by far the bulkiest data, and the only part a game can
do without) go above them.  An emulator that cannot reach the highest banks
therefore loses cutscenes rather than the game.  `make small` builds a
cut-down 2 MB ROM (level 1, no cutscenes) whose data ends around bank 70, for
emulators with a lower mapper limit.  `tools/`-built `banktest.sms` reports how
many banks an emulator actually reaches.

Known gap: a simulated frame still takes about 3.3 times its 30 Hz budget
(6.6 frames of CPU per 30 Hz step, down from 8.4).

`tools/smstest` now has a sampling profiler (`profstart` / `profstop` /
`profdump`) that attributes cycles to the program counter, so this is measured
rather than guessed.  Where a simulated frame goes:

| share | cycles/frame | |
|---|---|---|
| 56% | 219k | the interpreter's internals (collision prep, script walk, animation setup) |
| 18% | 71k | the per-frame object loops in `logic_step` |
| 18% | 70k | `sim_step`, the sprite renderer loop (was 148k) |

Optimisations that worked: computing the state checksum only for the
verification harness (25% of the harness frame), replacing the room-coordinate
divisions by 72 and 36 with comparison chains (9%), walking a pointer over the
object table instead of indexing it, which costs a 22-byte multiply per object
on a Z80 (5%), and reading sprite parts straight from ROM - re-mapping the bank
per part - instead of copying the part list into RAM first (7%).

No measurable difference: a hash index for grid cells, incremental grid
stepping, hoisting multiplies out of the sprite loops, and SDCC's
`--opt-code-speed`.

Profiling down to the assembly (bucket addresses mapped through `build/*.lst`)
shows the remaining cost is spread across SDCC's stack-frame addressing - most
hot instructions are `ix`-relative loads of locals, at 19 cycles each.  The
wins therefore come from doing less work, not from tightening expressions.

The largest so far: the renderer now walks the interpreter's own per-room
object list instead of scanning all 107 objects every frame, which more than
halved it (148k -> 70k, 12% off the whole frame).  The same treatment is the
obvious next step for the interpreter's own loops, where the remaining 219k
sits: collision prep, the script walk and animation setup, measured at 54k,
30k and 25k per frame respectively.

A warning for whoever continues: do not profile by disabling parts of the
frame.  Most parts change what the game simulates, so the run takes a
different, cheaper path and the timing is meaningless - that mistake sent
three optimisations at the wrong target before the profiler existed.

## Verified
- `make check`: every playtest assertion passes. Each clip the Z80 plays to its end consumes exactly the tick and upload counts that the host decoder derives from the same ROM bytes. Four high-motion cutscene frames match the reference decoder with 0 differing pixels (one matching tick out of 7–9 distinct frames), and so do four room views.
- Timing: the intro stays in sync over 1200 frames, with transient lag of at most 7 frames.
- Rebuilding from the captures reproduces `bank_data.bin` byte-for-byte.
- Game logic: the first 504 frames (~17 s) of the level-1 demo reproduce the
  original engine's state for all 107 objects, collision, hit detection (melee
  and gun), messages, inventory and room changes included.
- Level object tables: L1 107 objects / 26 active / checksum 0xBB1D, L3 176 / 43
  / 0xA0BB, L5 109 / 43 / 0xFFF4 - all three identical to the original engine.
- The object-script walks are bounded: unreadable level data (an emulator that
  cannot reach the bank) can no longer spin the frame forever.
- Picking up an item works: the item moves into Conrad's inventory and the
  frame keeps running.  It used to hang, because an object whose room was set
  directly by a script could end up in two room lists at once and the walk
  round that cycle never ended; each object now records which list it is on.
- Simulated gameplay runs in the emulator and responds to the controller:
  holding a direction moves Conrad and the renderer draws him (asserted in
  `make check`).

## Not verified / known gaps
- PAL (50 Hz): the cutscene path adjusts, but the replay runs 30 Hz ticks off a
  2-frame divider, so it would play ~20% slow on a PAL console.  Untested.
- The collision-grid overlay holds 24 modified spans (the engine has no such
  limit); a level that modifies more would diverge.
- Collision slots cap at 160 (the engine allows 255); level 1 stays under it,
  but a busier room could overflow and diverge.
- The live object table caps at 180 entries (level 3 needs 176); the full logic
  port will need a tighter RAM layout than 22 bytes per object.
- The inventory overlay (one frame in 1954) is drawn by the engine as 256
  full-screen blocks and exceeds the 64-sprite limit; it needs background
  rendering instead.
- `tools/smstest` is a local copy of the skill's emulator with two fixes needed
  for sprite work: the 8-sprites-per-line limit and the background priority bit.
- Real hardware and 4 MB-aware emulators such as Emulicious haven't been tried; only the headless `smstest` has.
- Level 1 rooms need up to 416 tiles, so similar tiles get merged. Worst case is 6.1% pixel error (room 52).

## Next milestones
1. Chase the behavioural difference at frame 504, then keep going in first-use
   order.
2. Speed up the simulated frame: it currently runs at about a third of 30 Hz
   because the renderer re-reads each object's data through bank switches.  Expect this to be the largest single piece of work in the project.
2. Load level tables when a level starts instead of all three at boot: the scan
   currently costs about two seconds of boot time.
3. Draw level objects into the background layer to relieve the 8-per-line limit.
4. PSG music and sound effects.
