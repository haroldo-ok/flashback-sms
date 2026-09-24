#!/usr/bin/env python3
"""Convert the game's MIDI music to the SMS sound chip.

The PSG has three square-wave voices and one noise voice, so the score has to
be reduced: at every frame the loudest three sounding melodic notes get the
tone channels (a note keeps the channel it already holds, so lines do not jump
between voices), and percussion (MIDI channel 10) drives the noise voice.

Output is a 60 Hz stream of ready-made PSG bytes:

    0x01..0x0F  n : that many PSG bytes follow, written this frame
    0x80|k        : nothing happens for k frames (1..127)
    0x00          : end of track (players loop back to the start)

    midiconv.py DATA/capture.mid --out gen/mus_capture.pkl
"""
import argparse, os, pickle, struct, sys

PSG_CLOCK = 3579545
FPS = 60


def read_varlen(d, p):
    v = 0
    while True:
        b = d[p]
        p += 1
        v = (v << 7) | (b & 0x7F)
        if not (b & 0x80):
            return v, p


def parse_midi(path):
    """-> (events, division) with events as (tick, kind, channel, note, velocity)"""
    d = open(path, 'rb').read()
    assert d[:4] == b'MThd', path
    ntrks, division = struct.unpack('>HH', d[10:14])
    p = 14
    events = []
    for _ in range(ntrks):
        assert d[p:p + 4] == b'MTrk'
        length = struct.unpack('>I', d[p + 4:p + 8])[0]
        end = p + 8 + length
        q = p + 8
        tick = 0
        status = 0
        while q < end:
            delta, q = read_varlen(d, q)
            tick += delta
            if d[q] & 0x80:
                status = d[q]
                q += 1
            if status == 0xFF:                       # meta
                kind = d[q]
                q += 1
                ln, q = read_varlen(d, q)
                if kind == 0x51:                     # tempo
                    events.append((tick, 'tempo', 0, struct.unpack('>I', b'\0' + d[q:q + 3])[0], 0))
                q += ln
            elif status in (0xF0, 0xF7):
                ln, q = read_varlen(d, q)
                q += ln
            else:
                hi, ch = status & 0xF0, status & 0x0F
                if hi in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                    a, b = d[q], d[q + 1]
                    q += 2
                    if hi == 0x90 and b > 0:
                        events.append((tick, 'on', ch, a, b))
                    elif hi == 0x80 or (hi == 0x90 and b == 0):
                        events.append((tick, 'off', ch, a, 0))
                else:                                 # program change, aftertouch
                    q += 1
        p = end
    events.sort(key=lambda e: e[0])
    return events, division


def tone_period(note):
    f = 440.0 * 2 ** ((note - 69) / 12.0)
    n = int(round(PSG_CLOCK / (32.0 * f)))
    return max(1, min(1023, n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('midi')
    ap.add_argument('--out', required=True)
    ap.add_argument('--max-seconds', type=float, default=100.0)
    a = ap.parse_args()

    events, division = parse_midi(a.midi)
    # ticks -> frames, following tempo changes
    tempo = 500000                     # microseconds per quarter note
    frames, tick, last = [], 0, 0.0
    timed = []
    for ev in events:
        dt = ev[0] - tick
        last += dt * (tempo / division) / 1e6
        tick = ev[0]
        if ev[1] == 'tempo':
            tempo = ev[3]
            continue
        timed.append((last, ev[1], ev[2], ev[3], ev[4]))

    total = int(min(a.max_seconds, timed[-1][0] if timed else 0) * FPS) + 1
    sounding = {}                      # (ch, note) -> velocity
    chan_note = [None, None, None]     # what each tone voice holds
    noise_timer, noise_vol = 0, 15
    stream = bytearray()
    idx = 0
    pending = []                       # PSG bytes for this frame
    prev_period = [None] * 3
    prev_vol = [15] * 3
    prev_noise_vol = 15
    wait = 0

    def flush(pending):
        nonlocal wait
        if not pending:
            wait += 1
            return
        while wait:
            k = min(wait, 127)
            stream.append(0x80 | k)
            wait -= k
        for i in range(0, len(pending), 15):
            chunk = pending[i:i + 15]
            stream.append(len(chunk))
            stream.extend(chunk)
        wait += 1

    for f in range(total):
        t = f / FPS
        while idx < len(timed) and timed[idx][0] <= t:
            _, kind, ch, note, vel = timed[idx]
            idx += 1
            if ch == 9:                                  # percussion -> noise
                if kind == 'on':
                    noise_timer = 4
                    noise_vol = max(0, 15 - (vel >> 3))
                continue
            if kind == 'on':
                sounding[(ch, note)] = vel
            else:
                sounding.pop((ch, note), None)
        # pick the three loudest notes, keeping voices where they are
        want = sorted(sounding.items(), key=lambda kv: (-kv[1], -kv[0][1]))[:3]
        want_keys = [k for k, _ in want]
        for c in range(3):
            if chan_note[c] is not None and chan_note[c] not in want_keys:
                chan_note[c] = None
        for k in want_keys:
            if k not in chan_note:
                for c in range(3):
                    if chan_note[c] is None:
                        chan_note[c] = k
                        break
        pending = []
        for c in range(3):
            k = chan_note[c]
            if k is None:
                if prev_vol[c] != 15:
                    pending.append(0x90 | (c << 5) | 15)
                    prev_vol[c] = 15
                continue
            per = tone_period(k[1])
            if per != prev_period[c]:
                pending.append(0x80 | (c << 5) | (per & 0x0F))
                pending.append((per >> 4) & 0x3F)
                prev_period[c] = per
            vol = max(0, 15 - (sounding[k] >> 3))
            if vol != prev_vol[c]:
                pending.append(0x90 | (c << 5) | vol)
                prev_vol[c] = vol
        if noise_timer > 0:
            noise_timer -= 1
            v = min(15, noise_vol + (4 - noise_timer) * 3)
            if v != prev_noise_vol:
                if prev_noise_vol == 15:
                    pending.append(0xE7)               # white noise, mid rate
                pending.append(0xF0 | v)
                prev_noise_vol = v
        elif prev_noise_vol != 15:
            pending.append(0xFF)
            prev_noise_vol = 15
        flush(pending)
    while wait:                      # trailing silence, so the track keeps its length
        k = min(wait, 127)
        stream.append(0x80 | k)
        wait -= k
    stream.append(0x00)
    print(f'{os.path.basename(a.midi)}: {total} frames ({total/FPS:.1f}s), '
          f'{len(stream)} bytes of PSG stream')
    pickle.dump(dict(stream=bytes(stream), frames=total,
                     name=os.path.basename(a.midi)), open(a.out, 'wb'))


if __name__ == '__main__':
    main()
