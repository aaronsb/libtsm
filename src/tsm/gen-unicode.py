#!/usr/bin/env python3
"""Generate libtsm's Unicode tables from the Unicode Character Database.

Usage: gen-unicode.py [--cache DIR]

Fetches the UCD files below at the pinned version, checks each against its
pinned SHA-256, and writes:

  src/tsm/tsm-unicode-data.h        one property table for every codepoint
  test/tsm-grapheme-break-test.h    the UCD's GraphemeBreakTest cases

relative to the repository root. Both outputs are committed; rerun this only to
move the pin, and commit the new pin and the regenerated tables on their own
(ADR-102 in aaronsb/kmscon). --cache keeps downloaded files in DIR and reuses
them when their hashes match.

The table holds, per codepoint: Grapheme_Cluster_Break, Extended_Pictographic,
Indic_Conjunct_Break, the cell width (0, 1 or 2) and whether the codepoint is a
base of an emoji-style variation sequence (it takes VS16). It is a sorted list
of runs, each entry (first codepoint << 11) | properties, for a binary search.

Width: 0 for General_Category Mn, Me and Cf (except U+00AD SOFT HYPHEN), for
Hangul medial vowels and final consonants (Grapheme_Cluster_Break V and T), and
for controls; 2 for East_Asian_Width W or F, or Emoji_Presentation; else 1.

SPDX-License-Identifier: MIT
"""

import hashlib
import sys
import urllib.request
from pathlib import Path

UNICODE_VERSION = "18.0.0"
BASE = f"https://www.unicode.org/Public/{UNICODE_VERSION}/ucd/"

FILES = {
    "auxiliary/GraphemeBreakProperty.txt":
        "0839dcb79e4ac639ecd538b1abf7c9d22e3f9dd265b7e182d33627aa4d75b45a",
    "auxiliary/GraphemeBreakTest.txt":
        "b0cf047ee94485bbdc846de2b902f5f8a815f6b674f9d04223cddadd91c9df31",
    "emoji/emoji-data.txt":
        "80d00f8e616a0ef27fd6b8de3b758c06383b5d917e2977709578e68baf733bf1",
    "emoji/emoji-variation-sequences.txt":
        "ff1707564aa1f1b2fcf4ec92d609d4cb26940bc0d4dcf07f5328ad6879e84da3",
    "DerivedCoreProperties.txt":
        "09c928886a178fcafd93c29e4bd59073a058e5a100b716d425cb563ab50f68c9",
    "EastAsianWidth.txt":
        "a0cf29eacd00cfcaec4381c6b7c281685f18dbb4e7ff82b4076ccb342ca839aa",
    "extracted/DerivedGeneralCategory.txt":
        "d6b151d2d40ee9b1876d26f417980f45ffae47b6055ccf7203cb31f07a030f94",
}

# Grapheme_Cluster_Break values, in the order of enum tsm_gcb
GCB = ["Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator",
       "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT"]
INCB = ["None", "Linker", "Consonant", "Extend"]

# EastAsianWidth.txt's header: unassigned code points in these ranges are W
EAW_DEFAULT_W = [(0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFAFF),
                 (0x20000, 0x2FFFD), (0x30000, 0x3FFFD)]

NCP = 0x110000
ROOT = Path(__file__).resolve().parents[2]


def fetch(name, cache):
    want = FILES[name]
    if cache:
        cached = cache / Path(name).name
        if cached.exists() and hashlib.sha256(cached.read_bytes()).hexdigest() == want:
            return cached.read_text(encoding="utf-8")
    with urllib.request.urlopen(BASE + name, timeout=60) as resp:
        data = resp.read()
    got = hashlib.sha256(data).hexdigest()
    if got != want:
        sys.exit(f"gen-unicode: {name}: sha256 {got}, pinned {want}")
    if cache:
        cache.mkdir(parents=True, exist_ok=True)
        (cache / Path(name).name).write_bytes(data)
    return data.decode("utf-8")


def records(text):
    """Yield (first, last, fields) for each data line of a UCD file."""
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(";")]
        cps = fields[0].split("..")
        first = int(cps[0], 16)
        last = int(cps[-1], 16)
        yield first, last, fields[1:]


def build(cache):
    gcb = bytearray(NCP)           # 0 = Other
    gc_zero = bytearray(NCP)       # Mn, Me, Cf, Cc
    eaw_wide = bytearray(NCP)
    for first, last in EAW_DEFAULT_W:
        eaw_wide[first:last + 1] = b"\1" * (last - first + 1)
    pict = bytearray(NCP)
    epres = bytearray(NCP)
    incb = bytearray(NCP)
    vsbase = bytearray(NCP)

    for first, last, f in records(fetch("auxiliary/GraphemeBreakProperty.txt", cache)):
        v = GCB.index(f[0])
        gcb[first:last + 1] = bytes([v]) * (last - first + 1)

    for first, last, f in records(fetch("extracted/DerivedGeneralCategory.txt", cache)):
        if f[0] in ("Mn", "Me", "Cf", "Cc"):
            gc_zero[first:last + 1] = b"\1" * (last - first + 1)
    gc_zero[0xAD] = 0

    for first, last, f in records(fetch("EastAsianWidth.txt", cache)):
        v = 1 if f[0] in ("W", "F") else 0
        eaw_wide[first:last + 1] = bytes([v]) * (last - first + 1)

    for first, last, f in records(fetch("emoji/emoji-data.txt", cache)):
        if f[0] == "Extended_Pictographic":
            pict[first:last + 1] = b"\1" * (last - first + 1)
        elif f[0] == "Emoji_Presentation":
            epres[first:last + 1] = b"\1" * (last - first + 1)

    for first, last, f in records(fetch("DerivedCoreProperties.txt", cache)):
        if f[0] == "InCB":
            v = INCB.index(f[1])
            incb[first:last + 1] = bytes([v]) * (last - first + 1)

    for line in fetch("emoji/emoji-variation-sequences.txt", cache).splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        seq = line.split(";")[0].split()
        if len(seq) == 2 and seq[1] == "FE0F":
            vsbase[int(seq[0], 16)] = 1

    props = []
    v_t = (GCB.index("V"), GCB.index("T"))
    for cp in range(NCP):
        if gc_zero[cp] or gcb[cp] in v_t:
            width = 0
        elif eaw_wide[cp] or epres[cp]:
            width = 2
        else:
            width = 1
        props.append(gcb[cp] | pict[cp] << 4 | incb[cp] << 5 | width << 7 | vsbase[cp] << 9)

    runs = []
    for cp in range(NCP):
        if not runs or props[cp] != props[runs[-1]]:
            runs.append(cp)
    return props, runs


def write_table(props, runs):
    out = ROOT / "src/tsm/tsm-unicode-data.h"
    lines = [
        "/* Generated by src/tsm/gen-unicode.py from the Unicode Character",
        f" * Database {UNICODE_VERSION}. Do not edit; rerun the generator. */",
        "/* SPDX-License-Identifier: MIT */",
        "",
        f'#define TSM_UNICODE_VERSION "{UNICODE_VERSION}"',
        "",
        "/* Grapheme_Cluster_Break */",
        "enum tsm_gcb {",
    ]
    lines += [f"\tTSM_GCB_{name.upper()}," for name in GCB]
    lines += [
        "};",
        "",
        "/* Indic_Conjunct_Break */",
        "enum tsm_incb {",
    ]
    lines += [f"\tTSM_INCB_{name.upper()}," for name in INCB]
    lines += [
        "};",
        "",
        "/* A codepoint's properties: bits 0-3 Grapheme_Cluster_Break, bit 4",
        " * Extended_Pictographic, bits 5-6 Indic_Conjunct_Break, bits 7-8 the cell",
        " * width, bit 9 a base of an emoji-style variation sequence. */",
        "#define TSM_UPROP_GCB(p)\t((p) & 0xf)",
        "#define TSM_UPROP_PICT(p)\t(((p) >> 4) & 1)",
        "#define TSM_UPROP_INCB(p)\t(((p) >> 5) & 3)",
        "#define TSM_UPROP_WIDTH(p)\t(((p) >> 7) & 3)",
        "#define TSM_UPROP_VSBASE(p)\t(((p) >> 9) & 1)",
        "",
        "/* Runs of equal properties: (first codepoint << 11) | properties, sorted */",
        f"static const uint32_t tsm_unicode_runs[{len(runs)}] = {{",
    ]
    row = []
    for cp in runs:
        row.append(f"0x{cp << 11 | props[cp]:08x},")
        if len(row) == 6:
            lines.append("\t" + " ".join(row))
            row = []
    if row:
        lines.append("\t" + " ".join(row))
    lines += ["};", ""]
    out.write_text("\n".join(lines))
    return out


def write_break_test(cache):
    out = ROOT / "test/tsm-grapheme-break-test.h"
    cases = []
    for line in fetch("auxiliary/GraphemeBreakTest.txt", cache).splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        tokens = line.split()
        cps, breaks = [], []
        for i in range(1, len(tokens) - 1, 2):
            cps.append(int(tokens[i], 16))
            breaks.append(1 if tokens[i - 1] == "÷" else 0)
        cases.append((cps, breaks))
    maxlen = max(len(c) for c, _ in cases)
    lines = [
        "/* Generated by src/tsm/gen-unicode.py from GraphemeBreakTest.txt,",
        f" * Unicode {UNICODE_VERSION}. Do not edit. */",
        "/* SPDX-License-Identifier: MIT */",
        "",
        f"#define GRAPHEME_BREAK_TEST_MAX {maxlen}",
        "",
        "/* brk[i]: a boundary before cps[i] (brk[0] is always 1) */",
        "static const struct grapheme_break_test {",
        "\tunsigned int len;",
        "\tuint32_t cps[GRAPHEME_BREAK_TEST_MAX];",
        "\tuint8_t brk[GRAPHEME_BREAK_TEST_MAX];",
        f"}} grapheme_break_tests[{len(cases)}] = {{",
    ]
    for cps, breaks in cases:
        lines.append("\t{ %d, { %s }, { %s } }," % (
            len(cps), ", ".join(f"0x{c:x}" for c in cps), ", ".join(map(str, breaks))))
    lines += ["};", ""]
    out.write_text("\n".join(lines))
    return out, len(cases)


def main():
    args = sys.argv[1:]
    cache = None
    if args[:1] == ["--cache"] and len(args) == 2:
        cache = Path(args[1])
    elif args:
        sys.exit(__doc__)
    props, runs = build(cache)
    table = write_table(props, runs)
    test, ncases = write_break_test(cache)
    print(f"{table.relative_to(ROOT)}: Unicode {UNICODE_VERSION}, {len(runs)} runs, "
          f"{len(runs) * 4} bytes")
    print(f"{test.relative_to(ROOT)}: {ncases} cases")


if __name__ == "__main__":
    main()
