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

			/* Encode attributes into the id to avoid caching problems */
			id = cell->ch;
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
			/* don't handle multiple codepoints yet */
			if (j < line->size && line->cells[j].ch <= TSM_UCS4_MAX)
				cell = &line->cells[j];
			else
				cell = &empty;
			cell_attr_resolve(con->palette, &cell->attr, &attr);
			out->ch = cell->ch ? cell->ch : ' ';
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
