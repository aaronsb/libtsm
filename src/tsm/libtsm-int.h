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
/* true between sequences, where an ASCII byte is accepted as it is */
bool tsm_utf8_mach_idle(struct tsm_utf8_mach *mach);
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
	struct shl_dlist placements;	/* the image placements anchored here */
};

/*
 * Images (ADR-103 in aaronsb/kmscon; tsm-image.c). Each screen buffer has a
 * store of images and of the placements that show them. A placement is
 * anchored to the line holding its top row and listed on that line, so it
 * moves wherever the line moves: into scrollback and back, or up and down
 * the screen.
 *
 * A placement keeps its picture uncut: @ox, @oy, @dw and @dh place the whole
 * source rectangle @sx, @sy, @sw, @sh, scaled, relative to the box's top left,
 * in pixels of the cell size it was made with. Clipping and splitting move
 * the box and the anchor and leave the picture, so the part shown is always
 * the picture inside the box, and cuts never accumulate rounding.
 */
struct tsm_image_store;

struct tsm_image {
	struct shl_dlist list;		/* in its store */
	struct tsm_image_store *store;
	uint64_t key;			/* slot and generation */
	uint32_t id;			/* kitty: 1 to 2^32-1; 0 for a sixel */
	uint32_t number;		/* kitty's I, or 0 */
	unsigned int width;
	unsigned int height;
	uint32_t *pixels;		/* premultiplied ARGB32 */
	size_t bytes;			/* reserved from the budget */
	uint8_t kind;			/* enum tsm_image_kind */
	bool opaque;
	bool transient;
	bool pinned;			/* scratch: placed on a screen's own lines */
	bool dying;			/* being freed: its placements go uncharged */
	unsigned int hold;		/* never evicted while being placed */
	unsigned int placements;	/* placements showing it */
	uint64_t last_use;
	tsm_age_t age;			/* when its pixels last changed */
};

struct tsm_placement {
	struct shl_dlist line_link;	/* in its line's placements, or pending */
	struct shl_dlist store_link;	/* in its store */
	struct tsm_image *image;
	uint64_t key;			/* never reused */
	uint64_t seq;			/* creation order, which its pieces share */
	uint32_t id;			/* kitty placement id, or 0 */
	struct line *line;		/* the anchor, or NULL while pending */
	unsigned int col;
	unsigned int cols;		/* the box in cells */
	unsigned int rows;
	int ox;				/* the uncut picture, from the box's top left */
	int oy;
	unsigned int dw;
	unsigned int dh;
	unsigned int sx;		/* the uncut source rectangle */
	unsigned int sy;
	unsigned int sw;
	unsigned int sh;
	unsigned int cw;		/* the cell size it was made with */
	unsigned int ch;
	int32_t z;
	tsm_age_t age;			/* when it was added, moved or cut */
	int row;			/* scratch: its top's screen row */
};

struct tsm_image_store {
	struct shl_dlist images;
	struct shl_dlist placements;
	unsigned int count;		/* images, at most TSM_SCREEN_IMAGES_PER_STORE */
	unsigned int nplacements;	/* at most TSM_SCREEN_PLACEMENTS_PER_STORE */
	unsigned int sixels;		/* sixel placements, which text cuts */
	unsigned int max_rows;		/* at least the tallest placement's rows */
	unsigned int max_sixel_rows;	/* and the tallest sixel's */
	bool max_rows_stale;		/* a placement went since they were counted */
};

struct tsm_image_slot {
	struct tsm_image *image;
	uint32_t generation;
};

struct tsm_images {
	struct tsm_image_store main;
	struct tsm_image_store alt;

	/* image keys: slot + 1 in the low half, its generation in the high */
	struct tsm_image_slot *slots;
	unsigned int slot_count;
	unsigned int slot_free;		/* a hint: no free slot below it */

	uint64_t last_placement_key;
	uint64_t last_seq;

	/* the budget: a host's, else the screen's own limit */
	tsm_screen_image_reserve_cb reserve;
	tsm_screen_image_release_cb release;
	void *budget_data;
	size_t limit;
	size_t bytes;			/* granted by the host or under the limit */
	/* the charges of placement pieces a screen event made, which the
	 * images hold but the budget has yet to grant: screen_images_settle
	 * asks for them once no event is in progress */
	size_t owed;
	struct tsm_image *keep;		/* never evicted: a replacement frees it */
	/* images and placements the eviction scans have visited, ever, which
	 * the vte counts against one input call's work */
	uint64_t work;

	unsigned int cell_width;
	unsigned int cell_height;

	/* pieces a screen event detached, to anchor once lines have moved */
	struct shl_dlist pending;

	/* the frame's placement list */
	struct tsm_frame_placement *frame;
	unsigned int frame_size;
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
	uint32_t age_epoch;			/* times age_cnt has wrapped */

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

	/* the codepoint that started the last printed cell, which REP
	 * repeats; a codepoint that joins a cluster leaves it, as Ghostty
	 * keeps previous_char; 0 before any print and after a reset */
	tsm_symbol_t last_printed;

	/* draw2 interface */
	struct tsm_screen_cell *cells;
	unsigned int cells_count;

	/* images and their placements */
	struct tsm_images img;

	/* frame interface */
	struct tsm_frame frame;
	struct tsm_frame_cell *frame_cells;
	unsigned int frame_cells_count;
	struct tsm_frame_age frame_age;		/* tsm_screen_frame's own caller age */
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

void screen_age_wrap(struct tsm_screen *con);

/*
 * Images: what each screen event does to placements (ADR-103's table). Each
 * acts on the store of the buffer in use.
 */
void screen_images_init(struct tsm_screen *con);
void screen_images_destroy(struct tsm_screen *con);
/* a line is freed: the placements anchored on it go */
void screen_images_line_free(struct tsm_screen *con, struct line *line);
/* a line is discarded: its placements lose their top row and move to @next */
void screen_images_discard_line(struct tsm_screen *con, struct line *line,
				struct line *next);
/*
 * Rows @top to @bottom of the screen are about to scroll @num rows, up or
 * down; screen_images_scroll_done anchors what moved once they have.
 */
void screen_images_scroll(struct tsm_screen *con, unsigned int top,
			  unsigned int bottom, unsigned int num, bool up);
void screen_images_scroll_done(struct tsm_screen *con);
/* cells @x0 to @x1 of screen row @y are written or erased: sixels lose them */
void screen_images_cut(struct tsm_screen *con, unsigned int y,
		       unsigned int x0, unsigned int x1);
/* ED 2: kitty placements on the screen's lines go (sixels are cut by the erase) */
void screen_images_erase_screen(struct tsm_screen *con);
/* the alternate screen is entered or left */
void screen_images_clear_alt(struct tsm_screen *con);
/* RIS */
void screen_images_reset(struct tsm_screen *con);
void screen_images_age_wrap(struct tsm_screen *con);
/*
 * An image as a protocol stores it, and a placement of it, which the test
 * calls (tsm-image-test.c) describe in the same structs. A kitty id already
 * in the store is replaced once the new image has its bytes.
 */
struct tsm_image *screen_image_find(struct tsm_screen *con, uint64_t key);
int screen_image_new(struct tsm_screen *con, const struct tsm_test_image *desc,
		     struct tsm_image **out);
int screen_image_place(struct tsm_screen *con, struct tsm_image *image,
		       const struct tsm_test_placement *pl, struct tsm_placement **out);
/*
 * Sixel (tsm-vte.c). A picture being decoded holds its bytes through
 * screen_images_reserve and screen_images_unreserve, under the screen's
 * budget like any image; screen_sixel_new then stores it with those bytes,
 * taking @pixels (freed, with the bytes released, on failure).
 * screen_sixel_place shows it at the cursor and moves the cursor as ADR-103
 * says, and screen_image_drop frees an image nothing placed.
 */
int screen_images_reserve(struct tsm_screen *con, size_t bytes);
void screen_images_unreserve(struct tsm_screen *con, size_t bytes);
int screen_sixel_new(struct tsm_screen *con, unsigned int width, unsigned int height,
		     uint32_t *pixels, bool opaque, struct tsm_image **out);
int screen_sixel_place(struct tsm_screen *con, struct tsm_image *image,
		       bool scroll, bool cursor_right);
void screen_image_drop(struct tsm_screen *con, struct tsm_image *image);
/*
 * Asks the budget for what placement pieces owe, evicting as any reservation
 * may; refused, drops the smallest placements until nothing is owed. Called
 * where no screen event is in progress.
 */
void screen_images_settle(struct tsm_screen *con);
/* the frame's placement list, flagged changed against @since */
int screen_images_frame(struct tsm_screen *con, const struct tsm_frame_age *since,
			bool all_changed);

static inline void screen_inc_age(struct tsm_screen *con)
{
	if (!++con->age_cnt)
		screen_age_wrap(con);
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
