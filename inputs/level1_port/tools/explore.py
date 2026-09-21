#!/usr/bin/env python3
"""Explore the Flashback demo data: palettes, tiles, rooms, sprites, anims."""
import os, sys, struct
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
import fbextract as fb
from PIL import Image

DATA = os.path.join(os.path.dirname(__file__), "..", "assets", "DATA")
OUT = os.path.join(os.path.dirname(__file__), "..", "work", "explore")
os.makedirs(OUT, exist_ok=True)

arch = fb.read_aba(os.path.join(DATA, "DEMO_UK.ABA"))
sgd = open(os.path.join(DATA, "LEVEL1.SGD"), "rb").read()
lev = open(os.path.join(DATA, "LEVEL1.LEV"), "rb").read()
perso = open(os.path.join(DATA, "PERSO.SPR"), "rb").read()

# ---- palette
pal = arch["LEVEL1.PAL"]
print("PAL len", len(pal), "first 16 bytes:", pal[:16].hex())

def pal_rgb(p, n=16, stride=3):
    return [tuple(p[i*stride:i*stride+3]) for i in range(n)]

print("pal entries (RGB):", pal_rgb(pal, 16))

# ---- mbk
mbk = arch["LEVEL1.MBK"]
ents = fb.load_mbk(mbk)
print("MBK entries:", len(ents), "first 8:", ents[:8])

# ---- rooms
rooms = fb.decode_lev(lev)
print("LEV rooms:", len(rooms))
for i, r in enumerate(rooms[:6]):
    print("  room", i, "flag", r["flag"], "pal", [hex(x) for x in r["pal"]],
          "off10", r["off10"], "off12", r["off12"], "off14", r["off14"], "bloblen", len(r["blob"]))
