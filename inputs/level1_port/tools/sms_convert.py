#!/usr/bin/env python3
"""Convert Flashback DOS demo data -> devkitSMS C assets (banked)."""
import sys, os, struct
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
import fbextract as fb

ROOT = os.path.join(os.path.dirname(__file__), "..")
DATA = os.path.join(ROOT, "assets", "DATA")
GEN = os.path.join(ROOT, "sms", "gen")
os.makedirs(GEN, exist_ok=True)

arch = fb.read_aba(os.path.join(DATA, "DEMO_UK.ABA"))

def sms_rgb(r, g, b):
    # map 0-255 -> 0-3 per channel
    def q(v): return min(3, v * 4 // 256)
    return (q(b) << 2) | (q(g) << 1) | q(r)  # SMS RGB(r,g,b) macro = r|g<<1|b<<2? verify

# SMSlib RGB macro: #define RGB(r,g,b) ((r)|((g)<<1)|((b)<<2))  -> r bit0-1,g bit2-3,b bit4-5
def sms_color(r, g, b):
    def q(v): return (v * 3 + 127) // 255
    return (q(r) & 3) | ((q(g) & 3) << 2) | ((q(b) & 3) << 4)

def chunky_to_planar(tile):
    # SMS tile: 8 rows x 4 bytes (byte p = bitplane p for that row)
    out = bytearray(32)
    for row in range(8):
        for plane in range(4):
            bit = 1 << plane
            b = 0
            for col in range(8):
                if tile[row * 8 + col] & bit:
                    b |= 1 << (7 - col)
            out[row * 4 + plane] = b
    return bytes(out)

def vq_room(A, K):
    h, w = A.shape
    cells = A.reshape(28, 8, 32, 8).transpose(0, 2, 1, 3).reshape(28 * 32, 64).astype(np.int16)
    uniq, inv = np.unique(cells, axis=0, return_inverse=True)
    X = uniq.astype(np.float32)
    var = X.var(1)
    flat = var < 2.0
    flat_ids = np.where(flat)[0]
    rest_ids = np.where(~flat)[0]
    Krest = max(1, K - len(flat_ids))
    if len(rest_ids) > Krest:
        Xr = X[rest_ids]
        rng = np.random.default_rng(0)
        C = Xr[rng.choice(len(Xr), Krest, replace=False)]
        for it in range(12):
            D = ((Xr[:, None, :] - C[None, :, :]) ** 2).sum(-1); asg = D.argmin(1)
            newC = C.copy()
            for k in range(Krest):
                m = asg == k
                if m.any(): newC[k] = Xr[m].mean(0)
            C = newC
        D = ((Xr[:, None, :] - C[None, :, :]) ** 2).sum(-1); asg = D.argmin(1)
        med = np.arange(len(Xr))
        for k in range(Krest):
            m = np.where(asg == k)[0]
            if len(m):
                sub = Xr[m]; dd = ((sub[:, None, :] - sub[None, :, :]) ** 2).sum(-1).sum(1); med[m] = m[dd.argmin()]
        reps = {}
        code_of = np.zeros(len(uniq), dtype=np.int64)
        tiles = [uniq[i] for i in flat_ids]
        for i in flat_ids: code_of[i] = len(tiles) - 1 if False else 0
        # rebuild properly
        tiles = []
        for i in flat_ids: code_of[i] = len(tiles); tiles.append(uniq[i])
        for j, rid in enumerate(rest_ids):
            rep = rest_ids[med[j]]
            if rep not in reps: reps[rep] = len(tiles); tiles.append(uniq[rep])
            code_of[rid] = reps[rep]
    else:
        code_of = np.arange(len(uniq))
        tiles = [uniq[i] for i in range(len(uniq))]
    tilemap = code_of[inv].astype(np.uint16)
    tiles = np.array(tiles, dtype=np.uint8) if tiles else np.zeros((1, 64), np.uint8)
    return tiles, tilemap

print("converter loaded")
