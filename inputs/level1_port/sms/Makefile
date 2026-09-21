# ---------------------------------------------------------------------------
# devkitSMS / SDCC project Makefile  (bank-switched ROM > 48 KiB)
#
# Code lives in the fixed first 32 KiB; large data is split across 16 KiB banks
# (bank2, bank3, ...) each compiled into its own CONST segment BANK<n> and
# placed at ROM offset 0x<n>8000 by the linker. At run time, page a bank in
# with SMS_mapROMBank(n) before reading its arrays. See references/assets.md.
# ---------------------------------------------------------------------------
DEVKIT   ?= $(HOME)/.devkitsms
PROG     ?= game

CC       := sdcc
IHX2SMS  := ihx2sms

# Which banks exist and where each maps in ROM. Bank n -> file offset n*0x10000
# + 0x8000 (the slot-2 window). Extend both lists together.
BANKS       := 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43
BANKFLAGS   := -Wl-b_BANK2=0x28000 -Wl-b_BANK3=0x38000 -Wl-b_BANK4=0x48000 -Wl-b_BANK5=0x58000 -Wl-b_BANK6=0x68000 -Wl-b_BANK7=0x78000 -Wl-b_BANK8=0x88000 -Wl-b_BANK9=0x98000 -Wl-b_BANK10=0xA8000 -Wl-b_BANK11=0xB8000 -Wl-b_BANK12=0xC8000 -Wl-b_BANK13=0xD8000 -Wl-b_BANK14=0xE8000 -Wl-b_BANK15=0xF8000 -Wl-b_BANK16=0x108000 -Wl-b_BANK17=0x118000 -Wl-b_BANK18=0x128000 -Wl-b_BANK19=0x138000 -Wl-b_BANK20=0x148000 -Wl-b_BANK21=0x158000 -Wl-b_BANK22=0x168000 -Wl-b_BANK23=0x178000 -Wl-b_BANK24=0x188000 -Wl-b_BANK25=0x198000 -Wl-b_BANK26=0x1A8000 -Wl-b_BANK27=0x1B8000 -Wl-b_BANK28=0x1C8000 -Wl-b_BANK29=0x1D8000 -Wl-b_BANK30=0x1E8000 -Wl-b_BANK31=0x1F8000 -Wl-b_BANK32=0x208000 -Wl-b_BANK33=0x218000 -Wl-b_BANK34=0x228000 -Wl-b_BANK35=0x238000 -Wl-b_BANK36=0x248000 -Wl-b_BANK37=0x258000 -Wl-b_BANK38=0x268000 -Wl-b_BANK39=0x278000 -Wl-b_BANK40=0x288000 -Wl-b_BANK41=0x298000 -Wl-b_BANK42=0x2A8000 -Wl-b_BANK43=0x2B8000
BANKRELS    := $(addsuffix .rel,$(addprefix bank,$(BANKS)))

CFLAGS   := -mz80 -I$(DEVKIT)/include --peep-file $(DEVKIT)/include/peep-rules.txt -Isrc -Igen
LDFLAGS  := -mz80 --no-std-crt0 --data-loc 0xC000 $(BANKFLAGS)

CODEOBJS := main.rel roomtable.rel roompals.rel frametable.rel itemtable.rel

all: $(PROG).sms

# Code objects (fixed region).
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

# Each data bank compiles into its own CONST segment. Only ~16320 bytes of the
# 16384 are usable per bank; keep each bank<n>.c under that.
bank%.rel: gen/bank%.c
	$(CC) -mz80 --constseg BANK$* -I$(DEVKIT)/include -Igen -c $< -o $@

.PHONY: $(PROG).ihx
$(PROG).ihx: $(CODEOBJS) $(BANKRELS)
	-$(CC) -o $@ $(LDFLAGS) $(DEVKIT)/lib/crt0_sms.rel $(CODEOBJS) \
	      $(DEVKIT)/lib/SMSlib.lib $(DEVKIT)/lib/PSGlib.lib $(BANKRELS)
	test -f $@

$(PROG).sms: $(PROG).ihx
	$(IHX2SMS) $< $@

clean:
	rm -f *.rel *.ihx *.sym *.lst *.map *.noi *.lk *.asm *.rst $(PROG).sms

.PHONY: all clean
