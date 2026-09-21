#ifndef AUDIO_H
#define AUDIO_H

enum {
    SFX_NONE = 0,
    SFX_LASER_SHOT,
    SFX_FOOTSTEP,
    SFX_ROLL_JUMP,
    SFX_PICKUP,
    SFX_DAMAGE,
    SFX_MENU_MOVE,
    SFX_MENU_SELECT,
    SFX_SWITCH
};

void audio_init(void);
void audio_play_sfx(unsigned char sfx_id);
void audio_update(void);

#endif
