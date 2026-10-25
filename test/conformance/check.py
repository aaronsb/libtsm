#!/usr/bin/env python3
"""Replay every conformance case and compare it with its golden.

Usage:
  check.py REPLAY DIR [--draw2 | --frame] [--no-images] [--report FILE]
                                                compare; exit 1 on any difference
  check.py REPLAY DIR [--draw2 | --frame] --update
                                                rewrite the goldens from this REPLAY

--draw2 and --frame replay every case through tsm-replay's draw2 or frame view
(resolved colours, the cells a renderer reads) and compare it with
golden-draw2/<name>.txt or golden-frame/<name>.txt. Those goldens are all
fork-sourced (see README).

--report writes the run as JSON for the contact sheet: for each case its
name, size, status (pass, fail or missing), golden and actual output, and
the tools object: the dev image digest, archive date and package versions. The
sheet is drawn from the same run that gives the verdict, so the two cannot
disagree.

DIR holds cases.list ("<name> <cols> <rows>" per line, # comments allowed),
cases/<name>.bin, an optional cases/<name>.cuts of checkpoint offsets, and
golden/<name>.txt.

--no-images is for a tsm-replay built against a libtsm with -Dimages=false
(ADR-103 in aaronsb/kmscon): the cases DIR/images-cases.list names need
images and are left out, and each must differ from its golden there, so the
list names no case that does not need them. Every name in that list must be
in cases.list, with or without the flag; --update does not read the list.

--update is for producing goldens from a reference build (make-golden.sh);
accepting a changed golden is a reviewed change of its own (ADR-301).

SPDX-License-Identifier: MIT
"""

import difflib
import json
import os
import subprocess
import sys
from pathlib import Path


# Packages whose version can move a rendered pixel or a replayed cell.
TOOL_PACKAGES = ("freetype2", "fontconfig", "harfbuzz", "pixman", "gcc", "meson")


def tool_versions():
    """The dev image's pins and package versions, read inside the container.

    Outside the image (no DEV_* variables, no pacman) the fields are null, so a
    report from a bare host says it does not know rather than guessing.
    """
    packages = {}
    for pkg in TOOL_PACKAGES:
        try:
            out = subprocess.run(["pacman", "-Q", pkg], capture_output=True, text=True)
        except OSError:
            break
        if out.returncode == 0:
            packages[pkg] = out.stdout.split()[1]
    return {
        "image_base": os.environ.get("DEV_IMAGE_BASE"),
        "archive_date": os.environ.get("DEV_ARCHIVE_DATE"),
        "packages": packages,
    }


def load_cases(root):
    cases = []
    for line in (root / "cases.list").read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, cols, rows = line.split()
        cases.append((name, cols, rows))
    return cases


def load_image_cases(root, cases):
    names = set()
    for line in (root / "images-cases.list").read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.add(line)
    unknown = names - {name for name, _, _ in cases}
    if unknown:
        raise SystemExit(f"conformance: images-cases.list names cases not in cases.list: "
                         f"{', '.join(sorted(unknown))}")
    return names


def replay(tool, root, name, cols, rows, view=None):
    cmd = [tool] + ([f"--{view}"] if view else []) + [cols, rows, str(root / "cases" / f"{name}.bin")]
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
    no_images = "--no-images" in args
    view = "draw2" if "--draw2" in args else "frame" if "--frame" in args else None
    args = [a for a in args if a not in ("--update", "--draw2", "--frame", "--no-images")]
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
    if no_images and update:
        sys.exit("conformance: goldens come from a build with images; --update with --no-images refused")
    # make-golden.sh's --update reads the cases from a copy without the list
    image_cases = set() if update else load_image_cases(root, cases)

    golden_dir = root / (f"golden-{view}" if view else "golden")
    label = f"conformance-{view}" if view else "conformance"
    failed, missing, results, skipped, stale = [], [], [], [], []
    for name, cols, rows in cases:
        got = replay(tool, root, name, cols, rows, view)
        golden = golden_dir / f"{name}.txt"
        if no_images and name in image_cases:
            skipped.append(name)
            if golden.exists() and golden.read_text() == got:
                stale.append(name)
            continue
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
                fromfile=f"{golden_dir.name}/{name}.txt", tofile=f"replay/{name}"))

    if update:
        print(f"{label}: wrote {len(cases)} goldens")
        return 0
    if report:
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps({"tier": 1, "tools": tool_versions(), "cases": results}, ensure_ascii=False, indent=1) + "\n")
    for name in missing:
        print(f"{label}: {name}: no golden")
    for name in stale:
        print(f"{label}: {name}: in images-cases.list, but matches its golden without images")
    passed = len(cases) - len(failed) - len(missing) - len(skipped)
    print(f"{label}: {passed}/{len(cases) - len(skipped)} match"
          + (f" ({len(skipped)} cases that need images left out, a build without images)"
             if no_images else "")
          + (f"; differ: {', '.join(failed)}" if failed else "")
          + (f"; missing golden: {', '.join(missing)}" if missing else ""))
    return 1 if failed or missing or stale else 0


if __name__ == "__main__":
    sys.exit(main())
