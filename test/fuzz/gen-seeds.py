#!/usr/bin/env python3
"""Write the seed corpus of fuzz-sixel into sixel-seeds/.

Each seed is fuzz-sixel's input: three header bytes (P1, P2, flags) and a
payload. The seeds are the libsixel fixtures and the edge cases of
test_sixel.c, so fuzzing starts from every construct the tests know.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "..", "sixel")
OUT = os.path.join(HERE, "sixel-seeds")

# P1 byte: value + 1 (0 is omitted); P2 byte: value + 1; flags: limits in
# bits 0-1, chunk size in bits 2-4, two registers in bit 6, shared in bit 7
CASES = {
    "bits": (8, 2, 0, b"#1~#1A#1B#1_#1?@"),
    "repeat": (8, 2, 0, b"#2!5@!0@!@#2!3#3@#2!3@@"),
    "cr-nl": (8, 2, 0, b"#1~~$#2@-#2!2@--@-"),
    "colours": (8, 2, 0, b"#5;2;100;0;0@#6;1;120;50;100@#7;2;250;0;900@"
                b"#2;2;100;0;0;55;66@#1@#1;2;0;100;0"),
    "background": (8, 1, 0, b"#1A#0;2;0;0;100\x1b#1A"),
    "aspect": (1, 2, 0, b"\"1;2#1!3~\x1b\"100;1#1~\x1b#1~\"3;1;50;50~"),
    "raster": (1, 1, 0, b"\"1;1;10;20#1~\x1b\"1;1;2;2\x1b\"1;1;1;5#1N"),
    "max-size": (8, 2, 1, b"#1!10~-~-~\x1b\"1;1;100;100#1@\x1b#1--~"),
    "overflow": (8, 2, 0x40, b"#20;2;100;0;0@#99999999999999999999@"
                 b"!99999999999999999999999~\"1;5#1!4294967295~"),
    "huge-raster": (8, 1, 0, b"\"1;1;99999999;99999999#1~\x1b\"1;1;10000;10000"),
    "abort": (8, 2, 0x80, b"#1!50~-!50~\x18#3;2;100;0;0@\x1b#3@"),
    "growth": (8, 2, 0x04, b"#1" + b"".join(b"!%d?@-" % (i * 7) for i in range(1, 40))),
    "ignored": (8, 2, 0, b"#1@ \r\n\t\x01\x7f\x80\xff%&'()*+,./:;<=>@#1 @!3 @"),
}


def payload(name):
    with open(os.path.join(FIXTURES, name), "rb") as f:
        data = f.read()
    return data[data.index(b"q") + 1:data.rindex(b"\x1b")]


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, (p1, p2, flags, data) in CASES.items():
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(bytes([p1, p2, flags]) + data)
    for name in ("palette16", "gradient", "highcolor"):
        with open(os.path.join(OUT, "libsixel-" + name), "wb") as f:
            f.write(bytes([0, 0, 0x14]) + payload(name + ".six"))


if __name__ == "__main__":
    main()
