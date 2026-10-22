# Conformance replay

Tier 1 of the kmscon fork's conformance harness (ADR-301 in aaronsb/kmscon). Recorded byte streams are replayed into libtsm and the resulting screen state is compared, as text, with a golden produced by upstream libtsm. Any difference fails `meson test`.

| Path | What it is |
|---|---|
| `tsm-replay.c` | Replays a stream through libtsm's public API and prints the screen: size, cursor, flags, replies, then each row's text and attribute runs. With a `.cuts` file it prints the screen at every checkpoint. |
| `cases.list` | One case per line: name, columns, rows. |
| `fork-goldens.list` | The cases whose golden comes from a fork commit, each with the change of behaviour it judges. |
| `cases/*.bin`, `cases/*.cuts` | The streams, and checkpoint offsets for recorded programs. |
| `golden/*.txt` | The expected output for each case. |
| `golden-draw2/*.txt` | The expected output of every case through the draw2 view, all fork-sourced (below). |
| `gen-synthetic.py` | Writes the synthetic streams, one area of terminal behaviour each. |
| `record-programs.py` | Records real programs (shell, less, vim, tmux) in a pseudo-terminal of fixed size with scripted keys. |
| `check.py` | Replays every case and compares it with its golden; `--update` rewrites goldens. |
| `make-golden.sh` | Builds tsm-replay against a reference libtsm (default upstream `v4.8.0`) and rewrites the goldens of one source (below). |

From the repository root: `make test` runs everything, `make cases` and `make record` regenerate streams, and `make golden REF=...` regenerates goldens.

A golden is the judge. Regenerate goldens only from a reference build or for an intended change of behaviour, and commit them on their own, with the diff read, never in the same commit as the code they judge.

## Where each golden comes from

A case judges either behaviour the fork preserves or behaviour it changed on purpose, and its golden comes from upstream or from the fork accordingly:

- **Upstream-sourced**, every case not in `fork-goldens.list`: `make golden REF=v4.8.0` writes these, from upstream's code and this tree's `tsm-replay.c`. The fork's code never produces them.
- **Fork-sourced**, the cases in `fork-goldens.list`: `make golden REF=<fork commit>` writes these, from the fork's code at that commit. Name the commit that holds the change, and commit the goldens on their own, saying what changed.

`make-golden.sh` writes only the set its `REF` selects, so regenerating one set never overwrites the other. It refuses any other `REF`: an older upstream tag would write fork-listed goldens from upstream code. A commit counts as the fork's when it carries `fork-goldens.list`. It also refuses a name in `fork-goldens.list` that is not in `cases.list`, since a misspelling would leave that case in the upstream set. Moving a case into `fork-goldens.list` is itself a change to the judge and goes in the golden commit, with the reason beside the name.

`tsm-replay.c` prints the fork's added attributes (underline style, underline colour, strike) only where `libtsm.h` defines `TSM_SCREEN_ATTR_UNDERLINE_STYLE`, and only when set, so it builds against upstream and prints what upstream would. To check that the upstream-sourced goldens still hold, run `make golden REF=v4.8.0` and read `git status`: nothing should change.

`make golden` runs git inside the dev container, with only this checkout mounted. In a `git worktree` checkout, the `.git` file points at the main repository, which the container cannot see, so git fails ("gitfile does not point to a valid repository"). Mount the main repository at the same path by overriding `RUN`, for example:

```sh
M=/path/to/main/libtsm
make golden REF=v4.8.0 RUN="docker run --rm --user $(id -u):$(id -g) -e HOME=/tmp -v $PWD:/src -v $M:$M -w /src libtsm-dev"
```

Upstream `v4.8.0`, as its goldens recorded before the fork changed these cases, discarded a whole SGR sequence containing a colon sub-parameter (`4:3`, `58:2::r:g:b`), set blink on `4;58;5;196` because the unhandled 58 left its `5` to be read as SGR 5, and ignored SGR 9. The fork implements colon sub-parameters, underline styles, underline colour and strike-through (ADR-102 in aaronsb/kmscon), so `sgr-basic`, `sgr-underline-extended` and the cases added with them are fork-sourced.

## The draw2 view

`tsm-replay --draw2` prints each screen through `tsm_screen_draw2`, the cells a renderer such as kmscon reads, in place of `tsm_screen_draw`. Colours appear resolved to RGB through the palette, with bold-as-bright, reverse video, dim and the underline colour applied, so tier 1 judges what the screen looks like as well as what it holds (aaronsb/kmscon#26). The view pins the `legacy` palette and prints it with its default colours. Rows show the codepoint each cell carries, and `cells` lines show runs that differ from the default cell. The header of `tsm-replay.c` gives the format.

The meson test `conformance-draw2` runs every case through this view and compares it with `golden-draw2/`. Only the fork's libtsm has the view, so that whole set is fork-sourced: a fork `REF` writes all of it, and `REF=v4.8.0` none. The view is a separate set of files, not a section added to `golden/`, so every file still has one source. A section in `golden/` would have mixed upstream output with fork output in one file, and every upstream-sourced golden would have needed a fork run as well.

## Pinned dev image

`DEV_BASE` (the `archlinux:base-devel` digest) and `DEV_ARCHIVE` (an Arch Linux Archive date) at the top of the Makefile pin the dev image, so a rebuild installs the same packages. The kmscon fork pins its image the same way.
`check.py --report` records them in a `tools` object, with the versions of the toolchain packages installed in the image, so a golden mismatch can be traced to a version change.
To move a pin, change the one line, run `make test`, and read `git status`. Nothing is regenerated unless a golden moves, and a moved golden lands in its own commit.
