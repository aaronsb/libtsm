/*
 * tsm-replay - replay a recorded byte stream into libtsm and print the
 * resulting screen state as text.
 *
 * The text is the conformance contract (ADR-301 tier 1 in the kmscon fork):
 * the same stream must produce the same text before and after a change to the
 * screen model, unless the change is meant to alter it. Only the public API is
 * used, so the tool builds against any libtsm, including upstream releases.
 *
 * Usage: tsm-replay COLS ROWS FILE [CUTS]
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
 * nothing, so wide characters keep their row aligned in a monospace view.
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

#define MAX_CP 8

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
	c = &g->cells[posy * g->cols + posx];
	c->len = len < MAX_CP ? len : MAX_CP;
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
	       !a->blink && !a->dim;
}

static bool attr_equal(const struct tsm_screen_attr *a, const struct tsm_screen_attr *b)
{
	return a->fccode == b->fccode && a->bccode == b->bccode &&
	       (a->fccode >= 0 || (a->fr == b->fr && a->fg == b->fg && a->fb == b->fb)) &&
	       (a->bccode >= 0 || (a->br == b->br && a->bg == b->bg && a->bb == b->bb)) &&
	       a->bold == b->bold && a->italic == b->italic && a->underline == b->underline &&
	       a->inverse == b->inverse && a->protect == b->protect && a->blink == b->blink &&
	       a->dim == b->dim;
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
	if (a->underline)
		printf(" underline");
	if (a->inverse)
		printf(" inverse");
	if (a->protect)
		printf(" protect");
	if (a->blink)
		printf(" blink");
	if (a->dim)
		printf(" dim");
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

static void dump(struct tsm_screen *screen, struct grid *grid, struct reply *reply)
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

	/* The cursor is reported above; keep draw from painting it into a cell. */
	memset(grid->cells, 0, (size_t)grid->cols * grid->rows * sizeof(*grid->cells));
	tsm_screen_set_flags(screen, TSM_SCREEN_HIDE_CURSOR);
	tsm_screen_draw(screen, on_draw, grid);
	if (!(flags & TSM_SCREEN_HIDE_CURSOR))
		tsm_screen_reset_flags(screen, TSM_SCREEN_HIDE_CURSOR);

	for (row = 0; row < grid->rows; row++)
		print_row(grid, row);
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

	if (argc != 4 && argc != 5) {
		fprintf(stderr, "usage: tsm-replay COLS ROWS FILE [CUTS]\n");
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

	if (argc == 5)
		cuts = read_cuts(argv[4], len, &ncuts);

	for (i = 0; i < ncuts; i++) {
		tsm_vte_input(vte, (const char *)data + pos, cuts[i] - pos);
		pos = cuts[i];
		printf("checkpoint %zu\n", pos);
		dump(screen, &grid, &reply);
	}
	tsm_vte_input(vte, (const char *)data + pos, len - pos);
	if (ncuts)
		printf("checkpoint %zu\n", len);
	dump(screen, &grid, &reply);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
	free(cuts);
	free(grid.cells);
	free(data);
	free(reply.buf);
	return 0;
}
