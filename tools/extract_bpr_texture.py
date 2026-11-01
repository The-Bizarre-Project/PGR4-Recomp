"""Extract a texture from a PGR4 .bpr package (Bizarre Creations BZR2) as PNG.

Usage: python tools/extract_bpr_texture.py <package.bpr> <texture name> <out.png>
       python tools/extract_bpr_texture.py <package.bpr> --list

Package layout (big-endian): "BZR2" header, a table of 28-byte texture
records, the names (NUL separated, same order), then the zlib-compressed
texture data. Record: u16 record size, u16 name offset, u32 data offset,
u32 ?, u32 data size, u32 ?, u16 format, u16 mip count, u16 width,
u16 height. Formats seen: 1 DXT1, 5 DXT5, 6 A8R8G8B8. Data is Xbox 360
tiled with 16-bit words byte-swapped.
"""
import re
import struct
import sys
import zlib


def read_package(path):
    d = open(path, "rb").read()
    if d[:4] != b"BZR2":
        raise ValueError("not a BZR2 package")
    z = d.find(b"\x78\x9c")
    first = re.search(rb"[A-Za-z_][A-Za-z0-9_.-]{2,}\x00", d[0x14:z]).start() + 0x14
    names = d[first:z].split(b"\0")[:-1]
    textures = {}
    for i, name in enumerate(names):
        r = d[0x14 + i * 28: 0x14 + i * 28 + 28]
        _, _, offset, _, size, _, fmt, mips, w, h = struct.unpack(">HHIIIIHHHH", r)
        textures[name.decode()] = (offset, size, fmt, mips, w, h)
    return textures, zlib.decompress(d[z:])


def tiled_offset(x, y, width, log_bpp):
    """XGAddress2DTiledOffset: block (x, y) -> block index in tiled data."""
    aligned = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log_bpp
    offset = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4)
    return (((offset & ~511) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log_bpp


def rgb565(c):
    return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def decode_color_block(b, out, stride, px, py, dxt1):
    c0, c1, bits = struct.unpack("<HHI", b)
    p0, p1 = rgb565(c0), rgb565(c1)
    if c0 > c1 or not dxt1:
        pal = [p0 + (255,), p1 + (255,), tuple((2 * a + c) // 3 for a, c in zip(p0, p1)) + (255,),
               tuple((a + 2 * c) // 3 for a, c in zip(p0, p1)) + (255,)]
    else:
        pal = [p0 + (255,), p1 + (255,), tuple((a + c) // 2 for a, c in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
    for i in range(16):
        x, y = px + (i & 3), py + (i >> 2)
        o = (y * stride + x) * 4
        out[o:o + 4] = bytes(pal[(bits >> (2 * i)) & 3])


def decode_alpha_block(b, out, stride, px, py):
    a0, a1 = b[0], b[1]
    bits = int.from_bytes(b[2:8], "little")
    if a0 > a1:
        pal = [a0, a1] + [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
    else:
        pal = [a0, a1] + [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255]
    for i in range(16):
        x, y = px + (i & 3), py + (i >> 2)
        out[(y * stride + x) * 4 + 3] = pal[(bits >> (3 * i)) & 7]


def decode(data, fmt, w, h):
    data = bytearray(data)
    data[0::2], data[1::2] = data[1::2], data[0::2]  # 16-bit endian swap
    out = bytearray(w * h * 4)
    if fmt in (1, 5):
        block = 8 if fmt == 1 else 16
        log_bpp = 3 if fmt == 1 else 4
        bw, bh = (w + 3) // 4, (h + 3) // 4
        for by in range(bh):
            for bx in range(bw):
                o = tiled_offset(bx, by, bw, log_bpp) * block
                b = bytes(data[o:o + block])
                if fmt == 5:
                    decode_color_block(b[8:16], out, w, bx * 4, by * 4, False)
                    decode_alpha_block(b[0:8], out, w, bx * 4, by * 4)
                else:
                    decode_color_block(b, out, w, bx * 4, by * 4, True)
    elif fmt == 6:
        for y in range(h):
            for x in range(w):
                o = tiled_offset(x, y, w, 2) * 4
                a, r, g, b_ = data[o + 1], data[o], data[o + 3], data[o + 2]
                out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((r, g, b_, a))
    else:
        raise ValueError(f"format {fmt} not supported")
    return bytes(out)


def write_png(path, rgba, w, h):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    textures, blob = read_package(sys.argv[1])
    if sys.argv[2] == "--list":
        for name, (off, size, fmt, mips, w, h) in textures.items():
            print(f"{name:32} {w}x{h} fmt {fmt} mips {mips} @ {off:#x} ({size} bytes)")
        return
    off, size, fmt, mips, w, h = textures[sys.argv[2]]
    write_png(sys.argv[3], decode(blob[off:off + size], fmt, w, h), w, h)
    print(f"{sys.argv[2]}: {w}x{h} fmt {fmt} -> {sys.argv[3]}")


if __name__ == "__main__":
    main()
