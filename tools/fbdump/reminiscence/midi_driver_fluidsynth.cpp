
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "midi_driver.h"

#include <fluidsynth.h>

static const uint8_t _mt32ToGm[128] = {
	 0,   1,   0,   2,   4,   4,   5,   3,  16,  17,  18,  16,  16,  19,  20,  21,
	 6,   6,   6,   7,   7,   7,   8, 112,  62,  62,  63,  63,  38,  38,  39,  39,
	88,  95,  52,  98,  97,  99,  14,  54, 102,  96,  53, 102,  81, 100,  14,  80,
	48,  48,  49,  45,  41,  40,  42,  42,  43,  46,  45,  24,  25,  28,  27, 104,
	32,  32,  34,  33,  36,  37,  35,  35,  79,  73,  72,  72,  74,  75,  64,  65,
	66,  67,  71,  71,  68,  69,  70,  22,  56,  59,  57,  57,  60,  60,  58,  61,
	61,  11,  11,  98,  14,   9,  14,  13,  12, 107, 107,  77,  78,  78,  76,  76,
	47, 117, 127, 118, 118, 116, 115, 119, 115, 112,  55, 124, 123,   0,  14, 117
};

static const char *SF2;

static fluid_synth_t *_fluidSynth;
static fluid_settings_t *_fluidSettings;
static int _soundFont = -1;

struct MidiDriver_fluidsynth: MidiDriver {
	virtual int init() {
		if (!SF2) {
			fprintf(stdout, "WARNING: No soundfont specified\n");
			return -1;
		}
		return 0;
	}
	virtual void reset(int rate) {
		_fluidSettings = new_fluid_settings();
		fluid_settings_setnum(_fluidSettings, "synth.sample-rate", rate);
		fluid_settings_setstr(_fluidSettings, "synth.midi-bank-select", "gm");

		_fluidSynth = new_fluid_synth(_fluidSettings);
		_soundFont = fluid_synth_sfload(_fluidSynth, SF2, 1);
	}
	virtual void fini() {
		if (_fluidSynth) {
			delete_fluid_synth(_fluidSynth);
			_fluidSynth = 0;
		}
		if (_fluidSettings) {
			delete_fluid_settings(_fluidSettings);
			_fluidSettings = 0;
		}
		if (!(_soundFont < 0)) {
			fluid_synth_sfunload(_fluidSynth, _soundFont, 1);
		}
	}
	virtual void setSoundFont(const char *path) {
		SF2 = path;
	}

	virtual void noteOff(int channel, int note, int velocity) {
		fluid_synth_noteoff(_fluidSynth, channel, note);
	}
	virtual void noteOn(int channel, int note, int velocity) {
		fluid_synth_noteon(_fluidSynth, channel, note, velocity);
	}
	virtual void controlChange(int channel, int type, int value) {
		fluid_synth_cc(_fluidSynth, channel, type, value);
	}
	virtual void programChange(int channel, int num) {
		assert(num >= 0 && num < 128);
		fluid_synth_program_change(_fluidSynth, channel, _mt32ToGm[num]);
	}
	virtual void pitchBend(int channel, int lsb, int msb) {
		fluid_synth_pitch_bend(_fluidSynth, channel, (msb << 7) | lsb);
	}

	virtual void readSamples(int16_t *buffer, int count) {
		fluid_synth_write_s16(_fluidSynth, count, buffer, 0, 2, buffer, 1, 2);
	}
};

static MidiDriver *createMidiDriver() {
	return new MidiDriver_fluidsynth;
}

const MidiDriverInfo midi_driver_fluidsynth = {
	"FluidSynth",
	createMidiDriver
};

