"""The Orb OS — the one kind of PNG the Orb reads, read and written in plain Python.

The firmware decodes theme pictures with PNGdec and keeps a pixel only when the file is 8-bit
RGBA (colour type 6, bit depth 8); any other kind comes out black, with nothing said. So this
reads exactly that kind and reports anything else, and writes it. No dependencies: zlib and
struct are enough for one colour type.
"""
import struct
import zlib

SIGNATURE = b"\x89PNG\r\n\x1a\n"
COLOUR_TYPES = {0: "greyscale", 2: "RGB", 3: "palette", 4: "greyscale+alpha", 6: "RGBA"}


class NotOrbPng(ValueError):
    """A file the Orb would not draw: not a PNG, or not 8-bit RGBA."""


def header(data):
    """(width, height, bit depth, colour type, interlace) without decoding the pixels."""
    if data[:8] != SIGNATURE:
        raise NotOrbPng("not a PNG file")
    length, kind = struct.unpack(">I4s", data[8:16])
    if kind != b"IHDR":
        raise NotOrbPng("no IHDR chunk")
    w, h, depth, ctype, _comp, _filt, interlace = struct.unpack(">IIBBBBB", data[16:29])
    return w, h, depth, ctype, interlace


def check(data):
    """Raise NotOrbPng unless the Orb can draw this file."""
    w, h, depth, ctype, interlace = header(data)
    if ctype != 6 or depth != 8:
        raise NotOrbPng("%s at %d bits; the Orb draws only 8-bit RGBA and shows this one black"
                        % (COLOUR_TYPES.get(ctype, "colour type %d" % ctype), depth))
    if interlace:
        raise NotOrbPng("interlaced; save it without interlacing")
    return w, h


def read_rgba(path):
    """(width, height, bytes) with 4 bytes a pixel, row by row."""
    data = open(path, "rb").read()
    w, h = check(data)
    pos, idat = 8, []
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if kind == b"IDAT":
            idat.append(data[pos + 8:pos + 8 + length])
        pos += 12 + length
    raw = zlib.decompress(b"".join(idat))
    stride = w * 4
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                left = line[i - 4] if i >= 4 else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        elif f != 0:
            raise NotOrbPng("unknown row filter %d" % f)
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bytes(out)


def write_rgba(path, w, h, rgba):
    """Write 8-bit RGBA, every row unfiltered: plain, and the zlib pass does the work."""
    stride = w * 4
    raw = b"".join(b"\x00" + bytes(rgba[y * stride:(y + 1) * stride]) for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(SIGNATURE)
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))
