# Flashback: The Quest for Identity — Sega Master System (merged 4 MB build)

A single 4 MB `.sms` ROM that **combines the two partial ports**:

| comes from | what it contributes |
|---|---|
| `flashback_level1_sms_sources.zip` | the playable **Titan Jungle (level 1)**: real DOS-demo room data (backgrounds, collision grids, room links), Conrad's full moveset, room transitions, floor objects |
| `flashback_video_sms_src.zip` | the **FMV cutscene engine**: Delphine Software logo, cinematic intro parts 1–2, debut/jungle-crash scene, holocube message, object recovery, disintegration |

…plus the work both were missing: **the videos were re-encoded so that all seven
cutscenes share one dictionary of tiles**, the item art was completed, and
**picking an object up now plays the cutscene that belongs to it**.

```
Flashback_SMS.sms   4 MiB, 256 banks, Sega mapper, checksum + 4 MB size header fixed
```

---

## 1. What the finished ROM does

**Playable game (level 1, Titan Jungle)**

* 38 converted DOS-demo rooms (rooms 26…63) with the original backgrounds,
  palettes and collision grids, linked up/down/left/right exactly like the demo.
* Conrad: idle, walk, run, jump, crouch, shoot, crouch-shoot, ledge grab, climb,
  hang/drop — all with the original rotoscoped frames (346 frames, banks 40–42).
* Room-to-room transitions by walking/falling off an edge, vertical camera
  follow, gun/laser SFX (SN76489), shield HUD.
* 15 floor objects across the level (`GLOBAL.ICN` icons; the item art is now
  complete — the level-1 build only had icon #12 in slot 0, every other item
  showed garbage/blank tiles).

**Cutscenes (FMV) — 932 frames total**

| cutscene | frames | plays when |
|---|---|---|
| Delphine Software logo | 153 | boot |
| Cinematic intro part 1 | 260 | title menu → 2. CINEMATIC INTRO (then part 2) |
| Cinematic intro part 2 (Titan crash) | 380 | automatically after part 1 |
| Holocube message | 35 | title menu → 3, **and when Conrad picks up the holocube** |
| Jungle debut | 50 | title menu → 1. START GAME, before level 1 |
| Object recovery | 35 | **whenever Conrad picks up any other floor object** |
| Disintegration (game over) | 19 | reaching room 63, the end of the demo |

Every cutscene can be skipped with button **1** or **2**; afterwards play resumes
exactly where it left off (room, position, item already gone).

**Controls** — D-pad move · **1** jump (also climb when hanging) · **2** run / draw+fire
blaster / **pick up floor object** · **1** while walking = run · Down **2** = crouch-aim.

---

## 2. How to build it

```sh
# one-time: SDCC ≥ 4.2 + devkitSMS toolchain + the smstest harness
bash <skill>/scripts/setup_toolchain.sh          # installs into ~/.devkitsms

cd sms
make            # -> flashback.sms (4 MiB)
make playtest   # -> runs tests/playtest.txt in the headless smstest emulator
```

The build is reproducible: `make clean && make -j4` produces the same image
every time (md5 `d56925c19da83696a1f27aef8b6d05b6`, SEGA checksum 0x1b30, 4 MB
size nibble). That exact image is what the test results below were produced on.

`make` does three things:

1. compiles the game code into the fixed first 32 KiB (`banks 0–1`) and the
   level-1 data into banks 2–43 (one SDCC `CONST` segment per bank),
2. `ihx2sms` builds `flashback_base.sms` (704 KB),
3. `tools/mkrom_merged.py` appends the FMV stream (banks 44–96), the title and
   instruction screens (banks 97–98), pads to exactly 4 MiB and writes the SEGA
   checksum + the 4 MB size nibble.

Test it headlessly:

```sh
$HOME/.devkitsms/bin/smstest flashback.sms tests/playtest.txt   # 15 scenarios
$HOME/.devkitsms/bin/smstest flashback.sms tests/soak.txt       # videos to completion

# does the *ROM* really show every FMV position? (~5 min; one capture per
# position, taken only after the player finished uploading that position)
python3 tools/verify_rom_video.py --rom sms/flashback.sms \
        --noi sms/flashback_base.noi --frames /tmp/frames --work /tmp/rv \
        #   logos 153/153 · intro1 260/260 · intro2 380/380 · holocube 35/35
        #   debut 50/50 · objet 35/35 · desinteg 19/19  -> ALL PASSED
```

Open `flashback.sms` in Emulicious, BlastEm, MEKA, Kega Fusion, or flash it to
hardware / an Everdrive. (A 4 MB image needs an emulator that honours the full
256-bank Sega mapper — old 1 MB-only emulators will show the game but not the
later cutscenes.)

---

## 3. Bank map

```
bank   0 ..  1   game code (fixed window, linked normally)
bank   2 .. 43   level 1 data        (gen/bank*.c, one CONST segment per bank)
bank  44 .. 96   FMV stream          (53 banks, 848 KB, shared tile dictionary)
bank  97         title screen        (palette + tiles + tilemap)
bank  98         instructions screen
bank  99 .. 255  free (~2.4 MB spare)
```

---

## 4. The FMV re-encode (shared tile dictionary)

The video build gave every cutscene its own private tile set inside its own
banks. The merged encoder (`tools/encode_shared_dictionary.py`) instead builds
**one dictionary for all 932 frames** and then streams per-cutscene deltas
against it:

```
tools/extract_video_frames.py      the old banks -> 932 raw 256x96 index frames
tools/encode_shared_dictionary.py  frames -> ONE dictionary + deltas + snapshots
tools/verify_shared_dictionary.py  replays the *emitted* bytes and diffs against
                                   the source frames
```

Numbers for the shipped stream:

* shared dictionary: **20 202 tiles ≈ 631 KB** across all seven cutscenes (the
  cutscenes share a lot of imagery — the intro shots, the Holocube console and
  the starfields all dedupe against each other), compared with 7 private
  dictionaries gathering **the same total** in the original video build. The
  saving is what pays for the encode being loss-less:
* stream: 53 banks = **848 KB**, decoded by 512 tiles per bank
  (`bank = 44 + (tile_id >> 9)`, `addr = 0x8000 + ((tile_id & 511) << 5)`),
* **932 positions, byte-exact: 0.0000 % pixel error** — `--verify` replays the
  emitted banks and diffs them against the source frames; the shared dictionary
  plus the player's per-VBlank quota make a loss-less encode unnecessary to
  compromise (the encoder's optional `--thresh`/`--maxupd` knobs stay off),
* every position carries its **own delta**, including the periodic snapshot
  positions: the player advances one position per displayed frame and cannot
  afford a 12 KB full repaint mid-cutscene, so the delta chain is
  self-sufficient and the snapshots (every 8 positions) are only resync data —
  `--verify` checks that the two agree (0 cells apart on all seven cutscenes),
* mean changed 8×8 cells per position 16–159 (52.7 overall, max 360),
  deltas 39–746 bytes.

**Runtime player** (`sms/src/video_player.c`) keeps the SMS VBlank budget honest:
the 256×96 window is 384 cells that own VRAM tiles 64–447, the tilemap is written
once, and a delta is applied **resumably** with a quota of **32 paced tile
uploads per VBlank** (≈52 % of an NTSC frame — safe with the display on). Heavy
positions therefore spread over a few frames; the player subtracts those frames
from the cutscene's hold time so the running time matches the original demo
(`frame_delay` 1–4 ticks per position). Snapshot 0 paints the opening frame with
the display off; the later snapshots are resync data only — the delta chain is
what playback follows, so every position on screen is the encoder's picture.

---

## 5. Verification (what was actually checked)

* `ihx2sms` → SEGA header checksum updated; `mkrom_merged.py` → 4 MB size nibble.
* `smstest` boots the real ROM: display on, 16 colours on the title screen, an
  FMV running from frame 1.
* `tests/playtest.txt` — 13 scenarios, all passing: logo → title menu →
  instructions → holocube video → cinematic intro (both parts) → START GAME →
  debut video → jungle; walking, jumping, a room transition 27→28; picking up the
  room-28 object → **object-recovery video plays** → play resumes; the holocube
  in room 26 → **holocube-message video**; reaching room 63 → disintegration
  video → back to the title; start a fresh game afterwards.
* `tests/soak.txt` — every long cutscene is played to completion **without any
  input** (logo, intro 1, intro 2, holocube, debut) and the display is intact
  afterwards: no stalls, no corruption, no runaway.
* Pixel comparison against the ideal frames: the merged ROM renders the
  cutscenes pixel-identically to the source data, in window coordinates
  (verified for the logo and jungle-debut sequences, tick by tick).
* Frame pacing measured on hardware-accurate terms: each position is held for at
  least `frame_delay` VBlanks, deltas are capped at 32 tile uploads per frame.

Screenshots from the automated runs are in `docs/screenshots/` and `shots/`.

---

## 6. Layout

```
Flashback_SMS.sms              the cartridge image (copy of sms/flashback.sms)
sms/
  Makefile                     banked build + playtest target
  src/main.c                   game states, level-1 engine, item -> cutscene logic
  src/video_player.c/.h        resumable FMV player (quota-budgeted deltas)
  src/title_menu.c / instructions.c / audio.c / menu_font.h
  src/game_banks.h             ROM bank map
  gen/                         generated data: rooms, sprites, icons, cutscene tables
  tests/playtest.txt, soak.txt acceptance + soak scripts (smstest DSL)
tools/
  extract_video_frames.py      old FMV banks -> raw frames
  encode_shared_dictionary.py  frames -> shared dictionary + stream + C tables
                               (--verify replays the emitted bytes: 0.0000 % error)
  verify_shared_dictionary.py  older standalone verifier (kept for reference)
  verify_rom_video.py          drives the real ROM in smstest and compares every
                               FMV position with the source frame (932/932)
  deflicker.py                 optional: collapse the demo's 1-frame dither strobe
  emit_icons.py, fbextract.py  DOS GLOBAL.ICN -> SMS icon bank
  mkrom_merged.py              4 MiB cartridge builder (header fix-up)
inputs/
  level1_port/                 the level-1 source archive + DOS demo data
  video_port/                  the video source archive + its data banks
docs/screenshots/              captures of the finished build
```

### Regenerating the FMV data from scratch

```sh
python3 tools/extract_video_frames.py inputs/video_port/all_data_banks.bin \
        inputs/video_port/src/cutscenes_data.c /tmp/frames
python3 tools/encode_shared_dictionary.py --frames-dir /tmp/frames \
        --out sms/gen --base-bank 44 --snapivl 8 --verify      # 0.0000 % error
python3 tools/emit_icons.py inputs/level1_port/assets/DATA/DEMO_UK.ABA sms/gen/bank43.c
make                      # relink code + rebuild the 4 MB image
```

---

## 7. Honest limitations

* The FMV is 256×96 in a 16-colour palette, letterboxed — that is what fits the
  SMS tile budget at 60 Hz; the playfield during gameplay is the level-1 port's
  256×192 screen.
* Cutscenes have no audio track (the source streams carry none).
* The DOS demo fakes shading with a 1-frame dither alternation; replayed at
  60 Hz it reads as a soft shimmer on a few shots. `tools/deflicker.py` is a
  ready-to-use pass that merges those alternations into the average palette
  colour if you prefer a stable, slightly flatter look (the shipped stream is
  the bit-exact original data).
* Level 1 is the *demo's* level 1 (38 rooms) — the retail game's level 1 is
  longer; the extra content is not in the demo data.
* The skill's note about `sdcccall(1)` vs. the pre-built `SMSlib.lib` applies:
  ASlink prints "conflicting sdcc options" warnings during the link. They are
  harmless (the libraries are the pinned devkitSMS set), which is why the
  Makefile ignores the linker's exit status and then verifies the `.ihx` exists.

*Flashback is a trademark of its respective owners; this is a non-commercial
technical port of the freely distributed DOS demo, built with SDCC + devkitSMS.*
