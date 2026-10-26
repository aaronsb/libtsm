/*
 * tsm-replay - replay a recorded byte stream into libtsm and print the
 * resulting screen state as text.
 *
 * The text is the conformance contract (ADR-301 tier 1 in the kmscon fork).
 * INTERFACE.md, beside this file, defines it in terms any implementation can
 * meet: the command line, the input, the test commands, the reference host
 * and the output. Only the public API is used, so the tool builds against any
 * libtsm, including upstream releases. What follows is how this one meets the
 * interface in libtsm's terms.
 *
 * Each part needs a declaration in libtsm.h and is left out without it:
 *   --frame                   TSM_SCREEN_FRAME (tsm_screen_frame)
 *   --draw2                   TSM_SCREEN_CELL_UNDERLINE_COLOR (tsm_screen_draw2)
 *   underline=, ul=, strike   TSM_SCREEN_ATTR_UNDERLINE_STYLE
 *   placement, image lines    TSM_SCREEN_IMAGES (tsm_screen_frame_image)
 *   test commands, --describe TSM_SCREEN_IMAGES; elsewhere the APC reaches the
 *                             vte, which ignores it
 * A build with -Dimages=false declares every macro but never turns sixel on;
 * --describe reads that from DECRQM 80 and leaves out the image features.
 *
 * Each test command, by the call that runs it and the declaration it needs
 * beyond TSM_SCREEN_IMAGES:
 *   resize                    tsm_screen_resize
 *   sb-max                    tsm_screen_set_max_sb
 *   sb-up, sb-down, sb-reset  tsm_screen_sb_up, _sb_down, _sb_reset
 *   cell                      tsm_screen_set_cell_size
 *   limit                     tsm_screen_set_image_limit
 *   image, place              tsm_screen_test_image, tsm_screen_test_place
 *   sixel                     tsm_vte_set_sixel (TSM_VTE_SIXEL)
 *   graphic-max               tsm_vte_set_graphic_max_size (TSM_VTE_SIXEL)
 *   registers                 tsm_vte_set_sixel_registers (TSM_VTE_SIXEL)
 *   kitty-graphics            tsm_vte_set_kitty_graphics (TSM_VTE_KITTY_GRAPHICS)
 *   host                      tsm_vte_set_image_cb (TSM_VTE_IMAGE_HOST)
 *   sync                      tsm_vte_set_sync (TSM_VTE_SYNC)
 *   sync-frames               tsm_vte_get_sync_frames (TSM_VTE_SYNC)
 *   version                   tsm_vte_set_version (TSM_VTE_XTVERSION)
 *   termcap                   tsm_vte_set_termcap (TSM_VTE_TERMCAP)
 *   key                       tsm_vte_handle_keyboard, the modifier bits as
 *                             TSM_*_MASK
 *   mouse                     tsm_vte_handle_mouse, a move with a button held
 *                             as that button plus 32 and with none as 3, the
 *                             modifiers as TSM_MOUSE_MODIFIER_*
 *   paste                     tsm_vte_paste
 *   focus                     tsm_vte_handle_focus (TSM_VTE_HANDLE_FOCUS)
 *   palette                   tsm_vte_set_palette (TSM_SCREEN_CELL_UNDERLINE_COLOR)
 *   frame-since               tsm_screen_frame_since (TSM_SCREEN_FRAME_SINCE)
 * A negative return prints the command's error line.
 *
 * The reference host is tsm_screen_set_image_limit of 320 MB,
 * tsm_vte_set_sixel, tsm_vte_set_kitty_graphics and tsm_vte_set_sync on,
 * tsm_vte_set_version("tsm-replay"), and tsm_vte_set_palette("legacy") for
 * --draw2 and --frame (libtsm's default elsewhere, the same palette). Where
 * libtsm declares TSM_VTE_IMAGE_HOST, the fixture host is the image hook:
 * the stream goes through tsm_vte_input_bounded, and each request is
 * answered with tsm_vte_image_reply before the rest is fed, the path kmscon
 * takes; elsewhere through tsm_vte_input. A request's fields print as the
 * host: line: ID is the request's id, M its medium, F its flags (inflate,
 * png, exact, discard as TSM_VTE_IMAGE_INFLATE, _PNG, _EXACT, _DISCARD),
 * then offset, read_max, min_bytes, name and data_len; the outcomes are
 * enum tsm_vte_image_status.
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

#ifdef TSM_MODE_TRACE
/* the fork's test builds: names the case in the mode trace (ADR-201) */
void tsm_mode_trace_test(const char *name);
#endif

#define MAX_CP 32
#define INTERFACE_VERSION "1.0"

#ifdef TSM_SCREEN_CELL_UNDERLINE_COLOR
/* the palette the --draw2 and --frame views resolve through, set explicitly
 * so a change of libtsm's default palette shows as a change here */
static const char *palette = "legacy";
#endif

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

/* the bytes libtsm wrote back, and the host lines printed among them */
struct host_line {
	size_t at;			/* the reply bytes before it */
	char *text;
};

struct reply {
	unsigned char *buf;
	size_t len, cap;
	struct host_line *lines;
	size_t nlines;
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
/* FILE's fixture directory: FILE with ".bin" replaced by ".files" */
static char fixture_dir[4096];

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

#ifdef TSM_VTE_IMAGE_HOST
static int host_cb(struct tsm_vte *vte, const struct tsm_vte_image_request *req,
		   void *data);
#endif

/*
 * The test commands, in INTERFACE.md's order, each with its feature (NULL
 * for none) and the keys it takes. Every other key exits 2.
 */
#define TEST_KEYS 14
static const struct test_spec {
	const char *name;
	const char *feature;
	const char *keys[TEST_KEYS];
} test_specs[] = {
	{ "resize", NULL, { "c", "r" } },
	{ "sb-max", NULL, { "n" } },
	{ "sb-up", NULL, { "n" } },
	{ "sb-down", NULL, { "n" } },
	{ "sb-reset", NULL, { NULL } },
	{ "cell", "images", { "w", "h" } },
	{ "limit", "images", { "b" } },
	{ "image", "images", { "n", "w", "h", "kitty", "id", "tr", "fill" } },
	{ "place", "images", { "img", "col", "row", "x", "y", "c", "r", "sx", "sy", "sw", "sh",
			       "z", "p" } },
#ifdef TSM_VTE_SIXEL
	{ "sixel", "sixel", { "on" } },
	{ "graphic-max", "sixel", { "w", "h" } },
	{ "registers", "sixel", { "n", "private" } },
#endif
#ifdef TSM_VTE_KITTY_GRAPHICS
	{ "kitty-graphics", "kitty-graphics", { "on" } },
#endif
#ifdef TSM_VTE_IMAGE_HOST
	{ "host", "image-host", { "on" } },
#endif
#ifdef TSM_VTE_SYNC
	{ "sync", "sync", { "on" } },
	{ "sync-frames", "sync", { NULL } },
#endif
#ifdef TSM_VTE_XTVERSION
	{ "version", "identify", { "text" } },
#endif
#ifdef TSM_VTE_TERMCAP
	{ "termcap", "identify", { "file" } },
#endif
	{ "key", "input", { "sym", "ascii", "unicode", "mods" } },
	{ "mouse", "input", { "col", "row", "px", "py", "button", "event", "mods" } },
	{ "paste", "input", { "hex" } },
#ifdef TSM_VTE_HANDLE_FOCUS
	{ "focus", "focus", { "in" } },
#endif
#ifdef TSM_SCREEN_CELL_UNDERLINE_COLOR
	{ "palette", "palette", { "name" } },
#endif
#ifdef TSM_SCREEN_FRAME_SINCE
	{ "frame-since", "query", { "caller" } },
#endif
};

/* a test command split at ';': its name, then each KEY=VALUE */
#define TEST_CMD_MAX 511
#define TEST_ARGS 32
struct test_cmd {
	const struct test_spec *spec;
	int argc;
	char *key[TEST_ARGS];
	char *val[TEST_ARGS];
};

static void test_fail(const struct test_cmd *c, const char *key, const char *what)
{
	fprintf(stderr, "tsm-replay: test command %s: %s%s%s\n", c->spec->name,
		key ? key : "", key ? ": " : "", what);
	exit(2);
}

/* KEY's value, or NULL */
static const char *test_val(const struct test_cmd *c, const char *key)
{
	int i;

	for (i = 0; i < c->argc; i++)
		if (!strcmp(c->key[i], key))
			return c->val[i];
	return NULL;
}

/* KEY, required, as a string */
static const char *test_str(const struct test_cmd *c, const char *key)
{
	const char *v = test_val(c, key);

	if (!v)
		test_fail(c, key, "missing");
	return v;
}

static int hex_digit(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

/* @v, KEY's value, as a number from 0 to @max, in decimal or in hex */
static uint64_t parse_num(const struct test_cmd *c, const char *key, const char *v,
			  uint64_t max, bool hex)
{
	unsigned int base = hex ? 16 : 10;
	uint64_t n = 0;
	int d;

	if (!*v)
		test_fail(c, key, "empty");
	for (; *v; v++) {
		d = hex_digit(*v);
		if (d < 0 || (unsigned int)d >= base)
			test_fail(c, key, hex ? "not hex" : "not a decimal number");
		if ((uint64_t)d > max || n > (max - d) / base)
			test_fail(c, key, "out of range");
		n = n * base + d;
	}
	return n;
}

/* KEY as a decimal number from 0 to @max; @def when absent, where -1 makes it required */
static uint64_t test_uint(const struct test_cmd *c, const char *key, int64_t def,
			  uint64_t max)
{
	const char *v = test_val(c, key);

	if (!v && def < 0)
		test_fail(c, key, "missing");
	return v ? parse_num(c, key, v, max, false) : (uint64_t)def;
}

/* KEY as a hex number from 0 to @max, as test_uint */
static uint64_t test_hex(const struct test_cmd *c, const char *key, int64_t def,
			 uint64_t max)
{
	const char *v = test_val(c, key);

	if (!v && def < 0)
		test_fail(c, key, "missing");
	return v ? parse_num(c, key, v, max, true) : (uint64_t)def;
}

/* KEY as a signed 32-bit decimal number, @def when absent */
static int32_t test_int(const struct test_cmd *c, const char *key, int32_t def)
{
	const char *v = test_val(c, key);

	if (!v)
		return def;
	if (*v == '-')
		return (int32_t)-(int64_t)parse_num(c, key, v + 1, (uint64_t)INT32_MAX + 1, false);
	return (int32_t)parse_num(c, key, v, INT32_MAX, false);
}

/* KEY as 0 or 1 */
static bool test_flag(const struct test_cmd *c, const char *key, int def)
{
	return test_uint(c, key, def, 1);
}

/* KEY as the index of one of the NULL-terminated @names */
static unsigned int test_name(const struct test_cmd *c, const char *key,
			      const char *const *names)
{
	const char *v = test_str(c, key);
	unsigned int i;

	for (i = 0; names[i]; i++)
		if (!strcmp(v, names[i]))
			return i;
	test_fail(c, key, "not a name the command takes");
	return 0;
}

/* KEY as a fixture's name, which stays inside the fixture directory */
static const char *test_fixture(const struct test_cmd *c, const char *key)
{
	const char *v = test_str(c, key);
	size_t i, len = strlen(v);

	if (!len || len > 255 || v[0] == '.')
		test_fail(c, key, "not a fixture name");
	for (i = 0; i < len; i++)
		if (!((v[i] >= 'a' && v[i] <= 'z') || (v[i] >= 'A' && v[i] <= 'Z') ||
		      (v[i] >= '0' && v[i] <= '9') || v[i] == '.' || v[i] == '_' || v[i] == '-'))
			test_fail(c, key, "not a fixture name");
	return v;
}

/* the bytes hex @v spells, none of them 0, in @out, at most @max; their count */
static size_t hex_bytes(const struct test_cmd *c, const char *key, const char *v,
			unsigned char *out, size_t max)
{
	size_t len = strlen(v), i;
	int hi, lo;

	if (len % 2 || len / 2 > max)
		test_fail(c, key, "not hex bytes of a length the command takes");
	for (i = 0; i < len / 2; i++) {
		hi = hex_digit(v[2 * i]);
		lo = hex_digit(v[2 * i + 1]);
		if (hi < 0 || lo < 0)
			test_fail(c, key, "not hex");
		if (!hi && !lo)
			test_fail(c, key, "holds a zero byte");
		out[i] = hi << 4 | lo;
	}
	return len / 2;
}

static int test_image_cmd(struct tsm_screen *screen, const struct test_cmd *c)
{
	struct tsm_test_image img = { 0 };
	uint64_t n = test_uint(c, "n", 0, TEST_IMAGES - 1);
	uint32_t fill = (uint32_t)test_hex(c, "fill", 0xff808080, UINT32_MAX);
	uint32_t *px;
	size_t i;
	int r;

	img.width = (unsigned int)test_uint(c, "w", 0, 65535);
	img.height = (unsigned int)test_uint(c, "h", 0, 65535);
	if ((uint64_t)img.width * img.height > 16777216)
		test_fail(c, "w", "times h is over 16777216");
	img.kind = test_flag(c, "kitty", 0) ? TSM_IMAGE_KITTY : TSM_IMAGE_SIXEL;
	if (img.kind == TSM_IMAGE_KITTY)
		img.id = (uint32_t)test_uint(c, "id", n, UINT32_MAX);
	img.transient = test_flag(c, "tr", 0);
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

static int test_place_cmd(struct tsm_screen *screen, const struct test_cmd *c)
{
	struct tsm_test_placement pl = { 0 };
	uint64_t key;

	pl.image = test_images[test_uint(c, "img", 0, TEST_IMAGES - 1)];
	pl.id = (uint32_t)test_uint(c, "p", 0, UINT32_MAX);
	pl.col = (unsigned int)test_uint(c, "col", tsm_screen_get_cursor_x(screen), UINT32_MAX);
	pl.row = (unsigned int)test_uint(c, "row", tsm_screen_get_cursor_y(screen), UINT32_MAX);
	pl.x = (unsigned int)test_uint(c, "x", 0, UINT32_MAX);
	pl.y = (unsigned int)test_uint(c, "y", 0, UINT32_MAX);
	pl.cols = (unsigned int)test_uint(c, "c", 0, UINT32_MAX);
	pl.rows = (unsigned int)test_uint(c, "r", 0, UINT32_MAX);
	pl.src_x = (unsigned int)test_uint(c, "sx", 0, UINT32_MAX);
	pl.src_y = (unsigned int)test_uint(c, "sy", 0, UINT32_MAX);
	pl.src_width = (unsigned int)test_uint(c, "sw", 0, UINT32_MAX);
	pl.src_height = (unsigned int)test_uint(c, "sh", 0, UINT32_MAX);
	pl.z = test_int(c, "z", 0);
	return tsm_screen_test_place(screen, &pl, &key);
}

/* INTERFACE.md's modifier bits as libtsm's */
static unsigned int key_mods(unsigned int mods)
{
	return (mods & 1 ? TSM_SHIFT_MASK : 0) | (mods & 2 ? TSM_LOCK_MASK : 0) |
	       (mods & 4 ? TSM_CONTROL_MASK : 0) | (mods & 8 ? TSM_ALT_MASK : 0) |
	       (mods & 16 ? TSM_LOGO_MASK : 0);
}

static void test_key_cmd(struct tsm_vte *vte, const struct test_cmd *c)
{
	uint32_t sym = (uint32_t)test_hex(c, "sym", -1, 0x1fffffff);
	uint32_t ascii = (uint32_t)test_hex(c, "ascii", 0, 0x1fffffff);
	uint32_t unicode = test_val(c, "unicode") ?
			   (uint32_t)test_hex(c, "unicode", -1, 0x10ffff) : TSM_VTE_INVALID;

	tsm_vte_handle_keyboard(vte, sym, ascii, key_mods(test_uint(c, "mods", 0, 31)), unicode);
}

static void test_mouse_cmd(struct tsm_screen *screen, struct tsm_vte *vte,
			   const struct test_cmd *c)
{
	static const char *const buttons[] = {
		"left", "middle", "right", "wheel-up", "wheel-down", "none", NULL,
	};
	static const unsigned int button_codes[] = {
		TSM_MOUSE_BUTTON_LEFT, TSM_MOUSE_BUTTON_MIDDLE, TSM_MOUSE_BUTTON_RIGHT,
		TSM_MOUSE_BUTTON_WHEEL_UP, TSM_MOUSE_BUTTON_WHEEL_DOWN,
	};
	static const char *const events[] = { "press", "release", "move", NULL };
	static const unsigned int event_codes[] = {
		TSM_MOUSE_EVENT_PRESSED, TSM_MOUSE_EVENT_RELEASED, TSM_MOUSE_EVENT_MOVED,
	};
	unsigned int col = test_uint(c, "col", -1, tsm_screen_get_width(screen) - 1);
	unsigned int row = test_uint(c, "row", -1, tsm_screen_get_height(screen) - 1);
	unsigned int px = test_uint(c, "px", 0, 65535), py = test_uint(c, "py", 0, 65535);
	unsigned int button = test_name(c, "button", buttons);
	unsigned int event = test_name(c, "event", events);
	unsigned int mods = test_uint(c, "mods", 0, 31), code, flags = 0;

	bool none = !strcmp(buttons[button], "none");

	if (event_codes[event] == TSM_MOUSE_EVENT_MOVED) {
		/* motion with a button held is that button plus 32, with none 3 */
		if (!none && button_codes[button] > TSM_MOUSE_BUTTON_RIGHT)
			test_fail(c, "button", "a wheel in a move");
		code = none ? 3 : button_codes[button] + 32;
	} else {
		if (none)
			test_fail(c, "button", "none in a press or release");
		code = button_codes[button];
	}
	if (mods & 1)
		flags |= TSM_MOUSE_MODIFIER_SHIFT;
	if (mods & 4)
		flags |= TSM_MOUSE_MODIFIER_CTRL;
	if (mods & 8)
		flags |= TSM_MOUSE_MODIFIER_META;
	tsm_vte_handle_mouse(vte, col, row, px, py, code, event_codes[event], flags);
}

static void test_paste_cmd(struct tsm_vte *vte, const struct test_cmd *c)
{
	char data[TEST_CMD_MAX / 2 + 1];

	data[hex_bytes(c, "hex", test_str(c, "hex"), (unsigned char *)data,
		       sizeof(data) - 1)] = 0;
	tsm_vte_paste(vte, data);
}

#ifdef TSM_VTE_TERMCAP
/* a termcap fixture: one capability a line, NAME or NAME=HEX */
static int test_termcap_cmd(struct tsm_vte *vte, const struct test_cmd *c)
{
	struct tsm_vte_termcap *caps;
	char path[4400], *buf, *line, *next, *eq;
	unsigned char *values;
	size_t len, n = 0, i, j, vlen;
	int r;

	snprintf(path, sizeof(path), "%s/%s", fixture_dir, test_fixture(c, "file"));
	buf = (char *)read_file(path, &len);
	buf = realloc(buf, len + 1);
	caps = calloc(TSM_VTE_TERMCAP_MAX, sizeof(*caps));
	values = malloc((size_t)TSM_VTE_TERMCAP_MAX * (TSM_VTE_TERMCAP_VALUE_MAX + 1));
	if (!buf || !caps || !values) {
		perror("malloc");
		exit(2);
	}
	buf[len] = 0;
	if (strlen(buf) != len)
		test_fail(c, "file", "holds a zero byte");
	for (line = buf; line; line = next) {
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		if (!*line)
			continue;
		if (n == TSM_VTE_TERMCAP_MAX)
			test_fail(c, "file", "has more entries than a table holds");
		eq = strchr(line, '=');
		if (eq) {
			*eq = 0;
			vlen = hex_bytes(c, "file", eq + 1,
					 values + n * (TSM_VTE_TERMCAP_VALUE_MAX + 1),
					 TSM_VTE_TERMCAP_VALUE_MAX);
			values[n * (TSM_VTE_TERMCAP_VALUE_MAX + 1) + vlen] = 0;
			caps[n].value = (char *)values + n * (TSM_VTE_TERMCAP_VALUE_MAX + 1);
		}
		if (!*line || strlen(line) > TSM_VTE_TERMCAP_NAME_MAX)
			test_fail(c, "file", "names a capability of no or too many bytes");
		for (j = 0; line[j]; j++)
			if (line[j] < 0x21 || line[j] > 0x7e)
				test_fail(c, "file", "names a capability with a byte outside 0x21-0x7e");
		for (i = 0; i < n; i++)
			if (!strcmp(caps[i].name, line))
				test_fail(c, "file", "names a capability twice");
		caps[n++].name = line;
	}
	r = tsm_vte_set_termcap(vte, caps, n);
	free(values);
	free(caps);
	free(buf);
	return r;
}
#endif

#ifdef TSM_SCREEN_FRAME_SINCE
/* the frame ages of frame-since's callers */
#define TEST_CALLERS 8
static struct tsm_frame_age test_callers[TEST_CALLERS];
#endif

/* runs the test command in @cmd, @len bytes after "tsm-test;" */
static void test_command(struct tsm_screen *screen, struct tsm_vte *vte,
			 const char *cmd, size_t len)
{
	char buf[TEST_CMD_MAX + 1], *tok, *save, *eq;
	struct test_cmd c = { 0 };
	const char *name;
	size_t i;
	int j, k, r = 0;

	if (len > TEST_CMD_MAX) {
		fprintf(stderr, "tsm-replay: test command over %d bytes\n", TEST_CMD_MAX);
		exit(2);
	}
	memcpy(buf, cmd, len);
	buf[len] = 0;
	name = strtok_r(buf, ";", &save);
	if (!name) {
		fprintf(stderr, "tsm-replay: empty test command\n");
		exit(2);
	}
	for (i = 0; i < sizeof(test_specs) / sizeof(test_specs[0]); i++)
		if (!strcmp(test_specs[i].name, name))
			c.spec = &test_specs[i];
	if (!c.spec) {
		fprintf(stderr, "tsm-replay: unknown test command %s\n", name);
		exit(2);
	}
	while ((tok = strtok_r(NULL, ";", &save))) {
		if (c.argc == TEST_ARGS - 1)
			test_fail(&c, NULL, "has too many keys");
		eq = strchr(tok, '=');
		if (!eq)
			test_fail(&c, tok, "has no value");
		*eq = 0;
		for (k = 0; k < TEST_KEYS && c.spec->keys[k] && strcmp(c.spec->keys[k], tok); k++)
			;
		if (k == TEST_KEYS || !c.spec->keys[k])
			test_fail(&c, tok, "is not a key the command takes");
		for (j = 0; j < c.argc; j++)
			if (!strcmp(c.key[j], tok))
				test_fail(&c, tok, "given twice");
		c.key[c.argc] = tok;
		c.val[c.argc++] = eq + 1;
	}

	if (!strcmp(name, "cell")) {
		r = tsm_screen_set_cell_size(screen, test_uint(&c, "w", -1, UINT32_MAX),
					     test_uint(&c, "h", -1, UINT32_MAX));
	} else if (!strcmp(name, "limit")) {
		tsm_screen_set_image_limit(screen, (size_t)test_uint(&c, "b", -1, SIZE_MAX));
	} else if (!strcmp(name, "sb-max")) {
		tsm_screen_set_max_sb(screen, test_uint(&c, "n", -1, UINT32_MAX));
	} else if (!strcmp(name, "resize")) {
		r = tsm_screen_resize(screen, test_uint(&c, "c", -1, UINT32_MAX),
				      test_uint(&c, "r", -1, UINT32_MAX));
	} else if (!strcmp(name, "sb-up")) {
		tsm_screen_sb_up(screen, test_uint(&c, "n", 1, UINT32_MAX));
	} else if (!strcmp(name, "sb-down")) {
		tsm_screen_sb_down(screen, test_uint(&c, "n", 1, UINT32_MAX));
	} else if (!strcmp(name, "sb-reset")) {
		tsm_screen_sb_reset(screen);
	} else if (!strcmp(name, "image")) {
		r = test_image_cmd(screen, &c);
	} else if (!strcmp(name, "place")) {
		r = test_place_cmd(screen, &c);
#ifdef TSM_VTE_SIXEL
	} else if (!strcmp(name, "sixel")) {
		tsm_vte_set_sixel(vte, test_flag(&c, "on", 1));
	} else if (!strcmp(name, "graphic-max")) {
		r = tsm_vte_set_graphic_max_size(vte, test_uint(&c, "w", -1, UINT32_MAX),
						 test_uint(&c, "h", -1, UINT32_MAX));
	} else if (!strcmp(name, "registers")) {
		r = tsm_vte_set_sixel_registers(vte, test_uint(&c, "n", -1, UINT32_MAX),
						test_flag(&c, "private", 1));
#endif
#ifdef TSM_VTE_KITTY_GRAPHICS
	} else if (!strcmp(name, "kitty-graphics")) {
		tsm_vte_set_kitty_graphics(vte, test_flag(&c, "on", 1));
#endif
#ifdef TSM_VTE_IMAGE_HOST
	} else if (!strcmp(name, "host")) {
		tsm_vte_set_image_cb(vte, test_flag(&c, "on", 1) ? host_cb : NULL, NULL);
#endif
#ifdef TSM_VTE_SYNC
	} else if (!strcmp(name, "sync")) {
		tsm_vte_set_sync(vte, test_flag(&c, "on", 1));
	} else if (!strcmp(name, "sync-frames")) {
		printf("test sync-frames %llu\n",
		       (unsigned long long)tsm_vte_get_sync_frames(vte));
#endif
#ifdef TSM_VTE_XTVERSION
	} else if (!strcmp(name, "version")) {
		const char *text = test_str(&c, "text");

		if (strlen(text) > TSM_VTE_VERSION_MAX)
			test_fail(&c, "text", "over 64 bytes");
		for (i = 0; text[i]; i++)
			if (text[i] < 0x20 || text[i] > 0x7e)
				test_fail(&c, "text", "holds a byte outside 0x20-0x7e");
		r = tsm_vte_set_version(vte, text);
#endif
#ifdef TSM_VTE_TERMCAP
	} else if (!strcmp(name, "termcap")) {
		r = test_termcap_cmd(vte, &c);
#endif
	} else if (!strcmp(name, "key")) {
		test_key_cmd(vte, &c);
	} else if (!strcmp(name, "mouse")) {
		test_mouse_cmd(screen, vte, &c);
	} else if (!strcmp(name, "paste")) {
		test_paste_cmd(vte, &c);
#ifdef TSM_VTE_HANDLE_FOCUS
	} else if (!strcmp(name, "focus")) {
		tsm_vte_handle_focus(vte, test_flag(&c, "in", -1));
#endif
#ifdef TSM_SCREEN_CELL_UNDERLINE_COLOR
	} else if (!strcmp(name, "palette")) {
		static const char *const palettes[] = {
			"legacy", "vga", "nord", "solarized", "solarized-black",
			"solarized-white", "soft-black", "base16-dark", "base16-light", NULL,
		};

		palette = palettes[test_name(&c, "name", palettes)];
		r = tsm_vte_set_palette(vte, palette);
#endif
#ifdef TSM_SCREEN_FRAME_SINCE
	} else if (!strcmp(name, "frame-since")) {
		const struct tsm_frame *f;
		unsigned int n = test_uint(&c, "caller", -1, TEST_CALLERS - 1), changed = 0;

		f = tsm_screen_frame_since(screen, &test_callers[n]);
		if (!f) {
			fprintf(stderr, "tsm-replay: tsm_screen_frame_since failed\n");
			exit(3);
		}
		for (i = 0; i < (size_t)f->cols * f->rows; i++)
			if (tsm_frame_cell_at(f, i % f->cols, i / f->cols)->flags &
			    TSM_FRAME_CHANGED)
				changed++;
		printf("test frame-since %u changed %u\n", n, changed);
#endif
	}
	if (r < 0)
		printf("test %s error %d\n", name, r);
}

/*
 * The features of INTERFACE.md this build has, in its order. A build with
 * -Dimages=false declares every macro but never turns sixel on, so
 * images_on() leaves the image features out there.
 */
static const char *const features[] = {
#ifdef TSM_SCREEN_ATTR_UNDERLINE_STYLE
	"attrs-extended",
#endif
#ifdef TSM_SCREEN_FRAME
	"view-frame",
#endif
#ifdef TSM_SCREEN_CELL_UNDERLINE_COLOR
	"view-draw2",
#endif
	"images",
#ifdef TSM_VTE_SIXEL
	"sixel",
#endif
#ifdef TSM_VTE_KITTY_GRAPHICS
	"kitty-graphics",
#endif
#ifdef TSM_VTE_IMAGE_HOST
	"image-host",
#endif
#ifdef TSM_VTE_SYNC
	"sync",
#endif
#if defined(TSM_VTE_XTVERSION) && defined(TSM_VTE_TERMCAP)
	"identify",
#endif
	"input",
#ifdef TSM_VTE_HANDLE_FOCUS
	"focus",
#endif
#ifdef TSM_SCREEN_CELL_UNDERLINE_COLOR
	"palette",
#endif
#ifdef TSM_SCREEN_FRAME_SINCE
	"query",
#endif
};

/* whether sixel comes on: DECRQM 80 reports a fixed 4 while it is off */
static bool images_on(void)
{
#if defined(TSM_VTE_SIXEL) && defined(TSM_VTE_GET_MODE)
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct reply reply = { 0 };
	bool on;

	if (tsm_screen_new(&screen, NULL, NULL) < 0 ||
	    tsm_vte_new(&vte, screen, on_write, &reply, NULL, NULL) < 0) {
		fprintf(stderr, "tsm-replay: cannot create screen or vte\n");
		exit(2);
	}
	tsm_screen_set_image_limit(screen, (size_t)320 * 1000 * 1000);
	tsm_vte_set_sixel(vte, true);
	on = tsm_vte_get_mode(vte, true, 80) != 4;
	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
	free(reply.buf);
	return on;
#else
	return false;
#endif
}

static bool has_feature(const char *name, bool images)
{
	size_t i;

	if (!images && (!strcmp(name, "images") || !strcmp(name, "sixel") ||
			!strcmp(name, "kitty-graphics") || !strcmp(name, "image-host")))
		return false;
	for (i = 0; i < sizeof(features) / sizeof(features[0]); i++)
		if (!strcmp(features[i], name))
			return true;
	return false;
}

/* --describe: the interface version, views, features and commands */
static int describe(void)
{
	bool images = images_on();
	size_t i;

	printf("interface %s\n", INTERFACE_VERSION);
	printf("view screen\n");
#ifdef TSM_SCREEN_FRAME
	printf("view frame\n");
#endif
#ifdef TSM_SCREEN_CELL_UNDERLINE_COLOR
	printf("view draw2\n");
#endif
	for (i = 0; i < sizeof(features) / sizeof(features[0]); i++)
		if (has_feature(features[i], images))
			printf("feature %s\n", features[i]);
	for (i = 0; i < sizeof(test_specs) / sizeof(test_specs[0]); i++) {
		if (!test_specs[i].feature)
			printf("command %s\n", test_specs[i].name);
		else if (has_feature(test_specs[i].feature, images))
			printf("command %s %s\n", test_specs[i].name, test_specs[i].feature);
	}
	return 0;
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

#ifdef TSM_VTE_IMAGE_HOST
/* the fixture host: the request out, and where its answers come from */
static struct {
	bool out;
	uint64_t id;
	unsigned int medium, flags;
	uint64_t offset, read_max, min_bytes;
	char fixture[4200];
	unsigned int direct;		/* inline requests so far */
	struct reply *reply;
} host;

static int host_cb(struct tsm_vte *vte, const struct tsm_vte_image_request *req,
		   void *data)
{
	static const char *const flag_names[] = { "inflate", "png", "exact", "discard" };
	struct reply *r = host.reply;
	const char *base;
	/* the whole name, every byte escaped at worst, and the rest */
	size_t cap = 256 + (req->name ? 4 * strlen(req->name) : 0), len, i;
	char *line = malloc(cap);
	bool first = true;

	(void)vte;
	(void)data;
	host.out = true;
	host.id = req->id;
	host.medium = req->medium;
	host.flags = req->flags;
	host.offset = req->offset;
	host.read_max = req->read_max;
	host.min_bytes = req->min_bytes;
	if (req->name) {
		base = strrchr(req->name, '/');
		snprintf(host.fixture, sizeof(host.fixture), "%s", base ? base + 1 : req->name);
	} else {
		snprintf(host.fixture, sizeof(host.fixture), "direct-%u", ++host.direct);
	}

	if (!line) {
		perror("malloc");
		exit(2);
	}
	len = snprintf(line, cap, "host: %llu t=%c flags=",
		       (unsigned long long)req->id, req->medium);
	for (i = 0; i < 4; i++) {
		if (req->flags & (1u << i)) {
			len += snprintf(line + len, cap - len, "%s%s",
					first ? "" : ",", flag_names[i]);
			first = false;
		}
	}
	if (first)
		line[len++] = '-';
	len += snprintf(line + len, cap - len, " O=%llu max=%llu min=%llu name=",
			(unsigned long long)req->offset, (unsigned long long)req->read_max,
			(unsigned long long)req->min_bytes);
	for (i = 0; req->name && req->name[i]; i++) {
		unsigned char c = req->name[i];

		if (c > 0x20 && c < 0x7f && c != '\\')
			line[len++] = c;
		else
			len += snprintf(line + len, cap - len, "\\x%02x", c);
	}
	snprintf(line + len, cap - len, " len=%zu", req->data_len);

	r->lines = realloc(r->lines, (r->nlines + 1) * sizeof(*r->lines));
	if (!r->lines) {
		perror("realloc");
		exit(2);
	}
	r->lines[r->nlines].at = r->len;
	r->lines[r->nlines].text = line;
	r->nlines++;
	return 0;
}

/* fixture @name's bytes in @len, or NULL when it does not exist */
static unsigned char *host_file(const char *suffix, size_t *len)
{
	char path[8400];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s%s", fixture_dir, host.fixture, suffix);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fclose(f);
	return read_file(path, len);
}

/* the outcome a .error fixture names in its first word */
static unsigned int host_outcome(const unsigned char *buf, size_t len)
{
	static const char *const names[] = {
		[TSM_VTE_IMAGE_UNREADABLE] = "UNREADABLE",
		[TSM_VTE_IMAGE_TOO_SHORT] = "TOO_SHORT",
		[TSM_VTE_IMAGE_CORRUPT] = "CORRUPT",
		[TSM_VTE_IMAGE_TOO_LARGE] = "TOO_LARGE",
		[TSM_VTE_IMAGE_OVER_BUDGET] = "OVER_BUDGET",
	};
	size_t n, word = 0;
	unsigned int i;

	while (word < len && buf[word] > ' ')
		word++;
	for (i = TSM_VTE_IMAGE_UNREADABLE; i <= TSM_VTE_IMAGE_OVER_BUDGET; i++) {
		n = strlen(names[i]);
		if (word == n && !memcmp(buf, names[i], n))
			return i;
	}
	fprintf(stderr, "tsm-replay: %s.error names no outcome\n", host.fixture);
	exit(2);
}

/*
 * Answers the request out from the fixtures, the first that applies, with
 * min_bytes, offset and read_max applied as the image helper applies them;
 * a discard is answered TSM_VTE_IMAGE_OK with no bytes, as the helper
 * answers one
 */
static void host_answer(struct tsm_vte *vte)
{
	struct tsm_vte_image_result res = { .status = TSM_VTE_IMAGE_OK };
	unsigned char *buf = NULL;
	uint64_t avail;
	size_t len;

	host.out = false;
	if (host.flags & TSM_VTE_IMAGE_DISCARD) {
		/* nothing to read */
	} else if ((buf = host_file(".error", &len))) {
		res.status = host_outcome(buf, len);
	} else if ((host.flags & TSM_VTE_IMAGE_PNG) && (buf = host_file(".rgba", &len))) {
		if (len < 8) {
			fprintf(stderr, "tsm-replay: %s.rgba is short\n", host.fixture);
			exit(2);
		}
		res.width = buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24;
		res.height = buf[4] | buf[5] << 8 | buf[6] << 16 | (uint32_t)buf[7] << 24;
		if ((uint64_t)res.width * res.height * 4 != len - 8) {
			fprintf(stderr, "tsm-replay: %s.rgba is not %ux%u\n", host.fixture,
				res.width, res.height);
			exit(2);
		}
		res.data = buf + 8;
		res.len = len - 8;
	} else if ((host.flags & (TSM_VTE_IMAGE_INFLATE | TSM_VTE_IMAGE_PNG)) ==
		   TSM_VTE_IMAGE_INFLATE && (buf = host_file(".bytes", &len))) {
		/* the inflated bytes: the first min_bytes of them, the rest
		 * ignored, or under EXACT one byte more is corrupt */
		if (len < host.min_bytes)
			res.status = TSM_VTE_IMAGE_TOO_SHORT;
		else if ((host.flags & TSM_VTE_IMAGE_EXACT) && len > host.min_bytes)
			res.status = TSM_VTE_IMAGE_CORRUPT;
		res.data = buf;
		res.len = host.min_bytes;
	} else if (!(host.flags & (TSM_VTE_IMAGE_INFLATE | TSM_VTE_IMAGE_PNG)) &&
		   (buf = host_file("", &len))) {
		/* from offset, at most read_max (0: to the end), the first
		 * min_bytes of what that holds */
		avail = host.offset < len ? len - host.offset : 0;
		if (host.read_max && host.read_max < avail)
			avail = host.read_max;
		if (host.offset >= len)
			res.status = TSM_VTE_IMAGE_UNREADABLE;
		else if (avail < host.min_bytes)
			res.status = TSM_VTE_IMAGE_TOO_SHORT;
		else
			res.data = buf + host.offset;
		res.len = host.min_bytes;
	} else {
		res.status = host.medium == 'd' ? TSM_VTE_IMAGE_CORRUPT :
						  TSM_VTE_IMAGE_UNREADABLE;
	}
	if (res.status != TSM_VTE_IMAGE_OK) {
		res.data = NULL;
		res.len = 0;
	}

	if (tsm_vte_image_reply(vte, host.id, &res) < 0) {
		fprintf(stderr, "tsm-replay: tsm_vte_image_reply refused request %llu\n",
			(unsigned long long)host.id);
		exit(3);
	}
	free(buf);
}
#endif

/*
 * Feeds @len bytes to the vte: through tsm_vte_input_bounded where the fork
 * has the host hook, answering each request before the rest, else through
 * tsm_vte_input.
 */
static void vte_feed(struct tsm_vte *vte, const char *data, size_t len)
{
#ifdef TSM_VTE_IMAGE_HOST
	size_t n;

	while (len) {
		n = tsm_vte_input_bounded(vte, data, len);
		data += n;
		len -= n;
		if (host.out) {
			host_answer(vte);
		} else if (!n) {
			fprintf(stderr, "tsm-replay: input paused with no request out\n");
			exit(3);
		}
	}
#else
	tsm_vte_input(vte, data, len);
#endif
}

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
		vte_feed(vte, data, start - data);
		start += sizeof(intro) - 1;
		end = find_bytes(start, len - (start - data), "\x1b\\", 2);
		if (!end) {
			fprintf(stderr, "tsm-replay: test command without ST\n");
			exit(2);
		}
		test_command(screen, vte, start, end - start);
		len -= end + 2 - data;
		data = end + 2;
	}
#endif
	(void)screen;
	vte_feed(vte, data, len);
}

#ifdef TSM_SCREEN_CELL_UNDERLINE_COLOR
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
	printf("palette %s fg=#%02x%02x%02x bg=#%02x%02x%02x\n", palette,
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
	printf("palette %s fg=#%02x%02x%02x bg=#%02x%02x%02x\n", palette,
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

/* the reply bytes from @from to @to, as one line */
static void print_reply_bytes(const struct reply *reply, size_t from, size_t to)
{
	size_t i;

	if (from == to)
		return;
	printf("reply ");
	for (i = from; i < to; i++)
		printf("%02x", reply->buf[i]);
	putchar('\n');
}

/* the replies since the last dump, a host line where each request came */
static void print_reply(struct reply *reply)
{
	size_t i, from = 0;

	for (i = 0; i < reply->nlines; i++) {
		print_reply_bytes(reply, from, reply->lines[i].at);
		printf("%s\n", reply->lines[i].text);
		from = reply->lines[i].at;
		free(reply->lines[i].text);
	}
	print_reply_bytes(reply, from, reply->len);
	reply->len = 0;
	reply->nlines = 0;
}

static void dump(struct tsm_screen *screen, struct tsm_vte *vte, struct grid *grid,
		 struct reply *reply, enum view view)
{
	unsigned int flags = tsm_screen_get_flags(screen);
	unsigned int row;

	printf("size %ux%u\n", tsm_screen_get_width(screen), tsm_screen_get_height(screen));
	printf("cursor %u,%u\n", tsm_screen_get_cursor_x(screen), tsm_screen_get_cursor_y(screen));
	print_flags(flags);
	print_reply(reply);

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

#ifdef TSM_MODE_TRACE
	if (getenv("TSM_MODE_TRACE_TEST"))
		tsm_mode_trace_test(getenv("TSM_MODE_TRACE_TEST"));
#endif

	if (argc == 2 && !strcmp(argv[1], "--describe")) {
#ifdef TSM_SCREEN_IMAGES
		return describe();
#else
		fprintf(stderr, "tsm-replay: --describe needs the fork's libtsm\n");
		return 2;
#endif
	}
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
		fprintf(stderr, "usage: tsm-replay [--draw2 | --frame] COLS ROWS FILE [CUTS]\n"
				"       tsm-replay --describe\n");
		return 2;
	}
	grid.cols = (unsigned int)strtoul(argv[1], NULL, 10);
	grid.rows = (unsigned int)strtoul(argv[2], NULL, 10);
	if (!grid.cols || !grid.rows || strspn(argv[1], "0123456789") != strlen(argv[1]) ||
	    strspn(argv[2], "0123456789") != strlen(argv[2])) {
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
#ifdef TSM_VTE_SIXEL
	/* paid for from that limit */
	tsm_vte_set_sixel(vte, true);
#endif
#ifdef TSM_VTE_KITTY_GRAPHICS
	tsm_vte_set_kitty_graphics(vte, true);
#endif
#ifdef TSM_VTE_SYNC
	/* a host that presents by mode 2026 */
	tsm_vte_set_sync(vte, true);
#endif
#ifdef TSM_VTE_XTVERSION
	/* a fixed XTVERSION reply, so no golden carries libtsm's version */
	tsm_vte_set_version(vte, "tsm-replay");
#endif
#ifdef TSM_SCREEN_IMAGES
	/* the fixtures beside the stream: cases/<case>.files/ */
	i = strlen(argv[3]);
	if (i >= 4 && !strcmp(argv[3] + i - 4, ".bin"))
		i -= 4;
	snprintf(fixture_dir, sizeof(fixture_dir), "%.*s.files", (int)i, argv[3]);
#endif
#ifdef TSM_VTE_IMAGE_HOST
	host.reply = &reply;
	tsm_vte_set_image_cb(vte, host_cb, NULL);
#endif
#ifdef TSM_SCREEN_CELL_UNDERLINE_COLOR
	if (view != VIEW_DRAW && tsm_vte_set_palette(vte, palette) < 0) {
		fprintf(stderr, "tsm-replay: cannot set palette %s\n", palette);
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
	free(reply.lines);
	return 0;
}
