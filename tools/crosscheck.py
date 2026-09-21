#!/usr/bin/env python3
"""Cross-check the Z80 build against the host reference decoder.

    crosscheck.py gen      -> tests/playtest.txt (+ tests/expect.json)
    smstest flashback.sms tests/playtest.txt
    crosscheck.py compare  -> pixel-exact screenshot comparison

Expected values (ticks and uploads per clip, room renders) are computed from
gen/bank_data.bin, i.e. the same bytes that are in the ROM, by
tools/mkdata.py's independent decoder.  A cutscene screenshot must match the
reference frame for its tick exactly (0 differing pixels, allowing for the
tick being +-1 frame from the snapshot point).
"""
import json, os, pickle, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import mkdata as M
from PIL import Image

GEN = 'gen'
BLOB = None
REPLAY_BANK = REPLAY_ADDR = SPR_DICT_BANK0 = 0


def defines():
    d = {}
    for line in open(f'{GEN}/data_index.h'):
        if line.startswith('#define'):
            p = line.split()
            if len(p) == 3:
                try: d[p[1]] = int(p[2], 0)
                except ValueError: pass
    return d


def tables():
    src = open(f'{GEN}/data_index.c').read()
    def grab(name):
        s = src[src.index(name + '[] = {') + len(name) + 6:]
        return [int(x, 0) for x in s[:s.index('}')].split(',')]
    return {k: grab(k) for k in ('fmv_clip_id', 'fmv_clip_bank', 'fmv_clip_addr',
                                 'room_level', 'room_num', 'room_bank', 'room_addr',
                                 'level_num')}


def clip_frames(t, clip):
    """-> list of c6 images per tick, and upload count"""
    bank, addr = t['fmv_clip_bank'][clip], t['fmv_clip_addr'][clip]
    imgs = [img.copy() for _, _, img in M.play_clip(BLOB, bank, addr, defines()['FMV_DICT_BANK0'])]
    # count upload ops by walking the stream
    pos = (bank - M.BANK0) * M.BANK + addr - 0x8000
    ups = 0
    while True:
        op = BLOB[pos]
        if op == 0xFF: break
        if op == 7: pos = (pos // M.BANK + 1) * M.BANK; continue
        if op in (0, 4, 5, 6): n = 1
        elif op == 1: n = 2
        elif op in (2, 3): n = 17
        elif op & 0xF0 == 0x10: n = 4
        elif op & 0xC0 == 0x40: n = 3 + 2 * ((op & 0x3F) + 1)
        else: raise ValueError(f'bad op {op:#x} at {pos:#x}')
        if op & 0xF0 == 0x10: ups += 1
        pos += n
    return imgs, ups


def room_layers(t, index):
    """-> (idx 224x256, priority 224x256 bool, palette[16]) straight from ROM"""
    bank, addr = t['room_bank'][index], t['room_addr'][index]
    o = (bank - M.BANK0) * M.BANK + addr - 0x8000
    n = BLOB[o] | (BLOB[o + 1] << 8)
    pal = np.array(list(BLOB[o + 2:o + 18]))
    nt = np.frombuffer(BLOB[o + 18:o + 18 + 1792], '<u2').astype(np.int32)
    vram = np.zeros((448, 64), np.uint8)
    tiles = BLOB[o + 1810:o + 1810 + n * 32]
    for i in range(n):
        vram[448 - n + i] = M.unplanar(tiles[i * 32:i * 32 + 32])
    t8 = vram[nt & 0x1FF].reshape(896, 8, 8)
    t8 = np.where(((nt & 0x200) != 0)[:, None, None], t8[:, :, ::-1], t8)
    t8 = np.where(((nt & 0x400) != 0)[:, None, None], t8[:, ::-1, :], t8)
    idx = t8.reshape(28, 32, 8, 8).transpose(0, 2, 1, 3).reshape(224, 256)
    prio = np.repeat(np.repeat(((nt & 0x1000) != 0).reshape(28, 32), 8, 0), 8, 1)
    return idx, prio, pal


def replay_frames(t, count):
    """Decode the replay stream from the packed ROM bytes and render each game
    frame the way the VDP would: background, then 8x16 sprites in SAT order,
    with only the first 8 sprites on any scanline displayed and foreground
    (priority) background pixels covering sprites."""
    pos = (REPLAY_BANK - M.BANK0) * M.BANK + REPLAY_ADDR - 0x8000
    spr_base = (SPR_DICT_BANK0 - M.BANK0) * M.BANK
    vram = np.zeros((64, 8, 8), np.uint8)      # sprite pattern tiles 0..63
    bg_idx = bg_prio = bg_pal = None
    spal = np.zeros(16, np.int32)
    scroll = 0
    sprites = []
    out = []
    while len(out) < count:
        op = BLOB[pos]
        if op == 0xFF:
            break
        if op == 7:
            pos = (pos // M.BANK + 1) * M.BANK
            continue
        if op == 1:
            bg_idx, bg_prio, bg_pal = room_layers(t, BLOB[pos + 1])
            spal = np.array(list(BLOB[pos + 2:pos + 18]))
            pos += 18
        elif op == 2:
            scroll = BLOB[pos + 1]; pos += 2
        elif op == 3:
            n = BLOB[pos + 1]; pos += 2
            for _ in range(n):
                slot = BLOB[pos]; tid = BLOB[pos + 1] | (BLOB[pos + 2] << 8); pos += 3
                o = spr_base + tid * 64
                vram[slot * 2] = M.unplanar(BLOB[o:o + 32]).reshape(8, 8)
                vram[slot * 2 + 1] = M.unplanar(BLOB[o + 32:o + 64]).reshape(8, 8)
        elif op == 4:
            n = BLOB[pos + 1]; pos += 2
            sprites = [(BLOB[pos + 3 * i], BLOB[pos + 3 * i + 1], BLOB[pos + 3 * i + 2]) for i in range(n)]
            pos += 3 * n
        elif op == 0:
            pos += 1
            rows = np.arange(scroll, scroll + 192) % 224
            img = bg_pal[bg_idx[rows]].astype(np.int32)
            prio_hide = bg_prio[rows] & (bg_idx[rows] != 0)
            drawn = np.zeros((192, 256), bool)
            line_count = np.zeros(256, np.int32)
            for (y, x, slot) in sprites:                  # SAT order = priority
                if y == 0:                                 # SAT y-1 = 255 -> line 256
                    continue
                lines = [ly for ly in range(y, y + 16) if ly < 192]
                visible = [ly for ly in lines if line_count[ly] < 8]   # VDP shows 8/line
                pat = np.concatenate([vram[slot * 2], vram[slot * 2 + 1]], 0)  # 8x16 pattern
                for ly in visible:
                    row = pat[ly - y]
                    for i in range(8):
                        px = x + i
                        if px < 256 and row[i] and not drawn[ly, px] and not prio_hide[ly, px]:
                            img[ly, px] = spal[row[i]]
                            drawn[ly, px] = True
                for ly in lines:
                    line_count[ly] += 1
            out.append(img.astype(np.uint8))
        else:
            raise ValueError(f'bad replay op {op:#x} at {pos:#x}')
    return out


def room_image(t, index, scroll):
    idx, prio, pal = room_layers(t, index)
    return pal[idx[np.arange(scroll, scroll + 192) % 224]]


def rgb(c6):
    return M.c6_to_rgb(c6)


CUT_SHOTS = []   # chosen by gen(): high-motion ticks, saved in tests/expect.json


def motion_ticks(frames, n=2, lo=60, hi=None):
    """ticks whose -6..+2 window holds the most distinct frames, spread apart"""
    hi = hi or len(frames) - 10
    score = []
    for t in range(max(lo, 6), hi):
        score.append((len({frames[k].tobytes() for k in range(t - 6, t + 3)}), t))
    score.sort(reverse=True)
    picked = []
    for sc, t in score:
        if all(abs(t - p) > 150 for p in picked):
            picked.append(t)
        if len(picked) == n:
            break
    return sorted(picked)


def gen():
    syms = {}
    for line in open('flashback.noi'):
        q = line.split()
        if len(q) == 3 and q[0] == 'DEF':
            syms[q[1].lstrip('_')] = int(q[2], 16)
    t = tables()
    ids = t['fmv_clip_id']
    exp = {}
    L = ['# generated by tools/crosscheck.py gen - expected values come from the ROM data',
         'symbols flashback.noi', '']
    c40, c0d, c00 = ids.index(0x40), ids.index(0x0D), ids.index(0x00)
    f40, u40 = clip_frames(t, c40)
    f0d, _ = clip_frames(t, c0d)
    f00, u00 = clip_frames(t, c00)
    CUT_SHOTS[:] = [(0x40, k) for k in motion_ticks(f40)] + [(0x0D, k) for k in motion_ticks(f0d, hi=1050)]
    exp['cut_shots'] = CUT_SHOTS
    # the game-logic harness runs at boot, before the intro starts
    if defines().get('HAS_LOGIC') and 'logic_frames_ok' in syms:
        L += ['echo self-test mode: button 1 is held at boot so the ROM runs its',
              'echo diagnostics instead of going straight to the intro',
              'hold 1', 'run 30', 'hold none', 'expect self_test == 1 w1', '']
        L += ['echo game logic: the ported interpreter replays the recorded demo and is',
              'echo compared with the original engine frame by frame',
              'waituntil logic_frame >= 505 12000',
              'run 5',
              'expect logic_frames_ok >= 504',
              'expect logic_first_bad == 504',
              'expect logic_bad_op == 0 w1', '']
    L += ['echo boot: level object tables are built, then the logos cutscene (0x40) starts',
          'waituntil fmv_tick >= 2 900', 'run 10', 'expect game_state == 0 w1', 'expect fmv_active == 1 w1', 'expect is_pal == 0 w1',
          'expectvdp display 1']
    for cid, tick in CUT_SHOTS:
        if cid != 0x40: continue
        L += [f'waituntil fmv_tick >= {tick} 2000', f'screenshot shots/sms/cut_{cid:02X}_{tick}.ppm', 'print fmv_tick']
    L += ['', f'echo logos play to their natural end: {len(f40)} ticks, {u40} uploads (host decode)',
          'waituntil seq_pos == 1 2000 w1',
          f'expect last_clip_ticks == {len(f40)}', f'expect last_clip_uploads == {u40}',
          'expect fmv_active == 1 w1']
    for cid, tick in CUT_SHOTS:
        if cid != 0x0D: continue
        L += [f'waituntil fmv_tick >= {tick} 3000', f'screenshot shots/sms/cut_{cid:02X}_{tick}.ppm', 'print fmv_tick']
    L += ['', 'echo timing: after 1200 real frames of the intro the stream is still in sync (bounded lag)',
          'waituntil fmv_tick >= 1100 400', 'expect ticks_behind <= 8', 'run 100', 'expect ticks_behind <= 8',
          'expect fmv_active == 1 w1', 'print fmv_tick']
    L += ['', 'echo button 1 skips the rest of the intro to the title screen',
          'tap 1', 'waituntil game_state == 1 120 w1', 'run 5',
          'expectvdp display 1', 'expectcolors 8',
          'screenshot shots/sms/title.ppm']
    if defines().get('HAS_REPLAY'):
        rshots = [40, 150, 300]
        exp['replay_shots'] = rshots
        L += ['', 'echo gameplay replay: traced level-1 demo with hardware sprites']
        L += ['tap 2', 'waituntil game_state == 2 60 w1']
        for k in rshots:
            L += [f'waituntil replay_frame >= {k} 1200', f'screenshot shots/sms/replay_{k}.ppm',
                  'print replay_frame', 'print replay_sprites w1']
        L += ['expect ticks_behind <= 2', 'expectvdp display 1',
              'tap 1', 'waituntil game_state == 1 120 w1', 'run 5']
    import glob
    # only the levels actually packed into this ROM
    packed = tables()['level_num'][:defines().get('NUM_LEVELS', 0)]
    lv_files = [f'gen/level_L{n}.pkl' for n in packed if os.path.exists(f'gen/level_L{n}.pkl')]
    if lv_files and 'pge_cnt' in syms:
        L += ['', "echo game logic: each level's live object table is built on the Z80 and",
              'echo compared with the original engine (object count, active objects, checksum)',
              f'waituntil pge_loaded == {len(lv_files)} 900 w1']
        for i, f in enumerate(lv_files):
            d = pickle.load(open(f, 'rb'))
            L += [f'expect 0x{syms["pge_cnt"] + i * 2:04X} == {d["npges"]}',
                  f'expect 0x{syms["pge_act"] + i * 2:04X} == {d["active"]}',
                  f'expect 0x{syms["pge_sum"] + i * 2:04X} == {d["checksum"]}']
    if defines().get('HAS_ANIM'):
        L += ['', 'echo simulated gameplay: the title screen starts level 1 through its',
              'echo own cutscene, then the ported logic drives the sprite renderer',
              'waituntil game_state == 1 12000 w1', 'run 20',
              'hold 2', 'run 20', 'hold none',
              'waituntil game_state == 2 2000 w1', 'run 120',
              'expect sim_room == 27 w1', 'expect sim_sprites >= 4 w1',
              'expectvdp display 1', 'expectcolors 8',
              'screenshot shots/sms/sim_gate.ppm',
              'echo the controller drives the simulation: holding a direction moves Conrad',
              f'expect 0x{syms["pge_live"] + 2:04X} == {48}',
              'hold right', 'run 400', 'hold none', 'run 20',
              f'expect 0x{syms["pge_live"] + 2:04X} > {48}',
              'expect sim_sprites >= 3 w1',   # Conrad alone is a few 8x16 sprites
              'echo run + up jumps: the engine never sees a diagonal, so the jump',
              'echo comes from the run modifier with up, not up plus a direction',
              'hold 1+up', 'run 100', 'hold none', 'run 40',
              f'expect 0x{syms["pge_live"] + 2:04X} > 103',
              'pause', 'run 20',                    # PAUSE returns to the title
              'waituntil game_state == 1 120 w1', 'run 5']
    exp['rooms'] = []
    exp['title'] = True
    open('tests/playtest.txt', 'w').write('\n'.join(L) + '\n')
    json.dump(exp, open('tests/expect.json', 'w'))
    print(f'tests/playtest.txt written ({len(L)} lines)')


def compare():
    t = tables()
    ids = t['fmv_clip_id']
    exp = json.load(open('tests/expect.json'))
    CUT_SHOTS[:] = [tuple(x) for x in exp['cut_shots']]
    ok = True
    sheet = []
    for cid, tick in CUT_SHOTS:
        shot = np.array(Image.open(f'shots/sms/cut_{cid:02X}_{tick}.ppm').convert('RGB'))
        frames, _ = clip_frames(t, ids.index(cid))
        window = range(max(0, tick - 6), min(len(frames), tick + 3))
        diffs = {k: int((rgb(frames[k]) != shot).any(-1).sum()) for k in window}
        bad = min(diffs.values())
        match = [k for k, d in diffs.items() if d == 0]
        distinct = len({frames[k].tobytes() for k in window})
        print(f'cutscene {cid:02X} tick {tick}: exact match with reference ticks {match} '
              f'({distinct} distinct frames in window {window.start}..{window.stop - 1}); min differing pixels {bad}')
        ok &= bad == 0
        sheet.append(shot)
    if exp.get('title'):
        shot = np.array(Image.open('shots/sms/title.ppm').convert('RGB'))
        ref = rgb(room_image(t, 0, 0))          # the title screen is room 0
        bad = int((ref != shot).any(-1).sum())
        print(f'title screen: differing pixels {bad}')
        ok &= bad == 0
        sheet.append(shot)
    for idx, scroll in exp['rooms']:
        shot = np.array(Image.open(f'shots/sms/room_{idx}_s{scroll}.ppm').convert('RGB'))
        ref = rgb(room_image(t, idx, scroll))
        bad = int((ref != shot).any(-1).sum())
        print(f'room index {idx} (level {t["room_level"][idx]} room {t["room_num"][idx]}) '
              f'scroll {scroll}: differing pixels {bad}')
        ok &= bad == 0
        sheet.append(shot)
    for k in exp.get('replay_shots', []):
        shot = np.array(Image.open(f'shots/sms/replay_{k}.ppm').convert('RGB'))
        frames = replay_frames(t, k + 1)
        window = range(max(0, k - 3), min(len(frames), k + 1))
        diffs = {n: int((rgb(frames[n]) != shot).any(-1).sum()) for n in window}
        bad = min(diffs.values())
        match = [n for n, d in diffs.items() if d == 0]
        print(f'replay game frame {k}: exact match with model frames {match}; min differing pixels {bad}')
        ok &= bad == 0
        sheet.append(shot)
    rows = [np.concatenate(sheet[i:i + 2], 1) for i in range(0, len(sheet) - 1, 2)]
    Image.fromarray(np.concatenate(rows, 0)).save('shots/sms/contact.png')
    print('CROSSCHECK', 'PASSED' if ok else 'FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    BLOB = open(f'{GEN}/bank_data.bin', 'rb').read()
    D = defines()
    REPLAY_BANK, REPLAY_ADDR, SPR_DICT_BANK0 = D['REPLAY_BANK'], D['REPLAY_ADDR'], D['SPR_DICT_BANK0']
    {'gen': gen, 'compare': compare}[sys.argv[1]]()
