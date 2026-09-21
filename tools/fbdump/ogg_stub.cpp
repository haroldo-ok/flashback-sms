/* No music needed for extraction: replaces ogg_player.cpp */
#include "ogg_player.h"
OggPlayer::OggPlayer(Mixer *mixer, FileSystem *fs) : _mix(mixer), _fs(fs), _impl(0) {}
OggPlayer::~OggPlayer() {}
bool OggPlayer::playTrack(int) { return false; }
void OggPlayer::stopTrack() {}
void OggPlayer::pauseTrack() {}
void OggPlayer::resumeTrack() {}
bool OggPlayer::mix(int16_t *, int) { return false; }
bool OggPlayer::mixCallback(void *, int16_t *, int) { return false; }
