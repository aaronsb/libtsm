/*
 * tsm-replay - replay a recorded byte stream into libtsm and print the
 * resulting screen state as text.
 *
 * The text is the conformance contract (ADR-301 tier 1 in the kmscon fork):
 * the same stream must produce the same text before and after a change to the
 * screen model, unless the change is meant to alter it. Only the public API is
 * used, so the tool builds against any libtsm, including upstream releases.
 *
 * Usage: tsm-replay [--draw2 | --frame] COLS ROWS FILE [CUTS]
 *
 * CUTS, if given, is a file of byte offsets into FILE, one per line, in
 * increasing order. The screen state is printed at each offset (and at the end
 * of FILE), each block headed "checkpoint OFFSET", so a recording of a
 * full-screen program can be checked at every screen it showed, not only after
 * it exits.
 *
 * Output, one item per line:
 *   size COLSxROWS
 *   cursor X,Y
 *   flags NAME|NAME...        (screen flags, "none" if clear)
 *   reply HEX                 (bytes libtsm wrote back, e.g. to DA; omitted if none)
 *   row N |TEXT|              (cells left to right, trailing blanks trimmed)
 *   attr N A-B KEY...         (a run of cells A..B on row N with non-default attributes)
 *
 * In TEXT a cell with no character is a space, a cell holding several
 * codepoints prints them all, and the cell after a wide character prints
 * nothing, so wide characters keep their row aligned in a monospace view. A
 * cell with more than MAX_CP codepoints is an error (exit 3), so a cluster is
 * never truncated silently.
 *
 * KEY is fg=, bg= (a palette index or #rrggbb), or a flag: bold, italic,
 * underline, inverse, protect, blink, dim. Against a libtsm that declares
 * TSM_SCREEN_ATTR_UNDERLINE_STYLE it also prints, only when set:
 *   underline=STYLE           (in place of "underline" for double, curly, dotted, dashed)
 *   ul=                       (underline colour set by SGR 58, as fg=)
 *   strike
 *
 * --draw2 prints the screen through tsm_screen_draw2 instead, the cells a
 * renderer such as kmscon reads: colours resolved to RGB through the palette,
 * with reverse video, dim and the underline colour applied. It needs the fork's
 * libtsm (TSM_SCREEN_CELL_UNDERLINE_COLOR) and exits 2 elsewhere. The size,
 * cursor, flags and reply lines are as above, then:
 *   palette NAME fg=#rrggbb bg=#rrggbb   (the palette and its default colours)
 *   row N |TEXT|              (each cell's codepoint as draw2 gives it: the
 *                              base of a cluster, a space for an empty cell or
 *                              the trailing half of a wide one; trailing blanks
 *                              trimmed)
 *   cells N A-B fg=#rrggbb bg=#rrggbb ul=#rrggbb[ ulset] FLAG...
 *                             (a run of cells A..B on row N that differs from
 *                              the default cell: default fg and bg, underline
 *                              in the foreground, no flags; FLAG is bold,
 *                              italic, underline or blink)
 *
 * --frame prints the screen through tsm_screen_frame, the call that replaces
 * draw2 (TSM_SCREEN_FRAME; exits 2 without it), with the same palette:
 *   palette NAME fg=#rrggbb bg=#rrggbb
 *   changed N                 (cells flagged changed since the previous frame;
 *                              the first frame of a run flags all of them)
 *   frame-cursor X,Y STYLE visible|hidden[ KEY...]
 *                             (the cursor; when visible, the cell it draws, as
 *                              in a cells line)
 *   row N |TEXT|              (as the default view: a cluster prints all its
 *                              codepoints, a trailing half nothing)
 *   cells N A-B w=W fg=#rrggbb bg=#rrggbb ul=#rrggbb KEY...
 *                             (a run of cells that differs from the default
 *                              cell: width 1, default fg and bg, underline in
 *                              the foreground, no attributes; KEY is ulset,
 *                              underline=STYLE, bold, italic, dim, blink,
 *                              inverse, protect or strike)
 *
 * Against a libtsm that declares TSM_SCREEN_IMAGES every view ends with the
 * placements a frame lists, in draw order, and the images they show, each
 * numbered in that order so no key enters a golden; nothing when there are
 * none:
 *   placement N row R col C box CxR at X,Y size WxH src X,Y WxH cell WxH
 *             z Z BAND KIND image M[ opaque][ changed]
 *                             (one line: R is the visible row of the box's
 *                              top, negative when it is above the view, as
 *                              for a placement anchored in scrollback; BAND is
 *                              below-bg, below-text or above-text; KIND sixel
 *                              or kitty; changed only in the --frame view)
 *   image M WxH digest HEX    (FNV-1a of the pixels, each as A, R, G, B)
 *
 * There, too, the stream may hold test commands, which no protocol has yet
 * (ADR-103 step 3a): an APC "ESC _ tsm-test;CMD;KEY=VALUE... ESC \" runs CMD
 * through libtsm's public and test-only calls in place of being fed to the
 * vte. Elsewhere it reaches the vte, which ignores it. A command that fails
 * prints "test CMD error N" (a negative errno) where it ran. CMD and its keys:
 *   cell;w=;h=                tsm_screen_set_cell_size
 *   limit;b=                  tsm_screen_set_image_limit
 *   sb-max;n=                 tsm_screen_set_max_sb
 *   resize;c=;r=              tsm_screen_resize
 *   sb-up;n= | sb-down;n= | sb-reset
 *                             scroll the view through scrollback
 *   image;n=;w=;h=[;kitty=1;id=;tr=1;fill=AARRGGBB]
 *                             an image of one premultiplied colour (default
 *                             ff808080), a sixel unless kitty=1 (id defaults
 *                             to n), called n in later commands
 *   place;img=[;col=;row=;x=;y=;c=;r=;sx=;sy=;sw=;sh=;z=;p=]
 *                             a placement of image n at the cursor unless col
 *                             and row are given
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libtsm.h"

#define MAX_CP 32

struct cell {
	uint32_t cp[MAX_CP];
	size_t len;
	unsigned int width;
	bool drawn;
	struct tsm_screen_attr attr;
};

struct grid {
	unsigned int cols, rows;
	struct cell *cells;
};

struct reply {
	unsigned char *buf;
	size_t len, cap;
};

static void on_write(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	struct reply *r = data;

	(void)vte;
	if (r->len + len > r->cap) {
		r->cap = (r->len + len) * 2;
		r->buf = realloc(r->buf, r->cap);
		if (!r->buf) {
			perror("realloc");
			exit(2);
		}
	}
	memcpy(r->buf + r->len, u8, len);
	r->len += len;
}

static int on_draw(struct tsm_screen *con, uint64_t id, const uint32_t *ch, size_t len,
		   unsigned int width, unsigned int posx, unsigned int posy,
		   const struct tsm_screen_attr *attr, tsm_age_t age, void *data)
{
	struct grid *g = data;
	struct cell *c;

	(void)con;
	(void)id;
	(void)age;
	if (posx >= g->cols || posy >= g->rows)
		return 0;
	if (len > MAX_CP) {
		fprintf(stderr, "tsm-replay: cell %u,%u holds %zu codepoints, more than %d\n",
			posx, posy, len, MAX_CP);
		exit(3);
	}
	c = &g->cells[posy * g->cols + posx];
	c->len = len;
	memcpy(c->cp, ch, c->len * sizeof(*ch));
	c->width = width;
	c->drawn = true;
	c->attr = *attr;
	return 0;
}

static void put_utf8(uint32_t cp)
{
	if (cp < 0x80) {
		putchar((int)cp);
	} else if (cp < 0x800) {
		putchar(0xc0 | (cp >> 6));
		putchar(0x80 | (cp & 0x3f));
	} else if (cp < 0x10000) {
		putchar(0xe0 | (cp >> 12));
		putchar(0x80 | ((cp >> 6) & 0x3f));
		putchar(0x80 | (cp & 0x3f));
	} else {
		putchar(0xf0 | (cp >> 18));
		putchar(0x80 | ((cp >> 12) & 0x3f));
		putchar(0x80 | ((cp >> 6) & 0x3f));
		putchar(0x80 | (cp & 0x3f));
	}
}

static bool attr_is_default(const struct tsm_screen_attr *a)
{
	return a->fccode == TSM_COLOR_FOREGROUND && a->bccode == TSM_COLOR_BACKGROUND &&
	       !a->bold && !a->italic && !a->underline && !a->inverse && !a->protect &&
	       !a->blink && !a->dim
#ifdef TSM_SCREEN_ATTR_UNDERLINE_STYLE
	       && !a->ucset && !a->strike
#endif
		;
}

static bool attr_equal(const struct tsm_screen_attr *a, const struct tsm_screen_attr *b)
{
	return a->fccode == b->fccode && a->bccode == b->bccode &&
	       (a->fccode >= 0 || (a->fr == b->fr && a->fg == b->fg && a->fb == b->fb)) &&
	       (a->bccode >= 0 || (a->br == b->br && a->bg == b->bg && a->bb == b->bb)) &&
	       a->bold == b->bold && a->italic == b->italic && a->underline == b->underline &&
	       a->inverse == b->inverse && a->protect == b->protect && a->blink == b->blink &&
	       a->dim == b->dim
#ifdef TSM_SCREEN_ATTR_UNDERLINE_STYLE
	       && a->underline_style == b->underline_style && a->strike == b->strike &&
	       a->ucset == b->ucset &&
	       (!a->ucset || (a->uccode == b->uccode &&
			      (a->uccode >= 0 || (a->ur == b->ur && a->ug == b->ug && a->ub == b->ub))))
#endif
		;
}

static void print_color(const char *key, int8_t code, uint8_t r, uint8_t g, uint8_t b)
{
	if (code < 0)
		printf(" %s=#%02x%02x%02x", key, r, g, b);
	else
		printf(" %s=%d", key, code);
}

static void print_attr(unsigned int row, unsigned int from, unsigned int to,
		       const struct tsm_screen_attr *a)
{
	printf("attr %u %u-%u", row, from, to);
	if (a->fccode != TSM_COLOR_FOREGROUND)
		print_color("fg", a->fccode, a->fr, a->fg, a->fb);
	if (a->bccode != TSM_COLOR_BACKGROUND)
		print_color("bg", a->bccode, a->br, a->bg, a->bb);
	if (a->bold)
		printf(" bold");
	if (a->italic)
		printf(" italic");
	if (a->underline) {
#ifdef TSM_SCREEN_ATTR_UNDERLINE_STYLE
		static const char *const styles[] = {
			[TSM_UNDERLINE_DOUBLE] = "double", [TSM_UNDERLINE_CURLY] = "curly",
			[TSM_UNDERLINE_DOTTED] = "dotted", [TSM_UNDERLINE_DASHED] = "dashed",
		};

		if (a->underline_style < sizeof(styles) / sizeof(styles[0]) &&
		    styles[a->underline_style])
			printf(" underline=%s", styles[a->underline_style]);
		else
#endif
			printf(" underline");
	}
#ifdef TSM_SCREEN_ATTR_UNDERLINE_STYLE
	if (a->ucset)
		print_color("ul", a->uccode, a->ur, a->ug, a->ub);
#endif
	if (a->inverse)
		printf(" inverse");
	if (a->protect)
		printf(" protect");
	if (a->blink)
		printf(" blink");
	if (a->dim)
		printf(" dim");
#ifdef TSM_SCREEN_ATTR_UNDERLINE_STYLE
	if (a->strike)
		printf(" strike");
#endif
	putchar('\n');
}

static void print_flags(unsigned int flags)
{
	static const struct {
		unsigned int bit;
		const char *name;
	} names[] = {
		{ TSM_SCREEN_INSERT_MODE, "INSERT_MODE" }, { TSM_SCREEN_AUTO_WRAP, "AUTO_WRAP" },
		{ TSM_SCREEN_REL_ORIGIN, "REL_ORIGIN" },   { TSM_SCREEN_INVERSE, "INVERSE" },
		{ TSM_SCREEN_HIDE_CURSOR, "HIDE_CURSOR" }, { TSM_SCREEN_FIXED_POS, "FIXED_POS" },
		{ TSM_SCREEN_ALTERNATE, "ALTERNATE" },
	};
	bool first = true;
	size_t i;

	printf("flags ");
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (flags & names[i].bit) {
			printf("%s%s", first ? "" : "|", names[i].name);
			first = false;
		}
	}
	printf("%s\n", first ? "none" : "");
}

static void print_row(const struct grid *g, unsigned int row)
{
	const struct cell *line = &g->cells[row * g->cols];
	unsigned int last = 0, x, skip = 0, start;
	size_t i;

	for (x = 0; x < g->cols; x++)
		if (line[x].len > 0)
			last = x + 1;

	printf("row %u |", row);
	for (x = 0; x < last; x++) {
		if (skip) {
			skip--;
			continue;
		}
		if (line[x].len == 0)
			putchar(' ');
		for (i = 0; i < line[x].len; i++)
			put_utf8(line[x].cp[i]);
		if (line[x].width > 1)
			skip = line[x].width - 1;
	}
	printf("|\n");

	for (x = 0; x < g->cols; x = start) {
		start = x + 1;
		if (!line[x].drawn || attr_is_default(&line[x].attr))
			continue;
		while (start < g->cols && line[start].drawn &&
		       attr_equal(&line[start].attr, &line[x].attr))
			start++;
		print_attr(row, x, start - 1, &line[x].attr);
	}
}

static unsigned char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf = NULL;
	size_t cap = 0, n;

	if (!f) {
		fprintf(stderr, "tsm-replay: %s: %s\n", path, strerror(errno));
		exit(2);
	}
	*len = 0;
	do {
		if (*len == cap) {
			cap = cap ? cap * 2 : 65536;
			buf = realloc(buf, cap);
			if (!buf) {
				perror("realloc");
				exit(2);
			}
		}
		n = fread(buf + *len, 1, cap - *len, f);
		*len += n;
	} while (n > 0);
	fclose(f);
	return buf;
}

#ifdef TSM_SCREEN_IMAGES
/* images the test commands made, by the number the stream gives them */
#define TEST_IMAGES 64
static uint64_t test_images[TEST_IMAGES];
/* tsm-replay's own frame age, so printing placements leaves the --frame
 * view's changes as they are */
static struct tsm_frame_age image_age;

static uint32_t fnv1a(uint32_t h, uint8_t byte)
{
	return (h ^ byte) * 16777619u;
}

/*
 * The placements @f lists and the images they show, numbered in draw order.
 * @changed prints each placement's changed flag, which only the --frame view
 * judges.
 */
static void dump_images(struct tsm_screen *screen, const struct tsm_frame *f, bool changed)
{
	static const char *const bands[] = { "below-bg", "below-text", "above-text" };
	const struct tsm_frame_placement *p;
	struct tsm_frame_image img;
	uint64_t *keys;
	unsigned int i, j, n = 0, x, y;
	uint32_t h, px;

	if (!f->placement_count)
		return;
	keys = calloc(f->placement_count, sizeof(*keys));
	if (!keys) {
		perror("calloc");
		exit(2);
	}
	for (i = 0; i < f->placement_count; i++) {
		p = tsm_frame_placement_at(f, i);
		for (j = 0; j < n && keys[j] != p->image_key; j++)
			;
		if (j == n)
			keys[n++] = p->image_key;
		printf("placement %u row %d col %u box %ux%u at %u,%u size %ux%u src %u,%u %ux%u"
		       " cell %ux%u z %d %s %s image %u%s%s\n",
		       i, p->row, p->col, p->cols, p->rows, p->x, p->y, p->width, p->height,
		       p->src_x, p->src_y, p->src_width, p->src_height, p->cell_width,
		       p->cell_height, (int)p->z, p->band < 3 ? bands[p->band] : "unknown",
		       p->kind == TSM_IMAGE_SIXEL ? "sixel" : "kitty", j,
		       (p->flags & TSM_FRAME_OPAQUE) ? " opaque" : "",
		       changed && (p->flags & TSM_FRAME_CHANGED) ? " changed" : "");
	}
	for (j = 0; j < n; j++) {
		if (tsm_screen_frame_image(screen, keys[j], &img) < 0) {
			fprintf(stderr, "tsm-replay: image of placement key %llx does not read\n",
				(unsigned long long)keys[j]);
			exit(3);
		}
		h = 2166136261u;
		for (y = 0; y < img.height; y++) {
			for (x = 0; x < img.width; x++) {
				px = ((const uint32_t *)((const char *)img.pixels +
							 y * img.stride))[x];
				h = fnv1a(h, px >> 24);
				h = fnv1a(h, px >> 16);
				h = fnv1a(h, px >> 8);
				h = fnv1a(h, px);
			}
		}
		printf("image %u %ux%u digest %08x\n", j, img.width, img.height, h);
	}
	free(keys);
}

/* the value of KEY= among @argc tokens, or @def */
static long test_arg(char **argv, int argc, const char *key, long def)
{
	size_t len = strlen(key);
	int i;

	for (i = 0; i < argc; i++)
		if (!strncmp(argv[i], key, len) && argv[i][len] == '=')
			return strtol(argv[i] + len + 1, NULL, !strcmp(key, "fill") ? 16 : 10);
	return def;
}

static int test_image_cmd(struct tsm_screen *screen, char **argv, int argc)
{
	struct tsm_test_image img = { 0 };
	long n = test_arg(argv, argc, "n", 0);
	uint32_t fill = (uint32_t)test_arg(argv, argc, "fill", 0xff808080);
	uint32_t *px;
	size_t i;
	int r;

	if (n < 0 || n >= TEST_IMAGES)
		return -EINVAL;
	img.width = (unsigned int)test_arg(argv, argc, "w", 0);
	img.height = (unsigned int)test_arg(argv, argc, "h", 0);
	img.kind = test_arg(argv, argc, "kitty", 0) ? TSM_IMAGE_KITTY : TSM_IMAGE_SIXEL;
	if (img.kind == TSM_IMAGE_KITTY)
		img.id = (uint32_t)test_arg(argv, argc, "id", n);
	img.transient = test_arg(argv, argc, "tr", 0);
	px = malloc(sizeof(*px) * img.width * img.height + 1);
	if (!px) {
		perror("malloc");
		exit(2);
	}
	for (i = 0; i < (size_t)img.width * img.height; i++)
		px[i] = fill;
	img.pixels = px;
	r = tsm_screen_test_image(screen, &img, &test_images[n]);
	free(px);
	return r;
}

static int test_place_cmd(struct tsm_screen *screen, char **argv, int argc)
{
	struct tsm_test_placement pl = { 0 };
	uint64_t key;
	long n = test_arg(argv, argc, "img", 0);

	if (n < 0 || n >= TEST_IMAGES)
		return -EINVAL;
	pl.image = test_images[n];
	pl.id = (uint32_t)test_arg(argv, argc, "p", 0);
	pl.col = (unsigned int)test_arg(argv, argc, "col", tsm_screen_get_cursor_x(screen));
	pl.row = (unsigned int)test_arg(argv, argc, "row", tsm_screen_get_cursor_y(screen));
	pl.x = (unsigned int)test_arg(argv, argc, "x", 0);
	pl.y = (unsigned int)test_arg(argv, argc, "y", 0);
	pl.cols = (unsigned int)test_arg(argv, argc, "c", 0);
	pl.rows = (unsigned int)test_arg(argv, argc, "r", 0);
	pl.src_x = (unsigned int)test_arg(argv, argc, "sx", 0);
	pl.src_y = (unsigned int)test_arg(argv, argc, "sy", 0);
	pl.src_width = (unsigned int)test_arg(argv, argc, "sw", 0);
	pl.src_height = (unsigned int)test_arg(argv, argc, "sh", 0);
	pl.z = (int32_t)test_arg(argv, argc, "z", 0);
	return tsm_screen_test_place(screen, &pl, &key);
}

/* runs the test command in @cmd, @len bytes after "tsm-test;" */
static void test_command(struct tsm_screen *screen, const char *cmd, size_t len)
{
	char buf[512], *argv[32], *tok, *save;
	int argc = 0, r = 0;

	if (len >= sizeof(buf)) {
		fprintf(stderr, "tsm-replay: test command too long\n");
		exit(2);
	}
	memcpy(buf, cmd, len);
	buf[len] = 0;
	for (tok = strtok_r(buf, ";", &save); tok && argc < 32; tok = strtok_r(NULL, ";", &save))
		argv[argc++] = tok;
	if (!argc) {
		fprintf(stderr, "tsm-replay: empty test command\n");
		exit(2);
	}

	if (!strcmp(argv[0], "cell")) {
		r = tsm_screen_set_cell_size(screen, test_arg(argv, argc, "w", 0),
					     test_arg(argv, argc, "h", 0));
	} else if (!strcmp(argv[0], "limit")) {
		tsm_screen_set_image_limit(screen, (size_t)test_arg(argv, argc, "b", 0));
	} else if (!strcmp(argv[0], "sb-max")) {
		tsm_screen_set_max_sb(screen, test_arg(argv, argc, "n", 0));
	} else if (!strcmp(argv[0], "resize")) {
		r = tsm_screen_resize(screen, test_arg(argv, argc, "c", 0),
				      test_arg(argv, argc, "r", 0));
	} else if (!strcmp(argv[0], "sb-up")) {
		tsm_screen_sb_up(screen, test_arg(argv, argc, "n", 1));
	} else if (!strcmp(argv[0], "sb-down")) {
		tsm_screen_sb_down(screen, test_arg(argv, argc, "n", 1));
	} else if (!strcmp(argv[0], "sb-reset")) {
		tsm_screen_sb_reset(screen);
	} else if (!strcmp(argv[0], "image")) {
		r = test_image_cmd(screen, argv, argc);
	} else if (!strcmp(argv[0], "place")) {
		r = test_place_cmd(screen, argv, argc);
	} else {
		fprintf(stderr, "tsm-replay: unknown test command %s\n", argv[0]);
		exit(2);
	}
	if (r < 0)
		printf("test %s error %d\n", argv[0], r);
}
#endif

#ifdef TSM_SCREEN_IMAGES
/* the first @nlen bytes @needle in @len bytes at @hay, or NULL */
static const char *find_bytes(const char *hay, size_t len, const char *needle, size_t nlen)
{
	size_t i;

	for (i = 0; i + nlen <= len; i++)
		if (!memcmp(hay + i, needle, nlen))
			return hay + i;
	return NULL;
}
#endif

/*
 * Feeds @len bytes to the vte. Where the fork has images, a test command in
 * them runs in its place; a command must lie wholly between two checkpoints.
 */
static void feed(struct tsm_screen *screen, struct tsm_vte *vte, const char *data, size_t len)
{
#ifdef TSM_SCREEN_IMAGES
	static const char intro[] = "\x1b_tsm-test;";
	const char *start, *end;

	while ((start = find_bytes(data, len, intro, sizeof(intro) - 1))) {
		tsm_vte_input(vte, data, start - data);
		start += sizeof(intro) - 1;
		end = find_bytes(start, len - (start - data), "\x1b\\", 2);
		if (!end) {
			fprintf(stderr, "tsm-replay: test command without ST\n");
			exit(2);
		}
		test_command(screen, start, end - start);
		len -= end + 2 - data;
		data = end + 2;
	}
#endif
	(void)screen;
	tsm_vte_input(vte, data, len);
}

#ifdef TSM_SCREEN_CELL_UNDERLINE_COLOR
/* the palette the --draw2 view resolves through, set explicitly so a change of
 * libtsm's default palette shows as a change here */
#define DRAW2_PALETTE "legacy"

static bool color_equal(const struct tsm_screen_color *a, const struct tsm_screen_color *b)
{
	return a->r == b->r && a->g == b->g && a->b == b->b;
}

static bool cell2_equal(const struct tsm_screen_cell *a, const struct tsm_screen_cell *b)
{
	return color_equal(&a->fg, &b->fg) && color_equal(&a->bg, &b->bg) &&
	       color_equal(&a->ul, &b->ul) && a->ul_set == b->ul_set &&
	       a->attr2.u8 == b->attr2.u8;
}

static void dump_draw2(struct tsm_screen *screen, struct tsm_vte *vte)
{
	unsigned int cols = tsm_screen_get_width(screen), rows = tsm_screen_get_height(screen);
	const struct tsm_screen_cell *cells, *line, *c;
	struct tsm_screen_cell def;
	struct tsm_screen_attr attr;
	unsigned int row, x, last, start;

	tsm_vte_get_def_attr(vte, &attr);
	memset(&def, 0, sizeof(def));
	def.fg.r = attr.fr;
	def.fg.g = attr.fg;
	def.fg.b = attr.fb;
	def.bg.r = attr.br;
	def.bg.g = attr.bg;
	def.bg.b = attr.bb;
	def.ul = def.fg;
	printf("palette %s fg=#%02x%02x%02x bg=#%02x%02x%02x\n", DRAW2_PALETTE,
	       def.fg.r, def.fg.g, def.fg.b, def.bg.r, def.bg.g, def.bg.b);

	cells = tsm_screen_draw2(screen);
	if (!cells) {
		fprintf(stderr, "tsm-replay: tsm_screen_draw2 failed\n");
		exit(2);
	}

	for (row = 0; row < rows; row++) {
		line = &cells[row * cols];
		last = 0;
		for (x = 0; x < cols; x++)
			if (line[x].ch != ' ')
				last = x + 1;
		printf("row %u |", row);
		for (x = 0; x < last; x++)
			put_utf8(line[x].ch);
		printf("|\n");

		for (x = 0; x < cols; x = start) {
			start = x + 1;
			if (cell2_equal(&line[x], &def))
				continue;
			while (start < cols && cell2_equal(&line[start], &line[x]))
				start++;
			c = &line[x];
			printf("cells %u %u-%u fg=#%02x%02x%02x bg=#%02x%02x%02x ul=#%02x%02x%02x%s",
			       row, x, start - 1, c->fg.r, c->fg.g, c->fg.b,
			       c->bg.r, c->bg.g, c->bg.b, c->ul.r, c->ul.g, c->ul.b,
			       c->ul_set ? " ulset" : "");
			if (c->attr2.bold)
				printf(" bold");
			if (c->attr2.italic)
				printf(" italic");
			if (c->attr2.underline)
				printf(" underline");
			if (c->attr2.blink)
				printf(" blink");
			putchar('\n');
		}
	}
}
#endif

#ifdef TSM_SCREEN_FRAME
static const char *const underline_names[] = {
	"none", "single", "double", "curly", "dotted", "dashed",
};

static const char *cursor_style_name(enum tsm_screen_cursor_style style)
{
	static const char *const names[] = {
		"default", "block-blink", "block-steady", "underline-blink",
		"underline-steady", "bar-blink", "bar-steady",
	};

	return (unsigned int)style < sizeof(names) / sizeof(names[0]) ? names[style] : "unknown";
}

/* a cell's description without its glyph, its position or the changed flag */
static bool frame_cell_equal(const struct tsm_frame_cell *a, const struct tsm_frame_cell *b)
{
	return color_equal(&a->fg, &b->fg) && color_equal(&a->bg, &b->bg) &&
	       color_equal(&a->ul, &b->ul) && a->width == b->width &&
	       a->underline == b->underline &&
	       (a->flags & ~TSM_FRAME_CHANGED) == (b->flags & ~TSM_FRAME_CHANGED);
}

static void print_frame_cell(const struct tsm_frame_cell *c)
{
	static const struct {
		uint16_t bit;
		const char *name;
	} flags[] = {
		{ TSM_FRAME_BOLD, "bold" }, { TSM_FRAME_ITALIC, "italic" },
		{ TSM_FRAME_DIM, "dim" }, { TSM_FRAME_BLINK, "blink" },
		{ TSM_FRAME_INVERSE, "inverse" }, { TSM_FRAME_PROTECT, "protect" },
		{ TSM_FRAME_STRIKE, "strike" },
	};
	size_t i;

	printf(" w=%u fg=#%02x%02x%02x bg=#%02x%02x%02x ul=#%02x%02x%02x", c->width,
	       c->fg.r, c->fg.g, c->fg.b, c->bg.r, c->bg.g, c->bg.b, c->ul.r, c->ul.g, c->ul.b);
	if (c->flags & TSM_FRAME_UL_SET)
		printf(" ulset");
	if (c->underline)
		printf(" underline=%s", c->underline < 6 ? underline_names[c->underline] : "unknown");
	for (i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
		if (c->flags & flags[i].bit)
			printf(" %s", flags[i].name);
}

static void dump_frame(struct tsm_screen *screen, struct tsm_vte *vte)
{
	const struct tsm_frame *f = tsm_screen_frame(screen);
	const struct tsm_frame_cell *c;
	struct tsm_frame_cell def;
	struct tsm_screen_attr attr;
	unsigned int row, x, last, start, changed = 0;
	const uint32_t *cps;
	size_t len, i;

	if (!f) {
		fprintf(stderr, "tsm-replay: tsm_screen_frame failed\n");
		exit(2);
	}

	tsm_vte_get_def_attr(vte, &attr);
	memset(&def, 0, sizeof(def));
	def.width = 1;
	def.fg.r = attr.fr;
	def.fg.g = attr.fg;
	def.fg.b = attr.fb;
	def.bg.r = attr.br;
	def.bg.g = attr.bg;
	def.bg.b = attr.bb;
	def.ul = def.fg;
	printf("palette %s fg=#%02x%02x%02x bg=#%02x%02x%02x\n", DRAW2_PALETTE,
	       def.fg.r, def.fg.g, def.fg.b, def.bg.r, def.bg.g, def.bg.b);

	for (i = 0; i < (size_t)f->cols * f->rows; i++)
		if (tsm_frame_cell_at(f, i % f->cols, i / f->cols)->flags & TSM_FRAME_CHANGED)
			changed++;
	printf("changed %u\n", changed);

	printf("frame-cursor %u,%u %s %s", f->cursor.x, f->cursor.y,
	       cursor_style_name(f->cursor.style), f->cursor.visible ? "visible" : "hidden");
	if (f->cursor.visible)
		print_frame_cell(&f->cursor.cell);
	putchar('\n');

	/* cells are cell_size apart, which a later libtsm may grow */
#define CELL(x) tsm_frame_cell_at(f, (x), row)
	for (row = 0; row < f->rows; row++) {
		last = 0;
		for (x = 0; x < f->cols; x++)
			if (CELL(x)->key && CELL(x)->key != ' ')
				last = x + 1;
		printf("row %u |", row);
		for (x = 0; x < last; x++) {
			c = CELL(x);
			if (!c->width)
				continue;
			if (!c->key) {
				putchar(' ');
			} else if (c->key < TSM_FRAME_CLUSTER_KEY) {
				put_utf8((uint32_t)c->key);
			} else {
				cps = tsm_screen_frame_cluster(screen, c->key, &len);
				if (!cps) {
					fprintf(stderr, "tsm-replay: cluster key %llx does not read\n",
						(unsigned long long)c->key);
					exit(3);
				}
				for (i = 0; i < len; i++)
					put_utf8(cps[i]);
			}
		}
		printf("|\n");

		for (x = 0; x < f->cols; x = start) {
			start = x + 1;
			if (frame_cell_equal(CELL(x), &def))
				continue;
			while (start < f->cols && frame_cell_equal(CELL(start), CELL(x)))
				start++;
			printf("cells %u %u-%u", row, x, start - 1);
			print_frame_cell(CELL(x));
			putchar('\n');
		}
	}
#undef CELL
#ifdef TSM_SCREEN_IMAGES
	dump_images(screen, f, true);
#endif
}
#endif

enum view {
	VIEW_DRAW,
	VIEW_DRAW2,
	VIEW_FRAME,
};

static void dump(struct tsm_screen *screen, struct tsm_vte *vte, struct grid *grid,
		 struct reply *reply, enum view view)
{
	unsigned int flags = tsm_screen_get_flags(screen);
	unsigned int row;
	size_t i;

	printf("size %ux%u\n", tsm_screen_get_width(screen), tsm_screen_get_height(screen));
	printf("cursor %u,%u\n", tsm_screen_get_cursor_x(screen), tsm_screen_get_cursor_y(screen));
	print_flags(flags);
	if (reply->len) {
		printf("reply ");
		for (i = 0; i < reply->len; i++)
			printf("%02x", reply->buf[i]);
		putchar('\n');
		reply->len = 0;
	}

#ifdef TSM_SCREEN_CELL_UNDERLINE_COLOR
	if (view == VIEW_DRAW2) {
		dump_draw2(screen, vte);
#ifdef TSM_SCREEN_IMAGES
		dump_images(screen, tsm_screen_frame_since(screen, &image_age), false);
#endif
		return;
	}
#endif
#ifdef TSM_SCREEN_FRAME
	if (view == VIEW_FRAME) {
		dump_frame(screen, vte);
		return;
	}
#endif
	(void)vte;
	(void)view;

	/* The cursor is reported above; keep draw from painting it into a cell. */
	memset(grid->cells, 0, (size_t)grid->cols * grid->rows * sizeof(*grid->cells));
	tsm_screen_set_flags(screen, TSM_SCREEN_HIDE_CURSOR);
	tsm_screen_draw(screen, on_draw, grid);
	if (!(flags & TSM_SCREEN_HIDE_CURSOR))
		tsm_screen_reset_flags(screen, TSM_SCREEN_HIDE_CURSOR);

	for (row = 0; row < grid->rows; row++)
		print_row(grid, row);
#ifdef TSM_SCREEN_IMAGES
	dump_images(screen, tsm_screen_frame_since(screen, &image_age), false);
#endif
}

static size_t *read_cuts(const char *path, size_t len, size_t *count)
{
	FILE *f = fopen(path, "r");
	size_t *cuts = NULL, cap = 0, prev = 0;
	unsigned long long v;

	if (!f) {
		fprintf(stderr, "tsm-replay: %s: %s\n", path, strerror(errno));
		exit(2);
	}
	*count = 0;
	while (fscanf(f, "%llu", &v) == 1) {
		if (v < prev || v > len) {
			fprintf(stderr, "tsm-replay: %s: offset %llu out of order or past end\n", path, v);
			exit(2);
		}
		if (*count == cap) {
			cap = cap ? cap * 2 : 16;
			cuts = realloc(cuts, cap * sizeof(*cuts));
			if (!cuts) {
				perror("realloc");
				exit(2);
			}
		}
		cuts[(*count)++] = prev = (size_t)v;
	}
	fclose(f);
	return cuts;
}

int main(int argc, char **argv)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct reply reply = { 0 };
	struct grid grid;
	unsigned char *data;
	size_t len, pos = 0, ncuts = 0, i;
	size_t *cuts = NULL;
	enum view view = VIEW_DRAW;

	if (argc > 1 && !strcmp(argv[1], "--draw2")) {
#ifdef TSM_SCREEN_CELL_UNDERLINE_COLOR
		view = VIEW_DRAW2;
		argv++;
		argc--;
#else
		fprintf(stderr, "tsm-replay: --draw2 needs the fork's libtsm\n");
		return 2;
#endif
	} else if (argc > 1 && !strcmp(argv[1], "--frame")) {
#ifdef TSM_SCREEN_FRAME
		view = VIEW_FRAME;
		argv++;
		argc--;
#else
		fprintf(stderr, "tsm-replay: --frame needs the fork's libtsm\n");
		return 2;
#endif
	}
	if (argc != 4 && argc != 5) {
		fprintf(stderr, "usage: tsm-replay [--draw2 | --frame] COLS ROWS FILE [CUTS]\n");
		return 2;
	}
	grid.cols = (unsigned int)strtoul(argv[1], NULL, 10);
	grid.rows = (unsigned int)strtoul(argv[2], NULL, 10);
	if (!grid.cols || !grid.rows) {
		fprintf(stderr, "tsm-replay: bad size %sx%s\n", argv[1], argv[2]);
		return 2;
	}
	grid.cells = calloc((size_t)grid.cols * grid.rows, sizeof(*grid.cells));
	data = read_file(argv[3], &len);

	if (tsm_screen_new(&screen, NULL, NULL) < 0 ||
	    tsm_screen_resize(screen, grid.cols, grid.rows) < 0 ||
	    tsm_vte_new(&vte, screen, on_write, &reply, NULL, NULL) < 0) {
		fprintf(stderr, "tsm-replay: cannot create screen or vte\n");
		return 2;
	}
#ifdef TSM_SCREEN_IMAGES
	/* no host budget here: Ghostty's per-screen default as the limit */
	tsm_screen_set_image_limit(screen, (size_t)320 * 1000 * 1000);
#endif
#ifdef TSM_SCREEN_CELL_UNDERLINE_COLOR
	if (view != VIEW_DRAW && tsm_vte_set_palette(vte, DRAW2_PALETTE) < 0) {
		fprintf(stderr, "tsm-replay: cannot set palette %s\n", DRAW2_PALETTE);
		return 2;
	}
#endif

	if (argc == 5)
		cuts = read_cuts(argv[4], len, &ncuts);

	for (i = 0; i < ncuts; i++) {
		feed(screen, vte, (const char *)data + pos, cuts[i] - pos);
		pos = cuts[i];
		printf("checkpoint %zu\n", pos);
		dump(screen, vte, &grid, &reply, view);
	}
	feed(screen, vte, (const char *)data + pos, len - pos);
	if (ncuts)
		printf("checkpoint %zu\n", len);
	dump(screen, vte, &grid, &reply, view);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
	free(cuts);
	free(grid.cells);
	free(data);
	free(reply.buf);
	return 0;
}
