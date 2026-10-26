#!/usr/bin/env python3
"""Fail when a mode's row names no test, or a test that does not set and reset it.

Usage:
  modes-tests.py [--no-images] GEN-MODES TEST-DIR TSM-REPLAY CONFORMANCE-DIR BINARY...

ADR-201 in aaronsb/kmscon. Reads the mode table through GEN-MODES (its tsv
output) and checks each row's tests column:

- every SUPPORTED row names at least one test;
- every test a SUPPORTED or PARTIAL row names exists: a START_TEST in a .c
  file under TEST-DIR, or a case in CONFORMANCE-DIR/cases.list;
- every SUPPORTED row is set and reset, through SM or RM, by one test it
  names. Each named unit test's BINARY (named after its .c file) and each
  named tier 1 case through TSM-REPLAY runs with TSM_MODE_TRACE set; the test
  build appends each row's set and reset to that file with the running
  test's name.

With --no-images, for a libtsm built with -Dimages=false (ADR-103), the
rows of sixel's (host sixel) are left out of the last check: their tests are
sixel's, which that build does not have. The count of rows left out is
printed.

SPDX-License-Identifier: MIT
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def table(gen):
    out = subprocess.run([gen, "tsv"], check=True, capture_output=True,
                         text=True).stdout.splitlines()
    head = out[0].split("\t")
    return [dict(zip(head, line.split("\t"))) for line in out[1:]]


def unit_tests(test_dir):
    """START_TEST name -> the .c file's stem, which names its binary."""
    found = {}
    for path in sorted(Path(test_dir).rglob("*.c")):
        for name in re.findall(r"^START_TEST\((\w+)\)", path.read_text(errors="replace"), re.M):
            found[name] = path.stem
    return found


def cases(conf_dir):
    found = {}
    for line in (Path(conf_dir) / "cases.list").read_text().splitlines():
        line = line.split("#", 1)[0].split()
        if len(line) == 3:
            found[line[0]] = (line[1], line[2])
    return found


def main():
    args = sys.argv[1:]
    no_images = bool(args) and args[0] == "--no-images"
    if no_images:
        args = args[1:]
    if len(args) < 5:
        print(__doc__, file=sys.stderr)
        return 2
    gen, test_dir, replay, conf_dir = args[:4]
    binaries = {Path(b).name: b for b in args[4:]}

    rows = table(gen)
    units = unit_tests(test_dir)
    tier1 = cases(conf_dir)
    errors = []
    named = {}
    sixel_rows = [r for r in rows if no_images and r["host"] == "sixel"]
    if no_images and not sixel_rows:
        errors.append("--no-images, but no row's host is sixel")
    for r in rows:
        label = f"{r['kind']} {r['number']} {r['name']} ({r['status']})"
        tests = r["tests"].split()
        if r["status"] == "supported" and not tests:
            errors.append(f"{label} names no test")
        if r["status"] in ("supported", "partial"):
            for t in tests:
                if t not in units and t not in tier1:
                    errors.append(f"{label} names {t}, which is neither a START_TEST nor a tier 1 case")
                if r not in sixel_rows:
                    named.setdefault(t, []).append(r)

    with tempfile.TemporaryDirectory() as tmp:
        trace = os.path.join(tmp, "trace.tsv")
        env = dict(os.environ, TSM_MODE_TRACE=trace)
        for binary in sorted({units[t] for t in named if t in units}):
            if binary not in binaries:
                errors.append(f"no binary {binary} given for the tests in {binary}.c")
                continue
            subprocess.run([binaries[binary]], env=env, capture_output=True)
        for case in sorted(t for t in named if t in tier1 and t not in units):
            cols, nrows = tier1[case]
            subprocess.run([replay, cols, nrows, str(Path(conf_dir) / "cases" / f"{case}.bin")],
                           env=dict(env, TSM_MODE_TRACE_TEST=case), capture_output=True)
        seen = set()
        if os.path.exists(trace):
            with open(trace) as f:
                for line in f:
                    parts = line.rstrip("\n").split("\t")
                    if len(parts) == 4:
                        seen.add(tuple(parts))

    if not seen:
        errors.append("the traced runs recorded no set or reset at all; the trace is broken")

    checked = 0
    for r in rows:
        if r["status"] != "supported" or r in sixel_rows:
            continue
        checked += 1
        tests = r["tests"].split()
        if not any((t, r["kind"], r["number"], "set") in seen and
                   (t, r["kind"], r["number"], "reset") in seen for t in tests):
            errors.append(f"{r['kind']} {r['number']} {r['name']} (supported): no test it names "
                          f"({', '.join(tests) or 'none'}) both sets and resets it")

    for e in errors:
        print(f"modes-tests: {e}")
    if errors:
        return 1
    print(f"modes-tests: {checked} supported rows each set and reset by a test they name; "
          f"{len(seen)} distinct traced sets and resets"
          + (f"; {len(sixel_rows)} rows of sixel's left out, a build without images"
             if no_images else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
