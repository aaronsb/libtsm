/*
 * TSM - Main Header
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

#ifndef TSM_LIBTSM_H
#define TSM_LIBTSM_H

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @mainpage
 *
 * TSM is a Terminal-emulator State Machine. It implements all common DEC-VT100
 * to DEC-VT520 control codes and features. A state-machine is used to parse TTY
 * input and saved in a virtual screen. TSM does not provide any rendering,
 * glyph/font handling or anything more advanced. TSM is just a simple
 * state-machine for control-codes handling.
 * The main use-case for TSM are terminal-emulators. TSM has no dependencies
 * other than an ISO-C99 compiler and C-library. Any terminal emulator for any
 * window-environment or rendering-pipline can make use of TSM. However, TSM can
 * also be used for control-code validation, TTY-screen-capturing or other
 * advanced users of terminal escape-sequences.
 */

/**
 * @defgroup misc Miscellaneous Definitions
 * Miscellaneous definitions
 *
 * This section contains several miscellaneous definitions of small helpers and
 * constants. These are shared between other parts of the API and have common
 * semantics/syntax.
 *
 * @{
 */

/**
 * Logging Callback
 *
 * @param data: User-provided data
 * @param file: Source code file where the log message originated or NULL
 * @param line: Line number in source code or 0
 * @param func: C function name or NULL
 * @param subs: Subsystem where the message came from or NULL
 * @param sev: Kernel-style severity between 0=FATAL and 7=DEBUG
 * @param format: Printf-formatted message
 * @param args: Arguments for printf-style @p format
 *
 * This is the type of a logging callback function. You can always pass NULL
 * instead of such a function to disable logging.
 */
typedef void (*tsm_log_t) (void *data,
			   const char *file,
			   int line,
			   const char *func,
			   const char *subs,
			   unsigned int sev,
			   const char *format,
			   va_list args);

/** @} */

/**
 * @defgroup symbols Unicode Helpers
 * Unicode helpers
 *
 * Unicode uses 32bit types to uniquely represent symbols. However, combining
 * characters allow modifications of such symbols but require additional space.
 * To avoid passing around allocated strings, TSM provides a symbol-table which
 * can store combining-characters with their base-symbol to create a new symbol.
 * This way, only the symbol-identifiers have to be passed around (which are
 * simple integers). No string allocation is needed by the API user.
 *
 * The symbol table is currently not exported. Once the API is fixed, we will
 * provide it to outside users.
 *
 * Additionally, this contains some general UTF8/UCS4 helpers.
 *
 * @{
 */

/* UCS4 helpers */

#define TSM_UCS4_MAX_BITS 31
#define TSM_UCS4_MAX ((1UL << TSM_UCS4_MAX_BITS) - 1UL)
#define TSM_UCS4_INVALID (TSM_UCS4_MAX + 1)
#define TSM_UCS4_REPLACEMENT (0xfffdUL)

/* ucs4 to utf8 converter */

/* the cells a codepoint takes, 0 to 2, from the Unicode tables the library
 * was built with */
unsigned int tsm_ucs4_get_width(uint32_t ucs4);

/*
 * How printed text is divided into cells (ADR-102 in aaronsb/kmscon).
 * TSM_WIDTH_UNICODE: grapheme clusters (UAX #29) take one cell each, as wide
 * as their first codepoint, or two when VS16 follows a base that takes it.
 * TSM_WIDTH_WCWIDTH: every codepoint with width takes its own cells, as
 * programs that count per codepoint expect; zero-width ones join the cell
 * before them.
 */
enum tsm_width_method {
	TSM_WIDTH_UNICODE,
	TSM_WIDTH_WCWIDTH,
};
size_t tsm_ucs4_to_utf8(uint32_t ucs4, char *out);
char *tsm_ucs4_to_utf8_alloc(const uint32_t *ucs4, size_t len, size_t *len_out);

/* symbols */

typedef uint32_t tsm_symbol_t;

/** @} */

/**
 * @defgroup screen Terminal Screens
 * Virtual terminal-screen implementation
 *
 * A TSM screen respresents the real screen of a terminal/application. It does
 * not render anything, but only provides a table of cells. Each cell contains
 * the stored symbol, attributes and more. Applications iterate a screen to
 * render each cell on their framebuffer.
 *
 * Screens provide all features that are expected from terminals. They include
 * scroll-back buffers, alternate screens, cursor positions and selection
 * support. Thus, it needs event-input from applications to drive these
 * features. Most of them are optional, though.
 *
 * @{
 */

struct tsm_screen;
typedef uint_fast32_t tsm_age_t;

#define TSM_SCREEN_INSERT_MODE	0x01
#define TSM_SCREEN_AUTO_WRAP	0x02
#define TSM_SCREEN_REL_ORIGIN	0x04
#define TSM_SCREEN_INVERSE	0x08
#define TSM_SCREEN_HIDE_CURSOR	0x10
#define TSM_SCREEN_FIXED_POS	0x20
#define TSM_SCREEN_ALTERNATE	0x40

/*
 * Defined when struct tsm_screen_attr carries underline_style, strike and the
 * underline colour (ucset, uccode, ur, ug, ub), so programs built against
 * several libtsm versions can test for them.
 */
#define TSM_SCREEN_ATTR_UNDERLINE_STYLE 1

enum tsm_underline_style {
	TSM_UNDERLINE_NONE	= 0,
	TSM_UNDERLINE_SINGLE	= 1,
	TSM_UNDERLINE_DOUBLE	= 2,
	TSM_UNDERLINE_CURLY	= 3,
	TSM_UNDERLINE_DOTTED	= 4,
	TSM_UNDERLINE_DASHED	= 5,
};

struct tsm_screen_attr {
	int8_t fccode;			/* foreground color code or <0 for rgb */
	int8_t bccode;			/* background color code or <0 for rgb */
	uint8_t fr;			/* foreground red */
	uint8_t fg;			/* foreground green */
	uint8_t fb;			/* foreground blue */
	uint8_t br;			/* background red */
	uint8_t bg;			/* background green */
	uint8_t bb;			/* background blue */
	unsigned int bold : 1;		/* bold character */
	unsigned int italic : 1;	/* italics character */
	unsigned int underline : 1;	/* underlined, in any style */
	unsigned int inverse : 1;	/* inverse colors */
	unsigned int protect : 1;	/* cannot be erased */
	unsigned int blink : 1;		/* blinking character */
	unsigned int dim:1;		/* dim color */
	unsigned int underline_style : 3; /* enum tsm_underline_style */
	unsigned int strike : 1;	/* struck through */
	unsigned int ucset : 1;		/* underline colour set (SGR 58); clear follows the foreground */
	int8_t uccode;			/* underline color code or <0 for rgb, if ucset */
	uint8_t ur;			/* underline red */
	uint8_t ug;			/* underline green */
	uint8_t ub;			/* underline blue */
};

/* Attributes that alter the glyph shape */
typedef union{
	struct {
		uint8_t bold      : 1;
		uint8_t italic    : 1;
		uint8_t underline : 1;
		uint8_t blink     : 1;
		uint8_t reserved  : 4;
	};
	uint8_t u8;
} tsm_screen_attr2_t;

struct tsm_screen_color {
	uint8_t r; /* red */
	uint8_t g; /* green */
	uint8_t b; /* blue */
};

/*
 * Defined when struct tsm_screen_cell carries the underline colour (ul, ul_set).
 */
#define TSM_SCREEN_CELL_UNDERLINE_COLOR 1

struct tsm_screen_cell {
	uint32_t ch;                  /* character */
	struct tsm_screen_color fg;   /* foreground color */
	struct tsm_screen_color bg;   /* background color */
	tsm_screen_attr2_t attr2;     /* glyph attributes */
	struct tsm_screen_color ul;   /* underline color: when ul_set, exactly as
				         set (SGR 58), neither swapped by reverse
				         video nor dimmed; otherwise the foreground
				         as drawn, after reverse video and dim */
	uint8_t ul_set;               /* the underline color was set by SGR 58 */
};

enum tsm_screen_cursor_style {
	TSM_SCREEN_CURSOR_DEFAULT		= 0,
	TSM_SCREEN_CURSOR_BLOCK_BLINK		= 1,
	TSM_SCREEN_CURSOR_BLOCK_STEADY		= 2,
	TSM_SCREEN_CURSOR_UNDERLINE_BLINK	= 3,
	TSM_SCREEN_CURSOR_UNDERLINE_STEADY	= 4,
	TSM_SCREEN_CURSOR_VBAR_BLINK		= 5,
	TSM_SCREEN_CURSOR_VBAR_STEADY		= 6,
};

/*
 * Called by tsm_screen_draw for every cell. @ch and @len are the cell's
 * codepoints: one, or a whole grapheme cluster. @id identifies what the cell
 * shows, for a glyph cache: its low 31 bits are the cell's symbol, then five
 * attribute bits, then, for a cluster, a generation. A symbol of 2^21 or more
 * names a cluster only while some cell holds it; the key is reused after, and
 * the generation keeps the id from naming a different cluster.
 */
typedef int (*tsm_screen_draw_cb) (struct tsm_screen *con,
				   uint64_t id,
				   const uint32_t *ch,
				   size_t len,
				   unsigned int width,
				   unsigned int posx,
				   unsigned int posy,
				   const struct tsm_screen_attr *attr,
				   tsm_age_t age,
				   void *data);

int tsm_screen_new(struct tsm_screen **out, tsm_log_t log, void *log_data);
void tsm_screen_ref(struct tsm_screen *con);
void tsm_screen_unref(struct tsm_screen *con);

unsigned int tsm_screen_get_width(struct tsm_screen *con);
unsigned int tsm_screen_get_height(struct tsm_screen *con);
int tsm_screen_resize(struct tsm_screen *con, unsigned int x,
		      unsigned int y);
int tsm_screen_set_margins(struct tsm_screen *con,
			   unsigned int top, unsigned int bottom);
void tsm_screen_set_max_sb(struct tsm_screen *con, unsigned int max);
void tsm_screen_clear_sb(struct tsm_screen *con);

void tsm_screen_sb_up(struct tsm_screen *con, unsigned int num);
void tsm_screen_sb_down(struct tsm_screen *con, unsigned int num);
void tsm_screen_sb_page_up(struct tsm_screen *con, unsigned int num);
void tsm_screen_sb_page_down(struct tsm_screen *con, unsigned int num);
void tsm_screen_sb_reset(struct tsm_screen *con);
unsigned int tsm_screen_sb_get_line_count(struct tsm_screen *con);
unsigned int tsm_screen_sb_get_line_pos(struct tsm_screen *con);

/* The colours in @attr are stored as given, so they do not follow palette
 * changes or bold-as-bright. */
void tsm_screen_set_def_attr(struct tsm_screen *con,
			     const struct tsm_screen_attr *attr);
void tsm_screen_reset(struct tsm_screen *con);
void tsm_screen_set_flags(struct tsm_screen *con, unsigned int flags);
void tsm_screen_reset_flags(struct tsm_screen *con, unsigned int flags);
unsigned int tsm_screen_get_flags(struct tsm_screen *con);

unsigned int tsm_screen_get_cursor_x(struct tsm_screen *con);
unsigned int tsm_screen_get_cursor_y(struct tsm_screen *con);

void tsm_screen_set_tabstop(struct tsm_screen *con);
void tsm_screen_reset_tabstop(struct tsm_screen *con);
void tsm_screen_reset_all_tabstops(struct tsm_screen *con);

/* The colours in @attr are stored as given, so they do not follow palette
 * changes or bold-as-bright. */
void tsm_screen_write(struct tsm_screen *con, tsm_symbol_t ch,
		      const struct tsm_screen_attr *attr);
void tsm_screen_newline(struct tsm_screen *con);
void tsm_screen_scroll_up(struct tsm_screen *con, unsigned int num);
void tsm_screen_scroll_down(struct tsm_screen *con, unsigned int num);
void tsm_screen_move_to(struct tsm_screen *con, unsigned int x,
			unsigned int y);
void tsm_screen_move_up(struct tsm_screen *con, unsigned int num,
			bool scroll);
void tsm_screen_move_down(struct tsm_screen *con, unsigned int num,
			  bool scroll);
void tsm_screen_move_left(struct tsm_screen *con, unsigned int num);
void tsm_screen_move_right(struct tsm_screen *con, unsigned int num);
void tsm_screen_move_line_end(struct tsm_screen *con);
void tsm_screen_move_line_home(struct tsm_screen *con);
void tsm_screen_tab_right(struct tsm_screen *con, unsigned int num);
void tsm_screen_tab_left(struct tsm_screen *con, unsigned int num);
void tsm_screen_insert_lines(struct tsm_screen *con, unsigned int num);
void tsm_screen_delete_lines(struct tsm_screen *con, unsigned int num);
void tsm_screen_insert_chars(struct tsm_screen *con, unsigned int num);
void tsm_screen_delete_chars(struct tsm_screen *con, unsigned int num);
void tsm_screen_erase_cursor(struct tsm_screen *con);
void tsm_screen_erase_chars(struct tsm_screen *con, unsigned int num);
void tsm_screen_erase_cursor_to_end(struct tsm_screen *con,
				    bool protect);
void tsm_screen_erase_home_to_cursor(struct tsm_screen *con,
				     bool protect);
void tsm_screen_erase_current_line(struct tsm_screen *con,
				   bool protect);
void tsm_screen_erase_screen_to_cursor(struct tsm_screen *con,
				       bool protect);
void tsm_screen_erase_cursor_to_screen(struct tsm_screen *con,
				       bool protect);
void tsm_screen_erase_screen(struct tsm_screen *con, bool protect);

void tsm_screen_selection_reset(struct tsm_screen *con);
void tsm_screen_selection_start(struct tsm_screen *con,
				unsigned int posx,
				unsigned int posy);
void tsm_screen_selection_target(struct tsm_screen *con,
				 unsigned int posx,
				 unsigned int posy);
void tsm_screen_selection_word(struct tsm_screen *con,
			       unsigned int posx,
			       unsigned int posy);
int tsm_screen_selection_copy(struct tsm_screen *con, char **out);

tsm_age_t tsm_screen_draw(struct tsm_screen *con, tsm_screen_draw_cb draw_cb,
			  void *data);

/* Deprecated: use tsm_screen_frame, which carries width, cluster keys, every
 * attribute and the cursor. Kept until kmscon has moved to it. */
const struct tsm_screen_cell *tsm_screen_draw2(struct tsm_screen *con);

/*
 * The frame: what a renderer draws, cell by cell (ADR-102 in aaronsb/kmscon).
 * Defined when tsm_screen_frame and its structs exist.
 */
#define TSM_SCREEN_FRAME 1

/* struct tsm_frame_cell flags */
#define TSM_FRAME_BOLD		0x0001
#define TSM_FRAME_ITALIC	0x0002
#define TSM_FRAME_DIM		0x0004	/* already applied to fg */
#define TSM_FRAME_BLINK		0x0008	/* the renderer runs the blink phase */
#define TSM_FRAME_INVERSE	0x0010	/* already applied to fg and bg; see
					   tsm_screen_frame */
#define TSM_FRAME_PROTECT	0x0020
#define TSM_FRAME_STRIKE	0x0040
#define TSM_FRAME_UL_SET	0x0080	/* ul was set by SGR 58 */
#define TSM_FRAME_CHANGED	0x0100	/* differs from the previous frame */

/* a key below this is a codepoint; at or above it, a cluster */
#define TSM_FRAME_CLUSTER_KEY	(1U << 21)

struct tsm_frame_cell {
	/*
	 * The glyph: 0 for no glyph (an empty cell or the trailing half of a
	 * wide one), a codepoint below TSM_FRAME_CLUSTER_KEY, or a cluster.
	 * A cluster key holds the cluster's symbol in its low 32 bits and a
	 * generation in its high 32, so it stays the same while any cell holds
	 * the cluster and never names two different clusters: a glyph cache
	 * can key on it. Read a cluster's codepoints with
	 * tsm_screen_frame_cluster.
	 */
	uint64_t key;
	struct tsm_screen_color fg;	/* resolved, after inverse and dim */
	struct tsm_screen_color bg;	/* resolved, after inverse */
	struct tsm_screen_color ul;	/* underline colour: as set (UL_SET),
					   else fg as drawn */
	/*
	 * 0 for a trailing half, which always follows a cell of width 2; else
	 * 1 or 2. A width-2 cell in the last column has no trailing half and
	 * is cut at the margin.
	 */
	uint8_t width;
	uint8_t underline;		/* enum tsm_underline_style */
	uint8_t reserved0;		/* 0 */
	uint16_t flags;			/* TSM_FRAME_* */
	uint16_t reserved1;		/* 0 */
};

struct tsm_frame_cursor {
	/*
	 * Where the cursor draws: the visible row that shows the cursor's line,
	 * which differs from tsm_screen_get_cursor_y while scrollback is shown;
	 * the last column at the pending-wrap position; and the leading cell
	 * when the cursor is on the trailing half of a wide character.
	 */
	unsigned int x;
	unsigned int y;
	enum tsm_screen_cursor_style style;
	/* not hidden by TSM_SCREEN_HIDE_CURSOR, and its line is in view; the
	 * renderer runs the blink phase */
	bool visible;
	/*
	 * The cell under the cursor as the cursor draws it: a block swaps fg
	 * and bg, an underline cursor toggles a single underline, a bar keeps
	 * the colours and has no glyph (key 0), for the renderer to draw its
	 * bar. A default underline colour follows the drawn fg. Valid when
	 * visible.
	 */
	struct tsm_frame_cell cell;
};

struct tsm_frame_placement;

struct tsm_frame {
	unsigned int cols;
	unsigned int rows;
	/*
	 * rows * cols cells, row by row, each cell_size bytes apart: index with
	 * cell_size, not sizeof, so a later libtsm can grow the cell without
	 * breaking a caller built against this header.
	 */
	size_t cell_size;
	const struct tsm_frame_cell *cells;
	struct tsm_frame_cursor cursor;
	/*
	 * With TSM_SCREEN_IMAGES: every placement with a visible row, in draw
	 * order, each placement_size bytes apart, as cells are cell_size apart.
	 */
	size_t placement_size;
	unsigned int placement_count;
	const struct tsm_frame_placement *placements;
};

/* cell @x, @y of @frame */
static inline const struct tsm_frame_cell *
tsm_frame_cell_at(const struct tsm_frame *frame, unsigned int x, unsigned int y)
{
	return (const struct tsm_frame_cell *)((const char *)frame->cells +
		((size_t)y * frame->cols + x) * frame->cell_size);
}

/*
 * Resolves the visible screen into a frame and returns it. The frame belongs
 * to the screen and stays valid until the next tsm_screen_frame or
 * tsm_screen_frame_since call on the screen, by any caller, or until the
 * screen is resized or freed.
 *
 * TSM_FRAME_CHANGED marks the cells whose content, colours, attributes or
 * cursor state changed since the previous tsm_screen_frame call, from the
 * screen's age; every cell on the first call and after a resize. Both halves
 * of a wide character carry it together. The screen keeps one such age, so
 * a screen drawn by more than one caller, such as one console shown on
 * several displays, uses tsm_screen_frame_since instead.
 *
 * Inverse is the cell's SGR 7, the selection and the screen's DECSCNM, each
 * toggling the others, as tsm_screen_draw applies it: a reverse-video cell on
 * a reverse-video screen draws in its normal colours. (tsm_screen_draw2
 * instead makes every cell inverse on a DECSCNM screen.)
 *
 * SGR 8 (conceal) is not parsed, so no cell is concealed.
 */
const struct tsm_frame *tsm_screen_frame(struct tsm_screen *con);

/*
 * Defined when tsm_screen_frame_since and struct tsm_frame_age exist.
 */
#define TSM_SCREEN_FRAME_SINCE 1

/*
 * One caller's place in a screen's history, for tsm_screen_frame_since. Each
 * caller that draws the screen keeps its own, zeroed before its first frame,
 * and passes it to each of its calls. The fields are the library's: a caller
 * neither reads nor sets them, beyond zeroing them to ask for a full frame.
 */
struct tsm_frame_age {
	tsm_age_t age;		/* the screen's age at this caller's last frame;
				   0 before its first */
	uint32_t epoch;		/* how often the screen's age had wrapped then */
};

/*
 * tsm_screen_frame for one of several callers: TSM_FRAME_CHANGED marks the
 * cells that changed since this caller's previous frame, as @since records
 * it, and @since is advanced to this frame. Another caller's frame, or
 * tsm_screen_frame, leaves @since and so this caller's changes as they are.
 *
 * Every cell is changed when @since is zeroed, after a resize, and on the
 * first frame after the screen's age counter wraps, which @since records.
 *
 * The frame is the same buffer tsm_screen_frame returns, shared by every
 * caller: a caller reads its frame before the next frame call on the screen,
 * its own or any other caller's, and copies what it keeps. Returns NULL, with
 * @since unchanged, when @con or @since is NULL or memory runs out.
 */
const struct tsm_frame *tsm_screen_frame_since(struct tsm_screen *con,
					       struct tsm_frame_age *since);

/*
 * The codepoints of the cluster @key names, and their count in @len. NULL if
 * @key is a codepoint, 0, or a cluster no cell holds any more. The pointer is
 * valid until the screen next changes (any input, write, erase, scroll,
 * resize or reset); a frame does not change the screen.
 */
const uint32_t *tsm_screen_frame_cluster(struct tsm_screen *con, uint64_t key,
					 size_t *len);

/*
 * Images (ADR-103 in aaronsb/kmscon): pictures sixel and the kitty graphics
 * protocol store, and placements that put them on the screen. Defined when
 * the calls and structs below exist.
 *
 * Each screen buffer, main and alternate, has its own store. A placement is
 * anchored to the line holding its top row, so it travels with the text into
 * scrollback and back. Text and erase remove the part of a sixel placement
 * they cover; of the erase commands only ED 2 touches a kitty placement.
 */
#define TSM_SCREEN_IMAGES 1

enum tsm_image_kind {
	TSM_IMAGE_SIXEL		= 0,
	TSM_IMAGE_KITTY		= 1,
};

/* the composition bands a placement's z chooses, drawn in this order */
enum tsm_frame_band {
	TSM_FRAME_BAND_BELOW_BG		= 0,	/* z < -1073741824: under cell
						   backgrounds, showing through
						   default-background cells only */
	TSM_FRAME_BAND_BELOW_TEXT	= 1,	/* z < 0: over backgrounds, under
						   glyphs and decorations */
	TSM_FRAME_BAND_ABOVE_TEXT	= 2,	/* z >= 0, and every sixel */
};

/* struct tsm_frame_placement flags, beside TSM_FRAME_CHANGED */
#define TSM_FRAME_OPAQUE	0x0200	/* no pixel of the image is below full alpha */

struct tsm_frame_placement {
	uint64_t key;			/* never reused */
	uint64_t image_key;		/* for tsm_screen_frame_image */
	/*
	 * The box in cells: its top left at visible row @row, negative when
	 * its top is above the view, and column @col; @cols by @rows cells.
	 * Drawing clips it at the frame's right edge and bottom.
	 */
	int row;
	unsigned int col;
	unsigned int cols;
	unsigned int rows;
	/*
	 * The picture's visible part: @width by @height pixels at @x, @y from
	 * the box's top left, showing the image's pixels @src_x, @src_y,
	 * @src_width by @src_height, scaled to that size. The pixel quantities
	 * here are measured in cells of @cell_width by @cell_height, the cell
	 * size the placement was made with; a renderer drawing other cells
	 * scales them by its own over these.
	 */
	unsigned int x;
	unsigned int y;
	unsigned int width;
	unsigned int height;
	unsigned int src_x;
	unsigned int src_y;
	unsigned int src_width;
	unsigned int src_height;
	unsigned int cell_width;
	unsigned int cell_height;
	int32_t z;
	uint8_t band;			/* enum tsm_frame_band */
	uint8_t kind;			/* enum tsm_image_kind */
	/*
	 * TSM_FRAME_CHANGED when the placement was added, moved, clipped or
	 * given new image data since this caller's previous frame, as cells
	 * are flagged; TSM_FRAME_OPAQUE. A placement in the previous frame and
	 * missing from this one, by key, was removed.
	 */
	uint16_t flags;
};

/* placement @i of @frame */
static inline const struct tsm_frame_placement *
tsm_frame_placement_at(const struct tsm_frame *frame, unsigned int i)
{
	return (const struct tsm_frame_placement *)((const char *)frame->placements +
		(size_t)i * frame->placement_size);
}

struct tsm_frame_image {
	unsigned int width;
	unsigned int height;
	size_t stride;			/* bytes from one row to the next */
	const uint32_t *pixels;		/* premultiplied ARGB32, native endian */
	bool opaque;			/* no pixel below full alpha */
};

/*
 * The image @key names, as a frame's placements name it. Returns 0, or
 * -ENOENT when no image has that key any more, and -EINVAL for NULL
 * arguments. The pixels are valid until the screen next changes, as
 * tsm_screen_frame_cluster's codepoints are.
 */
int tsm_screen_frame_image(struct tsm_screen *con, uint64_t key,
			   struct tsm_frame_image *out);

/*
 * The cell size in pixels, the size the host reports through TIOCSWINSZ.
 * Placements made from now on measure their pixel quantities in it; those
 * made before keep the size they were made with. 8x16 until set.
 */
int tsm_screen_set_cell_size(struct tsm_screen *con, unsigned int width,
			     unsigned int height);

/*
 * The image budget. An image costs width x height x 4 bytes, reserved before
 * it is allocated. With a host budget, libtsm asks @reserve for each
 * reservation (0 grants it, a negative errno refuses it) and tells @release
 * of every image it frees, evictions included. A host that runs several
 * screens evicts across them in its @reserve, with tsm_screen_image_victim
 * and tsm_screen_image_evict, which may call @release before @reserve
 * returns. NULL callbacks remove the host budget.
 *
 * Without one, each screen holds at most the limit tsm_screen_set_image_limit
 * sets, TSM_SCREEN_IMAGE_LIMIT_DEFAULT until then, over its two stores, and
 * evicts its own images in the order tsm_screen_image_victim gives.
 *
 * An image placed on a screen's own lines is never evicted; a new image that
 * still does not fit is refused.
 */
typedef int (*tsm_screen_image_reserve_cb)(struct tsm_screen *con,
					   size_t bytes, void *data);
typedef void (*tsm_screen_image_release_cb)(struct tsm_screen *con,
					    size_t bytes, void *data);

void tsm_screen_set_image_budget(struct tsm_screen *con,
				 tsm_screen_image_reserve_cb reserve,
				 tsm_screen_image_release_cb release,
				 void *data);

/* Ghostty's per-screen default */
#define TSM_SCREEN_IMAGE_LIMIT_DEFAULT ((size_t)320 * 1000 * 1000)

void tsm_screen_set_image_limit(struct tsm_screen *con, size_t bytes);
/* the bytes this screen's images hold, in both stores */
size_t tsm_screen_get_image_bytes(struct tsm_screen *con);

/* the eviction order: by rank, then least recently used first */
enum tsm_image_rank {
	TSM_IMAGE_UNPLACED_TRANSIENT	= 0,	/* no placement, kitty's N=1 */
	TSM_IMAGE_UNPLACED		= 1,	/* no placement */
	TSM_IMAGE_SCROLLBACK		= 2,	/* every placement wholly in
						   scrollback */
};

struct tsm_image_victim {
	uint64_t key;
	unsigned int rank;		/* enum tsm_image_rank */
	/* when it was last created, placed or drawn, on a clock all screens
	 * in the process share, so a host can order victims across screens */
	uint64_t last_use;
	size_t bytes;
};

/*
 * The image this screen evicts next, from either store: false when it has
 * none it may evict.
 */
bool tsm_screen_image_victim(struct tsm_screen *con, struct tsm_image_victim *out);
/* Evicts that image, with its placements; returns the bytes freed, 0 if none. */
size_t tsm_screen_image_evict(struct tsm_screen *con);

/*
 * For tests only (ADR-103 step 3a): an image and a placement made without a
 * protocol, as the sixel and kitty parsers will make them. Each acts on the
 * store of the screen buffer in use.
 */
struct tsm_test_image {
	unsigned int width;
	unsigned int height;
	const uint32_t *pixels;		/* width x height premultiplied ARGB32,
					   copied */
	uint8_t kind;			/* enum tsm_image_kind */
	uint32_t id;			/* kitty: 1 to 2^32-1; an existing id is
					   replaced, with its placements */
	uint32_t number;		/* kitty's I, or 0 */
	bool transient;			/* kitty's N=1 */
};

/* Stores @img and returns its key in @key: 0, -EINVAL, -ENOMEM or -ENOSPC. */
int tsm_screen_test_image(struct tsm_screen *con, const struct tsm_test_image *img,
			  uint64_t *key);

struct tsm_test_placement {
	uint64_t image;			/* the image's key */
	uint32_t id;			/* kitty placement id, or 0; one with the
					   same image and id is replaced */
	unsigned int col;		/* the top left cell, on the screen */
	unsigned int row;
	unsigned int x;			/* pixel offset inside that cell */
	unsigned int y;
	/*
	 * The box in cells. 0 for both: the picture at its pixel size, the box
	 * rounded up to whole cells. Both given: the picture scaled to fit the
	 * box less the offset, centred there. One given: the other follows the
	 * source's aspect.
	 */
	unsigned int cols;
	unsigned int rows;
	/* the source rectangle, cut to the image; a width or height of 0
	 * reaches the image's edge */
	unsigned int src_x;
	unsigned int src_y;
	unsigned int src_width;
	unsigned int src_height;
	int32_t z;			/* kitty only; a sixel is z 0 */
};

/*
 * Places an image and returns the placement's key in @key: 0, -EINVAL,
 * -ENOENT (no such image) or -ENOMEM. A sixel image whose pixels are all
 * opaque removes the parts of older sixel placements it covers, as text
 * written there would. The cursor does not move.
 */
int tsm_screen_test_place(struct tsm_screen *con, const struct tsm_test_placement *pl,
			  uint64_t *key);

enum tsm_screen_cursor_style tsm_screen_get_cursor_style(struct tsm_screen *con);
void tsm_screen_set_cursor_style(struct tsm_screen *con, enum tsm_screen_cursor_style type);

uint32_t tsm_screen_get_cursor_blinking_rate_ms(struct tsm_screen *con);
void tsm_screen_set_cursor_blinking_rate_ms(struct tsm_screen *con, uint32_t ms);

/** @} */

/**
 * @defgroup vte State Machine
 * Virtual terminal emulation with state machine
 *
 * A TSM VTE object provides the terminal state machine. It takes input from the
 * application (which usually comes from a TTY/PTY from a client), parses it,
 * modifies the attach screen or returns data which has to be written back to
 * the client.
 *
 * Furthermore, VTE objects accept keyboard or mouse input from the application
 * which is interpreted compliant to DEV-VTs.
 *
 * @{
 */

/* virtual terminal emulator */

struct tsm_vte;

/* terminal flags */
#define TSM_VTE_FLAG_CURSOR_KEY_MODE			0x00000001 /* DEC cursor key mode */
#define TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE		0x00000002 /* DEC keypad application mode; TODO: toggle on numlock? */
#define TSM_VTE_FLAG_LINE_FEED_NEW_LINE_MODE		0x00000004 /* DEC line-feed/new-line mode */
#define TSM_VTE_FLAG_8BIT_MODE				0x00000008 /* Disable UTF-8 mode and enable 8bit compatible mode */
#define TSM_VTE_FLAG_7BIT_MODE				0x00000010 /* Disable 8bit mode and use 7bit compatible mode */
#define TSM_VTE_FLAG_USE_C1				0x00000020 /* Explicitly use 8bit C1 codes; TODO: implement */
#define TSM_VTE_FLAG_KEYBOARD_ACTION_MODE		0x00000040 /* Disable keyboard; TODO: implement? */
#define TSM_VTE_FLAG_INSERT_REPLACE_MODE		0x00000080 /* Enable insert mode */
#define TSM_VTE_FLAG_SEND_RECEIVE_MODE			0x00000100 /* Disable local echo */
#define TSM_VTE_FLAG_TEXT_CURSOR_MODE			0x00000200 /* Show cursor */
#define TSM_VTE_FLAG_INVERSE_SCREEN_MODE		0x00000400 /* Inverse colors */
#define TSM_VTE_FLAG_ORIGIN_MODE			0x00000800 /* Relative origin for cursor */
#define TSM_VTE_FLAG_AUTO_WRAP_MODE			0x00001000 /* Auto line wrap mode */
#define TSM_VTE_FLAG_AUTO_REPEAT_MODE			0x00002000 /* Auto repeat key press; TODO: implement */
#define TSM_VTE_FLAG_NATIONAL_CHARSET_MODE		0x00004000 /* Send keys from nation charsets; TODO: implement */
#define TSM_VTE_FLAG_BACKGROUND_COLOR_ERASE_MODE	0x00008000 /* Set background color on erase (bce) */
#define TSM_VTE_FLAG_PREPEND_ESCAPE			0x00010000 /* Prepend escape character to next output */
#define TSM_VTE_FLAG_TITE_INHIBIT_MODE			0x00020000 /* Prevent switching to alternate screen buffer */

/* keep in sync with shl_xkb_mods */
enum tsm_vte_modifier {
	TSM_SHIFT_MASK		= (1 << 0),
	TSM_LOCK_MASK		= (1 << 1),
	TSM_CONTROL_MASK	= (1 << 2),
	TSM_ALT_MASK		= (1 << 3),
	TSM_LOGO_MASK		= (1 << 4),
};

/* keep in sync with TSM_INPUT_INVALID */
#define TSM_VTE_INVALID 0xffffffff

enum tsm_vte_color {
	TSM_COLOR_BLACK,
	TSM_COLOR_RED,
	TSM_COLOR_GREEN,
	TSM_COLOR_YELLOW,
	TSM_COLOR_BLUE,
	TSM_COLOR_MAGENTA,
	TSM_COLOR_CYAN,
	TSM_COLOR_LIGHT_GREY,
	TSM_COLOR_DARK_GREY,
	TSM_COLOR_LIGHT_RED,
	TSM_COLOR_LIGHT_GREEN,
	TSM_COLOR_LIGHT_YELLOW,
	TSM_COLOR_LIGHT_BLUE,
	TSM_COLOR_LIGHT_MAGENTA,
	TSM_COLOR_LIGHT_CYAN,
	TSM_COLOR_WHITE,

	TSM_COLOR_FOREGROUND,
	TSM_COLOR_BACKGROUND,

	TSM_COLOR_NUM
};

/**
 * Mouse Tracking
 *
 * Reference:
 *
 * https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-Mouse-Tracking
 *
 * The application running in the terminal can request a mouse tracking mode
 * and it can configure the event type (send position on click only or on click
 * and on mouse movement). The terminal will then send the position
 * of the mouse cursor as requested by the application.
 *
 * Since libtsm doesn't know anything about the UI or the mouse this can only
 * work if the terminal emulator built on top of libtsm cooperates.
 *
 * To implement mouse tracking a terminal emulator must first set a mouse
 * callback with tsm_vte_set_mouse_cb. This callback will be called whenever the
 * mouse mode changes in a way that is relevant to the terminal. It tells the
 * terminal if it needs to pass mouse events on click, on move, or not at all.
 *
 * To pass the mouse events the terminal needs to call tsm_vte_handle_mouse with
 * all parameters filled appropriately. The function will then take care of
 * sending the mouse events to the application in the correct encoding and
 * it will also discard events that are duplicate or unnecessary.
 */

/* control sequence codes sent be the application */
#define TSM_VTE_MOUSE_MODE_X10      9 /* legacy mode (only cell mode, only on mouse click and x and y can be 223 max) */
#define TSM_VTE_MOUSE_MODE_VT200 1000 /* normal tracking mode (sends mouse position both on button press and release) */
#define TSM_VTE_MOUSE_EVENT_BTN  1002 /* sends position on mouse click only */
#define TSM_VTE_MOUSE_EVENT_ANY  1003 /* sends position on mouse click and mouse move */
#define TSM_VTE_MOUSE_MODE_SGR   1006 /* modern mode that allows unlimited x and y coordinates */
#define TSM_VTE_MOUSE_MODE_PIXEL 1016 /* sends pixel coordinates instead of cell coordinates */
#define TSM_VTE_BRACKETED_PASTE  2004 /* enclose paste data with escape characters */

enum tsm_mouse_track_mode {
	TSM_MOUSE_TRACK_DISABLE = 0, /* don't track mouse events */
	TSM_MOUSE_TRACK_BTN = TSM_VTE_MOUSE_EVENT_BTN, /* call tsm_vte_handle_mouse only for mouse clicks */
	TSM_MOUSE_TRACK_ANY = TSM_VTE_MOUSE_EVENT_ANY  /* call tsm_vte_handle_mouse for mouse clicks and mouse movement */
};

/* mouse buttons to be passed to tsm_vte_handle_mouse */
#define TSM_MOUSE_BUTTON_LEFT       0
#define TSM_MOUSE_BUTTON_MIDDLE     1
#define TSM_MOUSE_BUTTON_RIGHT      2
#define TSM_MOUSE_BUTTON_WHEEL_UP   4
#define TSM_MOUSE_BUTTON_WHEEL_DOWN 5

/* modifier keys to be passed to tsm_vte_handle_mouse (can be combined with OR) */
#define TSM_MOUSE_MODIFIER_SHIFT  4
#define TSM_MOUSE_MODIFIER_META   8
#define TSM_MOUSE_MODIFIER_CTRL  16

/* events to be passed to tsm_vte_handle_mouse */
#define TSM_MOUSE_EVENT_PRESSED  1
#define TSM_MOUSE_EVENT_RELEASED 2
#define TSM_MOUSE_EVENT_MOVED    4

typedef void (*tsm_vte_write_cb) (struct tsm_vte *vte,
				  const char *u8,
				  size_t len,
				  void *data);

typedef void (*tsm_vte_osc_cb) (struct tsm_vte *vte,
				  const char *u8,
				  size_t len,
				  void *data);

typedef void (*tsm_vte_mouse_cb) (struct tsm_vte *vte,
				  enum tsm_mouse_track_mode track_mode,
				  bool track_pixels,
				  void *data);

typedef void (*tsm_vte_bell_cb) (struct tsm_vte *vte,
				 void *data);

enum tsm_vte_led {
	TSM_VTE_LED_SCROLL_LOCK = (1 << 0),
	TSM_VTE_LED_NUM_LOCK    = (1 << 1),
	TSM_VTE_LED_CAPS_LOCK   = (1 << 2),
};

typedef void (*tsm_vte_led_cb) (struct tsm_vte *vte,
				unsigned int leds,
				void *data);

int tsm_vte_new(struct tsm_vte **out, struct tsm_screen *con,
		tsm_vte_write_cb write_cb, void *data,
		tsm_log_t log, void *log_data);
void tsm_vte_ref(struct tsm_vte *vte);
void tsm_vte_unref(struct tsm_vte *vte);

void tsm_vte_set_osc_cb(struct tsm_vte *vte, tsm_vte_osc_cb osc_cb, void *osc_data);
void tsm_vte_set_mouse_cb(struct tsm_vte *vte, tsm_vte_mouse_cb mouse_cb, void *mouse_data);
void tsm_vte_set_bell_cb(struct tsm_vte *vte, tsm_vte_bell_cb bell_cb, void *bell_data);
void tsm_vte_set_led_cb(struct tsm_vte *vte, tsm_vte_led_cb led_cb, void *led_data);

/**
 * @brief Set color palette to one of the predefined palette on the vte object.
 *
 * Current supported palette names are
 *
 * - solarized
 * - solarized-black
 * - solarized-white
 * - soft-black
 * - base16-dark
 * - base16-light
 *
 * In addition, when palette name is "custom", the custom palette set in
 * tsm_vte_set_custom_palette() is used.
 *
 * @sa tsm_vte_set_custom_palette to set custom palette.
 *
 * @param vte The vte object to set on.
 * @param palette_name Name of the color palette. Pass NULL to reset to default.
 *
 * @retval 0 on success.
 * @retval -EINVAL if vte is NULL.
 * @retval -ENOMEM if malloc fails.
 */
int tsm_vte_set_palette(struct tsm_vte *vte, const char *palette_name);

/**
 * @brief Set a custom palette on the vte object.
 *
 * An example:
 *
 * @code
 * static uint8_t color_palette[TSM_COLOR_NUM][3] = {
 * 	[TSM_COLOR_BLACK]         = { 0x00, 0x00, 0x00 },
 * 	[TSM_COLOR_RED]           = { 0xab, 0x46, 0x42 },
 * 	[TSM_COLOR_GREEN]         = { 0xa1, 0xb5, 0x6c },
 * 	[TSM_COLOR_YELLOW]        = { 0xf7, 0xca, 0x88 },
 * 	[TSM_COLOR_BLUE]          = { 0x7c, 0xaf, 0xc2 },
 * 	[TSM_COLOR_MAGENTA]       = { 0xba, 0x8b, 0xaf },
 * 	[TSM_COLOR_CYAN]          = { 0x86, 0xc1, 0xb9 },
 * 	[TSM_COLOR_LIGHT_GREY]    = { 0xaa, 0xaa, 0xaa },
 * 	[TSM_COLOR_DARK_GREY]     = { 0x55, 0x55, 0x55 },
 * 	[TSM_COLOR_LIGHT_RED]     = { 0xab, 0x46, 0x42 },
 * 	[TSM_COLOR_LIGHT_GREEN]   = { 0xa1, 0xb5, 0x6c },
 *	[TSM_COLOR_LIGHT_YELLOW]  = { 0xf7, 0xca, 0x88 },
 * 	[TSM_COLOR_LIGHT_BLUE]    = { 0x7c, 0xaf, 0xc2 },
 * 	[TSM_COLOR_LIGHT_MAGENTA] = { 0xba, 0x8b, 0xaf },
 * 	[TSM_COLOR_LIGHT_CYAN]    = { 0x86, 0xc1, 0xb9 },
 * 	[TSM_COLOR_WHITE]         = { 0xff, 0xff, 0xff },
 *
 * 	[TSM_COLOR_FOREGROUND]    = { 0x18, 0x18, 0x18 },
 * 	[TSM_COLOR_BACKGROUND]    = { 0xd8, 0xd8, 0xd8 },
 * };
 * @endcode
 *
 * The palette array is copied into the vte object.
 *
 * @param vte The vte object to set on
 * @param palette The palette array, which should have shape `uint8_t palette[TSM_COLOR_NUM][3]`. Pass NULL to clear.
 *
 * @retval 0 on success.
 * @retval -EINVAL if vte is NULL.
 * @retval -ENOMEM if malloc fails.
 */
int tsm_vte_set_custom_palette(struct tsm_vte *vte, uint8_t (*palette)[3]);

/*
 * Writes to @rgb the colour of entry @index of the 256-colour table in use:
 * 0-15 from the palette tsm_vte_set_palette chose, 16-231 the colour cube,
 * 232-255 the grey ramp. This is the colour SGR 38;5;@index draws and OSC 4
 * reports. A renderer uses it to resolve a colour its own configuration names
 * by index, so that colour follows the palette.
 * Defined when tsm_vte_get_palette_color exists.
 *
 * @retval 0 on success.
 * @retval -EINVAL if vte or rgb is NULL, or @index is above 255.
 */
#define TSM_VTE_GET_PALETTE_COLOR 1
int tsm_vte_get_palette_color(struct tsm_vte *vte, unsigned int index,
			      uint8_t rgb[3]);

/*
 * The width method the terminal starts with, and returns to on a hard (RIS)
 * or soft (DECSTR) reset. DEC private mode 2027 switches it while the
 * terminal runs: set is TSM_WIDTH_UNICODE, reset TSM_WIDTH_WCWIDTH.
 * The default is TSM_WIDTH_UNICODE.
 */
int tsm_vte_set_width_method(struct tsm_vte *vte, enum tsm_width_method method);

void tsm_vte_get_def_attr(struct tsm_vte *vte, struct tsm_screen_attr *out);
unsigned int tsm_vte_get_flags(struct tsm_vte *vte);

unsigned int tsm_vte_get_mouse_mode(struct tsm_vte *vte);
unsigned int tsm_vte_get_mouse_event(struct tsm_vte *vte);

void tsm_vte_reset(struct tsm_vte *vte);
void tsm_vte_hard_reset(struct tsm_vte *vte);
void tsm_vte_input(struct tsm_vte *vte, const char *u8, size_t len);

/**
 * @brief Set backspace key to send either backspace or delete.
 *
 * Some terminals send ASCII backspace (010, 8, 0x08), some send ASCII delete
 * (0177, 127, 0x7f).
 *
 * The default for vte is to send ASCII backspace.
 *
 * @param vte The vte object to set on
 * @param enable Send ASCII delete if \c true, send ASCII backspace if \c false.
 */
void tsm_vte_set_backspace_sends_delete(struct tsm_vte *vte, bool enable);
bool tsm_vte_handle_keyboard(struct tsm_vte *vte, uint32_t keysym,
			     uint32_t ascii, unsigned int mods,
			     uint32_t unicode);
bool tsm_vte_handle_mouse(struct tsm_vte *vte, unsigned int cell_x,
        unsigned int cell_y, unsigned int pixel_x, unsigned int pixel_y,
        unsigned int button, unsigned int event, unsigned char flags);

void tsm_vte_paste(struct tsm_vte *vte, const char *data);
/** @} */

#ifdef __cplusplus
}
#endif

#endif /* TSM_LIBTSM_H */
