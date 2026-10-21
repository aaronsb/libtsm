#!/usr/bin/env python3
"""Record real programs' terminal output for the conformance corpus.

Each recording runs a program in a pseudo-terminal of a fixed size with a fixed
environment, types a scripted sequence of keys, and saves every byte the
program wrote. The recordings are committed; rerun this only to add or change
a recording, inside the dev container so the programs and their versions are
the image's, and review the resulting goldens as their own change.

Usage: record-programs.py CASES_DIR [NAME...]
Writes CASES_DIR/<name>.bin, and CASES_DIR/<name>.cuts with the byte offset
at which each screen had settled (after start-up and after each scripted key),
so the replay checks every screen the program showed, not only its last.
Prints one "<name> <cols> <rows>" line per recording, which belongs in
cases.list.

SPDX-License-Identifier: MIT
"""

import fcntl
import os
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time
from pathlib import Path

ESC = "\x1b"
QUIET = 0.4      # seconds of silence that count as "the screen has settled"
SETTLE_MAX = 5.0  # longest wait for a screen to settle


def env(tmp):
    return {
        "PATH": "/usr/bin:/bin",
        "HOME": tmp,
        "TERM": "xterm-256color",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
        "PS1": "\\[\\e[1;32m\\]user@host\\[\\e[0m\\]:\\[\\e[1;34m\\]~\\[\\e[0m\\]$ ",
    }


def demo_files(tmp):
    root = Path(tmp) / "demo"
    (root / "src").mkdir(parents=True)
    (root / "src" / "main.c").write_text(
        "#include <stdio.h>\n\n"
        "/* A small file for the editor recording. */\n"
        "int main(void)\n{\n\tfor (int i = 0; i < 3; i++)\n"
        "\t\tprintf(\"line %d\\n\", i);\n\treturn 0;\n}\n")
    (root / "notes.txt").write_text(
        "".join(f"{n:3d}  The quick brown fox jumps over the lazy dog. "
                f"漢字 {'*' * (n % 7)}\n" for n in range(1, 121)))
    (root / "run.sh").write_text("#!/bin/sh\necho run\n")
    os.chmod(root / "run.sh", 0o755)
    stamp = time.mktime((2026, 1, 1, 0, 0, 0, 0, 0, 0))
    for p in [root, *root.rglob("*")]:
        os.utime(p, (stamp, stamp))
    return root


def drain(fd, out):
    """Read output until it has been quiet for QUIET seconds."""
    deadline = time.monotonic() + SETTLE_MAX
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], QUIET)
        if not ready:
            return
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            return
        if not chunk:
            return
        out.extend(chunk)


def record(argv, keys, cols, rows, tmp, cwd):
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(cwd)
        os.execve(argv[0], argv, env(tmp))
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    os.kill(pid, signal.SIGWINCH)
    out = bytearray()
    cuts = []
    drain(fd, out)
    cuts.append(len(out))
    for k in keys:
        os.write(fd, k.encode())
        drain(fd, out)
        cuts.append(len(out))
    try:
        os.kill(pid, signal.SIGHUP)
    except ProcessLookupError:
        pass
    drain(fd, out)
    os.close(fd)
    os.waitpid(pid, 0)
    return bytes(out), sorted(set(c for c in cuts if 0 < c < len(out)))


def line(cmd):
    return cmd + "\r"


RECORDINGS = {
    # name: (cols, rows, argv, keys)
    "prog-shell": (80, 24, ["/usr/bin/bash", "--norc", "--noprofile", "-i"], [
        line("ls --color=always -l"),
        line("ls --color=always src"),
        line("printf '\\e[1mbold\\e[0m \\e[31mred\\e[0m \\e[4munder\\e[0m\\n'"),
        line("seq 1 30 | paste -sd' '"),
        line("for i in 1 2 3; do echo \"loop $i\"; done"),
        line("exit"),
    ]),
    "prog-less": (80, 24, ["/usr/bin/less", "-R", "notes.txt"], [
        " ", " ", "b", "/fox\r", "n", "G", "g", "q",
    ]),
    "prog-vim": (80, 24, ["/usr/bin/vim", "--clean", "-N", "src/main.c"], [
        "jjj", "A // edited" + ESC, "o\tputs(\"added\");" + ESC, ":set number\r",
        "gg", "/printf\r", ":q!\r",
    ]),
    "prog-tmux": (80, 24, ["/usr/bin/tmux", "-f", "/dev/null", "-L", "conformance",
                            "new-session", "/usr/bin/bash", "--norc", "--noprofile", "-i"], [
        line("echo left pane"),
        "\x02%",
        line("echo right pane"),
        "\x02\"",
        line("seq 1 5"),
        line("tmux -L conformance kill-server"),
    ]),
}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    names = sys.argv[2:] or list(RECORDINGS)
    out.mkdir(parents=True, exist_ok=True)
    for name in names:
        cols, rows, argv, keys = RECORDINGS[name]
        with tempfile.TemporaryDirectory() as tmp:
            cwd = demo_files(tmp)
            data, cuts = record(argv, keys, cols, rows, tmp, cwd)
        (out / f"{name}.bin").write_bytes(data)
        (out / f"{name}.cuts").write_text("".join(f"{c}\n" for c in cuts))
        print(name, cols, rows)
        print(f"{name}: {len(data)} bytes", file=sys.stderr)


if __name__ == "__main__":
    main()
