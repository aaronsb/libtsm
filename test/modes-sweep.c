/*
 * modes-sweep - every mode number against the mode table (ADR-201 in
 * aaronsb/kmscon)
 *
 * For every DEC private mode 0 to 65535 and every ANSI mode 0 to 255:
 * DECRQM replies 0 exactly when tsm-modes.def has no row for it, and for a
 * number with no row, SM and then RM change nothing: no field of struct
 * tsm_screen, no field of struct tsm_vte but the parser's scratch
 * (tsm_vte_test_state), no row's DECRQM value, and nothing is written to the
 * host. A handler added outside the table, anywhere on the parse path,
 * changes one of these and fails here, unless all it changes is held behind
 * a pointer (cell contents, the tab ruler, image state).
 *
 * A row of sixel's (HOST_SIXEL) replies its fixed state, 3 or 4, while sixel
 * is off and 1 or 2 while it is on, and an SM or RM made while it is off
 * holds when it comes on. With --no-images, for a libtsm built with
 * -Dimages=false (ADR-103), sixel never comes on, and the row keeps its fixed
 * state when the host turns sixel on and gives it a budget.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"

struct mode_row {
	bool dec;
	unsigned int number;
	bool sixel;
};

#define MODE_KIND_ANSI false
#define MODE_KIND_DEC true
/* one per host value, so a new one fails to compile until it is placed */
#define MODE_SIXEL_HOST_NONE false
#define MODE_SIXEL_HOST_INPUT false
#define MODE_SIXEL_HOST_PASTE false
#define MODE_SIXEL_HOST_DRAW false
#define MODE_SIXEL_HOST_SIXEL true

static const struct mode_row rows[] = {
#define MODE(kind, number, name, status, dflt, reset, host, ...) \
	{ MODE_KIND_##kind, number, MODE_SIXEL_##host },
#include "tsm-modes.def"
#undef MODE
};

#define ROW_COUNT (sizeof(rows) / sizeof(*rows))

static char out[64];
static size_t out_len;
static unsigned long writes;

static void write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	(void)vte;
	(void)data;
	++writes;
	if (out_len + len < sizeof(out)) {
		memcpy(out + out_len, u8, len);
		out_len += len;
		out[out_len] = 0;
	}
}

struct state {
	struct tsm_screen screen;
	unsigned char vte[4096];
	int reports[ROW_COUNT];
	unsigned long writes;
};

static void snapshot(struct tsm_vte *vte, struct tsm_screen *con, struct state *s)
{
	size_t i;

	memset(s, 0, sizeof(*s));
	memcpy(&s->screen, con, sizeof(*con));
	if (!tsm_vte_test_state(vte, s->vte, sizeof(s->vte))) {
		fprintf(stderr, "modes-sweep: struct tsm_vte outgrew the snapshot\n");
		exit(1);
	}
	for (i = 0; i < ROW_COUNT; ++i)
		s->reports[i] = tsm_vte_get_mode(vte, rows[i].dec, rows[i].number);
	s->writes = writes;
}

/* the first byte at which @a and @b differ, or -1 */
static long first_change(const void *a, const void *b, size_t len)
{
	const unsigned char *x = a, *y = b;
	size_t i;

	for (i = 0; i < len; ++i)
		if (x[i] != y[i])
			return i;
	return -1;
}

/* what differs between @a and @b, or NULL */
static const char *changed(const struct state *a, const struct state *b)
{
	static char what[64];
	long at;

	if ((at = first_change(&a->screen, &b->screen, sizeof(a->screen))) >= 0) {
		snprintf(what, sizeof(what), "struct tsm_screen at byte %ld", at);
		return what;
	}
	if ((at = first_change(a->vte, b->vte, sizeof(a->vte))) >= 0) {
		snprintf(what, sizeof(what), "struct tsm_vte at byte %ld", at);
		return what;
	}
	if (memcmp(a->reports, b->reports, sizeof(a->reports)))
		return "a row's DECRQM value";
	if (a->writes != b->writes)
		return "what is written to the host";
	return NULL;
}

static bool has_row(bool dec, unsigned int number)
{
	size_t i;

	for (i = 0; i < ROW_COUNT; ++i)
		if (rows[i].dec == dec && rows[i].number == number)
			return true;
	return false;
}

static void input(struct tsm_vte *vte, const char *s)
{
	tsm_vte_input(vte, s, strlen(s));
}

/* the failures among the rows of sixel's, each set and reset with sixel off;
 * @images says whether turning sixel on offers them */
static unsigned int check_sixel_rows(bool images, unsigned int *checked)
{
	struct tsm_screen *con;
	struct tsm_vte *vte;
	unsigned int failures = 0, i, set;
	int off, after, on, again;
	char seq[32];

	for (i = 0; i < ROW_COUNT; ++i) {
		if (!rows[i].sixel)
			continue;
		++*checked;
		for (set = 0; set < 2; ++set) {
			if (tsm_screen_new(&con, NULL, NULL) ||
			    tsm_vte_new(&vte, con, write_cb, NULL, NULL, NULL)) {
				fprintf(stderr, "modes-sweep: cannot make a vte\n");
				exit(1);
			}
			off = tsm_vte_get_mode(vte, true, rows[i].number);
			snprintf(seq, sizeof(seq), "\e[?%u%c", rows[i].number,
				 set ? 'h' : 'l');
			input(vte, seq);
			after = tsm_vte_get_mode(vte, true, rows[i].number);
			tsm_screen_set_image_limit(con, 1 << 20);
			tsm_vte_set_sixel(vte, true);
			on = tsm_vte_get_mode(vte, true, rows[i].number);
			tsm_vte_set_sixel(vte, false);
			again = tsm_vte_get_mode(vte, true, rows[i].number);
			if ((off != 3 && off != 4) || after != off || again != off ||
			    on != (images ? (set ? 1 : 2) : off)) {
				printf("modes-sweep: DEC %u is sixel's: with sixel off it replied %d, then %d after %s; with sixel on %d, want %d; off again %d\n",
				       rows[i].number, off, after, set ? "SM" : "RM",
				       on, images ? (set ? 1 : 2) : off, again);
				++failures;
			}
			tsm_vte_unref(vte);
			tsm_screen_unref(con);
		}
	}
	return failures;
}

int main(int argc, char **argv)
{
	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct state base, now;
	unsigned int failures = 0, rows_seen = 0, unknown = 0, number, kind;
	unsigned int sixel_rows = 0;
	bool images = !(argc > 1 && !strcmp(argv[1], "--no-images"));
	char seq[32], want[32];
	const char *what;
	bool dec, row;

	if (tsm_screen_new(&con, NULL, NULL) || tsm_screen_resize(con, 8, 4) ||
	    tsm_vte_new(&vte, con, write_cb, NULL, NULL, NULL)) {
		fprintf(stderr, "modes-sweep: cannot make a vte\n");
		return 1;
	}
	/* some state for a stray handler to disturb, with mode 2026 offered
	 * and held, so a stray end is seen as well */
	tsm_vte_set_sync(vte, true);
	input(vte, "ab\r\ncd\e[2;3r\e[3;2H\e[?2026h");

	for (kind = 0; kind < 2; ++kind) {
		dec = kind;
		for (number = 0; number <= (dec ? 65535u : 255u); ++number) {
			row = has_row(dec, number);
			rows_seen += row;

			out_len = 0;
			out[0] = 0;
			snprintf(seq, sizeof(seq), "\e[%s%u$p", dec ? "?" : "", number);
			input(vte, seq);
			snprintf(want, sizeof(want), "\e[%s%u;0$y", dec ? "?" : "", number);
			if (!strcmp(out, want) == row) {
				printf("modes-sweep: %s %u: DECRQM replied \"%s\" (%s)\n",
				       dec ? "DEC" : "ANSI", number, out + 1,
				       row ? "has a row" : "no row");
				++failures;
			}
			if (row)
				continue;

			++unknown;
			snapshot(vte, con, &base);
			snprintf(seq, sizeof(seq), "\e[%s%uh", dec ? "?" : "", number);
			input(vte, seq);
			snapshot(vte, con, &now);
			if ((what = changed(&base, &now))) {
				printf("modes-sweep: %s %u has no row, and SM changed %s\n",
				       dec ? "DEC" : "ANSI", number, what);
				++failures;
			}
			snprintf(seq, sizeof(seq), "\e[%s%ul", dec ? "?" : "", number);
			input(vte, seq);
			snapshot(vte, con, &now);
			if ((what = changed(&base, &now))) {
				printf("modes-sweep: %s %u has no row, and RM changed %s\n",
				       dec ? "DEC" : "ANSI", number, what);
				++failures;
			}
			if (failures > 20) {
				printf("modes-sweep: stopping after 20 failures\n");
				goto out;
			}
		}
	}

	failures += check_sixel_rows(images, &sixel_rows);
	if (!sixel_rows) {
		printf("modes-sweep: no row is sixel's (HOST_SIXEL)\n");
		++failures;
	}

	if (rows_seen != ROW_COUNT) {
		printf("modes-sweep: met %u of the table's %zu rows\n", rows_seen,
		       ROW_COUNT);
		++failures;
	}

out:
	tsm_vte_unref(vte);
	tsm_screen_unref(con);
	if (failures)
		return 1;
	printf("modes-sweep: %u rows answer DECRQM; %u numbers with no row reply 0 and SM and RM change nothing; %u rows of sixel's reply their fixed state while sixel is off%s\n",
	       rows_seen, unknown, sixel_rows,
	       images ? "" : ", and on, in a build without images");
	return 0;
}
