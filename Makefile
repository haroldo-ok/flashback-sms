# Flashback SMS - banked 4 MB build: code in banks 0-1, data blob in banks 2..
DEVKIT ?= $(HOME)/.devkitsms
# local copy of the skill's headless emulator, patched for the 8-sprites-per-line
# limit and the background priority bit (see tools/smstest/README)
SMSTEST := tools/smstest/smstest
CC     := sdcc
# 1 = boot diagnostics build (used by `make check`); 0 = the ROM you play
SELF_TEST ?= 0
TEST_NO_INVENTORY ?= 0
# 1 = show the pad the game logic received as markers on screen (input debugging)
DEBUG_PAD ?= 0
CFLAGS := -mz80 -I$(DEVKIT)/include -Igen -Isrc --peep-file $(DEVKIT)/include/peep-rules.txt --max-allocs-per-node 20000 -DSELF_TEST=$(SELF_TEST) -DTEST_NO_INVENTORY=$(TEST_NO_INVENTORY) -DDEBUG_PAD=$(DEBUG_PAD)
LDFLAGS:= -mz80 --no-std-crt0 --data-loc 0xC000
OBJS   := build/main.rel build/fmv.rel build/room.rel build/pge.rel build/logic.rel build/sim.rel build/tiledec.rel build/psg.rel build/hud.rel build/data_index.rel
DEMO   ?= demo/DATA
DICT_MERGE ?= 0      # 0 = no tile merging: flat-shaded polygons suffer from it      # merge cutscene tiles differing in <= N pixels (saves ~530 KB)
CAP    ?= capture

all: flashback.sms

build/%.rel: src/%.c gen/data_index.h src/fmv.h src/room.h src/pge.h src/logic.h src/sim.h src/tiledec.h src/psg.h src/hud.h
	@mkdir -p build; $(CC) $(CFLAGS) -c $< -o $@
build/data_index.rel: gen/data_index.c gen/data_index.h
	@mkdir -p build; $(CC) $(CFLAGS) -c $< -o $@

build/flashback.ihx: $(OBJS)
	$(CC) -o $@ $(LDFLAGS) $(DEVKIT)/lib/crt0_sms.rel $(OBJS) $(DEVKIT)/lib/SMSlib.lib $(DEVKIT)/lib/PSGlib.lib
.PHONY: build/flashback.ihx

flashback.sms: build/flashback.ihx gen/bank_data.bin
	$(DEVKIT)/bin/ihx2sms build/flashback.ihx build/code.sms
	python3 tools/mkrom.py build/code.sms gen/bank_data.bin $@
	cp build/flashback.noi flashback.noi

# ---- data pipeline (needs the demo in $(DEMO)) -----------------------------
fbdump: ; $(MAKE) -C tools/fbdump
capture: fbdump
	tools/fbdump/fbdump $(DEMO) cutscenes $(CAP)
	tools/fbdump/fbdump $(DEMO) rooms $(CAP) 0
	tools/fbdump/fbdump $(DEMO) title $(CAP)
	tools/fbdump/fbdump $(DEMO) replay $(CAP) 0   # level 1 gameplay trace
	tools/fbdump/fbdump $(DEMO) anims $(CAP)     # every animation frame, for played gameplay
	for l in 0 2 5; do tools/fbdump/fbdump $(DEMO) level $(CAP) $$l; done
convert:
	# the intro, level 1's entry, and every cutscene level 1's scripts can ask
	# for (opcode 0x5A) that exists in the demo data - picking items up plays them
	python3 tools/fmvenc.py --out gen/fmv.pkl $(foreach c,40 0D 4A 00 01 02 04 05 09 0A 0E 0F 10 11 12 14 15 2C 31,$(CAP)/cut_$(c).fbv)
	python3 tools/roomconv.py $(CAP)/rooms_L0.fbr --out gen/rooms_L0.pkl
	python3 tools/levelconv.py $(CAP)/level_L0.fbl --out gen/level_L0.pkl
	python3 tools/roomconv.py $(CAP)/title.fbr --out gen/title.pkl --max-tiles 447
	python3 tools/logicconv.py $(CAP)/trace_D0.fbt --out gen/logic_D0.pkl
	python3 tools/fbdump/../../tools/mkdata.py --help >/dev/null 2>&1 || true
	if [ "$(DICT_MERGE)" -gt 0 ]; then \
	  python3 tools/dictmerge.py gen/fmv.pkl --out gen/fmv_merged.pkl --max-diff $(DICT_MERGE); \
	else cp gen/fmv.pkl gen/fmv_merged.pkl; fi
	python3 tools/animconv.py $(CAP)/anims.fbt --out gen/anim_D0.pkl
	python3 tools/mkdata.py pack --fmv gen/fmv_merged.pkl --rooms gen/rooms_L0.pkl gen/rooms_L2.pkl gen/rooms_L5.pkl --anim gen/anim_D0.pkl --levels gen/level_L0.pkl gen/level_L2.pkl gen/level_L5.pkl --logic gen/logic_D0.pkl --out gen
verify:
	python3 tools/mkdata.py verify --out gen --capture $(CAP)
playtest: flashback.sms
	$(SMSTEST) flashback.sms tests/playtest.txt
# full gate: regenerate the test from the ROM data, run it, compare pixels
$(SMSTEST): tools/smstest/smstest.c tools/smstest/z80.c
	cc -O2 -o $@ tools/smstest/smstest.c tools/smstest/z80.c

# The data for the full game.  The verification harness (the recorded demo
# and its expected states) is only packed for test builds: it is not part of
# the ROM you play, and the ROM has no room to spare.
PACK_ARGS = --fmv gen/full_fmv5.pkl --title gen/full_title.pkl --menu gen/menu.pkl --music - gen/mus_01.pkl - gen/mus_03.pkl - gen/mus_05.pkl - gen/mus_07.pkl - - gen/mus_10.pkl gen/mus_11.pkl - - - - gen/mus_16.pkl - - gen/mus_19.pkl - gen/mus_21.pkl - - gen/mus_24.pkl gen/mus_25.pkl --sfx gen/sfx.pkl --rooms gen/full_rooms_L0.pkl gen/full_rooms_L1.pkl gen/full_rooms_L2.pkl gen/full_rooms_L3.pkl gen/full_rooms_L5.pkl --levels gen/full_level_L0.pkl gen/full_level_L1.pkl gen/full_level_L2.pkl gen/full_level_L3.pkl gen/full_level_L4.pkl gen/full_level_L5.pkl gen/full_level_L6.pkl --sprsets gen/full_sprsets.pkl

pack:
	python3 tools/mkdata.py pack $(PACK_ARGS) --hud gen/hud.pkl --out gen

# (the item names and inventory are left out here to make room: the gate
# does not exercise them)
pack-test:
	python3 tools/mkdata.py pack $(PACK_ARGS) --logic gen/full_logic_D0.pkl --logic-level 0 --out gen

# the gate needs the diagnostics build; the shipped ROM is rebuilt clean after
check: $(SMSTEST)
	$(MAKE) pack-test
	$(MAKE) clean
	$(MAKE) SELF_TEST=1
	python3 tools/crosscheck.py gen
	$(SMSTEST) flashback.sms tests/playtest.txt
	python3 tools/crosscheck.py compare
	$(MAKE) pack
	$(MAKE) clean
	$(MAKE)
# a cut-down ROM for emulators that cannot reach high banks: level 1 only,
# no cutscenes.  Everything it needs sits in the first megabyte.
small:
	python3 tools/mkdata.py pack --rooms gen/rooms_L0.pkl --levels gen/level_L0.pkl \
	        --anim gen/anim_D0.pkl --out gen
	$(MAKE) build/flashback.ihx
	$(DEVKIT)/bin/ihx2sms build/flashback.ihx build/code.sms
	python3 tools/mkrom.py build/code.sms gen/bank_data.bin flashback-small.sms
	cp build/flashback.noi flashback-small.noi

clean:
	rm -rf build flashback.sms flashback.noi
.PHONY: all fbdump capture convert verify playtest check clean
