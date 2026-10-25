#!/usr/bin/env python3
"""Write the seed corpora of fuzz-sixel into sixel-seeds/, of fuzz-vte-sixel
into vte-sixel-seeds/ and of fuzz-vte-kitty into vte-kitty-seeds/.

Each fuzz-sixel seed is its input: three header bytes (P1, P2, flags) and a
payload. The seeds are the libsixel fixtures and the edge cases of
test_sixel.c, so fuzzing starts from every construct the tests know.

Each fuzz-vte-sixel seed is two header bytes (screen and cell size; limits,
registers and feeding) and a terminal stream: the six-* and img-* streams of
the conformance corpus, so fuzzing starts from every sixel case tier 1
judges, and a stream of sixel frames in mode 2026, fed whole and in bounded
chunks, with the mode offered (the host ending holds in one) and not. Each
fuzz-vte-kitty seed is the same two header bytes and a kit-*
stream, every kitty graphics case tier 1 judges.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "..", "sixel")
OUT = os.path.join(HERE, "sixel-seeds")
VTE_OUT = os.path.join(HERE, "vte-sixel-seeds")
KITTY_OUT = os.path.join(HERE, "vte-kitty-seeds")
CASES_DIR = os.path.join(HERE, "..", "conformance", "cases")
# header bytes for the streams: 12x4 screen of 8x16 cells, defaults, fed
# whole; and the same fed in 7-byte bounded chunks with shared registers
VTE_HEADERS = (bytes([0x00, 0x30]), bytes([0x10, 0x18]))
# mode 2026 offered (bit 6): fed whole; in 7-byte bounded chunks with the
# host ending a hold every third call (bit 7); in 4096-byte bounded chunks,
# where one call could reach past a frame end; and in those chunks with the
# mode not offered
SYNC_HEADERS = (bytes([0x00, 0x70]), bytes([0x10, 0xd8]),
                bytes([0x00, 0x68]), bytes([0x00, 0x20]))
SYNC_STREAM = (b"\x1b[?2026h\x1bP0;0q#1!8~-!8~\x1b\\\x1b[?2026l"
               b"\x1b[?2026h\x1b[?2026h\x1bP0;0q#2!4@\x1b\\x\x1b[?2026l\x1b[?2026l"
               b"\x1b[?2026h\x1bP0;0q#1~\x1b\\\x1bc"
               b"\x1b[?2026hy\x1b[!p\x1b[?2026h\x1b[?1049hz\x1b[?1049l"
               b"\x1b[?2026$p\x1b[?2026;25l\x1b[?2026h\x1bP0;0q#1!3~")

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

    os.makedirs(VTE_OUT, exist_ok=True)
    # a piece still owed when its picture, in scrollback, is evicted, under
    # the 1188-byte budget (review of #36)
    with open(os.path.join(VTE_OUT, "owed-evicted"), "wb") as f:
        f.write(bytes([0x30, 0x30]) + b"\x1b[1;1H\x1bP0;0q!24~\x1b\\"
                b"\x1b[1;2HX\x1b[4;1H\n\n\n\n\n")
    for i, header in enumerate(SYNC_HEADERS):
        with open(os.path.join(VTE_OUT, f"sync-frames-{i}"), "wb") as f:
            f.write(header + SYNC_STREAM)
    for case in sorted(os.listdir(CASES_DIR)):
        if not case.startswith(("six-", "img-")) or not case.endswith(".bin"):
            continue
        with open(os.path.join(CASES_DIR, case), "rb") as f:
            stream = f.read()
        for i, header in enumerate(VTE_HEADERS):
            name = f"{case[:-4]}-{i}"
            with open(os.path.join(VTE_OUT, name), "wb") as f:
                f.write(header + stream)

    os.makedirs(KITTY_OUT, exist_ok=True)
    for case in sorted(os.listdir(CASES_DIR)):
        if not case.startswith(("kit-", "apc-")) or not case.endswith(".bin"):
            continue
        with open(os.path.join(CASES_DIR, case), "rb") as f:
            stream = f.read()
        for i, header in enumerate(VTE_HEADERS):
            name = f"{case[:-4]}-{i}"
            with open(os.path.join(KITTY_OUT, name), "wb") as f:
                f.write(header + stream)


if __name__ == "__main__":
    main()
