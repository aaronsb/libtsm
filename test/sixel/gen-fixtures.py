#!/usr/bin/env python3
"""Write the source pictures of the libsixel fixtures as binary PPMs.

Each picture is a pure function of its coordinates, so the files are
reproducible. gen-fixtures.sh encodes them with libsixel.
"""
import os
import sys


def ppm(path, w, h, pixel):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        for y in range(h):
            for x in range(w):
                f.write(bytes(pixel(x, y)))


# 16 flat colours in 5x4 blocks; odd width, height not a multiple of six
PALETTE = [
    (0, 0, 0), (255, 255, 255), (255, 0, 0), (0, 255, 0),
    (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255),
    (128, 128, 128), (64, 32, 16), (16, 64, 128), (200, 100, 50),
    (30, 200, 120), (120, 30, 200), (250, 180, 200), (90, 90, 30),
]


def palette16(x, y):
    return PALETTE[(x // 5 + 3 * (y // 4)) % 16]


def gradient(x, y, w=64, h=50):
    return (x * 255 // (w - 1), y * 255 // (h - 1), (x + y) * 255 // (w + h - 2))


def main(out):
    ppm(os.path.join(out, "palette16.ppm"), 37, 23, palette16)
    ppm(os.path.join(out, "gradient.ppm"), 64, 50, gradient)
    ppm(os.path.join(out, "highcolor.ppm"), 48, 30,
        lambda x, y: gradient(x, y, 48, 30))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(__file__) or ".")
