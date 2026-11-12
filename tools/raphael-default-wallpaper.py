#!/usr/bin/env python3
"""Write a plain original gradient wallpaper PNG (default 1125x2499) for the overlay.

    tools/raphael-default-wallpaper.py OUT.png [W H]

Deep blue at the top to violet at the bottom with a soft diagonal glow; no external assets.
"""
import math
import struct
import sys
import zlib


def png(path, w, h, rows):
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    raw = b''.join(b'\x00' + r for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))


def main():
    out = sys.argv[1]
    w = int(sys.argv[2]) if len(sys.argv) > 3 else 1125
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 2499
    top, bottom = (18, 32, 92), (112, 48, 140)
    rows = []
    for y in range(h):
        t = y / (h - 1)
        base = [top[i] + (bottom[i] - top[i]) * t for i in range(3)]
        row = bytearray()
        for x in range(0, w):
            d = (x / w * 0.6 + (1 - t) * 0.4)
            glow = 40 * math.exp(-((d - 0.55) ** 2) / 0.02)
            row += bytes((min(255, int(base[0] + glow * 0.6)), min(255, int(base[1] + glow * 0.9)),
                          min(255, int(base[2] + glow))))
        rows.append(bytes(row))
    png(out, w, h, rows)
    print(f'wallpaper {w}x{h} -> {out}')


if __name__ == '__main__':
    main()
