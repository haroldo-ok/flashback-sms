"""Readers/writers for fbdump's capture formats (see tools/fbdump/fbdump.cpp)."""
import numpy as np

W, H = 256, 224
FRAME_REC = 1 + 4 + 768 + W * H


def read_fbv(path):
    """-> list of (time_ms, pal uint8[256,3], pix uint8[224,256])"""
    data = open(path, 'rb').read()
    assert data[:4] == b'FBV1', path
    out, p = [], 4
    while p + FRAME_REC <= len(data):
        assert data[p] == ord('F')
        t = int.from_bytes(data[p + 1:p + 5], 'little')
        pal = np.frombuffer(data, np.uint8, 768, p + 5).reshape(256, 3)
        pix = np.frombuffer(data, np.uint8, W * H, p + 773).reshape(H, W)
        out.append((t, pal, pix))
        p += FRAME_REC
    return out


def write_fbv(path, frames):
    with open(path, 'wb') as f:
        f.write(b'FBV1')
        for t, pal, pix in frames:
            f.write(b'F' + int(t).to_bytes(4, 'little'))
            f.write(np.asarray(pal, np.uint8).tobytes())
            f.write(np.asarray(pix, np.uint8).tobytes())


ROOM_REC = 1 + 1 + 4 + 768 + W * H


def read_fbr(path):
    """-> list of dict(room, slots, pal[256,3], pix[224,256])"""
    data = open(path, 'rb').read()
    assert data[:4] == b'FBR1', path
    out, p = [], 4
    while p + ROOM_REC <= len(data):
        assert data[p] == ord('R')
        room = data[p + 1]
        slots = tuple(data[p + 2:p + 6])
        pal = np.frombuffer(data, np.uint8, 768, p + 6).reshape(256, 3)
        pix = np.frombuffer(data, np.uint8, W * H, p + 774).reshape(H, W)
        out.append(dict(room=room, slots=slots, pal=pal, pix=pix))
        p += ROOM_REC
    return out


def write_fbr(path, rooms):
    with open(path, 'wb') as f:
        f.write(b'FBR1')
        for r in rooms:
            f.write(b'R' + bytes([r['room']]) + bytes(r['slots']))
            f.write(np.asarray(r['pal'], np.uint8).tobytes())
            f.write(np.asarray(r['pix'], np.uint8).tobytes())


def to_sms_rgb(pal8):
    """8-bit RGB (from Amiga 4-bit x 0x11) -> SMS 2-bit levels 0..3."""
    return ((pal8.astype(np.int32) * 3 + 127) // 255).astype(np.uint8)


def sms_color_byte(rgb2):
    r, g, b = (int(x) for x in rgb2)
    return r | (g << 2) | (b << 4)
