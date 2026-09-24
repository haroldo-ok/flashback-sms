#!/usr/bin/env python3
"""Approximate the game's sampled sound effects on the SMS sound chip.

The PSG cannot play samples, so each effect is analysed and re-created from
what it is made of:

  * how noisy it is (zero-crossing rate against a pitch estimate) decides
    whether it becomes a NOISE burst or a TONE
  * a pitch estimate (autocorrelation of the loudest part) sets the tone period
  * the loudness envelope, sampled per 60 Hz frame, becomes the volume steps,
    and a falling pitch over the sample becomes a falling tone

These are approximations, not reproductions: a sampled voice or explosion
becomes a tone or noise burst with the same shape in time.

Output: one short PSG stream per effect, in the same format as the music
(tools/midiconv.py), but written on the third tone voice and the noise voice
so a playing score keeps the other two.

    sfxconv.py capfull/sfx.bin --out gen/sfx.pkl
"""
import argparse, pickle, struct
import numpy as np

FPS = 60
PSG_CLOCK = 3579545
MAX_FRAMES = 45          # 0.75s: longer effects are cut


def read_sfx(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<H', d, 0)[0]
    p = 2
    out = []
    for _ in range(n):
        ln, freq = struct.unpack_from('<IH', d, p)
        p += 6
        data = np.frombuffer(d[p:p + ln], np.int8).astype(np.float32) if ln else np.zeros(0, np.float32)
        p += ln
        out.append((freq, data))
    return out


def pitch(x, rate):
    """rough pitch by autocorrelation; None if it looks unvoiced"""
    if len(x) < 64:
        return None
    x = x - x.mean()
    if x.std() < 1:
        return None
    n = min(len(x), 2048)
    x = x[:n]
    ac = np.correlate(x, x, 'full')[n - 1:]
    ac /= (ac[0] + 1e-6)
    lo = max(2, int(rate / 2000))          # up to 2 kHz
    hi = min(len(ac) - 1, int(rate / 60))  # down to 60 Hz
    if hi <= lo:
        return None
    k = int(np.argmax(ac[lo:hi])) + lo
    return (rate / k, float(ac[k]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sfx')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    effects = read_sfx(a.sfx)
    out = {}
    tones = noises = empty = 0
    for i, (rate, data) in enumerate(effects):
        if len(data) == 0 or rate == 0:
            empty += 1
            continue
        spf = max(1, int(rate / FPS))                   # samples per frame
        nf = min(MAX_FRAMES, max(1, len(data) // spf))
        env = np.array([np.abs(data[f * spf:(f + 1) * spf]).mean() for f in range(nf)])
        if env.max() < 2:
            empty += 1
            continue
        env = env / env.max()
        loud = data[int(np.argmax(env)) * spf: int(np.argmax(env)) * spf + 4 * spf]
        p = pitch(loud, rate)
        voiced = p is not None and p[1] > 0.30 and 60 < p[0] < 2500
        stream = bytearray()
        prev_vol = 15
        prev_per = None
        for f in range(nf):
            vol = int(round(15 - env[f] * 15))
            body = bytearray()
            if voiced:
                # follow the pitch as it falls: re-estimate on this frame
                seg = data[f * spf:(f + 1) * spf + 64]
                pf = pitch(seg, rate)
                freq = pf[0] if (pf and pf[1] > 0.25) else p[0]
                per = max(1, min(1023, int(round(PSG_CLOCK / (32.0 * freq)))))
                if per != prev_per:
                    body.append(0xC0 | (per & 0x0F))    # voice 2 tone
                    body.append((per >> 4) & 0x3F)
                    prev_per = per
                if vol != prev_vol:
                    body.append(0xD0 | vol)
                    prev_vol = vol
            else:
                if f == 0:
                    body.append(0xE7)                   # white noise
                if vol != prev_vol:
                    body.append(0xF0 | vol)
                    prev_vol = vol
            if body:
                stream.append(len(body))
                stream.extend(body)
            stream.append(0x81)                         # next frame
        stream.append(0x00)                             # end of effect
        out[i] = bytes(stream)
        if voiced:
            tones += 1
        else:
            noises += 1
    print(f'{len(out)} effects: {tones} as tones, {noises} as noise bursts, {empty} silent/absent')
    print(f'total {sum(len(v) for v in out.values())} bytes')
    pickle.dump(out, open(a.out, 'wb'))


if __name__ == '__main__':
    main()
