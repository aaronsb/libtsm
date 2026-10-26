#!/usr/bin/env python3
"""Check tsm-replay and the goldens against INTERFACE.md.

Usage: interface-check.py [--no-images] SPEC REPLAY DIR

SPEC is INTERFACE.md, whose fenced blocks this reads; REPLAY is tsm-replay;
DIR holds cases.list, golden/, golden-draw2/ and golden-frame/. Checks:

1. REPLAY --describe prints the spec's describe block. With --no-images, for
   a libtsm built with -Dimages=false, it prints that block without the
   image features (IMAGE_FEATURES) and their commands.
2. Every line of every golden matches the grammar block for its view, and
   every golden file the grammar's structure and the numbering rules of the
   spec's output section.
3. Every worked example replays to the output the spec shows, except one
   that needs a feature REPLAY does not declare, which is counted.
4. Each palette in the palettes block is the colours REPLAY's frame view
   resolves the sixteen colours and the defaults to under that palette.
5. Malformed and edge inputs exit as the spec gives (check_refusals), and
   the grammar rejects the outputs in BAD and accepts GOOD, with its
   reserved keys accepted only when expanded.

SPDX-License-Identifier: MIT
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

# the features a libtsm built with -Dimages=false cannot have
IMAGE_FEATURES = {"images", "sixel", "kitty-graphics", "image-host"}
VIEWS = {"screen": ("golden", None), "draw2": ("golden-draw2", "--draw2"),
         "frame": ("golden-frame", "--frame")}


def blocks(spec):
    """[(info words, body)] for every fenced block of the spec."""
    return [(m.group(1).split(), m.group(2))
            for m in re.finditer(r"^```([^\n`]*)\n(.*?)^```$", spec, re.M | re.S)]


def block(spec, name):
    found = [body for info, body in blocks(spec) if info[:1] == [name]]
    if len(found) != 1:
        sys.exit(f"replay-interface: the spec has {len(found)} {name} blocks, not one")
    return found[0]


class Grammar:
    """The grammar block: fragments, line forms by kind and view, structures."""

    def __init__(self, text, reserved=False):
        self.defs, self.lines, self.structure = {}, {}, {}
        for raw in text.splitlines():
            if not raw or raw.startswith("#"):
                continue
            word, rest = raw.split(" ", 1)
            if word in ("define", "reserved"):
                name, rx = rest.split(" ", 1)
                self.defs[name] = self.expand(rx) if word == "define" or reserved else ""
            elif word == "line":
                kind, views, rx = rest.split(" ", 2)
                for view in VIEWS if views == "*" else views.split(","):
                    self.lines.setdefault((kind, view), []).append(re.compile(self.expand(rx)))
            elif word == "structure":
                view, rx = rest.split(" ", 1)
                self.structure[view] = re.compile(self.expand(rx))
            else:
                sys.exit(f"replay-interface: grammar line {raw!r}")

    def expand(self, rx):
        return re.sub(r"\{([A-Z][A-Z0-9]*)\}", lambda m: f"(?:{self.defs[m.group(1)]})", rx)

    def check(self, view, text, cols, rows):
        """The first fault of @text, the output of @view at @cols x @rows, or None."""
        lines = text.split("\n")
        if lines[-1] != "":
            return "does not end in a newline"
        kinds = ""
        for n, line in enumerate(lines[:-1], 1):
            kind = line.split(" ", 1)[0]
            forms = self.lines.get((kind, view))
            if not forms:
                return f"line {n}: no line of kind {kind!r} in the {view} view: {line!r}"
            if not any(f.fullmatch(line) for f in forms):
                return f"line {n}: not a {kind} line: {line!r}"
            kinds += " " + kind
        if not self.structure[view].fullmatch(kinds):
            return "lines out of the order the structure gives"
        return numbering(lines[:-1], view, cols, rows)


def numbering(lines, view, cols, rows):
    """
    The first break of the spec's numbering rules, or None. The screen view
    prints the @cols x @rows of the command line; the others the size line's.
    """
    row = None
    last = (-1, None)
    checkpoint = -1
    placement = image = 0

    def unfinished():
        return row is not None and row != rows - 1

    for n, line in enumerate(lines, 1):
        f = line.split(" ")
        if f[0] in ("checkpoint", "size", "test") and unfinished():
            return f"line {n}: a block ends after row {row} of {rows}"
        if f[0] == "checkpoint":
            if int(f[1]) < checkpoint:
                return f"line {n}: checkpoint before the one above it"
            checkpoint = int(f[1])
        elif f[0] == "size":
            if view != "screen":
                cols, rows = map(int, f[1].split("x"))
            row, placement, image = -1, 0, 0
        elif f[0] == "row":
            row += 1
            if int(f[1]) != row or row >= rows:
                return f"line {n}: row {f[1]} where row {row} of {rows} belongs"
            last = (-1, None)
        elif f[0] in ("attr", "cells"):
            a, b = map(int, f[2].split("-"))
            if int(f[1]) != row or a <= last[0] or b < a or b >= cols:
                return f"line {n}: run {f[2]} out of order or off row {row} of {cols} cells"
            if a == last[0] + 1 and f[3:] == last[1]:
                return f"line {n}: run {f[2]} continues the run before it"
            last = (b, f[3:])
        elif f[0] == "placement":
            if int(f[1]) != placement:
                return f"line {n}: placement {f[1]} where {placement} belongs"
            placement += 1
        elif f[0] == "image":
            if int(f[1]) != image:
                return f"line {n}: image {f[1]} where {image} belongs"
            image += 1
        if f[0] in ("placement", "image") and row != rows - 1:
            return f"line {n}: {f[0]} before the last row"
    if unfinished():
        return f"the output ends after row {row} of {rows}"
    return None


# Outputs the grammar must reject, with the view and size each is checked
# at, so that a check grown too loose fails on every run; and one it must
# accept, so that one grown too strict does.
GOOD = "size 4x2\ncursor 0,0\nflags none\nrow 0 |ab|\nattr 0 0-1 bold\nrow 1 ||\n"
BAD = [
    ("cursor X Y", GOOD.replace("cursor 0,0", "cursor 0 0")),
    ("a missing row", GOOD.replace("row 1 ||\n", "")),
    ("a row past the size", GOOD + "row 2 ||\n"),
    ("a split run", GOOD.replace("attr 0 0-1 bold", "attr 0 0-0 bold\nattr 0 1-1 bold")),
    ("keys out of order", GOOD.replace("attr 0 0-1 bold", "attr 0 0-1 bold fg=1")),
    ("a run past the columns", GOOD.replace("attr 0 0-1", "attr 0 3-4")),
    ("a reserved key", GOOD.replace("attr 0 0-1 bold", "attr 0 0-1 strike overline")),
    ("a frame line", GOOD.replace("flags none\n", "flags none\nchanged 8\n")),
    ("flags out of order", GOOD.replace("flags none", "flags AUTO_WRAP|INSERT_MODE")),
    ("a colour code past 15", GOOD.replace("attr 0 0-1 bold", "attr 0 0-1 fg=16")),
    ("no final newline", GOOD[:-1]),
    ("a block cut short", "checkpoint 0\n" + GOOD.replace("row 1 ||\n", "") + "checkpoint 1\n" + GOOD),
]


def check_controls(grammar_text):
    faults = []
    grammar = Grammar(grammar_text)
    if grammar.check("screen", GOOD, 4, 2):
        faults.append(f"control: the grammar rejects a good output: {grammar.check('screen', GOOD, 4, 2)}")
    for what, text in BAD:
        if not grammar.check("screen", text, 4, 2):
            faults.append(f"control: the grammar accepts {what}")
    reserved = GOOD.replace("attr 0 0-1 bold", "attr 0 0-1 strike overline")
    if Grammar(grammar_text, reserved=True).check("screen", reserved, 4, 2):
        faults.append("control: the reserved grammar rejects a reserved key")
    return len(BAD) + 2, faults


def unescape(s):
    """The bytes of a quoted string in the examples' notation."""
    out, i = bytearray(), 0
    simple = {"e": b"\x1b", "r": b"\r", "n": b"\n", "t": b"\t", "\\": b"\\", '"': b'"'}
    while i < len(s):
        if s[i] != "\\":
            out += s[i].encode("utf-8")
            i += 1
        elif s[i + 1] in simple:
            out += simple[s[i + 1]]
            i += 2
        elif s[i + 1] == "x":
            out.append(int(s[i + 2:i + 4], 16))
            i += 4
        else:
            raise ValueError(f"unknown escape \\{s[i + 1]}")
    return bytes(out)


def run(replay, view_flag, cols, rows, data, cuts):
    with tempfile.TemporaryDirectory() as tmp:
        stream = Path(tmp) / "example.bin"
        stream.write_bytes(data)
        cmd = [replay] + ([view_flag] if view_flag else []) + [str(cols), str(rows), str(stream)]
        if cuts:
            (Path(tmp) / "example.cuts").write_text("".join(f"{c}\n" for c in cuts))
            cmd.append(str(Path(tmp) / "example.cuts"))
        result = subprocess.run(cmd, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"exited {result.returncode}: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout.decode("utf-8", errors="replace")


def example(body):
    """size, view, needed features, stream and cuts of an example block."""
    size, view, needs, data, cuts = None, "screen", set(), b"", []
    for line in body.splitlines():
        word, _, rest = line.partition(" ")
        if word == "size":
            size = tuple(int(v) for v in rest.split())
        elif word == "view":
            view = rest
        elif word == "needs":
            needs.update(rest.split())
        elif word == "send":
            if not (rest.startswith('"') and rest.endswith('"')):
                raise ValueError(f"send takes a quoted string: {line!r}")
            data += unescape(rest[1:-1])
        elif word == "cmd":
            data += b"\x1b_tsm-test;" + rest.encode("utf-8") + b"\x1b\\"
        elif word == "check":
            cuts.append(len(data))
        else:
            raise ValueError(f"example line {line!r}")
    return size, view, needs, data, cuts


def check_palettes(spec, replay):
    faults = []
    rows = [line.split() for line in block(spec, "palettes").splitlines() if line]
    for name, *colours in rows:
        if len(colours) != 18:
            faults.append(f"palette {name}: {len(colours)} colours, not 18")
            continue
        data = (b"\x1b_tsm-test;palette;name=" + name.encode() + b"\x1b\\" +
                b"".join(b"\x1b[%dmx" % (30 + i if i < 8 else 82 + i) for i in range(16)))
        out = run(replay, "--frame", 16, 1, data, [])
        pal = re.search(r"^palette (\S+) fg=#(\w+) bg=#(\w+)$", out, re.M)
        fg = {i: pal.group(2) for i in range(16)}
        for m in re.finditer(r"^cells 0 (\d+)-(\d+) w=1 fg=#(\w+) ", out, re.M):
            for i in range(int(m.group(1)), int(m.group(2)) + 1):
                fg[i] = m.group(3)
        got = [fg[i] for i in range(16)] + [pal.group(2), pal.group(3)]
        if pal.group(1) != name or got != colours:
            faults.append(f"palette {name}: the frame view resolves {pal.group(1)} "
                          f"{' '.join(got)}")
    return len(rows), faults


def check_refusals(replay):
    """The exit status the spec gives each malformed or edge input."""
    intro = b"\x1b_tsm-test;"

    def cmd(text):
        return intro + text + b"\x1b\\"

    cases = [
        # (what, cols, rows, stream, cuts text, exit)
        ("COLS 0", "0", "2", b"", None, 2),
        ("COLS 2049", "2049", "2", b"", None, 2),
        ("ROWS 1025", "4", "1025", b"", None, 2),
        ("COLS with a leading zero", "04", "2", b"", None, 2),
        ("COLS with a sign", "+4", "2", b"", None, 2),
        ("COLS 2048", "2048", "1", b"", None, 0),
        ("ROWS 1024", "1", "1024", b"", None, 0),
        ("a CUTS line with a letter", "4", "2", b"ab", "1x\n", 2),
        ("a CUTS line with a space", "4", "2", b"ab", " 1\n", 2),
        ("a cut inside an introducer", "4", "2", b"a" + cmd(b"sb-reset"), "4\n", 2),
        ("a cut inside a command", "4", "2", b"a" + cmd(b"sb-reset"), "13\n", 2),
        ("a cut before a command", "4", "2", b"a" + cmd(b"sb-reset"), "1\n", 0),
        ("a command with no ST", "4", "2", intro + b"sb-reset", None, 2),
        ("a tab in a command", "4", "2", cmd(b"sb-up;n=1\t"), None, 2),
        ("a leading ;", "4", "2", cmd(b";sb-reset"), None, 2),
        ("= inside a value", "4", "2", cmd(b"sb-up;n=1=2"), None, 2),
        ("a decimal leading zero", "4", "2", cmd(b"sb-up;n=01"), None, 2),
        ("a decimal sign", "4", "2", cmd(b"sb-up;n=+1"), None, 2),
        ("a decimal 0", "4", "2", cmd(b"sb-up;n=0"), None, 0),
        ("a signed -0", "4", "2", cmd(b"place;z=-0"), None, 2),
        ("a signed leading zero", "4", "2", cmd(b"place;z=-01"), None, 2),
        ("an empty CUTS file", "4", "2", b"ab", "", 0),
        ("an unknown command", "4", "2", cmd(b"no-such"), None, 2),
        ("an unknown key", "4", "2", cmd(b"sb-up;m=1"), None, 2),
        ("a key given twice", "4", "2", cmd(b"sb-up;n=1;n=1"), None, 2),
        ("a missing required key", "4", "2", cmd(b"resize;c=4"), None, 2),
        ("resize to 0 columns", "4", "2", cmd(b"resize;c=0;r=2"), None, 2),
        ("resize to 2049 columns", "4", "2", cmd(b"resize;c=2049;r=2"), None, 2),
        ("resize to 1025 rows", "4", "2", cmd(b"resize;c=4;r=1025"), None, 2),
        ("resize within bounds", "4", "2", cmd(b"resize;c=6;r=3"), None, 0),
        ("a caller past 7", "4", "2", cmd(b"frame-since;caller=8"), None, 2),
    ]
    faults = []
    with tempfile.TemporaryDirectory() as tmp:
        for what, cols, rows, data, cuts, want in cases:
            stream = Path(tmp) / "refusal.bin"
            stream.write_bytes(data)
            argv = [replay, cols, rows, str(stream)]
            if cuts is not None:
                (Path(tmp) / "refusal.cuts").write_text(cuts)
                argv.append(str(Path(tmp) / "refusal.cuts"))
            got = subprocess.run(argv, capture_output=True).returncode
            if got != want:
                faults.append(f"refusal: {what} exits {got}, not {want}")
    return len(cases), faults


def main():
    args = sys.argv[1:]
    no_images = "--no-images" in args
    args = [a for a in args if a != "--no-images"]
    if len(args) != 3:
        sys.exit(__doc__)
    spec, replay, root = Path(args[0]).read_text(), args[1], Path(args[2])
    faults = []

    # 1. --describe
    want = block(spec, "describe").splitlines()
    if no_images:
        want = [l for l in want if not (l.split()[0] == "feature" and l.split()[1] in IMAGE_FEATURES)
                and not (l.split()[0] == "command" and l.split()[-1] in IMAGE_FEATURES)]
    describe = subprocess.run([replay, "--describe"], capture_output=True, text=True)
    if describe.returncode:
        faults.append(f"--describe exits {describe.returncode}")
    got = describe.stdout.splitlines()
    for line in want:
        if line not in got:
            faults.append(f"--describe lacks the spec's line {line!r}")
    for line in got:
        if line not in want:
            faults.append(f"--describe prints {line!r}, which the spec lacks")
    if not faults and got != want:
        faults.append("--describe prints the spec's lines in another order")
    commands = sum(1 for l in got if l.startswith("command "))

    # 2. the goldens under the grammar
    grammar = Grammar(block(spec, "grammar"))
    sizes = {}
    for line in (root / "cases.list").read_text().splitlines():
        fields = line.split("#", 1)[0].split()
        if fields:
            sizes[fields[0]] = (int(fields[1]), int(fields[2]))
    goldens = 0
    for view, (directory, _) in VIEWS.items():
        files = sorted((root / directory).glob("*.txt"))
        if not files:
            faults.append(f"no goldens in {directory}/")
        for path in files:
            goldens += 1
            if path.stem not in sizes:
                faults.append(f"{directory}/{path.name}: no case in cases.list")
                continue
            fault = grammar.check(view, path.read_text(), *sizes[path.stem])
            if fault:
                faults.append(f"{directory}/{path.name}: {fault}")

    # 3. the worked examples
    outputs = {info[1]: body for info, body in blocks(spec)
               if info[:1] == ["output"] and len(info) == 2}
    examples = [(info[1], body) for info, body in blocks(spec)
                if info[:1] == ["example"] and len(info) == 2]
    declared = {l.split()[1] for l in got if l.startswith("feature ")}
    ran = skipped = 0
    for name in set(outputs) - {name for name, _ in examples}:
        faults.append(f"output {name}: no example block")
    for name, body in examples:
        try:
            size, view, needs, data, cuts = example(body)
            if needs - declared:
                skipped += 1
                continue
            out = run(replay, VIEWS[view][1], size[0], size[1], data, cuts)
        except (ValueError, RuntimeError, TypeError, KeyError) as e:
            faults.append(f"example {name}: {e}")
            continue
        ran += 1
        if name not in outputs:
            faults.append(f"example {name}: no output block")
        elif out != outputs[name]:
            faults.append(f"example {name}: replays to\n{out}")
        fault = grammar.check(view, out, *size)
        if fault:
            faults.append(f"example {name}: {fault}")

    if not ran:
        faults.append("no worked example ran")

    # 4. the palettes
    palettes, palette_faults = check_palettes(spec, replay)
    faults += palette_faults

    # 5. the refusals, and the grammar's own controls
    refusals, refusal_faults = check_refusals(replay)
    faults += refusal_faults
    controls, control_faults = check_controls(block(spec, "grammar"))
    faults += control_faults

    for fault in faults:
        print(f"replay-interface: {fault}")
    if faults:
        return 1
    print(f"replay-interface: --describe matches the spec ({commands} commands); "
          f"{goldens} goldens parse; {ran} examples replay as shown"
          + (f", {skipped} left out for features not declared" if skipped else "")
          + f"; {palettes} palettes match; {refusals} refusals exit as given; "
          f"{controls} grammar controls hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
