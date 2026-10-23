/*
 * libtsm - Rendering
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
 * Rendering
 * TSM does not depend on any graphics system or rendering libraries. Instead,
 * it provides iterators and ageing support so you can implement renderers
 * yourself.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "shl-llog.h"
#include "shl_dlist.h"

#define LLOG_SUBSYSTEM "tsm-render"

/*
 * Colour resolution
 * Cells keep colours as written (struct cell_color). They become RGB here,
 * when the screen is drawn, through the screen's palette: so a palette change
 * recolours text already written, and bold brightens the base colours of the
 * cell it is on.
 */

static void cube_rgb(int index, uint8_t *r, uint8_t *g, uint8_t *b)
{
	static const uint8_t bval[6] = { 0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff };

	index -= 16;
	*b = bval[index % 6];
	index /= 6;
	*g = bval[index % 6];
	index /= 6;
	*r = bval[index % 6];
}

static void greyscale_rgb(int index, uint8_t *r, uint8_t *g, uint8_t *b)
{
	*r = *g = *b = (index - 232) * 10 + 8;
}

void tsm_color_table_rgb(uint8_t (*palette)[3], int index,
			 uint8_t *r, uint8_t *g, uint8_t *b)
{
	/* indices come from untrusted input (SGR, OSC 4); one out of range
	 * would read past palette[] or bval[] */
	if (index < 0 || index > 255) {
		*r = *g = *b = 0;
	} else if (index < 16) {
		*r = palette[index][0];
		*g = palette[index][1];
		*b = palette[index][2];
	} else if (index < 232) {
		cube_rgb(index, r, g, b);
	} else {
		greyscale_rgb(index, r, g, b);
	}
}

/* @fallback is the palette entry for a code outside it; @bright adds 8 to a
 * base colour below 8, which is how bold shows on the foreground */
static void color_resolve(uint8_t (*palette)[3], const struct cell_color *c,
			  unsigned int kind, int fallback, bool bright,
			  int8_t *code, uint8_t *r, uint8_t *g, uint8_t *b)
{
	int entry;

	switch (kind) {
	case CELL_COLOR_CODE:
		*code = c->code;
		entry = c->code;
		if (bright && entry >= 0 && entry < 8)
			entry += 8;
		if (entry < 0 || entry >= TSM_COLOR_NUM)
			entry = fallback;
		*r = palette[entry][0];
		*g = palette[entry][1];
		*b = palette[entry][2];
		break;
	case CELL_COLOR_INDEX:
		*code = -1;
		tsm_color_table_rgb(palette, c->rgb[0], r, g, b);
		break;
	default:
		*code = c->code;
		*r = c->rgb[0];
		*g = c->rgb[1];
		*b = c->rgb[2];
		break;
	}
}

void cell_attr_resolve(uint8_t (*palette)[3], const struct cell_attr *in,
		       struct tsm_screen_attr *out)
{
	memset(out, 0, sizeof(*out));
	color_resolve(palette, &in->fg, in->fg_kind, TSM_COLOR_FOREGROUND, in->bold,
		      &out->fccode, &out->fr, &out->fg, &out->fb);
	color_resolve(palette, &in->bg, in->bg_kind, TSM_COLOR_BACKGROUND, false,
		      &out->bccode, &out->br, &out->bg, &out->bb);
	if (in->ucset) {
		out->ucset = 1;
		color_resolve(palette, &in->ul, in->ul_kind, TSM_COLOR_FOREGROUND,
			      false, &out->uccode, &out->ur, &out->ug, &out->ub);
	}
	out->bold = in->bold;
	out->italic = in->italic;
	out->underline = in->underline;
	out->inverse = in->inverse;
	out->protect = in->protect;
	out->blink = in->blink;
	out->dim = in->dim;
	out->underline_style = in->underline_style;
	out->strike = in->strike;
}

static void color_from_screen_attr(struct cell_color *c, int8_t code,
				   uint8_t r, uint8_t g, uint8_t b)
{
	c->code = code;
	c->rgb[0] = r;
	c->rgb[1] = g;
	c->rgb[2] = b;
}

void cell_attr_from_screen_attr(const struct tsm_screen_attr *in,
				struct cell_attr *out)
{
	/* The public struct carries colours already resolved, and the screen
	 * has always stored and returned them as given, code included. */
	memset(out, 0, sizeof(*out));
	color_from_screen_attr(&out->fg, in->fccode, in->fr, in->fg, in->fb);
	color_from_screen_attr(&out->bg, in->bccode, in->br, in->bg, in->bb);
	out->fg_kind = CELL_COLOR_RGB;
	out->bg_kind = CELL_COLOR_RGB;
	if (in->ucset) {
		out->ucset = 1;
		color_from_screen_attr(&out->ul, in->uccode, in->ur, in->ug, in->ub);
		out->ul_kind = CELL_COLOR_RGB;
	}
	out->bold = in->bold;
	out->italic = in->italic;
	out->underline = in->underline;
	out->inverse = in->inverse;
	out->protect = in->protect;
	out->blink = in->blink;
	out->dim = in->dim;
	out->underline_style = in->underline_style;
	out->strike = in->strike;
}

SHL_EXPORT
tsm_age_t tsm_screen_draw(struct tsm_screen *con, tsm_screen_draw_cb draw_cb,
			  void *data)
{
	unsigned int cur_x, cur_y;
	unsigned int i, j, k;
	struct line *line, *next_line = NULL;
	struct cell *cell, empty;
	struct tsm_screen_attr attr;
	int ret, warned = 0;
	const uint32_t *ch;
	uint64_t id;
	size_t len;
	bool in_sel = false, sel_start = false, sel_end = false;
	bool was_sel = false;
	tsm_age_t age;

	if (!con || !draw_cb)
		return 0;

	screen_cell_init(con, &empty);

	cur_x = con->cursor_x;
	if (con->cursor_x >= con->size_x)
		cur_x = con->size_x - 1;
	cur_y = con->cursor_y;
	if (con->cursor_y >= con->size_y)
		cur_y = con->size_y - 1;

	/* push each character into rendering pipeline */
	k = 0;
	next_line = con->sb.pos;

	if (con->sel_active) {
		if (!con->sel_start.line)
			in_sel = !in_sel;
		if (!con->sel_end.line)
			in_sel = !in_sel;

		if (is_in_scrollback(&con->sel_start) &&
		    (!con->sb.pos || con->sel_start.line->sb_id < con->sb.pos->sb_id))
			in_sel = !in_sel;
		if (is_in_scrollback(&con->sel_end) &&
		    (!con->sb.pos || con->sel_end.line->sb_id < con->sb.pos->sb_id))
			in_sel = !in_sel;
	}

	for (i = 0; i < con->size_y; ++i) {
		if (next_line) {
			line = next_line;
			next_line = shl_dlist_next(next_line, &con->sb.list, list);
		} else {
			line = con->lines[k];
			k++;
		}

		if (con->sel_active) {
			if (con->sel_start.line == line)
				sel_start = true;
			else
				sel_start = false;
			if (con->sel_end.line == line)
				sel_end = true;
			else
				sel_end = false;
			was_sel = false;
		}

		for (j = 0; j < con->size_x; ++j) {
			if (j < line->size)
				cell = &line->cells[j];
			else
				cell = &empty;

			cell_attr_resolve(con->palette, &cell->attr, &attr);

			if (con->sel_active) {
				if (sel_start &&
				    j == con->sel_start.x) {
					was_sel = in_sel;
					in_sel = !in_sel;
				}
				if (sel_end &&
				    j == con->sel_end.x) {
					was_sel = in_sel;
					in_sel = !in_sel;
				}
			}

			if (k == cur_y + 1 && j == cur_x &&
			    !(con->flags & TSM_SCREEN_HIDE_CURSOR))
				attr.inverse = !attr.inverse;

			/* TODO: do some more sophisticated inverse here. When
			 * INVERSE mode is set, we should instead just select
			 * inverse colors instead of switching background and
			 * foreground */
			if (con->flags & TSM_SCREEN_INVERSE)
				attr.inverse = !attr.inverse;

			if (in_sel || was_sel) {
				was_sel = false;
				attr.inverse = !attr.inverse;
			}

			if (con->age_reset) {
				age = 0;
			} else {
				age = cell->age;
				if (line->age > age)
					age = line->age;
				if (con->age > age)
					age = con->age;
			}

			/* Encode attributes into the id to avoid caching problems.
			 * A cluster's key is reused once its cells are gone, so
			 * its generation goes in too: an id never names two
			 * different clusters. */
			id = cell->ch;
			id |= (uint64_t)(tsm_symbol_generation(con->sym_table, cell->ch) &
					 0xfffffff) << (TSM_UCS4_MAX_BITS + 5);
			if (attr.bold)
				id |= 1ULL << TSM_UCS4_MAX_BITS;
			if (attr.italic)
				id |= 1ULL << (TSM_UCS4_MAX_BITS + 1);
			if (attr.underline)
				id |= 1ULL << (TSM_UCS4_MAX_BITS + 2);
			if (attr.inverse)
				id |= 1ULL << (TSM_UCS4_MAX_BITS + 3);
			if (attr.blink)
				id |= 1ULL << (TSM_UCS4_MAX_BITS + 4);

			ch = tsm_symbol_get(con->sym_table, &cell->ch, &len);
			if (cell->ch == 0 || (cell->ch == ' ' && !attr.underline))
				len = 0;
			ret = draw_cb(con, id, ch, len, cell->width,
				      j, i, &attr, age, data);
			if (ret && warned++ < 3) {
				llog_debug(con,
					   "cannot draw glyph at %ux%u via text-renderer",
					   j, i);
				if (warned == 3)
					llog_debug(con,
						   "suppressing further warnings during this rendering round");
			}
		}
	}

	if (con->age_reset) {
		con->age_reset = 0;
		return 0;
	} else {
		return con->age_cnt;
	}
}

static void color_dim(struct tsm_screen_color  *out, const struct tsm_screen_color *fg, const struct tsm_screen_color *bg)
{
	out->r = bg->r / 2 + fg->r / 2;
	out->g = bg->g / 2 + fg->g / 2;
	out->b = bg->b / 2 + fg->b / 2;
}

/*
 * tsm_screen_draw2 returns a pointer to a table of tsm_screen_cell, one for each cell on the screen.
 * It uses a bit more memory, but should be much faster, as each cell is only 16 bytes.
 * The caller can read the table, until the next call to tsm_screen_draw2, or until con is destroyed.
 */
SHL_EXPORT
const struct tsm_screen_cell *tsm_screen_draw2(struct tsm_screen *con)
{
	unsigned int i, j, k;
	struct line *line, *next_line = NULL;
	struct cell *cell, empty;
	struct tsm_screen_cell *out;
	struct tsm_screen_attr attr;
	tsm_symbol_t ch;
	bool in_sel = false, sel_start = false, sel_end = false;
	bool inverse = false;

	if (!con)
		return NULL;

	if (con->cells_count < con->size_x * con->size_y) {
		free(con->cells);
		con->cells_count = 0;
		con->cells = malloc(con->size_x * con->size_y * sizeof(struct tsm_screen_cell));
		if (!con->cells)
			return NULL;
		con->cells_count = con->size_x * con->size_y;
		memset(con->cells, 0, con->cells_count * sizeof(struct tsm_screen_cell));
	}

	screen_cell_init(con, &empty);

	/* push each character into rendering pipeline */
	k = 0;
	next_line = con->sb.pos;

	if (con->sel_active) {
		if (!con->sel_start.line && con->sel_end.line)
			in_sel = true;

		if (is_in_scrollback(&con->sel_start)
			&& (!con->sb.pos || con->sel_start.line->sb_id < con->sb.pos->sb_id))
			in_sel = !in_sel;
		if (is_in_scrollback(&con->sel_end)
			&& (!con->sb.pos || con->sel_end.line->sb_id < con->sb.pos->sb_id))
			in_sel = !in_sel;
	}

	for (i = 0; i < con->size_y; ++i) {
		if (next_line) {
			line = next_line;
			next_line = shl_dlist_next(next_line, &con->sb.list, list);
		} else {
			line = con->lines[k];
			k++;
		}

		if (con->sel_active) {
			sel_start = (con->sel_start.line == line);
			sel_end = (con->sel_end.line == line);
		}

		for (j = 0; j < con->size_x; ++j) {
			out = &con->cells[i * con->size_x + j];
			if (j < line->size)
				cell = &line->cells[j];
			else
				cell = &empty;
			cell_attr_resolve(con->palette, &cell->attr, &attr);
			/* a cluster shows its base codepoint here; its marks and
			 * the rest of it reach tsm_screen_draw only */
			ch = cell->ch;
			out->ch = *tsm_symbol_get(con->sym_table, &ch, NULL);
			if (!out->ch)
				out->ch = ' ';
			out->attr2.bold = attr.bold;
			out->attr2.italic = attr.italic;
			out->attr2.underline = attr.underline;
			out->attr2.blink = attr.blink;

			if (sel_start && j == con->sel_start.x)
				in_sel = !in_sel;

			/* Inverse logic, cell attribute, selection, and whole screen inverse */
			inverse = attr.inverse ^ in_sel ^ (con->flags & TSM_SCREEN_INVERSE);

			if (inverse) {
				out->fg.r = attr.br;
				out->fg.g = attr.bg;
				out->fg.b = attr.bb;
				out->bg.r = attr.fr;
				out->bg.g = attr.fg;
				out->bg.b = attr.fb;
			} else {
				out->fg.r = attr.fr;
				out->fg.g = attr.fg;
				out->fg.b = attr.fb;
				out->bg.r = attr.br;
				out->bg.g = attr.bg;
				out->bg.b = attr.bb;
			}
			if (attr.dim)
				color_dim(&out->fg, &out->fg, &out->bg);

			/* A set underline colour is exact: reverse video does not
			 * swap it and dim does not touch it. A default one is the
			 * foreground as drawn, dim included (the kitty underlines
			 * doc, ADR-102). */
			out->ul_set = attr.ucset;
			if (attr.ucset) {
				out->ul.r = attr.ur;
				out->ul.g = attr.ug;
				out->ul.b = attr.ub;
			} else {
				out->ul = out->fg;
			}

			if (sel_end && j == con->sel_end.x)
				in_sel = !in_sel;
		}
	}
	return con->cells;
}

/*
 * The frame (ADR-102): every visible cell resolved for a renderer, with its
 * glyph key, width, colours after inverse and dim, underline colour and style,
 * attribute bits and whether it changed since the previous frame, plus the
 * cursor and the cell it draws.
 */

_Static_assert(TSM_FRAME_CLUSTER_KEY == TSM_CLUSTER_BASE,
	       "the public cluster key bound is the symbol table's");

static uint64_t frame_key(struct tsm_screen *con, const struct cell *cell)
{
	if (!cell->width || !cell->ch)
		return 0;
	if (cell->ch < TSM_CLUSTER_BASE)
		return cell->ch;
	return (uint64_t)cell->ch |
	       (uint64_t)tsm_symbol_generation(con->sym_table, cell->ch) << 32;
}

static void frame_resolve(struct tsm_screen *con, const struct cell *cell,
			  bool inverse, struct tsm_frame_cell *out)
{
	struct tsm_screen_attr attr;
	struct tsm_screen_color fg, bg;

	cell_attr_resolve(con->palette, &cell->attr, &attr);
	memset(out, 0, sizeof(*out));
	out->key = frame_key(con, cell);
	out->width = cell->width;

	fg.r = attr.fr;
	fg.g = attr.fg;
	fg.b = attr.fb;
	bg.r = attr.br;
	bg.g = attr.bg;
	bg.b = attr.bb;
	out->fg = inverse ? bg : fg;
	out->bg = inverse ? fg : bg;
	if (attr.dim)
		color_dim(&out->fg, &out->fg, &out->bg);

	/* a set underline colour is exact; a default one is fg as drawn */
	if (attr.ucset) {
		out->ul.r = attr.ur;
		out->ul.g = attr.ug;
		out->ul.b = attr.ub;
		out->flags |= TSM_FRAME_UL_SET;
	} else {
		out->ul = out->fg;
	}

	/* a cell written through the public API may carry only the bit */
	if (attr.underline)
		out->underline = attr.underline_style ? attr.underline_style :
				 TSM_UNDERLINE_SINGLE;

	if (attr.bold)
		out->flags |= TSM_FRAME_BOLD;
	if (attr.italic)
		out->flags |= TSM_FRAME_ITALIC;
	if (attr.dim)
		out->flags |= TSM_FRAME_DIM;
	if (attr.blink)
		out->flags |= TSM_FRAME_BLINK;
	if (inverse)
		out->flags |= TSM_FRAME_INVERSE;
	if (attr.protect)
		out->flags |= TSM_FRAME_PROTECT;
	if (attr.strike)
		out->flags |= TSM_FRAME_STRIKE;
}

/* the cell under the cursor as the cursor draws it, as kmscon has drawn it */
static void frame_cursor_cell(struct tsm_frame_cursor *cur)
{
	struct tsm_screen_color swap;

	switch (cur->style) {
	case TSM_SCREEN_CURSOR_UNDERLINE_BLINK:
	case TSM_SCREEN_CURSOR_UNDERLINE_STEADY:
		cur->cell.underline = cur->cell.underline ? TSM_UNDERLINE_NONE :
				      TSM_UNDERLINE_SINGLE;
		break;
	case TSM_SCREEN_CURSOR_VBAR_BLINK:
	case TSM_SCREEN_CURSOR_VBAR_STEADY:
		cur->cell.key = 0;
		break;
	default:
		/* the default cursor and the blocks */
		swap = cur->cell.fg;
		cur->cell.fg = cur->cell.bg;
		cur->cell.bg = swap;
		if (!(cur->cell.flags & TSM_FRAME_UL_SET))
			cur->cell.ul = cur->cell.fg;
		break;
	}
}

/* a wide character's two halves draw as one: when either changed, both are
 * flagged */
static void frame_pair_row(struct tsm_frame_cell *row, unsigned int cols)
{
	unsigned int j;
	uint16_t changed;

	for (j = 1; j < cols; ++j) {
		if (row[j].width || row[j - 1].width != 2)
			continue;
		changed = (row[j - 1].flags | row[j].flags) & TSM_FRAME_CHANGED;
		row[j - 1].flags |= changed;
		row[j].flags |= changed;
	}
}

/*
 * The frame, with TSM_FRAME_CHANGED relative to @since, which it advances.
 * The frame buffer is the screen's, whoever calls. A caller learns of an age
 * wrap from the epoch; the age-reset flag is tsm_screen_draw's alone.
 */
static const struct tsm_frame *screen_frame(struct tsm_screen *con,
					    struct tsm_frame_age *since)
{
	unsigned int i, j, k, n, cur_x, cur_y, cur_row;
	struct line *line, *next_line;
	struct cell *cell, empty;
	struct tsm_frame_cell *out, *cells, *row;
	struct tsm_frame_cursor *cur;
	bool in_sel = false, sel_start = false, sel_end = false, inverse;
	bool all_changed;
	tsm_age_t age;

	n = con->size_x * con->size_y;
	if (con->frame_cells_count < n) {
		cells = realloc(con->frame_cells, n * sizeof(*cells));
		if (!cells)
			return NULL;
		con->frame_cells = cells;
		con->frame_cells_count = n;
	}

	/* a frame of another shape has no cell in common with this one */
	all_changed = !since->age || since->epoch != con->age_epoch ||
		      con->frame.cols != con->size_x ||
		      con->frame.rows != con->size_y;
	screen_cell_init(con, &empty);

	/* the cursor sits on lines[cur_y], wherever the view shows it; at the
	 * pending-wrap column it draws on the last cell, as tsm_screen_draw
	 * places it */
	cur_x = con->cursor_x < con->size_x ? con->cursor_x : con->size_x - 1;
	cur_y = con->cursor_y < con->size_y ? con->cursor_y : con->size_y - 1;
	cur_row = con->size_y;

	k = 0;
	next_line = con->sb.pos;

	if (con->sel_active) {
		if (!con->sel_start.line && con->sel_end.line)
			in_sel = true;

		if (is_in_scrollback(&con->sel_start) &&
		    (!con->sb.pos || con->sel_start.line->sb_id < con->sb.pos->sb_id))
			in_sel = !in_sel;
		if (is_in_scrollback(&con->sel_end) &&
		    (!con->sb.pos || con->sel_end.line->sb_id < con->sb.pos->sb_id))
			in_sel = !in_sel;
	}

	for (i = 0; i < con->size_y; ++i) {
		if (next_line) {
			line = next_line;
			next_line = shl_dlist_next(next_line, &con->sb.list, list);
		} else {
			if (k == cur_y)
				cur_row = i;
			line = con->lines[k];
			k++;
		}

		if (con->sel_active) {
			sel_start = (con->sel_start.line == line);
			sel_end = (con->sel_end.line == line);
		}

		row = &con->frame_cells[i * con->size_x];
		for (j = 0; j < con->size_x; ++j) {
			out = &row[j];
			if (j < line->size) {
				cell = &line->cells[j];
				age = cell->age;
			} else {
				/* past a scrollback line made narrower */
				cell = &empty;
				age = 0;
			}
			if (line->age > age)
				age = line->age;
			if (con->age > age)
				age = con->age;

			if (sel_start && j == con->sel_start.x)
				in_sel = !in_sel;

			inverse = cell->attr.inverse ^ in_sel ^
				  !!(con->flags & TSM_SCREEN_INVERSE);
			frame_resolve(con, cell, inverse, out);
			if (all_changed || age > since->age)
				out->flags |= TSM_FRAME_CHANGED;

			if (sel_end && j == con->sel_end.x)
				in_sel = !in_sel;
		}
		frame_pair_row(row, con->size_x);
	}

	con->frame.cols = con->size_x;
	con->frame.rows = con->size_y;
	con->frame.cell_size = sizeof(struct tsm_frame_cell);
	con->frame.cells = con->frame_cells;

	/* on the trailing half of a wide character, the cursor covers the
	 * character, from its leading cell */
	cur = &con->frame.cursor;
	memset(cur, 0, sizeof(*cur));
	cur->style = con->cstyle;
	cur->visible = !(con->flags & TSM_SCREEN_HIDE_CURSOR) &&
		       cur_row < con->size_y;
	if (cur->visible) {
		row = &con->frame_cells[cur_row * con->size_x];
		if (cur_x && !row[cur_x].width && row[cur_x - 1].width == 2)
			--cur_x;
		cur->cell = row[cur_x];
		frame_cursor_cell(cur);
	}
	cur->x = cur_x;
	cur->y = cur->visible ? cur_row : cur_y;

	if (screen_images_frame(con, since, all_changed))
		return NULL;

	/* later changes take a newer age than this frame */
	since->age = con->age_cnt;
	since->epoch = con->age_epoch;
	screen_inc_age(con);

	return &con->frame;
}

SHL_EXPORT
const struct tsm_frame *tsm_screen_frame(struct tsm_screen *con)
{
	if (!con)
		return NULL;

	return screen_frame(con, &con->frame_age);
}

SHL_EXPORT
const struct tsm_frame *tsm_screen_frame_since(struct tsm_screen *con,
					       struct tsm_frame_age *since)
{
	if (!con || !since)
		return NULL;

	return screen_frame(con, since);
}

SHL_EXPORT
const uint32_t *tsm_screen_frame_cluster(struct tsm_screen *con, uint64_t key,
					 size_t *len)
{
	tsm_symbol_t sym = (tsm_symbol_t)key;
	const uint32_t *cps;

	if (len)
		*len = 0;
	if (!con || key < TSM_CLUSTER_BASE || (key >> 32) == 0)
		return NULL;
	if (tsm_symbol_generation(con->sym_table, sym) != (uint32_t)(key >> 32))
		return NULL;

	cps = tsm_symbol_get(con->sym_table, &sym, len);
	return cps;
}
