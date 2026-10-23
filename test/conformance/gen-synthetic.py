#!/usr/bin/env python3
"""Write the synthetic byte streams of the conformance corpus.

Each case exercises one area of terminal behaviour with hand-written escape
sequences. The streams are committed; rerun this script only to add or change
a case, and review the resulting goldens as their own change.

Usage: gen-synthetic.py CASES_DIR
Writes CASES_DIR/<name>.bin and prints one "<name> <cols> <rows>" line per
case, which belongs in cases.list.

SPDX-License-Identifier: MIT
"""

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


# Images (ADR-103 step 3a in aaronsb/kmscon). No protocol makes images yet,
# so these streams make them through tsm-replay's test commands, an APC that
# only a tsm-replay built against the fork's libtsm runs (see tsm-replay.c).
# Each case judges one row of ADR-103's screen-event table for both kinds: a
# sixel, image 1, and a kitty image, image 2, each 24x32 pixels, 3x2 cells of
# 8x16.

def tt(cmd, **kv):
    return APC + "tsm-test;" + cmd + "".join(f";{k}={v}" for k, v in kv.items()) + ST


def img_setup(sb=0):
    out = tt("cell", w=8, h=16)
    if sb:
        out += tt("sb-max", n=sb)
    return (out + tt("image", n=1, w=24, h=32, fill="ff0000ff") +
            tt("image", n=2, w=24, h=32, kitty=1, fill="ff00ff00"))


def put(img, col, row, **kv):
    return tt("place", img=img, col=col, row=row, **kv)


@case("img-print", 12, 4)
def _():
    # Text written over a sixel removes the cells it covers; a kitty
    # placement is untouched.
    return (img_setup() + put(1, 1, 1) + put(2, 5, 1) +
            cup(2, 3) + "xy" + cup(2, 7) + "z")


@case("img-sixel-over", 12, 4)
def _():
    # An opaque sixel removes the parts of older sixels under it; a
    # transparent one leaves them; neither touches a kitty placement.
    return (img_setup() + tt("image", n=3, w=16, h=16, fill="ff00ffff") +
            tt("image", n=4, w=8, h=16, fill="80000080") +
            tt("image", n=5, w=16, h=16, fill="ffffff00") +
            put(1, 1, 1) + put(2, 6, 1) + put(3, 2, 2) + put(4, 1, 1) + put(5, 6, 1))


def erase_case(op):
    # a sixel at columns 2-4 and a kitty image at 7-9, rows 1-2; the erase
    # runs with the cursor at row 1, column 3
    return img_setup() + put(1, 2, 1) + put(2, 7, 1) + cup(2, 4) + op


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
            tt("image", n=4, w=8, h=16, fill="ff00ffff") +
            put(3, 0, 0) + cup(4, 1) + "\n" +
            put(1, 2, 0) + put(2, 7, 0) + put(4, 0, 2) + put(3, 0, 3) +
            "\n" + CSI + "2J" + tt("sb-up", n=2))


@case("img-ed3", 12, 4)
def _():
    # ED 3 drops the scrollback, and the placements anchored on it, even
    # those reaching onto the screen
    return (img_setup(sb=10) + tt("image", n=3, w=8, h=16, kitty=1, fill="ffffffff") +
            tt("image", n=4, w=8, h=16, fill="ff00ffff") +
            put(1, 0, 0) + put(2, 4, 0) + cup(4, 1) + "\n" +
            put(3, 5, 1) + put(4, 0, 2) + CSI + "3J")


@case("img-scroll-up", 12, 4)
def _():
    # LF at the bottom, IND and SU scroll the whole screen: placements move
    # with their lines into scrollback, whole, and the view brings them back
    return (img_setup(sb=10) + put(1, 0, 1) + put(2, 4, 1) +
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
    return (img_setup() + put(1, 0, 1) + put(2, 4, 1) +
            CSI + "T" + cup(1, 1) + ESC + "M" + CSI + "T")


@case("img-scroll-down-sb", 12, 4)
def _():
    # SD with placements reaching from scrollback onto the screen: the sixel
    # splits at the screen's top, its screen part moving down; the kitty
    # placement, not wholly on the screen, stays
    return (img_setup(sb=10) + put(1, 0, 0) + put(2, 4, 0) +
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
            put(1, 0, 1) + put(1, 3, 3) + put(1, 6, 5) +
            put(2, 9, 3) + put(3, 13, 5) + put(4, 14, 1) +
            CSI + "3;6r" + cup(6, 1) + "\n\n")


@case("img-il", 14, 6)
def _():
    # IL scrolls rows 3-5 down from the cursor's line
    return (region_images() + put(1, 0, 2) + put(1, 3, 4) + put(2, 6, 4) +
            tt("image", n=5, w=24, h=32, kitty=1, fill="ffff00ff") + put(5, 9, 2) +
            cup(4, 1) + CSI + "L")


@case("img-dl", 14, 6)
def _():
    # DL scrolls rows 3-5 up from the cursor's line
    return (region_images() + put(1, 0, 2) + put(1, 3, 4) + put(2, 6, 4) +
            tt("image", n=5, w=24, h=32, kitty=1, fill="ffff00ff") + put(5, 9, 2) +
            put(3, 12, 3) + cup(4, 1) + CSI + "M")


@case("img-discard-sb", 12, 3)
def _():
    # Scrollback past its size discards its oldest line: a placement on it
    # loses that row and is anchored on the next line
    return (tt("cell", w=8, h=16) + tt("sb-max", n=2) +
            tt("image", n=1, w=24, h=48, fill="ff0000ff") +
            tt("image", n=2, w=24, h=48, kitty=1, fill="ff00ff00") +
            put(1, 0, 0) + put(2, 4, 0) + cup(3, 1) + "\n\n\n\n" + tt("sb-up", n=2))


@case("img-discard-alt", 12, 4)
def _():
    # The alternate screen has no scrollback: its top line is discarded
    return (CSI + "?1049h" + img_setup() + put(1, 0, 0) + put(2, 4, 0) +
            cup(4, 1) + "\n")


@case("img-resize", 12, 4)
def _():
    # Lines the height change brings back from scrollback carry their
    # placements; a narrower screen keeps them, drawn clipped
    return (img_setup(sb=10) + put(1, 0, 0) + put(2, 6, 0) +
            tt("image", n=3, w=24, h=32, kitty=1, fill="ffffffff") + put(3, 9, 2) +
            cup(4, 1) + "\n\n" + tt("resize", c=10, r=6))


@case("img-resize-shrink", 12, 6)
def _():
    # Lines the height change sends to scrollback carry theirs
    return (img_setup(sb=10) + put(1, 0, 3) + put(2, 4, 3) + cup(6, 1) +
            tt("resize", c=12, r=3))


@case("img-alt-1049", 12, 4)
def _():
    # The alternate screen's store is emptied on entry and on leaving, and
    # the main screen's placements show again
    return (img_setup() + put(1, 0, 0) + put(2, 4, 0) + CSI + "?1049h" +
            img_setup() + put(1, 0, 1) + put(2, 4, 2) + CSI + "?1049l")


@case("img-alt-47", 12, 4)
def _():
    # 47 keeps the alternate screen's text when left, but not its images:
    # entered again, it shows only the sixel placed then
    return (img_setup() + put(1, 0, 0) + CSI + "?47h" + "alt" +
            img_setup() + put(2, 4, 1) + CSI + "?47l" + CSI + "?47h" +
            img_setup() + put(1, 0, 2))


@case("img-alt-1047", 12, 4)
def _():
    return (img_setup() + put(2, 4, 0) + CSI + "?1047h" +
            img_setup() + put(1, 0, 1) + CSI + "?1047l")


@case("img-alt-shown", 12, 4)
def _():
    # On the alternate screen only its own placements show
    return (img_setup() + put(1, 0, 0) + CSI + "?1049h" +
            img_setup() + put(2, 4, 1))


@case("img-ris", 12, 4)
def _():
    # RIS empties both stores; a sixel placed after it shows alone
    return (img_setup(sb=10) + put(1, 0, 0) + cup(4, 1) + "\n" + put(2, 4, 2) +
            CSI + "?1049h" + img_setup() + put(2, 4, 1) + ESC + "c" +
            img_setup() + put(1, 0, 1))


@case("img-decstr", 12, 4)
def _():
    # A soft reset leaves text, so it leaves pictures
    return img_setup() + "text" + put(1, 0, 1) + put(2, 4, 1) + CSI + "!p"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, (cols, rows, fn) in CASES.items():
        (out / f"{name}.bin").write_bytes(fn().encode("utf-8"))
        print(name, cols, rows)


if __name__ == "__main__":
    main()
