#!/usr/bin/env python3
"""Gameplay trace -> per-frame inputs and expected object state for the Z80
game-logic port.

For each 30 Hz game frame the trace holds the demo input byte the engine
consumed and the state of every object afterwards.  This writes both to ROM:

  u16 frames, u16 npges, u8 start_room, u8 start_x, u8 start_y, u8 pad,
  then per frame: u8 input, u16 checksum

The start point is the demo's own (the engine moves Conrad there on load), read
from the demo_D*.bin the extractor writes next to the trace.

The checksum sums, over every object, its x, y, animation number, object
type, flags, room, animation sequence and entry point.  The Z80 computes the
same value after running its own frame; the first frame where they differ is
exactly where the port diverges from the original engine.

    logicconv.py capture/trace_D0.fbt --out gen/logic_D0.pkl
"""
import argparse, os, pickle, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from tracefmt import read_fbt


def frame_checksum(pges):
    s = 0
    for r in pges:
        x = int(r[0]) | (int(r[1]) << 8)
        y = int(r[2]) | (int(r[3]) << 8)
        anim = int(r[4]) | (int(r[5]) << 8)
        typ = int(r[6]) | (int(r[7]) << 8)
        s += x + y + anim + typ + int(r[8]) + int(r[9]) + int(r[10]) + int(r[11])
    return s & 0xFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--out', required=True)
    ap.add_argument('--frames', type=int, default=0)
    a = ap.parse_args()
    rows = []
    npges = 0
    for f in read_fbt(a.trace, a.frames or None):
        npges = len(f['pges'])
        rows.append((f['demo_input'], frame_checksum(f['pges'])))
    start = os.path.join(os.path.dirname(a.trace),
                         'demo_D' + os.path.basename(a.trace).split('_D')[1].split('.')[0] + '.bin')
    sd = open(start, 'rb').read() if os.path.exists(start) else bytes(4)
    blob = struct.pack('<HH', len(rows), npges) + bytes([sd[1], sd[2], sd[3], 0])
    print(f'  demo start: room {sd[1]} x {sd[2]} y {sd[3]}')
    for inp, chk in rows:
        blob += struct.pack('<BH', inp, chk)
    print(f'{os.path.basename(a.trace)}: {len(rows)} frames, {npges} objects, '
          f'{len(blob)} bytes of inputs + expected checksums')
    print(f'  first frames: ' + ', '.join(f'in 0x{i:02X} chk 0x{c:04X}' for i, c in rows[:4]))
    pickle.dump(dict(blob=blob, frames=len(rows), npges=npges), open(a.out, 'wb'))


if __name__ == '__main__':
    main()
