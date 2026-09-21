# ---------------------------------------------------------------------------
# Flashback SMS -- merged build: level 1 gameplay + FMV cutscenes in 4 MB.
#
# Bank map (see src/game_banks.h):
#   banks 0..1    code (fixed first 32 KiB -- linked, not bank switched)
#   banks 2..43   level 1 data: sms/gen/bank*.c, each compiled into its own
#                 CONST segment placed at 0x<n>8000, i.e. ROM bank n
#   banks 44..97  FMV stream + title/instructions screens, appended by
#                 ../tools/mkrom_merged.py together with the 4 MiB padding
# ---------------------------------------------------------------------------
DEVKIT  ?= $(HOME)/.devkitsms
PROG    ?= flashback

CC      := sdcc
IHX2SMS := $(DEVKIT)/bin/ihx2sms
SMSTEST := $(DEVKIT)/bin/smstest

BANKS     := 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 \
             27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43
BANKFLAGS := $(foreach n,$(BANKS),-Wl-b_BANK$(n)=0x$(n)8000)
BANKRELS  := $(addsuffix .rel,$(addprefix bank,$(BANKS)))

CFLAGS  := -mz80 -I$(DEVKIT)/include --peep-file $(DEVKIT)/include/peep-rules.txt \
           -Isrc -Igen --max-allocs-per-node 20000
LDFLAGS := -mz80 --no-std-crt0 --data-loc 0xC000 $(BANKFLAGS)

CODEOBJS := main.rel roomtable.rel roompals.rel frametable.rel itemtable.rel \
            video_player.rel title_menu.rel instructions.rel audio.rel \
            cutscenes_data.rel

all: $(PROG).sms

# code objects (fixed region)
%.rel: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

roomtable.rel: gen/roomtable.c
	$(CC) $(CFLAGS) -c $< -o $@
roompals.rel: gen/roompals.c
	$(CC) $(CFLAGS) -c $< -o $@
frametable.rel: gen/frametable.c
	$(CC) $(CFLAGS) -c $< -o $@
itemtable.rel: gen/itemtable.c
	$(CC) $(CFLAGS) -c $< -o $@
cutscenes_data.rel: gen/cutscenes_data.c gen/cutscenes_data.h
	$(CC) $(CFLAGS) -c $< -o $@

# one data bank per CONST segment (max ~16320 usable bytes per bank)
bank%.rel: gen/bank%.c
	$(CC) -mz80 --constseg BANK$* -I$(DEVKIT)/include -Igen -c $< -o $@

# The ASlink "conflicting sdcc options" warning (sdcccall(1) code vs the
# pre-built SMSlib) makes the linker exit non-zero even on success, hence '-'.
$(PROG)_base.ihx: $(CODEOBJS) $(BANKRELS)
	-$(CC) -o $@ $(LDFLAGS) $(DEVKIT)/lib/crt0_sms.rel $(CODEOBJS) \
	      $(DEVKIT)/lib/SMSlib.lib $(DEVKIT)/lib/PSGlib.lib $(BANKRELS)
	@test -s $@ || (echo "link failed" && exit 1)

$(PROG)_base.sms: $(PROG)_base.ihx
	$(IHX2SMS) $< $@

# final cartridge: base ROM + FMV blob + screens + 4 MiB padding
$(PROG).sms: $(PROG)_base.sms gen/video_bank_data.bin gen/title_bank.bin \
             gen/instru_bank.bin ../tools/mkrom_merged.py
	python3 ../tools/mkrom_merged.py $(PROG)_base.sms gen $(PROG).sms

# the test scripts write their screenshots to ../shots (kept out of the
# sources), so make sure the directory exists before running them
playtest: $(PROG).sms
	@mkdir -p ../shots
	$(SMSTEST) $(PROG).sms tests/playtest.txt

soak: $(PROG).sms
	@mkdir -p ../shots
	$(SMSTEST) $(PROG).sms tests/soak.txt

clean:
	rm -f *.rel *.ihx *.asm *.lst *.sym *.map *.noi *.lk *.adb \
	      $(PROG)_base.sms $(PROG).sms

.PHONY: all playtest soak clean
