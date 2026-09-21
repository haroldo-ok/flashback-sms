CC      = sdcc
PREFIX  = /home/user/.devkitsms
IHX2SMS = $(PREFIX)/bin/ihx2sms
SMSTEST = $(PREFIX)/bin/smstest

CFLAGS  = -mz80 -I$(PREFIX)/include -Isrc -I. --peep-file $(PREFIX)/include/peep-rules.txt
LDFLAGS = -mz80 --no-std-crt0 --data-loc 0xC000

SRCS    = src/main.c src/video_player.c src/title_menu.c src/instructions.c \
          src/game_play.c src/conrad.c src/actors.c src/hud.c src/audio.c \
          src/cutscenes_data.c src/sprite_patterns.c

OBJS    = $(SRCS:.c=.rel)

all: Flashback_SMS.sms

$(OBJS): %.rel: %.c
	$(CC) -c $(CFLAGS) $< -o $@

flashback_base.ihx: $(OBJS)
	-$(CC) $(LDFLAGS) $(PREFIX)/lib/crt0_sms.rel $(OBJS) \
		$(PREFIX)/lib/SMSlib.lib $(PREFIX)/lib/PSGlib.lib -o $@
	@test -s $@ || (echo "Link failed" && exit 1)

flashback_base.sms: flashback_base.ihx
	$(IHX2SMS) flashback_base.ihx flashback_base.sms

Flashback_SMS.sms: flashback_base.sms all_data_banks.bin mkrom_4mb.py
	python3 mkrom_4mb.py flashback_base.sms all_data_banks.bin Flashback_SMS.sms

playtest: Flashback_SMS.sms test_playtest.txt
	$(SMSTEST) Flashback_SMS.sms test_playtest.txt

clean:
	rm -f src/*.rel src/*.asm src/*.lst src/*.sym src/*.adb src/*.map src/*.noi \
	      flashback_base.ihx flashback_base.sms flashback_base.map flashback_base.noi \
	      flashback_base.lk Flashback_SMS.sms

.PHONY: all playtest clean
