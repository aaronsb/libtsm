/*
 * modes-sweep - every mode number against the mode table (ADR-201 in
 * aaronsb/kmscon)
 *
 * For every DEC private mode 0 to 65535 and every ANSI mode 0 to 255:
 * DECRQM replies 0 exactly when tsm-modes.def has no row for it, and for a
 * number with no row, SM and then RM change nothing: not the screen's flags,
 * margins, cursor, age or width method, not the vte's flags, mouse fields or
 * count of synchronized frames,
 * no row's DECRQM value, and nothing is written to the host. A handler added
 * outside the table changes one of these and fails here.
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
};

#define MODE_KIND_ANSI false
#define MODE_KIND_DEC true

static const struct mode_row rows[] = {
#define MODE(kind, number, ...) { MODE_KIND_##kind, number },
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
	unsigned int screen_flags;
	unsigned int margin_top, margin_bottom;
	unsigned int cursor_x, cursor_y;
	tsm_age_t age;
	enum tsm_width_method width_method;
	unsigned int vte_flags;
	unsigned int mouse_mode, mouse_event;
	uint64_t sync_frames;
	int reports[ROW_COUNT];
	unsigned long writes;
};

static void snapshot(struct tsm_vte *vte, struct tsm_screen *con, struct state *s)
{
	size_t i;

	memset(s, 0, sizeof(*s));
	s->screen_flags = tsm_screen_get_flags(con);
	s->margin_top = con->margin_top;
	s->margin_bottom = con->margin_bottom;
	s->cursor_x = tsm_screen_get_cursor_x(con);
	s->cursor_y = tsm_screen_get_cursor_y(con);
	s->age = con->age_cnt;
	s->width_method = con->width_method;
	s->vte_flags = tsm_vte_get_flags(vte);
	s->mouse_mode = tsm_vte_get_mouse_mode(vte);
	s->mouse_event = tsm_vte_get_mouse_event(vte);
	s->sync_frames = tsm_vte_get_sync_frames(vte);
	for (i = 0; i < ROW_COUNT; ++i)
		s->reports[i] = tsm_vte_get_mode(vte, rows[i].dec, rows[i].number);
	s->writes = writes;
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

int main(void)
{
	struct tsm_screen *con;
	struct tsm_vte *vte;
	struct state base, now;
	unsigned int failures = 0, rows_seen = 0, unknown = 0, number, kind;
	char seq[32], want[32];
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
			if (memcmp(&base, &now, sizeof(base))) {
				printf("modes-sweep: %s %u has no row, and SM changed state\n",
				       dec ? "DEC" : "ANSI", number);
				++failures;
			}
			snprintf(seq, sizeof(seq), "\e[%s%ul", dec ? "?" : "", number);
			input(vte, seq);
			snapshot(vte, con, &now);
			if (memcmp(&base, &now, sizeof(base))) {
				printf("modes-sweep: %s %u has no row, and RM changed state\n",
				       dec ? "DEC" : "ANSI", number);
				++failures;
			}
			if (failures > 20) {
				printf("modes-sweep: stopping after 20 failures\n");
				goto out;
			}
		}
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
	printf("modes-sweep: %u rows answer DECRQM; %u numbers with no row reply 0 and SM and RM change nothing\n",
	       rows_seen, unknown);
	return 0;
}
