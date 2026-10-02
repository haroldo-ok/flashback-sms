#!/bin/sh
# Rebuild every generated file of the full-game ROM from a DOS copy of
# Flashback.  Usage:   tools/rebuild_full.sh /path/to/flashback/data
# then:                make pack && make            (the ROM)
#                      make check                   (the verification gate)
#
# The game's data is not part of this repository; point DATA at your own copy
# (the folder holding level1.map, global.icn, introlon.mid, ...).
set -e
DATA="$1"
[ -n "$DATA" ] || { echo "usage: $0 DATA_DIR" >&2; exit 1; }
CAP=capfull
mkdir -p $CAP gen
F=tools/fbdump/fbdump

make -C tools/fbdump

# --- captures from the original engine (REminiscence, headless) -------------
$F "$DATA" rooms     $CAP 0,1,2,3,5        # one capture per map (4_x and 5_x share)
$F "$DATA" cutscenes $CAP
for l in 0 1 2 3 4 5 6; do $F "$DATA" level $CAP $l; done
$F "$DATA" sprites   $CAP                  # Conrad, 4 monster sets, level objects
$F "$DATA" title     $CAP
$F "$DATA" menutext  $CAP                  # "LEVEL 1..5" in the game's own font
$F "$DATA" sfx       $CAP
$F "$DATA" hudpal    $CAP                  # icon and text colours
$F "$DATA" replay    $CAP 0                # recorded demo: the verification oracle

# --- conversion --------------------------------------------------------------
for l in 0 1 2 3 5; do python3 tools/roomconv.py $CAP/rooms_L$l.fbr --out gen/full_rooms_L$l.pkl; done
python3 tools/roomconv.py $CAP/title.fbr --out gen/full_title.pkl --max-tiles 438
for l in 0 1 2 3 4 5 6; do python3 tools/levelconv.py $CAP/level_L$l.fbl --out gen/full_level_L$l.pkl; done
python3 tools/animconv.py $CAP/sprites.fbt --sets --out gen/full_sprites.pkl
python3 tools/sprconv.py gen/full_sprites.pkl \
    --levels $(for l in 0 1 2 3 4 5 6; do printf "$CAP/level_L%d.fbl " $l; done) \
    --out gen/full_sprsets.pkl
# the cutscenes that fit: logos, level intros, and the short in-game clips the
# level scripts ask for (opcode 0x5A)
CLIPS="00 01 02 04 05 0A 0F 10 12 14 15 20 21 22 23 24 2B 2C 2F 31 35 36 37 39 3A 3C 40"
python3 tools/fmvenc.py --out gen/full_fmv5.pkl $(for c in $CLIPS; do printf "$CAP/cut_$c.fbv "; done)
python3 tools/menuconv.py $CAP/menutext.fbr --out gen/menu.pkl
python3 tools/sfxconv.py $CAP/sfx.bin --out gen/sfx.pkl
python3 tools/hudconv.py "$DATA" $CAP/hudpal.bin gen/full_sprsets.pkl \
    $(for l in 0 1 2 3 4 5 6; do printf "$CAP/level_L%d.fbl " $l; done) --out gen/hud.pkl
python3 tools/logicconv.py $CAP/trace_D0.fbt --out gen/full_logic_D0.pkl

# music: each PrfPlayer track names its MIDI file inside its .prf
python3 - "$DATA" <<'EOF'
import os, subprocess, sys
D = sys.argv[1]
names = ("introl3 option3 journal3 chute3 desinte3 capture3 voyage3 telepor3 planexp3 "
         "end31 lift3 present3 gameove3 holo3 memory3 chutevi3 reveil3 misvali3 taxi3 "
         "donner3 mission3 objet3 recharg3 generat3 pont3 rechage3").split()
base = 16 * 30 + 16 * 2 + 16 * 2          # instruments, adlib notes, velocities
for i, n in enumerate(names):
    d = open(os.path.join(D, n + '.prf'), 'rb').read()
    midi = d[base + 6:base + 26].split(b'\0')[0].decode('latin1').lower()
    subprocess.run(['python3', 'tools/midiconv.py', os.path.join(D, midi),
                    '--out', f'gen/mus_{i:02d}.pkl'], check=True)
EOF

echo "data rebuilt: now run   make pack && make   (and   make check)"
