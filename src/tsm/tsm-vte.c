/*
 * libtsm - VT Emulator
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

/*
 * Virtual Terminal Emulator
 * This is the VT implementation. It is written from scratch. It uses the
 * screen state-machine as output and is tightly bound to it. It supports
 * functionality from vt100 up to vt500 series. It doesn't implement an
 * explicitly selected terminal but tries to support the most important commands
 * to be compatible with existing implementations. However, full vt102
 * compatibility is the least that is provided.
 *
 * The main parser in this file controls the parser-state and dispatches the
 * actions to the related handlers. The parser is based on the state-diagram
 * from Paul Williams: http://vt100.net/emu/
 * It is written from scratch, though.
 * This parser is fully compatible up to the vt500 series. It requires UTF-8 and
 * does not support any other input encoding. The G0 and G1 sets are therefore
 * defined as subsets of UTF-8. You may still map G0-G3 into GL, though.
 *
 * However, the CSI/DCS/etc handlers are not designed after a specific VT
 * series. We try to support all vt102 commands but implement several other
 * often used sequences, too. Feel free to add further.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "tsm-kitty.h"
#include "tsm-sixel.h"
#include "shl-llog.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#define LLOG_SUBSYSTEM "tsm-vte"

/* Input parser states */
enum parser_state {
	STATE_NONE,		/* placeholder */
	STATE_GROUND,		/* initial state and ground */
	STATE_ESC,		/* ESC sequence was started */
	STATE_ESC_INT,		/* intermediate escape characters */
	STATE_CSI_ENTRY,	/* starting CSI sequence */
	STATE_CSI_PARAM,	/* CSI parameters */
	STATE_CSI_INT,		/* intermediate CSI characters */
	STATE_CSI_IGNORE,	/* CSI error; ignore this CSI sequence */
	STATE_DCS_ENTRY,	/* starting DCS sequence */
	STATE_DCS_PARAM,	/* DCS parameters */
	STATE_DCS_INT,		/* intermediate DCS characters */
	STATE_DCS_PASS,		/* DCS data passthrough */
	STATE_DCS_IGNORE,	/* DCS error; ignore this DCS sequence */
	STATE_OSC_STRING,	/* parsing OCS sequence */
	STATE_ST_IGNORE,	/* unimplemented seq; ignore until ST */
	STATE_APC_STRING,	/* APC: a kitty graphics command, or ignored */
	STATE_NUM
};

/* Input parser actions */
enum parser_action {
	ACTION_NONE,		/* placeholder */
	ACTION_IGNORE,		/* ignore the character entirely */
	ACTION_PRINT,		/* print the character on the console */
	ACTION_EXECUTE,		/* execute single control character (C0/C1) */
	ACTION_CLEAR,		/* clear current parameter state */
	ACTION_COLLECT,		/* collect intermediate character */
	ACTION_PARAM,		/* collect parameter character */
	ACTION_ESC_DISPATCH,	/* dispatch escape sequence */
	ACTION_CSI_DISPATCH,	/* dispatch csi sequence */
	ACTION_DCS_START,	/* start of DCS data */
	ACTION_DCS_COLLECT,	/* collect DCS data */
	ACTION_DCS_END,		/* end of DCS data */
	ACTION_OSC_START,	/* start of OSC data */
	ACTION_OSC_COLLECT,	/* collect OSC data */
	ACTION_OSC_END,		/* end of OSC data */
	ACTION_APC_START,	/* start of APC data */
	ACTION_APC_COLLECT,	/* collect APC data */
	ACTION_APC_END,		/* end of APC data */
	ACTION_NUM
};

/* CSI flags */
#define CSI_BANG	0x0001		/* CSI: ! */
#define CSI_CASH	0x0002		/* CSI: $ */
#define CSI_WHAT	0x0004		/* CSI: ? */
#define CSI_GT		0x0008		/* CSI: > */
#define CSI_SPACE	0x0010		/* CSI:   */
#define CSI_SQUOTE	0x0020		/* CSI: ' */
#define CSI_DQUOTE	0x0040		/* CSI: " */
#define CSI_MULT	0x0080		/* CSI: * */
#define CSI_PLUS	0x0100		/* CSI: + */
#define CSI_POPEN	0x0200		/* CSI: ( */
#define CSI_PCLOSE	0x0400		/* CSI: ) */
#define CSI_HASH	0x0800		/* CSI: # */

/* max CSI arguments */
#define CSI_ARG_MAX 16

/* max colon-separated sub-parameters of one CSI argument */
#define CSI_SUBARG_MAX 8

/* max length of an OSC code */
#define OSC_MAX_LEN 128

struct vte_saved_state {
	unsigned int cursor_x;
	unsigned int cursor_y;
	struct cell_attr cattr;
	tsm_vte_charset **gl;
	tsm_vte_charset **gr;
	enum tsm_screen_cursor_style cursor_style;
	bool wrap_mode;
	bool origin_mode;
};

struct tsm_vte {
	unsigned long ref;
	tsm_log_t llog;
	void *llog_data;
	struct tsm_screen *con;
	tsm_vte_write_cb write_cb;
	void *data;
	char *palette_name;
	bool backspace_sends_delete;
	/* the width method the terminal starts with and resets to */
	enum tsm_width_method width_method;

	struct tsm_utf8_mach *mach;
	unsigned long parse_cnt;

	unsigned int state;
	unsigned int csi_argc;
	int csi_argv[CSI_ARG_MAX];
	/* Sub-parameters: "4:3" is csi_argv 4 with csi_subargc 1 and
	 * csi_subargv {3}; an argument without a colon has csi_subargc 0. An
	 * empty sub-parameter is -1. Colons past CSI_SUBARG_MAX set
	 * csi_subarg_drop, which discards digits until the next ';'. */
	unsigned int csi_subargc[CSI_ARG_MAX];
	int csi_subargv[CSI_ARG_MAX][CSI_SUBARG_MAX];
	bool csi_subarg_drop;
	unsigned int csi_flags;

	tsm_vte_osc_cb osc_cb;
	void *osc_data;
	unsigned int osc_len;
	char osc_arg[OSC_MAX_LEN];

	tsm_vte_mouse_cb mouse_cb;
	void *mouse_data;
	/* the mouse protocol (9, 1000, 1002, 1003) and encoding (1006,
	 * 1016; 0 for X10), and the cell of the last motion reported, plus
	 * one, 0 for none */
	unsigned int mouse_track;
	unsigned int mouse_encoding;
	unsigned int mouse_last_col;
	unsigned int mouse_last_row;
	bool bracketed_paste;

	tsm_vte_bell_cb bell_cb;
	void *bell_data;

	tsm_vte_led_cb led_cb;
	void *led_data;
	unsigned int led_state;

	uint8_t (*custom_palette_storage)[3];
	uint8_t (*palette)[3];
	struct cell_attr def_attr;
	struct cell_attr cattr;
	unsigned int flags;

	tsm_vte_charset **gl;
	tsm_vte_charset **gr;
	tsm_vte_charset **glt;
	tsm_vte_charset **grt;
	tsm_vte_charset *g0;
	tsm_vte_charset *g1;
	tsm_vte_charset *g2;
	tsm_vte_charset *g3;

	struct vte_saved_state saved_state;
	unsigned int alt_cursor_x;
	unsigned int alt_cursor_y;

	/* sixel (ADR-103): the host's settings */
	bool sixel_enabled;
	unsigned int graphic_max_w;		/* max-graphic-size */
	unsigned int graphic_max_h;
	unsigned int sixel_max_registers;	/* num-color-registers */
	bool sixel_private_default;		/* private-color-registers */
	/* the client's: XTSMGRAPHICS's register count, modes 1070, 80, 8452 */
	unsigned int sixel_registers;
	bool sixel_private;
	bool sixel_no_scroll;
	bool sixel_cursor_right;
	/* made at the first image, and again after a setting changes */
	struct tsm_sixel *sixel;
	struct tsm_sixel_palette *sixel_shared;
	bool sixel_active;			/* a DCS q streams into it */
	/* the kitty graphics protocol (ADR-103 step 4) */
	bool kitty_enabled;
	/* the host blinks the cursor in the styles that blink (mode 12) */
	bool cursor_blink;
	/* mode 2026: the host presents by it, a hold, the holds ended, and
	 * whether this input call returns at an end */
	bool sync_enabled;
	bool sync;
	uint64_t sync_frames;
	bool sync_stop;
	struct tsm_kitty *kitty;
	bool apc_kitty;				/* the APC streams into it */
	/* the image work tsm_vte_input_bounded has left in this call */
	uint64_t work_left;
};

static void sixel_drop(struct tsm_vte *vte);
static void sixel_reset(struct tsm_vte *vte);
static void kitty_write(struct tsm_vte *vte, const char *u8, size_t len);

/*
 * Modes (ADR-201 in aaronsb/kmscon): tsm-modes.def has one row per mode,
 * which these expand by kind and by status.
 */
#define MODE_ONLY_ANSI_ANSI(...) __VA_ARGS__
#define MODE_ONLY_ANSI_DEC(...)
#define MODE_ONLY_DEC_ANSI(...)
#define MODE_ONLY_DEC_DEC(...) __VA_ARGS__
#define MODE_ACCESSORS_SUPPORTED(...) __VA_ARGS__
#define MODE_ACCESSORS_PARTIAL(...) __VA_ARGS__
#define MODE_ACCESSORS_IGNORED(...)
#define MODE_ACCESSORS_REFUSED(...)
#define MODE_SET_SUPPORTED(name) mode_##name##_set(vte, set)
#define MODE_SET_PARTIAL(name) mode_##name##_set(vte, set)
/* an ignored row has no effect, but keeps its legacy flag for
 * tsm_vte_get_flags, upstream's API; MODE_FLAG_<name> is 0 for one without */
#define MODE_SET_IGNORED(name) set_reset_flag(vte, set, MODE_FLAG_##name)
#define MODE_SET_REFUSED(name)
#define MODE_FLAG_KAM TSM_VTE_FLAG_KEYBOARD_ACTION_MODE
#define MODE_FLAG_DECCOLM 0
#define MODE_FLAG_DECSCLM 0
#define MODE_FLAG_DECARM TSM_VTE_FLAG_AUTO_REPEAT_MODE
#define MODE_FLAG_DECPFF 0
#define MODE_FLAG_DECPEX 0
#define MODE_FLAG_DECNRCM TSM_VTE_FLAG_NATIONAL_CHARSET_MODE
/* a default the host configures */
#define CONFIGURED(value) (value)

enum mode_host {
	HOST_NONE,
	HOST_INPUT,
	HOST_PASTE,
	HOST_DRAW,
};

enum mode_reset {
	RESET_NONE,
	RESET_SOFT,	/* DECSTR and RIS */
	RESET_HARD,	/* RIS only */
};

#define MODE(kind, number, name, status, ...)				\
	MODE_ACCESSORS_##status(					\
		static void mode_##name##_set(struct tsm_vte *vte, bool set); \
		static bool mode_##name##_get(struct tsm_vte *vte);)
#include "tsm-modes.def"
#undef MODE

static int mode_report(struct tsm_vte *vte, bool dec, unsigned int number);

#ifdef TSM_MODE_TRACE
/*
 * Test builds only (libtsm_test_dep): with TSM_MODE_TRACE naming a file,
 * each SM and RM of a row is appended to it with the running test's name,
 * for the modes-tests check.
 */
static const char *mode_trace_name;

void tsm_mode_trace_test(const char *name)
{
	mode_trace_name = name;
}

static void mode_trace(bool dec, unsigned int number, bool set)
{
	const char *path = getenv("TSM_MODE_TRACE");
	FILE *f;

	if (!path || !mode_trace_name)
		return;
	f = fopen(path, "a");
	if (!f)
		return;
	fprintf(f, "%s\t%s\t%u\t%s\n", mode_trace_name, dec ? "dec" : "ansi",
		number, set ? "set" : "reset");
	fclose(f);
}
#define MODE_TRACE(dec, number) mode_trace(dec, number, set)

/*
 * Test builds only: struct tsm_vte copied into @buf with the parser's
 * scratch zeroed (the sequence being parsed, which every sequence
 * rewrites), so modes-sweep compares every other field whole, a field
 * added later included. Returns the size copied, or 0 when @len is short.
 */
size_t tsm_vte_test_state(struct tsm_vte *vte, void *buf, size_t len)
{
	struct tsm_vte copy;

	if (len < sizeof(copy))
		return 0;
	memcpy(&copy, vte, sizeof(copy));
	copy.state = 0;
	copy.csi_argc = 0;
	memset(copy.csi_argv, 0, sizeof(copy.csi_argv));
	memset(copy.csi_subargc, 0, sizeof(copy.csi_subargc));
	memset(copy.csi_subargv, 0, sizeof(copy.csi_subargv));
	copy.csi_subarg_drop = false;
	copy.csi_flags = 0;
	copy.osc_len = 0;
	memset(copy.osc_arg, 0, sizeof(copy.osc_arg));
	memcpy(buf, &copy, sizeof(copy));
	return sizeof(copy);
}
#else
#define MODE_TRACE(dec, number)
#endif

static uint8_t color_palette_legacy[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = {   0,   0,   0 }, /* black */
	[TSM_COLOR_RED]           = { 205,   0,   0 }, /* red */
	[TSM_COLOR_GREEN]         = {   0, 205,   0 }, /* green */
	[TSM_COLOR_YELLOW]        = { 205, 205,   0 }, /* yellow */
	[TSM_COLOR_BLUE]          = {   0,   0, 238 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 205,   0, 205 }, /* magenta */
	[TSM_COLOR_CYAN]          = {   0, 205, 205 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 229, 229, 229 }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = { 127, 127, 127 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 255,   0,   0 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = {   0, 255,   0 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 255, 255,   0 }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = {  92,  92, 255 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 255,   0, 255 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = {   0, 255, 255 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 255, 255, 255 }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 229, 229, 229 }, /* light grey */
	[TSM_COLOR_BACKGROUND]    = {   0,   0,   0 }, /* black */
};

// vga palette from https://en.wikipedia.org/wiki/ANSI_escape_code
static uint8_t color_palette_vga[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = {   0,   0,   0 }, /* black */
	[TSM_COLOR_RED]           = { 170,   0,   0 }, /* red */
	[TSM_COLOR_GREEN]         = {   0, 170,   0 }, /* green */
	[TSM_COLOR_YELLOW]        = { 170,  85,   0 }, /* yellow */
	[TSM_COLOR_BLUE]          = {   0,   0, 170 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 170,   0, 170 }, /* magenta */
	[TSM_COLOR_CYAN]          = {   0, 170, 170 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 170, 170, 170 }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = {  85,  85,  85 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 255,  85,  85 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = {  85, 255,  85 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 255, 255,  85 }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = {  85,  85, 255 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 255,  85, 255 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = {  85, 255, 255 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 255, 255, 255 }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 170, 170, 170 }, /* light grey */
	[TSM_COLOR_BACKGROUND]    = {   0,   0,   0 }, /* black */
};

static uint8_t color_palette_nord[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = {  59,  66,  82 }, /* black */
	[TSM_COLOR_RED]           = { 191,  97, 106 }, /* red */
	[TSM_COLOR_GREEN]         = { 163, 190, 140 }, /* green */
	[TSM_COLOR_YELLOW]        = { 235, 203, 139 }, /* yellow */
	[TSM_COLOR_BLUE]          = { 129, 161, 193 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 180, 142, 173 }, /* magenta */
	[TSM_COLOR_CYAN]          = { 136, 192, 208 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 229, 233, 240 }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = {  76,  86, 106 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 191,  97, 106 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = { 163, 190, 140 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 235, 203, 139 }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = { 129, 161, 193 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 180, 142, 173 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = { 143, 188, 187 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 236, 239, 244 }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 216, 222, 233 }, /* light grey */
	[TSM_COLOR_BACKGROUND]    = {  46,  52,  64 }, /* black */
};

static uint8_t color_palette_solarized[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = {   7,  54,  66 }, /* black */
	[TSM_COLOR_RED]           = { 220,  50,  47 }, /* red */
	[TSM_COLOR_GREEN]         = { 133, 153,   0 }, /* green */
	[TSM_COLOR_YELLOW]        = { 181, 137,   0 }, /* yellow */
	[TSM_COLOR_BLUE]          = {  38, 139, 210 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 211,  54, 130 }, /* magenta */
	[TSM_COLOR_CYAN]          = {  42, 161, 152 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 238, 232, 213 }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = {   0,  43,  54 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 203,  75,  22 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = {  88, 110, 117 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 101, 123, 131 }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = { 131, 148, 150 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 108, 113, 196 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = { 147, 161, 161 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 253, 246, 227 }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 238, 232, 213 }, /* light grey */
	[TSM_COLOR_BACKGROUND]    = {   7,  54,  66 }, /* black */
};

static uint8_t color_palette_solarized_black[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = {   0,   0,   0 }, /* black */
	[TSM_COLOR_RED]           = { 220,  50,  47 }, /* red */
	[TSM_COLOR_GREEN]         = { 133, 153,   0 }, /* green */
	[TSM_COLOR_YELLOW]        = { 181, 137,   0 }, /* yellow */
	[TSM_COLOR_BLUE]          = {  38, 139, 210 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 211,  54, 130 }, /* magenta */
	[TSM_COLOR_CYAN]          = {  42, 161, 152 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 238, 232, 213 }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = {   0,  43,  54 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 203,  75,  22 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = {  88, 110, 117 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 101, 123, 131 }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = { 131, 148, 150 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 108, 113, 196 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = { 147, 161, 161 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 253, 246, 227 }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 238, 232, 213 }, /* light grey */
	[TSM_COLOR_BACKGROUND]    = {   0,   0,   0 }, /* black */
};

static uint8_t color_palette_solarized_white[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = {   7,  54,  66 }, /* black */
	[TSM_COLOR_RED]           = { 220,  50,  47 }, /* red */
	[TSM_COLOR_GREEN]         = { 133, 153,   0 }, /* green */
	[TSM_COLOR_YELLOW]        = { 181, 137,   0 }, /* yellow */
	[TSM_COLOR_BLUE]          = {  38, 139, 210 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 211,  54, 130 }, /* magenta */
	[TSM_COLOR_CYAN]          = {  42, 161, 152 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 238, 232, 213 }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = {   0,  43,  54 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 203,  75,  22 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = {  88, 110, 117 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 101, 123, 131 }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = { 131, 148, 150 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 108, 113, 196 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = { 147, 161, 161 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 253, 246, 227 }, /* white */

	[TSM_COLOR_FOREGROUND]    = {   7,  54,  66 }, /* black */
	[TSM_COLOR_BACKGROUND]    = { 238, 232, 213 }, /* light grey */
};

static uint8_t color_palette_soft_black[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = { 0x3f, 0x3f, 0x3f }, /* black */
	[TSM_COLOR_RED]           = { 0x70, 0x50, 0x50 }, /* red */
	[TSM_COLOR_GREEN]         = { 0x60, 0xb4, 0x8a }, /* green */
	[TSM_COLOR_YELLOW]        = { 0xdf, 0xaf, 0x8f }, /* yellow */
	[TSM_COLOR_BLUE]          = { 0x9a, 0xb8, 0xd7 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 0xdc, 0x8c, 0xc3 }, /* magenta */
	[TSM_COLOR_CYAN]          = { 0x8c, 0xd0, 0xd3 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 0xff, 0xff, 0xff }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = { 0x70, 0x90, 0x80 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 0xdc, 0xa3, 0xa3 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = { 0x72, 0xd5, 0xa3 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 0xf0, 0xdf, 0xaf }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = { 0x94, 0xbf, 0xf3 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 0xec, 0x93, 0xd3 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = { 0x93, 0xe0, 0xe3 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 0xdc, 0xdc, 0xcc }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 0xdc, 0xdc, 0xcc }, /* white */
	[TSM_COLOR_BACKGROUND]    = { 0x2c, 0x2c, 0x2c }, /* light grey */
};

static uint8_t color_palette_base16_dark[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = { 0x00, 0x00, 0x00 }, /* black */
	[TSM_COLOR_RED]           = { 0xab, 0x46, 0x42 }, /* red, base08 */
	[TSM_COLOR_GREEN]         = { 0xa1, 0xb5, 0x6c }, /* green, base0B */
	[TSM_COLOR_YELLOW]        = { 0xf7, 0xca, 0x88 }, /* yellow, base0A */
	[TSM_COLOR_BLUE]          = { 0x7c, 0xaf, 0xc2 }, /* blue, base0D */
	[TSM_COLOR_MAGENTA]       = { 0xba, 0x8b, 0xaf }, /* magenta, base0E */
	[TSM_COLOR_CYAN]          = { 0x86, 0xc1, 0xb9 }, /* cyan, base0C */
	[TSM_COLOR_LIGHT_GREY]    = { 0xaa, 0xaa, 0xaa }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = { 0x55, 0x55, 0x55 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 0xab, 0x46, 0x42 }, /* red, base08 */
	[TSM_COLOR_LIGHT_GREEN]   = { 0xa1, 0xb5, 0x6c }, /* green, base0B */
	[TSM_COLOR_LIGHT_YELLOW]  = { 0xf7, 0xca, 0x88 }, /* yellow, base0A */
	[TSM_COLOR_LIGHT_BLUE]    = { 0x7c, 0xaf, 0xc2 }, /* blue, base0D */
	[TSM_COLOR_LIGHT_MAGENTA] = { 0xba, 0x8b, 0xaf }, /* magenta, base0E */
	[TSM_COLOR_LIGHT_CYAN]    = { 0x86, 0xc1, 0xb9 }, /* cyan, base0C */
	[TSM_COLOR_WHITE]         = { 0xff, 0xff, 0xff }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 0xd8, 0xd8, 0xd8 }, /* light grey */
	[TSM_COLOR_BACKGROUND]    = { 0x18, 0x18, 0x18 }, /* dark grey */
};

static uint8_t color_palette_base16_light[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = { 0x00, 0x00, 0x00 }, /* black */
	[TSM_COLOR_RED]           = { 0xab, 0x46, 0x42 }, /* red, base08 */
	[TSM_COLOR_GREEN]         = { 0xa1, 0xb5, 0x6c }, /* green, base0B */
	[TSM_COLOR_YELLOW]        = { 0xf7, 0xca, 0x88 }, /* yellow, base0A */
	[TSM_COLOR_BLUE]          = { 0x7c, 0xaf, 0xc2 }, /* blue, base0D */
	[TSM_COLOR_MAGENTA]       = { 0xba, 0x8b, 0xaf }, /* magenta, base0E */
	[TSM_COLOR_CYAN]          = { 0x86, 0xc1, 0xb9 }, /* cyan, base0C */
	[TSM_COLOR_LIGHT_GREY]    = { 0xaa, 0xaa, 0xaa }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = { 0x55, 0x55, 0x55 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 0xab, 0x46, 0x42 }, /* red, base08 */
	[TSM_COLOR_LIGHT_GREEN]   = { 0xa1, 0xb5, 0x6c }, /* green, base0B */
	[TSM_COLOR_LIGHT_YELLOW]  = { 0xf7, 0xca, 0x88 }, /* yellow, base0A */
	[TSM_COLOR_LIGHT_BLUE]    = { 0x7c, 0xaf, 0xc2 }, /* blue, base0D */
	[TSM_COLOR_LIGHT_MAGENTA] = { 0xba, 0x8b, 0xaf }, /* magenta, base0E */
	[TSM_COLOR_LIGHT_CYAN]    = { 0x86, 0xc1, 0xb9 }, /* cyan, base0C */
	[TSM_COLOR_WHITE]         = { 0xff, 0xff, 0xff }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 0x18, 0x18, 0x18 }, /* dark grey */
	[TSM_COLOR_BACKGROUND]    = { 0xd8, 0xd8, 0xd8 }, /* light grey */
};

static uint8_t (*get_palette(struct tsm_vte *vte))[3]
{
	if (!vte->palette_name)
		return color_palette_legacy;

	if (!strcmp(vte->palette_name, "custom") && vte->custom_palette_storage)
		return vte->custom_palette_storage;
	if (!strcmp(vte->palette_name, "nord"))
		return color_palette_nord;
	if (!strcmp(vte->palette_name, "solarized"))
		return color_palette_solarized;
	if (!strcmp(vte->palette_name, "solarized-black"))
		return color_palette_solarized_black;
	if (!strcmp(vte->palette_name, "solarized-white"))
		return color_palette_solarized_white;
	if (!strcmp(vte->palette_name, "soft-black"))
		return color_palette_soft_black;
	if (!strcmp(vte->palette_name, "base16-dark"))
		return color_palette_base16_dark;
	if (!strcmp(vte->palette_name, "base16-light"))
		return color_palette_base16_light;
	if (!strcmp(vte->palette_name, "vga"))
		return color_palette_vga;
	if (!strcmp(vte->palette_name, "legacy"))
		return color_palette_legacy;

	return color_palette_legacy;
}

/* the palette's default foreground and background, as cells keep them */
static void set_default_colors(struct cell_attr *attr)
{
	attr->fg.code = TSM_COLOR_FOREGROUND;
	attr->fg_kind = CELL_COLOR_CODE;
	attr->bg.code = TSM_COLOR_BACKGROUND;
	attr->bg_kind = CELL_COLOR_CODE;
}

/*
 * Background colour erase: a cell erased, scrolled in or added takes the pen's
 * background colour and nothing else of it, the default foreground and no
 * rendition, underline, strike or protection. SGR 7 is not applied: the cell
 * takes the background as set, unswapped. As Ghostty's Screen.blankCell and
 * Style.bgCell (src/terminal/Screen.zig, style.zig) and foot's
 * erase_cell_range (terminal.c) do; both are MIT-licensed.
 */
static void set_erase_attr(struct tsm_vte *vte)
{
	struct cell_attr attr = vte->def_attr;

	attr.bg = vte->cattr.bg;
	attr.bg_kind = vte->cattr.bg_kind;
	screen_set_def_cell_attr(vte->con, &attr);
}

static void set_underline(struct cell_attr *attr,
			  enum tsm_underline_style style)
{
	attr->underline_style = style;
	attr->underline = style != TSM_UNDERLINE_NONE;
}

/* SGR 59: the underline takes the foreground colour again */
static void clear_ucolor(struct cell_attr *attr)
{
	memset(&attr->ul, 0, sizeof(attr->ul));
	attr->ul_kind = CELL_COLOR_CODE;
	attr->ucset = 0;
}

/* SGR 0: everything SGR sets returns to its default */
static void reset_sgr(struct tsm_vte *vte, struct cell_attr *attr)
{
	attr->fg = vte->def_attr.fg;
	attr->fg_kind = vte->def_attr.fg_kind;
	attr->bg = vte->def_attr.bg;
	attr->bg_kind = vte->def_attr.bg_kind;
	attr->bold = 0;
	attr->italic = 0;
	attr->inverse = 0;
	attr->blink = 0;
	attr->dim = 0;
	attr->strike = 0;
	set_underline(attr, TSM_UNDERLINE_NONE);
	clear_ucolor(attr);
}

SHL_EXPORT
int tsm_vte_new(struct tsm_vte **out, struct tsm_screen *con,
		tsm_vte_write_cb write_cb, void *data,
		tsm_log_t log, void *log_data)
{
	struct tsm_vte *vte;
	int ret;

	if (!out || !con || !write_cb)
		return -EINVAL;

	vte = malloc(sizeof(*vte));
	if (!vte)
		return -ENOMEM;

	memset(vte, 0, sizeof(*vte));
	vte->ref = 1;
	vte->llog = log;
	vte->llog_data = log_data;
	vte->con = con;
	vte->write_cb = write_cb;
	vte->data = data;
	vte->backspace_sends_delete = false;
	vte->osc_cb = NULL;
	vte->osc_data = NULL;
	vte->mouse_cb = NULL;
	vte->mouse_data = NULL;
	vte->custom_palette_storage = NULL;
	vte->palette = get_palette(vte);
	screen_set_palette(vte->con, vte->palette);
	set_default_colors(&vte->def_attr);

	vte->graphic_max_w = TSM_SIXEL_MAX_SIZE_DEFAULT;
	vte->graphic_max_h = TSM_SIXEL_MAX_SIZE_DEFAULT;
	vte->sixel_max_registers = TSM_SIXEL_REGISTERS_DEFAULT;
	vte->sixel_private_default = true;
	vte->cursor_blink = true;
	sixel_reset(vte);

	ret = tsm_utf8_mach_new(&vte->mach);
	if (ret)
		goto err_free;
	ret = tsm_kitty_new(&vte->kitty, vte, con, kitty_write);
	if (ret)
		goto err_mach;
	tsm_kitty_set_max_size(vte->kitty, vte->graphic_max_w, vte->graphic_max_h);

	tsm_vte_reset(vte);
	tsm_screen_erase_screen(vte->con, false);

	llog_debug(vte, "new vte object");
	tsm_screen_ref(vte->con);
	*out = vte;
	return 0;

err_mach:
	tsm_utf8_mach_free(vte->mach);
err_free:
	free(vte);
	return ret;
}

SHL_EXPORT
void tsm_vte_ref(struct tsm_vte *vte)
{
	if (!vte)
		return;

	vte->ref++;
}

SHL_EXPORT
void tsm_vte_unref(struct tsm_vte *vte)
{
	if (!vte || !vte->ref)
		return;

	if (--vte->ref)
		return;

	llog_debug(vte, "destroying vte object");
	free(vte->palette_name);
	/* a picture in progress gives its bytes back to the screen first */
	sixel_drop(vte);
	tsm_kitty_free(vte->kitty);
	tsm_screen_unref(vte->con);
	tsm_utf8_mach_free(vte->mach);
	free(vte->custom_palette_storage);
	free(vte);
}

SHL_EXPORT
void tsm_vte_set_osc_cb(struct tsm_vte *vte, tsm_vte_osc_cb osc_cb, void *osc_data)
{
	if (!vte)
		return;

	vte->osc_cb = osc_cb;
	vte->osc_data = osc_data;
}

SHL_EXPORT
void tsm_vte_set_mouse_cb(struct tsm_vte *vte, tsm_vte_mouse_cb mouse_cb, void *mouse_data)
{
	if (!vte)
		return;

	vte->mouse_cb = mouse_cb;
	vte->mouse_data = mouse_data;
}

SHL_EXPORT
void tsm_vte_set_bell_cb(struct tsm_vte *vte, tsm_vte_bell_cb bell_cb, void *bell_data)
{
	if (!vte)
		return;

	vte->bell_cb = bell_cb;
	vte->bell_data = bell_data;
}

SHL_EXPORT
void tsm_vte_set_led_cb(struct tsm_vte *vte, tsm_vte_led_cb led_cb, void *led_data)
{
	if (!vte)
		return;

	vte->led_cb = led_cb;
	vte->led_data = led_data;
}

static int vte_update_palette(struct tsm_vte *vte)
{
	vte->palette = get_palette(vte);
	screen_set_palette(vte->con, vte->palette);
	set_default_colors(&vte->def_attr);
	vte->cattr = vte->def_attr;

	screen_set_def_cell_attr(vte->con, &vte->def_attr);
	tsm_screen_erase_screen(vte->con, false);

	return 0;
}

SHL_EXPORT
int tsm_vte_set_palette(struct tsm_vte *vte, const char *palette_name)
{
	char *tmp = NULL;

	if (!vte)
		return -EINVAL;

	if (palette_name) {
		tmp = strdup(palette_name);
		if (!tmp)
			return -ENOMEM;
	}

	free(vte->palette_name);
	vte->palette_name = tmp;

	return vte_update_palette(vte);
}

SHL_EXPORT
int tsm_vte_set_custom_palette(struct tsm_vte *vte, uint8_t (*palette)[3])
{
	const size_t palette_byte_size = sizeof(color_palette_legacy);
	uint8_t (*tmp)[3] = NULL;

	if (!vte)
		return -EINVAL;

	if (palette) {
		tmp = malloc(palette_byte_size);
		if (!tmp)
			return -ENOMEM;
		memcpy(tmp, palette, palette_byte_size);
	}

	free(vte->custom_palette_storage);
	vte->custom_palette_storage = tmp;

	return vte_update_palette(vte);
}

SHL_EXPORT
int tsm_vte_get_palette_color(struct tsm_vte *vte, unsigned int index,
			      uint8_t rgb[3])
{
	if (!vte || !rgb || index > 255)
		return -EINVAL;

	tsm_color_table_rgb(vte->palette, index, &rgb[0], &rgb[1], &rgb[2]);
	return 0;
}

SHL_EXPORT
void tsm_vte_get_def_attr(struct tsm_vte *vte, struct tsm_screen_attr *out)
{
	if (!vte || !out)
		return;

	cell_attr_resolve(vte->palette, &vte->def_attr, out);
}

SHL_EXPORT
unsigned int tsm_vte_get_flags(struct tsm_vte *vte)
{
	if (!vte) {
		return 0;
	}

	return vte->flags;
}

SHL_EXPORT
int tsm_vte_get_mode(struct tsm_vte *vte, bool dec, unsigned int number)
{
	if (!vte)
		return -EINVAL;

	return mode_report(vte, dec, number);
}

SHL_EXPORT
unsigned int tsm_vte_get_mouse_mode(struct tsm_vte *vte)
{
	if (!vte || !vte->mouse_track)
		return 0;
	if (vte->mouse_encoding)
		return vte->mouse_encoding;
	if (vte->mouse_track == TSM_VTE_MOUSE_MODE_X10)
		return TSM_VTE_MOUSE_MODE_X10;
	return TSM_VTE_MOUSE_MODE_VT200;
}

SHL_EXPORT
unsigned int tsm_vte_get_mouse_event(struct tsm_vte *vte)
{
	if (!vte || !vte->mouse_track)
		return 0;
	if (vte->mouse_track == TSM_VTE_MOUSE_EVENT_ANY)
		return TSM_VTE_MOUSE_EVENT_ANY;
	return TSM_VTE_MOUSE_EVENT_BTN;
}

/*
 * Write raw byte-stream to pty.
 * When writing data to the client we must make sure that we send the correct
 * encoding. For backwards-compatibility reasons we should always send 7bit
 * characters exclusively. However, when TSM_VTE_FLAG_7BIT_MODE is not set, then we can
 * also send raw 8bit characters. For instance, in TSM_VTE_FLAG_8BIT_MODE we can use the
 * GR characters as keyboard input and send them directly or even use the C1
 * escape characters. In unicode mode (default) we can send multi-byte utf-8
 * characters which are also 8bit. When sending these characters, set the \raw
 * flag to true so this function does not perform debug checks on data we send.
 * If debugging is disabled, these checks are also disabled and won't affect
 * performance.
 * For better debugging, we also use the __LINE__ and __FILE__ macros. Use the
 * vte_write() and vte_write_raw() macros below for more convenient use.
 *
 * As a rule of thumb do never send 8bit characters in escape sequences and also
 * avoid all 8bit escape codes including the C1 codes. This will guarantee that
 * all kind of clients are always compatible to us.
 *
 * If SEND_RECEIVE_MODE is off (that is, local echo is on) we have to send all
 * data directly to ourself again. However, we must avoid recursion when
 * tsm_vte_input() itself calls vte_write*(), therefore, we increase the
 * PARSER counter when entering tsm_vte_input() and reset it when leaving it
 * so we never echo data that origins from tsm_vte_input().
 * But note that SEND_RECEIVE_MODE is inherently broken for escape sequences
 * that request answers. That is, if we send a request to the client that awaits
 * a response and parse that request via local echo ourself, then we will also
 * send a response to the client even though he didn't request one. This
 * recursion fix does not avoid this but only prevents us from endless loops
 * here. Anyway, only few applications rely on local echo so we can safely
 * ignore this.
 */
static void vte_write_debug(struct tsm_vte *vte, const char *u8, size_t len,
			    bool raw, const char *file, int line)
{
#ifdef BUILD_ENABLE_DEBUG
	/* in debug mode we check that escape sequences are always <0x7f so they
	 * are correctly parsed by non-unicode and non-8bit-mode clients. */
	size_t i;

	if (!raw) {
		for (i = 0; i < len; ++i) {
			if (u8[i] & 0x80)
				llog_warning(vte, "sending 8bit character inline to client in %s:%d",
					     file, line);
		}
	}
#endif

	/* in local echo mode, directly parse the data again */
	if (!vte->parse_cnt && !(vte->flags & TSM_VTE_FLAG_SEND_RECEIVE_MODE)) {
		if (vte->flags & TSM_VTE_FLAG_PREPEND_ESCAPE)
			tsm_vte_input(vte, "\e", 1);
		tsm_vte_input(vte, u8, len);
	}

	if (vte->flags & TSM_VTE_FLAG_PREPEND_ESCAPE)
		vte->write_cb(vte, "\e", 1, vte->data);
	vte->write_cb(vte, u8, len, vte->data);

	vte->flags &= ~TSM_VTE_FLAG_PREPEND_ESCAPE;
}

#define vte_write(_vte, _u8, _len) \
	vte_write_debug((_vte), (_u8), (_len), false, __FILE__, __LINE__)
#define vte_write_raw(_vte, _u8, _len) \
	vte_write_debug((_vte), (_u8), (_len), true, __FILE__, __LINE__)

/* write to console */
static void write_console(struct tsm_vte *vte, tsm_symbol_t sym)
{
	screen_write_char(vte->con, sym, &vte->cattr);
}

static void reset_state(struct tsm_vte *vte)
{
	vte->saved_state.cursor_x = 0;
	vte->saved_state.cursor_y = 0;
	vte->saved_state.origin_mode = false;
	vte->saved_state.wrap_mode = true;
	vte->saved_state.gl = &vte->g0;
	vte->saved_state.gr = &vte->g1;
	vte->saved_state.cursor_style = TSM_SCREEN_CURSOR_DEFAULT;

	reset_sgr(vte, &vte->saved_state.cattr);
	vte->saved_state.cattr.protect = 0;
}

static void save_state(struct tsm_vte *vte)
{
	vte->saved_state.cursor_x = tsm_screen_get_cursor_x(vte->con);
	vte->saved_state.cursor_y = tsm_screen_get_cursor_y(vte->con);
	vte->saved_state.cattr = vte->cattr;
	vte->saved_state.gl = vte->gl;
	vte->saved_state.gr = vte->gr;
	vte->saved_state.wrap_mode = vte->flags & TSM_VTE_FLAG_AUTO_WRAP_MODE;
	vte->saved_state.origin_mode = vte->flags & TSM_VTE_FLAG_ORIGIN_MODE;
	vte->saved_state.cursor_style = tsm_screen_get_cursor_style(vte->con);
}

static void restore_state(struct tsm_vte *vte)
{
	tsm_screen_move_to(vte->con, vte->saved_state.cursor_x,
			       vte->saved_state.cursor_y);
	vte->cattr = vte->saved_state.cattr;
	if (vte->flags & TSM_VTE_FLAG_BACKGROUND_COLOR_ERASE_MODE)
		set_erase_attr(vte);
	vte->gl = vte->saved_state.gl;
	vte->gr = vte->saved_state.gr;
	tsm_screen_set_cursor_style(vte->con, vte->saved_state.cursor_style);

	if (vte->saved_state.wrap_mode) {
		vte->flags |= TSM_VTE_FLAG_AUTO_WRAP_MODE;
		tsm_screen_set_flags(vte->con, TSM_SCREEN_AUTO_WRAP);
	} else {
		vte->flags &= ~TSM_VTE_FLAG_AUTO_WRAP_MODE;
		tsm_screen_reset_flags(vte->con, TSM_SCREEN_AUTO_WRAP);
	}

	if (vte->saved_state.origin_mode) {
		vte->flags |= TSM_VTE_FLAG_ORIGIN_MODE;
		tsm_screen_set_flags(vte->con, TSM_SCREEN_REL_ORIGIN);
	} else {
		vte->flags &= ~TSM_VTE_FLAG_ORIGIN_MODE;
		tsm_screen_reset_flags(vte->con, TSM_SCREEN_REL_ORIGIN);
	}
}

/*
 * Restores each row @reset covers to its default, through its setter, where
 * the row's state differs from it: state the caller has already cleared stays
 * as it is, without the setter's side effects.
 */
static void modes_reset(struct tsm_vte *vte, enum mode_reset reset)
{
#define MODE(kind, number, name, status, dflt, row_reset, ...)		\
	MODE_ACCESSORS_##status(					\
		if (row_reset == reset && mode_##name##_get(vte) != (dflt)) \
			mode_##name##_set(vte, dflt);)
#include "tsm-modes.def"
#undef MODE
}

/*
 * Reset VTE state
 * This performs a soft reset of the VTE. That is, everything is reset to the
 * same state as when the VTE was created. This does not affect the console,
 * though.
 */
SHL_EXPORT
void tsm_vte_reset(struct tsm_vte *vte)
{
	if (!vte)
		return;

	/* the modes' flags start reset, and modes_reset sets those whose
	 * default is set; DECARM is an ignored row with no setter, so its
	 * legacy flag is set here */
	vte->flags = TSM_VTE_FLAG_AUTO_REPEAT_MODE |
		     TSM_VTE_FLAG_BACKGROUND_COLOR_ERASE_MODE;
	tsm_screen_reset(vte->con);

	tsm_utf8_mach_reset(vte->mach);
	/* leaving a DCS without its end discards the picture, and a kitty
	 * upload in progress goes */
	if (vte->sixel_active) {
		tsm_sixel_abort(vte->sixel);
		vte->sixel_active = false;
	}
	tsm_kitty_cancel(vte->kitty);
	vte->apc_kitty = false;
	vte->state = STATE_GROUND;
	vte->gl = &vte->g0;
	vte->gr = &vte->g1;
	vte->glt = NULL;
	vte->grt = NULL;
	vte->g0 = &tsm_vte_unicode_lower;
	vte->g1 = &tsm_vte_unicode_upper;
	vte->g2 = &tsm_vte_unicode_lower;
	vte->g3 = &tsm_vte_unicode_upper;

	/* both mouse sets, cleared here without telling the host, as before
	 * the mode table */
	vte->mouse_track = 0;
	vte->mouse_encoding = 0;
	vte->mouse_last_col = 0;
	vte->mouse_last_row = 0;

	vte->cattr = vte->def_attr;
	screen_set_def_cell_attr(vte->con, &vte->def_attr);
	/* RIS and DECSTR also return mode 2027 to the configured method,
	 * where the draft resets it to off (a deviation ADR-102 records) */
	modes_reset(vte, RESET_SOFT);

	reset_state(vte);
}

SHL_EXPORT
void tsm_vte_hard_reset(struct tsm_vte *vte)
{
	if (!vte)
		return;

	tsm_vte_reset(vte);
	tsm_screen_erase_screen(vte->con, false);
	tsm_screen_clear_sb(vte->con);
	/* both image stores are emptied, and the shared sixel registers
	 * (ADR-103) */
	screen_images_reset(vte->con);
	sixel_reset(vte);
	tsm_screen_move_to(vte->con, 0, 0);
}

/*
 * Sixel (ADR-103 in aaronsb/kmscon). The DCS payload streams into the
 * decoder (tsm-sixel.c), which reserves its memory from the screen's image
 * budget; at ST the picture moves into the screen's store with that
 * reservation and is placed at the cursor.
 */

/* the image work one tsm_vte_input_bounded call may do, in pixels */
#define VTE_INPUT_WORK ((uint64_t)1 << 24)

/* the host's switch, and a budget to pay for the pictures */
static bool sixel_on(struct tsm_vte *vte)
{
	return vte->sixel_enabled && (vte->con->img.reserve || vte->con->img.limit);
}

static bool sixel_reserve(void *data, size_t old_size, size_t new_size)
{
	struct tsm_vte *vte = data;

	if (new_size > old_size)
		return !screen_images_reserve(vte->con, new_size - old_size);
	screen_images_unreserve(vte->con, old_size - new_size);
	return true;
}

/*
 * Frees the decoder, with any picture in progress, and the shared registers;
 * the next picture makes them again from the settings in force then.
 */
static void sixel_drop(struct tsm_vte *vte)
{
	tsm_sixel_free(vte->sixel);
	vte->sixel = NULL;
	tsm_sixel_palette_free(vte->sixel_shared);
	vte->sixel_shared = NULL;
	vte->sixel_active = false;
}

/* RIS: what the client set returns to what the host set */
static void sixel_reset(struct tsm_vte *vte)
{
	sixel_drop(vte);
	vte->sixel_registers = vte->sixel_max_registers;
	/* modes 80, 1070 and 8452, the rows only RIS resets */
	modes_reset(vte, RESET_HARD);
}

static bool sixel_ready(struct tsm_vte *vte)
{
	struct tsm_sixel_limits lim = {
		.max_width = vte->graphic_max_w,
		.max_height = vte->graphic_max_h,
		.registers = vte->sixel_registers,
		.reserve = sixel_reserve,
		.reserve_data = vte,
	};

	if (!vte->sixel && tsm_sixel_new(&vte->sixel, &lim))
		return false;
	if (!vte->sixel_private && !vte->sixel_shared &&
	    tsm_sixel_palette_new(&vte->sixel_shared, vte->sixel_registers))
		return false;
	return true;
}

/*
 * The image work done so far: the sixel decoder's, the kitty parser's, and
 * the eviction scans' the screen's budget and store limits cost
 */
static uint64_t sixel_work(struct tsm_vte *vte)
{
	return tsm_sixel_work(vte->sixel) + tsm_kitty_work(vte->kitty) +
	       vte->con->img.work;
}

/* counts @work against what this input call may still do */
static void sixel_charge(struct tsm_vte *vte, uint64_t work)
{
	vte->work_left = work < vte->work_left ? vte->work_left - work : 0;
}

/* DCS P1;P2;P3 q: sixel. Every other DCS stays ignored. */
static void do_dcs_start(struct tsm_vte *vte, uint32_t data)
{
	if (data != 'q' || vte->csi_flags || !sixel_on(vte) || !sixel_ready(vte))
		return;

	tsm_sixel_begin(vte->sixel, vte->csi_argv[0], vte->csi_argv[1],
			vte->sixel_private ? NULL : vte->sixel_shared);
	vte->sixel_active = true;
}

static void do_dcs_collect(struct tsm_vte *vte, uint32_t data)
{
	uint64_t work;
	char c = data;

	/* the payload is ASCII; anything else is ignored, as the decoder
	 * ignores bytes it has no use for */
	if (!vte->sixel_active || data >= 0x80)
		return;

	work = sixel_work(vte);
	tsm_sixel_feed(vte->sixel, &c, 1);
	sixel_charge(vte, sixel_work(vte) - work);
}

/* the end of the string: @data is what ended it */
static void do_dcs_end(struct tsm_vte *vte, uint32_t data)
{
	struct tsm_sixel_image img;
	struct tsm_image *image;
	uint64_t work;
	int ret;

	if (!vte->sixel_active)
		return;
	vte->sixel_active = false;

	/* CAN and SUB discard the picture; ST, or anything else that ends
	 * the string, shows it, unless sixel went off while it streamed (its
	 * budget removed, say). The budget would refuse that picture too, but
	 * only after the store made room for it, which can evict an image */
	if (data == 0x18 || data == 0x1a || !sixel_on(vte)) {
		tsm_sixel_abort(vte->sixel);
		return;
	}

	work = sixel_work(vte);
	ret = tsm_sixel_finish(vte->sixel, &img);
	if (!ret)
		ret = screen_sixel_new(vte->con, img.width, img.height, img.pixels,
				       !img.transparent, &image);
	/* a transparent picture is read once more for its opacity */
	sixel_charge(vte, sixel_work(vte) - work +
		     (!ret && img.transparent ? (uint64_t)img.width * img.height : 0));
	if (ret) {
		llog_debug(vte, "sixel picture not stored: %d", ret);
		return;
	}

	ret = screen_sixel_place(vte->con, image, !vte->sixel_no_scroll,
				 vte->sixel_cursor_right);
	if (ret) {
		llog_debug(vte, "sixel picture %ux%u not placed: %d", img.width,
			   img.height, ret);
		screen_image_drop(vte->con, image);
	}
}

/*
 * The kitty graphics protocol (ADR-103 in aaronsb/kmscon, step 4): an APC
 * streams into the kitty parser (tsm-kitty.c) while kitty graphics are on and
 * the screen has an image budget; otherwise it is parsed only to find where
 * it ends, and gets no reply.
 */
static bool kitty_on(struct tsm_vte *vte)
{
	return vte->kitty_enabled && (vte->con->img.reserve || vte->con->img.limit);
}

static void kitty_write(struct tsm_vte *vte, const char *u8, size_t len)
{
	vte_write(vte, u8, len);
}

static void do_apc_start(struct tsm_vte *vte)
{
	vte->apc_kitty = kitty_on(vte);
	if (vte->apc_kitty)
		tsm_kitty_begin(vte->kitty);
	else
		tsm_kitty_cancel(vte->kitty);
}

static void do_apc_collect(struct tsm_vte *vte, uint32_t data)
{
	uint64_t work;

	if (!vte->apc_kitty)
		return;
	work = sixel_work(vte);
	tsm_kitty_feed(vte->kitty, data);
	sixel_charge(vte, sixel_work(vte) - work);
}

/* the end of the string: @data is what ended it */
static void do_apc_end(struct tsm_vte *vte, uint32_t data)
{
	uint64_t work;

	if (!vte->apc_kitty)
		return;
	vte->apc_kitty = false;
	/* kitty graphics went off while it streamed (its budget removed) */
	if (!kitty_on(vte)) {
		tsm_kitty_cancel(vte->kitty);
		return;
	}
	work = sixel_work(vte);
	tsm_kitty_end(vte->kitty, data);
	sixel_charge(vte, sixel_work(vte) - work);
}

static void send_primary_da(struct tsm_vte *vte)
{
	const static char str[] = "\e[?60;1;6;9;15c";
	/* attribute 4: sixel graphics */
	const static char sixel[] = "\e[?60;1;4;6;9;15c";

	if (sixel_on(vte))
		vte_write(vte, sixel, sizeof(sixel) - 1);
	else
		vte_write(vte, str, sizeof(str) - 1);
}

/* execute control character (C0 or C1) */
static void do_execute(struct tsm_vte *vte, uint32_t ctrl)
{
	/* text after a control character starts its own cluster */
	if (ctrl)
		screen_seg_reset(vte->con);

	switch (ctrl) {
	case 0x00: /* NUL */
		/* Ignore on input */
		break;
	case 0x05: /* ENQ */
		/* Transmit answerback message */
		/* TODO: is there a better answer than ACK?  */
		vte_write(vte, "\x06", 1);
		break;
	case 0x07: /* BEL */
		if (vte->bell_cb)
			vte->bell_cb(vte, vte->bell_data);
		break;
	case 0x08: /* BS */
		/* Move cursor one position left */
		tsm_screen_move_left(vte->con, 1);
		break;
	case 0x09: /* HT */
		/* Move to next tab stop or end of line */
		tsm_screen_tab_right(vte->con, 1);
		break;
	case 0x0a: /* LF */
	case 0x0b: /* VT */
	case 0x0c: /* FF */
		/* Line feed or newline (CR/NL mode) */
		if (vte->flags & TSM_VTE_FLAG_LINE_FEED_NEW_LINE_MODE)
			tsm_screen_newline(vte->con);
		else
			tsm_screen_move_down(vte->con, 1, true);
		break;
	case 0x0d: /* CR */
		/* Move cursor to left margin */
		tsm_screen_move_line_home(vte->con);
		break;
	case 0x0e: /* SO */
		/* Map G1 character set into GL */
		vte->gl = &vte->g1;
		break;
	case 0x0f: /* SI */
		/* Map G0 character set into GL */
		vte->gl = &vte->g0;
		break;
	case 0x11: /* XON */
		/* Resume transmission */
		/* TODO */
		break;
	case 0x13: /* XOFF */
		/* Stop transmission */
		/* TODO */
		break;
	case 0x18: /* CAN */
		/* Cancel escape sequence */
		/* nothing to do here */
		break;
	case 0x1a: /* SUB */
		/* Discard current escape sequence and show err-sym */
		write_console(vte, 0xbf);
		break;
	case 0x1b: /* ESC */
		/* Invokes an escape sequence */
		/* nothing to do here */
		break;
	case 0x1f: /* DEL */
		/* Ignored */
		break;
	case 0x84: /* IND */
		/* Move down one row, perform scroll-up if needed */
		tsm_screen_move_down(vte->con, 1, true);
		break;
	case 0x85: /* NEL */
		/* CR/NL with scroll-up if needed */
		tsm_screen_newline(vte->con);
		break;
	case 0x88: /* HTS */
		/* Set tab stop at current position */
		tsm_screen_set_tabstop(vte->con);
		break;
	case 0x8d: /* RI */
		/* Move up one row, perform scroll-down if needed */
		tsm_screen_move_up(vte->con, 1, true);
		break;
	case 0x8e: /* SS2 */
		/* Temporarily map G2 into GL for next char only */
		vte->glt = &vte->g2;
		break;
	case 0x8f: /* SS3 */
		/* Temporarily map G3 into GL for next char only */
		vte->glt = &vte->g3;
		break;
	case 0x9a: /* DECID */
		/* Send device attributes response like ANSI DA */
		send_primary_da(vte);
		break;
	case 0x9c: /* ST */
		/* End control string */
		/* nothing to do here */
		break;
	default:
		llog_debug(vte, "unhandled control char %u", ctrl);
	}
}

static void do_clear(struct tsm_vte *vte)
{
	int i;

	vte->csi_argc = 0;
	for (i = 0; i < CSI_ARG_MAX; ++i) {
		vte->csi_argv[i] = -1;
		vte->csi_subargc[i] = 0;
	}
	vte->csi_subarg_drop = false;
	vte->csi_flags = 0;

	vte->osc_len = 0;
	memset(vte->osc_arg, 0, sizeof(vte->osc_arg));
}

static void do_collect(struct tsm_vte *vte, uint32_t data)
{
	switch (data) {
	case '!':
		vte->csi_flags |= CSI_BANG;
		break;
	case '$':
		vte->csi_flags |= CSI_CASH;
		break;
	case '?':
		vte->csi_flags |= CSI_WHAT;
		break;
	case '>':
		vte->csi_flags |= CSI_GT;
		break;
	case ' ':
		vte->csi_flags |= CSI_SPACE;
		break;
	case '\'':
		vte->csi_flags |= CSI_SQUOTE;
		break;
	case '"':
		vte->csi_flags |= CSI_DQUOTE;
		break;
	case '*':
		vte->csi_flags |= CSI_MULT;
		break;
	case '+':
		vte->csi_flags |= CSI_PLUS;
		break;
	case '(':
		vte->csi_flags |= CSI_POPEN;
		break;
	case ')':
		vte->csi_flags |= CSI_PCLOSE;
		break;
	case '#':
		vte->csi_flags |= CSI_HASH;
		break;
	}
}

static void do_param(struct tsm_vte *vte, uint32_t data)
{
	unsigned int *subargc;
	int *val, new;

	if (data == ';') {
		if (vte->csi_argc < CSI_ARG_MAX) {
			// default parameter value is 0 if omitted
			if (vte->csi_argv[vte->csi_argc] == -1)
				vte->csi_argv[vte->csi_argc] = 0;
			vte->csi_argc++;
		}
		vte->csi_subarg_drop = false;
		return;
	}

	if (vte->csi_argc >= CSI_ARG_MAX)
		return;

	subargc = &vte->csi_subargc[vte->csi_argc];
	if (data == ':') {
		/* a sub-parameter of the current argument; past the limit the
		 * rest are dropped and the sequence still dispatches */
		if (*subargc < CSI_SUBARG_MAX)
			vte->csi_subargv[vte->csi_argc][(*subargc)++] = -1;
		else
			vte->csi_subarg_drop = true;
		return;
	}

	if (vte->csi_subarg_drop)
		return;
	if (*subargc)
		val = &vte->csi_subargv[vte->csi_argc][*subargc - 1];
	else
		val = &vte->csi_argv[vte->csi_argc];

	/* avoid integer overflows; max allowed value is 16384 anyway */
	if (*val > 0xffff)
		return;

	if (data >= '0' && data <= '9') {
		new = *val;
		if (new <= 0)
			new = data - '0';
		else
			new = new * 10 + data - '0';
		*val = new;
	}
}

static bool set_charset(struct tsm_vte *vte, tsm_vte_charset *set)
{
	if (vte->csi_flags & CSI_POPEN)
		vte->g0 = set;
	else if (vte->csi_flags & CSI_PCLOSE)
		vte->g1 = set;
	else if (vte->csi_flags & CSI_MULT)
		vte->g2 = set;
	else if (vte->csi_flags & CSI_PLUS)
		vte->g3 = set;
	else
		return false;

	return true;
}

static void do_esc(struct tsm_vte *vte, uint32_t data)
{
	switch (data) {
	case 'B': /* map ASCII into G0-G3 */
		if (set_charset(vte, &tsm_vte_unicode_lower))
			return;
		break;
	case '<': /* map DEC supplemental into G0-G3 */
		if (set_charset(vte, &tsm_vte_dec_supplemental_graphics))
			return;
		break;
	case '0': /* map DEC special into G0-G3 */
		if (set_charset(vte, &tsm_vte_dec_special_graphics))
			return;
		break;
	case 'A': /* map British into G0-G3 */
		/* TODO: create British charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case '4': /* map Dutch into G0-G3 */
		/* TODO: create Dutch charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'C':
	case '5': /* map Finnish into G0-G3 */
		/* TODO: create Finnish charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'R': /* map French into G0-G3 */
		/* TODO: create French charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'Q': /* map French-Canadian into G0-G3 */
		/* TODO: create French-Canadian charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'K': /* map German into G0-G3 */
		/* TODO: create German charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'Y': /* map Italian into G0-G3 */
		/* TODO: create Italian charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'E':
	case '6': /* map Norwegian/Danish into G0-G3 */
		/* TODO: create Norwegian/Danish charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'Z': /* map Spanish into G0-G3 */
		/* TODO: create Spanish charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'H':
	case '7': /* map Swedish into G0-G3 */
		/* TODO: create Swedish charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case '=': /* map Swiss into G0-G3 */
		/* TODO: create Swiss charset from DEC */
		if (set_charset(vte, &tsm_vte_unicode_upper))
			return;
		break;
	case 'F':
		if (vte->csi_flags & CSI_SPACE) {
			/* S7C1T */
			/* Disable 8bit C1 mode */
			vte->flags &= ~TSM_VTE_FLAG_USE_C1;
			return;
		}
		break;
	case 'G':
		if (vte->csi_flags & CSI_SPACE) {
			/* S8C1T */
			/* Enable 8bit C1 mode */
			vte->flags |= TSM_VTE_FLAG_USE_C1;
			return;
		}
		break;
	}

	/* everything below is only valid without CSI flags */
	if (vte->csi_flags) {
		llog_debug(vte, "unhandled escape seq %u", data);
		return;
	}

	switch (data) {
	case 'D': /* IND */
		/* Move down one row, perform scroll-up if needed */
		tsm_screen_move_down(vte->con, 1, true);
		break;
	case 'E': /* NEL */
		/* CR/NL with scroll-up if needed */
		tsm_screen_newline(vte->con);
		break;
	case 'H': /* HTS */
		/* Set tab stop at current position */
		tsm_screen_set_tabstop(vte->con);
		break;
	case 'M': /* RI */
		/* Move up one row, perform scroll-down if needed */
		tsm_screen_move_up(vte->con, 1, true);
		break;
	case 'N': /* SS2 */
		/* Temporarily map G2 into GL for next char only */
		vte->glt = &vte->g2;
		break;
	case 'O': /* SS3 */
		/* Temporarily map G3 into GL for next char only */
		vte->glt = &vte->g3;
		break;
	case 'Z': /* DECID */
		/* Send device attributes response like ANSI DA */
		send_primary_da(vte);
		break;
	case '\\': /* ST */
		/* End control string */
		/* nothing to do here */
		break;
	case '~': /* LS1R */
		/* Invoke G1 into GR */
		vte->gr = &vte->g1;
		break;
	case 'n': /* LS2 */
		/* Invoke G2 into GL */
		vte->gl = &vte->g2;
		break;
	case '}': /* LS2R */
		/* Invoke G2 into GR */
		vte->gr = &vte->g2;
		break;
	case 'o': /* LS3 */
		/* Invoke G3 into GL */
		vte->gl = &vte->g3;
		break;
	case '|': /* LS3R */
		/* Invoke G3 into GR */
		vte->gr = &vte->g3;
		break;
	case '=': /* DECKPAM */
		/* Set application keypad mode */
		vte->flags |= TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE;
		break;
	case '>': /* DECKPNM */
		/* Set numeric keypad mode */
		vte->flags &= ~TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE;
		break;
	case 'c': /* RIS */
		/* hard reset */
		tsm_vte_hard_reset(vte);
		break;
	case '7': /* DECSC */
		/* save console state */
		save_state(vte);
		break;
	case '8': /* DECRC */
		/* restore console state */
		restore_state(vte);
		break;
	default:
		llog_debug(vte, "unhandled escape seq %u", data);
	}
}

/* An SGR colour as a cell keeps it, unresolved */
struct sgr_color {
	unsigned int kind;	/* enum cell_color_kind */
	struct cell_color c;
};

static void sgr_rgb(struct sgr_color *out, uint8_t r, uint8_t g, uint8_t b)
{
	out->kind = CELL_COLOR_RGB;
	out->c.code = -1;
	out->c.rgb[0] = r;
	out->c.rgb[1] = g;
	out->c.rgb[2] = b;
}

/* one of the 256 colours: the 16 base colours stay palette codes, so the
 * palette and bold apply when the cell is drawn */
static void sgr_index(struct sgr_color *out, int index)
{
	memset(&out->c, 0, sizeof(out->c));
	if (index < 16) {
		out->kind = CELL_COLOR_CODE;
		out->c.code = index;
	} else {
		out->kind = CELL_COLOR_INDEX;
		out->c.code = -1;
		out->c.rgb[0] = index;
	}
}

/*
 * Read the colour that SGR 38, 48 or 58 at argument @i selects. The forms are
 * xterm's ";5;n" and ";2;r;g;b", which always takes exactly three values, and
 * the colon forms of ITU T.416: ":5:n" and ":2:cs:r:g:b", where the colour
 * space cs may be empty and the fields T.416 puts after b are ignored. The
 * colon form ":2:r:g:b" is not in T.416 but programs send it; it is told apart
 * from ":2:cs:r:g:b" by its count of sub-parameters, as foot does (csi.c,
 * MIT, Copyright (c) 2019 Daniel Eklöf). An empty colon field reads as 0.
 *
 * Out-of-range values differ by form. The colon forms are new to this fork, so
 * an index or component above 255 rejects the colour. The semicolon forms keep
 * upstream's handling, which the upstream goldens record: a component keeps its
 * low byte and any index is looked up. A semicolon form whose arguments carry
 * sub-parameters is not that form.
 *
 * Returns how many arguments after @i the colour used (the semicolon forms
 * span several), or -1 if it is not one of these forms.
 */
static int sgr_color(struct tsm_vte *vte, unsigned int i, struct sgr_color *out)
{
	const unsigned int *nsubs = &vte->csi_subargc[i];
	const int *sub = vte->csi_subargv[i];
	const int *arg = &vte->csi_argv[i];
	unsigned int nargs = vte->csi_argc - i - 1;
	int index, r, g, b, used;

	if (nsubs[0] == 2 && sub[0] == 5) {
		index = sub[1] < 0 ? 0 : sub[1];
		if (index > 255)
			return -1;
		used = 0;
	} else if (nsubs[0] >= 4 && sub[0] == 2) {
		/* r, g, b follow the colour-space slot when there is one */
		sub += nsubs[0] == 4 ? 1 : 2;
		r = sub[0] < 0 ? 0 : sub[0];
		g = sub[1] < 0 ? 0 : sub[1];
		b = sub[2] < 0 ? 0 : sub[2];
		if (r > 255 || g > 255 || b > 255)
			return -1;
		sgr_rgb(out, r, g, b);
		return 0;
	} else if (nsubs[0]) {
		return -1;
	} else if (nargs >= 2 && arg[1] == 5 && arg[2] >= 0 &&
		   !nsubs[1] && !nsubs[2]) {
		index = arg[2];
		used = 2;
	} else if (nargs >= 4 && arg[1] == 2 &&
		   arg[2] >= 0 && arg[3] >= 0 && arg[4] >= 0 &&
		   !nsubs[1] && !nsubs[2] && !nsubs[3] && !nsubs[4]) {
		sgr_rgb(out, arg[2], arg[3], arg[4]);
		return 4;
	} else {
		return -1;
	}

	/* upstream draws a semicolon index above 255 black */
	if (index > 255)
		sgr_rgb(out, 0, 0, 0);
	else
		sgr_index(out, index);
	return used;
}

/* SGR 4 with its sub-parameter: 4 and 4:1 single, 4:0 none, 4:2-4:5
 * double, curly, dotted, dashed; any other style draws single, as Ghostty
 * does (sgr.zig, MIT). Returns false for more than one sub-parameter. */
static bool sgr_underline(struct tsm_vte *vte, unsigned int i)
{
	int style;

	if (vte->csi_subargc[i] > 1)
		return false;

	style = TSM_UNDERLINE_SINGLE;
	if (vte->csi_subargc[i] == 1) {
		style = vte->csi_subargv[i][0];
		if (style < 0)
			style = TSM_UNDERLINE_NONE;
		else if (style > TSM_UNDERLINE_DASHED)
			style = TSM_UNDERLINE_SINGLE;
	}
	set_underline(&vte->cattr, style);
	return true;
}

static void csi_attribute(struct tsm_vte *vte)
{
	struct sgr_color color;
	unsigned int i;
	int used;

	if (vte->csi_argc <= 1 && vte->csi_argv[0] == -1 &&
	    !vte->csi_subargc[0]) {
		vte->csi_argc = 1;
		vte->csi_argv[0] = 0;
	}

	for (i = 0; i < vte->csi_argc; ++i) {
		/* Only 4, 38, 48 and 58 take sub-parameters. Any other
		 * argument that has them is skipped on its own. */
		switch (vte->csi_argv[i]) {
		case 4:
		case 38:
		case 48:
		case 58:
			break;
		default:
			if (vte->csi_subargc[i]) {
				llog_debug(vte, "SGR %i with unknown sub-parameters",
					   vte->csi_argv[i]);
				continue;
			}
		}

		switch (vte->csi_argv[i]) {
		case -1:
			break;
		case 0:
			reset_sgr(vte, &vte->cattr);
			break;
		case 1:
			vte->cattr.bold = 1;
			break;
		case 2:
			vte->cattr.dim = 1;
			break;
		case 3:
			vte->cattr.italic = 1;
			break;
		case 4:
			if (!sgr_underline(vte, i))
				llog_debug(vte, "SGR 4 with unknown sub-parameters");
			break;
		case 5:
			vte->cattr.blink = 1;
			break;
		case 7:
			vte->cattr.inverse = 1;
			break;
		case 9:
			vte->cattr.strike = 1;
			break;
		case 21:
			/* ECMA-48 double underline; the Linux VT once read it as
			 * normal intensity */
			set_underline(&vte->cattr, TSM_UNDERLINE_DOUBLE);
			break;
		case 22:
			vte->cattr.bold = 0;
			vte->cattr.dim = 0;
			break;
		case 23:
			vte->cattr.italic = 0;
			break;
		case 24:
			set_underline(&vte->cattr, TSM_UNDERLINE_NONE);
			break;
		case 25:
			vte->cattr.blink = 0;
			break;
		case 27:
			vte->cattr.inverse = 0;
			break;
		case 29:
			vte->cattr.strike = 0;
			break;
		case 30 ... 37:
			sgr_index(&color, vte->csi_argv[i] - 30);
			vte->cattr.fg = color.c;
			vte->cattr.fg_kind = color.kind;
			break;
		case 39:
			vte->cattr.fg = vte->def_attr.fg;
			vte->cattr.fg_kind = vte->def_attr.fg_kind;
			break;
		case 40 ... 47:
			sgr_index(&color, vte->csi_argv[i] - 40);
			vte->cattr.bg = color.c;
			vte->cattr.bg_kind = color.kind;
			break;
		case 49:
			vte->cattr.bg = vte->def_attr.bg;
			vte->cattr.bg_kind = vte->def_attr.bg_kind;
			break;
		case 90 ... 97:
			sgr_index(&color, vte->csi_argv[i] - 90 + 8);
			vte->cattr.fg = color.c;
			vte->cattr.fg_kind = color.kind;
			break;
		case 100 ... 107:
			sgr_index(&color, vte->csi_argv[i] - 100 + 8);
			vte->cattr.bg = color.c;
			vte->cattr.bg_kind = color.kind;
			break;
		case 38:
		case 48:
		case 58:
			used = sgr_color(vte, i, &color);
			if (used < 0) {
				llog_debug(vte, "invalid colour in SGR %i",
					   vte->csi_argv[i]);
				break;
			}
			if (vte->csi_argv[i] == 38) {
				vte->cattr.fg = color.c;
				vte->cattr.fg_kind = color.kind;
			} else if (vte->csi_argv[i] == 48) {
				vte->cattr.bg = color.c;
				vte->cattr.bg_kind = color.kind;
			} else {
				vte->cattr.ul = color.c;
				vte->cattr.ul_kind = color.kind;
				vte->cattr.ucset = 1;
			}
			i += used;
			break;
		case 59:
			clear_ucolor(&vte->cattr);
			break;
		default:
			llog_debug(vte, "unhandled SGR attr %i",
				   vte->csi_argv[i]);
		}
	}

	if (vte->flags & TSM_VTE_FLAG_BACKGROUND_COLOR_ERASE_MODE)
		set_erase_attr(vte);
}

static void csi_soft_reset(struct tsm_vte *vte)
{
	tsm_vte_reset(vte);
}

static void csi_compat_mode(struct tsm_vte *vte)
{
	/* always perform soft reset */
	csi_soft_reset(vte);

	if (vte->csi_argv[0] == 61) {
		/* Switching to VT100 compatibility mode. We do
		 * not support this mode, so ignore it. In fact,
		 * we are almost compatible to it, anyway, so
		 * there is no need to explicitly select it.
		 * However, we enable 7bit mode to avoid
		 * character-table problems */
		vte->flags |= TSM_VTE_FLAG_7BIT_MODE;
		vte->g0 = &tsm_vte_unicode_lower;
		vte->g1 = &tsm_vte_dec_supplemental_graphics;
	} else if (vte->csi_argv[0] == 62 ||
		   vte->csi_argv[0] == 63 ||
		   vte->csi_argv[0] == 64) {
		/* Switching to VT2/3/4 compatibility mode. We
		 * are always compatible with this so ignore it.
		 * We always send 7bit controls so we also do
		 * not care for the parameter value here that
		 * select the control-mode.
		 * VT220 defines argument 2 as 7bit mode but
		 * VT3xx up to VT5xx use it as 8bit mode. We
		 * choose to conform with the latter here.
		 * We also enable 8bit mode when VT220
		 * compatibility is requested explicitly. */
		if (vte->csi_argv[1] == 1 ||
		    vte->csi_argv[1] == 2)
			vte->flags |= TSM_VTE_FLAG_USE_C1;

		vte->flags |= TSM_VTE_FLAG_8BIT_MODE;
		vte->g0 = &tsm_vte_unicode_lower;
		vte->g1 = &tsm_vte_dec_supplemental_graphics;
	} else {
		llog_debug(vte, "unhandled DECSCL 'p' CSI %i, switching to utf-8 mode again",
			   vte->csi_argv[0]);
	}
}

static inline void set_reset_flag(struct tsm_vte *vte, bool set,
				  unsigned int flag)
{
	if (set)
		vte->flags |= flag;
	else
		vte->flags &= ~flag;
}

/*
 * The mode accessors (ADR-201 in aaronsb/kmscon). Each SUPPORTED and PARTIAL
 * row of tsm-modes.def has a setter, which SM or RM and the resets call, and
 * a getter, which DECRQM and the resets read. A getter answers whether the
 * mode is set as libtsm holds it.
 */

static void mode_IRM_set(struct tsm_vte *vte, bool set)
{
	set_reset_flag(vte, set, TSM_VTE_FLAG_INSERT_REPLACE_MODE);
	if (set)
		tsm_screen_set_flags(vte->con, TSM_SCREEN_INSERT_MODE);
	else
		tsm_screen_reset_flags(vte->con, TSM_SCREEN_INSERT_MODE);
}

static bool mode_IRM_get(struct tsm_vte *vte)
{
	return vte->flags & TSM_VTE_FLAG_INSERT_REPLACE_MODE;
}

static void mode_SRM_set(struct tsm_vte *vte, bool set)
{
	set_reset_flag(vte, set, TSM_VTE_FLAG_SEND_RECEIVE_MODE);
}

static bool mode_SRM_get(struct tsm_vte *vte)
{
	return vte->flags & TSM_VTE_FLAG_SEND_RECEIVE_MODE;
}

static void mode_LNM_set(struct tsm_vte *vte, bool set)
{
	set_reset_flag(vte, set, TSM_VTE_FLAG_LINE_FEED_NEW_LINE_MODE);
}

static bool mode_LNM_get(struct tsm_vte *vte)
{
	return vte->flags & TSM_VTE_FLAG_LINE_FEED_NEW_LINE_MODE;
}

static void mode_DECCKM_set(struct tsm_vte *vte, bool set)
{
	set_reset_flag(vte, set, TSM_VTE_FLAG_CURSOR_KEY_MODE);
}

static bool mode_DECCKM_get(struct tsm_vte *vte)
{
	return vte->flags & TSM_VTE_FLAG_CURSOR_KEY_MODE;
}

static void mode_DECSCNM_set(struct tsm_vte *vte, bool set)
{
	set_reset_flag(vte, set, TSM_VTE_FLAG_INVERSE_SCREEN_MODE);
	if (set)
		tsm_screen_set_flags(vte->con, TSM_SCREEN_INVERSE);
	else
		tsm_screen_reset_flags(vte->con, TSM_SCREEN_INVERSE);
}

static bool mode_DECSCNM_get(struct tsm_vte *vte)
{
	return vte->flags & TSM_VTE_FLAG_INVERSE_SCREEN_MODE;
}

static void mode_DECOM_set(struct tsm_vte *vte, bool set)
{
	set_reset_flag(vte, set, TSM_VTE_FLAG_ORIGIN_MODE);
	if (set)
		tsm_screen_set_flags(vte->con, TSM_SCREEN_REL_ORIGIN);
	else
		tsm_screen_reset_flags(vte->con, TSM_SCREEN_REL_ORIGIN);
}

static bool mode_DECOM_get(struct tsm_vte *vte)
{
	return vte->flags & TSM_VTE_FLAG_ORIGIN_MODE;
}

static void mode_DECAWM_set(struct tsm_vte *vte, bool set)
{
	set_reset_flag(vte, set, TSM_VTE_FLAG_AUTO_WRAP_MODE);
	if (set)
		tsm_screen_set_flags(vte->con, TSM_SCREEN_AUTO_WRAP);
	else
		tsm_screen_reset_flags(vte->con, TSM_SCREEN_AUTO_WRAP);
}

static bool mode_DECAWM_get(struct tsm_vte *vte)
{
	return vte->flags & TSM_VTE_FLAG_AUTO_WRAP_MODE;
}

static void mode_DECTCEM_set(struct tsm_vte *vte, bool set)
{
	set_reset_flag(vte, set, TSM_VTE_FLAG_TEXT_CURSOR_MODE);
	if (set)
		tsm_screen_reset_flags(vte->con, TSM_SCREEN_HIDE_CURSOR);
	else
		tsm_screen_set_flags(vte->con, TSM_SCREEN_HIDE_CURSOR);
}

static bool mode_DECTCEM_get(struct tsm_vte *vte)
{
	return vte->flags & TSM_VTE_FLAG_TEXT_CURSOR_MODE;
}

/*
 * Mouse tracking, as ctlseqs has it: two sets of modes, each mutually
 * exclusive. The protocol, 9, 1000, 1002 or 1003, says which events are
 * reported, and the encoding, 1006 or 1016 (SGR in cells or in pixels), how;
 * with neither, the X10 encoding. SM of a mode replaces the other in its
 * set. RM of any protocol clears the protocol, as libtsm always has; RM of
 * an encoding clears it only when it is the one set, as xterm does.
 * 1001 (highlight tracking), 1005 (UTF-8) and 1015 (urxvt) have no row.
 */

/* tells the host which events to pass, and whether in pixels */
static void mouse_notify(struct tsm_vte *vte)
{
	enum tsm_mouse_track_mode mode = TSM_MOUSE_TRACK_DISABLE;

	if (!vte->mouse_cb)
		return;
	if (vte->mouse_track == TSM_VTE_MOUSE_EVENT_ANY)
		mode = TSM_MOUSE_TRACK_ANY;
	else if (vte->mouse_track)
		mode = TSM_MOUSE_TRACK_BTN;
	vte->mouse_cb(vte, mode, vte->mouse_encoding == TSM_VTE_MOUSE_MODE_PIXEL,
		      vte->mouse_data);
}

/* the host hears only of a change, since kmscon drops the selection when
 * told */
static void mouse_tracking_set(struct tsm_vte *vte, unsigned int mode, bool set)
{
	unsigned int track = set ? mode : 0;

	if (track == vte->mouse_track)
		return;
	vte->mouse_track = track;
	vte->mouse_last_col = 0;
	vte->mouse_last_row = 0;
	mouse_notify(vte);
}

/* the host hears of an encoding only while something is tracked, and only
 * of a change */
static void mouse_encoding_set(struct tsm_vte *vte, unsigned int encoding, bool set)
{
	if (set)
		vte->mouse_encoding = encoding;
	else if (vte->mouse_encoding == encoding)
		vte->mouse_encoding = 0;
	else
		return;
	if (vte->mouse_track)
		mouse_notify(vte);
}

static void mode_X10MOUSE_set(struct tsm_vte *vte, bool set)
{
	mouse_tracking_set(vte, TSM_VTE_MOUSE_MODE_X10, set);
}

static bool mode_X10MOUSE_get(struct tsm_vte *vte)
{
	return vte->mouse_track == TSM_VTE_MOUSE_MODE_X10;
}

static void mode_VT200MOUSE_set(struct tsm_vte *vte, bool set)
{
	mouse_tracking_set(vte, TSM_VTE_MOUSE_MODE_VT200, set);
}

static bool mode_VT200MOUSE_get(struct tsm_vte *vte)
{
	return vte->mouse_track == TSM_VTE_MOUSE_MODE_VT200;
}

static void mode_BTNMOUSE_set(struct tsm_vte *vte, bool set)
{
	mouse_tracking_set(vte, TSM_VTE_MOUSE_EVENT_BTN, set);
}

static bool mode_BTNMOUSE_get(struct tsm_vte *vte)
{
	return vte->mouse_track == TSM_VTE_MOUSE_EVENT_BTN;
}

static void mode_ANYMOUSE_set(struct tsm_vte *vte, bool set)
{
	mouse_tracking_set(vte, TSM_VTE_MOUSE_EVENT_ANY, set);
}

static bool mode_ANYMOUSE_get(struct tsm_vte *vte)
{
	return vte->mouse_track == TSM_VTE_MOUSE_EVENT_ANY;
}

static void mode_SGRMOUSE_set(struct tsm_vte *vte, bool set)
{
	mouse_encoding_set(vte, TSM_VTE_MOUSE_MODE_SGR, set);
}

static bool mode_SGRMOUSE_get(struct tsm_vte *vte)
{
	return vte->mouse_encoding == TSM_VTE_MOUSE_MODE_SGR;
}

static void mode_PIXMOUSE_set(struct tsm_vte *vte, bool set)
{
	mouse_encoding_set(vte, TSM_VTE_MOUSE_MODE_PIXEL, set);
}

static bool mode_PIXMOUSE_get(struct tsm_vte *vte)
{
	return vte->mouse_encoding == TSM_VTE_MOUSE_MODE_PIXEL;
}

/*
 * AT&T 610 cursor blink. DECRQM reports whether the cursor blinks: the host
 * blinks it (tsm_vte_set_cursor_blink) and the style is one that blinks, the
 * default or an odd DECSCUSR style, as kmscon draws them. SM and RM do
 * nothing yet (aaronsb/libtsm#57).
 */
static void mode_CURBLINK_set(struct tsm_vte *vte, bool set)
{
	(void)vte;
	(void)set;
}

static bool mode_CURBLINK_get(struct tsm_vte *vte)
{
	enum tsm_screen_cursor_style style = tsm_screen_get_cursor_style(vte->con);

	return vte->cursor_blink && (style == TSM_SCREEN_CURSOR_DEFAULT || (style & 1));
}

/*
 * The alternate screen. 47, 1047 and 1049 share the screen's flag, so each
 * reports the alternate screen however it was entered. The four do nothing
 * while TSM_VTE_FLAG_TITE_INHIBIT_MODE is set, which no sequence sets.
 */

static bool alt_screen_get(struct tsm_vte *vte)
{
	return tsm_screen_get_flags(vte->con) & TSM_SCREEN_ALTERNATE;
}

static void mode_ALTBUF_set(struct tsm_vte *vte, bool set)
{
	if (vte->flags & TSM_VTE_FLAG_TITE_INHIBIT_MODE)
		return;

	if (set)
		tsm_screen_set_flags(vte->con, TSM_SCREEN_ALTERNATE);
	else
		tsm_screen_reset_flags(vte->con, TSM_SCREEN_ALTERNATE);
}

static bool mode_ALTBUF_get(struct tsm_vte *vte)
{
	return alt_screen_get(vte);
}

/* the alternate screen, erased as it is left */
static void mode_ALTBUF1047_set(struct tsm_vte *vte, bool set)
{
	if (vte->flags & TSM_VTE_FLAG_TITE_INHIBIT_MODE)
		return;

	if (set) {
		tsm_screen_set_flags(vte->con, TSM_SCREEN_ALTERNATE);
	} else {
		tsm_screen_erase_screen(vte->con, false);
		tsm_screen_reset_flags(vte->con, TSM_SCREEN_ALTERNATE);
	}
}

static bool mode_ALTBUF1047_get(struct tsm_vte *vte)
{
	return alt_screen_get(vte);
}

/* set saves the cursor, reset restores it; nothing stays set */
static void mode_SAVECUR_set(struct tsm_vte *vte, bool set)
{
	if (vte->flags & TSM_VTE_FLAG_TITE_INHIBIT_MODE)
		return;

	if (set) {
		vte->alt_cursor_x = tsm_screen_get_cursor_x(vte->con);
		vte->alt_cursor_y = tsm_screen_get_cursor_y(vte->con);
	} else {
		tsm_screen_move_to(vte->con, vte->alt_cursor_x,
				   vte->alt_cursor_y);
	}
}

static bool mode_SAVECUR_get(struct tsm_vte *vte)
{
	(void)vte;
	return false;
}

/* the alternate screen, erased as it is entered, with the cursor saved */
static void mode_ALTBUF1049_set(struct tsm_vte *vte, bool set)
{
	if (vte->flags & TSM_VTE_FLAG_TITE_INHIBIT_MODE)
		return;

	if (set) {
		vte->alt_cursor_x = tsm_screen_get_cursor_x(vte->con);
		vte->alt_cursor_y = tsm_screen_get_cursor_y(vte->con);
		tsm_screen_set_flags(vte->con, TSM_SCREEN_ALTERNATE);
		tsm_screen_erase_screen(vte->con, false);
	} else {
		tsm_screen_reset_flags(vte->con, TSM_SCREEN_ALTERNATE);
		tsm_screen_move_to(vte->con, vte->alt_cursor_x,
				   vte->alt_cursor_y);
	}
}

static bool mode_ALTBUF1049_get(struct tsm_vte *vte)
{
	return alt_screen_get(vte);
}

/* DECSDM: set turns sixel scrolling off, in xterm's sense (ADR-103) */
static void mode_DECSDM_set(struct tsm_vte *vte, bool set)
{
	vte->sixel_no_scroll = set;
}

static bool mode_DECSDM_get(struct tsm_vte *vte)
{
	return vte->sixel_no_scroll;
}

/* each sixel starts from its own default registers */
static void mode_SIXPRIV_set(struct tsm_vte *vte, bool set)
{
	vte->sixel_private = set;
}

static bool mode_SIXPRIV_get(struct tsm_vte *vte)
{
	return vte->sixel_private;
}

static void mode_BRPASTE_set(struct tsm_vte *vte, bool set)
{
	vte->bracketed_paste = set;
}

static bool mode_BRPASTE_get(struct tsm_vte *vte)
{
	return vte->bracketed_paste;
}

/* grapheme cluster width (terminal-unicode-core) */
static void mode_GRAPHEME_set(struct tsm_vte *vte, bool set)
{
	screen_set_width_method(vte->con, set ? TSM_WIDTH_UNICODE :
				TSM_WIDTH_WCWIDTH);
}

static bool mode_GRAPHEME_get(struct tsm_vte *vte)
{
	return vte->con->width_method == TSM_WIDTH_UNICODE;
}

/* the cursor ends right of a sixel, not below it */
static void mode_SIXRIGHT_set(struct tsm_vte *vte, bool set)
{
	vte->sixel_cursor_right = set;
}

static bool mode_SIXRIGHT_get(struct tsm_vte *vte)
{
	return vte->sixel_cursor_right;
}

/*
 * Synchronized output: a hold starts at SM and ends at RM, RIS or DECSTR
 * (the table's soft reset), or by the host; each end is counted, and stops
 * tsm_vte_input_bounded after the byte that made it. SM does nothing while
 * the host does not present by the mode.
 */
static void sync_end(struct tsm_vte *vte)
{
	if (!vte->sync)
		return;
	vte->sync = false;
	++vte->sync_frames;
}

static void mode_SYNC_set(struct tsm_vte *vte, bool set)
{
	if (!set)
		sync_end(vte);
	else if (vte->sync_enabled)
		vte->sync = true;
}

static bool mode_SYNC_get(struct tsm_vte *vte)
{
	return vte->sync;
}

/* SM and RM (DECSET and DECRST with '?'): each row's setter, in order */
static void csi_mode(struct tsm_vte *vte, bool set)
{
	unsigned int i;

	for (i = 0; i < vte->csi_argc; ++i) {
		if (vte->csi_argv[i] < 0)
			continue;

		if (!(vte->csi_flags & CSI_WHAT)) {
			switch (vte->csi_argv[i]) {
#define MODE(kind, number, name, status, ...) \
	MODE_ONLY_ANSI_##kind(case number: MODE_TRACE(false, number); \
			      MODE_SET_##status(name); continue;)
#include "tsm-modes.def"
#undef MODE
			default:
				llog_debug(vte, "unknown non-DEC (Re)Set-Mode %d",
					   vte->csi_argv[i]);
				continue;
			}
		}

		switch (vte->csi_argv[i]) {
#define MODE(kind, number, name, status, ...) \
	MODE_ONLY_DEC_##kind(case number: MODE_TRACE(true, number); \
			     MODE_SET_##status(name); continue;)
#include "tsm-modes.def"
#undef MODE
		default:
			llog_debug(vte, "unknown DEC %set-Mode %d",
				   set?"S":"Res", vte->csi_argv[i]);
			continue;
		}
	}
}

/*
 * The DECRPM value for a mode: 1 set, 2 reset, 3 permanently set, 4
 * permanently reset, 0 not recognised. IGNORED and REFUSED rows report their
 * default as permanent.
 */
#define MODE_REPORT_SUPPORTED(name, dflt, host) \
	return !mode_offered(vte, host) ? MODE_FIXED(dflt) : mode_##name##_get(vte) ? 1 : 2;
#define MODE_REPORT_PARTIAL(name, dflt, host) \
	return !mode_offered(vte, host) ? MODE_FIXED(dflt) : mode_##name##_get(vte) ? 1 : 2;
#define MODE_REPORT_IGNORED(name, dflt, host) return MODE_FIXED(dflt);
#define MODE_REPORT_REFUSED(name, dflt, host) return MODE_FIXED(dflt);
#define MODE_FIXED(dflt) ((dflt) ? 3 : 4)

/*
 * A mode the host presents frames by (HOST_DRAW, mode 2026) is offered only
 * while the host says it does (tsm_vte_set_sync); until then it reports its
 * fixed state, as an ignored row does, since nothing would honour it.
 */
static bool mode_offered(struct tsm_vte *vte, enum mode_host host)
{
	return host != HOST_DRAW || vte->sync_enabled;
}

static int mode_report(struct tsm_vte *vte, bool dec, unsigned int number)
{
	if (!dec) {
		switch (number) {
#define MODE(kind, number, name, status, dflt, reset, host, ...) \
	MODE_ONLY_ANSI_##kind(case number: MODE_REPORT_##status(name, dflt, host))
#include "tsm-modes.def"
#undef MODE
		default:
			return 0;
		}
	}

	switch (number) {
#define MODE(kind, number, name, status, dflt, reset, host, ...) \
	MODE_ONLY_DEC_##kind(case number: MODE_REPORT_##status(name, dflt, host))
#include "tsm-modes.def"
#undef MODE
	default:
		return 0;
	}
}

/* DECRQM: CSI ? Ps $ p or CSI Ps $ p, answered for every number */
static void csi_decrqm(struct tsm_vte *vte)
{
	bool dec = vte->csi_flags & CSI_WHAT;
	unsigned int number = vte->csi_argv[0] > 0 ? vte->csi_argv[0] : 0;
	char buf[32];
	int len;

	len = snprintf(buf, sizeof(buf), "\e[%s%u;%d$y", dec ? "?" : "",
		       number, mode_report(vte, dec, number));
	if (len > 0 && (size_t)len < sizeof(buf))
		vte_write(vte, buf, len);
}

static void csi_dev_attr(struct tsm_vte *vte)
{
	if (vte->csi_argc <= 1 && vte->csi_argv[0] <= 0) {
		if (vte->csi_flags == 0) {
			send_primary_da(vte);
			return;
		} else if (vte->csi_flags & CSI_GT) {
			vte_write(vte, "\e[>1;1;0c", 9);
			return;
		}
	}

	llog_debug(vte, "unhandled DA: %x %d %d %d...", vte->csi_flags,
		   vte->csi_argv[0], vte->csi_argv[1], vte->csi_argv[2]);
}

static void csi_dsr(struct tsm_vte *vte)
{
	char buf[64];
	unsigned int x, y, len;

	if (vte->csi_argv[0] == 5) {
		vte_write(vte, "\e[0n", 4);
	} else if (vte->csi_argv[0] == 6) {
		x = tsm_screen_get_cursor_x(vte->con);
		y = tsm_screen_get_cursor_y(vte->con);
		len = snprintf(buf, sizeof(buf), "\e[%u;%uR", y + 1, x + 1);
		if (len >= sizeof(buf))
			vte_write(vte, "\e[0;0R", 6);
		else
			vte_write(vte, buf, len);
	}
}

static void csi_report_window_size(struct tsm_vte *vte)
{
	char buf[64];
	unsigned int w, h, len, resp;

	resp = vte->csi_argv[0] == 18 ? 8 : 9;

	w = tsm_screen_get_width(vte->con);
	h = tsm_screen_get_height(vte->con);
	len = snprintf(buf, sizeof(buf), "\e[%u;%u;%ut", resp,  h, w);
	if (len >= sizeof(buf))
		return;
	vte_write(vte, buf, len);
}

/* XTWINOPS 14 (text area) and 16 (cell) in pixels, the host's cell size */
static void csi_report_pixel_size(struct tsm_vte *vte)
{
	struct tsm_screen *con = vte->con;
	unsigned int cw = con->img.cell_width, ch = con->img.cell_height;
	char buf[64];
	int len;

	if (vte->csi_argv[0] == 14)
		len = snprintf(buf, sizeof(buf), "\e[4;%llu;%llut",
			       (unsigned long long)con->size_y * ch,
			       (unsigned long long)con->size_x * cw);
	else
		len = snprintf(buf, sizeof(buf), "\e[6;%u;%ut", ch, cw);
	if (len > 0 && (size_t)len < sizeof(buf))
		vte_write(vte, buf, len);
}

/*
 * XTSMGRAPHICS, CSI ? Pi ; Pa ; Pv S (ADR-103): item 1, the sixel colour
 * registers, read, reset, set from 2 to num-color-registers, and read the
 * maximum; item 2, the sixel geometry, read and read the maximum, both
 * max-graphic-size, and a change refused (status 3), as xterm refuses one.
 * Other items, ReGIS among them, and every item while sixel is off, answer
 * status 1.
 */
static void csi_xtsmgraphics(struct tsm_vte *vte)
{
	int item = vte->csi_argv[0], action = vte->csi_argv[1];
	int value = vte->csi_argv[2];
	unsigned int status = 0, nres = 0, res[2] = { 0, 0 }, count;
	char buf[64];
	int len;

	if (!sixel_on(vte) || (item != 1 && item != 2)) {
		status = 1;
	} else if (item == 1) {
		count = vte->sixel_registers;
		switch (action) {
		case 1: /* read */
			break;
		case 2: /* reset */
			count = vte->sixel_max_registers;
			break;
		case 3: /* set */
			if (value >= TSM_SIXEL_REGISTERS_MIN &&
			    (unsigned int)value <= vte->sixel_max_registers)
				count = value;
			else
				status = 3;
			break;
		case 4: /* read the maximum */
			res[0] = vte->sixel_max_registers;
			nres = 1;
			break;
		default:
			status = 2;
			break;
		}
		if (!status && !nres) {
			/* registers made at the next picture take the count */
			if (count != vte->sixel_registers) {
				sixel_drop(vte);
				vte->sixel_registers = count;
			}
			res[0] = count;
			nres = 1;
		}
	} else {
		switch (action) {
		case 1: /* read */
		case 4: /* read the maximum */
			res[0] = vte->graphic_max_w;
			res[1] = vte->graphic_max_h;
			nres = 2;
			break;
		case 2: /* reset */
		case 3: /* set */
			status = 3;
			break;
		default:
			status = 2;
			break;
		}
	}

	if (item < 0)
		item = 0;
	if (nres == 2)
		len = snprintf(buf, sizeof(buf), "\e[?%d;%u;%u;%uS", item,
			       status, res[0], res[1]);
	else if (nres)
		len = snprintf(buf, sizeof(buf), "\e[?%d;%u;%uS", item, status,
			       res[0]);
	else
		len = snprintf(buf, sizeof(buf), "\e[?%d;%uS", item, status);
	if (len > 0 && (size_t)len < sizeof(buf))
		vte_write(vte, buf, len);
}

static bool csi_has_subargs(struct tsm_vte *vte)
{
	unsigned int i;

	for (i = 0; i < vte->csi_argc; ++i)
		if (vte->csi_subargc[i])
			return true;
	return false;
}

static void do_csi(struct tsm_vte *vte, uint32_t data)
{
	int num, x, y, upper, lower;
	bool protect;

	if (vte->csi_argc < CSI_ARG_MAX)
		vte->csi_argc++;

	/* Only SGR reads sub-parameters. Any other sequence carrying them is
	 * ignored whole, as it was before colons were parsed, and as Ghostty
	 * does (Parser.zig, MIT). */
	if (data != 'm' && csi_has_subargs(vte)) {
		llog_debug(vte, "CSI %c with sub-parameters ignored", data);
		return;
	}

	switch (data) {
	case 'A': /* CUU */
		/* move cursor up */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_move_up(vte->con, num, false);
		break;
	case 'B': /* CUD */
		/* move cursor down */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_move_down(vte->con, num, false);
		break;
	case 'C': /* CUF */
		/* move cursor forward */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_move_right(vte->con, num);
		break;
	case 'D': /* CUB */
		/* move cursor backward */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_move_left(vte->con, num);
		break;
	case 'd': /* VPA */
		/* Vertical Line Position Absolute */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		x = tsm_screen_get_cursor_x(vte->con);
		tsm_screen_move_to(vte->con, x, num - 1);
		break;
	case 'E': /* CNL */
		/* Move cursor down the indicated # of rows, to column 1 */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_move_down(vte->con, num, false);
		tsm_screen_move_line_home(vte->con);
		break;
	case 'e': /* VPR */
		/* Vertical Line Position Relative */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		x = tsm_screen_get_cursor_x(vte->con);
		y = tsm_screen_get_cursor_y(vte->con);
		tsm_screen_move_to(vte->con, x, y + num);
		break;
	case 'F': /* CPL */
		/* Move cursor up the indicated # of rows, to column 1 */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_move_up(vte->con, num, false);
		tsm_screen_move_line_home(vte->con);
		break;
	case 'H': /* CUP */
	case 'f': /* HVP */
		/* position cursor */
		x = vte->csi_argv[0];
		if (x <= 0)
			x = 1;
		y = vte->csi_argv[1];
		if (y <= 0)
			y = 1;
		tsm_screen_move_to(vte->con, y - 1, x - 1);
		break;
	case 'G': /* CHA */
		/* Cursor Character Absolute */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		y = tsm_screen_get_cursor_y(vte->con);
		tsm_screen_move_to(vte->con, num - 1, y);
		break;
	case 'J':
		if (vte->csi_flags & CSI_WHAT)
			protect = true;
		else
			protect = false;

		if (vte->csi_argv[0] <= 0)
			tsm_screen_erase_cursor_to_screen(vte->con,
							      protect);
		else if (vte->csi_argv[0] == 1)
			tsm_screen_erase_screen_to_cursor(vte->con,
							      protect);
		else if (vte->csi_argv[0] == 2)
			tsm_screen_erase_screen(vte->con, protect);
		else if (vte->csi_argv[0] == 3)
			/* ED 3 (xterm): the scrollback, and the placements
			 * anchored on it */
			tsm_screen_clear_sb(vte->con);
		else
			llog_debug(vte, "unknown parameter to CSI-J: %d",
				   vte->csi_argv[0]);
		break;
	case 'K':
		if (vte->csi_flags & CSI_WHAT)
			protect = true;
		else
			protect = false;

		if (vte->csi_argv[0] <= 0)
			tsm_screen_erase_cursor_to_end(vte->con, protect);
		else if (vte->csi_argv[0] == 1)
			tsm_screen_erase_home_to_cursor(vte->con, protect);
		else if (vte->csi_argv[0] == 2)
			tsm_screen_erase_current_line(vte->con, protect);
		else
			llog_debug(vte, "unknown parameter to CSI-K: %d",
				   vte->csi_argv[0]);
		break;
	case 'X': /* ECH */
		/* erase characters */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_erase_chars(vte->con, num);
		break;
	case 'm':
		/* A '>' or '?' prefix marks a private sequence, not SGR:
		 * XTMODKEYS \033[>4;1m sets and XTQMODKEYS \033[?4m queries
		 * xterm's modifyOtherKeys level. Neither is SGR 4 (underline). */
		if (!(vte->csi_flags & (CSI_GT | CSI_WHAT)))
			csi_attribute(vte);
		break;
	case 'p':
		if (vte->csi_flags & CSI_GT) {
			/* XTSMPOINTER, CSI > Ps p, sets when xterm hides the
			 * pointer while the user types. libtsm gives the host
			 * no call for it, so it is dropped; it is not a reset
			 * (aaronsb/libtsm#59). */
			llog_debug(vte, "XTSMPOINTER ignored");
		} else if (vte->csi_flags & CSI_BANG) {
			/* DECSTR: Soft Reset */
			csi_soft_reset(vte);
		} else if (vte->csi_flags & CSI_CASH) {
			/* DECRQM: Request Mode */
			csi_decrqm(vte);
		} else if (!(vte->csi_flags & ~CSI_DQUOTE)) {
			/* DECSCL: Compatibility Level */
			/* Sometimes CSI_DQUOTE is set here, too */
			csi_compat_mode(vte);
		} else {
			llog_debug(vte, "unhandled CSI p, flags %x", vte->csi_flags);
		}
		break;
	case 'h': /* SM: Set Mode */
		csi_mode(vte, true);
		break;
	case 'l': /* RM: Reset Mode */
		csi_mode(vte, false);
		break;
	case 'r':
		/* XTRESTORE, CSI ? Pm r, which restores saved DEC private
		 * modes, is not implemented (ADR-201); it must not set the
		 * margins from its mode numbers */
		if (vte->csi_flags & CSI_WHAT) {
			llog_debug(vte, "XTRESTORE ignored");
			break;
		}

		/* DECSTBM: set margin size */
		upper = vte->csi_argv[0];
		if (upper < 0)
			upper = 0;
		lower = vte->csi_argv[1];
		if (lower < 0)
			lower = 0;
		tsm_screen_set_margins(vte->con, upper, lower);
		break;
	case 'c': /* DA */
		/* device attributes */
		csi_dev_attr(vte);
		break;
	case 'L': /* IL */
		/* insert lines */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_insert_lines(vte->con, num);
		break;
	case 'M': /* DL */
		/* delete lines */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_delete_lines(vte->con, num);
		break;
	case 'g': /* TBC */
		/* tabulation clear */
		num = vte->csi_argv[0];
		if (num <= 0)
			tsm_screen_reset_tabstop(vte->con);
		else if (num == 3)
			tsm_screen_reset_all_tabstops(vte->con);
		else
			llog_debug(vte, "invalid parameter %d to TBC CSI", num);
		break;
	case '@': /* ICH */
		/* insert characters */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_insert_chars(vte->con, num);
		break;
	case 'P': /* DCH */
		/* delete characters */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_delete_chars(vte->con, num);
		break;
	case 'Z': /* CBT */
		/* cursor horizontal backwards tab */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_tab_left(vte->con, num);
		break;
	case 'I': /* CHT */
		/* cursor horizontal forward tab */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_tab_right(vte->con, num);
		break;
	case 'n': /* DSR */
		/* device status reports */
		csi_dsr(vte);
		break;
	case 'S':
		if (vte->csi_flags & CSI_WHAT) {
			csi_xtsmgraphics(vte);
			break;
		}
		/* SU: scroll up */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_scroll_up(vte->con, num);
		break;
	case 'T': /* SD */
		/* scroll down */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_scroll_down(vte->con, num);
		break;
	case 't': /* XTWINOPS */
		if (vte->csi_argv[0] == 18 || vte->csi_argv[0] == 19)
			csi_report_window_size(vte);
		else if (vte->csi_argv[0] == 14 || vte->csi_argv[0] == 16)
			csi_report_pixel_size(vte);
		else
			llog_debug(vte, "unhandled CSI t sequence %d", vte->csi_argv[0]);
		break;
	case 'b': /* Repeat last char */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_repeat_char(vte->con, num, &vte->cattr);
		break;
	case 'q': /* DECLL - Load LEDs / DECSCUSR - Set Cursor Style */
		if (vte->csi_flags & CSI_SPACE) {
			num = vte->csi_argv[0];
			if (num < 0)
				num = 0;
			tsm_screen_set_cursor_style(vte->con, num);
		} else {
			num = vte->csi_argv[0];
			if (num <= 0) {
				vte->led_state = 0;
			} else if (num == 1) {
				vte->led_state |= TSM_VTE_LED_SCROLL_LOCK;
			} else if (num == 2) {
				vte->led_state |= TSM_VTE_LED_NUM_LOCK;
			} else if (num == 3) {
				vte->led_state |= TSM_VTE_LED_CAPS_LOCK;
			} else {
				break;
			}
			if (vte->led_cb)
				vte->led_cb(vte, vte->led_state, vte->led_data);
		}
		break;
	case ']': /* Cursor blinking rate */
		/* This behavior is similar to agetty */
		if (vte->csi_argv[0] == 16 && vte->csi_argc >= 2)
			tsm_screen_set_cursor_blinking_rate_ms(vte->con, vte->csi_argv[1]);
		break;
	default:
		llog_debug(vte, "unhandled CSI sequence %c", data);
	}
}

/* map a character according to current GL and GR maps */
static uint32_t vte_map(struct tsm_vte *vte, uint32_t val)
{
	/* 32, 127, 160 and 255 map to identity like all values >255 */
	switch (val) {
	case 33 ... 126:
		if (vte->glt) {
			val = (**vte->glt)[val - 32];
			vte->glt = NULL;
		} else {
			val = (**vte->gl)[val - 32];
		}
		break;
	case 161 ... 254:
		if (vte->grt) {
			val = (**vte->grt)[val - 160];
			vte->grt = NULL;
		} else {
			val = (**vte->gr)[val - 160];
		}
		break;
	}

	return val;
}

static void do_osc_collect(struct tsm_vte *vte, uint32_t val) {
	char buf[4];
	int len = tsm_ucs4_to_utf8(val, buf);
	if (vte->osc_len + len > sizeof(vte->osc_arg) - 1) {
		return;
	}

	memcpy(vte->osc_arg + vte->osc_len, buf, len);
	vte->osc_len += len;
}

static void vte_write_xcolor(struct tsm_vte *vte, char *code,
			     const char *end_seq,
			     uint8_t r, uint8_t g, uint8_t b)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "\e]%s;rgb:%02x%02x/%02x%02x/%02x%02x%s",
		 code, r, r, g, g, b, b, end_seq);
	vte_write(vte, buf, strlen(buf));
}

static void do_osc_4(struct tsm_vte *vte, const char *data, const char *end_seq)
{
	char buf[32];
	uint8_t cr, cg, cb;
	const char *c = data;

	while (*c) {
		// parse color code
		unsigned int code = 0;
		while (*c >= '0' && *c <= '9') {
			code = code * 10 + (*c++ - '0');
		}
		if (*c++ != ';') {
			break;
		}

		// parse value field
		if (*c == '?') {
			c++;
			// handle rgb value query
			snprintf(buf, sizeof(buf), "4;%u", code);
			tsm_color_table_rgb(vte->palette, code, &cr, &cg, &cb);
			vte_write_xcolor(vte, buf, end_seq, cr, cg, cb);
		} else {
			// parsing of new rgb value to assign to code would go
			// here. For now, skip past field contents.
			while (*c && *c != ';') {
				c++;
			}
		}
		if (*c++ != ';') {
			break;
		}
	}
}

static bool do_osc_internal(struct tsm_vte *vte, const char *end_seq)
{
	struct tsm_screen_attr def;

	// OSC 4, 10, and 11 query or change the RGB values of color numbers in
	// the foreground/background (SGR 38 & 48), 3-bit (SGR 30-37 & 40-47),
	// 4-bit (SGR 90-97 & 100-107), and 256-color (SGR 38;5 & 48;5) indexed
	// color tables. We don't support changing the tables, but we do support
	// querying them.
	if (!strncmp(vte->osc_arg, "4;", 2)) {
		do_osc_4(vte, vte->osc_arg + 2, end_seq);
		return true;
	}
	if (!strncmp(vte->osc_arg, "10;?", 4)) {
		cell_attr_resolve(vte->palette, &vte->def_attr, &def);
		vte_write_xcolor(vte, "10", end_seq, def.fr, def.fg, def.fb);
		return true;
	}
	if (!strncmp(vte->osc_arg, "11;?", 4)) {
		cell_attr_resolve(vte->palette, &vte->def_attr, &def);
		vte_write_xcolor(vte, "11", end_seq, def.br, def.bg, def.bb);
		return true;
	}
	return false;
}

/* XTerm OSC responses use the same sequence terminator used in the query */
static const char *osc_end_seq(uint32_t end)
{
	if (end == 0x07) {
		return "\x07";
	}
	return "\x1b\x5c";
}

static void do_osc_end(struct tsm_vte *vte, uint32_t val) {
	const char *end_seq = osc_end_seq(val);
	vte->osc_arg[vte->osc_len] = 0;

	if (do_osc_internal(vte, end_seq)) {
		return;
	}
	if (!vte->osc_cb) {
		return;
	}
	vte->osc_cb(vte, vte->osc_arg, vte->osc_len, vte->osc_data);
}

/* perform parser action */
static void do_action(struct tsm_vte *vte, uint32_t data, int action)
{
	tsm_symbol_t sym;

	switch (action) {
		case ACTION_NONE:
			/* do nothing */
			return;
		case ACTION_IGNORE:
			/* ignore character */
			break;
		case ACTION_PRINT:
			sym = tsm_symbol_make(vte_map(vte, data));
			write_console(vte, sym);
			break;
		case ACTION_EXECUTE:
			do_execute(vte, data);
			break;
		case ACTION_CLEAR:
			do_clear(vte);
			break;
		case ACTION_COLLECT:
			do_collect(vte, data);
			break;
		case ACTION_PARAM:
			do_param(vte, data);
			break;
		case ACTION_ESC_DISPATCH:
			do_esc(vte, data);
			break;
		case ACTION_CSI_DISPATCH:
			do_csi(vte, data);
			break;
		case ACTION_DCS_START:
			do_dcs_start(vte, data);
			break;
		case ACTION_DCS_COLLECT:
			do_dcs_collect(vte, data);
			break;
		case ACTION_DCS_END:
			do_dcs_end(vte, data);
			break;
		case ACTION_OSC_START:
			do_clear(vte);
			break;
		case ACTION_OSC_COLLECT:
			do_osc_collect(vte, data);
			break;
		case ACTION_OSC_END:
			do_osc_end(vte, data);
			break;
		case ACTION_APC_START:
			do_apc_start(vte);
			break;
		case ACTION_APC_COLLECT:
			do_apc_collect(vte, data);
			break;
		case ACTION_APC_END:
			do_apc_end(vte, data);
			break;
		default:
			llog_warning(vte, "invalid action %d", action);
	}
}

/* entry actions to be performed when entering the selected state */
static const int entry_action[] = {
	[STATE_CSI_ENTRY] = ACTION_CLEAR,
	[STATE_DCS_ENTRY] = ACTION_CLEAR,
	[STATE_DCS_PASS] = ACTION_DCS_START,
	[STATE_ESC] = ACTION_CLEAR,
	[STATE_OSC_STRING] = ACTION_OSC_START,
	[STATE_APC_STRING] = ACTION_APC_START,
	[STATE_NUM] = ACTION_NONE,
};

/* exit actions to be performed when leaving the selected state */
static const int exit_action[] = {
	[STATE_DCS_PASS] = ACTION_DCS_END,
	[STATE_OSC_STRING] = ACTION_OSC_END,
	[STATE_APC_STRING] = ACTION_APC_END,
	[STATE_NUM] = ACTION_NONE,
};

/* perform state transition and dispatch related actions */
static void do_trans(struct tsm_vte *vte, uint32_t data, int state, int act)
{
	if (state != STATE_NONE) {
		/* A state transition occurs. Perform exit-action,
		 * transition-action and entry-action. Even when performing a
		 * transition to the same state as the current state we do this.
		 * Use STATE_NONE if this is not the desired behavior.
		 */
		do_action(vte, data, exit_action[vte->state]);
		do_action(vte, data, act);
		do_action(vte, data, entry_action[state]);
		vte->state = state;
	} else {
		do_action(vte, data, act);
	}
}

/*
 * Escape sequence parser
 * This parses the new input character \data. It performs state transition and
 * calls the right callbacks for each action.
 */
static void parse_data(struct tsm_vte *vte, uint32_t raw)
{
	/* events that may occur in any state */
	switch (raw) {
		case 0x18:
		case 0x1a:
		case 0x80 ... 0x8f:
		case 0x91 ... 0x97:
		case 0x99:
		case 0x9a:
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_EXECUTE);
			return;
		case 0x1b:
			do_trans(vte, raw, STATE_ESC, ACTION_NONE);
			return;
		case 0x98:
		case 0x9e:
			do_trans(vte, raw, STATE_ST_IGNORE, ACTION_NONE);
			return;
		case 0x9f:
			do_trans(vte, raw, STATE_APC_STRING, ACTION_NONE);
			return;
		case 0x90:
			do_trans(vte, raw, STATE_DCS_ENTRY, ACTION_NONE);
			return;
		case 0x9d:
			do_trans(vte, raw, STATE_OSC_STRING, ACTION_NONE);
			return;
		case 0x9b:
			do_trans(vte, raw, STATE_CSI_ENTRY, ACTION_NONE);
			return;
	}

	/* events that depend on the current state */
	switch (vte->state) {
	case STATE_GROUND:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x80 ... 0x8f:
		case 0x91 ... 0x9a:
		case 0x9c:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x20 ... 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_PRINT);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_PRINT);
		return;
	case STATE_ESC:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_ESC_INT, ACTION_COLLECT);
			return;
		case 0x30 ... 0x4f:
		case 0x51 ... 0x57:
		case 0x59:
		case 0x5a:
		case 0x5c:
		case 0x60 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_ESC_DISPATCH);
			return;
		case 0x5b:
			do_trans(vte, raw, STATE_CSI_ENTRY, ACTION_NONE);
			return;
		case 0x5d:
			do_trans(vte, raw, STATE_OSC_STRING, ACTION_NONE);
			return;
		case 0x50:
			do_trans(vte, raw, STATE_DCS_ENTRY, ACTION_NONE);
			return;
		case 0x58:
		case 0x5e:
			do_trans(vte, raw, STATE_ST_IGNORE, ACTION_NONE);
			return;
		case 0x5f:
			do_trans(vte, raw, STATE_APC_STRING, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_ESC_INT, ACTION_COLLECT);
		return;
	case STATE_ESC_INT:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x30 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_ESC_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
		return;
	case STATE_CSI_ENTRY:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_CSI_INT, ACTION_COLLECT);
			return;
		case 0x30 ... 0x3b:
			do_trans(vte, raw, STATE_CSI_PARAM, ACTION_PARAM);
			return;
		case 0x3c ... 0x3f:
			do_trans(vte, raw, STATE_CSI_PARAM, ACTION_COLLECT);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_CSI_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
		return;
	case STATE_CSI_PARAM:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x30 ... 0x3b:
			do_trans(vte, raw, STATE_NONE, ACTION_PARAM);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x3c ... 0x3f:
			do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_CSI_INT, ACTION_COLLECT);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_CSI_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
		return;
	case STATE_CSI_INT:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x30 ... 0x3f:
			do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_CSI_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
		return;
	case STATE_CSI_IGNORE:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x20 ... 0x3f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
		return;
	case STATE_DCS_ENTRY:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x3a:
			do_trans(vte, raw, STATE_DCS_IGNORE, ACTION_NONE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_DCS_INT, ACTION_COLLECT);
			return;
		case 0x30 ... 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_DCS_PARAM, ACTION_PARAM);
			return;
		case 0x3c ... 0x3f:
			do_trans(vte, raw, STATE_DCS_PARAM, ACTION_COLLECT);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
		return;
	case STATE_DCS_PARAM:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x30 ... 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_NONE, ACTION_PARAM);
			return;
		case 0x3a:
		case 0x3c ... 0x3f:
			do_trans(vte, raw, STATE_DCS_IGNORE, ACTION_NONE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_DCS_INT, ACTION_COLLECT);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
		return;
	case STATE_DCS_INT:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
			return;
		case 0x30 ... 0x3f:
			do_trans(vte, raw, STATE_DCS_IGNORE, ACTION_NONE);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
		return;
	case STATE_DCS_PASS:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x20 ... 0x7e:
			do_trans(vte, raw, STATE_NONE, ACTION_DCS_COLLECT);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_DCS_COLLECT);
		return;
	case STATE_DCS_IGNORE:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x20 ... 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
		return;
	case STATE_OSC_STRING:
		switch (raw) {
		case 0x00 ... 0x06:
		case 0x08 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_OSC_COLLECT);
			return;
		case 0x07:
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_OSC_COLLECT);
		return;
	case STATE_ST_IGNORE:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x20 ... 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
		return;
	case STATE_APC_STRING:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x7e:
			do_trans(vte, raw, STATE_NONE, ACTION_APC_COLLECT);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_APC_COLLECT);
		return;
	}

	llog_warning(vte, "unhandled input %u in state %d", raw, vte->state);
}

/* a byte that DCS passthrough hands the decoder as it is, in every mode */
static inline bool sixel_byte(unsigned char c)
{
	return c < 0x7f && c != 0x18 && c != 0x1a && c != 0x1b;
}

/*
 * Feeds the run of sixel bytes at @u8 straight to the decoder, as the parser
 * would hand them over one at a time; returns how many it took.
 */
static size_t sixel_run(struct tsm_vte *vte, const char *u8, size_t len)
{
	uint64_t work = sixel_work(vte), start = tsm_sixel_work(vte->sixel), limit;
	size_t n;

	for (n = 1; n < len && sixel_byte(u8[n]); ++n)
		;
	limit = start + vte->work_left < start ? UINT64_MAX : start + vte->work_left;
	n = tsm_sixel_feed_some(vte->sixel, u8, n, limit);
	sixel_charge(vte, sixel_work(vte) - work);
	/* the UTF-8 machine ends where those bytes would have left it */
	if (!(vte->flags & (TSM_VTE_FLAG_7BIT_MODE | TSM_VTE_FLAG_8BIT_MODE)))
		tsm_utf8_mach_feed(vte->mach, u8[n - 1]);
	return n;
}

/* a byte a kitty payload's run takes as it is, in every mode */
static inline bool kitty_byte(unsigned char c)
{
	return c >= 0x20 && c < 0x7f;
}

/*
 * Feeds the run of payload bytes at @u8 straight to the kitty parser, as the
 * parser would hand them over one at a time, at most as many as the work
 * left allows (each byte is a unit of work); returns how many it took.
 */
static size_t kitty_run(struct tsm_vte *vte, const char *u8, size_t len)
{
	uint64_t work = sixel_work(vte);
	size_t n;

	for (n = 1; n < len && n < vte->work_left && kitty_byte(u8[n]); ++n)
		;
	tsm_kitty_feed_run(vte->kitty, u8, n);
	sixel_charge(vte, sixel_work(vte) - work);
	if (!(vte->flags & (TSM_VTE_FLAG_7BIT_MODE | TSM_VTE_FLAG_8BIT_MODE)))
		tsm_utf8_mach_feed(vte->mach, u8[n - 1]);
	return n;
}

/* parses until @len bytes are read, the image work runs out, a kitty
 * command is handed to the host hook, or, with sync_stop, a hold ends */
static size_t vte_input(struct tsm_vte *vte, const char *u8, size_t len)
{
	uint64_t frames = vte->sync_frames;
	int state;
	uint32_t ucs4;
	size_t i;

	for (i = 0; i < len; ) {
		if (vte->sixel_active && vte->state == STATE_DCS_PASS &&
		    sixel_byte(u8[i]) &&
		    ((vte->flags & (TSM_VTE_FLAG_7BIT_MODE | TSM_VTE_FLAG_8BIT_MODE)) ||
		     tsm_utf8_mach_idle(vte->mach))) {
			i += sixel_run(vte, u8 + i, len - i);
		} else if (vte->apc_kitty && vte->state == STATE_APC_STRING &&
			   kitty_byte(u8[i]) && tsm_kitty_in_payload(vte->kitty) &&
			   ((vte->flags & (TSM_VTE_FLAG_7BIT_MODE | TSM_VTE_FLAG_8BIT_MODE)) ||
			    tsm_utf8_mach_idle(vte->mach))) {
			i += kitty_run(vte, u8 + i, len - i);
		} else if (vte->flags & TSM_VTE_FLAG_7BIT_MODE) {
			if (u8[i] & 0x80)
				llog_debug(vte, "receiving 8bit character U+%d from pty while in 7bit mode",
					   (int)u8[i]);
			parse_data(vte, u8[i++] & 0x7f);
		} else if (vte->flags & TSM_VTE_FLAG_8BIT_MODE) {
			parse_data(vte, (uint8_t)u8[i++]);
		} else {
			state = tsm_utf8_mach_feed(vte->mach, u8[i++]);
			if (state == TSM_UTF8_ACCEPT ||
			    state == TSM_UTF8_REJECT) {
				ucs4 = tsm_utf8_mach_get(vte->mach);
				parse_data(vte, ucs4);
			}
		}
		if (!vte->work_left || tsm_kitty_paused(vte->kitty) ||
		    (vte->sync_stop && vte->sync_frames != frames))
			break;
	}
	return i;
}

/*
 * runs vte_input with @work to spend, as one call; with @bounded a kitty
 * command may go to the host hook, and the call ends there or at the end of
 * a hold (mode 2026)
 */
static size_t vte_input_with(struct tsm_vte *vte, const char *u8, size_t len,
			     uint64_t work, bool bounded)
{
	uint64_t saved = vte->work_left;
	bool saved_stop = vte->sync_stop;
	bool saved_host = tsm_kitty_set_host(vte->kitty, bounded);
	size_t n;

	++vte->parse_cnt;
	vte->work_left = work;
	vte->sync_stop = bounded;
	n = vte_input(vte, u8, len);
	/* what the call's cuts and scrolls owe the budget */
	screen_images_settle(vte->con);
	vte->work_left = saved;
	vte->sync_stop = saved_stop;
	tsm_kitty_set_host(vte->kitty, saved_host);
	--vte->parse_cnt;
	return n;
}

SHL_EXPORT
void tsm_vte_input(struct tsm_vte *vte, const char *u8, size_t len)
{
	if (!vte || !vte->con)
		return;

	/* the host feeds nothing while its request is out, nor from inside
	 * the hook */
	if (tsm_kitty_paused(vte->kitty) || tsm_kitty_in_hook(vte->kitty)) {
		if (len)
			llog_warning(vte, "%zu bytes dropped: a kitty image request is out",
				     len);
		return;
	}
	vte_input_with(vte, u8, len, UINT64_MAX, false);
}

SHL_EXPORT
size_t tsm_vte_input_bounded(struct tsm_vte *vte, const char *u8, size_t len)
{
	if (!vte || !vte->con)
		return len;

	if (tsm_kitty_paused(vte->kitty) || tsm_kitty_in_hook(vte->kitty))
		return 0;
	return vte_input_with(vte, u8, len, VTE_INPUT_WORK, true);
}

SHL_EXPORT
void tsm_vte_set_image_cb(struct tsm_vte *vte, tsm_vte_image_cb image_cb,
			  void *image_data)
{
	if (!vte)
		return;

	++vte->parse_cnt;
	tsm_kitty_set_hook(vte->kitty, image_cb, image_data);
	--vte->parse_cnt;
}

SHL_EXPORT
int tsm_vte_image_reply(struct tsm_vte *vte, uint64_t id,
			const struct tsm_vte_image_result *res)
{
	int ret;

	if (!vte)
		return -EINVAL;

	/* the client's reply is written as one to its input */
	++vte->parse_cnt;
	ret = tsm_kitty_reply(vte->kitty, id, res);
	screen_images_settle(vte->con);
	--vte->parse_cnt;
	return ret;
}

SHL_EXPORT
void tsm_vte_set_sixel(struct tsm_vte *vte, bool enable)
{
	if (!vte)
		return;

	vte->sixel_enabled = enable;
	if (!enable)
		sixel_drop(vte);
}

SHL_EXPORT
int tsm_vte_set_graphic_max_size(struct tsm_vte *vte, unsigned int width,
				 unsigned int height)
{
	if (!vte)
		return -EINVAL;

	vte->graphic_max_w = !width ? TSM_SIXEL_MAX_SIZE_DEFAULT :
			     width > TSM_SCREEN_IMAGE_MAX ? TSM_SCREEN_IMAGE_MAX : width;
	vte->graphic_max_h = !height ? TSM_SIXEL_MAX_SIZE_DEFAULT :
			     height > TSM_SCREEN_IMAGE_MAX ? TSM_SCREEN_IMAGE_MAX : height;
	sixel_drop(vte);
	tsm_kitty_set_max_size(vte->kitty, vte->graphic_max_w, vte->graphic_max_h);
	return 0;
}

SHL_EXPORT
void tsm_vte_set_kitty_graphics(struct tsm_vte *vte, bool enable)
{
	if (!vte)
		return;

	vte->kitty_enabled = enable;
	if (!enable) {
		tsm_kitty_cancel(vte->kitty);
		vte->apc_kitty = false;
	}
}

SHL_EXPORT
void tsm_vte_set_cursor_blink(struct tsm_vte *vte, bool enable)
{
	if (!vte)
		return;

	vte->cursor_blink = enable;
}

SHL_EXPORT
void tsm_vte_set_sync(struct tsm_vte *vte, bool enable)
{
	if (!vte)
		return;

	vte->sync_enabled = enable;
	if (!enable)
		sync_end(vte);
}

SHL_EXPORT
uint64_t tsm_vte_get_sync_frames(struct tsm_vte *vte)
{
	if (!vte)
		return 0;

	return vte->sync_frames;
}

SHL_EXPORT
void tsm_vte_end_sync(struct tsm_vte *vte)
{
	if (!vte)
		return;

	sync_end(vte);
}

SHL_EXPORT
int tsm_vte_set_sixel_registers(struct tsm_vte *vte, unsigned int count,
				bool private_registers)
{
	if (!vte || (count && (count < TSM_SIXEL_REGISTERS_MIN ||
			       count > TSM_SIXEL_REGISTERS_MAX)))
		return -EINVAL;

	vte->sixel_max_registers = count ? count : TSM_SIXEL_REGISTERS_DEFAULT;
	vte->sixel_private_default = private_registers;
	sixel_reset(vte);
	return 0;
}

SHL_EXPORT
int tsm_vte_set_width_method(struct tsm_vte *vte, enum tsm_width_method method)
{
	if (!vte || (method != TSM_WIDTH_UNICODE && method != TSM_WIDTH_WCWIDTH))
		return -EINVAL;

	vte->width_method = method;
	screen_set_width_method(vte->con, method);
	return 0;
}

SHL_EXPORT
void tsm_vte_set_backspace_sends_delete(struct tsm_vte *vte, bool enable)
{
	vte->backspace_sends_delete = enable;
}

/*
Code     Modifiers
---------+---------------------------
   2     | Shift
   3     | Alt
   4     | Shift + Alt
   5     | Control
   6     | Shift + Control
   7     | Alt + Control
   8     | Shift + Alt + Control
---------+---------------------------
*/

static char csi_modifier_from_mask(unsigned int mods)
{
	mods = mods & (TSM_CONTROL_MASK | TSM_SHIFT_MASK | TSM_ALT_MASK);
	switch(mods) {
		case TSM_SHIFT_MASK:
			return '2';
		case TSM_ALT_MASK:
			return '3';
		case TSM_SHIFT_MASK | TSM_ALT_MASK:
			return '4';
		case TSM_CONTROL_MASK:
			return '5';
		case TSM_CONTROL_MASK | TSM_SHIFT_MASK:
			return '6';
		case TSM_CONTROL_MASK | TSM_ALT_MASK:
			return '7';
		case TSM_CONTROL_MASK | TSM_ALT_MASK | TSM_SHIFT_MASK:
			return '8';
		default:
			return 0;
	}
}

static void vte_write_arrow(struct tsm_vte *vte, char direction, unsigned int mods)
{
	char code_with_modifier[6] = {'\e', '[', '1', ';', '0', 'A'};
	char code[3] = {'\e', '[',  'A'};

	vte->flags &= ~TSM_VTE_FLAG_PREPEND_ESCAPE;

	int modifier = csi_modifier_from_mask(mods);
	if (modifier) {
		code_with_modifier[4] = modifier;
		code_with_modifier[5] = direction;
		vte_write(vte, code_with_modifier, 6);
	} else {
		if (vte->flags & TSM_VTE_FLAG_CURSOR_KEY_MODE)
			code[1] = 'O';
		code[2] = direction;
		vte_write(vte, code, 3);
	}
}

SHL_EXPORT
bool tsm_vte_handle_keyboard(struct tsm_vte *vte, uint32_t keysym,
			     uint32_t ascii, unsigned int mods,
			     uint32_t unicode)
{
	char val, u8[4];
	size_t len;
	uint32_t sym;

	if (!vte) {
		return false;
	}

	/* MOD1 (mostly labeled 'Alt') prepends an escape character to every
	 * input that is sent by a key.
	 * TODO: Transform this huge handler into a lookup table to save a lot
	 * of code and make such modifiers easier to implement.
	 * Also check whether altSendsEscape should be the default (xterm
	 * disables this by default, why?) and whether we should implement the
	 * fallback shifting that xterm does. */
	if (mods & TSM_ALT_MASK)
		vte->flags |= TSM_VTE_FLAG_PREPEND_ESCAPE;

	/* A user might actually use multiple layouts for keyboard input. The
	 * @keysym variable contains the actual keysym that the user used. But
	 * if this keysym is not in the ascii range, the input handler does
	 * check all other layouts that the user specified whether one of them
	 * maps the key to some ASCII keysym and provides this via @ascii.
	 * We always use the real keysym except when handling CTRL+<XY>
	 * shortcuts we use the ascii keysym. This is for compatibility to xterm
	 * et. al. so ctrl+c always works regardless of the currently active
	 * keyboard layout.
	 * But if no ascii-sym is found, we still use the real keysym. */
	sym = ascii;
	if (sym == XKB_KEY_NoSymbol)
		sym = keysym;

	if (mods & TSM_CONTROL_MASK) {
		switch (sym) {
		case XKB_KEY_2:
		case XKB_KEY_space:
			vte_write(vte, "\x00", 1);
			return true;
		case XKB_KEY_a:
		case XKB_KEY_A:
			vte_write(vte, "\x01", 1);
			return true;
		case XKB_KEY_b:
		case XKB_KEY_B:
			vte_write(vte, "\x02", 1);
			return true;
		case XKB_KEY_c:
		case XKB_KEY_C:
			vte_write(vte, "\x03", 1);
			return true;
		case XKB_KEY_d:
		case XKB_KEY_D:
			vte_write(vte, "\x04", 1);
			return true;
		case XKB_KEY_e:
		case XKB_KEY_E:
			vte_write(vte, "\x05", 1);
			return true;
		case XKB_KEY_f:
		case XKB_KEY_F:
			vte_write(vte, "\x06", 1);
			return true;
		case XKB_KEY_g:
		case XKB_KEY_G:
			vte_write(vte, "\x07", 1);
			return true;
		case XKB_KEY_h:
		case XKB_KEY_H:
			vte_write(vte, "\x08", 1);
			return true;
		case XKB_KEY_i:
		case XKB_KEY_I:
			vte_write(vte, "\x09", 1);
			return true;
		case XKB_KEY_j:
		case XKB_KEY_J:
			vte_write(vte, "\x0a", 1);
			return true;
		case XKB_KEY_k:
		case XKB_KEY_K:
			vte_write(vte, "\x0b", 1);
			return true;
		case XKB_KEY_l:
		case XKB_KEY_L:
			vte_write(vte, "\x0c", 1);
			return true;
		case XKB_KEY_m:
		case XKB_KEY_M:
			vte_write(vte, "\x0d", 1);
			return true;
		case XKB_KEY_n:
		case XKB_KEY_N:
			vte_write(vte, "\x0e", 1);
			return true;
		case XKB_KEY_o:
		case XKB_KEY_O:
			vte_write(vte, "\x0f", 1);
			return true;
		case XKB_KEY_p:
		case XKB_KEY_P:
			vte_write(vte, "\x10", 1);
			return true;
		case XKB_KEY_q:
		case XKB_KEY_Q:
			vte_write(vte, "\x11", 1);
			return true;
		case XKB_KEY_r:
		case XKB_KEY_R:
			vte_write(vte, "\x12", 1);
			return true;
		case XKB_KEY_s:
		case XKB_KEY_S:
			vte_write(vte, "\x13", 1);
			return true;
		case XKB_KEY_t:
		case XKB_KEY_T:
			vte_write(vte, "\x14", 1);
			return true;
		case XKB_KEY_u:
		case XKB_KEY_U:
			vte_write(vte, "\x15", 1);
			return true;
		case XKB_KEY_v:
		case XKB_KEY_V:
			vte_write(vte, "\x16", 1);
			return true;
		case XKB_KEY_w:
		case XKB_KEY_W:
			vte_write(vte, "\x17", 1);
			return true;
		case XKB_KEY_x:
		case XKB_KEY_X:
			vte_write(vte, "\x18", 1);
			return true;
		case XKB_KEY_y:
		case XKB_KEY_Y:
			vte_write(vte, "\x19", 1);
			return true;
		case XKB_KEY_z:
		case XKB_KEY_Z:
			vte_write(vte, "\x1a", 1);
			return true;
		case XKB_KEY_3:
		case XKB_KEY_bracketleft:
		case XKB_KEY_braceleft:
			vte_write(vte, "\x1b", 1);
			return true;
		case XKB_KEY_4:
		case XKB_KEY_backslash:
		case XKB_KEY_bar:
			vte_write(vte, "\x1c", 1);
			return true;
		case XKB_KEY_5:
		case XKB_KEY_bracketright:
		case XKB_KEY_braceright:
			vte_write(vte, "\x1d", 1);
			return true;
		case XKB_KEY_6:
		case XKB_KEY_grave:
		case XKB_KEY_asciitilde:
			vte_write(vte, "\x1e", 1);
			return true;
		case XKB_KEY_7:
		case XKB_KEY_slash:
		case XKB_KEY_question:
			vte_write(vte, "\x1f", 1);
			return true;
		case XKB_KEY_8:
			vte_write(vte, "\x7f", 1);
			return true;
		}
	}

	switch (keysym) {
		case XKB_KEY_BackSpace:
			if (vte->backspace_sends_delete)
				vte_write(vte, "\x7f", 1);
			else
				vte_write(vte, "\x08", 1);
			return true;
		case XKB_KEY_Tab:
		case XKB_KEY_KP_Tab:
			vte_write(vte, "\x09", 1);
			return true;
		case XKB_KEY_ISO_Left_Tab:
			vte_write(vte, "\e[Z", 3);
			return true;
		case XKB_KEY_Linefeed:
			vte_write(vte, "\x0a", 1);
			return true;
		case XKB_KEY_Clear:
			vte_write(vte, "\x0b", 1);
			return true;
		/*
		 TODO: What should we do with this key? Sending XOFF is awful as
		       there is no simple way on modern keyboards to send XON
		       again. If someone wants this, we can re-eanble it and set
		       some flag.
		case XKB_KEY_Pause:
			vte_write(vte, "\x13", 1);
			return true;
		*/
		/*
		 TODO: What should we do on scroll-lock? Sending 0x14 is what
		       the specs say but it is not used today the way most
		       users would expect so we disable it. If someone wants
		       this, we can re-enable it and set some flag.
		case XKB_KEY_Scroll_Lock:
			vte_write(vte, "\x14", 1);
			return true;
		*/
		case XKB_KEY_Sys_Req:
			vte_write(vte, "\x15", 1);
			return true;
		case XKB_KEY_Escape:
			vte_write(vte, "\x1b", 1);
			return true;
		case XKB_KEY_KP_Enter:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE) {
				vte_write(vte, "\eOM", 3);
				return true;
			}
			/* fallthrough */
		case XKB_KEY_Return:
			if (vte->flags & TSM_VTE_FLAG_LINE_FEED_NEW_LINE_MODE)
				vte_write(vte, "\x0d\x0a", 2);
			else
				vte_write(vte, "\x0d", 1);
			return true;
		case XKB_KEY_Find:
			vte_write(vte, "\e[1~", 4);
			return true;
		case XKB_KEY_Insert:
			vte_write(vte, "\e[2~", 4);
			return true;
		case XKB_KEY_Delete:
			vte_write(vte, "\e[3~", 4);
			return true;
		case XKB_KEY_Select:
			vte_write(vte, "\e[4~", 4);
			return true;
		case XKB_KEY_Page_Up:
		case XKB_KEY_KP_Page_Up:
			/* Combinations with ALT are handled as TSM_VTE_FLAG_PREPEND_ESCAPE flag */
			if ((mods & (TSM_CONTROL_MASK | TSM_SHIFT_MASK)) == (TSM_CONTROL_MASK | TSM_SHIFT_MASK)) {
				vte_write(vte, "\e[5;6~", 6);
			} else if (mods & TSM_CONTROL_MASK) {
				vte_write(vte, "\e[5;5~", 6);
			} else if (mods & TSM_SHIFT_MASK) {
				vte_write(vte, "\e[5;2~", 6);
			} else {
				vte_write(vte, "\e[5~", 4);
			}
			return true;
		case XKB_KEY_KP_Page_Down:
		case XKB_KEY_Page_Down:
			/* Combinations with ALT are handled as TSM_VTE_FLAG_PREPEND_ESCAPE flag */
			if ((mods & (TSM_CONTROL_MASK | TSM_SHIFT_MASK)) == (TSM_CONTROL_MASK | TSM_SHIFT_MASK)) {
				vte_write(vte, "\e[6;6~", 6);
			} else if (mods & TSM_CONTROL_MASK) {
				vte_write(vte, "\e[6;5~", 6);
			} else if (mods & TSM_SHIFT_MASK) {
				vte_write(vte, "\e[6;2~", 6);
			} else {
				vte_write(vte, "\e[6~", 4);
			}
			return true;
		case XKB_KEY_Up:
		case XKB_KEY_KP_Up:
			vte_write_arrow(vte, 'A', mods);
			return true;
		case XKB_KEY_Down:
		case XKB_KEY_KP_Down:
			vte_write_arrow(vte, 'B', mods);
			return true;
		case XKB_KEY_Right:
		case XKB_KEY_KP_Right:
			vte_write_arrow(vte, 'C', mods);
			return true;
		case XKB_KEY_Left:
		case XKB_KEY_KP_Left:
			vte_write_arrow(vte, 'D', mods);
			return true;
		case XKB_KEY_KP_Insert:
		case XKB_KEY_KP_0:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOp", 3);
			else
				vte_write(vte, "0", 1);
			return true;
		case XKB_KEY_KP_1:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOq", 3);
			else
				vte_write(vte, "1", 1);
			return true;
		case XKB_KEY_KP_2:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOr", 3);
			else
				vte_write(vte, "2", 1);
			return true;
		case XKB_KEY_KP_3:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOs", 3);
			else
				vte_write(vte, "3", 1);
			return true;
		case XKB_KEY_KP_4:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOt", 3);
			else
				vte_write(vte, "4", 1);
			return true;
		case XKB_KEY_KP_5:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOu", 3);
			else
				vte_write(vte, "5", 1);
			return true;
		case XKB_KEY_KP_6:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOv", 3);
			else
				vte_write(vte, "6", 1);
			return true;
		case XKB_KEY_KP_7:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOw", 3);
			else
				vte_write(vte, "7", 1);
			return true;
		case XKB_KEY_KP_8:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOx", 3);
			else
				vte_write(vte, "8", 1);
			return true;
		case XKB_KEY_KP_9:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOy", 3);
			else
				vte_write(vte, "9", 1);
			return true;
		case XKB_KEY_KP_Subtract:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOm", 3);
			else
				vte_write(vte, "-", 1);
			return true;
		case XKB_KEY_KP_Separator:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOl", 3);
			else
				vte_write(vte, ",", 1);
			return true;
		case XKB_KEY_KP_Delete:
		case XKB_KEY_KP_Decimal:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOn", 3);
			else
				vte_write(vte, ".", 1);
			return true;
		case XKB_KEY_KP_Equal:
		case XKB_KEY_KP_Divide:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOj", 3);
			else
				vte_write(vte, "/", 1);
			return true;
		case XKB_KEY_KP_Multiply:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOo", 3);
			else
				vte_write(vte, "*", 1);
			return true;
		case XKB_KEY_KP_Add:
			if (vte->flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOk", 3);
			else
				vte_write(vte, "+", 1);
			return true;
		case XKB_KEY_Home:
		case XKB_KEY_KP_Home:
			if (vte->flags & TSM_VTE_FLAG_CURSOR_KEY_MODE)
				vte_write(vte, "\eOH", 3);
			else
				vte_write(vte, "\e[H", 3);
			return true;
		case XKB_KEY_End:
		case XKB_KEY_KP_End:
			if (vte->flags & TSM_VTE_FLAG_CURSOR_KEY_MODE)
				vte_write(vte, "\eOF", 3);
			else
				vte_write(vte, "\e[F", 3);
			return true;
		case XKB_KEY_KP_Space:
			vte_write(vte, " ", 1);
			return true;
		/* TODO: check what to transmit for functions keys when
		 * shift/ctrl etc. are pressed. Every terminal behaves
		 * differently here which is really weird.
		 * We now map F4 to F14 if shift is pressed and so on for all
		 * keys. However, such mappings should rather be done via
		 * xkb-configurations and we should instead add a flags argument
		 * to the CSIs as some of the keys here already do. */
		case XKB_KEY_F1:
		case XKB_KEY_KP_F1:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[23~", 5);
			else
				vte_write(vte, "\eOP", 3);
			return true;
		case XKB_KEY_F2:
		case XKB_KEY_KP_F2:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[24~", 5);
			else
				vte_write(vte, "\eOQ", 3);
			return true;
		case XKB_KEY_F3:
		case XKB_KEY_KP_F3:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[25~", 5);
			else
				vte_write(vte, "\eOR", 3);
			return true;
		case XKB_KEY_F4:
		case XKB_KEY_KP_F4:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, "\e[1;2S", 6);
				vte_write(vte, "\e[26~", 5);
			else
				vte_write(vte, "\eOS", 3);
			return true;
		case XKB_KEY_F5:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, "\e[15;2~", 7);
				vte_write(vte, "\e[28~", 5);
			else
				vte_write(vte, "\e[15~", 5);
			return true;
		case XKB_KEY_F6:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, "\e[17;2~", 7);
				vte_write(vte, "\e[29~", 5);
			else
				vte_write(vte, "\e[17~", 5);
			return true;
		case XKB_KEY_F7:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, "\e[18;2~", 7);
				vte_write(vte, "\e[31~", 5);
			else
				vte_write(vte, "\e[18~", 5);
			return true;
		case XKB_KEY_F8:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, "\e[19;2~", 7);
				vte_write(vte, "\e[32~", 5);
			else
				vte_write(vte, "\e[19~", 5);
			return true;
		case XKB_KEY_F9:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, "\e[20;2~", 7);
				vte_write(vte, "\e[33~", 5);
			else
				vte_write(vte, "\e[20~", 5);
			return true;
		case XKB_KEY_F10:
			if (mods & TSM_SHIFT_MASK)
				//vte_write(vte, "\e[21;2~", 7);
				vte_write(vte, "\e[34~", 5);
			else
				vte_write(vte, "\e[21~", 5);
			return true;
		case XKB_KEY_F11:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[23;2~", 7);
			else
				vte_write(vte, "\e[23~", 5);
			return true;
		case XKB_KEY_F12:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[24;2~", 7);
			else
				vte_write(vte, "\e[24~", 5);
			return true;
		case XKB_KEY_F13:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[25;2~", 7);
			else
				vte_write(vte, "\e[25~", 5);
			return true;
		case XKB_KEY_F14:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[26;2~", 7);
			else
				vte_write(vte, "\e[26~", 5);
			return true;
		case XKB_KEY_F15:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[28;2~", 7);
			else
				vte_write(vte, "\e[28~", 5);
			return true;
		case XKB_KEY_F16:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[29;2~", 7);
			else
				vte_write(vte, "\e[29~", 5);
			return true;
		case XKB_KEY_F17:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[31;2~", 7);
			else
				vte_write(vte, "\e[31~", 5);
			return true;
		case XKB_KEY_F18:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[32;2~", 7);
			else
				vte_write(vte, "\e[32~", 5);
			return true;
		case XKB_KEY_F19:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[33;2~", 7);
			else
				vte_write(vte, "\e[33~", 5);
			return true;
		case XKB_KEY_F20:
			if (mods & TSM_SHIFT_MASK)
				vte_write(vte, "\e[34;2~", 7);
			else
				vte_write(vte, "\e[34~", 5);
			return true;
	}

	if (unicode != TSM_VTE_INVALID) {
		if (vte->flags & TSM_VTE_FLAG_7BIT_MODE) {
			val = unicode;
			if (unicode & 0x80) {
				llog_debug(vte, "invalid keyboard input in 7bit mode U+%x; mapping to '?'",
					   unicode);
				val = '?';
			}
			vte_write(vte, &val, 1);
		} else if (vte->flags & TSM_VTE_FLAG_8BIT_MODE) {
			val = unicode;
			if (unicode > 0xff) {
				llog_debug(vte, "invalid keyboard input in 8bit mode U+%x; mapping to '?'",
					   unicode);
				val = '?';
			}
			vte_write_raw(vte, &val, 1);
		} else {
			len = tsm_ucs4_to_utf8(tsm_symbol_make(unicode), u8);
			vte_write_raw(vte, u8, len);
		}
		return true;
	}

	vte->flags &= ~TSM_VTE_FLAG_PREPEND_ESCAPE;
	return false;
}

bool tsm_vte_handle_mouse(struct tsm_vte *vte, unsigned int cell_x,
		unsigned int cell_y, unsigned int pixel_x, unsigned int pixel_y,
		unsigned int button, unsigned int event, unsigned char modifiers)
{
	char buffer[48];
	unsigned int cb, x, y;
	bool moved = event & TSM_MOUSE_EVENT_MOVED;
	bool released = event & TSM_MOUSE_EVENT_RELEASED;
	/* the host passes motion with a button down as the button plus 32 */
	bool drag = button >= 32 && button <= 34;

	if (!vte || !vte->mouse_track)
		return false;

	/* 9 reports presses, 1000 releases too, 1002 drags too, and 1003
	 * all motion */
	if (moved && (vte->mouse_track == TSM_VTE_MOUSE_MODE_X10 ||
		      vte->mouse_track == TSM_VTE_MOUSE_MODE_VT200 ||
		      (vte->mouse_track == TSM_VTE_MOUSE_EVENT_BTN && !drag)))
		return false;
	if (released && vte->mouse_track == TSM_VTE_MOUSE_MODE_X10)
		return false;

	if (button == TSM_MOUSE_BUTTON_WHEEL_UP)
		button = 64;
	else if (button == TSM_MOUSE_BUTTON_WHEEL_DOWN)
		button = 65;
	/* a wheel has no release to report */
	if (released && button >= 64)
		return false;

	/* X10 has codes for three buttons and no modifiers */
	if (vte->mouse_track == TSM_VTE_MOUSE_MODE_X10) {
		if (button >= 64)
			return false;
		modifiers = 0;
	}

	/* motion is reported when it enters a cell other than the last one
	 * reported, by any event; in pixels, whenever it moves */
	if (vte->mouse_encoding != TSM_VTE_MOUSE_MODE_PIXEL) {
		if (moved && cell_x + 1 == vte->mouse_last_col &&
		    cell_y + 1 == vte->mouse_last_row)
			return false;
		vte->mouse_last_col = cell_x + 1;
		vte->mouse_last_row = cell_y + 1;
	}
	/* motion with no button down is button 3, a release, plus 32 */
	if (moved && !drag)
		button = 35;
	cb = button | modifiers;

	switch (vte->mouse_encoding) {
	case TSM_VTE_MOUSE_MODE_SGR:
		/* the top left cell is 1,1, and a release ends in m */
		snprintf(buffer, sizeof(buffer), "\e[<%u;%u;%u%c", cb, cell_x + 1,
			 cell_y + 1, released ? 'm' : 'M');
		break;
	case TSM_VTE_MOUSE_MODE_PIXEL:
		snprintf(buffer, sizeof(buffer), "\e[<%u;%u;%u%c", cb, pixel_x, pixel_y,
			 released ? 'm' : 'M');
		break;
	default:
		/* one byte each, plus 32: a release is button 3, and the cell
		 * starts at 1,1 and stops at 223 */
		if (released)
			cb = 3 | modifiers;
		x = cell_x >= 0xff - 0x21 ? 0xff : cell_x + 0x21;
		y = cell_y >= 0xff - 0x21 ? 0xff : cell_y + 0x21;
		snprintf(buffer, sizeof(buffer), "\e[M%c%c%c", cb + 0x20, x, y);
		break;
	}

	vte_write(vte, buffer, strlen(buffer));
	return true;
}

void tsm_vte_paste(struct tsm_vte *vte, const char *data)
{
	if (!vte || !data)
		return;

	if (vte->bracketed_paste) {
		const char start[] = "\e[200~";
		const char end[] = "\e[201~";
		char *buf;
		int len;

		len = strlen(data) + sizeof(start) + sizeof(end) + 1;
		buf = malloc(len);
		if (!buf)
			return;
		snprintf(buf, len, "%s%s%s", start, data, end);
		vte_write(vte, buf, strlen(buf));
		free(buf);
	} else
		vte_write(vte, data, strlen(data));
}
