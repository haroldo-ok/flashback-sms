#!/usr/bin/env python3
"""Flashback cutscene -> SMS tile-stream encoder.

Every captured cutscene (fbdump .fbv) is re-expressed as a command stream
for a tiny Z80 player.  Design:

* **Shared dictionary.** Every distinct 8x8 4bpp pattern across *all*
  cutscenes is stored once in ROM (H/V-flip canonicalised, so mirrored
  shapes share an entry).  Streams only reference dictionary ids.
* **VRAM as a cache.** The screen is 32x24 = 768 cells but VRAM holds 448
  patterns.  The encoder simulates the VRAM: a cell change is either a
  nametable write pointing at a slot that already holds the pattern (2
  bytes, cheap) or an upload of a dictionary tile into a free slot followed
  by that write.  Flat-shaded polygons repeat patterns heavily, so most
  changes are pure nametable writes.
* **Per-tick budget.** At most MAXUP uploads and MAXNT nametable entries per
  60 Hz tick; the highest-error cells go first and the rest converge on the
  following ticks.  Big shot cuts optionally go through a short display-off
  burst instead of a visible tile wipe.
* **Palettes.** The 32 cutscene colours (plus caption colours) are snapped to
  SMS 6-bit colour and packed into the BG and sprite palettes (both usable
  by background tiles); each tile picks the palette that represents it.

Screen mapping (source is 256x224): picture clip rows 50..177 -> SMS rows
16..143, caption rows 178..223 -> SMS rows 144..189.

    fmvenc.py --out gen/fmv.pkl capture/cut_0D.fbv capture/cut_40.fbv ...
"""
import argparse, os, pickle, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from fbfmt import read_fbv, to_sms_rgb

TW, TH = 32, 24
CELLS = TW * TH
SLOTS = 448
SLOT0 = 1                      # slot 0 = blank tile (pattern of index 0)

# stream opcodes (mirrored in src/fmv.c)
OP_TICK, OP_WAIT, OP_PALBG, OP_PALSPR, OP_DISPOFF, OP_DISPON, OP_CLEAR, OP_NEXTBANK = range(8)
OP_UPLOAD = 0x10               # 0x10|slot_hi, slot_lo, id_lo, id_hi
OP_NTRUN = 0x40                # 0x40|(n-1) [n<=64], cell_lo, cell_hi, n*(lo,hi)
OP_END = 0xFF

FLIPS = [(False, False), (True, False), (False, True), (True, True)]


def variants(p):  # p: (8,8)
    return [p, p[:, ::-1], p[::-1, :], p[::-1, ::-1]]


def canon(p):
    vs = [v.tobytes() for v in variants(p)]
    k = min(vs)
    return k, vs.index(k)


# colour distance in SMS 2-bit space, as a 64x64 table
_c = np.arange(64)
_rgb = np.stack([_c & 3, (_c >> 2) & 3, (_c >> 4) & 3], 1)
DIST = ((_rgb[:, None, :] - _rgb[None, :, :]) ** 2).sum(2)


def composite(pal, pix):
    """source frame -> (192,256) SMS 6-bit colour codes"""
    rgb2 = to_sms_rgb(pal)
    c6 = (rgb2[:, 0] | (rgb2[:, 1] << 2) | (rgb2[:, 2] << 4)).astype(np.uint8)
    fill = pix[223, 0]
    comp = np.full((192, 256), fill, np.uint8)
    comp[16:144] = pix[50:178]
    comp[144:190] = pix[178:224]
    return c6[comp]


def cells_of(img):  # (192,256) -> (768,64)
    return img.reshape(TH, 8, TW, 8).transpose(0, 2, 1, 3).reshape(CELLS, 64)


class PaletteState:
    def __init__(self):
        self.pals = np.zeros((2, 16), np.int32)   # c6 values; entry 0 fixed black
        self.occ = np.zeros((2, 16), bool)
        self.occ[:, 0] = True

    def fit(self, img):
        """Make the frame's colours present, most frequent first.  Stable:
        colours still in use keep their index, so on-screen tiles stay valid.
        Returns the set of palettes that changed."""
        cols, counts = np.unique(img, return_counts=True)
        order = [int(c) for c, _ in sorted(zip(cols, counts), key=lambda x: -x[1])]
        have = {int(self.pals[p, i]) for p in (0, 1) for i in range(16) if self.occ[p, i]}
        missing = [c for c in order if c not in have]
        if not missing:
            return set()
        used = set(order)
        cap = set(int(c) for c in np.unique(img[144:]))
        pic = set(int(c) for c in np.unique(img[:144]))
        changed = set()
        for c in missing:
            pref = [1, 0] if (c in cap and c not in pic) else [0, 1]
            for pb in pref:
                free = [i for i in range(1, 16)
                        if not self.occ[pb, i] or int(self.pals[pb, i]) not in used]
                if free:
                    i = free[0]
                    self.pals[pb, i] = c
                    self.occ[pb, i] = True
                    changed.add(pb)
                    break
            # no room anywhere -> nearest-colour mapping absorbs it (lossy)
        return changed

    def map_cells(self, cells):
        """(768,64) c6 -> palbit(768), idx(768,64), repr c6 (768,64)"""
        best_err = None
        for pb in (0, 1):
            d = DIST[:, self.pals[pb]]                 # 64x16
            nearest = d.argmin(1)                      # c6 -> idx
            idx = nearest[cells]
            err = d[cells, idx].sum(1)
            if best_err is None:
                best_err, best_idx, best_pb = err, idx, np.zeros(CELLS, np.int32)
            else:
                better = err < best_err
                best_err = np.where(better, err, best_err)
                best_idx = np.where(better[:, None], idx, best_idx)
                best_pb = np.where(better, 1, best_pb)
        rep = self.pals[best_pb[:, None], best_idx]
        return best_pb, best_idx.astype(np.uint8), rep


class Encoder:
    def __init__(self, maxup, maxnt, cut_cells, blank_cuts):
        self.dict = {}            # canonical 64-byte key -> id
        self.dict_list = []
        self.maxup, self.maxnt = maxup, maxnt
        self.cut_cells, self.blank_cuts = cut_cells, blank_cuts

    def dict_id(self, key):
        if key not in self.dict:
            self.dict[key] = len(self.dict_list)
            self.dict_list.append(key)
        return self.dict[key]

    def encode_clip(self, frames, name):
        pst = PaletteState()
        slot4 = np.zeros((SLOTS, 4, 64), np.uint8)          # flip variants per slot
        slot_key = [None] * SLOTS
        slot_key[0] = bytes(64)
        slot_of = {bytes(64): 0}
        ref = np.zeros(SLOTS, np.int32); ref[0] = CELLS
        freed_at = np.zeros(SLOTS, np.int64)
        nt_slot = np.zeros(CELLS, np.int32)
        nt_flip = np.zeros(CELLS, np.int32)
        nt_pal = np.zeros(CELLS, np.int32)

        ticks = []                  # list of op-byte-strings, one per tick
        errs = []
        ups_total = nts_total = 0
        dispoff = False
        # source frame -> ideal image (cached)
        src_cache = {}
        end_ms = frames[-1][0] + 400
        nticks = end_ms * 60 // 1000
        si = 0
        pending_pal = set()
        for tick in range(nticks):
            ops = bytearray()
            if tick == 0:
                ops += bytes([OP_CLEAR])
                dispoff = True           # the player starts every clip with display off
            tms = tick * 1000 // 60
            while si + 1 < len(frames) and frames[si + 1][0] <= tms:
                si += 1
            if si not in src_cache:
                src_cache.clear()
                src_cache[si] = composite(frames[si][1], frames[si][2])
            img = src_cache[si]
            pending_pal |= pst.fit(img)
            cells = cells_of(img)
            tpb, tidx, trep = pst.map_cells(cells)

            # what the screen shows now
            shown = pst.pals[nt_pal[:, None], slot4[nt_slot, nt_flip]]
            diff = (shown != cells).sum(1)
            cand = np.nonzero(diff)[0]
            # fast path: nothing to do
            if len(cand) == 0 and not pending_pal:
                if dispoff:
                    ops += bytes([OP_DISPON]); dispoff = False
                ticks.append(bytes(ops)); errs.append(0.0)
                continue
            big = len(cand) >= self.cut_cells
            if self.blank_cuts and big and not dispoff and tick > 0:
                ops += bytes([OP_DISPOFF]); dispoff = True
            maxup = 160 if dispoff else self.maxup
            maxnt = CELLS if dispoff else self.maxnt

            for pb in sorted(pending_pal):
                ops += bytes([OP_PALBG if pb == 0 else OP_PALSPR])
                ops += bytes(int(c) for c in pst.pals[pb])
            pending_pal = set()

            order = cand[np.argsort(-diff[cand], kind='stable')]
            ups = 0
            writes = {}
            freed_now = set()
            for cell in order:
                if len(writes) >= maxnt:
                    break
                key, fl = canon(tidx[cell].reshape(8, 8))
                s = slot_of.get(key)
                if s is None:
                    if ups >= maxup:
                        continue
                    free = np.nonzero((ref == 0))[0]
                    free = [f for f in free if f >= SLOT0 and f not in freed_now]
                    if not free:
                        continue      # VRAM saturated this tick; converge later
                    s = min(free, key=lambda f: freed_at[f])
                    if slot_key[s] is not None:
                        del slot_of[slot_key[s]]
                    slot_key[s] = key
                    slot_of[key] = s
                    p = np.frombuffer(key, np.uint8).reshape(8, 8)
                    slot4[s] = np.array([v.ravel() for v in variants(p)])
                    did = self.dict_id(key)
                    ops += bytes([OP_UPLOAD | (s >> 8), s & 0xFF, did & 0xFF, did >> 8])
                    ups += 1
                old = nt_slot[cell]
                ref[old] -= 1
                if ref[old] == 0:
                    freed_at[old] = tick
                    freed_now.add(old)
                ref[s] += 1
                nt_slot[cell], nt_flip[cell], nt_pal[cell] = s, fl, tpb[cell]
                writes[int(cell)] = s | (0x200 if FLIPS[fl][0] else 0) | (0x400 if FLIPS[fl][1] else 0) | (0x800 if tpb[cell] else 0)
            # nametable runs (after all uploads: never reuse a slot freed this tick)
            cellsw = sorted(writes)
            i = 0
            while i < len(cellsw):
                start = cellsw[i]; run = [writes[start]]; j = i + 1
                while j < len(cellsw) and len(run) < 64:
                    gap = cellsw[j] - (start + len(run))
                    if gap == 0:
                        run.append(writes[cellsw[j]]); j += 1
                    elif gap == 1 and len(run) < 63:        # bridge a 1-cell gap
                        c = start + len(run)
                        run.append(nt_slot[c] | (0x200 if FLIPS[nt_flip[c]][0] else 0) |
                                   (0x400 if FLIPS[nt_flip[c]][1] else 0) | (0x800 if nt_pal[c] else 0))
                    else:
                        break
                ops += bytes([OP_NTRUN | (len(run) - 1), start & 0xFF, start >> 8])
                for e in run:
                    ops += bytes([e & 0xFF, e >> 8])
                i = j
            ups_total += ups; nts_total += len(writes)
            shown = pst.pals[nt_pal[:, None], slot4[nt_slot, nt_flip]]
            err = float((shown != cells).mean()) * 100
            if dispoff and err < 0.5:
                ops += bytes([OP_DISPON]); dispoff = False
            errs.append(err if not dispoff else 100.0)
            ticks.append(bytes(ops))
        # collapse into a stream: ticks end with OP_TICK or OP_WAIT n
        stream = []
        i = 0
        while i < len(ticks):
            body = ticks[i]
            j = i + 1
            while j < len(ticks) and ticks[j] == b'' and j - i < 255:
                j += 1
            n = j - i
            stream.append(body + (bytes([OP_TICK]) if n == 1 else bytes([OP_WAIT, n - 1])))
            i = j
        stream.append(bytes([OP_END]))
        e = np.array(errs)
        print(f'{name}: {len(frames)} src frames, {nticks} ticks, '
              f'{sum(map(len, stream))} stream bytes, uploads {ups_total} '
              f'(avg {ups_total/nticks:.1f}/tick), nt writes {nts_total}, '
              f'pixel err mean {e[e < 100].mean():.2f}% (blank ticks {int((e == 100).sum())})')
        return stream


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('clips', nargs='+')
    ap.add_argument('--out', required=True)
    ap.add_argument('--maxup', type=int, default=16)
    ap.add_argument('--maxnt', type=int, default=96)
    ap.add_argument('--cut-cells', type=int, default=260)
    ap.add_argument('--no-blank-cuts', action='store_true')
    ap.add_argument('--max-ms', action='append', default=[],
                    help='CLIPID:ms  cap a clip that loops waiting for input (e.g. 3B:9000)')
    a = ap.parse_args()
    enc = Encoder(a.maxup, a.maxnt, a.cut_cells, not a.no_blank_cuts)
    caps = {int(k, 16): int(v) for k, v in (m.split(':') for m in a.max_ms)}
    clips = []
    for path in a.clips:
        name = os.path.basename(path).split('.')[0]
        cid = int(name.split('_')[1], 16)
        frames = read_fbv(path)
        if cid in caps:
            frames = [f for f in frames if f[0] <= caps[cid]]
        clips.append((cid, enc.encode_clip(frames, name)))
    print(f'shared dictionary: {len(enc.dict_list)} tiles = {len(enc.dict_list) * 32} bytes')
    os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
    with open(a.out, 'wb') as f:
        pickle.dump(dict(dict=enc.dict_list, clips=clips), f)


if __name__ == '__main__':
    main()
