#!/usr/bin/env python3
"""Fail when fork/modes.md or fork/modes.tsv differs from the mode table.

Usage:
  modes-doc.py GEN-MODES DIR

GEN-MODES is the built test/gen-modes, which prints both files from
src/tsm/tsm-modes.def (ADR-201 in aaronsb/kmscon); DIR holds the committed
modes.md and modes.tsv. `make modes` rewrites them.

SPDX-License-Identifier: MIT
"""

import difflib
import os
import subprocess
import sys


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    gen, directory = sys.argv[1], sys.argv[2]
    stale = []
    for fmt, name in (("md", "modes.md"), ("tsv", "modes.tsv")):
        want = subprocess.run([gen, fmt], check=True, capture_output=True,
                              text=True).stdout
        path = os.path.join(directory, name)
        try:
            with open(path, encoding="utf-8") as f:
                have = f.read()
        except FileNotFoundError:
            have = ""
        if have != want:
            stale.append(name)
            sys.stdout.writelines(difflib.unified_diff(
                have.splitlines(keepends=True), want.splitlines(keepends=True),
                "committed/" + name, "generated/" + name))
    if stale:
        print(f"modes-doc: {', '.join(stale)} differ from src/tsm/tsm-modes.def; "
              "run make modes and commit the result")
        return 1
    print("modes-doc: modes.md and modes.tsv match the mode table")
    return 0


if __name__ == "__main__":
    sys.exit(main())
