/*
 * libtsm - Screen Management
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Screen Management
 * This provides the abstracted screen management. It does not do any
 * terminal-emulation, instead it provides a resizable table of cells. You can
 * insert, remove and modify the cells freely.
 * A screen has always a fixed, but changeable, width and height. This defines
 * the number of columns and rows. The screen doesn't care for pixels, glyphs or
 * framebuffers. The screen only contains information about each cell.
 *
 * Screens are the logical model behind a real screen of a terminal emulator.
 * Users usually allocate a screen for each terminal-emulator they run. All they
 * have to do is render the screen onto their widget on each change and forward
 * any widget-events to the screen.
 *
 * The screen object already includes scrollback-buffers, selection support and
 * more. This simplifies terminal emulators a lot, but also prevents them from
 * accessing the real screen data. However, terminal emulators should have no
 * reason to access the data directly. The screen API should provide everything
 * they need.
 *
 * AGEING:
 * Each cell, line and screen has an "age" field. This field describes when it
 * was changed the last time. After drawing a screen, the current screen age is
 * returned. This allows users to skip drawing specific cells, if their
 * framebuffer was already drawn with a newer age than a given cell.
 * However, the screen-age might overflow. This is properly detected and causes
 * drawing functions to return "0" as age. Users must reset all their
 * framebuffer ages then. Otherwise, further drawing operations might
 * incorrectly skip cells.
 * Furthermore, if a cell has age "0", it means it _has_ to be drawn. No ageing
 * information is available.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "shl-llog.h"
#include "shl-macro.h"
#include "shl_dlist.h"

#define LLOG_SUBSYSTEM "tsm-screen"

static struct cell *get_cursor_cell(struct tsm_screen *con)
{
	unsigned int cur_x, cur_y;

	cur_x = con->cursor_x;
	if (cur_x >= con->size_x)
		cur_x = con->size_x - 1;

	cur_y = con->cursor_y;
	if (cur_y >= con->size_y)
		cur_y = con->size_y - 1;

	return &con->lines[cur_y]->cells[cur_x];
}

void screen_seg_reset(struct tsm_screen *con)
{
	con->seg.valid = false;
}

static void move_cursor(struct tsm_screen *con, unsigned int x, unsigned int y)
{
	struct cell *c;

	/* text printed after the cursor moves starts its own cluster */
	screen_seg_reset(con);

	/* if cursor is hidden, just move it */
	if (con->flags & TSM_SCREEN_HIDE_CURSOR) {
		con->cursor_x = x;
		con->cursor_y = y;
		return;
	}

	/* If cursor is visible, we have to mark the current and the new cell
	 * as changed by resetting their age. We skip it if the cursor-position
	 * didn't actually change. */

	if (con->cursor_x == x && con->cursor_y == y)
		return;

	c = get_cursor_cell(con);
	c->age = con->age_cnt;

	con->cursor_x = x;
	con->cursor_y = y;

	c = get_cursor_cell(con);
	c->age = con->age_cnt;
}

static void screen_cell_init_generic(struct tsm_screen *con, struct cell *cell, struct cell_attr *attr)
{
	cell->ch = 0;
	cell->width = 1;
	cell->age = con->age_cnt;

	memcpy(&cell->attr, attr, sizeof(cell->attr));
}

void screen_cell_init(struct tsm_screen *con, struct cell *cell)
{
	screen_cell_init_generic(con, cell, &con->def_attr);
}

/* drops the cell's reference on its cluster; the cell's content is then stale */
static void cell_release(struct tsm_screen *con, struct cell *cell)
{
	tsm_symbol_unref(con->sym_table, cell->ch);
}

/* empties a cell that may hold a cluster */
static void screen_cell_clear(struct tsm_screen *con, struct cell *cell)
{
	cell_release(con, cell);
	screen_cell_init(con, cell);
}

/* @cell now holds @ch: takes a reference on it and drops the old one */
static void cell_set_ch(struct tsm_screen *con, struct cell *cell, tsm_symbol_t ch)
{
	if (cell->ch == ch)
		return;
	tsm_symbol_ref(con->sym_table, ch);
	cell_release(con, cell);
	cell->ch = ch;
}

static int line_new(struct tsm_screen *con, struct line **out,
		    unsigned int width)
{
	struct line *line;
	unsigned int i;

	if (!width)
		return -EINVAL;

	line = malloc(sizeof(*line));
	if (!line)
		return -ENOMEM;
	line->list.next = NULL;
	line->list.prev = NULL;
	line->sb_id = 0;
	line->size = width;
	line->age = con->age_cnt;
	shl_dlist_init(&line->placements);

	line->cells = malloc(sizeof(struct cell) * width);
	if (!line->cells) {
		free(line);
		return -ENOMEM;
	}

	for (i = 0; i < width; ++i)
		screen_cell_init(con, &line->cells[i]);

	*out = line;
	return 0;
}

static void line_free(struct tsm_screen *con, struct line *line)
{
	unsigned int i;

	screen_images_line_free(con, line);
	for (i = 0; i < line->size; ++i)
		cell_release(con, &line->cells[i]);
	free(line->cells);
	free(line);
}

static void line_set_age(struct line *line, tsm_age_t age)
{
	unsigned int i;

	line->age = age;
	for (i = 0; i < line->size; ++i)
		line->cells[i].age = age;
}

/*
 * The age counter wrapped. Ages stored before it would read as newer than
 * every age after it, so all of them become 1, and the counter restarts
 * above that: content from before the wrap is older than any frame drawn
 * after it. A frame caller's age from before the wrap names no point in this
 * history any more; the epoch tells it so, and it draws everything.
 * tsm_screen_draw learns of the wrap through age_reset.
 */
void screen_age_wrap(struct tsm_screen *con)
{
	struct shl_dlist *iter;
	unsigned int i;

	for (i = 0; i < con->line_num; ++i) {
		line_set_age(con->main_lines[i], 1);
		line_set_age(con->alt_lines[i], 1);
	}
	shl_dlist_for_each(iter, &con->sb.list)
		line_set_age(shl_dlist_entry(iter, struct line, list), 1);
	screen_images_age_wrap(con);
	con->age = 1;
	con->age_cnt = 2;
	con->age_reset = 1;
	++con->age_epoch;
}

static int line_resize(struct tsm_screen *con, struct line *line,
		       unsigned int width)
{
	struct cell *tmp;

	if (!line || !width)
		return -EINVAL;

	if (line->size < width) {
		tmp = realloc(line->cells, width * sizeof(struct cell));
		if (!tmp)
			return -ENOMEM;

		line->cells = tmp;

		while (line->size < width) {
			screen_cell_init(con, &line->cells[line->size]);
			++line->size;
		}
	}

	return 0;
}

static void clear_selection_on_line(struct tsm_screen *con, struct line *line)
{
	if (!con->sel_active)
		return;
	if (con->sel_begin.line == line)
		con->sel_begin.line = NULL;
	if (con->sel_start.line == line)
		con->sel_start.line = NULL;
	if (con->sel_end.line == line)
		con->sel_end.line = NULL;
}

/* This links the given line into the scrollback-buffer */
static void link_to_scrollback(struct tsm_screen *con, struct line *line)
{
	struct line *tmp;

	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	if (con->sb.max == 0) {
		clear_selection_on_line(con, line);
		line_free(con, line);
		return;
	}

	/* Remove a line from the scrollback buffer if it reaches its maximum.
	 * We must take care to correctly keep the current position as the new
	 * line is linked in after we remove the top-most line here. */
	if (con->sb.count >= con->sb.max) {
		tmp = shl_dlist_first(&con->sb.list, struct line, list);
		shl_dlist_unlink(&tmp->list);
		--con->sb.count;

		/* Only consider sb.max > 1, so there is always another line in sb. */
		if (con->sb.pos == tmp) {
			con->sb.pos = shl_dlist_first(&con->sb.list, struct line, list);
			con->sb.pos_num = 0;
		} else {
			con->sb.pos_num--;
		}
		clear_selection_on_line(con, tmp);
		/* its placements lose that row and go on from the next line */
		screen_images_discard_line(con, tmp,
			shl_dlist_first(&con->sb.list, struct line, list));
		line_free(con, tmp);
	}

	line->sb_id = ++con->sb.last_id;
	shl_dlist_link_tail(&con->sb.list, &line->list);
	++con->sb.count;
	if (con->sb.pos == NULL)
		con->sb.pos_num = con->sb.count;
}

/* Remove num lines from scroll back to current buffer */
static void remove_from_sb(struct tsm_screen *con, unsigned int num)
{
	struct line *tmp;

	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	if (!con->sb.max || !con->sb.count || shl_dlist_empty(&con->sb.list))
		return;

	if (num > con->sb.count)
		num = con->sb.count;

	while (num--) {
		tmp = shl_dlist_last(&con->sb.list, struct line, list);

		if (tmp->size < con->size_x)
			if (line_resize(con, tmp, con->size_x) < 0)
				goto end_sbpos;
		shl_dlist_unlink(&tmp->list);
		--con->sb.count;
		/* ids stay contiguous, so a line's distance from the end of
		 * scrollback is a difference of ids */
		con->sb.last_id = tmp->sb_id - 1;

		if (con->sb.pos == tmp) {
			con->sb.pos_num = con->sb.count;
			con->sb.pos = NULL;
		}
		clear_selection_on_line(con, con->lines[num]);
		line_free(con, con->lines[num]);
		tmp->sb_id = 0;
		con->lines[num] = tmp;
		con->lines[num]->age = con->age_cnt;
	}
end_sbpos:
	if (!con->sb.pos)
		con->sb.pos_num = con->sb.count;
}

/*
 * Moves rows margin_top to margin_bottom up @num rows. The lines leaving the
 * top enter scrollback, on the main screen with one; others are cleared and
 * come back at the bottom.
 */
static void screen_rotate_up(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, j, max, pos;
	int ret;

	if (!num)
		return;

	screen_seg_reset(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	max = con->margin_bottom + 1 - con->margin_top;
	if (num > max)
		num = max;

	/* We cache lines on the stack to speed up the scrolling. However, if
	 * num is too big we might get overflows here so use recursion if num
	 * exceeds a hard-coded limit.
	 * 128 seems to be a sane limit that should never be reached but should
	 * also be small enough so we do not get stack overflows. */
	if (num > 128) {
		screen_rotate_up(con, 128);
		return screen_rotate_up(con, num - 128);
	}
	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		pos = con->margin_top + i;
		if (!(con->flags & TSM_SCREEN_ALTERNATE))
			ret = line_new(con, &cache[i], con->size_x);
		else
			ret = -EAGAIN;

		if (!ret) {
			link_to_scrollback(con, con->lines[pos]);
		} else {
			cache[i] = con->lines[pos];
			screen_images_line_free(con, cache[i]);
			for (j = 0; j < cache[i]->size; ++j)
				screen_cell_clear(con, &cache[i]->cells[j]);
		}
	}

	if (num < max) {
		memmove(&con->lines[con->margin_top],
			&con->lines[con->margin_top + num],
			(max - num) * sizeof(struct line*));
	}

	memcpy(&con->lines[con->margin_top + (max - num)],
	       cache, num * sizeof(struct line*));
}

/* a scroll up of the margins, which placements follow (ADR-103) */
static void screen_scroll_up(struct tsm_screen *con, unsigned int num)
{
	if (!num)
		return;
	screen_images_scroll(con, con->margin_top, con->margin_bottom, num, true);
	screen_rotate_up(con, num);
	screen_images_scroll_done(con);
}

/*
 * Moves rows margin_top to margin_bottom down @num rows; the lines leaving
 * the bottom are cleared and come back at the top.
 */
static void screen_rotate_down(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, j, max;

	if (!num)
		return;

	screen_seg_reset(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	max = con->margin_bottom + 1 - con->margin_top;
	if (num > max)
		num = max;

	/* see screen_scroll_up() for an explanation */
	if (num > 128) {
		screen_rotate_down(con, 128);
		return screen_rotate_down(con, num - 128);
	}
	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		cache[i] = con->lines[con->margin_bottom - i];
		for (j = 0; j < cache[i]->size; ++j)
			screen_cell_clear(con, &cache[i]->cells[j]);
	}

	if (num < max) {
		memmove(&con->lines[con->margin_top + num],
			&con->lines[con->margin_top],
			(max - num) * sizeof(struct line*));
	}

	memcpy(&con->lines[con->margin_top],
	       cache, num * sizeof(struct line*));
}

/* a scroll down of the margins, which placements follow (ADR-103) */
static void screen_scroll_down(struct tsm_screen *con, unsigned int num)
{
	if (!num)
		return;
	screen_images_scroll(con, con->margin_top, con->margin_bottom, num, false);
	screen_rotate_down(con, num);
	screen_images_scroll_done(con);
}

/*
 * A wide character whose halves lie on both sides of the boundary before cell
 * @x of @line goes whole: an edit on one side would leave half a character.
 * The boundary may be the margin: a line keeps its cells past it after the
 * screen narrows, and a character there must not lose a half that comes
 * back into view when it widens again.
 */
static void line_split_wide(struct tsm_screen *con, struct line *line,
			    unsigned int x)
{
	if (!x || x >= line->size || line->cells[x].width ||
	    line->cells[x - 1].width != 2)
		return;
	screen_cell_clear(con, &line->cells[x - 1]);
	screen_cell_clear(con, &line->cells[x]);
}

static void screen_write(struct tsm_screen *con, unsigned int x,
			  unsigned int y, tsm_symbol_t ch, unsigned int len,
			  const struct cell_attr *attr)
{
	struct line *line;
	unsigned int i;

	if (!len)
		return;

	if (x >= con->size_x || y >= con->size_y) {
		llog_warning(con, "writing beyond buffer boundary");
		return;
	}

	line = con->lines[y];

	/* text written over a sixel removes the part it covers; in insert
	 * mode the rest of the row shifts, as ICH does */
	screen_images_cut(con, y, x, (con->flags & TSM_SCREEN_INSERT_MODE) ?
			  con->size_x - 1 : x + len - 1);

	/* a wide character the write cuts goes whole: at @x, and where the
	 * write ends or, in insert mode, where the margin cuts the cells it
	 * moves */
	line_split_wide(con, line, x);
	if (!(con->flags & TSM_SCREEN_INSERT_MODE)) {
		line_split_wide(con, line, x + len);
	} else {
		if (len < con->size_x)
			line_split_wide(con, line, con->size_x - len);
		line_split_wide(con, line, con->size_x);
	}

	/* len may exceed a screen narrower than a wide cell */
	if ((con->flags & TSM_SCREEN_INSERT_MODE) &&
	    len < con->size_x && x < con->size_x - len) {
		line->age = con->age_cnt;
		/* the cells pushed past the margin go; the vacated ones are
		 * copies, overwritten below */
		for (i = con->size_x - len; i < con->size_x; ++i)
			cell_release(con, &line->cells[i]);
		memmove(&line->cells[x + len], &line->cells[x],
			sizeof(struct cell) * (con->size_x - len - x));
		for (i = 0; i < len; ++i)
			line->cells[x + i].ch = 0;
	}

	line->cells[x].age = con->age_cnt;
	cell_set_ch(con, &line->cells[x], ch);
	line->cells[x].width = len;
	memcpy(&line->cells[x].attr, attr, sizeof(*attr));

	/* the trailing half takes the character's attributes, so an erase
	 * that skips protected cells keeps or clears both halves */
	for (i = 1; i < len && i + x < con->size_x; ++i) {
		line->cells[x + i].age = con->age_cnt;
		line->cells[x + i].width = 0;
		cell_set_ch(con, &line->cells[x + i], 0);
		memcpy(&line->cells[x + i].attr, attr, sizeof(*attr));
	}
}

static void screen_erase_region(struct tsm_screen *con,
				 unsigned int x_from,
				 unsigned int y_from,
				 unsigned int x_to,
				 unsigned int y_to,
				 bool protect)
{
	unsigned int to, run;
	struct line *line;

	/* each cleared cell takes the age the caller moved to; the rest of the
	 * screen keeps its own */
	screen_seg_reset(con);

	if (y_to >= con->size_y)
		y_to = con->size_y - 1;
	if (x_to >= con->size_x)
		x_to = con->size_x - 1;

	for ( ; y_from <= y_to; ++y_from) {
		line = con->lines[y_from];
		if (!line) {
			x_from = 0;
			continue;
		}

		if (y_from == y_to)
			to = x_to;
		else
			to = con->size_x - 1;
		/* a wide character cut by the region goes whole */
		if (x_from && !line->cells[x_from].width &&
		    line->cells[x_from - 1].width == 2)
			--x_from;
		if (to + 1 < line->size && line->cells[to].width == 2 &&
		    !line->cells[to + 1].width)
			++to;
		/* sixels lose the cells erased, run by run between the
		 * protected cells a selective erase skips */
		run = x_from;
		for ( ; x_from <= to; ++x_from) {
			if (protect && line->cells[x_from].attr.protect) {
				if (run < x_from)
					screen_images_cut(con, y_from, run, x_from - 1);
				run = x_from + 1;
				continue;
			}

			screen_cell_clear(con, &line->cells[x_from]);
		}
		if (run <= to)
			screen_images_cut(con, y_from, run, to);
		x_from = 0;
	}
}

static inline unsigned int to_abs_x(struct tsm_screen *con, unsigned int x)
{
	return x;
}

static inline unsigned int to_abs_y(struct tsm_screen *con, unsigned int y)
{
	if (!(con->flags & TSM_SCREEN_REL_ORIGIN))
		return y;

	return con->margin_top + y;
}

static void reset_scrollback_position(struct tsm_screen *con)
{
	con->sb.pos = NULL;
	con->sb.pos_num = con->sb.count;
}

SHL_EXPORT
int tsm_screen_new(struct tsm_screen **out, tsm_log_t log, void *log_data)
{
	struct tsm_screen *con;
	int ret;
	unsigned int i;

	if (!out)
		return -EINVAL;

	con = malloc(sizeof(*con));
	if (!con)
		return -ENOMEM;

	memset(con, 0, sizeof(*con));
	screen_images_init(con);
	con->ref = 1;
	con->llog = log;
	con->llog_data = log_data;
	con->age_cnt = 1;
	con->age = con->age_cnt;
	/* white on black until a vte or the program sets its own */
	con->def_attr.fg_kind = CELL_COLOR_RGB;
	con->def_attr.fg.rgb[0] = 255;
	con->def_attr.fg.rgb[1] = 255;
	con->def_attr.fg.rgb[2] = 255;
	con->def_attr.bg_kind = CELL_COLOR_RGB;
	con->blinking_rate_ms = 500;
	con->width_method = TSM_WIDTH_UNICODE;
	shl_dlist_init(&con->sb.list);

	ret = tsm_symbol_table_new(&con->sym_table);
	if (ret)
		goto err_free;

	ret = tsm_screen_resize(con, 80, 24);
	if (ret)
		goto err_free;

	llog_debug(con, "new screen");
	*out = con;

	return 0;

err_free:
	for (i = 0; i < con->line_num; ++i) {
		line_free(con, con->main_lines[i]);
		line_free(con, con->alt_lines[i]);
	}
	free(con->main_lines);
	free(con->alt_lines);
	free(con->tab_ruler);
	screen_images_destroy(con);
	tsm_symbol_table_unref(con->sym_table);
	free(con);
	return ret;
}

SHL_EXPORT
void tsm_screen_ref(struct tsm_screen *con)
{
	if (!con)
		return;

	++con->ref;
}

SHL_EXPORT
void tsm_screen_unref(struct tsm_screen *con)
{
	unsigned int i;

	if (!con || !con->ref || --con->ref)
		return;

	llog_debug(con, "destroying screen");
	tsm_screen_clear_sb(con);

	for (i = 0; i < con->line_num; ++i) {
		line_free(con, con->main_lines[i]);
		line_free(con, con->alt_lines[i]);
	}
	free(con->main_lines);
	free(con->alt_lines);
	free(con->tab_ruler);
	screen_images_destroy(con);
	tsm_symbol_table_unref(con->sym_table);
	free(con->cells);
	free(con->frame_cells);
	free(con);
}

void tsm_screen_set_opts(struct tsm_screen *scr, unsigned int opts)
{
	if (!scr || !opts)
		return;

	scr->opts |= opts;
}

void tsm_screen_reset_opts(struct tsm_screen *scr, unsigned int opts)
{
	if (!scr || !opts)
		return;

	scr->opts &= ~opts;
}

unsigned int tsm_screen_get_opts(struct tsm_screen *scr)
{
	if (!scr)
		return 0;

	return scr->opts;
}

SHL_EXPORT
unsigned int tsm_screen_get_width(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->size_x;
}

SHL_EXPORT
unsigned int tsm_screen_get_height(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->size_y;
}

SHL_EXPORT
int tsm_screen_resize(struct tsm_screen *con, unsigned int x,
		      unsigned int y)
{
	struct line **cache;
	unsigned int i, width, diff;
	int ret;
	bool *tab_ruler;

	if (!con || !x || !y)
		return -EINVAL;

	if (con->size_x == x && con->size_y == y)
		return 0;

	/* First make sure the line buffer is big enough for our new screen.
	 * That is, allocate all new lines and make sure each line has enough
	 * cells to hold the new screen or the current screen. If we fail, we
	 * can safely return -ENOMEM and the buffer is still valid. We must
	 * allocate the new lines to at least the same size as the current
	 * lines. Otherwise, if this function fails in later turns, we will have
	 * invalid lines in the buffer. */
	if (y > con->line_num) {
		/* resize main buffer */
		cache = realloc(con->main_lines, sizeof(struct line*) * y);
		if (!cache)
			return -ENOMEM;

		if (con->lines == con->main_lines)
			con->lines = cache;
		con->main_lines = cache;

		/* resize alt buffer */
		cache = realloc(con->alt_lines, sizeof(struct line*) * y);
		if (!cache)
			return -ENOMEM;

		if (con->lines == con->alt_lines)
			con->lines = cache;
		con->alt_lines = cache;

		/* allocate new lines */
		if (x > con->size_x)
			width = x;
		else
			width = con->size_x;

		while (con->line_num < y) {
			ret = line_new(con, &con->main_lines[con->line_num],
				       width);
			if (ret)
				return ret;

			ret = line_new(con, &con->alt_lines[con->line_num],
				       width);
			if (ret) {
				line_free(con, con->main_lines[con->line_num]);
				return ret;
			}

			++con->line_num;
		}
	}

	/* Resize all lines in the buffer if we increase screen width. This
	 * will guarantee that all lines are big enough so we can resize the
	 * buffer without reallocating them later. */
	if (x > con->size_x) {
		tab_ruler = realloc(con->tab_ruler, sizeof(bool) * x);
		if (!tab_ruler)
			return -ENOMEM;
		con->tab_ruler = tab_ruler;

		for (i = 0; i < con->line_num; ++i) {
			ret = line_resize(con, con->main_lines[i], x);
			if (ret)
				return ret;
			ret = line_resize(con, con->alt_lines[i], x);
			if (ret)
				return ret;
		}
	}

	/* every cell now sits in a differently shaped screen and is redrawn */
	screen_seg_reset(con);
	screen_inc_age(con);
	con->age = con->age_cnt;

	/* xterm destroys margins on resize, so do we */
	con->margin_top = 0;
	con->margin_bottom = con->size_y - 1;

	/* reset tabs */
	for (i = 0; i < x; ++i) {
		if (i % 8 == 0)
			con->tab_ruler[i] = true;
		else
			con->tab_ruler[i] = false;
	}

	/* We need to adjust x-size first as screen_scroll_up() and friends may
	 * have to reallocate lines. The y-size is adjusted after them to avoid
	 * missing lines when shrinking y-size.
	 * We need to carefully look for the functions that we call here as they
	 * have stronger invariants as when called normally. */

	con->size_x = x;
	if (con->cursor_x >= con->size_x)
		move_cursor(con, con->size_x - 1, con->cursor_y);

	/* scroll buffer if screen height shrinks */
	if (y < con->size_y) {
		diff = con->size_y - y;
		if (shl_dlist_empty(&con->sb.list) || (con->flags & TSM_SCREEN_ALTERNATE)) {
			/* If there is nothing in the scrollback buffer,
			 * Only scroll up if the cursor would go off-screen */
			if (con->cursor_y >= y) {
				diff = con->cursor_y - y + 1;
				tsm_screen_scroll_up(con, diff);
				move_cursor(con, con->cursor_x, y - 1);
			}
		} else {
			tsm_screen_scroll_up(con, diff);
			if (con->cursor_y > diff)
				move_cursor(con, con->cursor_x, con->cursor_y - diff);
			else
			 	move_cursor(con, con->cursor_x, 0);
		}
	} else if (y > con->size_y) {
		diff = y - con->size_y;
		if (diff > con->sb.count)
			diff = con->sb.count;
		/*
		 * When increasing the terminal number of rows, we can move some
		 * lines from the scrollback buffer to the main buffer.
		 */
		if (diff && !(con->flags & TSM_SCREEN_ALTERNATE)) {
			con->size_y = y;
			con->margin_bottom = con->size_y - 1;
			/* the lines rotated to the top are replaced by those
			 * from scrollback: what the screen showed keeps its
			 * placements, and nothing scrolls */
			screen_rotate_down(con, diff);
			remove_from_sb(con, diff);
			move_cursor(con, con->cursor_x, con->cursor_y + diff);
		}
	}

	con->size_y = y;
	con->margin_bottom = con->size_y - 1;
	if (con->cursor_y >= con->size_y)
		move_cursor(con, con->cursor_x, con->size_y - 1);

	return 0;
}

SHL_EXPORT
int tsm_screen_set_margins(struct tsm_screen *con,
			       unsigned int top, unsigned int bottom)
{
	unsigned int upper, lower;

	if (!con)
		return -EINVAL;

	if (!top)
		top = 1;

	if (bottom <= top) {
		upper = 0;
		lower = con->size_y - 1;
	} else if (bottom > con->size_y) {
		upper = 0;
		lower = con->size_y - 1;
	} else {
		upper = top - 1;
		lower = bottom - 1;
	}

	con->margin_top = upper;
	con->margin_bottom = lower;
	return 0;
}

/* set maximum scrollback buffer size in number of lines*/
SHL_EXPORT
void tsm_screen_set_max_sb(struct tsm_screen *con,
			       unsigned int max)
{
	struct line *line;

	if (!con)
		return;

	// Don't allow only one line in the scrollback buffer, this simplifies
	// the code, and is not a useful usecase.
	if (max == 1)
		max = 2;

	screen_inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	while (con->sb.count > max) {
		line = shl_dlist_first(&con->sb.list, struct line, list);
		shl_dlist_unlink(&line->list);
		--con->sb.count;

		/* Lines go from the top, so a viewed position below them moves
		 * up one; a viewed top line passes the view to the next one,
		 * or to the screen when it was the last. */
		if (con->sb.pos == line) {
			con->sb.pos = con->sb.count ?
				shl_dlist_first(&con->sb.list, struct line, list) :
				NULL;
			con->sb.pos_num = 0;
		} else if (con->sb.pos) {
			--con->sb.pos_num;
		} else {
			con->sb.pos_num = con->sb.count;
		}

		clear_selection_on_line(con, line);
		screen_images_discard_line(con, line, con->sb.count ?
			shl_dlist_first(&con->sb.list, struct line, list) :
			con->main_lines[0]);
		line_free(con, line);
	}
	con->sb.max = max;
}

/* clear scrollback buffer */
SHL_EXPORT
void tsm_screen_clear_sb(struct tsm_screen *con)
{
	struct shl_dlist *iter, *safe;
	struct line *tmp;

	if (!con)
		return;

	screen_inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	if (con->sel_active) {
		if (con->sel_begin.line && is_in_scrollback(&con->sel_begin))
			con->sel_begin.line = NULL;
		if (con->sel_start.line && is_in_scrollback(&con->sel_start))
			con->sel_start.line = NULL;
		if (con->sel_end.line && is_in_scrollback(&con->sel_end))
			con->sel_end.line = NULL;
	}
	shl_dlist_for_each_safe(iter, safe, &con->sb.list) {
		tmp = shl_dlist_entry(iter, struct line, list);
		shl_dlist_unlink(&tmp->list);
		line_free(con, tmp);
	}
	con->sb.count = 0;
	con->sb.pos = NULL;
	con->sb.pos_num = 0;
}

SHL_EXPORT
void tsm_screen_sb_up(struct tsm_screen *con, unsigned int num)
{
	struct line *prev;

	if (!con || !num)
		return;

	screen_inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	if (shl_dlist_empty(&con->sb.list))
		return;

	while (num--) {
		if (con->sb.pos) {
			if (con->sb.pos_num == 0)
				return;

			prev = shl_dlist_prev(con->sb.pos, &con->sb.list, list);
			if (!prev) {
				llog_error(con, "prev is NULL, con->sb.pos_num: %d con->sb.count: %d",
					   con->sb.pos_num, con->sb.count);
				return;
			}
			--con->sb.pos_num;
			con->sb.pos = prev;
		} else {
			con->sb.pos = shl_dlist_last(&con->sb.list, struct line, list);
			con->sb.pos_num = con->sb.count - 1;
		}
	}
}

SHL_EXPORT
void tsm_screen_sb_down(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	screen_inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	while (num-- && con->sb.pos && con->sb.pos_num < con->sb.count) {
			con->sb.pos = shl_dlist_next(con->sb.pos, &con->sb.list, list);
			++con->sb.pos_num;
	}
	if (con->sb.pos_num == con->sb.count)
		con->sb.pos = NULL;
}

SHL_EXPORT
void tsm_screen_sb_page_up(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	screen_inc_age(con);
	tsm_screen_sb_up(con, num * con->size_y);
}

SHL_EXPORT
void tsm_screen_sb_page_down(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	screen_inc_age(con);
	tsm_screen_sb_down(con, num * con->size_y);
}

SHL_EXPORT
void tsm_screen_sb_reset(struct tsm_screen *con)
{
	if (!con || !con->sb.pos)
		return;

	screen_inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	con->sb.pos = NULL;
	con->sb.pos_num = con->sb.count;
}

unsigned int tsm_screen_sb_get_line_count(struct tsm_screen *con)
{
	if (!con) {
		return 0;
	}

	return con->sb.count;
}

unsigned int tsm_screen_sb_get_line_pos(struct tsm_screen *con)
{
	if (!con) {
		return 0;
	}

	return con->sb.pos_num;
}

void screen_set_def_cell_attr(struct tsm_screen *con, const struct cell_attr *attr)
{
	con->def_attr = *attr;
	if (!(con->flags & TSM_SCREEN_ALTERNATE))
		con->def_attr_main = *attr;
}

SHL_EXPORT
void tsm_screen_set_def_attr(struct tsm_screen *con,
				 const struct tsm_screen_attr *attr)
{
	struct cell_attr cattr;

	if (!con || !attr)
		return;
	cell_attr_from_screen_attr(attr, &cattr);
	screen_set_def_cell_attr(con, &cattr);
}

void screen_set_palette(struct tsm_screen *con, uint8_t (*palette)[3])
{
	memcpy(con->palette, palette, sizeof(con->palette));
	/* every cell resolves anew, so the whole screen is redrawn */
	screen_inc_age(con);
	con->age = con->age_cnt;
}

SHL_EXPORT
void tsm_screen_reset(struct tsm_screen *con)
{
	unsigned int i;

	if (!con)
		return;

	screen_seg_reset(con);
	screen_inc_age(con);
	con->age = con->age_cnt;

	con->last_printed = 0;
	if (con->lines == con->alt_lines)
		screen_images_clear_alt(con);
	con->flags = 0;
	con->margin_top = 0;
	con->margin_bottom = con->size_y - 1;
	con->lines = con->main_lines;

	for (i = 0; i < con->size_x; ++i) {
		if (i % 8 == 0)
			con->tab_ruler[i] = true;
		else
			con->tab_ruler[i] = false;
	}
	tsm_screen_selection_reset(con);
	reset_scrollback_position(con);
}

SHL_EXPORT
void tsm_screen_set_flags(struct tsm_screen *con, unsigned int flags)
{
	unsigned int old;
	struct cell *c;

	if (!con || !flags)
		return;

	screen_inc_age(con);

	old = con->flags;
	con->flags |= flags;

	if (!(old & TSM_SCREEN_ALTERNATE) && (flags & TSM_SCREEN_ALTERNATE)) {
		con->age = con->age_cnt;
		screen_seg_reset(con);
		con->lines = con->alt_lines;
		screen_images_clear_alt(con);
		tsm_screen_selection_reset(con);
		reset_scrollback_position(con);

		/* save attributes of main screen when we switch to alt screen */
		memcpy(&con->def_attr_main, &con->def_attr, sizeof(con->def_attr));
	}

	if (!(old & TSM_SCREEN_HIDE_CURSOR) &&
	    (flags & TSM_SCREEN_HIDE_CURSOR)) {
		c = get_cursor_cell(con);
		c->age = con->age_cnt;
	}

	if (!(old & TSM_SCREEN_INVERSE) && (flags & TSM_SCREEN_INVERSE))
		con->age = con->age_cnt;
}

SHL_EXPORT
void tsm_screen_reset_flags(struct tsm_screen *con, unsigned int flags)
{
	unsigned int old;
	struct cell *c;

	if (!con || !flags)
		return;

	screen_inc_age(con);

	old = con->flags;
	con->flags &= ~flags;

	if ((old & TSM_SCREEN_ALTERNATE) && (flags & TSM_SCREEN_ALTERNATE)) {
		con->age = con->age_cnt;
		screen_seg_reset(con);
		con->lines = con->main_lines;
		screen_images_clear_alt(con);
		tsm_screen_selection_reset(con);
		reset_scrollback_position(con);
	}

	if ((old & TSM_SCREEN_HIDE_CURSOR) &&
	    (flags & TSM_SCREEN_HIDE_CURSOR)) {
		c = get_cursor_cell(con);
		c->age = con->age_cnt;
	}

	if ((old & TSM_SCREEN_INVERSE) && (flags & TSM_SCREEN_INVERSE))
		con->age = con->age_cnt;
}

SHL_EXPORT
unsigned int tsm_screen_get_flags(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->flags;
}

SHL_EXPORT
unsigned int tsm_screen_get_cursor_x(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->cursor_x;
}

SHL_EXPORT
unsigned int tsm_screen_get_cursor_y(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->cursor_y;
}

SHL_EXPORT
void tsm_screen_set_tabstop(struct tsm_screen *con)
{
	if (!con || con->cursor_x >= con->size_x)
		return;

	con->tab_ruler[con->cursor_x] = true;
}

SHL_EXPORT
void tsm_screen_reset_tabstop(struct tsm_screen *con)
{
	if (!con || con->cursor_x >= con->size_x)
		return;

	con->tab_ruler[con->cursor_x] = false;
}

SHL_EXPORT
void tsm_screen_reset_all_tabstops(struct tsm_screen *con)
{
	unsigned int i;

	if (!con)
		return;

	for (i = 0; i < con->size_x; ++i)
		con->tab_ruler[i] = false;
}

/* the last line a print at the cursor may use before the screen scrolls */
static unsigned int last_print_line(struct tsm_screen *con, unsigned int y)
{
	if (y <= con->margin_bottom || y >= con->size_y)
		return con->margin_bottom;
	return con->size_y - 1;
}

/* the cluster the last print wrote, if the next print may still join it */
static struct cell *seg_cell(struct tsm_screen *con)
{
	struct cell *cell;

	if (!con->seg.valid || con->seg.y >= con->size_y ||
	    con->seg.x >= con->size_x)
		return NULL;
	cell = &con->lines[con->seg.y]->cells[con->seg.x];
	/* an erase or scroll left something else there */
	if (cell->ch != con->seg.key)
		return NULL;
	return cell;
}

static void seg_set(struct tsm_screen *con, unsigned int x, unsigned int y,
		    tsm_symbol_t key)
{
	con->seg.valid = true;
	con->seg.x = x;
	con->seg.y = y;
	con->seg.key = key;
}

/* the segmentation state at the end of the cluster @key */
static void seg_replay(struct tsm_screen *con, tsm_symbol_t key)
{
	const uint32_t *cps;
	size_t len, i;

	cps = tsm_symbol_get(con->sym_table, &key, &len);
	tsm_gb_start(&con->seg.gb, cps[0]);
	for (i = 1; i < len; ++i)
		tsm_gb_continues(&con->seg.gb, cps[i]);
}

/*
 * A VS16 made the one-cell cluster at @x, @y two cells wide: it takes the cell
 * after it. In the last column the cluster moves to the start of the next
 * line, as a wide character wraps, and the cell it left is cleared.
 */
static void widen_cell(struct tsm_screen *con, unsigned int x, unsigned int y)
{
	struct line *line = con->lines[y];
	struct cell *cell = &line->cells[x], *next;
	struct cell_attr attr;
	tsm_symbol_t key = cell->ch;

	if (x + 1 < con->size_x) {
		next = &line->cells[x + 1];
		screen_images_cut(con, y, x + 1,
				  (con->flags & TSM_SCREEN_INSERT_MODE) ?
				  con->size_x - 1 : x + 1);
		if (con->flags & TSM_SCREEN_INSERT_MODE) {
			/* the rest of the line moves right one cell, as for an
			 * inserted wide character, and the last cell goes; a
			 * wide character the margin cuts, before or after the
			 * move, goes whole */
			line_split_wide(con, line, con->size_x - 1);
			line_split_wide(con, line, con->size_x);
			cell_release(con, &line->cells[con->size_x - 1]);
			memmove(&line->cells[x + 2], &line->cells[x + 1],
				sizeof(struct cell) * (con->size_x - x - 2));
			next->ch = 0;	/* a copy now */
			line->age = con->age_cnt;
		} else {
			/* a wide character there loses its lead, so its
			 * trailing half goes too, past the margin as well */
			line_split_wide(con, line, x + 2);
		}
		cell_set_ch(con, next, 0);
		next->width = 0;
		next->attr = cell->attr;
		next->age = con->age_cnt;
		cell->width = 2;
		if (con->cursor_y == y && con->cursor_x == x + 1)
			move_cursor(con, x + 2, y);
		seg_set(con, x, y, key);
		return;
	}

	/* a wide character does not fit in the last column: without autowrap
	 * the cluster stays narrow, as a wide character is not printed there;
	 * a screen one column wide has no room for it anywhere */
	if (!(con->flags & TSM_SCREEN_AUTO_WRAP) || con->size_x < 2)
		return;

	attr = cell->attr;
	tsm_symbol_ref(con->sym_table, key);
	screen_cell_clear(con, cell);
	cell->age = con->age_cnt;
	if (y + 1 > last_print_line(con, y)) {
		screen_scroll_up(con, 1);
	} else {
		++y;
	}
	screen_write(con, 0, y, key, 2, &attr);
	tsm_symbol_unref(con->sym_table, key);
	move_cursor(con, 2, y);
	seg_set(con, 0, y, key);
}

/* adds @ch to the cluster in the cell at @x, @y */
static void cluster_append(struct tsm_screen *con, unsigned int x, unsigned int y,
			   uint32_t ch)
{
	struct line *line = con->lines[y];
	struct cell *cell = &line->cells[x];
	tsm_symbol_t key;

	key = tsm_symbol_append(con->sym_table, cell->ch, ch);
	screen_inc_age(con);
	cell_set_ch(con, cell, key);
	cell->age = con->age_cnt;
	seg_set(con, x, y, key);

	if (cell->width == 1 &&
	    tsm_symbol_get_width(con->sym_table, key, con->width_method) == 2)
		widen_cell(con, x, y);
}

/*
 * A zero-width codepoint after the cursor moved, or that the break rules
 * keep apart, joins the cell before the cursor (as foot does); with no cell
 * there it is dropped.
 */
static void join_before_cursor(struct tsm_screen *con, uint32_t ch)
{
	struct cell *cells;
	unsigned int x, y = con->cursor_y;

	if (!con->cursor_x || y >= con->size_y)
		return;
	x = con->cursor_x - 1;
	if (x >= con->size_x)
		x = con->size_x - 1;
	cells = con->lines[y]->cells;
	if (!cells[x].width && x > 0)
		--x;		/* the trailing half of a wide cell */
	if (!cells[x].ch)
		return;

	seg_replay(con, cells[x].ch);
	tsm_gb_continues(&con->seg.gb, ch);
	cluster_append(con, x, y, ch);
}

void screen_set_width_method(struct tsm_screen *con, enum tsm_width_method method)
{
	con->width_method = method;
	screen_seg_reset(con);
}

/*
 * Prints a codepoint at the cursor. It joins the cluster the last print wrote
 * when the break rules of UAX #29 say the cluster continues (the unicode
 * method), or when it has no width (wcwidth); otherwise it starts a cell.
 */
void screen_write_char(struct tsm_screen *con, tsm_symbol_t ch,
		       const struct cell_attr *attr)
{
	unsigned int last, len, x, y;
	bool joins;

	if (ch >= TSM_CLUSTER_BASE)
		ch = TSM_UCS4_REPLACEMENT;
	len = tsm_ucs4_get_width(ch);

	if (seg_cell(con)) {
		if (con->width_method == TSM_WIDTH_UNICODE)
			joins = tsm_gb_continues(&con->seg.gb, ch);
		else
			joins = !len;
		if (joins) {
			cluster_append(con, con->seg.x, con->seg.y, ch);
			return;
		}
	}
	if (!len) {
		join_before_cursor(con, ch);
		return;
	}
	con->last_printed = ch;

	screen_inc_age(con);

	last = last_print_line(con, con->cursor_y);

	if (con->cursor_x >= con->size_x) {
		if (con->flags & TSM_SCREEN_AUTO_WRAP)
			move_cursor(con, 0, con->cursor_y + 1);
		else
			move_cursor(con, con->size_x - 1, con->cursor_y);
	}

	/*
	 * A wide character with only the last column left does not fit. With
	 * autowrap it goes to the next line, and the cell it leaves is
	 * cleared, as foot pads it with a spacer (terminal.c, print_spacer).
	 * Without autowrap it is not printed and the cursor stays, which
	 * Ghostty notes is how xterm behaves (src/terminal/Terminal.zig,
	 * print). On a screen one column wide it cannot fit anywhere, and an
	 * empty cell is printed instead, as Ghostty does. Ghostty and foot
	 * are MIT-licensed.
	 */
	if (len == 2 && con->cursor_x == con->size_x - 1) {
		if (con->size_x < 2) {
			ch = 0;
			len = 1;
		} else if (!(con->flags & TSM_SCREEN_AUTO_WRAP)) {
			return;
		} else {
			line_split_wide(con, con->lines[con->cursor_y],
					con->cursor_x);
			line_split_wide(con, con->lines[con->cursor_y],
					con->cursor_x + 1);
			screen_cell_clear(con,
				&con->lines[con->cursor_y]->cells[con->cursor_x]);
			move_cursor(con, 0, con->cursor_y + 1);
		}
	}

	if (con->cursor_y > last) {
		move_cursor(con, con->cursor_x, last);
		screen_scroll_up(con, 1);
	}

	x = con->cursor_x;
	y = con->cursor_y;
	screen_write(con, x, y, ch, len, attr);
	move_cursor(con, x + len, y);
	seg_set(con, x, y, ch);
	tsm_gb_start(&con->seg.gb, ch);
}

SHL_EXPORT
void tsm_screen_write(struct tsm_screen *con, tsm_symbol_t ch,
			  const struct tsm_screen_attr *attr)
{
	struct cell_attr cattr;

	if (!con || !attr)
		return;
	cell_attr_from_screen_attr(attr, &cattr);
	screen_write_char(con, ch, &cattr);
}

SHL_EXPORT
void tsm_screen_newline(struct tsm_screen *con)
{
	if (!con)
		return;

	screen_inc_age(con);

	tsm_screen_move_down(con, 1, true);
	tsm_screen_move_line_home(con);
}

SHL_EXPORT
void tsm_screen_scroll_up(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	screen_inc_age(con);

	screen_scroll_up(con, num);
}

SHL_EXPORT
void tsm_screen_scroll_down(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	screen_inc_age(con);

	screen_scroll_down(con, num);
}

SHL_EXPORT
void tsm_screen_move_to(struct tsm_screen *con, unsigned int x,
			    unsigned int y)
{
	unsigned int last;

	if (!con)
		return;

	screen_inc_age(con);

	if (con->flags & TSM_SCREEN_REL_ORIGIN)
		last = con->margin_bottom;
	else
		last = con->size_y - 1;

	x = to_abs_x(con, x);
	if (x >= con->size_x)
		x = con->size_x - 1;

	y = to_abs_y(con, y);
	if (y > last)
		y = last;

	move_cursor(con, x, y);
}

SHL_EXPORT
void tsm_screen_move_up(struct tsm_screen *con, unsigned int num,
			    bool scroll)
{
	unsigned int diff, size;

	if (!con || !num)
		return;

	screen_inc_age(con);

	if (con->cursor_y >= con->margin_top)
		size = con->margin_top;
	else
		size = 0;

	diff = con->cursor_y - size;
	if (num > diff) {
		num -= diff;
		if (scroll)
			screen_scroll_down(con, num);
		move_cursor(con, con->cursor_x, size);
	} else {
		move_cursor(con, con->cursor_x, con->cursor_y - num);
	}
}

SHL_EXPORT
void tsm_screen_move_down(struct tsm_screen *con, unsigned int num,
			      bool scroll)
{
	unsigned int diff, size;

	if (!con || !num)
		return;

	screen_inc_age(con);

	if (con->cursor_y <= con->margin_bottom)
		size = con->margin_bottom + 1;
	else
		size = con->size_y;

	diff = size - con->cursor_y - 1;
	if (num > diff) {
		num -= diff;
		if (scroll)
			screen_scroll_up(con, num);
		move_cursor(con, con->cursor_x, size - 1);
	} else {
		move_cursor(con, con->cursor_x, con->cursor_y + num);
	}
}

SHL_EXPORT
void tsm_screen_move_left(struct tsm_screen *con, unsigned int num)
{
	unsigned int x;

	if (!con || !num)
		return;

	screen_inc_age(con);

	if (num > con->size_x)
		num = con->size_x;

	x = con->cursor_x;
	if (x >= con->size_x)
		x = con->size_x - 1;

	if (num > x)
		move_cursor(con, 0, con->cursor_y);
	else
		move_cursor(con, x - num, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_move_right(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	screen_inc_age(con);

	if (num > con->size_x)
		num = con->size_x;

	if (num + con->cursor_x >= con->size_x)
		move_cursor(con, con->size_x - 1, con->cursor_y);
	else
		move_cursor(con, con->cursor_x + num, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_move_line_end(struct tsm_screen *con)
{
	if (!con)
		return;

	screen_inc_age(con);

	move_cursor(con, con->size_x - 1, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_move_line_home(struct tsm_screen *con)
{
	if (!con)
		return;

	screen_inc_age(con);

	move_cursor(con, 0, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_tab_right(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, j, x;

	if (!con || !num)
		return;

	screen_inc_age(con);

	x = con->cursor_x;
	for (i = 0; i < num; ++i) {
		for (j = x + 1; j < con->size_x; ++j) {
			if (con->tab_ruler[j])
				break;
		}

		x = j;
		if (x + 1 >= con->size_x)
			break;
	}

	/* tabs never cause pending new-lines */
	if (x >= con->size_x)
		x = con->size_x - 1;

	move_cursor(con, x, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_tab_left(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, x;
	int j;

	if (!con || !num)
		return;

	screen_inc_age(con);

	x = con->cursor_x;

	/* cursor_x may exceed size_x (e.g. CHT then a wide glyph at the last
	 * column); clamp before indexing tab_ruler[0..size_x-1]. */
	if (x > con->size_x)
		x = con->size_x;
	for (i = 0; i < num; ++i) {
		for (j = x - 1; j > 0; --j) {
			if (con->tab_ruler[j])
				break;
		}

		if (j <= 0) {
			x = 0;
			break;
		}
		x = j;
	}

	move_cursor(con, x, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_insert_lines(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, j, max;

	if (!con || !num)
		return;

	if (con->cursor_y < con->margin_top ||
	    con->cursor_y > con->margin_bottom)
		return;

	screen_seg_reset(con);
	screen_inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	max = con->margin_bottom - con->cursor_y + 1;
	if (num > max)
		num = max;

	/* the cursor's line to the bottom margin scroll down */
	screen_images_scroll(con, con->cursor_y, con->margin_bottom, num, false);

	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		cache[i] = con->lines[con->margin_bottom - i];
		for (j = 0; j < cache[i]->size; ++j)
			screen_cell_clear(con, &cache[i]->cells[j]);
	}

	if (num < max) {
		memmove(&con->lines[con->cursor_y + num],
			&con->lines[con->cursor_y],
			(max - num) * sizeof(struct line*));

		memcpy(&con->lines[con->cursor_y],
		       cache, num * sizeof(struct line*));
	}
	screen_images_scroll_done(con);

	con->cursor_x = 0;
}

SHL_EXPORT
void tsm_screen_delete_lines(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, j, max;

	if (!con || !num)
		return;

	if (con->cursor_y < con->margin_top ||
	    con->cursor_y > con->margin_bottom)
		return;

	screen_seg_reset(con);
	screen_inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	max = con->margin_bottom - con->cursor_y + 1;
	if (num > max)
		num = max;

	/* the cursor's line to the bottom margin scroll up */
	screen_images_scroll(con, con->cursor_y, con->margin_bottom, num, true);

	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		cache[i] = con->lines[con->cursor_y + i];
		for (j = 0; j < cache[i]->size; ++j)
			screen_cell_clear(con, &cache[i]->cells[j]);
	}

	if (num < max) {
		memmove(&con->lines[con->cursor_y],
			&con->lines[con->cursor_y + num],
			(max - num) * sizeof(struct line*));

		memcpy(&con->lines[con->cursor_y + (max - num)],
		       cache, num * sizeof(struct line*));
	}
	screen_images_scroll_done(con);

	con->cursor_x = 0;
}

SHL_EXPORT
void tsm_screen_insert_chars(struct tsm_screen *con, unsigned int num)
{
	struct cell *cells;
	unsigned int max, mv, i;

	if (!con || !num || !con->size_y || !con->size_x)
		return;

	screen_seg_reset(con);
	screen_inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	if (con->cursor_x >= con->size_x)
		con->cursor_x = con->size_x - 1;
	if (con->cursor_y >= con->size_y)
		con->cursor_y = con->size_y - 1;

	max = con->size_x - con->cursor_x;
	if (num > max)
		num = max;
	mv = max - num;

	/* a sixel loses the row from the cursor on: shifting part of a
	 * picture by whole cells would tear it */
	screen_images_cut(con, con->cursor_y, con->cursor_x, con->size_x - 1);

	/* a wide character at the insertion point, where the margin cuts
	 * the moved cells, or across the margin itself goes whole */
	line_split_wide(con, con->lines[con->cursor_y], con->cursor_x);
	line_split_wide(con, con->lines[con->cursor_y], con->cursor_x + mv);
	line_split_wide(con, con->lines[con->cursor_y], con->size_x);

	cells = con->lines[con->cursor_y]->cells;
	/* the cells pushed past the margin go; the vacated ones are copies */
	for (i = 0; i < num; ++i)
		cell_release(con, &cells[con->cursor_x + mv + i]);
	if (mv)
		memmove(&cells[con->cursor_x + num],
			&cells[con->cursor_x],
			mv * sizeof(*cells));

	for (i = 0; i < num; ++i)
		screen_cell_init(con, &cells[con->cursor_x + i]);
}

/*
 * REP: prints the codepoint that started the last printed cell @num more
 * times, through the same path as printed text, so the copies wrap at the
 * margin and honour insert mode. The codepoint is the one Ghostty keeps
 * (src/terminal/Terminal.zig, printRepeat and previous_char; MIT-licensed):
 * after a base and a combining mark, REP repeats the base. Unlike Ghostty,
 * which keeps its grapheme state across the copies, each copy starts a cell
 * of its own and joins neither the cluster before it nor the copy before
 * it. Nothing is repeated before the first print.
 */
void tsm_screen_repeat_char(struct tsm_screen *con, unsigned int num,
			    const struct cell_attr *attr)
{
	if (!con || !con->last_printed)
		return;

	while (num--) {
		screen_seg_reset(con);
		screen_write_char(con, con->last_printed, attr);
	}
}

SHL_EXPORT
void tsm_screen_delete_chars(struct tsm_screen *con, unsigned int num)
{
	struct cell *cells;
	unsigned int max, mv, i;

	if (!con || !num || !con->size_y || !con->size_x)
		return;

	screen_seg_reset(con);
	screen_inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	if (con->cursor_x >= con->size_x)
		con->cursor_x = con->size_x - 1;
	if (con->cursor_y >= con->size_y)
		con->cursor_y = con->size_y - 1;

	max = con->size_x - con->cursor_x;
	if (num > max)
		num = max;
	mv = max - num;

	/* as for ICH */
	screen_images_cut(con, con->cursor_y, con->cursor_x, con->size_x - 1);

	/* a wide character at either end of the deleted cells, or across the
	 * margin, goes whole */
	line_split_wide(con, con->lines[con->cursor_y], con->cursor_x);
	line_split_wide(con, con->lines[con->cursor_y], con->cursor_x + num);
	line_split_wide(con, con->lines[con->cursor_y], con->size_x);

	cells = con->lines[con->cursor_y]->cells;
	/* the deleted cells go; the vacated ones at the end are copies */
	for (i = 0; i < num; ++i)
		cell_release(con, &cells[con->cursor_x + i]);
	if (mv)
		memmove(&cells[con->cursor_x],
			&cells[con->cursor_x + num],
			mv * sizeof(*cells));

	for (i = 0; i < num; ++i)
		screen_cell_init(con, &cells[con->cursor_x + mv + i]);
}

SHL_EXPORT
void tsm_screen_erase_cursor(struct tsm_screen *con)
{
	unsigned int x;

	if (!con)
		return;

	screen_inc_age(con);

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	screen_erase_region(con, x, con->cursor_y, x, con->cursor_y, false);
}

SHL_EXPORT
void tsm_screen_erase_chars(struct tsm_screen *con, unsigned int num)
{
	unsigned int x;

	if (!con || !num)
		return;

	screen_inc_age(con);

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	screen_erase_region(con, x, con->cursor_y, x + num - 1, con->cursor_y,
			     false);
}

SHL_EXPORT
void tsm_screen_erase_cursor_to_end(struct tsm_screen *con,
				        bool protect)
{
	unsigned int x;

	if (!con)
		return;

	screen_inc_age(con);

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	screen_erase_region(con, x, con->cursor_y, con->size_x - 1,
			     con->cursor_y, protect);
}

SHL_EXPORT
void tsm_screen_erase_home_to_cursor(struct tsm_screen *con,
					 bool protect)
{
	if (!con)
		return;

	screen_inc_age(con);

	screen_erase_region(con, 0, con->cursor_y, con->cursor_x,
			     con->cursor_y, protect);
}

SHL_EXPORT
void tsm_screen_erase_current_line(struct tsm_screen *con,
				       bool protect)
{
	if (!con)
		return;

	screen_inc_age(con);

	screen_erase_region(con, 0, con->cursor_y, con->size_x - 1,
			     con->cursor_y, protect);
}

SHL_EXPORT
void tsm_screen_erase_screen_to_cursor(struct tsm_screen *con,
					   bool protect)
{
	if (!con)
		return;

	screen_inc_age(con);

	screen_erase_region(con, 0, 0, con->cursor_x, con->cursor_y, protect);
}

SHL_EXPORT
void tsm_screen_erase_cursor_to_screen(struct tsm_screen *con,
					   bool protect)
{
	unsigned int x;

	if (!con)
		return;

	screen_inc_age(con);

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	screen_erase_region(con, x, con->cursor_y, con->size_x - 1,
			     con->size_y - 1, protect);
}

SHL_EXPORT
void tsm_screen_erase_screen(struct tsm_screen *con, bool protect)
{
	if (!con)
		return;

	screen_inc_age(con);

	/* ED 2 removes kitty placements, which no other erase touches; the
	 * erase cuts the sixels (ADR-103) */
	if (!protect)
		screen_images_erase_screen(con);

	screen_erase_region(con, 0, 0, con->size_x - 1, con->size_y - 1,
			     protect);
}

SHL_EXPORT
enum tsm_screen_cursor_style tsm_screen_get_cursor_style(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->cstyle;
}

SHL_EXPORT
void tsm_screen_set_cursor_style(struct tsm_screen *con, enum tsm_screen_cursor_style type)
{
	if (!con)
		return;

	/* the cursor cell draws differently, so it counts as changed */
	if (con->cstyle != type) {
		screen_inc_age(con);
		get_cursor_cell(con)->age = con->age_cnt;
	}
	con->cstyle = type;
}

SHL_EXPORT
uint32_t tsm_screen_get_cursor_blinking_rate_ms(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->blinking_rate_ms;
}

SHL_EXPORT
void tsm_screen_set_cursor_blinking_rate_ms(struct tsm_screen *con, uint32_t ms)
{
	if (!con)
		return;

	if (ms < 50)
		ms = 50;
	if (ms > 5000)
		ms = 5000;

	con->blinking_rate_ms = ms;
}

/*
 * A sixel at the cursor (ADR-103, Sixel). With sixel scrolling (@scroll,
 * DECSDM reset), the picture's top left is the cursor's cell; the screen
 * scrolls as index does wherever the picture would pass the bottom margin,
 * and the cursor ends on the row holding the picture's last pixel row, at its
 * left column (foot), or with @cursor_right (mode 8452) in the column after
 * its right edge, clamped to the last column. Without it (DECSDM set), the
 * picture sits at the screen's top left, cut to the screen, and the cursor
 * stays where it is.
 */
int screen_sixel_place(struct tsm_screen *con, struct tsm_image *image,
		       bool scroll, bool cursor_right)
{
	struct tsm_test_placement pl = { 0 };
	struct tsm_placement *p;
	unsigned int cw = con->img.cell_width, ch = con->img.cell_height;
	unsigned int x, y, rows, n, room, s, cols;
	int ret;

	screen_inc_age(con);
	if (!scroll) {
		pl.src_width = image->width < (unsigned long long)con->size_x * cw ?
			       image->width : con->size_x * cw;
		pl.src_height = image->height < (unsigned long long)con->size_y * ch ?
				image->height : con->size_y * ch;
		return screen_image_place(con, image, &pl, &p);
	}

	/* a pending wrap leaves the cursor past the last column */
	x = con->cursor_x < con->size_x ? con->cursor_x : con->size_x - 1;
	y = con->cursor_y;
	rows = (image->height + ch - 1) / ch;
	n = rows - 1;

	/*
	 * Scrolled first, as far as the region allows, so a picture that fits
	 * the region moves whole: scrolled after placing, a region's edge
	 * would split it. Index then covers the rest, which only a picture
	 * taller than the region needs.
	 */
	if (y >= con->margin_top && y <= con->margin_bottom) {
		room = con->margin_bottom - y;
		if (n > room) {
			s = n - room;
			if (s > y - con->margin_top)
				s = y - con->margin_top;
			screen_scroll_up(con, s);
			y -= s;
		}
	}

	pl.col = x;
	pl.row = y;
	ret = screen_image_place(con, image, &pl, &p);
	if (ret)
		return ret;
	cols = p->cols;

	move_cursor(con, x, y);
	tsm_screen_move_down(con, n, true);
	if (cursor_right)
		x = cols < con->size_x - x ? x + cols : con->size_x - 1;
	move_cursor(con, x, con->cursor_y);
	return 0;
}

/*
 * After a kitty placement the cursor indexes down one row fewer than the
 * box's rows, then moves to the column after the box, or, when that reaches
 * the right edge, indexes once more and moves to column 0. The rows indexed
 * are bounded by the distance to the bottom margin plus one screen, so a box
 * of 2^32 rows costs at most a screen of scrolling. This is Ghostty's reading
 * of the spec, adapted from the cursor movement in its display
 * (src/terminal/kitty/graphics_exec.zig, MIT).
 */
int screen_kitty_place(struct tsm_screen *con, struct tsm_image *image,
		       struct tsm_test_placement *pl, bool cursor)
{
	struct tsm_placement *p;
	unsigned int x, y, target, rows, room;
	bool wraps;
	int ret;

	/* a pending wrap leaves the cursor past the last column */
	x = con->cursor_x < con->size_x ? con->cursor_x : con->size_x - 1;
	y = con->cursor_y;
	pl->col = x;
	pl->row = y;
	ret = screen_image_place(con, image, pl, &p);
	if (ret || !cursor)
		return ret;

	target = p->cols < con->size_x - x ? x + p->cols : con->size_x;
	wraps = target >= con->size_x;
	rows = p->rows - 1 + wraps;
	room = y >= con->margin_top && y <= con->margin_bottom ?
	       con->margin_bottom - y : 0;
	if (rows > room + con->size_y)
		rows = room + con->size_y;

	tsm_screen_move_down(con, rows, true);
	move_cursor(con, wraps ? 0 : target, con->cursor_y);
	return 0;
}
