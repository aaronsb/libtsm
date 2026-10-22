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
