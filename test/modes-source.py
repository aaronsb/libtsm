#!/usr/bin/env python3
"""Fail when tsm-vte.c handles or reports a mode outside the mode table.

Usage:
  modes-source.py TSM-VTE.C

ADR-201 in aaronsb/kmscon. Checks, in the source:

- csi_mode (SM and RM) and mode_report (DECRQM) each expand
  tsm-modes.def once per kind, and hold no case label of their own: no
  number and no constant;
- the $p branch of do_csi calls csi_decrqm and nothing else, and
  csi_decrqm answers from mode_report;
- the mode constants (TSM_VTE_MOUSE_MODE_*, TSM_VTE_MOUSE_EVENT_*,
  TSM_VTE_BRACKETED_PASTE) appear only in the accessors (mode_<name>_set,
  mode_<name>_get and the mouse helpers they share) and in
  tsm_vte_handle_mouse, which reads the mouse fields to encode reports.

SPDX-License-Identifier: MIT
"""

import re
import sys

CONSTANT = re.compile(r"\bTSM_VTE_(?:MOUSE_MODE_\w+|MOUSE_EVENT_\w+|BRACKETED_PASTE)\b")
ALLOWED = re.compile(r"^(?:mode_\w+_(?:set|get)|mouse_tracking_set|mouse_event_set|mouse_sgr|tsm_vte_handle_mouse)$")


def functions(src):
    """(name, first line, body) for each function defined at the top level."""
    lines = src.split("\n")
    found = []
    i = 0
    while i < len(lines):
        if lines[i] == "{" and i > 0:
            j = i - 1
            while j > 0 and "(" not in lines[j]:
                j -= 1
            m = re.search(r"(\w+)\s*\(", lines[j])
            k = i + 1
            while k < len(lines) and lines[k] != "}":
                k += 1
            if m:
                found.append((m.group(1), j + 1, "\n".join(lines[i:k + 1])))
            i = k
        i += 1
    return found


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    path = sys.argv[1]
    with open(path, encoding="utf-8") as f:
        src = f.read()
    funcs = {name: (line, body) for name, line, body in functions(src)}
    errors = []

    for name in ("csi_mode", "mode_report"):
        if name not in funcs:
            errors.append(f"{name} not found")
            continue
        line, body = funcs[name]
        if body.count('#include "tsm-modes.def"') != 2:
            errors.append(f"{name} (line {line}) does not expand tsm-modes.def once per kind")
        for m in re.finditer(r"^\s*case\s+([^:]+):", body, re.M):
            errors.append(f"{name} (line {line}) has a case of its own: case {m.group(1).strip()}")

    if "csi_decrqm" not in funcs or "mode_report(" not in funcs["csi_decrqm"][1]:
        errors.append("csi_decrqm does not answer from mode_report")
    cash = re.search(r"if \(vte->csi_flags & CSI_CASH\) \{(.*?)\n\t\t\}", src, re.S)
    if not cash:
        errors.append("the $p branch (CSI_CASH in do_csi) not found")
    else:
        code = re.sub(r"/\*.*?\*/", "", cash.group(1), flags=re.S).split()
        if code != ["csi_decrqm(vte);"]:
            errors.append(f"the $p branch does more than call csi_decrqm: {' '.join(code)}")

    uses = 0
    owner = {}
    for name, line, body in functions(src):
        for n in range(line, line + body.count("\n") + 2):
            owner[n] = name
    for n, text in enumerate(src.split("\n"), 1):
        for m in CONSTANT.finditer(text):
            uses += 1
            fn = owner.get(n)
            if not fn or not ALLOWED.match(fn):
                errors.append(f"line {n}: {m.group(0)} in {fn or 'no function'}, outside the accessors")

    if not uses:
        errors.append("no mode constant found at all; the check is not reading the file it should")
    for e in errors:
        print(f"modes-source: {path}: {e}")
    if errors:
        return 1
    print(f"modes-source: SM, RM and DECRQM expand the mode table; {uses} uses of the mode "
          "constants, all in the accessors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
