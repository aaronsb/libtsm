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
#include "tsm-version.h"
#include "tsm-vte-int.h"
#include "shl-llog.h"
#include "shl-macro.h"

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
#define CSI_OTHER	0x1000		/* CSI: any other intermediate or marker */

/* what the DCS string in progress is, picked at its start (vte->dcs) */
enum dcs_kind {
	DCS_IGNORED,		/* consumed with no effect */
	DCS_IMAGE,		/* sixel, the image glue's */
	DCS_XTGETTCAP,		/* DCS + q, answered here */
};

/* an XTGETTCAP reply's size */
#define XTGETTCAP_REPLY_MAX 4096

/* an entry of the host's XTGETTCAP table; value is NULL for a boolean */
struct vte_termcap {
	const char *name;
	const char *value;
	size_t value_len;
};

/* XTVERSION's text until the host sets one */
#define VTE_VERSION_DEFAULT "libtsm(" LIBTSM_VERSION ")"
shl_assert_cc(sizeof(VTE_VERSION_DEFAULT) <= TSM_VTE_VERSION_MAX + 1);

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
/* an ignored or refused row has no effect, but keeps its legacy flag for
 * tsm_vte_get_flags, upstream's API; MODE_FLAG_<name> is 0 for one without */
#define MODE_SET_IGNORED(name) set_reset_flag(vte, set, MODE_FLAG_##name)
#define MODE_SET_REFUSED(name) set_reset_flag(vte, set, MODE_FLAG_##name)
#define MODE_FLAG_KAM TSM_VTE_FLAG_KEYBOARD_ACTION_MODE
#define MODE_FLAG_DECANM 0
#define MODE_FLAG_DECCOLM 0
#define MODE_FLAG_DECSCLM 0
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
	HOST_SIXEL,
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
static void modes_reset(struct tsm_vte *vte, enum mode_reset reset);

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

	vte->cursor_blink = true;
	tsm_vte_set_version(vte, NULL);

	ret = tsm_utf8_mach_new(&vte->mach);
	if (ret)
		goto err_free;
	ret = vte_image_new(vte);
	if (ret)
		goto err_mach;
	/* modes 80, 1070 and 8452, the rows only RIS resets */
	modes_reset(vte, RESET_HARD);

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
	free(vte->termcap);
	/* a picture in progress gives its bytes back to the screen first */
	vte_image_free(vte);
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

SHL_EXPORT
int tsm_vte_set_version(struct tsm_vte *vte, const char *str)
{
	size_t i, len;

	if (!vte)
		return -EINVAL;
	if (!str)
		str = VTE_VERSION_DEFAULT;

	/* it goes to the client inside a DCS string, which a control or a
	 * byte past ASCII could end or break */
	len = strnlen(str, TSM_VTE_VERSION_MAX + 1);
	if (len > TSM_VTE_VERSION_MAX)
		return -EINVAL;
	for (i = 0; i < len; ++i)
		if ((unsigned char)str[i] < 0x20 || (unsigned char)str[i] > 0x7e)
			return -EINVAL;

	memcpy(vte->version, str, len + 1);
	return 0;
}

/* a capability name: 1 to TSM_VTE_TERMCAP_NAME_MAX bytes, each 0x21-0x7e */
static bool termcap_name_ok(const char *name, size_t *len)
{
	size_t i;

	*len = strnlen(name, TSM_VTE_TERMCAP_NAME_MAX + 1);
	if (!*len || *len > TSM_VTE_TERMCAP_NAME_MAX)
		return false;
	for (i = 0; i < *len; ++i)
		if ((unsigned char)name[i] < 0x21 || (unsigned char)name[i] > 0x7e)
			return false;
	return true;
}

static int termcap_cmp(const void *a, const void *b)
{
	const struct vte_termcap *x = a, *y = b;

	return strcmp(x->name, y->name);
}

SHL_EXPORT
int tsm_vte_set_termcap(struct tsm_vte *vte, const struct tsm_vte_termcap *caps,
			size_t n)
{
	struct vte_termcap *table = NULL;
	size_t i, size, len;
	char *p;

	if (!vte || (n && !caps) || n > TSM_VTE_TERMCAP_MAX)
		return -EINVAL;

	/* one block: the entries, then their strings */
	size = n * sizeof(*table);
	for (i = 0; i < n; ++i) {
		if (!caps[i].name || !termcap_name_ok(caps[i].name, &len))
			return -EINVAL;
		size += len + 1;
		if (caps[i].value) {
			len = strnlen(caps[i].value, TSM_VTE_TERMCAP_VALUE_MAX + 1);
			if (len > TSM_VTE_TERMCAP_VALUE_MAX)
				return -EINVAL;
			size += len + 1;
		}
	}

	if (n) {
		table = malloc(size);
		if (!table)
			return -ENOMEM;
		p = (char *)(table + n);
		for (i = 0; i < n; ++i) {
			len = strlen(caps[i].name) + 1;
			table[i].name = memcpy(p, caps[i].name, len);
			p += len;
			table[i].value = NULL;
			table[i].value_len = 0;
			if (caps[i].value) {
				len = strlen(caps[i].value);
				table[i].value = memcpy(p, caps[i].value, len + 1);
				table[i].value_len = len;
				p += len + 1;
			}
		}
		qsort(table, n, sizeof(*table), termcap_cmp);
		for (i = 1; i < n; ++i) {
			if (!strcmp(table[i - 1].name, table[i].name)) {
				free(table);
				return -EINVAL;
			}
		}
	}

	free(vte->termcap);
	vte->termcap = table;
	vte->termcap_n = n;
	return 0;
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
void vte_write_debug(struct tsm_vte *vte, const char *u8, size_t len, bool raw,
		     const char *file, int line)
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
 * This performs a soft reset of the VTE, as DECSTR and RIS do: everything is
 * reset to the same state as when the VTE was created. On the screen it
 * resets the modes, the margins and the tab stops (tsm_screen_reset) and the
 * cursor style, but leaves the cells and the scrollback.
 */
SHL_EXPORT
void tsm_vte_reset(struct tsm_vte *vte)
{
	if (!vte)
		return;

	/* the modes' flags start reset, and modes_reset sets those whose
	 * default is set */
	vte->flags = TSM_VTE_FLAG_BACKGROUND_COLOR_ERASE_MODE;
	tsm_screen_reset(vte->con);

	tsm_utf8_mach_reset(vte->mach);
	/* leaving a DCS without its end discards the picture, and a kitty
	 * upload in progress goes */
	vte_image_cancel(vte);
	vte->dcs = DCS_IGNORED;
	vte->dcs_esc = false;
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
	/* not a DEC mode, so no row; xterm's RIS and DECSTR reset it too */
	vte->modify_other_keys = 0;

	vte->cattr = vte->def_attr;
	screen_set_def_cell_attr(vte->con, &vte->def_attr);
	/* the cursor's shape and blink return to the default, as xterm's do */
	tsm_screen_set_cursor_style(vte->con, TSM_SCREEN_CURSOR_DEFAULT);
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
	vte_image_reset(vte);
	modes_reset(vte, RESET_HARD);
	tsm_screen_move_to(vte->con, 0, 0);
}

static void send_primary_da(struct tsm_vte *vte)
{
	const static char str[] = "\e[?60;1;6;9;15c";
	/* attribute 4: sixel graphics */
	const static char sixel[] = "\e[?60;1;4;6;9;15c";

	if (vte_sixel_on(vte))
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
	default:
		vte->csi_flags |= CSI_OTHER;
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

/* focus events: tsm_vte_handle_focus reports while set; setting it sends
 * nothing */
static void mode_FOCUS_set(struct tsm_vte *vte, bool set)
{
	vte->focus_events = set;
}

static bool mode_FOCUS_get(struct tsm_vte *vte)
{
	return vte->focus_events;
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
 * DECARM: the host makes a held key's repeats, and stops them while the
 * mode is reset (tsm_vte_get_mode)
 */
static void mode_DECARM_set(struct tsm_vte *vte, bool set)
{
	set_reset_flag(vte, set, TSM_VTE_FLAG_AUTO_REPEAT_MODE);
}

static bool mode_DECARM_get(struct tsm_vte *vte)
{
	return vte->flags & TSM_VTE_FLAG_AUTO_REPEAT_MODE;
}

/*
 * AT&T 610 cursor blink. Under a style DECSCUSR chose, set makes the cursor
 * blink and reset makes it steady, in the same shape: the odd DECSCUSR styles
 * blink and the even ones are steady. Under the default style the cursor
 * blinks as the host's setting says (kmscon's blink), and SM and RM leave it,
 * as xterm's preference wins when it is on and RIS or DECSTR has cleared the
 * flag it XORs with. DECRQM reports whether the cursor blinks: the host blinks
 * it (tsm_vte_set_cursor_blink) and the style is one that blinks, as kmscon
 * draws them.
 */
static void mode_CURBLINK_set(struct tsm_vte *vte, bool set)
{
	enum tsm_screen_cursor_style style = tsm_screen_get_cursor_style(vte->con);

	if (style == TSM_SCREEN_CURSOR_DEFAULT || style > TSM_SCREEN_CURSOR_VBAR_STEADY)
		return;
	if (set && !(style & 1))
		--style;
	else if (!set && (style & 1))
		++style;
	else
		return;
	tsm_screen_set_cursor_style(vte->con, style);
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
 * while the host says it does (tsm_vte_set_sync), and a mode of sixel's
 * (HOST_SIXEL: 80, 1070, 8452) only while sixel is on, turned on and with a
 * budget (vte_sixel_on). Until then it reports its fixed state, as an ignored
 * row does, since nothing would honour it.
 */
static bool mode_offered(struct tsm_vte *vte, enum mode_host host)
{
	switch (host) {
	case HOST_DRAW:
		return vte->sync_enabled;
	case HOST_SIXEL:
		return vte_sixel_on(vte);
	default:
		return true;
	}
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

static bool csi_has_subargs(struct tsm_vte *vte)
{
	unsigned int i;

	for (i = 0; i < vte->csi_argc; ++i)
		if (vte->csi_subargc[i])
			return true;
	return false;
}

/*
 * Whether the sequence carries a prefix or intermediate outside @allowed:
 * then it is another sequence (ctlseqs has CSI # P, CSI > T, CSI # q and
 * more), which libtsm does not implement, and it must not run as the plain
 * one (aaronsb/libtsm#63).
 */
static bool csi_foreign(struct tsm_vte *vte, uint32_t data, unsigned int allowed)
{
	if (!(vte->csi_flags & ~allowed))
		return false;
	llog_debug(vte, "unhandled CSI %c with flags %x", data, vte->csi_flags);
	return true;
}

/* XTVERSION, CSI > Ps q, for Ps 0 or omitted: DCS > | text ST */
static void csi_xtversion(struct tsm_vte *vte)
{
	char buf[TSM_VTE_VERSION_MAX + 8];
	int len;

	if (vte->csi_argv[0] > 0 || csi_has_subargs(vte)) {
		llog_debug(vte, "unhandled XTVERSION %d", vte->csi_argv[0]);
		return;
	}

	len = snprintf(buf, sizeof(buf), "\eP>|%s\e\\", vte->version);
	if (len > 0 && (size_t)len < sizeof(buf))
		vte_write(vte, buf, len);
}

/*
 * XTMODKEYS (CSI > Pp ; Pv m) and XTQMODKEYS (CSI ? Pp m) for Pp 4, xterm's
 * modifyOtherKeys (aaronsb/libtsm#25): levels 0 to 2 are taken, level 3 is
 * not, Pv omitted is 0, and CSI > m (every resource back to its initial
 * value) is 0 too. The query's reply is the XTMODKEYS that restores the
 * level. The other resources (modifyCursorKeys and the like), and the
 * sub-parameter that factors modifiers out, are xterm's alone and ignored.
 */
static void csi_modkeys(struct tsm_vte *vte)
{
	char buf[16];
	int len;

	if (csi_has_subargs(vte)) {
		llog_debug(vte, "XTMODKEYS with a sub-parameter ignored");
		return;
	}

	if (vte->csi_flags == CSI_WHAT) {
		if (vte->csi_argv[0] != 4)
			return;
		len = snprintf(buf, sizeof(buf), "\e[>4;%um", vte->modify_other_keys);
		vte_write(vte, buf, len);
		return;
	}
	if (vte->csi_flags != CSI_GT)
		return;

	if (vte->csi_argc <= 1 && vte->csi_argv[0] < 0)
		vte->modify_other_keys = 0;
	else if (vte->csi_argv[0] != 4)
		llog_debug(vte, "XTMODKEYS %d ignored", vte->csi_argv[0]);
	else if (vte->csi_argv[1] < 0)
		vte->modify_other_keys = 0;
	else if (vte->csi_argv[1] <= 2)
		vte->modify_other_keys = vte->csi_argv[1];
	else
		llog_debug(vte, "modifyOtherKeys %d ignored", vte->csi_argv[1]);
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
		if (csi_foreign(vte, data, 0))
			break;
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
		if (csi_foreign(vte, data, 0))
			break;
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
		if (vte->csi_flags & (CSI_GT | CSI_WHAT))
			csi_modkeys(vte);
		else
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
		} else if (!csi_foreign(vte, data, CSI_DQUOTE)) {
			/* DECSCL: Compatibility Level */
			/* Sometimes CSI_DQUOTE is set here, too */
			csi_compat_mode(vte);
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
		if (csi_foreign(vte, data, 0))
			break;

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
		if (csi_foreign(vte, data, 0))
			break;
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
		if (csi_foreign(vte, data, 0))
			break;
		/* insert characters */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_insert_chars(vte->con, num);
		break;
	case 'P': /* DCH */
		if (csi_foreign(vte, data, 0))
			break;
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
		/* XTMODKEYS disable (CSI > Ps n): modifyOtherKeys's -1 is
		 * level 0 here; the other resources are xterm's alone */
		if (vte->csi_flags == CSI_GT) {
			if (vte->csi_argv[0] == 4)
				vte->modify_other_keys = 0;
			break;
		}
		/* device status reports; DECXCPR (CSI ? 6 n) gets the plain
		 * CPR, as it always has */
		if (csi_foreign(vte, data, CSI_WHAT))
			break;
		csi_dsr(vte);
		break;
	case 'S':
		if (vte->csi_flags & CSI_WHAT) {
			vte_xtsmgraphics(vte);
			break;
		}
		if (csi_foreign(vte, data, 0))
			break;
		/* SU: scroll up */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_scroll_up(vte->con, num);
		break;
	case 'T': /* SD */
		if (csi_foreign(vte, data, 0))
			break;
		/* scroll down */
		num = vte->csi_argv[0];
		if (num <= 0)
			num = 1;
		tsm_screen_scroll_down(vte->con, num);
		break;
	case 't': /* XTWINOPS */
		if (csi_foreign(vte, data, 0))
			break;
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
	case 'q': /* DECLL - Load LEDs / DECSCUSR - Set Cursor Style / XTVERSION */
		if (vte->csi_flags & CSI_SPACE) {
			num = vte->csi_argv[0];
			if (num < 0)
				num = 0;
			tsm_screen_set_cursor_style(vte->con, num);
		} else if (vte->csi_flags == CSI_GT) {
			csi_xtversion(vte);
		} else if (!csi_foreign(vte, data, 0)) {
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

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int termcap_find(const void *key, const void *entry)
{
	const struct vte_termcap *cap = entry;

	return strcmp(key, cap->name);
}

/*
 * The host's entry for the hex name from @p up to the next ';' or @end,
 * which @next is set to; NULL when the name is not even, hex, or a name
 * termcap_name_ok takes, or the table has no such entry.
 */
static const struct vte_termcap *xtgettcap_lookup(struct tsm_vte *vte,
						  const char *p, const char *end,
						  const char **next)
{
	char name[TSM_VTE_TERMCAP_NAME_MAX + 1];
	size_t i, len;
	int hi, lo;

	*next = memchr(p, ';', end - p);
	if (!*next)
		*next = end;
	len = *next - p;
	if (!len || len % 2 || len / 2 > TSM_VTE_TERMCAP_NAME_MAX)
		return NULL;
	for (i = 0; i < len / 2; ++i) {
		hi = hex_digit(p[2 * i]);
		lo = hex_digit(p[2 * i + 1]);
		if (hi < 0 || lo < 0 || hi * 16 + lo < 0x21 || hi * 16 + lo > 0x7e)
			return NULL;
		name[i] = hi * 16 + lo;
	}
	name[i] = 0;
	if (!vte->termcap_n)
		return NULL;
	return bsearch(name, vte->termcap, vte->termcap_n, sizeof(*vte->termcap),
		       termcap_find);
}

/*
 * XTGETTCAP, DCS + q names ST, answered as xterm answers it: one reply,
 * DCS 1 + r name=value;name=value ST, each name as the request spelled it
 * and each value hex encoded; a boolean is its name alone. A name the host's
 * table lacks, or that is not hex for a name, ends the list there, after the
 * ';' before it, and when it is the first the reply is DCS 0 + r ST. So does
 * the name past the sixteenth, or one whose pair would take the reply past
 * XTGETTCAP_REPLY_MAX. A payload longer than XTGETTCAP_MAX_LEN is refused
 * whole, DCS 0 + r ST.
 */
static void dcs_xtgettcap(struct tsm_vte *vte)
{
	static const char hex[] = "0123456789ABCDEF";
	char buf[XTGETTCAP_REPLY_MAX];
	const char *p = vte->tcap_buf, *end = p + vte->tcap_len, *next;
	const struct vte_termcap *cap;
	size_t len, pair, i;
	unsigned int n;

	cap = vte->tcap_over ? NULL : xtgettcap_lookup(vte, p, end, &next);
	if (!cap) {
		vte_write(vte, "\eP0+r\e\\", 7);
		return;
	}

	memcpy(buf, "\eP1+r", 5);
	len = 5;
	for (n = 1; ; ++n) {
		/* the pair, a ';' after it and the ST that ends the reply */
		pair = (next - p) + (cap->value ? 1 + 2 * cap->value_len : 0);
		if (len + pair + 3 > sizeof(buf))
			break;
		memcpy(buf + len, p, next - p);
		len += next - p;
		if (cap->value) {
			buf[len++] = '=';
			for (i = 0; i < cap->value_len; ++i) {
				buf[len++] = hex[(unsigned char)cap->value[i] >> 4];
				buf[len++] = hex[(unsigned char)cap->value[i] & 0xf];
			}
		}
		if (next == end)
			break;
		buf[len++] = ';';
		p = next + 1;
		if (n == XTGETTCAP_NAMES ||
		    !(cap = xtgettcap_lookup(vte, p, end, &next)))
			break;
	}
	memcpy(buf + len, "\e\\", 2);
	vte_write(vte, buf, len + 2);
}

/*
 * DCS strings: the final byte and the collected prefixes and intermediates
 * pick the string's handler when its data starts, and its bytes and its end
 * go to that handler. DCS q with neither is sixel, which goes to the image
 * glue (tsm-vte-int.h); DCS + q without parameters is XTGETTCAP, answered
 * here in every build; every other DCS is consumed with no effect.
 */
static void do_dcs_start(struct tsm_vte *vte, uint32_t data)
{
	vte->dcs = DCS_IGNORED;
	if (data == 'q' && !vte->csi_flags) {
		vte->dcs = DCS_IMAGE;
		vte_dcs_start(vte, data);
	} else if (data == 'q' && vte->csi_flags == CSI_PLUS &&
		   !vte->csi_argc && vte->csi_argv[0] < 0) {
		vte->dcs = DCS_XTGETTCAP;
		vte->tcap_len = 0;
		vte->tcap_over = false;
	}
}

static void do_dcs_collect(struct tsm_vte *vte, uint32_t data)
{
	if (vte->dcs == DCS_IMAGE) {
		vte_dcs_collect(vte, data);
	} else if (vte->dcs == DCS_XTGETTCAP) {
		/* the payload is ASCII; anything else is kept as a byte no
		 * name holds, so its name is refused */
		if (vte->tcap_len < sizeof(vte->tcap_buf))
			vte->tcap_buf[vte->tcap_len++] = data < 0x80 ? data : 0;
		else
			vte->tcap_over = true;
	}
}

/* the end of the string: @data is what ended it */
static void do_dcs_end(struct tsm_vte *vte, uint32_t data)
{
	if (vte->dcs == DCS_IMAGE) {
		vte_dcs_end(vte, data);
	} else if (vte->dcs == DCS_XTGETTCAP) {
		/* ST answers it; ESC answers it if '\' follows (parse_data),
		 * and CAN, SUB or another C1 control drops it, as xterm does */
		if (data == 0x9c)
			dcs_xtgettcap(vte);
		else if (data == 0x1b)
			vte->dcs_esc = true;
	}
	vte->dcs = DCS_IGNORED;
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
			vte_apc_start(vte);
			break;
		case ACTION_APC_COLLECT:
			vte_apc_collect(vte, data);
			break;
		case ACTION_APC_END:
			vte_apc_end(vte, data);
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
	/* an XTGETTCAP request that an ESC ended: ESC \ is ST */
	if (vte->dcs_esc) {
		vte->dcs_esc = false;
		if (raw == '\\')
			dcs_xtgettcap(vte);
	}

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

/* parses until @len bytes are read, the image work runs out, a kitty
 * command is handed to the host hook, or, with sync_stop, a hold ends */
static size_t vte_input(struct tsm_vte *vte, const char *u8, size_t len)
{
	uint64_t frames = vte->sync_frames;
	int state;
	uint32_t ucs4;
	size_t i, n;

	for (i = 0; i < len; ) {
		if (vte->state == STATE_DCS_PASS &&
		    (n = vte_sixel_run(vte, u8 + i, len - i))) {
			i += n;
		} else if (vte->state == STATE_APC_STRING &&
			   (n = vte_kitty_run(vte, u8 + i, len - i))) {
			i += n;
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
		if (!vte->work_left || vte_image_paused(vte) ||
		    (vte->sync_stop && vte->sync_frames != frames))
			break;
	}
	return i;
}

/* the image work one tsm_vte_input_bounded call may do, in pixels */
#define VTE_INPUT_WORK ((uint64_t)1 << 24)

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
	bool saved_host = vte_image_set_host(vte, bounded);
	size_t n;

	++vte->parse_cnt;
	vte->work_left = work;
	vte->sync_stop = bounded;
	n = vte_input(vte, u8, len);
	/* what the call's cuts and scrolls owe the budget */
	screen_images_settle(vte->con);
	vte->work_left = saved;
	vte->sync_stop = saved_stop;
	vte_image_set_host(vte, saved_host);
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
	if (vte_image_busy(vte)) {
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

	if (vte_image_busy(vte))
		return 0;
	return vte_input_with(vte, u8, len, VTE_INPUT_WORK, true);
}

/* restores the rows only RIS resets (80, 1070 and 8452) */
void vte_modes_reset_hard(struct tsm_vte *vte)
{
	modes_reset(vte, RESET_HARD);
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

/*
 * The C0 byte Ctrl turns @sym into, or -1 for a key Ctrl leaves as it is:
 * xterm's bytes (its us-pc105 table, mode 0). @sym is the ascii keysym, so
 * Ctrl+C works whatever layout is active.
 */
static int ctrl_byte(uint32_t sym)
{
	if ((sym >= XKB_KEY_a && sym <= XKB_KEY_z) ||
	    (sym >= XKB_KEY_A && sym <= XKB_KEY_Z))
		return sym & 0x1f;

	switch (sym) {
	case XKB_KEY_2:
	case XKB_KEY_space:
	case XKB_KEY_at:
	case XKB_KEY_grave:
		return 0x00;
	case XKB_KEY_3:
	case XKB_KEY_bracketleft:
	case XKB_KEY_braceleft:
		return 0x1b;
	case XKB_KEY_4:
	case XKB_KEY_backslash:
	case XKB_KEY_bar:
		return 0x1c;
	case XKB_KEY_5:
	case XKB_KEY_bracketright:
	case XKB_KEY_braceright:
		return 0x1d;
	case XKB_KEY_6:
	case XKB_KEY_asciicircum:
	case XKB_KEY_asciitilde:
		return 0x1e;
	case XKB_KEY_7:
	case XKB_KEY_slash:
	case XKB_KEY_underscore:
		return 0x1f;
	case XKB_KEY_8:
	case XKB_KEY_question:
		return 0x7f;
	default:
		return -1;
	}
}

/*
 * The character a printable key produced, after Shift: from the keysym where
 * it is Latin-1, else the host's codepoint; 0 for none. Keysyms from 0xfd00
 * up are function, keypad and other non-character keys (ctlseqs, "Alt and
 * Meta Keys"), never encoded, whatever codepoint the host gives them.
 */
static uint32_t key_char(uint32_t keysym, uint32_t unicode)
{
	if ((keysym >= 0x20 && keysym <= 0x7e) || (keysym >= 0xa0 && keysym <= 0xff))
		return keysym;
	if (keysym >= 0xfd00 && keysym <= 0xffff)
		return 0;
	if (unicode < 0x20 || unicode == 0x7f || unicode > 0x10ffff)
		return 0;
	return unicode;
}

/*
 * xterm's modifyOtherKeys (aaronsb/libtsm#25): at level 1 or 2, a key
 * pressed with Shift, Alt or Ctrl is sent as CSI 27 ; mod ; code ~, mod as
 * for the cursor keys and code the character, where the rules below say so;
 * returns false to leave the key to the usual encoding. The rules follow
 * ctlseqs ("Alt and Meta Keys") and xterm's "Modified Other Keys (us-pc105)"
 * table:
 *
 * - Level 1 encodes a printable key with Alt, or with Ctrl where libtsm has
 *   no control byte for it (Ctrl+1 encodes, Ctrl+a stays 0x01), and Ctrl+?,
 *   whose DEL xterm sends only at level 0: X11's own Ctrl mapping does not
 *   cover '?'; space and
 *   Escape only with Alt; Tab and Return with any modifier; never
 *   BackSpace. The sources disagree on Alt: xterm(1)'s modifyOtherKeys
 *   entry says Alt and Meta cause no escapes at level 1, while ctlseqs
 *   ("alt-Tab sends CSI 2 7 ; 3 ; 9 ~") and the us-pc105 table encode Alt
 *   there; libtsm follows ctlseqs and the table.
 * - Level 2 encodes all of them with any modifier, but BackSpace not with
 *   Ctrl alone, and, with Shift alone, only space and the characters @ to ~
 *   (the table's rows for ! to ? and for characters past ASCII send the
 *   character). The table decides by the character after Shift, which is
 *   what libtsm is given.
 * - Shift+Tab arrives as ISO_Left_Tab: CSI Z at level 1 whatever else is
 *   held, as before; level 2 encodes it as Tab with its modifiers, Shift
 *   included, so Shift+Tab sends CSI 27 ; 2 ; 9 ~ "rather than CSI Z", as
 *   ctlseqs has it, and level 2 drops the exceptions level 1 keeps.
 * - Ctrl reads @sym, the ASCII layout's keysym, as the legacy path does: on
 *   a Russian layout Ctrl+es keeps 0x03 (c) at level 1, where xterm, going
 *   by the keysym, encodes it; code is still the character the key made.
 *
 * Alt goes in mod, so no ESC comes before the sequence. Keypad, function,
 * cursor and edit keys and Linefeed are never encoded here.
 */
static bool vte_write_modified_key(struct tsm_vte *vte, uint32_t keysym, uint32_t sym,
				   unsigned int mods, uint32_t unicode)
{
	unsigned int m = mods & (TSM_SHIFT_MASK | TSM_ALT_MASK | TSM_CONTROL_MASK);
	bool level2 = vte->modify_other_keys == 2;
	uint32_t code;
	char buf[32];
	int len;

	if (!m)
		return false;

	switch (keysym) {
	case XKB_KEY_Tab:
	case XKB_KEY_Return:
		code = keysym == XKB_KEY_Tab ? 9 : 13;
		break;
	case XKB_KEY_ISO_Left_Tab:
		if (!level2)
			return false;
		code = 9;
		break;
	case XKB_KEY_Escape:
		if (!level2 && !(m & TSM_ALT_MASK))
			return false;
		code = 27;
		break;
	case XKB_KEY_BackSpace:
		if (!level2 || m == TSM_CONTROL_MASK)
			return false;
		code = vte->backspace_sends_delete ? 127 : 8;
		break;
	default:
		code = key_char(keysym, unicode);
		if (!code)
			return false;
		if (level2) {
			if (m == TSM_SHIFT_MASK && code != ' ' && (code < '@' || code > '~'))
				return false;
		} else if (!(m & TSM_ALT_MASK)) {
			if (!(m & TSM_CONTROL_MASK) ||
			    (ctrl_byte(sym) >= 0 && sym != XKB_KEY_question))
				return false;
		}
		break;
	}

	len = snprintf(buf, sizeof(buf), "\e[27;%c;%u~", csi_modifier_from_mask(m), code);
	vte->flags &= ~TSM_VTE_FLAG_PREPEND_ESCAPE;
	vte_write(vte, buf, len);
	return true;
}

SHL_EXPORT
bool tsm_vte_handle_keyboard(struct tsm_vte *vte, uint32_t keysym,
			     uint32_t ascii, unsigned int mods,
			     uint32_t unicode)
{
	char val, u8[4];
	size_t len;
	uint32_t sym;
	int ctrl;

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

	if (vte->modify_other_keys &&
	    vte_write_modified_key(vte, keysym, sym, mods, unicode))
		return true;

	if (mods & TSM_CONTROL_MASK) {
		ctrl = ctrl_byte(sym);
		if (ctrl >= 0) {
			val = ctrl;
			vte_write(vte, &val, 1);
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
		/* the top left pixel is 1,1, as xterm sends it (aaronsb/libtsm#67) */
		snprintf(buffer, sizeof(buffer), "\e[<%u;%u;%u%c", cb, pixel_x + 1,
			 pixel_y + 1, released ? 'm' : 'M');
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

SHL_EXPORT
bool tsm_vte_handle_focus(struct tsm_vte *vte, bool focused)
{
	if (!vte || !vte->focus_events)
		return false;

	vte_write(vte, focused ? "\e[I" : "\e[O", 3);
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
