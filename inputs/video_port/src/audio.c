#include "audio.h"
#include "SMSlib.h"
#include "PSGlib.h"

__sfr __at(0x7F) PSGPort;

static unsigned char cur_sfx = SFX_NONE;
static unsigned char sfx_timer = 0;

static void psg_tone(unsigned char chn, unsigned int div, unsigned char vol) {
    /* Set tone divisor: 0x80 | (chn << 5) | (div & 0x0F), followed by (div >> 4) */
    PSGPort = 0x80 | (chn << 5) | (div & 0x0F);
    PSGPort = (div >> 4) & 0x3F;
    /* Set attenuation (volume): 0x90 | (chn << 5) | ((15 - vol) & 0x0F) */
    PSGPort = 0x90 | (chn << 5) | ((15 - vol) & 0x0F);
}

static void psg_noise(unsigned char ctrl, unsigned char vol) {
    /* Set noise control: 0xE0 | (ctrl & 0x07) */
    PSGPort = 0xE0 | (ctrl & 0x07);
    /* Set attenuation: 0xF0 | ((15 - vol) & 0x0F) */
    PSGPort = 0xF0 | ((15 - vol) & 0x0F);
}

static void psg_mute(void) {
    PSGPort = 0x9F; /* Mute Ch 0 */
    PSGPort = 0xBF; /* Mute Ch 1 */
    PSGPort = 0xDF; /* Mute Ch 2 */
    PSGPort = 0xFF; /* Mute Noise */
}

void audio_init(void) {
    cur_sfx = SFX_NONE;
    sfx_timer = 0;
    psg_mute();
}

void audio_play_sfx(unsigned char sfx_id) {
    cur_sfx = sfx_id;
    sfx_timer = 0;
}

void audio_update(void) {
    if (cur_sfx == SFX_NONE) return;

    sfx_timer++;

    switch (cur_sfx) {
    case SFX_LASER_SHOT:
        if (sfx_timer < 6) {
            psg_tone(0, 200 + sfx_timer * 100, 15 - sfx_timer * 2);
        } else {
            psg_mute();
            cur_sfx = SFX_NONE;
        }
        break;

    case SFX_FOOTSTEP:
        if (sfx_timer == 1) {
            psg_noise(6, 6);
        } else {
            psg_mute();
            cur_sfx = SFX_NONE;
        }
        break;

    case SFX_ROLL_JUMP:
        if (sfx_timer < 8) {
            psg_tone(0, 500 - sfx_timer * 40, 12 - sfx_timer);
        } else {
            psg_mute();
            cur_sfx = SFX_NONE;
        }
        break;

    case SFX_PICKUP:
        if (sfx_timer < 5) {
            psg_tone(0, 180, 14);
        } else if (sfx_timer < 10) {
            psg_tone(0, 120, 15);
        } else {
            psg_mute();
            cur_sfx = SFX_NONE;
        }
        break;

    case SFX_DAMAGE:
        if (sfx_timer < 8) {
            psg_noise(7, 15 - sfx_timer);
        } else {
            psg_mute();
            cur_sfx = SFX_NONE;
        }
        break;

    case SFX_MENU_MOVE:
        if (sfx_timer < 3) {
            psg_tone(0, 250, 12);
        } else {
            psg_mute();
            cur_sfx = SFX_NONE;
        }
        break;

    case SFX_MENU_SELECT:
        if (sfx_timer < 4) {
            psg_tone(0, 180, 14);
        } else if (sfx_timer < 8) {
            psg_tone(0, 100, 15);
        } else {
            psg_mute();
            cur_sfx = SFX_NONE;
        }
        break;

    case SFX_SWITCH:
        if (sfx_timer < 6) {
            psg_tone(0, 350 + sfx_timer * 40, 12);
        } else {
            psg_mute();
            cur_sfx = SFX_NONE;
        }
        break;

    default:
        psg_mute();
        cur_sfx = SFX_NONE;
        break;
    }
}
