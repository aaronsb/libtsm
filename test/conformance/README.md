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
| `golden-frame/*.txt` | The same through the frame view, all fork-sourced (below). |
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

## The frame view

`tsm-replay --frame` prints each screen through `tsm_screen_frame`, the call that replaces draw2 (ADR-102 step 4). It shows what draw2 cannot: each cell's width, underline style, strike, dim, protect and inverse as applied. It also shows the cursor and the cell it draws, and how many cells the frame flagged as changed since the previous checkpoint. Rows print whole clusters, read back through `tsm_screen_frame_cluster`. The meson test `conformance-frame` compares the corpus with `golden-frame/`, which is fork-sourced like `golden-draw2/`. The draw2 view and its goldens stay until draw2 is removed.

## Images

Against a libtsm that declares `TSM_SCREEN_IMAGES` (ADR-103 in aaronsb/kmscon), every view ends with the image placements a frame lists, in draw order, and a digest of each image they show; the frame view adds each placement's changed flag. A screen with no placements prints nothing more, so the upstream-sourced goldens are untouched.

Sixel makes images through the vte (ADR-103 step 3c): against a libtsm that declares `TSM_VTE_SIXEL`, tsm-replay turns sixel on, so DA1 replies with attribute 4 in every case that asks, which makes `replies` fork-sourced. A screen stores no image until it has a host budget or a limit, so tsm-replay sets a limit of its own, Ghostty's 320 MB per screen, which pays for the pictures.

The kitty graphics protocol arrives with step 4a, so kitty images still come from test commands: an APC beginning `tsm-test;`, which tsm-replay runs through libtsm's test-only and host calls instead of feeding to the vte. The same commands set what a host sets (the cell size, the limit, scrollback, `max-graphic-size`, the register settings) and scroll the view. The header of `tsm-replay.c` lists them. A tsm-replay built against upstream feeds the APC to the vte, which ignores it, and since every image case is fork-sourced, upstream's output for them is never compared.

Each `img-*` case judges one row of ADR-103's screen-event table, for a sixel and a kitty placement side by side: text and erase over placements, ED 2 and ED 3, ICH and DCH, whole-screen and region scrolls, IL and DL, discarded lines, resize, the alternate screen, RIS and DECSTR. Their sixels are real sixel strings, wrapped in DECSC and DECRC so the cursor stays where the case left it, except in `img-scroll-up-bottom`, whose sixel reaches past the screen's bottom where no sixel string can place it.

The `six-*` cases judge sixel itself: the cursor after a picture, mode 8452, scrolling at the bottom margin and in a region, DECSDM, registers in RGB and HLS, raster attributes and P2, mode 1070 and RIS, `max-graphic-size`, CAN and SUB, text over a picture, scrollback and clear, the replies (DA1, XTSMGRAPHICS, XTWINOPS 14, 16 and 18), sixel off without a budget, and three streams img2sixel wrote (`test/sixel/`). Each asks for the cursor (DSR 6) after its pictures, so its reply line records where the cursor went. `six-cursor-xterm` replays the bytes of a probe run in xterm 411 (`-ti vt340`, 6x13 cells) as a black box; xterm's cursor reports agree with this golden's except in two places where ADR-103's rules differ from xterm's, which the case's comment in `gen-synthetic.py` lists. The parser sets no protected cells (DECSCA is not parsed), so `img-decsed` and `img-decsel` erase what ED and EL do, and `test_image.c` covers a selective erase around a protected cell.

## Pinned dev image

`DEV_BASE` (the `archlinux:base-devel` digest) and `DEV_ARCHIVE` (an Arch Linux Archive date) at the top of the Makefile pin the dev image, so a rebuild installs the same packages. The kmscon fork pins its image the same way.
`check.py --report` records them in a `tools` object, with the versions of the toolchain packages installed in the image, so a golden mismatch can be traced to a version change.
To move a pin, change the one line, run `make test`, and read `git status`. Nothing is regenerated unless a golden moves, and a moved golden lands in its own commit.
