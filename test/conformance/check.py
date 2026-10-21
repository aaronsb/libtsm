#!/usr/bin/env python3
"""Replay every conformance case and compare it with its golden.

Usage:
  check.py REPLAY DIR [--report FILE]   compare; exit 1 on any difference
  check.py REPLAY DIR --update          rewrite the goldens from this REPLAY

--report writes the run as JSON for the contact sheet: for each case its
name, size, status (pass, fail or missing), golden and actual output. The
sheet is drawn from the same run that gives the verdict, so the two cannot
disagree.

DIR holds cases.list ("<name> <cols> <rows>" per line, # comments allowed),
cases/<name>.bin, an optional cases/<name>.cuts of checkpoint offsets, and
golden/<name>.txt.

--update is for producing goldens from a reference build (make-golden.sh);
accepting a changed golden is a reviewed change of its own (ADR-301).

SPDX-License-Identifier: MIT
"""

import difflib
import json
import subprocess
import sys
from pathlib import Path


def load_cases(root):
    cases = []
    for line in (root / "cases.list").read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, cols, rows = line.split()
        cases.append((name, cols, rows))
    return cases


def replay(tool, root, name, cols, rows):
    cmd = [tool, cols, rows, str(root / "cases" / f"{name}.bin")]
    cuts = root / "cases" / f"{name}.cuts"
    if cuts.exists():
        cmd.append(str(cuts))
    result = subprocess.run(cmd, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(f"{name}: tsm-replay exited {result.returncode}: "
                           f"{result.stderr.decode(errors='replace').strip()}")
    return result.stdout.decode("utf-8", errors="replace")


def main():
    args = sys.argv[1:]
    update = "--update" in args
    args = [a for a in args if a != "--update"]
    report = None
    if "--report" in args:
        i = args.index("--report")
        report = Path(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    tool, root = args[0], Path(args[1])
    cases = load_cases(root)
    if not cases:
        print("conformance: no cases listed; refusing to report a pass")
        return 1

    golden_dir = root / "golden"
    failed, missing, results = [], [], []
    for name, cols, rows in cases:
        got = replay(tool, root, name, cols, rows)
        golden = golden_dir / f"{name}.txt"
        if update:
            golden_dir.mkdir(exist_ok=True)
            golden.write_text(got)
            continue
        if not golden.exists():
            missing.append(name)
            results.append({"name": name, "cols": int(cols), "rows": int(rows),
                            "status": "missing", "golden": None, "actual": got})
            continue
        want = golden.read_text()
        results.append({"name": name, "cols": int(cols), "rows": int(rows),
                        "status": "pass" if got == want else "fail",
                        "golden": want, "actual": got})
        if got != want:
            failed.append(name)
            sys.stdout.writelines(difflib.unified_diff(
                want.splitlines(keepends=True), got.splitlines(keepends=True),
                fromfile=f"golden/{name}.txt", tofile=f"replay/{name}"))

    if update:
        print(f"conformance: wrote {len(cases)} goldens")
        return 0
    if report:
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps({"tier": 1, "cases": results}, ensure_ascii=False, indent=1) + "\n")
    for name in missing:
        print(f"conformance: {name}: no golden")
    passed = len(cases) - len(failed) - len(missing)
    print(f"conformance: {passed}/{len(cases)} match"
          + (f"; differ: {', '.join(failed)}" if failed else "")
          + (f"; missing golden: {', '.join(missing)}" if missing else ""))
    return 1 if failed or missing else 0


if __name__ == "__main__":
    sys.exit(main())
