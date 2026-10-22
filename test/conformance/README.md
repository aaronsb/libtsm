# Conformance replay

Tier 1 of the kmscon fork's conformance harness (ADR-301 in aaronsb/kmscon). Recorded byte streams are replayed into libtsm and the resulting screen state is compared, as text, with a golden produced by upstream libtsm. Any difference fails `meson test`.

| Path | What it is |
|---|---|
| `tsm-replay.c` | Replays a stream through libtsm's public API and prints the screen: size, cursor, flags, replies, then each row's text and attribute runs. With a `.cuts` file it prints the screen at every checkpoint. |
| `cases.list` | One case per line: name, columns, rows. |
| `cases/*.bin`, `cases/*.cuts` | The streams, and checkpoint offsets for recorded programs. |
| `golden/*.txt` | The expected output for each case. |
| `gen-synthetic.py` | Writes the synthetic streams, one area of terminal behaviour each. |
| `record-programs.py` | Records real programs (shell, less, vim, tmux) in a pseudo-terminal of fixed size with scripted keys. |
| `check.py` | Replays every case and compares it with its golden; `--update` rewrites goldens. |
| `make-golden.sh` | Builds tsm-replay against a reference libtsm (default upstream `v4.8.0`) and rewrites the goldens. |

From the repository root: `make test` runs everything, `make cases` and `make record` regenerate streams, `make golden REF=v4.8.0` regenerates goldens.

A golden is the judge. Regenerate goldens only from a reference build or for an intended change of behaviour, and commit them on their own, with the diff read, never in the same commit as the code they judge.

Goldens from upstream `v4.8.0` record its behaviour as it is, defects included. `sgr-underline-extended` shows two: colon sub-parameters (`4:3`, `58:2::r:g:b`) discard the whole SGR sequence, and `4;58;5;196` sets blink, because the unhandled 58 leaves its `5` to be read as SGR 5.

## Pinned dev image

`DEV_BASE` (the `archlinux:base-devel` digest) and `DEV_ARCHIVE` (an Arch Linux Archive date) at the top of the Makefile pin the dev image, so a rebuild installs the same packages. The kmscon fork pins its image the same way.
`check.py --report` records them in a `tools` object, with the versions of the toolchain packages installed in the image, so a golden mismatch can be traced to a version change.
To move a pin, change the one line, run `make test`, and read `git status`. Nothing is regenerated unless a golden moves, and a moved golden lands in its own commit.
