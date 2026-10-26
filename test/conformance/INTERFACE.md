# The replay interface

Interface 1.0. This document is the contract a replay implementation meets, and the implementation it is written for is vtium's. A replay implementation plays a byte stream, as a program wrote it to a terminal, into a terminal implementation and prints the screen state that results as text. The goldens in `golden/`, `golden-draw2/` and `golden-frame/` are that text for the streams in `cases/`, made by `tsm-replay`, the reference implementation in the frozen C code, whose header holds the C notes. This document defines the command line, the input, the test commands, the reference host and the output by what they do. The goldens define the terminal's behaviour.

The fenced blocks named `describe`, `grammar`, `palettes`, `example` and `output` are machine-readable. The meson test `replay-interface` (`interface-check.py`) checks the reference implementation's `--describe` against the describe block, every golden against the grammar and the numbering rules, the palettes against the frame view, every example against its output, and the exit status of malformed and edge inputs. It also checks itself: the grammar must reject a fixed set of bad outputs on every run.

## Command line

```
IMPL [--draw2 | --frame] COLS ROWS FILE [CUTS]
IMPL --describe
```

The first form replays FILE into a screen of COLS columns, 1 to 2048, and ROWS rows, 1 to 1024, each a decimal number with no sign or leading zero, and prints the screen view, or the draw2 or frame view the flag names. The second prints what the implementation offers (Versioning and features).

| Exit | Meaning |
|---|---|
| 0 | The output is the result. |
| 2 | Usage: a wrong argument, a size out of bounds, a file that does not read, a CUTS line that is not an offset, CUTS out of order or past the end of FILE, a cut inside a test command, a view or test command the implementation lacks, or a malformed test command. |
| 3 | The implementation cannot print what it holds: a cell it cannot hold (the reference's bound is 32 codepoints, in the screen view), input that stops with no host request out, or another internal failure. |

Output goes to standard output. Standard error is free-form.

## Input

FILE is fed to the terminal byte by byte, in order. CUTS, when given, holds byte offsets into FILE, one per line, each a decimal number with no sign or leading zero and nothing else on the line, at least the one before it and at most FILE's length. The implementation prints a block at each offset and at the end of FILE, each headed by a `checkpoint` line. Without CUTS, or with an empty CUTS file, it prints one block, with no `checkpoint` line.

The fixture directory is FILE with a final `.bin` removed and `.files` appended. It holds the files that the `termcap` command and the fixture host read.

### Test commands

A test command is the bytes `ESC _ tsm-test;` (1b 5f, then the ASCII text `tsm-test;`), the command's text, and `ESC \` (1b 5c). The implementation runs the command at the point the stream reaches it, in place of feeding those bytes to the terminal. A cut inside a test command, its introducer included, exits 2, and so does a command without its closing `ESC \`.

The text is at most 511 bytes, each 0x20 to 0x7e: the command's name, then fields `KEY=VALUE`, all separated by `;`. The text starts with the name, and empty fields after it are ignored. A value holds no `;` and no `=`. A byte outside 0x20 to 0x7e, a leading `;`, an unknown command, a key the command does not take, a key given twice, a missing required key, a field with no `=`, an `=` inside a value and a value outside its range each exit 2.

Values take these forms:

| Form | Syntax |
|---|---|
| decimal | digits `0-9` with no sign or leading zero, within the range the command gives |
| signed | an optional `-`, then digits with no leading zero, from -2147483648 to 2147483647; `-0` exits 2 |
| hex | digits `0-9`, `a-f`, `A-F`, within the range the command gives |
| flag | `0` or `1` |
| name | one of the words the command lists |
| fixture | 1 to 255 bytes of `A-Z`, `a-z`, `0-9`, `.`, `_` and `-`, not starting with `.`: a file in the fixture directory |

When the terminal refuses a command that sets something, the implementation prints `test NAME error N`, N being the refusal's Linux errno negated (`-22` for EINVAL). A query prints its own `test` line. Each `test` line is printed when its command runs, so it comes before the `checkpoint` line, or without CUTS the `size` line, of the block that follows.

### The commands

Every implementation of interface 1 has the commands with no feature. A command's feature is declared by `--describe` (Versioning and features). Ranges are inclusive, and a key with no default is required.

| Command | Keys | Feature | What it does |
|---|---|---|---|
| `resize` | `c`: decimal, 1 to 2048; `r`: decimal, 1 to 1024 | | Resizes the screen to `c` columns and `r` rows, as a host does when its window changes. |
| `sb-max` | `n`: decimal, 0 to 4294967295 | | Sets the most lines the scrollback holds. |
| `sb-up` | `n`: decimal, 0 to 4294967295, default 1 | | Scrolls the view `n` lines up into the scrollback. |
| `sb-down` | `n`: decimal, 0 to 4294967295, default 1 | | Scrolls the view `n` lines back down. |
| `sb-reset` | none | | Returns the view to the bottom of the scrollback. |
| `cell` | `w`, `h`: decimal, 0 to 4294967295 | `images` | Sets the cell size in pixels that images are placed in. |
| `limit` | `b`: decimal, 0 to 18446744073709551615 | `images` | Sets the bytes the screen may spend on images. |
| `image` | `n`: decimal, 0 to 63, default 0; `w`, `h`: decimal, 0 to 65535, default 0, `w` times `h` at most 16777216; `kitty`: flag, default 0; `id`: decimal, 0 to 4294967295, default `n`; `tr`: flag, default 0; `fill`: hex, 0 to ffffffff, default ff808080 | `images` | Stores an image of `w` by `h` pixels, each the premultiplied colour `fill` (AARRGGBB), as a sixel would make it or, with `kitty=1`, as a kitty image of id `id`, transient (kitty's `N=1`) with `tr=1`. Later commands call it `n`. |
| `place` | `img`: decimal, 0 to 63, default 0; `col`, `row`: decimal, 0 to 4294967295, default the cursor's; `x`, `y`, `c`, `r`, `sx`, `sy`, `sw`, `sh`, `p`: decimal, 0 to 4294967295, default 0; `z`: signed, default 0 | `images` | Places image `img` with its box's top left cell at `col`, `row` and its picture `x`, `y` pixels into that cell. The box is `c` by `r` cells: with both 0 the picture at its pixel size, with both given the picture scaled to fit and centred, with one given the other following the source's aspect. The source is the rectangle at `sx`, `sy` of `sw` by `sh` pixels, cut to the image, a width or height of 0 reaching its edge. `z` orders a kitty placement, and `p` is a kitty placement id, which replaces one with the same image and id. The cursor does not move. |
| `sixel` | `on`: flag, default 1 | `sixel` | Turns sixel graphics on or off. |
| `graphic-max` | `w`, `h`: decimal, 0 to 4294967295 | `sixel` | Sets the largest picture in pixels, each dimension; 0 is the default, 10000. |
| `registers` | `n`: decimal, 0 to 4294967295; `private`: flag, default 1 | `sixel` | Sets the number of sixel colour registers (0 is the default, 1024) and whether each picture starts from its own (mode 1070's state at reset). |
| `kitty-graphics` | `on`: flag, default 1 | `kitty-graphics` | Turns the kitty graphics protocol on or off. |
| `host` | `on`: flag, default 1 | `image-host` | Installs (1) or removes (0) the fixture host. |
| `sync` | `on`: flag, default 1 | `sync` | Says whether the host presents by mode 2026 (synchronized output). Turning it off ends a hold. |
| `sync-frames` | none | `sync` | Prints `test sync-frames N`, N being the number of mode 2026 holds that have ended so far, by any cause. |
| `version` | `text`: 0 to 64 bytes, each 0x20 to 0x7e | `identify` | Sets the text that XTVERSION (`CSI > q`) replies. |
| `termcap` | `file`: fixture | `identify` | Replaces the capability table that XTGETTCAP (`DCS + q`) answers from with the fixture's (below). |
| `key` | `sym`: hex, 0 to 1fffffff; `ascii`: hex, 0 to 1fffffff, default 0; `unicode`: hex, 0 to 10ffff, default none; `mods`: decimal, 0 to 31, default 0 | `input` | Presses a key, as a host reports one. `sym` is the X11 keysym after Shift, `ascii` the keysym an ASCII layout gives the same key (0 for none), `unicode` the character the key makes, and `mods` the modifiers held. |
| `mouse` | `col`: decimal, below the screen's width; `row`: decimal, below its height; `px`, `py`: decimal, 0 to 65535, default 0; `button`: name, `left`, `middle`, `right`, `wheel-up`, `wheel-down` or `none`; `event`: name, `press`, `release` or `move`; `mods`: decimal, 0 to 31, default 0 | `input` | Reports a pointer event at cell `col`, `row` and pixel `px`, `py`, each counted from 0 at the top left of the text area. A press or release names its button, and a wheel step is a press. A move names the button held, `left`, `middle` or `right`, or `none`. Of `mods`, Shift, Ctrl and Alt are reported. |
| `paste` | `hex`: the pasted bytes, two hex digits each, none of them 00 | `input` | Pastes the bytes, as a host does. |
| `focus` | `in`: flag | `focus` | Reports that the terminal gained (1) or lost (0) focus. |
| `palette` | `name`: name, one of the palettes below | `palette` | Chooses the palette, as a host does. The `palette` line of the draw2 and frame views names it. |
| `frame-since` | `caller`: decimal, 0 to 7 | `query` | Takes a frame for caller `caller` and prints `test frame-since K changed N`, K being the caller and N the number of cells that changed since that caller's previous frame. Each caller keeps its own frame age, starting with none, so its first frame counts every cell. N follows the age rule of `changed N` and is informative (Comparing with the goldens). |

The modifier bits of `mods` are 1 Shift, 2 Lock, 4 Ctrl, 8 Alt and 16 Logo.

A termcap fixture holds one capability per line, and an empty line is ignored. A line is `NAME` for a boolean capability, or `NAME=HEX` for a string or numeric one, HEX being the value's bytes, two hex digits each: a string as `tigetstr()` returns it, its escapes decoded and its `%` sequences kept, and a number as decimal text. A NAME is 1 to 32 bytes of 0x21 to 0x7e other than `=`, and a value at most 256 bytes, none of them 00. A table holds at most 1024 capabilities, each named once. A fixture outside these bounds exits 2.

### The fixture host

The fixture host stands for the program a terminal hands kitty graphics transmissions to when it does not decode them itself. A transmission (`a=t`, `a=T` or `a=q`) reaches the host once its last chunk has arrived and its keys have been checked, when it comes from a file (`t=f`), a temporary file (`t=t`) or shared memory (`t=s`), or comes directly (`t=d`) as PNG (`f=100`) or compressed (`o=z`). A `t=t` or `t=s` transmission that the terminal refuses reaches it as a discard, which removes the object and reads nothing. The stream goes on after the host answers. Each request prints a `host:` line among the replies, at the point it was made:

```
host: ID t=M flags=F O=OFFSET max=MAX min=MIN name=NAME len=LEN
```

| Field | Value |
|---|---|
| ID | The request's number, counting from 1 in each replay. |
| M | The medium: `d`, `f`, `t` or `s`. |
| F | The flags set, comma-separated in this order, or `-` for none: `inflate` (`o=z`), `png` (`f=100`), `exact` (direct PNG with `o=z` and a non-zero `S`: the inflated bytes must number exactly MIN), `discard`. |
| OFFSET | Where a read starts: kitty's `O` for `f`, `t` and `s`, else 0. |
| MAX | The most bytes to read: kitty's `S` for `f`, `t` and `s`, 0 meaning to the end; else 0. |
| MIN | The fewest bytes the image needs after inflating: `s` times `v` times 3 for `f=24` or 4 for `f=32`; for PNG, `S` with `exact` and else 0; 0 for a discard. |
| NAME | For `f`, `t` and `s`, the path or name the payload gives, each byte outside 0x21 to 0x7e, and the backslash, written `\xNN`; empty for `d`. |
| LEN | For `d`, the payload's length after base64 decoding; else 0. |

The fixture a request names is the last `/`-separated part of its path or name as received, or for `t=d` `direct-K` for the replay's Kth direct request, counting from 1. The host answers with an outcome, from the first of these that applies:

| Fixture | Answer |
|---|---|
| `FIXTURE.error` | Its first word, one of `UNREADABLE`, `TOO_SHORT`, `CORRUPT`, `TOO_LARGE` and `OVER_BUDGET`. |
| `FIXTURE.rgba`, for a `png` request | OK with the decoded image: its width and height as little-endian 32-bit numbers, then its pixels as R, G, B, A bytes, rows top to bottom. |
| `FIXTURE.bytes`, for an `inflate` request without `png` | The inflated bytes: OK with the first MIN of them; TOO_SHORT for fewer; with `exact`, CORRUPT for more. |
| `FIXTURE`, for a request with neither | The data, read from OFFSET for at most MAX bytes (0: to the end): OK with the first MIN of them; UNREADABLE for an OFFSET at or past the end; TOO_SHORT for fewer than MIN. |
| none of these | UNREADABLE for `f`, `t` and `s`; CORRUPT for `d`. |

A discard is answered OK, and the host deletes nothing. The outcomes are the host's terms. The terminal turns each into the kitty reply the goldens show.

## Reference host

A terminal has settings that a host chooses. The goldens were made with these values, and a case that needs another value sets it with a command.

| Setting | Value |
|---|---|
| Image budget | 320000000 bytes per screen, and no other budget |
| Sixel graphics, kitty graphics | On |
| Presenting by mode 2026 | On |
| XTVERSION text | `tsm-replay` |
| XTGETTCAP table | Empty |
| Kitty graphics host | The fixture host |
| Palette | `legacy`, in every view |
| Cell size | 8 by 16 pixels |
| Scrollback | 0 lines |
| Largest sixel picture | 10000 by 10000 pixels |
| Sixel colour registers | 1024, each picture starting from its own |
| Cursor blinking | On, so DECRQM of mode 12 reports a blinking cursor |
| BackSpace key | Sends 0x08 |
| Character widths | Unicode's (mode 2027's configured state) |

## Output

### Blocks

A block is the screen at one point. Every view starts it with `size`, `cursor` and `flags`, then the replies since the previous block: `reply` lines of the bytes the terminal wrote back, and `host:` lines among them at the points requests were made. The rest depends on the view:

- screen: for each row, a `row` line then its `attr` lines; then `placement` lines and `image` lines.
- draw2: `palette`, then for each row a `row` line and its `cells` lines; then `placement` and `image` lines.
- frame: `palette`, `changed`, `frame-cursor`, then for each row a `row` line and its `cells` lines; then `placement` lines, each with its changed flag, and `image` lines.

Rows are numbered from 0 at the top of the view and printed in order, as many as the screen has. In the screen view the C reference printed the ROWS rows of the command line instead, each COLS cells wide, whatever size a `resize` had given the screen since, so that a cell past COLS was not printed there and a row past the screen's height was empty; `img-resize` and `img-resize-shrink` record it, and an implementation may list it as a departure. A run is a maximal span of consecutive cells A to B on one row whose description is the same, and the view prints the runs that differ from its default cell, left to right. In the screen view a cell's description is its attributes: a cell outside the screen has none, and an empty cell keeps the attributes it was erased with. In the draw2 and frame views it is everything the `cells` line prints. Placements are numbered from 0 in draw order and images from 0 in the order placements first show them, each image listed once.

### Lines

`size COLSxROWS` is the screen's size. `cursor X,Y` is the cursor's cell, X being the column count at the pending-wrap position. `flags` lists the screen's flags set, in the order `INSERT_MODE`, `AUTO_WRAP`, `REL_ORIGIN`, `INVERSE`, `HIDE_CURSOR`, `FIXED_POS`, `ALTERNATE`, joined by `|`, or `none`. `reply HEX` is bytes the terminal wrote back, two lowercase hex digits each, and the replies since the previous block print as one `reply` line between consecutive `host:` lines.

`row N |TEXT|` is a row's text. In the screen and frame views each cell prints all the codepoints it holds, an empty cell prints a space, and the cell after a wide character prints nothing. In the draw2 view each cell prints one codepoint: the first of its cluster, or a space for an empty cell and for the trailing half of a wide character. Each view trims the cells at the end of the row that hold nothing; draw2 and frame also trim spaces there.

`attr N A-B KEY...` is a run of the screen view. Its keys come in this order, each only when it differs from the default: `fg=` and `bg=` with a colour code, `bold`, `italic`, `underline` or `underline=STYLE` (`double`, `curly`, `dotted` or `dashed`), `ul=` (the underline colour set by SGR 58) with a colour code, `inverse`, `protect`, `blink`, `dim`, `strike`. A colour code, for `fg=`, `bg=` and `ul=` alike, is the palette index 0 to 15 for a colour chosen by an index below 16 (SGR 30 to 37, 40 to 47, 90 to 97 or 100 to 107, or 38, 48 or 58 with 5 and an index below 16), and `#rrggbb` in lowercase for every other colour.

`palette NAME fg=#rrggbb bg=#rrggbb` names the palette in force and its default colours. Every colour of the draw2 and frame views is `#rrggbb` in lowercase, resolved through that palette: indexes 0 to 15 and the defaults from the palettes block, 16 to 231 the 6x6x6 cube with levels 0, 95, 135, 175, 215 and 255, and 232 to 255 the grey ramp 8, 18, ..., 238. The goldens judge how bold, dim and inverse change the colours.

`cells N A-B fg= bg= ul=[ ulset] FLAG...` is a run of the draw2 view: its colours after the palette, reverse video, dim and the underline colour are applied, `ulset` when SGR 58 set the underline colour, and its flags in the order `bold`, `italic`, `underline`, `blink`. The default cell has the default colours, an underline colour equal to the foreground, and no flag.

`changed N` is the number of cells the frame flags as changed since the view's previous block, by the C reference's age rule: a cell changes when it is written or erased, or when a whole-screen redraw such as a scroll, a resize or a palette change covers it, and the first block counts every cell. It is informative (Comparing with the goldens), as is a placement's `changed` flag. `frame-cursor X,Y STYLE visible|hidden[ CELL]` is where the cursor draws, its style (`default`, `block-blink`, `block-steady`, `underline-blink`, `underline-steady`, `bar-blink` or `bar-steady`) and, when visible, the cell as the cursor draws it.

`cells N A-B CELL` is a run of the frame view, and CELL is `w=W fg= bg= ul=` then its keys in the order `ulset`, `underline=STYLE` (`single`, `double`, `curly`, `dotted` or `dashed`), `bold`, `italic`, `dim`, `blink`, `inverse`, `protect`, `strike`. W is 1 or 2, and 0 for the trailing half of a wide character. The default cell has width 1, the default colours, an underline colour equal to the foreground, and no key.

`placement N row R col C box CxR at X,Y size WxH src X,Y WxH cell WxH z Z BAND KIND image M[ opaque][ changed]` is a placement: its box's top left at visible row R (negative when it starts above the view) and column C, C by R cells; its picture's visible part, W by H pixels at X, Y from the box's top left, showing the source rectangle that follows, all in pixels of the cell size after `cell`, the one the placement was made with; its z order; its band, `below-bg`, `below-text` or `above-text`; its kind, `sixel` or `kitty`; and the image it shows. `opaque` means no pixel of the image is below full alpha. `changed` appears only in the frame view, for a placement added, moved, clipped or given new pixels since the previous block.

`image M WxH digest HEX` is an image's size and the 32-bit FNV-1a hash (offset basis 2166136261, prime 16777619) of its pixels, rows top to bottom, each pixel as the four bytes A, R, G, B of its premultiplied colour, in eight lowercase hex digits.

`checkpoint N` heads the block printed at offset N. `test ...` lines are under Test commands.

### Comparing with the goldens

A runner compares an implementation's output with a golden byte for byte. The N of a `changed N` line and of a `test frame-since K changed N` line, and a placement's ` changed` flag, follow the C reference's age rule, which another implementation may draw differently and still draw correctly. Unless both the implementation and the reference declare the feature `damage-exact`, the runner masks them on both sides before comparing: each such N greater than 0 becomes the letter `N`, and each placement's ` changed` flag is removed. Zero against nonzero stays comparable, so a probe can still check that each frame-since caller keeps its own changes. The reference declares `damage-exact`.

### Grammar

A line's kind is its first word. `define NAME REGEX` names a fragment, and `{NAME}` in a later regex stands for `(?:REGEX)`. `reserved NAME REGEX` names keys that interface 1 reserves (below): the fragment is empty when checking an implementation's output and REGEX when checking an expectation written for a missing feature. `line KIND VIEWS REGEX` is one form of a KIND line in the views listed (`*` for all three), matched against the whole line. `structure VIEW REGEX` is matched against a whole output's kinds, each preceded by a space. The regexes use no backreference or lookaround.

```grammar
define N 0|[1-9][0-9]*
define INT -?(?:0|[1-9][0-9]*)
define RGB #[0-9a-f]{6}
define CODE [0-9]|1[0-5]|#[0-9a-f]{6}
define F7 (?:\|ALTERNATE)?
define F6 (?:\|FIXED_POS)?{F7}
define F5 (?:\|HIDE_CURSOR)?{F6}
define F4 (?:\|INVERSE)?{F5}
define F3 (?:\|REL_ORIGIN)?{F4}
define F2 (?:\|AUTO_WRAP)?{F3}
define FLAGS INSERT_MODE{F2}|AUTO_WRAP{F3}|REL_ORIGIN{F4}|INVERSE{F5}|HIDE_CURSOR{F6}|FIXED_POS{F7}|ALTERNATE
define HOSTFLAGS -|inflate(?:,png)?(?:,exact)?(?:,discard)?|png(?:,exact)?(?:,discard)?|exact(?:,discard)?|discard
define NAME (?:[!-\[\]-~]|\\x[0-9a-f]{2})*
define CMD [a-z][a-z0-9-]*
define PALETTE legacy|vga|nord|solarized|solarized-black|solarized-white|soft-black|base16-dark|base16-light
define STYLE default|block-blink|block-steady|underline-blink|underline-steady|bar-blink|bar-steady
reserved RESERVED (?: overline)?(?: superscript| subscript)?
define ATTR (?: fg={CODE})?(?: bg={CODE})?(?: bold)?(?: italic)?(?: underline(?:=double|=curly|=dotted|=dashed)?)?(?: ul={CODE})?(?: inverse)?(?: protect)?(?: blink)?(?: dim)?(?: strike)?{RESERVED}
define FCELL w=[0-2] fg={RGB} bg={RGB} ul={RGB}(?: ulset)?(?: underline=(?:single|double|curly|dotted|dashed))?(?: bold)?(?: italic)?(?: dim)?(?: blink)?(?: inverse)?(?: protect)?(?: strike)?{RESERVED}
define PLACEMENT {N} row {INT} col {N} box {N}x{N} at {N},{N} size {N}x{N} src {N},{N} {N}x{N} cell {N}x{N} z {INT} (?:below-bg|below-text|above-text) (?:sixel|kitty) image {N}(?: opaque)?
line checkpoint * checkpoint {N}
line size * size [1-9][0-9]*x[1-9][0-9]*
line cursor * cursor {N},{N}
line flags * flags (?:none|{FLAGS})
line reply * reply (?:[0-9a-f]{2})+
line host: * host: [1-9][0-9]* t=[dfts] flags=(?:{HOSTFLAGS}) O={N} max={N} min={N} name={NAME} len={N}
line test * test (?:{CMD} error -[1-9][0-9]*|frame-since [0-7] changed {N}|sync-frames {N})
line palette draw2,frame palette (?:{PALETTE}) fg={RGB} bg={RGB}
line changed frame changed {N}
line frame-cursor frame frame-cursor {N},{N} (?:{STYLE}) (?:hidden|visible {FCELL})
line row * row {N} \|.*\|
line attr screen attr {N} {N}-{N}{ATTR}
line cells draw2 cells {N} {N}-{N} fg={RGB} bg={RGB} ul={RGB}(?: ulset)?(?: bold)?(?: italic)?(?: underline)?(?: blink)?
line cells frame cells {N} {N}-{N} {FCELL}
line placement screen,draw2 placement {PLACEMENT}
line placement frame placement {PLACEMENT}(?: changed)?
line image * image {N} {N}x{N} digest [0-9a-f]{8}
define HEAD (?: size)(?: cursor)(?: flags)(?: reply| host:)*
define IMAGES (?: placement)*(?: image)*
define SCREEN {HEAD}(?: row(?: attr)*)+{IMAGES}
define DRAW2 {HEAD}(?: palette)(?: row(?: cells)*)+{IMAGES}
define FRAME {HEAD}(?: palette)(?: changed)(?: frame-cursor)(?: row(?: cells)*)+{IMAGES}
structure screen (?:(?: test)*(?: checkpoint){SCREEN})+|(?: test)*{SCREEN}
structure draw2 (?:(?: test)*(?: checkpoint){DRAW2})+|(?: test)*{DRAW2}
structure frame (?:(?: test)*(?: checkpoint){FRAME})+|(?: test)*{FRAME}
```

The check also holds the numbering rules of Blocks: `row` lines number from 0 in order, as many as the view prints, each `attr` or `cells` line carries its row's number, a row's runs are in order, apart and within the columns, placements and images number from 0 and come after the last row, and checkpoint offsets do not decrease.

### Palettes

Each line is a palette's name, its colours 0 to 15, its default foreground and its default background.

```palettes
legacy 000000 cd0000 00cd00 cdcd00 0000ee cd00cd 00cdcd e5e5e5 7f7f7f ff0000 00ff00 ffff00 5c5cff ff00ff 00ffff ffffff e5e5e5 000000
vga 000000 aa0000 00aa00 aa5500 0000aa aa00aa 00aaaa aaaaaa 555555 ff5555 55ff55 ffff55 5555ff ff55ff 55ffff ffffff aaaaaa 000000
nord 3b4252 bf616a a3be8c ebcb8b 81a1c1 b48ead 88c0d0 e5e9f0 4c566a bf616a a3be8c ebcb8b 81a1c1 b48ead 8fbcbb eceff4 d8dee9 2e3440
solarized 073642 dc322f 859900 b58900 268bd2 d33682 2aa198 eee8d5 002b36 cb4b16 586e75 657b83 839496 6c71c4 93a1a1 fdf6e3 eee8d5 073642
solarized-black 000000 dc322f 859900 b58900 268bd2 d33682 2aa198 eee8d5 002b36 cb4b16 586e75 657b83 839496 6c71c4 93a1a1 fdf6e3 eee8d5 000000
solarized-white 073642 dc322f 859900 b58900 268bd2 d33682 2aa198 eee8d5 002b36 cb4b16 586e75 657b83 839496 6c71c4 93a1a1 fdf6e3 073642 eee8d5
soft-black 3f3f3f 705050 60b48a dfaf8f 9ab8d7 dc8cc3 8cd0d3 ffffff 709080 dca3a3 72d5a3 f0dfaf 94bff3 ec93d3 93e0e3 dcdccc dcdccc 2c2c2c
base16-dark 000000 ab4642 a1b56c f7ca88 7cafc2 ba8baf 86c1b9 aaaaaa 555555 ab4642 a1b56c f7ca88 7cafc2 ba8baf 86c1b9 ffffff d8d8d8 181818
base16-light 000000 ab4642 a1b56c f7ca88 7cafc2 ba8baf 86c1b9 aaaaaa 555555 ab4642 a1b56c f7ca88 7cafc2 ba8baf 86c1b9 ffffff 181818 d8d8d8
```

The C reference erased the screen and reset the current attributes when a palette was chosen; `host-palette` records it, and an implementation may list it as a departure.

### Reserved keys

These keys belong to features interface 1 lacks. No implementation of interface 1 prints them, and an expectation written for the missing feature may use them, at the place the grammar's `RESERVED` fragment gives: after `strike` in `attr` and frame `cells` lines.

| Key | Meaning |
|---|---|
| `overline` | SGR 53 set it, and SGR 55 clears it. |
| `superscript` | SGR 73 set it, and SGR 75 clears it. |
| `subscript` | SGR 74 set it, and SGR 75 clears it. |

## Versioning and features

`--describe` prints `interface MAJOR.MINOR`, then a `view NAME` line for each view, a `feature NAME` line for each feature, and a `command NAME` line for each command, followed by the command's feature when it has one, in the order shown below, which is part of the contract. An implementation with every feature prints:

```describe
interface 1.0
view screen
view frame
view draw2
feature attrs-extended
feature view-frame
feature view-draw2
feature images
feature sixel
feature kitty-graphics
feature image-host
feature sync
feature identify
feature input
feature focus
feature palette
feature query
feature damage-exact
command resize
command sb-max
command sb-up
command sb-down
command sb-reset
command cell images
command limit images
command image images
command place images
command sixel sixel
command graphic-max sixel
command registers sixel
command kitty-graphics kitty-graphics
command host image-host
command sync sync
command sync-frames sync
command version identify
command termcap identify
command key input
command mouse input
command paste input
command focus focus
command palette palette
command frame-since query
```

| Feature | What declaring it means |
|---|---|
| `attrs-extended` | The screen view prints `underline=STYLE`, `ul=` and `strike`. |
| `view-frame` | `--frame` prints the frame view. |
| `view-draw2` | `--draw2` prints the draw2 view. |
| `images` | Blocks end with `placement` and `image` lines; `cell`, `limit`, `image` and `place`. |
| `sixel` | Sixel strings make images; `sixel`, `graphic-max` and `registers`. |
| `kitty-graphics` | The kitty graphics protocol makes images; `kitty-graphics`. |
| `image-host` | The fixture host answers requests, which print `host:` lines; `host`. |
| `sync` | Mode 2026 holds frames; `sync` and `sync-frames`. |
| `identify` | `version` and `termcap`. |
| `input` | `key`, `mouse` and `paste`. |
| `focus` | `focus`. |
| `palette` | `palette`. |
| `query` | `frame-since`. |
| `damage-exact` | `changed N` lines, frame-since counts and placement `changed` flags follow the C reference's age rule, so a runner compares them unmasked. An implementation may decline it. |

Interface 1 makes every feature mandatory except `view-draw2`, whose goldens retire with the draw2 view, `damage-exact`, and the image features `images`, `sixel`, `kitty-graphics` and `image-host`, which an implementation built with images owes.

A change that adds a command, a key, a feature, or a line that only a new command or feature prints raises MINOR, and every existing golden stays as it is. A change to an existing line raises MAJOR, and the goldens re-made for it land in a commit of their own. A runner refuses an implementation of another MAJOR. A probe that needs a feature the implementation does not declare fails as absent.

## Worked examples

Each `example NAME` block is an input, and the `output NAME` block is what an implementation prints for it. In an example, `size COLS ROWS` and `view VIEW` give the command line (the screen view by default), `needs FEATURE...` the features it needs, `send "..."` appends bytes, `cmd TEXT` appends the test command TEXT, and `check` puts a cut at the length so far. In `send`, `\e`, `\r`, `\n`, `\t`, `\\`, `\"` and `\xHH` stand for one byte each, and every other character for its UTF-8.

The screen view: attributes in runs, a wide character, and a reply.

```example screen
size 12 3
needs attrs-extended
send "\e[1;31mred\e[0m \e[4:3;58:2::0:128:255mwavy\e[0m\r\n中x\e[6n"
```

```output screen
size 12x3
cursor 3,1
flags AUTO_WRAP
reply 1b5b323b3452
row 0 |red wavy|
attr 0 0-2 fg=1 bold
attr 0 4-7 underline=curly ul=#0080ff
row 1 |中x|
row 2 ||
```

Checkpoints and test commands: each `test` line comes before the block that follows its command, and a key's bytes are a reply.

```example commands
size 6 1
needs input query sync
send "a"
cmd key;sym=62;unicode=62
check
cmd frame-since;caller=0
cmd frame-since;caller=0
send "\e[?2026hb\e[?2026l"
cmd sync-frames
```

```output commands
checkpoint 35
size 6x1
cursor 1,0
flags AUTO_WRAP
reply 62
row 0 |a|
test frame-since 0 changed 6
test frame-since 0 changed 0
test sync-frames 1
checkpoint 142
size 6x1
cursor 2,0
flags AUTO_WRAP
row 0 |ab|
```

The frame view: a reverse-video cell, a wide character and its trailing half, and a cluster.

```example frame
size 6 2
view frame
needs view-frame
send "\e[7mx\e[0m中e\xcc\x81"
```

```output frame
size 6x2
cursor 4,0
flags AUTO_WRAP
palette legacy fg=#e5e5e5 bg=#000000
changed 12
frame-cursor 4,0 default visible w=1 fg=#000000 bg=#e5e5e5 ul=#000000
row 0 |x中é|
cells 0 0-0 w=1 fg=#000000 bg=#e5e5e5 ul=#000000 inverse
cells 0 1-1 w=2 fg=#e5e5e5 bg=#000000 ul=#e5e5e5
cells 0 2-2 w=0 fg=#e5e5e5 bg=#000000 ul=#e5e5e5
row 1 ||
```

The draw2 view of the same stream.

```example draw2
size 6 2
view draw2
needs view-draw2
send "\e[7mx\e[0m中e\xcc\x81"
```

```output draw2
size 6x2
cursor 4,0
flags AUTO_WRAP
palette legacy fg=#e5e5e5 bg=#000000
row 0 |x中 e|
cells 0 0-0 fg=#000000 bg=#e5e5e5 ul=#000000
row 1 ||
```

A kitty image of one cell, placed by test commands.

```example image
size 4 1
view frame
needs images view-frame
cmd image;n=1;w=8;h=16;kitty=1;fill=ff0000ff
cmd place;img=1;col=1;row=0
```

```output image
size 4x1
cursor 0,0
flags AUTO_WRAP
palette legacy fg=#e5e5e5 bg=#000000
changed 4
frame-cursor 0,0 default visible w=1 fg=#000000 bg=#e5e5e5 ul=#000000
row 0 ||
placement 0 row 0 col 1 box 1x1 at 0,0 size 8x16 src 0,0 8x16 cell 8x16 z 0 above-text kitty image 0 opaque changed
image 0 8x16 digest da180ec5
```
