/*
 * TSM - Main internal header
 *
 * Copyright (c) 2018 Aetf <aetf@unlimitedcodeworks.xyz>
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

#ifndef TSM_LIBTSM_INT_H
#define TSM_LIBTSM_INT_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include "libtsm.h"
#include "shl_dlist.h"
#include "shl-llog.h"

#define SHL_EXPORT __attribute__((visibility("default")))

/*
 * Symbols (glyph keys)
 * A key below TSM_CLUSTER_BASE is a codepoint. A key at or above it names a
 * cluster: a sequence of at most TSM_CLUSTER_MAX codepoints in the symbol
 * table, interned by content (ADR-102 in aaronsb/kmscon). Each cluster counts
 * the cells that hold its key, through tsm_symbol_ref and tsm_symbol_unref;
 * at zero it is freed and its key reused.
 */
#define TSM_CLUSTER_BASE (1U << 21)
#define TSM_CLUSTER_MAX 32

struct tsm_symbol_table;

extern const tsm_symbol_t tsm_symbol_default;

int tsm_symbol_table_new(struct tsm_symbol_table **out);
void tsm_symbol_table_ref(struct tsm_symbol_table *tbl);
void tsm_symbol_table_unref(struct tsm_symbol_table *tbl);
/* the number of clusters held */
unsigned int tsm_symbol_table_count(struct tsm_symbol_table *tbl);

tsm_symbol_t tsm_symbol_make(uint32_t ucs4);
/* the key of @sym's codepoints followed by @ucs4, without a reference of its
 * own; @sym itself once it holds TSM_CLUSTER_MAX codepoints */
tsm_symbol_t tsm_symbol_append(struct tsm_symbol_table *tbl,
			       tsm_symbol_t sym, uint32_t ucs4);
/* a number that differs between clusters that held the same key at different
 * times; 0 for a codepoint */
uint32_t tsm_symbol_generation(struct tsm_symbol_table *tbl, tsm_symbol_t sym);
void tsm_symbol_ref(struct tsm_symbol_table *tbl, tsm_symbol_t sym);
void tsm_symbol_unref(struct tsm_symbol_table *tbl, tsm_symbol_t sym);
const uint32_t *tsm_symbol_get(struct tsm_symbol_table *tbl,
			       tsm_symbol_t *sym, size_t *size);
unsigned int tsm_symbol_get_width(struct tsm_symbol_table *tbl,
				  tsm_symbol_t sym,
				  enum tsm_width_method method);

/*
 * Grapheme cluster boundaries (UAX #29, extended grapheme clusters), applied
 * one codepoint at a time.
 */
struct tsm_gb_state {
	uint8_t gcb;		/* Grapheme_Cluster_Break of the last codepoint */
	uint8_t pict;		/* 1: Extended_Pictographic Extend*, 2: then ZWJ */
	uint8_t ri;		/* an odd number of Regional_Indicators in a row */
	uint8_t linker;		/* InCB=Linker InCB=Extend* */
};

/* SipHash-c-d of @in under the 128-bit @key */
uint64_t tsm_siphash(const uint64_t key[2], const uint8_t *in, size_t len,
		     unsigned int crounds, unsigned int drounds);

void tsm_gb_start(struct tsm_gb_state *st, uint32_t ucs4);
/* true when @ucs4 continues the cluster; @st moves on to @ucs4 either way */
bool tsm_gb_continues(struct tsm_gb_state *st, uint32_t ucs4);

/* utf8 state machine */

struct tsm_utf8_mach;

enum tsm_utf8_mach_state {
	TSM_UTF8_START,
	TSM_UTF8_ACCEPT,
	TSM_UTF8_REJECT,
	TSM_UTF8_EXPECT1,
	TSM_UTF8_EXPECT2,
	TSM_UTF8_EXPECT3,
};

int tsm_utf8_mach_new(struct tsm_utf8_mach **out);
void tsm_utf8_mach_free(struct tsm_utf8_mach *mach);

int tsm_utf8_mach_feed(struct tsm_utf8_mach *mach, char c);
uint32_t tsm_utf8_mach_get(struct tsm_utf8_mach *mach);
void tsm_utf8_mach_reset(struct tsm_utf8_mach *mach);

/* TSM screen */

/*
 * Colours as cells keep them (ADR-102 in aaronsb/kmscon): as written, and
 * resolved to RGB only when the screen is drawn, so they follow the palette in
 * use and the cell's bold. The kind of each colour lives in struct cell_attr.
 */
enum cell_color_kind {
	CELL_COLOR_CODE,	/* code: a palette entry, 0-15, TSM_COLOR_FOREGROUND
				   or TSM_COLOR_BACKGROUND */
	CELL_COLOR_INDEX,	/* rgb[0]: an index 16-255 of the 256-colour table */
	CELL_COLOR_RGB,		/* rgb as written, reported with code: -1, or the
				   code a caller of the public API gave with it */
};

struct cell_color {
	int8_t code;
	uint8_t rgb[3];
};

struct cell_attr {
	struct cell_color fg;
	struct cell_color bg;
	struct cell_color ul;			/* underline colour, if ucset */
	unsigned int fg_kind : 2;		/* enum cell_color_kind */
	unsigned int bg_kind : 2;
	unsigned int ul_kind : 2;
	unsigned int bold : 1;
	unsigned int italic : 1;
	unsigned int underline : 1;		/* any style */
	unsigned int inverse : 1;
	unsigned int protect : 1;
	unsigned int blink : 1;
	unsigned int dim : 1;
	unsigned int underline_style : 3;	/* enum tsm_underline_style */
	unsigned int strike : 1;
	unsigned int ucset : 1;			/* clear: the underline follows the foreground */
};

/* the RGB of entry @index of the 256-colour table, whose first 16 entries are
 * @palette's; black outside 0-255 */
void tsm_color_table_rgb(uint8_t (*palette)[3], int index,
			 uint8_t *r, uint8_t *g, uint8_t *b);
/* @in as struct tsm_screen_attr reports it, resolved through @palette */
void cell_attr_resolve(uint8_t (*palette)[3], const struct cell_attr *in,
		       struct tsm_screen_attr *out);
/* a struct tsm_screen_attr from the public API, kept as given */
void cell_attr_from_screen_attr(const struct tsm_screen_attr *in,
				struct cell_attr *out);

struct cell {
	tsm_symbol_t ch;		/* stored character */
	unsigned int width;		/* character width */
	struct cell_attr attr;		/* cell attributes */
	tsm_age_t age;			/* age of the single cell */
};

/* a screen of 960x135 with 1000 lines of scrollback is a million cells */
_Static_assert(sizeof(struct cell) <= 32, "struct cell grew past 32 bytes");

struct line {
	struct shl_dlist list;		/* list node, next/prev are NULL if not in sb */
	unsigned int size;		/* real width */
	struct cell *cells;		/* actuall cells */
	uint64_t sb_id;			/* sb ID, 0 if not in sb */
	tsm_age_t age;			/* age of the whole line */
};

struct selection_pos {
	unsigned int x;			/* x offset from the start of the line */
	struct line *line;		/* line the selection is on */
};

struct tsm_scrollback {
	/* scroll-back buffer */
	unsigned int count;		/* number of lines in sb */
	struct shl_dlist list;	/* list of lines in sb */
	unsigned int max;		/* max-limit of lines in sb */
	struct line *pos;		/* current position in sb or NULL */
	unsigned int pos_num;	/* current numeric position in sb */
	uint64_t last_id;		/* last id given to sb-line */
};

struct tsm_screen {
	size_t ref;
	llog_submit_t llog;
	void *llog_data;
	unsigned int opts;
	unsigned int flags;
	struct tsm_symbol_table *sym_table;

	/* default attributes for new cells */
	struct cell_attr def_attr;

	/* save default attributes of main screen here when we switch to alt screen
	 * on resize of the alt screen we need to init the new cells of the main
	 * screen with these attributes and not the ones of the alt screen */
	struct cell_attr def_attr_main;

	/* the palette colours resolve through when drawn; the vte writing to
	 * this screen keeps it in step with its own */
	uint8_t palette[TSM_COLOR_NUM][3];

	/* ageing */
	tsm_age_t age_cnt;			/* current age counter */
	unsigned int age_reset : 1;		/* age-overflow flag */

	/* current buffer */
	unsigned int size_x;			/* width of screen */
	unsigned int size_y;			/* height of screen */
	unsigned int margin_top;		/* top-margin index */
	unsigned int margin_bottom;		/* bottom-margin index */
	unsigned int line_num;			/* real number of allocated lines */
	struct line **lines;			/* active lines; copy of main/alt */
	struct line **main_lines;		/* real main lines */
	struct line **alt_lines;		/* real alternative lines */
	tsm_age_t age;				/* whole screen age */

	struct tsm_scrollback sb;

	/* cursor: positions are always in-bound, but cursor_x might be
	 * bigger than size_x if new-line is pending */
	unsigned int cursor_x;			/* current cursor x-pos */
	unsigned int cursor_y;			/* current cursor y-pos */

	enum tsm_screen_cursor_style cstyle;	/* cursor shape */
	uint32_t blinking_rate_ms;				/* cursor blinking rate */

	/* tab ruler */
	bool *tab_ruler;			/* tab-flag for all cells of one row */

	/* selection */
	bool sel_active;
	struct selection_pos sel_begin;		/* First cell selected */
	struct selection_pos sel_start;		/* First cell to copy in terminal order */
	struct selection_pos sel_end;		/* Last cell to copy */

	/* how printed text is divided into cells */
	enum tsm_width_method width_method;
	/* the cluster the last print wrote, which the next print may join */
	struct {
		bool valid;			/* cleared by cursor movement and controls */
		unsigned int x, y;		/* its lead cell */
		tsm_symbol_t key;		/* what that cell held after the print */
		struct tsm_gb_state gb;
	} seg;

	/* the last codepoint printed, which REP repeats (as foot keeps
	 * vt.last_printed); 0 before any print and after a reset */
	tsm_symbol_t last_printed;

	/* draw2 interface */
	struct tsm_screen_cell *cells;
	unsigned int cells_count;

	/* frame interface */
	struct tsm_frame frame;
	struct tsm_frame_cell *frame_cells;
	unsigned int frame_cells_count;
	tsm_age_t frame_age;			/* age_cnt when the last frame was made */
};

void screen_cell_init(struct tsm_screen *con, struct cell *cell);
void screen_write_char(struct tsm_screen *con, tsm_symbol_t ch,
		       const struct cell_attr *attr);
void screen_set_def_cell_attr(struct tsm_screen *con, const struct cell_attr *attr);
void screen_set_palette(struct tsm_screen *con, uint8_t (*palette)[3]);
void screen_seg_reset(struct tsm_screen *con);
void screen_set_width_method(struct tsm_screen *con, enum tsm_width_method method);

void tsm_screen_set_opts(struct tsm_screen *scr, unsigned int opts);
void tsm_screen_reset_opts(struct tsm_screen *scr, unsigned int opts);
unsigned int tsm_screen_get_opts(struct tsm_screen *scr);
void tsm_screen_repeat_char(struct tsm_screen *con, unsigned int num,
			    const struct cell_attr *attr);

static inline void screen_inc_age(struct tsm_screen *con)
{
	if (!++con->age_cnt) {
		con->age_reset = 1;
		++con->age_cnt;
	}
}

static inline bool is_in_scrollback(struct selection_pos *sel) {
	return (sel->line && sel->line->sb_id);
}

/* available character sets */

typedef tsm_symbol_t tsm_vte_charset[96];

extern tsm_vte_charset tsm_vte_unicode_lower;
extern tsm_vte_charset tsm_vte_unicode_upper;
extern tsm_vte_charset tsm_vte_dec_supplemental_graphics;
extern tsm_vte_charset tsm_vte_dec_special_graphics;

#endif /* TSM_LIBTSM_INT_H */
