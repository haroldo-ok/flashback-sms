#!/usr/bin/env python3
"""
Flashback (DOS, Delphine Software) resource extractor.

Formats implemented from the REminiscence interpreter sources
(resource.cpp / resource_aba.cpp / unpack.cpp / video.cpp / piege.cpp):

  .ABA   Delphine archive: BE16 count, BE16 entrySize(=30),
         then per entry: name[14], offset u32be, compSize u32be, size u32be, tag u32be
         payloads bytekiller-compressed when compSize != size
  bytekiller  backward LZ with 32-bit CRC, read from the tail of the blob
  .MBK/.BNQ  tile bank index: 6 bytes/entry = BE32 dataOffset (DOS: &0xFFFF) + BE16 count
             count & 0x8000 -> raw (count&0x7FFF)*32 bytes, else bytekiller
  tiles      8x8, 32 bytes, one byte per row holding two 4bpp nibble pixels
  .LEV       BE32 offset per room; room blob = [flag, 4x BE16 palette slots,
             BE16 off10, BE16 off12, BE16 off14] then layer/tile-list data
  .SGD       BE32 offset per tile; negative -> raw blob at -offset,
             positive -> AMIGA-style RLE at +offset
  .SPR       'SPP' + 12 byte header, then frames; .SPR.OFF = list of
             (LE16 index, LE32 offset) terminated by 0xFFFF
  sprite     BE16 offset table per sprite bank; frame = dx i8, dy i8, w u8, h u8, pixels
  .OBJ       BE16 count, then BE32 offsets per object node
  .PGE       BE16 count, then InitPGE records
  .ANI       BE16 offsets per obj_type; record = BE16 frameCount, BE16 snd,
             BE16 unk, then frameCount x (BE16 sprite|flip, dx i8, dy i8)
"""
import os
import struct
import sys

# ---------------------------------------------------------------- bytekiller


class Bytekiller:
    def __init__(self, src):
        self.size = int.from_bytes(src[-4:], "big")
        self.src = len(src) - 8
        self.dst = bytearray(self.size)
        self.dstpos = self.size - 1
        self.crc = int.from_bytes(src[self.src : self.src + 4], "big")
        self.src -= 4
        self.bits = int.from_bytes(src[self.src : self.src + 4], "big")
        self.src -= 4
        self.crc ^= self.bits
        self.srcb = src

    def next_bit(self):
        bit = self.bits & 1
        self.bits >>= 1
        if self.bits == 0:
            self.bits = int.from_bytes(self.srcb[self.src : self.src + 4], "big")
            self.src -= 4
            self.crc ^= self.bits
            bit = self.bits & 1
            self.bits = (1 << 31) | (self.bits >> 1)
        return bit

    def get_bits(self, count):
        v = 0
        for _ in range(count):
            v = (v << 1) | self.next_bit()
        return v

    def copy_bytes(self, ln, offset):
        self.size -= ln
        if self.size < 0:
            ln += self.size
            self.size = 0
        if offset != 0:
            for _ in range(ln):
                self.dst[self.dstpos] = self.dst[self.dstpos + offset]
                self.dstpos -= 1
        else:
            for _ in range(ln):
                self.dst[self.dstpos] = self.get_bits(8)
                self.dstpos -= 1

    def run(self):
        while self.size > 0:
            code = self.get_bits(2)
            if code == 0:
                self.copy_bytes(self.get_bits(3) + 1, 0)
            elif code == 1:
                self.copy_bytes(2, self.get_bits(8))
            else:
                code2 = ((code & 1) << 1) | self.next_bit()
                if code2 == 3:
                    self.copy_bytes(self.get_bits(8) + 9, 0)
                elif code2 == 2:
                    ln = self.get_bits(8) + 1
                    self.copy_bytes(ln, self.get_bits(12))
                else:
                    self.copy_bytes(code2 + 3, self.get_bits(code2 + 9))
        return self.crc == 0


def bk_unpack(data):
    b = Bytekiller(data)
    ok = b.run()
    return bytes(b.dst), ok


# ---------------------------------------------------------------- .ABA


def read_aba(path):
    out = {}
    with open(path, "rb") as f:
        blob = f.read()
    count = int.from_bytes(blob[0:2], "big")
    entry_size = int.from_bytes(blob[2:4], "big")
    assert entry_size == 30, entry_size
    pos = 4
    for _ in range(count):
        name = blob[pos : pos + 14].split(b"\0")[0].decode("ascii", "replace")
        offset, csize, size, tag = struct.unpack(">4I", blob[pos + 14 : pos + 30])
        assert tag == 0x442E4D2E, hex(tag)
        payload = blob[offset : offset + csize]
        if csize == size:
            out[name] = payload
        else:
            dec, ok = bk_unpack(payload)
            assert ok, "bad CRC for %s" % name
            out[name] = dec
        pos += 30
    return out


# ---------------------------------------------------------------- tiles / MBK


def tile_to_pixels(tile):
    """32-byte 4bpp tile -> list of 8 rows of 8 palette indices."""
    rows = []
    for y in range(8):
        row = []
        for b in tile[y * 8 : y * 8 + 8]:
            row.append(b >> 4)
            row.append(b & 15)
        rows.append(row)
    return rows


def load_mbk(data, signed_demo=False):
    """MBK/BNQ index -> list of (offset, count, raw_flag)."""
    entries = []
    n = len(data) // 6
    for i in range(n):
        off = int.from_bytes(data[i * 6 : i * 6 + 4], "big") & 0xFFFF
        cnt = int.from_bytes(data[i * 6 + 4 : i * 6 + 6], "big")
        raw = bool(cnt & 0x8000)
        if cnt & 0x8000:
            cnt = (-struct.unpack(">h", data[i * 6 + 4 : i * 6 + 6])[0]) if signed_demo else (cnt & 0x7FFF)
        entries.append((off, cnt, raw))
    return entries


def bank_tiles(data, idx, signed_demo=False):
    """Return the 32-byte tiles of bank `idx`."""
    ent = load_mbk(data, signed_demo)[idx]
    off, cnt, raw = ent
    size = cnt * 32
    if raw:
        return data[off : off + size]
    dec, ok = bk_unpack(data[off:])
    assert ok, "bad CRC bank %d" % idx
    return dec[:size]


# ---------------------------------------------------------------- SGD


def rle_decode(src):
    """AMIGA_decodeRle: BE16 (size&0x7FFF) then literal/run codes."""
    size = int.from_bytes(src[0:2], "big") & 0x7FFF
    i = 2
    out = bytearray()
    while len(out) < size and i < len(src):
        code = src[i]
        i += 1
        if code & 0x80 == 0:
            n = code + 1
            if i + n > size:
                n = size - len(out)
            out += src[i : i + n]
            i += n
        else:
            n = 1 - struct.unpack("b", bytes([code]))[0]
            out += bytes([src[i]] * n)
            i += 1
    return bytes(out[:size])


def sgd_tiles(sgd):
    """SGD -> dict tile_index -> bytes (the planar DOS 'tilemask' blobs)."""
    n = int.from_bytes(sgd[0:4], "big")
    tiles = {}
    cache = {}
    for i in range(n):
        off = int.from_bytes(sgd[i * 4 : i * 4 + 4], "big")
        off = struct.unpack(">i", sgd[i * 4 : i * 4 + 4])[0]
        if off < 0:
            ptr = -off
            sz = int.from_bytes(sgd[ptr : ptr + 2], "big")
            tiles[i] = sgd[ptr : ptr + 2 + sz]
        else:
            if off not in cache:
                cache[off] = rle_decode(sgd[off:])
            blob = cache[off]
            sz = int.from_bytes(blob[0:2], "big") & 0x7FFF
            tiles[i] = blob[: 2 + sz]
    return tiles


def sgd_tile_pixels(blob):
    """DOS_drawTileMask blob -> (w_px, h, rows of palette indices), 4bpp packed."""
    w = (blob[0] + 1)
    h = blob[1] + 1
    planar_size = int.from_bytes(blob[2:4], "big")
    mask = blob[4 : 4 + planar_size]
    pix = blob[4 + planar_size : 4 + 2 * planar_size]
    rows = []
    p = 0
    for y in range(h):
        row = []
        for x in range(w):
            row.append(pix[p] >> 4)
            row.append(pix[p] & 15)
            p += 1
        rows.append(row)
    return w * 2, h, rows


# ---------------------------------------------------------------- SPR


def load_spr(spr, off_data):
    """Return dict frame_index -> bytes of the frame."""
    assert spr[:3] == b"SPP", spr[:3]
    body = spr[12:]
    frames = {}
    p = 0
    while True:
        if p + 6 > len(off_data):
            break
        idx = int.from_bytes(off_data[p : p + 2], "little")
        if idx == 0xFFFF:
            break
        off = int.from_bytes(off_data[p + 2 : p + 6], "little")
        if off != 0xFFFFFFFF:
            frames[idx] = body[off:]
        p += 6
    return frames


def sprite_frame_pixels(frame):
    """DOS sprite frame -> (dx, dy, w, h, rows).  w is bytes (2 px per byte)."""
    dx = struct.unpack("b", frame[0:1])[0]
    dy = struct.unpack("b", frame[1:2])[0]
    w = frame[2]
    h = frame[3]
    data = frame[4:]
    compressed = not (frame[-2] & 0x80)
    return dx, dy, w, h, data, compressed


def spm_decode(frame):
    """DOS_decodeSpm: the frame body after the 4-byte header is RLE-ish packed.
    Returns the expanded 4bpp pixel buffer (one index per byte)."""
    data = frame[4:]
    ln = 2 * int.from_bytes(data[0:2], "big")
    nib = bytearray()
    for b in data[2 : 2 + ln]:
        nib.append(b >> 4)
        nib.append(b & 15)
    src = nib
    i = 0
    out = bytearray()
    end = ln
    while i < end:
        code = src[i]
        i += 1
        if code == 0xF:
            color = src[i]
            i += 1
            count = src[i]
            i += 1
            if color == 0xF:
                count = (count << 4) | src[i]
                i += 1
                color = src[i]
                i += 1
            count += 4
            out += bytes([color] * count)
        else:
            out.append(code)
    return bytes(out)


# ---------------------------------------------------------------- OBJ / PGE / ANI


def decode_obj(data):
    """OBJ -> list (per node index) of list of Object dicts."""
    num_nodes = int.from_bytes(data[0:2], "big")
    body = data[2:]
    offs = []
    for i in range(num_nodes):
        offs.append(int.from_bytes(body[i * 4 : i * 4 + 4], "big"))
    offs.append(len(body))
    nodes = []
    seen = {}
    for i in range(num_nodes):
        if offs[i] in seen:
            nodes.append(seen[offs[i]])
            continue
        p = offs[i]
        cnt = int.from_bytes(body[p : p + 2], "big")
        p += 2
        objs = []
        for j in range(cnt):
            o = {}
            o["type"] = int.from_bytes(body[p : p + 2], "big")
            o["dx"] = struct.unpack("b", body[p + 2 : p + 3])[0]
            o["dy"] = struct.unpack("b", body[p + 3 : p + 4])[0]
            o["init_obj_type"] = int.from_bytes(body[p + 4 : p + 6], "big")
            o["opcode2"] = body[p + 6]
            o["opcode1"] = body[p + 7]
            o["flags"] = body[p + 8]
            o["opcode3"] = body[p + 9]
            o["init_obj_number"] = int.from_bytes(body[p + 10 : p + 12], "big")
            o["opcode_arg1"] = struct.unpack(">h", body[p + 12 : p + 14])[0]
            o["opcode_arg2"] = struct.unpack(">h", body[p + 14 : p + 16])[0]
            o["opcode_arg3"] = struct.unpack(">h", body[p + 16 : p + 18])[0]
            objs.append(o)
            p += 18
        nodes.append(objs)
        seen[offs[i]] = objs
    return nodes


def decode_pge(data):
    num = int.from_bytes(data[0:2], "big")
    p = 2
    out = []
    for _ in range(num):
        e = {}
        e["type"] = int.from_bytes(data[p : p + 2], "big")
        e["pos_x"] = int.from_bytes(data[p + 2 : p + 4], "big")
        e["pos_y"] = int.from_bytes(data[p + 4 : p + 6], "big")
        e["obj_node_number"] = int.from_bytes(data[p + 6 : p + 8], "big")
        e["life"] = int.from_bytes(data[p + 8 : p + 10], "big")
        e["data"] = [int.from_bytes(data[p + 10 + i * 2 : p + 12 + i * 2], "big") for i in range(4)]
        p += 18
        e["object_type"] = data[p]
        e["init_room"] = data[p + 1]
        e["room_location"] = data[p + 2]
        e["init_flags"] = data[p + 3]
        e["colliding_icon_num"] = data[p + 4]
        e["icon_num"] = data[p + 5]
        e["object_id"] = data[p + 6]
        e["skill"] = data[p + 7]
        e["mirror_x"] = data[p + 8]
        e["flags"] = data[p + 9]
        e["collision_data_len"] = data[p + 10]
        p += 12
        e["text_num"] = int.from_bytes(data[p : p + 2], "big")
        p += 2
        out.append(e)
    return out


def decode_ani(data):
    """ANI -> dict obj_type -> {'frames':n,'snd':s,'unk':u,'seq':[(spr,dx,dy)]}"""
    out = {}
    n = len(data)
    # table is a list of BE16 offsets; entries repeat for consecutive identical types
    offs = []
    p = 2
    while p + 2 <= n:
        offs.append(int.from_bytes(data[p : p + 2], "big"))
        p += 2
    seen = {}
    for i, off in enumerate(offs):
        if off in seen:
            out[i] = seen[off]
            continue
        base = 2 + off
        if base + 6 > n:
            continue
        cnt = int.from_bytes(data[base : base + 2], "big")
        snd = int.from_bytes(data[base + 2 : base + 4], "big")
        unk = int.from_bytes(data[base + 4 : base + 6], "big")
        seq = []
        q = base + 6
        for _ in range(cnt):
            spr = int.from_bytes(data[q : q + 2], "big")
            dx = struct.unpack("b", data[q + 2 : q + 3])[0]
            dy = struct.unpack("b", data[q + 3 : q + 4])[0]
            seq.append((spr, dx, dy))
            q += 4
        rec = {"frames": cnt, "snd": snd, "unk": unk, "seq": seq}
        out[i] = rec
        seen[off] = rec
    return out


# ---------------------------------------------------------------- LEV


def decode_lev(lev):
    """LEV -> list of per-room dicts."""
    n = len(lev) // 4
    rooms = []
    for r in range(n):
        off = int.from_bytes(lev[r * 4 : r * 4 + 4], "big")
        blob = lev[off:]
        rec = {"flag": blob[0]}
        rec["pal"] = [int.from_bytes(blob[2 + i * 2 : 4 + i * 2], "big") for i in range(4)]
        rec["off10"] = int.from_bytes(blob[10:12], "big")
        rec["off12"] = int.from_bytes(blob[12:14], "big")
        rec["off14"] = int.from_bytes(blob[14:16], "big")
        rec["blob"] = blob
        rooms.append(rec)
    return rooms


def room_tile_list(blob, off14):
    """Return the ordered list of 32-byte tiles for the room's MBK bank set."""
    tiles = [b"\0" * 32]
    p = off14
    banks = []
    while True:
        d0 = int.from_bytes(blob[p : p + 2], "big")
        p += 2
        num = d0 & ~0x8000
        banks.append(num)
        cnt = blob[p]
        p += 1
        if cnt == 255:
            banks[-1] = (num, "all")
        else:
            idx = []
            for _ in range(cnt + 1):
                idx.append(blob[p])
                p += 1
            banks[-1] = (num, idx)
        if d0 & 0x8000:
            break
    return banks, p


def layer_cells(blob, off, w=32, h=28):
    """Return list of (tile, xflip, yflip, mask_bits) row-major."""
    cells = []
    p = off
    for _ in range(w * h):
        d3 = int.from_bytes(blob[p : p + 2], "big")
        p += 2
        cells.append((d3 & 0x7FF, bool(d3 & (1 << 11)), bool(d3 & (1 << 12)), d3 & 0xF000))
    return cells
