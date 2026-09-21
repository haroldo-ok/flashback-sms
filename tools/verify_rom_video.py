#!/usr/bin/env python3
"""ROM-level FMV verification: does the *actual ROM* put the right picture on
screen for every position of every cutscene?

The offline verifier (`encode_shared_dictionary.py --verify`) proves the *stream*
decodes to the source frames; this one proves the *device code* renders it.

Method (one capture per video position, deterministic):
  for each cutscene, drive the ROM to that cutscene in the smstest emulator
  (menu taps / room warps), then for every source position p:
      waituntil video_pos    == p     # the player is on position p
      waituntil video_active == 0     # its delta finished uploading
      screenshot                      # -> compare with source frame p
  `video_active` goes 1 while a delta is being copied into VRAM, so the capture
  never catches a half-drawn window (that was the flaw of the first per-tick
  version of this tool: mid-upload captures match no single source frame).
  Position 0 is captured the same way: the player signals 0xFFFF while it paints
  the opening snapshot, then parks on video_pos == 0 for its display time.

Usage:
  verify_rom_video.py --rom sms/flashback.sms --noi sms/flashback_base.noi \
                      --frames /tmp/frames --work /tmp/romvideo \
                      [--cutscenes logos,intro1,...] [--keep] [--reuse]
"""
import argparse
import os
import shutil
import subprocess
import sys

import numpy as np

SMSTEST = os.path.expanduser('~/.devkitsms/bin/smstest')
W, H = 256, 96
WIN_Y = 48

# --- how to reach each cutscene, in the smstest command language -------------
# cur_mode: LOGOS 1 / INTRO 2 / TITLE 3 / GAMEPLAY 4 / CUTSCENE 5 / INSTR 6
# cur_scene: SC_LOGO+1 = 1 ... SC_DESINTEG+1 = 7 while that cutscene plays
TO_TITLE = ['waituntil cur_mode == 3 4000 w1']


def menu_option(n):
    """Title menu: option 1 is the initial cursor position."""
    out = []
    for _ in range(n - 1):
        out += ['hold down', 'run 4', 'hold none', 'run 4']
    out += ['tap 1', 'run 4']
    return out


CUTS = {
    'logos':    dict(scene=1, nav=[]),
    'intro1':   dict(scene=2, nav=TO_TITLE + menu_option(2)),
    'intro2':   dict(scene=3, nav=TO_TITLE + menu_option(2)),   # follows intro1
    'holocube': dict(scene=4, nav=TO_TITLE + menu_option(3)),
    'debut':    dict(scene=5, nav=TO_TITLE + menu_option(1)),
    'objet':    dict(scene=6, nav=TO_TITLE + menu_option(1) + [
        'waituntil cur_mode == 4 2000 w1',
        'poke warp_room 28 w1', 'run 20',
        'poke px 200 w2', 'poke py 200 w2',
        'hold 2', 'run 3', 'hold none']),
    'desinteg': dict(scene=7, nav=TO_TITLE + menu_option(1) + [
        'waituntil cur_mode == 4 2000 w1',
        'poke warp_room 63 w1']),
}


def sms_rgb(byte):
    return ((byte & 3) * 85, ((byte >> 2) & 3) * 85, ((byte >> 4) & 3) * 85)


def load_ideal(frames_dir, name):
    pal = np.array([sms_rgb(b) for b in
                    open(os.path.join(frames_dir, f'pal_{name}.bin'), 'rb').read()[:16]],
                   dtype=np.uint8)
    data = open(os.path.join(frames_dir, f'frames_{name}.bin'), 'rb').read()
    n = len(data) // (W * H)
    idx = np.frombuffer(data[:n * W * H], dtype=np.uint8).reshape(n, H, W)
    return pal[idx & 15]                      # (n, H, W, 3)


def build_script(rom_cut, ppm_dir, nframes):
    s = [f'symbols {{noi}}'] + rom_cut['nav']
    s.append(f"waituntil cur_scene == {rom_cut['scene']} 4000 w1")
    # 1. position 0: the player writes 0xffff while painting the opening
    #    snapshot, then parks on video_pos == 0 for its display time.  The poke
    #    (a test-only variable, 0 in normal play) lengthens every position's
    #    hold so a per-position poll can never miss a position on a busy frame.
    s.append('waituntil video_pos == 65535 900 w2')
    s.append('poke video_hold_extra 3 w1')
    s.append('waituntil video_pos == 0 400 w2')
    s.append('waituntil video_active == 0 20 w1')
    s.append(f'screenshot {ppm_dir}/p0000.ppm')
    for p in range(1, nframes):
        # wait for the position, then for its delta to *start* and finish: the
        # active flag is also 0 just before the delta begins, and capturing
        # then would photograph the previous position.
        s.append(f'waituntil video_pos == {p} 4000 w2')
        s.append('waituntil video_active == 1 30 w1')
        s.append('waituntil video_active == 0 400 w1')
        s.append(f'screenshot {ppm_dir}/p{p:04d}.ppm')
    return s


def run_smstest(rom, noi, work, name, nframes):
    ppm_dir = os.path.join(work, name)
    os.makedirs(ppm_dir, exist_ok=True)
    script = build_script(CUTS[name], ppm_dir, nframes)
    path = os.path.join(work, f'{name}.txt')
    open(path, 'w').write('\n'.join(script).replace('{noi}', noi) + '\n')
    r = subprocess.run([SMSTEST, rom, path], capture_output=True, text=True)
    warn = [l for l in r.stdout.splitlines()
            if 'FAIL' in l or 'WARN' in l or 'cannot' in l or 'timeout' in l]
    return ppm_dir, warn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--noi', required=True)
    ap.add_argument('--frames', required=True)
    ap.add_argument('--work', default='/tmp/romvideo')
    ap.add_argument('--cutscenes', default=','.join(CUTS))
    ap.add_argument('--min-match', type=float, default=100.0,
                    help='percent of window pixels that must match the source '
                         'frame for a position to count as correct')
    ap.add_argument('--keep', action='store_true', help='keep the screenshots')
    ap.add_argument('--reuse', action='store_true',
                    help='re-use screenshots already in the work dir')
    a = ap.parse_args()

    from PIL import Image
    ok = True
    for name in a.cutscenes.split(','):
        ideal = load_ideal(a.frames, name)
        n = len(ideal)
        ppm_dir = os.path.join(a.work, name)
        if a.reuse and os.path.isdir(ppm_dir) and len(os.listdir(ppm_dir)) >= n:
            warn = []
        else:
            ppm_dir, warn = run_smstest(a.rom, a.noi, a.work, name, n)
        got = 0
        match = np.zeros(n)
        for p in range(n):
            f = os.path.join(ppm_dir, f'p{p:04d}.ppm')
            if not os.path.exists(f):
                continue
            try:
                img = np.array(Image.open(f).convert('RGB'))[WIN_Y:WIN_Y + H]
            except OSError:
                continue                      # truncated write: not captured
            match[p] = 100.0 * (img == ideal[p]).all(axis=2).sum() / (W * H)
            got += 1
        shown = match >= a.min_match
        bad = [p for p in range(n) if not shown[p]]
        status = 'OK' if (not bad and got == n) else 'FAIL'
        if status == 'FAIL':
            ok = False
        print(f'{name:9s} {n:4d} positions: captured {got:4d}  '
              f'match min {match.min():6.2f}% mean {match.mean():7.3f}%  '
              f'correct {int(shown.sum()):4d}  {status}')
        if bad:
            print('          wrong positions: '
                  + ', '.join(f'p{p}({match[p]:.2f}%)' for p in bad[:16])
                  + (' ...' if len(bad) > 16 else ''))
        if warn:
            print(f'          emulator: {warn[0]}'
                  + (f' (+{len(warn)-1} more)' if len(warn) > 1 else ''))
        # keep the screenshots of a failing cutscene for diagnosis, drop the
        # ones that verified clean (they fill up the tmpfs fast)
        if status == 'OK' and not a.keep:
            shutil.rmtree(ppm_dir, ignore_errors=True)
    print('RESULT:', 'ALL PASSED' if ok else 'FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
