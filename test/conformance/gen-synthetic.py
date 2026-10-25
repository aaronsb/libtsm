#!/usr/bin/env python3
"""Write the synthetic byte streams of the conformance corpus.

Each case exercises one area of terminal behaviour with hand-written escape
sequences. The streams are committed; rerun this script only to add or change
a case, and review the resulting goldens as their own change.

Usage: gen-synthetic.py CASES_DIR
Writes CASES_DIR/<name>.bin, and the fixtures tsm-replay's host serves a
case into CASES_DIR/<name>.files/, and prints one "<name> <cols> <rows>"
line per case, which belongs in cases.list.

SPDX-License-Identifier: MIT
"""

import base64
import sys
from pathlib import Path

ESC = "\x1b"
CSI = ESC + "["
OSC = ESC + "]"
DCS = ESC + "P"
APC = ESC + "_"
ST = ESC + "\\"


def sgr(*params):
    return CSI + ";".join(str(p) for p in params) + "m"


def cup(row, col):
    return f"{CSI}{row};{col}H"


CASES = {}
# fixtures for tsm-replay's host, by case: {file name: bytes}
FILES = {}


def case(name, cols=80, rows=24):
    def register(fn):
        CASES[name] = (cols, rows, fn)
        return fn
    return register


@case("sgr-basic")
def _():
    out = "plain "
    for p, label in [(1, "bold"), (2, "dim"), (3, "italic"), (4, "underline"),
                     (5, "blink"), (7, "inverse"), (8, "conceal"), (9, "strike")]:
        out += sgr(p) + label + sgr(0) + " "
    out += "\r\n"
    for c in range(30, 38):
        out += sgr(c) + "fg" + str(c) + sgr(0) + " "
    out += "\r\n"
    for c in range(40, 48):
        out += sgr(c) + "bg" + str(c) + sgr(0) + " "
    out += "\r\n"
    for c in range(90, 98):
        out += sgr(c) + "hi" + str(c) + sgr(0) + " "
    out += "\r\n" + sgr(1, 4, 31, 42) + "combined" + sgr(22) + "nobold" + sgr(24) + "noul" + sgr(39, 49) + "default" + sgr(0)
    return out


@case("sgr-extended-color")
def _():
    out = ""
    for n in (16, 88, 196, 231, 232, 255):
        out += sgr(38, 5, n) + f"f{n}" + sgr(48, 5, n) + f"b{n}" + sgr(0) + " "
    out += "\r\n" + sgr(38, 2, 255, 128, 0) + "orange" + sgr(48, 2, 0, 0, 128) + "onnavy" + sgr(0)
    return out


@case("sgr-underline-extended")
def _():
    # Colon sub-parameters: styled and coloured underlines (SGR 4:x, 58, 59).
    return (CSI + "4:3m" + "curly" + sgr(0) + " " +
            CSI + "4:2m" + "double" + sgr(0) + " " +
            CSI + "1;4:3m" + "boldcurly" + sgr(0) + " " +
            CSI + "4;58:2::255:0:0m" + "redul" + CSI + "59m" + "nocolor" + sgr(0) + " " +
            CSI + "4;58;5;196m" + "ul196" + sgr(0) + " " +
            CSI + "4:0m" + "ulnone" + sgr(0))


def color_forms(selector, prefix=""):
    # Each colour form SGR 38, 48 and 58 accept, one labelled run each, with a
    # distinct colour per form so a form read wrong shows as a wrong value.
    forms = [
        ("c5", ":5:196"),                    # colon palette index, 16-255: resolved to RGB
        ("c5lo", ":5:9"),                    # colon palette index below 16: stays a code
        ("s5", ";5;33"),                     # semicolon palette index
        ("c2", ":2:10:20:30"),               # colon RGB without a colour-space slot
        ("c2e", ":2::40:50:60"),             # colon RGB, colour space empty (T.416)
        ("c2cs", ":2:0:70:80:90"),           # colon RGB with a colour space
        ("c2t", ":2:0:100:110:120:7:8:9"),   # trailing T.416 fields ignored
        ("s2", ";2;130;140;150;1"),          # semicolon RGB takes exactly 3; the 1 is bold
    ]
    return "".join(CSI + prefix + str(selector) + form + "m" + label + sgr(0) + " "
                   for label, form in forms)


@case("sgr-color-forms-fg")
def _():
    return color_forms(38)


@case("sgr-color-forms-bg")
def _():
    return color_forms(48)


@case("sgr-color-forms-ul")
def _():
    # The underline colour on a single underline.
    return color_forms(58, "4;")


@case("sgr-underline-styles")
def _():
    # 4:0-4:5, an unknown style (single), plain 4, 21 double, 24 none,
    # and a style replaced by plain 4.
    out = "".join(CSI + f"4:{n}m" + f"s{n}" + sgr(0) + " " for n in (0, 1, 2, 3, 4, 5, 9))
    return (out + sgr(4) + "plain" + sgr(0) + " " + sgr(21) + "dbl21" + sgr(0) + " " +
            CSI + "4:3m" + "curly" + sgr(24) + "off24" + " " +
            CSI + "4:3m" + "curly" + sgr(4) + "single" + sgr(0))


@case("sgr-strike")
def _():
    # 9 on, 29 off, strike beside other attributes, SGR 0 clearing it.
    return (sgr(9) + "struck" + sgr(29) + "clear" + " " +
            sgr(1, 9, 31) + "boldred" + sgr(22) + "red" + sgr(0) + "reset")


@case("sgr-underline-color-reset")
def _():
    # 59 clears the colour and keeps the style; 24 clears the style and keeps
    # the colour; SGR 0 clears both.
    return (CSI + "4:3;58:5:196m" + "curlyred" + CSI + "59m" + "curly" + " " +
            CSI + "4:2;58:5:2m" + "dblgreen" + sgr(24) + "colouronly" + sgr(4) + "single" + " " +
            CSI + "4:3;58;2;1;2;3m" + "set" + sgr(0) + "reset")


@case("sgr-subparam-unknown")
def _():
    # A parameter with sub-parameters SGR does not know is skipped on its own;
    # the rest of the sequence still applies.
    return (CSI + "1;3:1;4m" + "boldul" + sgr(0) + " " +       # italic with a sub-parameter
            CSI + "4:3:1;1m" + "bold" + sgr(0) + " " +          # underline with two
            CSI + "38:9:1;1m" + "bold" + sgr(0) + " " +         # unknown colour form
            CSI + "38:5;3m" + "italic" + sgr(0) + " " +         # colon palette form missing its index
            CSI + "7:1;9m" + "strike" + sgr(0))


@case("sgr-color-colon-out-of-range")
def _():
    # In the colon forms an index or component above 255 rejects the colour
    # alone; 255 itself is accepted. (The semicolon forms keep upstream's
    # handling of large values.)
    return (CSI + "48:2:256:300:65535;1m" + "bold" + sgr(0) + " " +
            CSI + "38:5:256;3m" + "italic" + sgr(0) + " " +
            CSI + "4;58:2::0:0:256m" + "ul" + sgr(0) + " " +
            CSI + "38:2:255:255:255;48:5:255m" + "max" + sgr(0))


@case("sgr-color-semicolon-subparams")
def _():
    # A semicolon colour form whose arguments carry sub-parameters is not that
    # form: nothing is consumed, and the arguments are read as SGR on their own.
    return (CSI + "38;5:1;9m" + "strike" + sgr(0) + " " +     # 5:1 skipped, 9 strike
            CSI + "48;2;1;2;3:4m" + "dimbold" + sgr(0))       # 2 dim, 1 bold, 3:4 skipped


@case("sgr-58-semicolon-no-blink")
def _():
    # 58 consumes its own 5 and index; a later 5 is still blink.
    return (sgr(4, 58, 5, 196) + "ul196" + sgr(0) + " " +
            sgr(4, 58, 2, 1, 2, 3, 5) + "blink" + sgr(0))


@case("sgr-subparam-overflow")
def _():
    # Sub-parameters past the eighth are dropped; the parameter and the rest of
    # the sequence still apply.
    return CSI + "38:2:0:10:20:30:0:0:0:99:99;1m" + "bold" + sgr(0)


@case("csi-subparam-non-sgr")
def _():
    # Only SGR reads sub-parameters: any other sequence carrying them is
    # ignored whole, as upstream ignores every sequence with a colon.
    return cup(2, 3) + CSI + "5:1H" + "here" + CSI + "1:2C" + "next"


@case("colour-resolve")
def _():
    # Colours as a renderer resolves them, for the draw2 view: bold brightens a
    # base colour, dim dims a default underline but not a set one, reverse
    # video swaps the default underline with the foreground but not a set one.
    return (sgr(1, 31) + "BR" + sgr(0) + " " +
            sgr(2, 4) + "DU" + sgr(0) + " " +
            sgr(2, 4, 58, 2, 10, 20, 30) + "DS" + sgr(0) + " " +
            sgr(7, 4) + "RU" + sgr(0) + " " +
            sgr(7, 4, 58, 5, 1) + "RS" + sgr(0) + " " +
            sgr(38, 5, 196) + "I" + sgr(0) + " " +
            sgr(38, 2, 1, 2, 3) + "T" + sgr(0) + " " +
            sgr(1, 38, 5, 1) + "B1" + sgr(0))


@case("cursor-motion")
def _():
    return (cup(5, 10) + "A" + CSI + "2A" + "B" + CSI + "3B" + "C" + CSI + "4C" + "D" +
            CSI + "6D" + "E" + "\r" + "F" + CSI + "20G" + "G" + CSI + "15d" + "H" +
            ESC + "7" + cup(1, 1) + "I" + ESC + "8" + "J" + CSI + "s" + cup(2, 2) + "K" + CSI + "u" + "L")


@case("erase")
def _():
    fill = "".join(cup(r, 1) + "abcdefghijklmnopqrst" for r in range(1, 7))
    return (fill + cup(1, 10) + CSI + "K" + cup(2, 10) + CSI + "1K" + cup(3, 10) + CSI + "2K" +
            cup(4, 5) + CSI + "3X" + cup(5, 10) + CSI + "J")


@case("erase-display-above", 20, 6)
def _():
    fill = "".join(cup(r, 1) + "abcdefghijklmnopqrst" for r in range(1, 7))
    return fill + cup(3, 10) + CSI + "1J"


@case("insert-delete")
def _():
    fill = "".join(cup(r, 1) + f"line{r:02d}-abcdefghij" for r in range(1, 9))
    return (fill + cup(1, 5) + CSI + "3@" + cup(2, 5) + CSI + "3P" +
            cup(4, 1) + CSI + "2L" + cup(7, 1) + CSI + "1M")


@case("scroll-region", 20, 8)
def _():
    fill = "".join(cup(r, 1) + f"row{r}" for r in range(1, 9))
    return (fill + CSI + "3;6r" + cup(6, 1) + "\n\nNEW" + cup(3, 1) + ESC + "M" + "TOP" +
            CSI + "2S" + CSI + "1T" + CSI + "r" + cup(8, 1) + "\nafter-reset")


@case("origin-mode", 20, 8)
def _():
    return CSI + "3;6r" + CSI + "?6h" + cup(1, 1) + "origin" + CSI + "?6l" + cup(1, 1) + "absolute"


@case("autowrap", 10, 4)
def _():
    return ("0123456789wrapped" + "\r\n" + CSI + "?7l" + "0123456789overwrite" +
            CSI + "?7h" + "\r\n" + "x")


@case("tabs")
def _():
    return ("a\tb\tc\r\n" + CSI + "3g" + cup(2, 5) + ESC + "H" + cup(2, 13) + ESC + "H" +
            "\r" + "\tx\ty\tz")


@case("wide-chars", 12, 4)
def _():
    return "漢字テスト" + "\r\n" + "ＡＢＣ" + "\r\n" + "0123456789A漢" + "\r\n" + "ab" + "한국어"


@case("combining")
def _():
    return "é ã̀ n̈x o̲̲ ́lead"


@case("emoji")
def _():
    return ("smile \U0001F600 | vs16 ❤️ | text ❤ | " +
            "zwj \U0001F468‍\U0001F469‍\U0001F467 | flag \U0001F1FA\U0001F1F8 | " +
            "tone \U0001F44D\U0001F3FD | end")


HEART, VS15, VS16, ZWJ = "❤", "︎", "️", "‍"
FAMILY = "\U0001F468" + ZWJ + "\U0001F469" + ZWJ + "\U0001F467"
FLAG_US = "\U0001F1FA\U0001F1F8"
THUMBS_TONE = "\U0001F44D\U0001F3FD"
KEYCAP_1 = "1" + VS16 + "⃣"


@case("emoji-sequences")
def _():
    # ZWJ family, flag, keycap and skin tone: one two-cell cluster each.
    return "|".join(["", FAMILY, FLAG_US, KEYCAP_1, THUMBS_TONE, ""])


@case("vs16-widen")
def _():
    # VS16 after a listed base makes the placed cell two wide, mid-line.
    return "a" + HEART + VS16 + "b"


@case("vs16-margin", 10, 4)
def _():
    # VS16 widening the cell in the last column: the cluster moves to the
    # next line and the cell it left is cleared.
    return "012345678" + HEART + VS16 + "x"


@case("vs16-unlisted-base")
def _():
    # VS16 after a base with no emoji variation sequence joins and changes
    # nothing.
    return "A" + VS16 + "b"


@case("vs15-width")
def _():
    # VS15 never changes width: a narrow base stays one cell, a wide one two.
    return "|" + HEART + VS15 + "|⌚" + VS15 + "|"


@case("width-methods")
def _():
    # The same text under each method: mode 2027 set (unicode), then reset
    # (wcwidth), where every codepoint with width takes its own cells.
    text = "é|" + THUMBS_TONE + "|" + FLAG_US + "|" + FAMILY + "|" + HEART + VS16 + "|"
    return CSI + "?2027h" + text + "\r\n" + CSI + "?2027l" + text


@case("mode-2027-decrqm")
def _():
    # DECRQM for mode 2027: 1 while set (the default), 2 once reset, 1 again.
    q = CSI + "?2027$p"
    return q + CSI + "?2027l" + q + CSI + "?2027h" + q


@case("mode-2027-resets")
def _():
    # RIS and DECSTR return mode 2027 to the configured method (unicode here).
    q = CSI + "?2027$p"
    return (CSI + "?2027l" + ESC + "c" + q + THUMBS_TONE + "\r\n" +
            CSI + "?2027l" + CSI + "!p" + q + THUMBS_TONE)


@case("mode-2026")
def _():
    # Mode 2026, synchronized output, with the host presenting by it as
    # tsm-replay's does: DECRQM replies 2, then 1 inside a frame, answered
    # there and not at the frame's end, and 2 after RM, RIS and DECSTR each
    # end one. With the host's setting off it replies 4 and SM does nothing.
    q = CSI + "?2026$p"
    bsu, esu = CSI + "?2026h", CSI + "?2026l"
    return (q + bsu + "frame" + q + esu + q + "\r\n" +
            bsu + "a" + ESC + "c" + q +
            bsu + "b" + CSI + "!p" + q + esu +
            tt("sync", on=0) + q + bsu + q + tt("sync", on=1) + q)


@case("combining-after-move")
def _():
    # After the cursor moves, a combining mark joins the cell before the
    # cursor; with nothing there it is dropped. SGR does not end a cluster.
    return ("ab" + CSI + "D" + "́" + "\r\n" +
            "x" + CSI + "C" + "̀" + "y" + "\r\n" +
            "e" + CSI + "31m" + "́" + CSI + "0m" + "z")


@case("cluster-reuse-erase")
def _():
    # Clusters erased and printed again, so their table entries are freed and
    # reused.
    return ("éà" + CSI + "2K\r" + "àéô" +
            CSI + "1;2H" + CSI + "K" + "é")


@case("prepend-width")
def _():
    # A prepended concatenation mark (U+0600) starts a cluster with width, and
    # the digit after it joins it, mid-line and at the start of a line.
    return "x\u06001|" + "\r\n" + "\u06001|"


@case("vs16-insert-mode", 8, 2)
def _():
    # VS16 widening in insert mode moves the rest of the line right instead
    # of overwriting the cell after the cluster.
    return "abc" + CSI + "1G" + CSI + "4h" + HEART + VS16 + CSI + "4l"


@case("insert-wide-one-column", 1, 3)
def _():
    # Insert mode on a one-column screen: a wide character has no room and
    # prints as an empty cell; a VS16 cluster stays narrow. Neither shifts
    # the line.
    return CSI + "4h" + "\u4e2d" + "\r\n" + HEART + VS16


@case("cluster-max")
def _():
    # A cluster of 33 codepoints keeps its first 32.
    return "a" + "̀" * 32 + "b"


@case("alt-screen", 20, 4)
def _():
    return "main-screen" + CSI + "?1049h" + CSI + "H" + "alt-screen" + CSI + "?1049l" + "-back"


@case("replies")
def _():
    return "q" + CSI + "c" + CSI + ">c" + CSI + "5n" + cup(3, 7) + CSI + "6n"


@case("dec-graphics")
def _():
    return ESC + "(0" + "lqqqk\r\nx   x\r\nmqqqj" + ESC + "(B" + " ascii"


@case("osc-sequences")
def _():
    return (OSC + "0;window title" + "\x07" + "a" + OSC + "2;other" + ST + "b" +
            OSC + "8;;https://example.com" + ST + "link" + OSC + "8;;" + ST + "c")


@case("dcs-sixel")
def _():
    # A 6x6 red square in sixel, with text on both sides.
    return "before" + DCS + "0;1;0q" + '"1;1;6;6' + "#1;2;100;0;0#1~~~~~~-~~~~~~" + ST + "after"


@case("apc-kitty-graphics")
def _():
    # kitty graphics: transmit-and-display a 1x1 RGBA pixel, with text around it.
    return "before" + APC + "Ga=T,f=32,s=1,v=1;/wAA/w==" + ST + "after"


@case("scrollback", 20, 5)
def _():
    return "".join(f"line {n}\r\n" for n in range(1, 13)) + "last"


@case("reset", 20, 4)
def _():
    return sgr(1, 31) + "red" + CSI + "?1049h" + CSI + "3;4r" + ESC + "c" + "clean"


# Images (ADR-103 step 3a in aaronsb/kmscon). Each img-* case judges one row of
# ADR-103's screen-event table for both kinds: a sixel and a kitty image,
# each 24x32 pixels, 3x2 cells of 8x16. The sixels are real sixel strings
# (step 3c), shown with the cursor saved and restored around them, so they
# land where a test placement would and leave the cursor where it was. No
# protocol makes kitty images yet (step 4a), so those, and the one sixel that
# reaches past the screen's bottom, which no sixel string can place, come
# from tsm-replay's test commands, an APC that only a tsm-replay built
# against the fork's libtsm runs (see tsm-replay.c).

def tt(cmd, **kv):
    return APC + "tsm-test;" + cmd + "".join(f";{k}={v}" for k, v in kv.items()) + ST


def img_setup(sb=0):
    out = tt("cell", w=8, h=16)
    if sb:
        out += tt("sb-max", n=sb)
    return out + tt("image", n=2, w=24, h=32, kitty=1, fill="ff00ff00")


def put(img, col, row, **kv):
    return tt("place", img=img, col=col, row=row, **kv)


def sixel(w, h, rgb=(0, 0, 100), p1=0, p2=0, holes=()):
    """A sixel string for a w x h picture of one colour, register 1 in RGB
    percent, drawn band by band; with P2 = 1 the pixels in @holes, (x, y)
    pairs, stay unset and so transparent."""
    out = f"{DCS}{p1};{p2};0q\"1;1;{w};{h}#1;2;{rgb[0]};{rgb[1]};{rgb[2]}#1"
    for y in range(0, h, 6):
        rows = min(6, h - y)
        cols = []
        for x in range(w):
            bits = (1 << rows) - 1
            for hx, hy in holes:
                if hx == x and y <= hy < y + rows:
                    bits &= ~(1 << (hy - y))
            cols.append(chr(63 + bits))
        out += "".join(cols) + "-"
    return out + ST


def six(col, row, w=24, h=32, **kv):
    """A sixel at @col, @row with the cursor kept, as a test placement."""
    return ESC + "7" + cup(row + 1, col + 1) + sixel(w, h, **kv) + ESC + "8"


@case("img-print", 12, 4)
def _():
    # Text written over a sixel removes the cells it covers; a kitty
    # placement is untouched.
    return (img_setup() + six(1, 1) + put(2, 5, 1) +
            cup(2, 3) + "xy" + cup(2, 7) + "z")


@case("img-sixel-over", 12, 4)
def _():
    # An opaque sixel removes the parts of older sixels under it; a
    # transparent one leaves them; neither touches a kitty placement.
    return (img_setup() + six(1, 1) + put(2, 6, 1) +
            six(2, 2, 16, 16, rgb=(0, 100, 100)) +
            six(1, 1, 8, 16, p2=1, holes=((0, 0), (7, 15))) +
            six(6, 1, 16, 16, rgb=(100, 100, 0)))


def erase_case(op):
    # a sixel at columns 2-4 and a kitty image at 7-9, rows 1-2; the erase
    # runs with the cursor at row 1, column 3
    return img_setup() + six(2, 1) + put(2, 7, 1) + cup(2, 4) + op


@case("img-el0", 12, 4)
def _():
    return erase_case(CSI + "K")


@case("img-el1", 12, 4)
def _():
    return erase_case(CSI + "1K")


@case("img-el2", 12, 4)
def _():
    return erase_case(CSI + "2K")


@case("img-ech", 12, 4)
def _():
    return erase_case(CSI + "X")


@case("img-ed0", 12, 4)
def _():
    return erase_case(CSI + "J")


@case("img-ed1", 12, 4)
def _():
    return erase_case(CSI + "1J")


# The parser sets no protected cells (DECSCA is not parsed), so selective
# erase erases what ED and EL do; test_image.c covers a protected cell.
@case("img-decsed", 12, 4)
def _():
    return erase_case(CSI + "?J")


@case("img-decsel", 12, 4)
def _():
    return erase_case(CSI + "?2K")


@case("img-ich", 12, 4)
def _():
    return erase_case(CSI + "@")


@case("img-dch", 12, 4)
def _():
    return erase_case(CSI + "P")


@case("img-ed2", 12, 4)
def _():
    # ED 2 removes every placement on the screen's lines: the kitty
    # placements whole, the sixels where they lie on the screen. A kitty
    # image wholly in scrollback stays; the view is scrolled back to show it.
    return (img_setup(sb=10) + tt("image", n=3, w=8, h=16, kitty=1, fill="ffffffff") +
            put(3, 0, 0) + cup(4, 1) + "\n" +
            six(2, 0) + put(2, 7, 0) + six(0, 2, 8, 16, rgb=(0, 100, 100)) +
            put(3, 0, 3) +
            "\n" + CSI + "2J" + tt("sb-up", n=2))


@case("img-ed3", 12, 4)
def _():
    # ED 3 drops the scrollback, and the placements anchored on it, even
    # those reaching onto the screen
    return (img_setup(sb=10) + tt("image", n=3, w=8, h=16, kitty=1, fill="ffffffff") +
            six(0, 0) + put(2, 4, 0) + cup(4, 1) + "\n" +
            put(3, 5, 1) + six(0, 2, 8, 16, rgb=(0, 100, 100)) + CSI + "3J")


@case("img-scroll-up", 12, 4)
def _():
    # LF at the bottom, IND and SU scroll the whole screen: placements move
    # with their lines into scrollback, whole, and the view brings them back
    return (img_setup(sb=10) + six(0, 1) + put(2, 4, 1) +
            cup(4, 1) + "\n" + ESC + "D" + CSI + "2S" + tt("sb-up", n=3))


@case("img-scroll-up-bottom", 20, 5)
def _():
    # Placements reaching past the screen's bottom move up with their
    # lines: the bottom of the screen is no region edge (aaronsb/libtsm#17)
    return (tt("cell", w=8, h=16) +
            tt("image", n=1, w=24, h=64, kitty=1, fill="ff00ff00") +
            tt("image", n=2, w=24, h=64, fill="ff0000ff") +
            put(1, 0, 3) + put(2, 4, 3) + cup(5, 1) + "\n\n\n")


@case("img-scroll-down", 12, 5)
def _():
    # SD and RI at the top: placements move down with their lines; the part
    # pushed past the bottom is clipped
    return (img_setup() + six(0, 1) + put(2, 4, 1) +
            CSI + "T" + cup(1, 1) + ESC + "M" + CSI + "T")


@case("img-scroll-down-sb", 12, 4)
def _():
    # SD with placements reaching from scrollback onto the screen: the sixel
    # splits at the screen's top, its screen part moving down; the kitty
    # placement, not wholly on the screen, stays
    return (img_setup(sb=10) + six(0, 0) + put(2, 4, 0) +
            cup(4, 1) + "\n" + CSI + "T" + tt("sb-up", n=1))


def region_images():
    return (img_setup() + tt("image", n=3, w=8, h=32, kitty=1, fill="ffffffff") +
            tt("image", n=4, w=8, h=32, kitty=1, fill="ff00ffff"))


@case("img-region", 16, 8)
def _():
    # Two lines scroll in the region of rows 2-5. A sixel splits at the
    # region's edges, the part inside moving and clipped where it leaves; a
    # kitty placement wholly inside moves and is clipped, one across an edge
    # stays.
    return (region_images() +
            six(0, 1) + six(3, 3) + six(6, 5) +
            put(2, 9, 3) + put(3, 13, 5) + put(4, 14, 1) +
            CSI + "3;6r" + cup(6, 1) + "\n\n")


@case("img-il", 14, 6)
def _():
    # IL scrolls rows 3-5 down from the cursor's line
    return (region_images() + six(0, 2) + six(3, 4) + put(2, 6, 4) +
            tt("image", n=5, w=24, h=32, kitty=1, fill="ffff00ff") + put(5, 9, 2) +
            cup(4, 1) + CSI + "L")


@case("img-dl", 14, 6)
def _():
    # DL scrolls rows 3-5 up from the cursor's line
    return (region_images() + six(0, 2) + six(3, 4) + put(2, 6, 4) +
            tt("image", n=5, w=24, h=32, kitty=1, fill="ffff00ff") + put(5, 9, 2) +
            put(3, 12, 3) + cup(4, 1) + CSI + "M")


@case("img-discard-sb", 12, 3)
def _():
    # Scrollback past its size discards its oldest line: a placement on it
    # loses that row and is anchored on the next line
    return (tt("cell", w=8, h=16) + tt("sb-max", n=2) +
            tt("image", n=2, w=24, h=48, kitty=1, fill="ff00ff00") +
            six(0, 0, 24, 48) + put(2, 4, 0) + cup(3, 1) + "\n\n\n\n" + tt("sb-up", n=2))


@case("img-discard-alt", 12, 4)
def _():
    # The alternate screen has no scrollback: its top line is discarded
    return (CSI + "?1049h" + img_setup() + six(0, 0) + put(2, 4, 0) +
            cup(4, 1) + "\n")


@case("img-resize", 12, 4)
def _():
    # Lines the height change brings back from scrollback carry their
    # placements; a narrower screen keeps them, drawn clipped
    return (img_setup(sb=10) + six(0, 0) + put(2, 6, 0) +
            tt("image", n=3, w=24, h=32, kitty=1, fill="ffffffff") + put(3, 9, 2) +
            cup(4, 1) + "\n\n" + tt("resize", c=10, r=6))


@case("img-resize-shrink", 12, 6)
def _():
    # Lines the height change sends to scrollback carry theirs
    return (img_setup(sb=10) + six(0, 3) + put(2, 4, 3) + cup(6, 1) +
            tt("resize", c=12, r=3))


@case("img-alt-1049", 12, 4)
def _():
    # The alternate screen's store is emptied on entry and on leaving, and
    # the main screen's placements show again
    return (img_setup() + six(0, 0) + put(2, 4, 0) + CSI + "?1049h" +
            img_setup() + six(0, 1) + put(2, 4, 2) + CSI + "?1049l")


@case("img-alt-47", 12, 4)
def _():
    # 47 keeps the alternate screen's text when left, but not its images:
    # entered again, it shows only the sixel placed then
    return (img_setup() + six(0, 0) + CSI + "?47h" + "alt" +
            img_setup() + put(2, 4, 1) + CSI + "?47l" + CSI + "?47h" +
            img_setup() + six(0, 2))


@case("img-alt-1047", 12, 4)
def _():
    return (img_setup() + put(2, 4, 0) + CSI + "?1047h" +
            img_setup() + six(0, 1) + CSI + "?1047l")


@case("img-alt-shown", 12, 4)
def _():
    # On the alternate screen only its own placements show
    return (img_setup() + six(0, 0) + CSI + "?1049h" +
            img_setup() + put(2, 4, 1))


@case("img-ris", 12, 4)
def _():
    # RIS empties both stores; a sixel placed after it shows alone
    return (img_setup(sb=10) + six(0, 0) + cup(4, 1) + "\n" + put(2, 4, 2) +
            CSI + "?1049h" + img_setup() + put(2, 4, 1) + ESC + "c" +
            img_setup() + six(0, 1))


@case("img-decstr", 12, 4)
def _():
    # A soft reset leaves text, so it leaves pictures
    return img_setup() + "text" + six(0, 1) + put(2, 4, 1) + CSI + "!p"


# Sixel (ADR-103 step 3c): the decoder, placement and cursor, the modes and
# the replies, through real sixel strings. Each case asks for the cursor
# (DSR 6) after each picture, so its replies record where the cursor went.
DSR = CSI + "6n"
SIXEL_DIR = Path(__file__).resolve().parent.parent / "sixel"


def cell(w=8, h=16):
    return tt("cell", w=w, h=h)


@case("six-cursor-xterm", 40, 12)
def _():
    # The byte stream of a probe run in xterm 411 (-ti vt340, 6x13 cells,
    # 40x12) as a black box; its cursor reports were 4;3 4;3 2;3 2;3, then
    # 4;5 with mode 8452, 3;1 at the right edge, 11;1 at the bottom, and
    # 3;4 with DECSDM set. This libtsm answers 12;1 at the bottom and 2;40
    # at the right edge, ADR-103's rules, where xterm differs.
    out = cell(6, 13)
    for pre, w, h in (
            (CSI + "2J" + cup(2, 3), 12, 32),
            (CSI + "2J" + cup(2, 3), 12, 39),
            (CSI + "2J" + cup(2, 3), 12, 1),
            (CSI + "2J" + cup(2, 3), 12, 13),
            (CSI + "2J" + CSI + "?8452h" + cup(2, 3), 12, 27),
            (CSI + "2J" + CSI + "?8452h" + cup(2, 39), 24, 13),
            (CSI + "?8452l" + CSI + "2J" + cup(12, 1), 12, 39),
            (CSI + "2J" + CSI + "?80h" + cup(3, 4), 12, 39)):
        out += pre + sixel(w, h) + DSR + CSI + "?80l" + CSI + "?8452l"
    return out


@case("six-cursor", 12, 6)
def _():
    # The cursor ends on the row of the picture's last pixel row, at its
    # left column: 40 pixels from row 1 reach row 3, and one pixel stays on
    # its row. A new line then starts under the picture.
    return (cell() + cup(2, 3) + sixel(16, 40) + DSR + "\r\nab" +
            cup(1, 9) + sixel(8, 1) + DSR)


@case("six-cursor-8452", 12, 4)
def _():
    # Mode 8452: the cursor ends right of the picture on its last row; at
    # the right edge, in the last column. Reset, it ends below again.
    return (cell() + CSI + "?8452h" + cup(1, 1) + sixel(16, 20) + DSR + "x" +
            cup(4, 9) + sixel(40, 16) + DSR +
            CSI + "?8452l" + cup(1, 6) + sixel(8, 16) + DSR)


@case("six-scroll-bottom", 12, 4)
def _():
    # A picture passing the bottom scrolls the screen as index would, its
    # top going into scrollback with its line; the view shows it back
    return (cell() + tt("sb-max", n=10) + "line0\r\nline1" + cup(3, 2) +
            sixel(16, 48) + DSR + tt("sb-up", n=1))


@case("six-scroll-region", 12, 6)
def _():
    # In the region of rows 2 to 5, a picture that fits is scrolled into the
    # region whole before it is placed. One taller than the region is
    # scrolled to its top, then by index: its top row leaves the region and
    # is cut, the part below the region stays, and the first picture
    # scrolls out of the region
    return (cell() + "top" + cup(6, 1) + "bottom" + CSI + "2;5r" +
            cup(4, 1) + sixel(8, 48) + DSR +
            cup(5, 4) + sixel(8, 80, rgb=(100, 0, 0)) + DSR)


@case("six-decsdm", 12, 4)
def _():
    # DECSDM set (mode 80, xterm's sense): the picture at the top left, cut
    # to the screen, the cursor unmoved; reset, at the cursor again
    return (cell() + "text" + CSI + "?80h" + cup(3, 5) + sixel(120, 80) + DSR +
            CSI + "?80l" + cup(3, 5) + sixel(8, 16, rgb=(100, 0, 0)) + DSR)


@case("six-colours", 12, 4)
def _():
    # RGB and HLS registers (HLS hue 0 is blue), repeats, graphics carriage
    # return and next line, colour taken when drawn, register 0 behind
    # unset pixels (P2 0)
    return (cell() + DCS + "0;0;0q" +
            "#1;2;100;0;0#2;1;0;50;100#3;1;120;50;100#4;1;240;50;100" +
            "#1!8~#2!8~$#3!4N#1;2;0;0;0!4N-" +
            "#4~?~?~?~?" + ST + DSR)


@case("six-raster", 12, 4)
def _():
    # Raster attributes size the picture past its data: P2 0 fills the
    # rest with register 0, P2 1 leaves it transparent; raster attributes
    # after data are ignored; Pan;Pad and P1 do not scale a dot
    return (cell() + DCS + "0;0;0q\"1;1;16;20#1~~~~" + ST + DSR +
            cup(1, 4) + DCS + "0;1;0q\"1;1;16;20#1~~~~\"1;1;40;40" + ST + DSR +
            cup(1, 7) + DCS + "2;1;0q\"2;1;8;12#1!8~-!8~" + ST + DSR)


@case("six-registers", 12, 4)
def _():
    # Mode 1070 set (the default): each picture starts from the default
    # registers, so the second is black; reset, definitions carry over, so
    # the fourth is red like the first and third
    red = "#20;2;100;0;0"
    return (cell() + DCS + "q" + red + "#20!8~" + ST +
            cup(1, 2) + DCS + "q#20!8~" + ST +
            CSI + "?1070l" + cup(1, 3) + DCS + "q" + red + "#20!8~" + ST +
            cup(1, 4) + DCS + "q#20!8~" + ST)


@case("six-registers-ris", 12, 4)
def _():
    # RIS empties the shared registers and sets mode 1070 again: after it,
    # both pictures are black, the second shared again
    return (cell() + CSI + "?1070l" + DCS + "q#20;2;100;0;0#20!8~" + ST +
            ESC + "c" + cell() + DCS + "q#20!8~" + ST +
            CSI + "?1070l" + cup(1, 2) + DCS + "q#20!8~" + ST)


@case("six-max-size", 12, 4)
def _():
    # max-graphic-size bounds each dimension; pixels past it are dropped
    return (cell() + tt("graphic-max", w=16, h=10) + sixel(40, 40) + DSR +
            CSI + "?2;1S")


@case("six-cancel", 12, 4)
def _():
    # CAN and SUB discard a picture; ESC ending the string shows it, and
    # what follows runs; an empty picture, and a DCS with an intermediate
    # (DECRQSS), make nothing
    pic = sixel(16, 16)[:-2]
    return (cell() + pic + "\x18" + DSR + cup(1, 4) + pic + "\x1a" + DSR +
            cup(1, 7) + pic + ESC + "[2;1H" + DSR +
            DCS + "q" + ST + DCS + "q#1" + ST + DCS + "$q\"p" + ST + DSR)


@case("six-text", 12, 4)
def _():
    # Text written over a sixel removes the cells it covers; a transparent
    # sixel over text leaves the text, which shows through it
    return (cell() + sixel(32, 16) + cup(1, 2) + "ab" +
            cup(3, 1) + "under" + cup(3, 1) +
            sixel(24, 16, p2=1, holes=((0, 0),)) + DSR)


@case("six-scrollback", 12, 4)
def _():
    # A1 in ADR-103: a picture scrolls into scrollback with its lines, and
    # back into view with them
    return (cell() + tt("sb-max", n=10) + "above" + cup(2, 1) + sixel(16, 32) +
            "\r\nbelow\n\n\n\n" + tt("sb-up", n=4))


@case("six-clear", 12, 4)
def _():
    # clear (ED 2, then ED 3) removes a picture on the screen and one in
    # scrollback
    return (cell() + tt("sb-max", n=10) + sixel(16, 32) + "\r\n\n\n\n" +
            sixel(16, 16) + CSI + "H" + CSI + "2J" + CSI + "3J" + DSR +
            tt("sb-up", n=4))


@case("six-replies", 12, 4)
def _():
    # DA1 with attribute 4; XTSMGRAPHICS for registers (read, set within
    # range, set out of range, reset, read the maximum, a bad action),
    # geometry (read, read the maximum, set refused) and ReGIS (none);
    # XTWINOPS 14, 16 and 18
    return (cell() + CSI + "c" +
            CSI + "?1;1S" + CSI + "?1;3;256S" + CSI + "?1;3;1S" +
            CSI + "?1;2S" + CSI + "?1;4S" + CSI + "?1;9S" +
            CSI + "?2;1S" + CSI + "?2;4S" + CSI + "?2;3;100;100S" +
            CSI + "?3;1S" +
            CSI + "14t" + CSI + "16t" + CSI + "18t")


@case("six-off", 12, 4)
def _():
    # With no budget, sixel is off: the string is ignored, DA1 has no
    # attribute 4 and XTSMGRAPHICS reports no graphics
    return (cell() + tt("limit", b=0) + sixel(16, 16) + DSR + CSI + "c" +
            CSI + "?1;1S" + CSI + "?2;1S")


def img2sixel(name):
    # libsixel's img2sixel output for test/sixel/<name>.ppm (gen-fixtures.sh)
    return (SIXEL_DIR / f"{name}.six").read_text("latin-1")


@case("six-img2sixel-palette16", 12, 4)
def _():
    return cell() + "img2sixel:" + "\r\n" + img2sixel("palette16") + DSR


@case("six-img2sixel-gradient", 12, 6)
def _():
    return cell() + cup(2, 2) + img2sixel("gradient") + DSR


@case("six-img2sixel-highcolor", 12, 4)
def _():
    return cell() + img2sixel("highcolor") + DSR


# The kitty graphics protocol (ADR-103 step 4a): commands written from the
# spec, https://sw.kovidgoyal.net/kitty/graphics-protocol/, alone (ADR-300).
# Each case asks for the cursor (DSR 6) where the cursor matters, so its
# replies record where it went beside the replies to the commands. Cells
# are 8x16.

def kitty(keys="", data=b"", raw=None):
    """ESC _ G <keys> ; <base64 of data> ST; @raw replaces the payload."""
    payload = raw if raw is not None else base64.b64encode(data).decode("ascii")
    return APC + "G" + keys + (";" + payload if payload else "") + ST


def rgba(w, h, px=(255, 0, 0, 255)):
    """w x h pixels of one straight-alpha RGBA colour, f=32."""
    return bytes(px) * (w * h)


def rgb(w, h, px=(0, 0, 255)):
    """w x h pixels of one RGB colour, f=24."""
    return bytes(px) * (w * h)


def chunks(keys, data, size=8):
    """The base64 of @data cut into chunks of @size characters (a multiple of
    4), m=1 on all but the last, the keys on the first only (spec)."""
    enc = base64.b64encode(data).decode("ascii")
    parts = [enc[i:i + size] for i in range(0, len(enc), size)]
    out = ""
    for n, part in enumerate(parts):
        last = n == len(parts) - 1
        head = (keys + "," if n == 0 else "") + ("m=0" if last else "m=1")
        out += kitty(head, raw=part)
    return out


def kt(i, w=16, h=16, px=(255, 0, 0, 255), keys=""):
    """Transmit and place image @i, f=32, at the cursor."""
    return kitty(f"a=T,f=32,s={w},v={h},i={i}" + keys, rgba(w, h, px))


@case("kit-transmit", 12, 4)
def _():
    # a=T with f=32 (half its pixels transparent) and f=24, each replying
    # OK for its id and moving the cursor past its box; an anonymous a=T,
    # which gets no reply; a=t stores without placing, and a=p places it
    data = rgba(16, 8) + rgba(16, 8, (0, 255, 0, 0))
    return (cell() + kitty("a=T,f=32,s=16,v=16,i=1", data) + DSR +
            kitty("a=T,f=24,s=8,v=16,i=2", rgb(8, 16)) + DSR +
            kitty("a=T,f=24,s=8,v=8", rgb(8, 8, (255, 255, 0))) + DSR +
            kitty("a=t,f=32,s=8,v=8,i=3", rgba(8, 8, (0, 0, 255, 128))) + DSR +
            cup(3, 1) + kitty("a=p,i=3") + DSR)


@case("kit-chunked", 12, 4)
def _():
    # Chunked direct transmission: the keys on the first chunk, m=1 until
    # the last; a chunk may change q. Text between chunks is not a graphics
    # command and leaves the upload pending; the placement is at the cursor
    # when the last chunk arrives (spec).
    first = chunks("a=T,f=24,s=8,v=16,i=1", rgb(8, 16))
    one, rest = first.split(ST, 1)
    return (cell() + one + ST + "ab" + rest + DSR +
            chunks("a=T,f=32,s=8,v=8,i=2,q=1", rgba(8, 8, (0, 255, 0, 255)), 12) +
            DSR)


def ramp(w, h):
    """w x h pixels of f=24 whose colour encodes their position, so a lost,
    doubled or misplaced byte changes the image's digest."""
    return bytes(c for y in range(h) for x in range(w)
                 for c in (x & 0xff, y & 0xff, (7 * x + 13 * y) & 0xff))


@case("kit-chunk-large", 12, 4)
def _():
    # A chunk of 131072 base64 characters, the size kitten icat sends, 32
    # times the spec's "no larger than 4096 bytes": the payload is
    # streamed, so the chunk is accepted whole and the image is the one
    # sent. 160x206 pixels of f=24 are 131840 characters, the big chunk
    # (m=1) and a tail of 768 (m=0). FNV-1a over the pixels as tsm-replay
    # reads them, A R G B with A=255, computed from ramp() alone: 5ce2ce65.
    return cell() + chunks("a=T,f=24,s=160,v=206,i=1,c=4,r=2", ramp(160, 206),
                           131072) + DSR


@case("kit-chunk-abort", 12, 4)
def _():
    # A pending upload is aborted by a delete, by any other graphics
    # command, and by CAN; its later chunks then arrive as commands of
    # their own, which libtsm refuses (no s and v) without a reply (no id)
    c1 = chunks("a=T,f=24,s=8,v=16,i=1", rgb(8, 16))
    c2 = chunks("a=T,f=24,s=8,v=16,i=2", rgb(8, 16))
    c3 = chunks("a=T,f=24,s=8,v=16,i=3", rgb(8, 16))
    def first(c):
        return c.split(ST, 1)[0] + ST
    def rest(c):
        return c.split(ST, 1)[1]
    return (cell() + first(c1) + kitty("a=d") + rest(c1) +
            first(c2) + kitty("a=p,i=9") + rest(c2) +
            first(c3) + APC + "Gm=1;AAAA\x18" + rest(c3) +
            kitty("a=p,i=1") + kitty("a=p,i=2") + kitty("a=p,i=3") + DSR)


@case("kit-ids", 12, 4)
def _():
    # I: the terminal picks the lowest unused id, and a=p with I places the
    # newest image with that number; i with I is refused; a=p of an id
    # never sent is ENOENT
    return (cell() + kitty("a=t,f=24,s=8,v=8,i=1", rgb(8, 8)) +
            kitty("a=t,f=24,s=8,v=8,I=7", rgb(8, 8, (255, 0, 0))) +
            kitty("a=t,f=24,s=8,v=8,I=7", rgb(8, 8, (0, 255, 0))) +
            kitty("a=p,I=7,p=3") + kitty("a=T,f=24,s=8,v=8,i=5,I=7", rgb(8, 8)) +
            kitty("a=p,i=4") + kitty("a=p,I=8"))


@case("kit-replace", 12, 4)
def _():
    # A placement id names a placement to replace, which moves it: image 1
    # ends with two placements, one moved from column 2 to row 1, column 6.
    # Transmitting an id again replaces the image and deletes its
    # placements (spec): image 2's two placements go, and the new image is
    # not shown until placed.
    return (cell() + kt(1) + kitty("a=p,i=1,p=5") +
            cup(2, 7) + kitty("a=p,i=1,p=5") + DSR +
            cup(3, 1) + kt(2, 8, 8) + kitty("a=p,i=2,C=1") +
            kitty("a=t,f=24,s=8,v=8,i=2", rgb(8, 8)))


@case("kit-replace-refused", 12, 4)
def _():
    # Under a tight limit, a replacement that does not fit beside a sixel
    # is refused with ENOSPC, and the old image and its placement stay
    # (libtsm#37): placed again, it is still the red one
    cost = 16 * 16 * 4 + 512
    # (the limit holds two images and one more placement)
    return (cell() + tt("limit", b=2 * cost + 256) + kt(1) + six(2, 0, 16, 16) +
            kitty("a=t,f=32,s=16,v=32,i=1", rgba(16, 32, (255, 255, 255, 255))) +
            cup(3, 1) + kitty("a=p,i=1"))


@case("kit-place-size", 24, 6)
def _():
    # c and r together letterbox or pillarbox the picture; one alone keeps
    # the aspect; x, y, w, h choose the source rectangle, cut to the image;
    # X, Y offset it in its first cell, and an offset outside the cell is
    # refused
    img = kitty("a=t,f=32,s=32,v=16,i=1", rgba(32, 16))
    return (cell() + img + kitty("a=p,i=1,c=2,r=2,C=1") + cup(1, 4) +
            kitty("a=p,i=1,c=3,C=1") + cup(1, 8) + kitty("a=p,i=1,r=2,C=1") +
            cup(1, 14) + kitty("a=p,i=1,x=8,y=4,w=16,h=100,C=1") +
            cup(4, 1) + kitty("a=p,i=1,X=3,Y=5,C=1") +
            cup(4, 8) + kitty("a=p,i=1,X=8,C=1") + kitty("a=p,i=1,x=32") + DSR)


@case("kit-bands", 12, 4)
def _():
    # z chooses the band: below cell backgrounds, below text, above it; at
    # one z the lower id draws first (spec)
    return (cell() + kt(3, 8, 8, keys=",z=-1073741825,C=1") +
            kt(2, 8, 8, keys=",z=-1,C=1") + kt(5, 8, 8, keys=",z=4,C=1") +
            kt(9, 8, 8, (0, 255, 0, 255), keys=",C=1") +
            kt(4, 8, 8, (0, 0, 255, 255), keys=",C=1"))


@case("kit-cursor", 12, 4)
def _():
    # C=1 leaves the cursor; otherwise it moves past the box: three rows
    # from the last row scroll the screen twice; a box reaching the right
    # edge sends the cursor to column 0 of the next row; inside a region
    # the scroll is the region's
    return (cell() + kt(1, 8, 32, keys=",C=1") + DSR +
            cup(4, 1) + kt(2, 8, 48) + DSR +
            cup(1, 11) + kt(3, 16, 16) + DSR +
            CSI + "2;3r" + cup(3, 5) + kt(4, 8, 32) + DSR + CSI + "r")


def delete_case(lower, upper, setup=""):
    # images 1, 2 and 3 placed at columns 0, 4 and 8, with placement ids 1;
    # @lower should remove image 1's placement and @upper image 2's, and
    # placing each again shows which kept its data (OK) and which lost it
    # (ENOENT); image 3 stays
    return (cell() + setup +
            kitty("a=T,f=32,s=16,v=16,i=1,p=1,z=1,C=1", rgba(16, 16)) +
            cup(1, 5) + kitty("a=T,f=32,s=16,v=16,i=2,p=1,z=2,C=1",
                              rgba(16, 16, (0, 255, 0, 255))) +
            cup(1, 9) + kitty("a=T,f=32,s=16,v=16,i=3,p=1,z=3,C=1",
                              rgba(16, 16, (0, 0, 255, 255))) +
            lower + upper + cup(4, 1) + kitty("a=p,i=1,C=1") +
            cup(4, 5) + kitty("a=p,i=2,C=1"))


@case("kit-delete-a", 12, 4)
def _():
    # a and A: every placement on the screen; A frees what it leaves
    # unplaced. Image 3 is placed again before A so A has something to do.
    return delete_case(kitty("a=d,d=a"),
                       kitty("a=p,i=2,C=1") + kitty("a=d,d=A"))


@case("kit-delete-i", 12, 4)
def _():
    # i and I, by id, and with p only that placement; an uppercase delete
    # whose p matches none of an image's placements frees nothing, even
    # an image with no placement at all (image 4, placed again at the end)
    return (delete_case(kitty("a=t,f=32,s=8,v=8,i=4", rgba(8, 8, (255, 0, 255, 255))) +
                        kitty("a=d,d=i,i=1"), kitty("a=d,d=I,i=2,p=1") +
                        kitty("a=d,d=I,i=3,p=2") + kitty("a=d,d=I,i=4,p=9")) +
            cup(4, 9) + kitty("a=p,i=4,C=1"))


@case("kit-delete-n", 12, 4)
def _():
    # n and N: the newest image with the number
    return (cell() + kitty("a=T,f=32,s=16,v=16,I=4,C=1", rgba(16, 16)) +
            cup(1, 5) + kitty("a=T,f=32,s=16,v=16,I=4,C=1",
                              rgba(16, 16, (0, 255, 0, 255))) +
            cup(1, 9) + kitty("a=T,f=32,s=16,v=16,I=6,C=1",
                              rgba(16, 16, (0, 0, 255, 255))) +
            kitty("a=d,d=n,I=4") + kitty("a=d,d=N,I=6") +
            cup(4, 1) + kitty("a=p,i=2,C=1") + cup(4, 5) + kitty("a=p,i=3,C=1"))


@case("kit-delete-c", 12, 4)
def _():
    # c and C: the placements under the cursor
    return delete_case(cup(1, 2) + kitty("a=d,d=c"), cup(1, 6) + kitty("a=d,d=C"))


@case("kit-delete-p", 12, 4)
def _():
    # p and P: the placements over cell x, y (1-based)
    return delete_case(kitty("a=d,d=p,x=2,y=1"), kitty("a=d,d=P,x=5,y=1"))


@case("kit-delete-q", 12, 4)
def _():
    # q and Q: over a cell and with a z; a wrong z deletes nothing
    return delete_case(kitty("a=d,d=q,x=1,y=1,z=1") + kitty("a=d,d=Q,x=9,y=1,z=1"),
                       kitty("a=d,d=Q,x=6,y=1,z=2"))


@case("kit-delete-r", 12, 4)
def _():
    # r and R: images whose id lies in x to y; an inverted range deletes
    # nothing
    return delete_case(kitty("a=d,d=r,x=1,y=1") + kitty("a=d,d=R,x=3,y=1"),
                       kitty("a=d,d=R,x=2,y=2"))


@case("kit-delete-x", 12, 4)
def _():
    # x and X: the placements over a column (1-based)
    return delete_case(kitty("a=d,d=x,x=2"), kitty("a=d,d=X,x=5"))


@case("kit-delete-y", 12, 4)
def _():
    # y and Y: the placements over a row (1-based); image 1 is moved to
    # row 3 and image 2 to row 4 first, its placement id replacing them
    return delete_case(cup(3, 1) + kitty("a=p,i=1,p=1,C=1") + kitty("a=d,d=y,y=3"),
                       cup(4, 5) + kitty("a=p,i=2,p=1,C=1") + kitty("a=d,d=Y,y=4"))


@case("kit-delete-z", 12, 4)
def _():
    # z and Z: the placements with a z
    return delete_case(kitty("a=d,d=z,z=1"), kitty("a=d,d=Z,z=2"))


@case("kit-delete-f", 12, 4)
def _():
    # f and F: animation frames, which do not exist yet: nothing goes
    return delete_case(kitty("a=d,d=f,i=1"), kitty("a=d,d=F,i=2"))


@case("kit-delete-sb", 12, 4)
def _():
    # a deletes only on the screen and I in scrollback too: images 1 and
    # 3 scroll into scrollback, and the view shows that a left image 1 and
    # I took image 3; neither touches a sixel
    return (cell() + tt("sb-max", n=10) + kt(1, 8, 16, keys=",C=1") +
            cup(1, 4) + kt(3, 8, 16, keys=",C=1") + cup(4, 1) + "\n" +
            kt(2, 8, 16, keys=",C=1") + six(4, 2, 8, 16) + kitty("a=d,d=a") +
            kitty("a=d,d=I,i=3") + tt("sb-up", n=1))


@case("kit-replies", 12, 4)
def _():
    # q=0 answers OK and errors, q=1 errors only, q=2 nothing; a command
    # with neither i nor I gets no reply
    return (cell() + kt(1, 8, 8, keys=",C=1") + kt(2, 8, 8, keys=",q=1,C=1") +
            kt(3, 8, 8, keys=",q=2,C=1") + kitty("a=p,i=7") + kitty("a=p,i=7,q=1") +
            kitty("a=p,i=7,q=2") + kitty("a=T,f=32,s=8,v=8", rgba(8, 8)))


@case("kit-query", 12, 4)
def _():
    # a=q loads and checks without storing or replacing: OK, then EINVAL
    # for data short of s x v, and a=p of its id finds nothing; an a=q with
    # the id of a stored image leaves that image
    return (cell() + kitty("a=q,f=24,s=1,v=1,i=31", rgb(1, 1)) +
            kitty("a=q,f=24,s=2,v=1,i=32", rgb(1, 1)) + kitty("a=p,i=31") +
            kt(5, 8, 8, keys=",C=1") + kitty("a=q,f=24,s=1,v=1,i=5", rgb(1, 1)) +
            cup(2, 1) + kitty("a=p,i=5,C=1"))


@case("kit-query-number", 12, 4)
def _():
    # an a=q without i, with I=52 or with neither, is dropped at its ST, as
    # kitty 0.49.2 drops it (ADR-107, rounds 4 to 6b): no reply under q
    # absent, 0, 1 or 2, on t=d, t=t and t=s, whether its data would load
    # or is too short, and a chunked one's continuation is swallowed; a=p of
    # number 52
    # then finds nothing (ENOENT), and the same a=q with i=51 answers OK
    tmp, shm = b"/tmp/tty-graphics-protocol-q", b"/kitty-q"
    out = cell()
    for ids in (",I=52", ""):
        for q in ("", ",q=0", ",q=1", ",q=2"):
            out += (kitty("a=q,f=24,s=1,v=1" + ids + q, rgb(1, 1)) +
                    kitty("a=q,f=24,s=2,v=1" + ids + q, rgb(1, 1)) +
                    kitty("a=q,t=t,f=24,s=1,v=1" + ids + q, tmp) +
                    kitty("a=q,t=s,f=24,s=1,v=1" + ids + q, shm))
        out += (kitty("a=q,f=24,s=1,v=1,m=1" + ids, raw="/w==") +
                kitty("m=0", raw="AAA="))
    return (out + kitty("a=p,I=52") + kitty("a=q,f=24,s=1,v=1,i=51", rgb(1, 1)))


@case("kit-refused", 12, 4)
def _():
    # EINVAL: animation, Unicode placeholders, relative placements, a raw
    # format without s and v, data short of s x v x bytes per pixel, a byte
    # outside base64, a key it cannot read, an unknown action; PNG and zlib
    # with no host hook; EBADF: files, temporary files and shared memory
    # with no host hook, and an empty name. Data past s x v x bytes per
    # pixel (i=4) is ignored, as kitty ignores it, an unknown key is
    # ignored, and a key section past 1 KiB ignores the whole APC.
    # The fixture host is removed first, so no command reaches a hook.
    return (cell() + tt("host", on=0) + kitty("a=f,i=1,s=1,v=1", rgb(1, 1)) +
            kitty("a=a,i=1") +
            kitty("a=c,i=1") + kitty("a=T,U=1,f=24,s=1,v=1,i=2", rgb(1, 1)) +
            kitty("a=p,i=2,P=1,Q=1") + kitty("a=t,f=24,i=3", rgb(1, 1)) +
            kitty("a=t,f=24,s=1,v=1,i=4", rgb(2, 1)) +
            kitty("a=t,f=24,s=2,v=1,i=5", rgb(1, 1)) +
            kitty("a=t,f=24,s=1,v=1,i=6", raw="AA*A") +
            kitty("a=t,f=24,s=x,v=1,i=7", rgb(1, 1)) + kitty("a=Z,i=8") +
            kitty("a=t,f=100,i=9", b"\x89PNG") +
            kitty("a=t,f=24,s=1,v=1,o=z,i=10", b"x\x9c") +
            kitty("a=t,t=f,f=100,i=11", b"/tmp/picture.png") +
            kitty("a=t,t=t,f=24,s=1,v=1,i=12", b"/tmp/tty-graphics-protocol-1") +
            kitty("a=t,t=s,s=1,v=1,i=13", b"/kitty-shm") +
            kitty("a=t,t=f,f=100,i=14") +
            kitty("a=T,f=24,s=1,v=1,i=15,K=9", rgb(1, 1)) +
            kitty("a=T,f=24,s=1,v=1,i=16," + "N=0," * 260, rgb(1, 1)))


@case("kit-cancel", 12, 4)
def _():
    # CAN and SUB abort a command: no image, no reply
    return (cell() + APC + "Ga=T,f=24,s=1,v=1,i=1;AAAA\x18" +
            APC + "Ga=T,f=24,s=1,v=1,i=2;AAAA\x1a" + kitty("a=p,i=1"))


@case("kit-evict", 12, 4)
def _():
    # Past the limit, images go unplaced first, then placed only in
    # scrollback, then kitty images placed on the screen, oldest
    # transmission first; a sixel on the screen is never evicted, so a new
    # image that does not fit beside it is refused with ENOSPC
    cost = 8 * 16 * 4 + 512
    return (cell() + tt("sb-max", n=10) + tt("limit", b=4 * cost) +
            kt(1, 8, 16) + "\r\n\n\n\n" + six(2, 0, 8, 16) +
            kt(2, 8, 16, keys=",C=1") + cup(1, 7) + kt(3, 8, 16, keys=",C=1") +
            kitty("a=t,f=32,s=8,v=16,i=4", rgba(8, 16)) +
            kitty("a=t,f=32,s=8,v=16,i=5", rgba(8, 16)) +
            kitty("a=t,f=32,s=8,v=16,i=6", rgba(8, 16)) +
            kitty("a=t,f=32,s=8,v=16,i=7", rgba(8, 16)) +
            kitty("a=t,f=32,s=8,v=16,i=8", rgba(8, 16)) +
            kitty("a=t,f=32,s=8,v=32,i=9", rgba(8, 32)) +
            kitty("a=t,f=32,s=8,v=96,i=10", rgba(8, 96)) + tt("sb-up", n=4))


@case("kit-evict-stamp", 12, 4)
def _():
    # Kitty images on the screen are evicted oldest transmission first: an
    # old image placed again with a=p keeps its old stamp and goes before a
    # newer one placed earlier
    # (the limit holds two images and image 1's second placement)
    cost = 8 * 16 * 4 + 512
    return (cell() + tt("limit", b=2 * cost + 256) + kt(1, 8, 16, keys=",C=1") +
            cup(1, 4) + kt(2, 8, 16, keys=",C=1") + cup(3, 1) +
            kitty("a=p,i=1,C=1") + cup(1, 8) + kt(3, 8, 16, keys=",C=1") +
            kitty("a=p,i=1") + kitty("a=p,i=2"))


@case("kit-ed2", 12, 4)
def _():
    # ED 2 removes kitty placements and keeps their images, so a=p shows
    # one again; the alternate screen's images go when it is left; RIS
    # empties both stores
    return (cell() + kt(1, 8, 16) + CSI + "2J" + kitty("a=p,i=1,C=1") +
            CSI + "?1049h" + kt(2, 8, 8) + CSI + "?1049l" + kitty("a=p,i=2") +
            cup(3, 1) + kt(3, 8, 8) + ESC + "c" + kitty("a=p,i=3") + kitty("a=p,i=1"))


# The host hook (ADR-103 step 4a, the Notes of 2026-10-24), through
# tsm-replay's fixture host: each request is printed among the replies as a
# "host:" line, and answered from cases/<case>.files/ (see tsm-replay.c).

def rgba_fixture(w, h, px=(255, 0, 0, 255)):
    """A .rgba fixture: width and height, little-endian, then the pixels."""
    return w.to_bytes(4, "little") + h.to_bytes(4, "little") + bytes(px) * (w * h)


@case("kit-host-media", 12, 4)
def _():
    # Each medium reaches the host: t=f with raw f=24 read from the
    # fixture, t=t with f=100 (a PNG the .rgba stands in for), t=s with
    # f=32, O and S, then inline PNG and inline zlib data, buffered and
    # handed over whole (the inflated bytes from .bytes); inline raw data
    # stays in libtsm (no host line)
    FILES["kit-host-media"] = {
        "picture.rgb": rgb(8, 16, (0, 0, 255)),
        "tty-graphics-protocol-pic.png.rgba": rgba_fixture(8, 8, (0, 255, 0, 255)),
        "frame": b"skip" + rgba(8, 8, (255, 255, 0, 255)) + b"tail",
        "direct-1.rgba": rgba_fixture(8, 8, (255, 0, 255, 255)),
        "direct-2.bytes": rgba(8, 8, (0, 255, 255, 255)),
    }
    png = b"\x89PNG\r\n\x1a\n" + bytes(range(40))
    return (cell() + kitty("a=T,t=f,f=24,s=8,v=16,i=1", b"/srv/img/picture.rgb") +
            kitty("a=T,t=t,f=100,i=2", b"/tmp/tty-graphics-protocol-pic.png") +
            kitty("a=T,t=s,f=32,s=8,v=8,O=4,S=256,i=3", b"/frame") +
            chunks("a=T,f=100,i=4", png, 12) +
            kitty("a=T,f=32,s=8,v=8,o=z,i=5", b"x\x9c" + bytes(30)) +
            kitty("a=T,f=24,s=8,v=8,i=6", rgb(8, 8)) + DSR)


@case("kit-host-names", 12, 4)
def _():
    # A name past 4095 bytes, or holding NUL, answers EBADF with no
    # request; a name with a space, a backslash and a newline reaches the
    # host as received, its host line escaping them
    return (cell() + kitty("a=t,t=f,f=100,i=1", b"/" + b"a" * 4095) +
            kitty("a=t,t=f,f=100,i=2", b"/tmp/a\x00b") +
            kitty("a=t,t=f,f=100,i=3", b"/tmp/a b\\c\nd") +
            kitty("a=t,t=f,f=100,i=4", b"/" + b"a" * 4094))


def recorded(path):
    # a stream a client wrote in the probes of ADR-105 and ADR-106, kept in
    # test/conformance/recorded/
    return (Path(__file__).resolve().parent / "recorded" / path).read_bytes().decode("latin-1")


@case("kit-host-mpv", 12, 4)
def _():
    # mpv 0.41.0's shared memory frames (ADR-105): each whole t=s command
    # carries m=1 and no m=0 follows, and each is taken whole, a request of
    # its own; the name has no leading /, which the image helper refuses
    # (UNREADABLE, EBADF), as the fixture does here, and q=2 keeps it quiet
    FILES["kit-host-mpv"] = {"mpv-kitty-0x5645ad83d520.error": b"UNREADABLE\n"}
    return cell() + recorded("mpv-shm-frames.bin") + DSR


@case("kit-host-icat", 12, 4)
def _():
    # kitten icat's detection (ADR-106): a=q direct, a=q with t=t and a=q
    # with t=s, then DA1; the three OK replies come before DA1's. icat's
    # t=t and t=s queries hold 3 bytes and give S as the length of their
    # name (47 and 19), which as kitty's maximum (ADR-107) reads to the
    # end, so each query loads.
    FILES["kit-host-icat"] = {
        "kitty-tty-graphics-protocol-2526776889": rgb(1, 1),
        "icat-KMIKYT5GUOEHE": rgb(1, 1),
    }
    return cell() + recorded("icat-detect.bin")


@case("kit-host-icat-gif", 12, 4)
def _():
    # kitten icat's GIF (ADR-106) after its first frame: a=a, answered
    # EINVAL for its I (no q); a=f with t=s, refused, and handed to the host
    # as a discard (discard=1) so its object is removed, q=2 keeping the
    # refusal quiet
    return cell() + recorded("icat-gif-frames.bin")


@case("kit-host-pause", 12, 4)
def _():
    # The pause: a put right after a file transmission, and text and DSR
    # after it, are parsed only after the host's reply, in order: the put
    # finds the image, its OK follows the transmission's, and the cursor
    # report comes last
    FILES["kit-host-pause"] = {"pic.rgba": rgba_fixture(8, 16)}
    return (cell() + kitty("a=t,t=f,f=100,i=1", b"/tmp/pic") + kitty("a=p,i=1") +
            "ab" + DSR)


@case("kit-host-errors", 12, 4)
def _():
    # The host's outcomes from .error fixtures for t=f PNG (UNREADABLE
    # EBADF, CORRUPT EINVAL, OVER_BUDGET ENOSPC, TOO_LARGE EINVAL); a
    # missing fixture (UNREADABLE, EBADF); a=q through the host storing
    # nothing; a reply past max-graphic-size (EINVAL); a raw f=32 file too
    # short for s x v (TOO_SHORT, EBADF); ENOSPC from the reply's
    # reservation, and from the inline buffer's
    FILES["kit-host-errors"] = {
        "e1.error": b"UNREADABLE\n", "e2.error": b"CORRUPT\n",
        "e3.error": b"OVER_BUDGET\n", "e4.error": b"TOO_LARGE\n",
        "q.rgba": rgba_fixture(1, 1),
        "big.rgba": rgba_fixture(40, 1), "short": rgba(3, 1),
        "huge.rgba": rgba_fixture(64, 64),
    }
    return (cell() + kitty("a=t,t=f,f=100,i=1", b"/e1") +
            kitty("a=t,t=f,f=100,i=2", b"/e2") + kitty("a=t,t=f,f=100,i=3", b"/e3") +
            kitty("a=t,t=f,f=100,i=10", b"/e4") +
            kitty("a=t,t=f,f=100,i=4", b"/none") +
            kitty("a=q,t=f,f=100,i=5", b"/q") + kitty("a=p,i=5") +
            tt("graphic-max", w=32, h=32) + kitty("a=t,t=f,f=100,i=6", b"/big") +
            tt("graphic-max", w=0, h=0) +
            kitty("a=t,t=f,f=32,s=2,v=2,i=7", b"/short") +
            tt("limit", b=8192) + kitty("a=t,t=f,f=100,i=8", b"/huge") +
            kitty("a=t,f=100,i=9", bytes(9000)))


@case("kit-host-requests", 12, 4)
def _():
    # The request each row of ADR-103's What libtsm asks for produces, its
    # host line judged: t=f raw (O and S as offset and read_max, s x v x
    # bpp as min); t=t raw with o=z (inflate); t=s PNG (png, min 0); t=f
    # PNG with o=z and S (inflate,png; S a read bound, no exact); inline
    # raw with o=z (inflate, min s x v x bpp); inline PNG, S ignored;
    # inline PNG with o=z and S=30 (inflate,png,exact, min 30); without S
    # and with S=0, no exact; a refused t=t and a refused t=s handed over
    # as discards (EINVAL); a refused t=f and a refused inline PNG never
    # reaching the host (EINVAL). No fixture exists: the file media answer
    # UNREADABLE (EBADF) and inline data CORRUPT (EINVAL)
    return (cell() + kitty("a=t,t=f,f=24,s=2,v=1,O=5,S=9,i=1", b"/r1") +
            kitty("a=t,t=t,f=32,s=2,v=2,o=z,O=1,S=100,i=2",
                  b"/tmp/tty-graphics-protocol-r2") +
            kitty("a=t,t=s,f=100,O=2,S=50,i=3", b"/r3") +
            kitty("a=t,t=f,f=100,o=z,S=40,i=4", b"/r4") +
            kitty("a=t,f=24,s=2,v=2,o=z,i=5", b"x\x9c") +
            kitty("a=t,f=100,S=99,i=6", b"\x89PNG") +
            kitty("a=t,f=100,o=z,S=30,i=7", b"x\x9c") +
            kitty("a=t,f=100,o=z,i=8", b"x\x9c") +
            kitty("a=t,f=100,o=z,S=0,i=9", b"x\x9c") +
            kitty("a=t,t=t,f=24,i=10", b"/tmp/tty-graphics-protocol-r10") +
            kitty("a=f,t=s,s=1,v=1,i=11", b"/r11") +
            kitty("a=t,t=f,f=24,i=12", b"/r12") +
            kitty("a=t,f=100,i=13,I=1", b"\x89PNG"))


OUTCOMES = ("UNREADABLE", "TOO_SHORT", "CORRUPT", "TOO_LARGE", "OVER_BUDGET")


@case("kit-host-outcomes", 12, 4)
def _():
    # The reply each outcome of ADR-103's What libtsm answers produces, in
    # its three columns: for t=f raw without o=z (i=1 to 5) UNREADABLE and
    # TOO_SHORT are EBADF; with o=z (i=11 to 15) and inline PNG (i=21 to
    # 25) TOO_SHORT is EINVAL; in all three CORRUPT and TOO_LARGE are
    # EINVAL and OVER_BUDGET ENOSPC. t=f PNG's TOO_SHORT (i=6) is EBADF, no
    # inflate. Then OK in each: raw from the file (i=7), inflated from
    # .bytes (i=16), inline PNG from .rgba (i=26), each stored
    files = {}
    out = cell()
    for n, name in enumerate(OUTCOMES):
        files[f"f{n + 1}.error"] = name.encode() + b"\n"
        files[f"z{n + 1}.error"] = name.encode() + b"\n"
        files[f"direct-{n + 1}.error"] = name.encode() + b"\n"
        out += kitty(f"a=t,t=f,f=24,s=1,v=1,i={n + 1}", f"/f{n + 1}".encode())
    for n in range(len(OUTCOMES)):
        out += kitty(f"a=t,t=f,f=24,s=1,v=1,o=z,i={n + 11}", f"/z{n + 1}".encode())
    for n in range(len(OUTCOMES)):
        out += kitty(f"a=t,f=100,i={n + 21}", b"\x89PNG")
    files["p6.error"] = b"TOO_SHORT\n"
    files["f7"] = rgb(1, 1)
    files["z16.bytes"] = rgb(1, 1, (0, 255, 0))
    files["direct-6.rgba"] = rgba_fixture(1, 1, (255, 255, 0, 255))
    FILES["kit-host-outcomes"] = files
    return (out + kitty("a=t,t=f,f=100,i=6", b"/p6") +
            kitty("a=T,t=f,f=24,s=1,v=1,C=1,i=7", b"/f7") +
            kitty("a=T,t=f,f=24,s=1,v=1,o=z,C=1,i=16", b"/z16") +
            kitty("a=T,f=100,C=1,i=26", b"\x89PNG"))


def pix47():
    # 47 distinct bytes, byte i being (37 i + 11) mod 256, as ADR-107's
    # third probe round held on each medium
    return bytes((37 * i + 11) % 256 for i in range(47))


@case("kit-host-lengths", 12, 4)
def _():
    # kitty's length rules (ADR-107), applied by the fixture host as the
    # image helper applies them: S past the end of a 3-byte file reads to
    # the end (OK, i=1); no S reads to the end and takes the first 3 bytes,
    # the excess ignored (OK, i=2, colour of bytes 0 to 2); O=10 with S=3
    # and O=10 alone take bytes 10 to 12 (OK, i=3 and 4); S=2 for 3 bytes,
    # and S=3 for a 2x1 that needs 6, too few (TOO_SHORT, EBADF, i=5 and
    # 6); an offset at the end and one past it (UNREADABLE, EBADF, i=7 and
    # 8); the same on t=t (OK, i=9); inline, too few (EINVAL, i=10) and
    # past s x v (OK, the excess ignored, i=11)
    FILES["kit-host-lengths"] = {
        "pix3": pix47()[:3], "pix47": pix47(),
        "tty-graphics-protocol-pix47": pix47(),
    }
    return (cell() + kitty("a=T,t=f,f=24,s=1,v=1,S=47,C=1,i=1", b"/pix3") +
            kitty("a=T,t=f,f=24,s=1,v=1,C=1,i=2", b"/pix47") +
            kitty("a=T,t=f,f=24,s=1,v=1,O=10,S=3,C=1,i=3", b"/pix47") +
            kitty("a=T,t=f,f=24,s=1,v=1,O=10,C=1,i=4", b"/pix47") +
            kitty("a=T,t=f,f=24,s=1,v=1,S=2,C=1,i=5", b"/pix47") +
            kitty("a=T,t=f,f=24,s=2,v=1,S=3,C=1,i=6", b"/pix47") +
            kitty("a=T,t=f,f=24,s=1,v=1,O=47,C=1,i=7", b"/pix47") +
            kitty("a=T,t=f,f=24,s=1,v=1,O=50,C=1,i=8", b"/pix3") +
            kitty("a=T,t=t,f=24,s=1,v=1,S=47,C=1,i=9",
                  b"/tmp/tty-graphics-protocol-pix47") +
            kitty("a=T,f=24,s=1,v=1,C=1,i=10", pix47()[:2]) +
            kitty("a=T,f=24,s=1,v=1,C=1,i=11", pix47()[:6]))


# Every host case again with the host removed (ADR-103's Testing): t=f,
# t=t and t=s answer EBADF and inline PNG and zlib EINVAL, with no request
# and no discard
for _name in [n for n in CASES if n.startswith("kit-host-")]:
    def _nohook(fn=CASES[_name][2]):
        return tt("host", on=0) + fn()
    CASES[_name + "-nohook"] = (CASES[_name][0], CASES[_name][1], _nohook)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, (cols, rows, fn) in CASES.items():
        (out / f"{name}.bin").write_bytes(fn().encode("utf-8"))
        if name in FILES:
            files = out / f"{name}.files"
            files.mkdir(exist_ok=True)
            for fixture, data in FILES[name].items():
                (files / fixture).write_bytes(data)
        print(name, cols, rows)


if __name__ == "__main__":
    main()
