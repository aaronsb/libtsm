#!/usr/bin/env python3
"""Fail when tsm-vte.c handles or reports a mode outside the mode table.

Usage:
  modes-source.py TSM-VTE.C

ADR-201 in aaronsb/kmscon. Checks, in the source:

- csi_mode (SM and RM) and mode_report (DECRQM) are, token for token, the
  skeletons below: a loop or switch around one expansion of tsm-modes.def
  per kind, so no case, if or other handling of their own;
- do_csi's 'h' and 'l' cases only call csi_mode, its $p branch only calls
  csi_decrqm, and csi_decrqm answers from mode_report;
- do_csi outside its switch is the preamble below, so nothing on the way to
  the 'h' and 'l' cases handles a mode either (modes-sweep sees a stray
  handler for a number with no row wherever it is, but not one for a row);
- the mode constants (TSM_VTE_MOUSE_MODE_*, TSM_VTE_MOUSE_EVENT_*,
  TSM_VTE_BRACKETED_PASTE) appear only in the accessors (mode_<name>_set,
  mode_<name>_get and the mouse helpers they share) and in
  tsm_vte_handle_mouse, which reads the mouse fields to encode reports.

SPDX-License-Identifier: MIT
"""

import re
import sys

# The bodies, with comments and whitespace removed and each expansion of
# the table (#define MODE ... #undef MODE) shown as <table>.
SKELETON = {
    "csi_mode": (
        "unsignedinti;for(i=0;i<vte->csi_argc;++i){if(vte->csi_argv[i]<0)continue;"
        "if(!(vte->csi_flags&CSI_WHAT)){switch(vte->csi_argv[i]){<table>"
        "default:llog_debug(vte,\"unknownnon-DEC(Re)Set-Mode%d\",vte->csi_argv[i]);continue;}}"
        "switch(vte->csi_argv[i]){<table>"
        "default:llog_debug(vte,\"unknownDEC%set-Mode%d\",set?\"S\":\"Res\",vte->csi_argv[i]);"
        "continue;}}"),
    "mode_report": (
        "if(!dec){switch(number){<table>default:return0;}}switch(number){<table>default:return0;}"),
}
# do_csi up to its switch on the final byte, squeezed the same way, and
# what follows the switch (nothing)
DO_CSI_PREAMBLE = (
    "intnum,x,y,upper,lower;boolprotect;"
    "if(vte->csi_argc<CSI_ARG_MAX)vte->csi_argc++;"
    "if(data!='m'&&csi_has_subargs(vte)){"
    "llog_debug(vte,\"CSI%cwithsub-parametersignored\",data);return;}")
# Each expansion, the same way, by function
EXPANSIONS = {
    "csi_mode": [
        "#defineMODE(kind,number,name,status,...)MODE_ONLY_ANSI_##kind(casenumber:"
        "MODE_TRACE(false,number);MODE_SET_##status(name);continue;)"
        "#include\"tsm-modes.def\"#undefMODE",
        "#defineMODE(kind,number,name,status,...)MODE_ONLY_DEC_##kind(casenumber:"
        "MODE_TRACE(true,number);MODE_SET_##status(name);continue;)"
        "#include\"tsm-modes.def\"#undefMODE",
    ],
    "mode_report": [
        "#defineMODE(kind,number,name,status,dflt,reset,host,...)MODE_ONLY_ANSI_##kind(casenumber:"
        "MODE_REPORT_##status(name,dflt,host))#include\"tsm-modes.def\"#undefMODE",
        "#defineMODE(kind,number,name,status,dflt,reset,host,...)MODE_ONLY_DEC_##kind(casenumber:"
        "MODE_REPORT_##status(name,dflt,host))#include\"tsm-modes.def\"#undefMODE",
    ],
}

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


def squeeze(text):
    return re.sub(r"\s|\\\n", "", re.sub(r"/\*.*?\*/", "", text, flags=re.S))


def closing(text, at):
    """The index of the brace that closes the one at @at, or -1; braces in
    comments, strings and character literals do not count."""
    blank = re.sub(r"/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
                   lambda m: " " * len(m.group(0)), text, flags=re.S)
    depth = 0
    for i in range(at, len(blank)):
        if blank[i] == "{":
            depth += 1
        elif blank[i] == "}":
            depth -= 1
            if not depth:
                return i
    return -1


def skeleton(body):
    """The body as SKELETON spells it, and its expansions in order."""
    inner = body[body.index("{") + 1:body.rindex("}")]
    found = []

    def table(m):
        found.append(squeeze(m.group(0)))
        return "<table>"
    inner = re.sub(r"#define MODE\(.*?#undef MODE", table, inner, flags=re.S)
    return squeeze(inner), found


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
        for m in re.finditer(r"^\s*case\s+([^:]+):", body, re.M):
            errors.append(f"{name} (line {line}) has a case of its own: case {m.group(1).strip()}")
        shape, found = skeleton(body)
        if found != EXPANSIONS[name]:
            errors.append(f"{name} (line {line}): its expansions of tsm-modes.def are not the table's")
        if shape != SKELETON[name]:
            want = SKELETON[name]
            at = next((i for i, (a, b) in enumerate(zip(shape, want)) if a != b),
                      min(len(shape), len(want)))
            errors.append(f"{name} (line {line}) does more than expand the table, at "
                          f"...{shape[max(0, at - 30):at + 50]}...")

    if "do_csi" not in funcs:
        errors.append("do_csi not found")
    else:
        line, body = funcs["do_csi"]
        start = body.find("\n\tswitch (data) {")
        end = closing(body, body.find("{", start)) if start >= 0 else -1
        if end < 0:
            errors.append(f"do_csi (line {line}): its switch (data) not found")
        else:
            before = squeeze(body[body.index("{") + 1:start])
            after = squeeze(body[end + 1:body.rindex("}")])
            if before != DO_CSI_PREAMBLE:
                errors.append(f"do_csi (line {line}) does more before its switch than "
                              f"the preamble: {before}")
            if after:
                errors.append(f"do_csi (line {line}) does more after its switch: {after}")

    for final, call in (("h", "csi_mode(vte,true);break;"), ("l", "csi_mode(vte,false);break;")):
        m = re.search(r"\tcase '%s':(.*?)\n\tcase " % final, src, re.S)
        if not m:
            errors.append(f"do_csi's case '{final}' not found")
        elif squeeze(m.group(1)) != call:
            errors.append(f"do_csi's case '{final}' does more than {call}: {squeeze(m.group(1))}")

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
