/*
 * TSM - VTE State Machine Tests
 *
 * Copyright (c) 2018 Aetf <aetf@unlimitedcodeworks.xyz>
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

#include "libtsm-int.h"
#include "libtsm.h"
#include "test_common.h"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <stdio.h>

static void log_cb(void *data, const char *file, int line, const char *func, const char *subs,
				   unsigned int sev, const char *format, va_list args)
{
	UNUSED(data);
	UNUSED(file);
	UNUSED(line);
	UNUSED(func);
	UNUSED(subs);
	UNUSED(sev);
	UNUSED(format);
	UNUSED(args);
}

static void write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	UNUSED(len);
	UNUSED(data);

	ck_assert_ptr_ne(vte, NULL);
	ck_assert_ptr_ne(u8, NULL);
}

START_TEST(test_vte_init)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	tsm_vte_unref(vte);
	vte = NULL;

	tsm_screen_unref(screen);
	screen = NULL;
}
END_TEST

START_TEST(test_vte_null)
{
	int r;
	bool rb;

	r = tsm_vte_new(NULL, NULL, NULL, NULL, log_cb, NULL);
	ck_assert_int_eq(r, -EINVAL);

	tsm_vte_ref(NULL);
	tsm_vte_unref(NULL);

	tsm_vte_set_osc_cb(NULL, NULL, NULL);

	r = tsm_vte_set_palette(NULL, "");
	ck_assert_int_eq(r, -EINVAL);

	r = tsm_vte_set_custom_palette(NULL, NULL);
	ck_assert_int_eq(r, -EINVAL);

	tsm_vte_get_def_attr(NULL, NULL);
	tsm_vte_get_flags(NULL);

	tsm_vte_get_mouse_mode(NULL);
	tsm_vte_get_mouse_event(NULL);

	tsm_vte_reset(NULL);
	tsm_vte_hard_reset(NULL);
	tsm_vte_input(NULL, "", 0);

	rb = tsm_vte_handle_keyboard(NULL, 0, 0, 0, 0);
	ck_assert(!rb);
}
END_TEST

static uint8_t test_palette[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = {   0,  18,  36 },
	[TSM_COLOR_RED]           = {   1,  19,  37 },
	[TSM_COLOR_GREEN]         = {   2,  20,  38 },
	[TSM_COLOR_YELLOW]        = {   3,  21,  39 },
	[TSM_COLOR_BLUE]          = {   4,  22,  40 },
	[TSM_COLOR_MAGENTA]       = {   5,  23,  41 },
	[TSM_COLOR_CYAN]          = {   6,  24,  42 },
	[TSM_COLOR_LIGHT_GREY]    = {   7,  25,  43 },
	[TSM_COLOR_DARK_GREY]     = {   8,  26,  44 },
	[TSM_COLOR_LIGHT_RED]     = {   9,  27,  45 },
	[TSM_COLOR_LIGHT_GREEN]   = {  10,  28,  46 },
	[TSM_COLOR_LIGHT_YELLOW]  = {  11,  29,  47 },
	[TSM_COLOR_LIGHT_BLUE]    = {  12,  30,  48 },
	[TSM_COLOR_LIGHT_MAGENTA] = {  13,  31,  49 },
	[TSM_COLOR_LIGHT_CYAN]    = {  14,  32,  50 },
	[TSM_COLOR_WHITE]         = {  15,  33,  51 },

	[TSM_COLOR_FOREGROUND]    = {  16,  34,  52 },
	[TSM_COLOR_BACKGROUND]    = {  17,  35,  53 },
};

START_TEST(test_vte_custom_palette)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_set_custom_palette(vte, test_palette);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_set_palette(vte, "custom");
	ck_assert_int_eq(r, 0);

	struct tsm_screen_attr attr;
	tsm_vte_get_def_attr(vte, &attr);
	ck_assert_uint_eq(attr.fr, test_palette[TSM_COLOR_FOREGROUND][0]);
	ck_assert_uint_eq(attr.fg, test_palette[TSM_COLOR_FOREGROUND][1]);
	ck_assert_uint_eq(attr.fb, test_palette[TSM_COLOR_FOREGROUND][2]);

	ck_assert_uint_eq(attr.br, test_palette[TSM_COLOR_BACKGROUND][0]);
	ck_assert_uint_eq(attr.bg, test_palette[TSM_COLOR_BACKGROUND][1]);
	ck_assert_uint_eq(attr.bb, test_palette[TSM_COLOR_BACKGROUND][2]);

	tsm_vte_unref(vte);
	vte = NULL;

	tsm_screen_unref(screen);
	screen = NULL;
}
END_TEST

/* the 256-colour table in use, read back: 0-15 follow the palette chosen,
 * named or custom; 16-255 are the cube and the grey ramp; past 255 is refused */
START_TEST(test_vte_get_palette_color)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	uint8_t rgb[3];
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* the default palette is legacy */
	r = tsm_vte_get_palette_color(vte, TSM_COLOR_RED, rgb);
	ck_assert_int_eq(r, 0);
	ck_assert_uint_eq(rgb[0], 205);
	ck_assert_uint_eq(rgb[1], 0);
	ck_assert_uint_eq(rgb[2], 0);

	r = tsm_vte_set_palette(vte, "vga");
	ck_assert_int_eq(r, 0);
	r = tsm_vte_get_palette_color(vte, TSM_COLOR_YELLOW, rgb);
	ck_assert_int_eq(r, 0);
	ck_assert_uint_eq(rgb[0], 170);
	ck_assert_uint_eq(rgb[1], 85);
	ck_assert_uint_eq(rgb[2], 0);

	r = tsm_vte_set_custom_palette(vte, test_palette);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_set_palette(vte, "custom");
	ck_assert_int_eq(r, 0);
	r = tsm_vte_get_palette_color(vte, TSM_COLOR_LIGHT_BLUE, rgb);
	ck_assert_int_eq(r, 0);
	ck_assert_uint_eq(rgb[0], 12);
	ck_assert_uint_eq(rgb[1], 30);
	ck_assert_uint_eq(rgb[2], 48);

	/* the cube and the grey ramp do not depend on the palette */
	r = tsm_vte_get_palette_color(vte, 196, rgb);
	ck_assert_int_eq(r, 0);
	ck_assert_uint_eq(rgb[0], 255);
	ck_assert_uint_eq(rgb[1], 0);
	ck_assert_uint_eq(rgb[2], 0);
	r = tsm_vte_get_palette_color(vte, 255, rgb);
	ck_assert_int_eq(r, 0);
	ck_assert_uint_eq(rgb[0], 238);
	ck_assert_uint_eq(rgb[1], 238);
	ck_assert_uint_eq(rgb[2], 238);

	/* refused, and @rgb left alone */
	rgb[0] = rgb[1] = rgb[2] = 7;
	r = tsm_vte_get_palette_color(vte, 256, rgb);
	ck_assert_int_eq(r, -EINVAL);
	ck_assert_uint_eq(rgb[0], 7);
	ck_assert_uint_eq(rgb[1], 7);
	ck_assert_uint_eq(rgb[2], 7);
	r = tsm_vte_get_palette_color(NULL, 1, rgb);
	ck_assert_int_eq(r, -EINVAL);
	r = tsm_vte_get_palette_color(vte, 1, NULL);
	ck_assert_int_eq(r, -EINVAL);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

static bool write_cb_called = false;

static void checking_write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	write_cb_called = true;

	ck_assert_ptr_ne(vte, NULL);
	ck_assert_ptr_ne(u8, NULL);
	ck_assert_uint_gt(len, 0);

	ck_assert_mem_eq(u8, data, len);
}

static void checked_vte_input(struct tsm_vte *vte, const char *u8, size_t len)
{
	write_cb_called = false;
	tsm_vte_input(vte, u8, len);
	ck_assert(write_cb_called);
}

START_TEST(test_vte_osc_query)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int r;
	char expected_output[32];
	const char *input;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, checking_write_cb, expected_output, NULL, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_set_custom_palette(vte, test_palette);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_set_palette(vte, "custom");
	ck_assert_int_eq(r, 0);

	// query foreground, end_seq = BEL
	input = "\e]10;?\x07";
	strcpy(expected_output, "\e]10;rgb:1010/2222/3434\x07");
	checked_vte_input(vte, input, strlen(input));

	// query foreground, end_seq = ST
	input = "\e]10;?\e\\";
	strcpy(expected_output, "\e]10;rgb:1010/2222/3434\e\\");
	checked_vte_input(vte, input, strlen(input));

	// ignore additional parameters after foreground query
	input = "\e]10;?;11;?\x07";
	strcpy(expected_output, "\e]10;rgb:1010/2222/3434\x07");
	checked_vte_input(vte, input, strlen(input));

	// query background
	input = "\e]11;?\x07";
	strcpy(expected_output, "\e]11;rgb:1111/2323/3535\x07");
	checked_vte_input(vte, input, strlen(input));

	// ignore additional parameters after background query
	input = "\e]11;?;12;?\x07";
	strcpy(expected_output, "\e]11;rgb:1111/2323/3535\x07");
	checked_vte_input(vte, input, strlen(input));

	tsm_vte_unref(vte);
	vte = NULL;

	tsm_screen_unref(screen);
	screen = NULL;
}
END_TEST

unsigned char write_buffer[512];
unsigned char *write_buffer_p = write_buffer;

static void storing_write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	ck_assert_ptr_ne(vte, NULL);
	ck_assert_ptr_ne(u8, NULL);

	memcpy(write_buffer_p, u8, len);
	write_buffer_p[len] = '\0';
	write_buffer_p += len;
}

static void storing_write_cb_reset(void)
{
	memset(write_buffer, 0, sizeof(write_buffer));
	write_buffer_p = write_buffer;
}

START_TEST(test_vte_osc4)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int r;
	const char *input;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, storing_write_cb, NULL, NULL, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_set_custom_palette(vte, test_palette);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_set_palette(vte, "custom");
	ck_assert_int_eq(r, 0);

	// query palette entries
	storing_write_cb_reset();
	input = "\e]4;1;?;13;?;3;?\x07";
	const char *expected =
		"\e]4;1;rgb:0101/1313/2525\x07"
		"\e]4;13;rgb:0d0d/1f1f/3131\x07"
		"\e]4;3;rgb:0303/1515/2727\x07";
	tsm_vte_input(vte, input, strlen(input));
	ck_assert_mem_eq(write_buffer, expected, strlen(expected));
	ck_assert_int_eq(write_buffer_p - write_buffer, strlen(expected));

	// query cube & grayscale entries
	storing_write_cb_reset();
	input = "\e]4;110;?;254;?;\x07";
	expected =
		"\e]4;110;rgb:8787/afaf/d7d7\x07"
		"\e]4;254;rgb:e4e4/e4e4/e4e4\x07";
	tsm_vte_input(vte, input, strlen(input));
	ck_assert_mem_eq(write_buffer, expected, strlen(expected));

	// ignore color change requests, incomplete messages
	storing_write_cb_reset();
	input = "\e]4;1;rgb:1111/2222/3333;2\x07";
	tsm_vte_input(vte, input, strlen(input));
	ck_assert_ptr_eq(write_buffer, write_buffer_p);
	storing_write_cb_reset();

	tsm_vte_unref(vte);
	vte = NULL;

	tsm_screen_unref(screen);
	screen = NULL;
}
END_TEST

START_TEST(test_vte_backspace_key)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	char expected_output;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, checking_write_cb, &expected_output, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	expected_output = '\010';
	r = tsm_vte_handle_keyboard(vte, XKB_KEY_BackSpace, 010, 0, 010);
	ck_assert(r);
	r = tsm_vte_handle_keyboard(vte, XKB_KEY_BackSpace, 0177, 0, 0177);
	ck_assert(r);

	tsm_vte_set_backspace_sends_delete(vte, true);

	expected_output = '\177';
	r = tsm_vte_handle_keyboard(vte, XKB_KEY_BackSpace, 010, 0, 010);
	ck_assert(r);
	r = tsm_vte_handle_keyboard(vte, XKB_KEY_BackSpace, 0177, 0, 0177);
	ck_assert(r);

	tsm_vte_set_backspace_sends_delete(vte, false);

	expected_output = '\010';
	r = tsm_vte_handle_keyboard(vte, XKB_KEY_BackSpace, 010, 0, 010);
	ck_assert(r);
	r = tsm_vte_handle_keyboard(vte, XKB_KEY_BackSpace, 0177, 0, 0177);
	ck_assert(r);

	tsm_vte_unref(vte);
	vte = NULL;

	tsm_screen_unref(screen);
	screen = NULL;
}
END_TEST

START_TEST(test_vte_get_flags)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	char expected_output;
	int r, flags;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, checking_write_cb, &expected_output, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	flags = tsm_vte_get_flags(vte);
	ck_assert(!(flags & TSM_VTE_FLAG_CURSOR_KEY_MODE));

	// enable cursor key mode
	tsm_vte_input(vte, "\033[?1h", 5);

	flags = tsm_vte_get_flags(vte);
	ck_assert(flags & TSM_VTE_FLAG_CURSOR_KEY_MODE);

	tsm_vte_unref(vte);
	vte = NULL;

	tsm_screen_unref(screen);
	screen = NULL;
}
END_TEST

/* Regression test for https://github.com/Aetf/libtsm/issues/26 */
START_TEST(test_vte_decrqm_no_reset)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int r;
	unsigned int flags;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* switch terminal to alternate screen mode */
	tsm_vte_input(vte, "\033[?1049h", 8);

	flags = tsm_screen_get_flags(screen);
	ck_assert(flags & TSM_SCREEN_ALTERNATE);

	/* send DECRQM SRM (12) request */
	tsm_vte_input(vte, "\033[?12$p", 7);

	/* terminal should still be in alternate screen mode */
	flags = tsm_screen_get_flags(screen);
	ck_assert(flags & TSM_SCREEN_ALTERNATE);

	tsm_vte_unref(vte);
	vte = NULL;

	tsm_screen_unref(screen);
	screen = NULL;
}
END_TEST

#define assert_tsm_screen_cursor_pos(screen, x1, y1)                 \
	do {                                                             \
		ck_assert_int_eq(x1, tsm_screen_get_cursor_x(screen));       \
		ck_assert_int_eq(y1, tsm_screen_get_cursor_y(screen));       \
	} while(0)

/* test for https://github.com/Aetf/kmscon/issues/78 */
START_TEST(test_vte_csi_cursor_up_down)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int r;
	int h;
	char csi_cmd[64];

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	tsm_vte_input(vte, "\n123", 4);
	assert_tsm_screen_cursor_pos(screen, 3, 1);

	// cursor move up, first col
	tsm_vte_input(vte, "\033[1F", 4);
	assert_tsm_screen_cursor_pos(screen, 0, 0);

	// cursor move down, first col
	tsm_vte_input(vte, "\033[1E", 4);
	assert_tsm_screen_cursor_pos(screen, 0, 1);

	h = tsm_screen_get_height(screen);

	// move cursor up out of screen, should at first line
	sprintf(csi_cmd, "\033[%dF", h + 10);
	tsm_vte_input(vte, csi_cmd, strlen(csi_cmd));
	assert_tsm_screen_cursor_pos(screen, 0, 0);

	// move cursor down out of screen, should at last line
	sprintf(csi_cmd, "\033[%dE", h + 10);
	tsm_vte_input(vte, csi_cmd, strlen(csi_cmd));
	assert_tsm_screen_cursor_pos(screen, 0, h - 1);

	tsm_vte_unref(vte);
	vte = NULL;

	tsm_screen_unref(screen);
	screen = NULL;
}
END_TEST

static bool bell_cb_called = false;

static void bell_cb(struct tsm_vte *vte, void *data)
{
	bool *flag = data;
	ck_assert_ptr_ne(vte, NULL);
	if (flag)
		*flag = true;
	bell_cb_called = true;
}

START_TEST(test_vte_bell)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	tsm_vte_set_bell_cb(vte, bell_cb, NULL);

	bell_cb_called = false;
	tsm_vte_input(vte, "\x07", 1);
	ck_assert(bell_cb_called);

	/* BEL inside an OSC sequence acts as the ST terminator, not as a bell */
	bell_cb_called = false;
	tsm_vte_input(vte, "\033]0;title\x07", 10);
	ck_assert(!bell_cb_called);

	tsm_vte_unref(vte);
	vte = NULL;

	tsm_screen_unref(screen);
	screen = NULL;
}
END_TEST

START_TEST(test_vte_bell_no_cb)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* BEL without a registered callback must not crash */
	tsm_vte_input(vte, "\x07", 1);

	tsm_vte_unref(vte);
	vte = NULL;

	tsm_screen_unref(screen);
	screen = NULL;
}
END_TEST

START_TEST(test_vte_bell_data)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	bool flag = false;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	tsm_vte_set_bell_cb(vte, bell_cb, &flag);

	tsm_vte_input(vte, "\x07", 1);
	ck_assert(flag);

	tsm_vte_unref(vte);
	vte = NULL;

	tsm_screen_unref(screen);
	screen = NULL;
}
END_TEST

/* A cell keeps its colour as written (ADR-102), so a palette change recolours
 * text already on the screen. The vte erases the visible screen on a palette
 * change, so the text that survives it is in the scrollback. */
static int palette_draw_cb(struct tsm_screen *con, uint64_t id, const uint32_t *ch,
			   size_t len, unsigned int width, unsigned int posx,
			   unsigned int posy, const struct tsm_screen_attr *attr,
			   tsm_age_t age, void *data)
{
	struct tsm_screen_attr *out = data;

	UNUSED(con);
	UNUSED(id);
	UNUSED(width);
	UNUSED(age);
	if (posx == 0 && posy == 0 && len == 1 && ch[0] == 'R')
		*out = *attr;
	return 0;
}

START_TEST(test_vte_palette_recolours_text)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_screen_cell *cells;
	struct tsm_screen_attr attr;
	const char *input = "\e[31mR\e[1mB\e[0m\r\n\n\n";
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 3);
	ck_assert_int_eq(r, 0);
	tsm_screen_set_max_sb(screen, 10);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_set_palette(vte, "legacy");
	ck_assert_int_eq(r, 0);

	/* red R and bold red B, scrolled off the top of a 3-row screen */
	tsm_vte_input(vte, input, strlen(input));
	ck_assert_uint_eq(tsm_screen_sb_get_line_count(screen), 1);

	r = tsm_vte_set_palette(vte, "vga");
	ck_assert_int_eq(r, 0);
	tsm_screen_sb_up(screen, 1);

	/* vga red is 170,0,0 and vga light red 255,85,85; legacy's are 205,0,0
	 * and 255,0,0 */
	cells = tsm_screen_draw2(screen);
	ck_assert_ptr_ne(cells, NULL);
	ck_assert_uint_eq(cells[0].ch, 'R');
	ck_assert_uint_eq(cells[0].fg.r, 170);
	ck_assert_uint_eq(cells[0].fg.g, 0);
	ck_assert_uint_eq(cells[0].fg.b, 0);
	ck_assert_uint_eq(cells[1].ch, 'B');
	ck_assert_uint_eq(cells[1].fg.r, 255);
	ck_assert_uint_eq(cells[1].fg.g, 85);
	ck_assert_uint_eq(cells[1].fg.b, 85);

	memset(&attr, 0, sizeof(attr));
	tsm_screen_draw(screen, palette_draw_cb, &attr);
	ck_assert_int_eq(attr.fccode, TSM_COLOR_RED);
	ck_assert_uint_eq(attr.fr, 170);
	ck_assert_uint_eq(attr.fg, 0);
	ck_assert_uint_eq(attr.fb, 0);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* draw2 reports the underline colour as set or default: a set one stays as
 * set under reverse video, a default one is the foreground as drawn. */
START_TEST(test_vte_draw2_underline_color)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_screen_cell *cells;
	const char *input = "\e[4;58;2;1;2;3mS\e[59mD\e[7md\e[58;5;1mt";
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_set_palette(vte, "legacy");
	ck_assert_int_eq(r, 0);
	tsm_vte_input(vte, input, strlen(input));

	cells = tsm_screen_draw2(screen);
	ck_assert_ptr_ne(cells, NULL);

	/* S: set to 1,2,3 */
	ck_assert_uint_eq(cells[0].ul_set, 1);
	ck_assert_uint_eq(cells[0].ul.r, 1);
	ck_assert_uint_eq(cells[0].ul.g, 2);
	ck_assert_uint_eq(cells[0].ul.b, 3);
	/* D: default, so legacy's foreground, 229,229,229 */
	ck_assert_uint_eq(cells[1].ul_set, 0);
	ck_assert_uint_eq(cells[1].ul.r, 229);
	ck_assert_uint_eq(cells[1].ul.g, 229);
	ck_assert_uint_eq(cells[1].ul.b, 229);
	/* d: default under reverse video follows the swapped foreground, the
	 * background 0,0,0 */
	ck_assert_uint_eq(cells[2].ul_set, 0);
	ck_assert_uint_eq(cells[2].fg.r, 0);
	ck_assert_uint_eq(cells[2].ul.r, 0);
	ck_assert_uint_eq(cells[2].ul.g, 0);
	ck_assert_uint_eq(cells[2].ul.b, 0);
	/* t: set to palette red under reverse video stays legacy red, 205,0,0 */
	ck_assert_uint_eq(cells[3].ul_set, 1);
	ck_assert_uint_eq(cells[3].ul.r, 205);
	ck_assert_uint_eq(cells[3].ul.g, 0);
	ck_assert_uint_eq(cells[3].ul.b, 0);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* A set underline colour given as a palette code is not brightened by bold;
 * bold-as-bright is for the foreground. */
START_TEST(test_vte_underline_color_not_bright)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_screen_cell *cells;
	const char *input = "\e[1;4;58;5;1mX";
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_set_palette(vte, "legacy");
	ck_assert_int_eq(r, 0);
	tsm_vte_input(vte, input, strlen(input));

	/* legacy red 205,0,0, not light red 255,0,0 */
	cells = tsm_screen_draw2(screen);
	ck_assert_ptr_ne(cells, NULL);
	ck_assert_uint_eq(cells[0].attr2.bold, 1);
	ck_assert_uint_eq(cells[0].ul_set, 1);
	ck_assert_uint_eq(cells[0].ul.r, 205);
	ck_assert_uint_eq(cells[0].ul.g, 0);
	ck_assert_uint_eq(cells[0].ul.b, 0);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* A set underline colour given as a palette code follows a palette change,
 * like any colour kept as written. */
START_TEST(test_vte_underline_color_follows_palette)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_screen_cell *cells;
	const char *input = "\e[4;58;5;1mU\e[0m\r\n\n\n";
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 3);
	ck_assert_int_eq(r, 0);
	tsm_screen_set_max_sb(screen, 10);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_set_palette(vte, "legacy");
	ck_assert_int_eq(r, 0);
	tsm_vte_input(vte, input, strlen(input));
	ck_assert_uint_eq(tsm_screen_sb_get_line_count(screen), 1);

	r = tsm_vte_set_palette(vte, "vga");
	ck_assert_int_eq(r, 0);
	tsm_screen_sb_up(screen, 1);

	/* vga red 170,0,0, where legacy drew it 205,0,0 */
	cells = tsm_screen_draw2(screen);
	ck_assert_ptr_ne(cells, NULL);
	ck_assert_uint_eq(cells[0].ch, 'U');
	ck_assert_uint_eq(cells[0].ul_set, 1);
	ck_assert_uint_eq(cells[0].ul.r, 170);
	ck_assert_uint_eq(cells[0].ul.g, 0);
	ck_assert_uint_eq(cells[0].ul.b, 0);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* Dim leaves a set underline colour exact and dims a default one with the
 * foreground. */
START_TEST(test_vte_underline_color_dim)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_screen_cell *cells;
	const char *input = "\e[2;4;58;2;10;20;30mS\e[59mD";
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_set_palette(vte, "legacy");
	ck_assert_int_eq(r, 0);
	tsm_vte_input(vte, input, strlen(input));

	cells = tsm_screen_draw2(screen);
	ck_assert_ptr_ne(cells, NULL);
	/* S: set, not dimmed */
	ck_assert_uint_eq(cells[0].ul_set, 1);
	ck_assert_uint_eq(cells[0].ul.r, 10);
	ck_assert_uint_eq(cells[0].ul.g, 20);
	ck_assert_uint_eq(cells[0].ul.b, 30);
	/* D: default, dimmed with the foreground: legacy 229 over 0 gives
	 * 0/2 + 229/2 = 114 */
	ck_assert_uint_eq(cells[1].ul_set, 0);
	ck_assert_uint_eq(cells[1].fg.r, 114);
	ck_assert_uint_eq(cells[1].ul.r, 114);
	ck_assert_uint_eq(cells[1].ul.g, 114);
	ck_assert_uint_eq(cells[1].ul.b, 114);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* xterm's modifyOtherKeys controls end in 'm' but carry a '>' or '?' prefix:
 * XTMODKEYS (CSI > 4 ; 1 m, CSI > 4 m) sets the level, XTQMODKEYS (CSI ? 4 m)
 * queries it. None of them is SGR, so none sets underline or bold, and the
 * bare query CSI ? m does not reset what SGR set. */
START_TEST(test_vte_modkeys_not_sgr)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_screen_cell *cells;
	const char *input = "\e[>4;1ma\e[>4mb\e[?4mc\e[?1md\e[4mU\e[?mV";
	int i, r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	tsm_vte_input(vte, input, strlen(input));

	cells = tsm_screen_draw2(screen);
	ck_assert_ptr_ne(cells, NULL);
	for (i = 0; i < 4; i++) {
		ck_assert_uint_eq(cells[i].ch, (uint32_t)"abcd"[i]);
		ck_assert_msg(!cells[i].attr2.underline, "%c underlined", cells[i].ch);
		ck_assert_msg(!cells[i].attr2.bold, "%c bold", cells[i].ch);
	}
	/* SGR 4 itself still underlines, and the query leaves it on */
	ck_assert_uint_eq(cells[4].ch, 'U');
	ck_assert_uint_eq(cells[4].attr2.underline, 1);
	ck_assert_uint_eq(cells[5].ch, 'V');
	ck_assert_uint_eq(cells[5].attr2.underline, 1);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* draw2 shows a cluster's base codepoint, so a combining mark does not blank
 * the letter it is on */
START_TEST(test_vte_draw2_cluster_base)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_screen_cell *cells;
	const char *input = "e\xcc\x81" "a";	/* e U+0301 a */
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	tsm_vte_input(vte, input, strlen(input));

	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 1);
	cells = tsm_screen_draw2(screen);
	ck_assert_ptr_ne(cells, NULL);
	ck_assert_uint_eq(cells[0].ch, 'e');
	ck_assert_uint_eq(cells[1].ch, 'a');

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

static void input_str(struct tsm_vte *vte, const char *s)
{
	tsm_vte_input(vte, s, strlen(s));
}

/* the cluster table holds only what the screen and scrollback hold: every
 * way a cell is overwritten, erased or dropped releases its cluster */
START_TEST(test_vte_cluster_release)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	unsigned int i;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 3);
	ck_assert_int_eq(r, 0);
	tsm_screen_set_max_sb(screen, 2);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* overwritten in place */
	input_str(vte, "e\xcc\x81\r" "x");
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 0);

	/* erased */
	input_str(vte, "\re\xcc\x81" "a\xcc\x80" "\e[2K");
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 0);

	/* deleted and pushed off the line */
	input_str(vte, "\re\xcc\x81" "a\xcc\x80" "\r\e[P");
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 1);
	input_str(vte, "\r\e[10@");
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 0);

	/* shared by two cells, copied by REP, released by both */
	input_str(vte, "\re\xcc\x81\e[3b");
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 1);
	input_str(vte, "\e[2K");
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 0);

	/* scrolled into the scrollback, then out past its limit of 2 lines */
	input_str(vte, "\re\xcc\x81\r\na\xcc\x80\r\no\xcc\x82\r\n");
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 3);
	for (i = 0; i < 5; ++i)
		input_str(vte, "\r\n");
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 0);

	/* the scrollback cleared */
	input_str(vte, "e\xcc\x81\r\n\r\n\r\n\r\n");
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 1);
	tsm_screen_clear_sb(screen);
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 0);

	/* the alternate screen cleared, and a hard reset */
	input_str(vte, "\e[?1049h" "e\xcc\x81" "\e[?1049l" "a\xcc\x80");
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 2);
	input_str(vte, "\e[?1049h");
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 1);
	input_str(vte, "\e[?1049l\ec");
	ck_assert_uint_eq(tsm_symbol_table_count(screen->sym_table), 0);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

static char decrqm_reply[64];

static void decrqm_write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	UNUSED(vte);
	UNUSED(data);
	ck_assert_uint_lt(len, sizeof(decrqm_reply));
	memcpy(decrqm_reply, u8, len);
	decrqm_reply[len] = 0;
}

/* RIS and DECSTR return mode 2027 to the configured method, here wcwidth */
START_TEST(test_vte_width_method_resets)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, decrqm_write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_set_width_method(vte, TSM_WIDTH_WCWIDTH);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_set_width_method(vte, 7);
	ck_assert_int_eq(r, -EINVAL);

	input_str(vte, "\e[?2027$p");
	ck_assert_str_eq(decrqm_reply, "\e[?2027;2$y");
	input_str(vte, "\e[?2027h\e[?2027$p");
	ck_assert_str_eq(decrqm_reply, "\e[?2027;1$y");
	input_str(vte, "\ec\e[?2027$p");
	ck_assert_str_eq(decrqm_reply, "\e[?2027;2$y");
	input_str(vte, "\e[?2027h\e[!p\e[?2027$p");
	ck_assert_str_eq(decrqm_reply, "\e[?2027;2$y");

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

static uint64_t first_id;

static int id_draw_cb(struct tsm_screen *con, uint64_t id, const uint32_t *ch,
		      size_t len, unsigned int width, unsigned int posx,
		      unsigned int posy, const struct tsm_screen_attr *attr,
		      tsm_age_t age, void *data)
{
	UNUSED(con);
	UNUSED(ch);
	UNUSED(len);
	UNUSED(width);
	UNUSED(attr);
	UNUSED(age);
	UNUSED(data);
	if (!posx && !posy)
		first_id = id;
	return 0;
}

/* a cluster key reused for different content gets a different draw id, so a
 * glyph cache keyed by id never shows the old cluster */
START_TEST(test_vte_draw_id_cluster_reuse)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	uint64_t old_id;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	input_str(vte, "e\xcc\x81");
	tsm_screen_draw(screen, id_draw_cb, NULL);
	old_id = first_id;
	ck_assert_uint_ge(old_id & TSM_UCS4_MAX, TSM_CLUSTER_BASE);

	/* the key is freed, then taken by a different cluster */
	input_str(vte, "\e[2K\ra\xcc\x80");
	tsm_screen_draw(screen, id_draw_cb, NULL);
	ck_assert_uint_eq(first_id & TSM_UCS4_MAX, old_id & TSM_UCS4_MAX);
	ck_assert(first_id != old_id);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

static unsigned int frame_count_changed(const struct tsm_frame *f)
{
	unsigned int i, n = 0;

	for (i = 0; i < f->cols * f->rows; i++)
		if (tsm_frame_cell_at(f, i % f->cols, i / f->cols)->flags &
		    TSM_FRAME_CHANGED)
			n++;
	return n;
}

static bool frame_changed(const struct tsm_frame *f, unsigned int x, unsigned int y)
{
	return tsm_frame_cell_at(f, x, y)->flags & TSM_FRAME_CHANGED;
}

/* a cluster's key stays the same across frames while its cell is untouched,
 * and reads back as its codepoints */
START_TEST(test_vte_frame_cluster_key)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	const uint32_t *cps;
	uint64_t key;
	size_t len;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 3);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	input_str(vte, "e\xcc\x81");
	f = tsm_screen_frame(screen);
	ck_assert_ptr_ne(f, NULL);
	key = f->cells[0].key;
	ck_assert_uint_ge(key & 0xffffffffu, TSM_FRAME_CLUSTER_KEY);
	ck_assert_uint_ne(key >> 32, 0);
	cps = tsm_screen_frame_cluster(screen, key, &len);
	ck_assert_ptr_ne(cps, NULL);
	ck_assert_uint_eq(len, 2);
	ck_assert_uint_eq(cps[0], 'e');
	ck_assert_uint_eq(cps[1], 0x301);
	ck_assert_ptr_eq(tsm_screen_frame_cluster(screen, 'e', &len), NULL);

	/* elsewhere on the screen */
	input_str(vte, "\e[3;5Hy");
	f = tsm_screen_frame(screen);
	ck_assert(f->cells[0].key == key);
	ck_assert(!frame_changed(f, 0, 0));

	/* gone: its key no longer reads */
	input_str(vte, "\e[1;1H\e[K");
	ck_assert_ptr_eq(tsm_screen_frame_cluster(screen, key, &len), NULL);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* the changed flag follows the screen's age: a frame with nothing new marks
 * nothing; a print marks its cell and the cursor's old and new cells; a
 * cursor move marks the two cursor cells; erase, scroll, palette and
 * selection mark what they touch */
START_TEST(test_vte_frame_changed)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 4);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	input_str(vte, "ab");
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 40);	/* the first frame */
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	/* a print at 5,1: its cell, and the cursor cells at 2,0 and 6,1 */
	input_str(vte, "\e[2;6Hz");
	f = tsm_screen_frame(screen);
	ck_assert(frame_changed(f, 5, 1));
	ck_assert(frame_changed(f, 2, 0));
	ck_assert(frame_changed(f, 6, 1));
	ck_assert_uint_eq(frame_count_changed(f), 3);

	/* a cursor move: the two cursor cells */
	input_str(vte, "\e[4;1H");
	f = tsm_screen_frame(screen);
	ck_assert(frame_changed(f, 6, 1));
	ck_assert(frame_changed(f, 0, 3));
	ck_assert_uint_eq(frame_count_changed(f), 2);

	/* a cursor style change: the cursor cell */
	input_str(vte, "\e[4 q");
	f = tsm_screen_frame(screen);
	ck_assert(frame_changed(f, 0, 3));
	ck_assert_uint_eq(frame_count_changed(f), 1);

	/* an erase marks its row and the cursor's old cell, and no other */
	input_str(vte, "\e[1;1H\e[K");
	f = tsm_screen_frame(screen);
	ck_assert(frame_changed(f, 0, 0) && frame_changed(f, 9, 0));
	ck_assert(frame_changed(f, 0, 3));
	ck_assert(!frame_changed(f, 5, 1));
	ck_assert_uint_eq(frame_count_changed(f), 11);

	/* a scroll, a palette change and a selection move every visible cell */
	input_str(vte, "\e[S");
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	r = tsm_vte_set_palette(vte, "vga");
	ck_assert_int_eq(r, 0);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 0);
	tsm_screen_selection_start(screen, 1, 1);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* two callers of one screen, as two displays of one console: each sees a
 * change once, in its own frame, whichever draws first and whatever lands
 * between them; tsm_screen_frame is a caller of its own */
START_TEST(test_vte_frame_since_callers)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	struct tsm_frame_age a = { 0 }, b = { 0 };
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 4);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* a fresh caller draws everything, then nothing while nothing changes */
	input_str(vte, "ab");
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 0);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	/* a print at 5,1: its cell and the cursor cells at 2,0 and 6,1, for
	 * each caller */
	input_str(vte, "\e[2;6Hz");
	f = tsm_screen_frame_since(screen, &a);
	ck_assert(frame_changed(f, 5, 1));
	ck_assert(frame_changed(f, 2, 0));
	ck_assert(frame_changed(f, 6, 1));
	ck_assert_uint_eq(frame_count_changed(f), 3);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert(frame_changed(f, 5, 1));
	ck_assert(frame_changed(f, 2, 0));
	ck_assert(frame_changed(f, 6, 1));
	ck_assert_uint_eq(frame_count_changed(f), 3);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 0);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	/* b draws first, between a print at 0,0 and a cursor move: a sees
	 * both, b the move alone */
	input_str(vte, "\e[1;1Hq");
	f = tsm_screen_frame_since(screen, &b);
	ck_assert(frame_changed(f, 0, 0));
	ck_assert(frame_changed(f, 1, 0));
	ck_assert(frame_changed(f, 6, 1));
	ck_assert_uint_eq(frame_count_changed(f), 3);
	input_str(vte, "\e[4;1H");
	f = tsm_screen_frame_since(screen, &a);
	ck_assert(frame_changed(f, 0, 0));
	ck_assert(frame_changed(f, 1, 0));
	ck_assert(frame_changed(f, 6, 1));
	ck_assert(frame_changed(f, 0, 3));
	ck_assert_uint_eq(frame_count_changed(f), 4);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert(frame_changed(f, 1, 0));
	ck_assert(frame_changed(f, 0, 3));
	ck_assert_uint_eq(frame_count_changed(f), 2);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 0);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	/* tsm_screen_frame takes nothing from the others, nor they from it:
	 * its frame has every change since its own previous one */
	input_str(vte, "\e[4 q");
	f = tsm_screen_frame(screen);
	ck_assert(frame_changed(f, 5, 1));
	ck_assert(frame_changed(f, 0, 0));
	ck_assert(frame_changed(f, 0, 3));
	ck_assert_uint_eq(frame_count_changed(f), 6);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert(frame_changed(f, 0, 3));
	ck_assert_uint_eq(frame_count_changed(f), 1);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert(frame_changed(f, 0, 3));
	ck_assert_uint_eq(frame_count_changed(f), 1);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	/* a selection and a resize move every cell, for every caller */
	tsm_screen_selection_start(screen, 1, 1);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	r = tsm_screen_resize(screen, 12, 4);
	ck_assert_int_eq(r, 0);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 48);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(frame_count_changed(f), 48);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	/* a zeroed age asks for everything again */
	memset(&a, 0, sizeof(a));
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 48);

	ck_assert_ptr_eq(tsm_screen_frame_since(NULL, &a), NULL);
	ck_assert_ptr_eq(tsm_screen_frame_since(screen, NULL), NULL);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* the age counter wraps: an age from before the wrap draws everything once,
 * however it compares with the ages after it, and an unchanged screen draws
 * nothing after that, whether the screen, the scrollback or the alternate
 * screen is in view */
START_TEST(test_vte_frame_since_wrap)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	struct tsm_frame_age a = { 0 }, early = { 0 }, idle = { 0 };
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 4);
	ck_assert_int_eq(r, 0);
	tsm_screen_set_max_sb(screen, 4);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* early's age is small */
	input_str(vte, "q");
	tsm_screen_frame_since(screen, &early);
	tsm_screen_frame_since(screen, &idle);
	tsm_screen_frame(screen);

	/* after many changes, the counter near the top of its range: a line
	 * goes to the scrollback, the alternate screen is written, the screen
	 * is erased and rewritten, and the screen's own age moves, all with
	 * ages there */
	screen->age_cnt = (tsm_age_t)-60;
	input_str(vte, "ab\n\n\n\nc");
	input_str(vte, "\e[?47h\e[1;1Halt\e[?47l");
	tsm_screen_erase_screen(screen, false);
	input_str(vte, "\e[2;6Hz");
	f = tsm_screen_frame_since(screen, &a);
	ck_assert(frame_changed(f, 5, 1));
	ck_assert_uint_eq(screen->age_epoch, 0);

	/* the counter wraps, and climbs past early's age */
	while (!screen->age_epoch)
		input_str(vte, "\e[3;1Hd");
	while (screen->age_cnt <= early.age)
		input_str(vte, "\e[3;2He");
	ck_assert_uint_eq(screen->age_epoch, 1);
	ck_assert_uint_lt(screen->age_cnt, a.age);

	/* every age from before the wrap draws everything, once */
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	f = tsm_screen_frame_since(screen, &early);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 0);
	f = tsm_screen_frame_since(screen, &early);
	ck_assert_uint_eq(frame_count_changed(f), 0);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	/* the scrollback in view: a view move redraws everything, and after
	 * it the line from before the wrap reads as unchanged */
	tsm_screen_sb_up(screen, 1);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	ck_assert_uint_eq(tsm_frame_cell_at(f, 1, 0)->key, 'a');
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 0);
	tsm_screen_sb_reset(screen);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	/* the alternate screen in view, the same */
	input_str(vte, "\e[?47h");
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	ck_assert_uint_eq(tsm_frame_cell_at(f, 0, 0)->key, 'a');
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 0);
	input_str(vte, "\e[?47l");
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	f = tsm_screen_frame_since(screen, &early);
	ck_assert_uint_eq(frame_count_changed(f), 40);

	/* a change after the wrap reaches each caller once */
	input_str(vte, "\e[4;5Hx");
	f = tsm_screen_frame_since(screen, &a);
	ck_assert(frame_changed(f, 4, 3));
	ck_assert_uint_eq(frame_count_changed(f), 3);
	f = tsm_screen_frame_since(screen, &early);
	ck_assert(frame_changed(f, 4, 3));
	ck_assert_uint_eq(frame_count_changed(f), 3);

	/* a caller that drew nothing across the wrap draws everything */
	f = tsm_screen_frame_since(screen, &idle);
	ck_assert_uint_eq(frame_count_changed(f), 40);
	f = tsm_screen_frame_since(screen, &idle);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

struct draw_newer {
	tsm_age_t prev;
	unsigned int count;
};

/* counts the cells tsm_screen_draw asks to redraw against data's prev: age 0
 * (redraw everything) or newer */
static int newer_draw_cb(struct tsm_screen *con, uint64_t id, const uint32_t *ch,
			 size_t len, unsigned int width, unsigned int posx,
			 unsigned int posy, const struct tsm_screen_attr *attr,
			 tsm_age_t age, void *data)
{
	struct draw_newer *n = data;

	UNUSED(con);
	UNUSED(id);
	UNUSED(ch);
	UNUSED(len);
	UNUSED(width);
	UNUSED(posx);
	UNUSED(posy);
	UNUSED(attr);
	if (!age || age > n->prev)
		n->count++;
	return 0;
}

/* a frame leaves the age-reset flag to tsm_screen_draw: a draw user on the
 * same screen still redraws everything after a wrap, whoever drew first */
START_TEST(test_vte_frame_keeps_draw_reset)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct draw_newer n = { 0 };
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 4);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	screen->age_cnt = (tsm_age_t)-60;
	input_str(vte, "ab");
	n.prev = tsm_screen_draw(screen, newer_draw_cb, &n);
	ck_assert_uint_ne(n.prev, 0);

	while (!screen->age_epoch)
		input_str(vte, "\e[2;1Hd");
	tsm_screen_frame(screen);
	input_str(vte, "\e[3;1Hx");

	n.count = 0;
	tsm_screen_draw(screen, newer_draw_cb, &n);
	ck_assert_uint_eq(n.count, 40);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* width comes from the cell: a VS16-widened cluster is two wide, and its
 * trailing half zero wide with no glyph */
START_TEST(test_vte_frame_width)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 2);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	input_str(vte, "\xe2\x9d\xa4\xef\xb8\x8f" "a");	/* U+2764 U+FE0F a */
	f = tsm_screen_frame(screen);
	ck_assert_uint_ge(f->cells[0].key & 0xffffffffu, TSM_FRAME_CLUSTER_KEY);
	ck_assert_uint_eq(f->cells[0].width, 2);
	ck_assert_uint_eq(f->cells[1].width, 0);
	ck_assert(f->cells[1].key == 0);
	ck_assert(f->cells[2].key == 'a');
	ck_assert_uint_eq(f->cells[2].width, 1);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* colours: a default underline follows the foreground as drawn, through
 * reverse video and in the block cursor; a set one stays; style and strike
 * come through */
START_TEST(test_vte_frame_colors)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	const struct tsm_frame_cell *c;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 2);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_set_palette(vte, "legacy");
	ck_assert_int_eq(r, 0);

	input_str(vte, "\e[7;4:3mR\e[58;2;1;2;3mS\e[0;9mT\e[0m\e[2;1H");
	f = tsm_screen_frame(screen);

	/* R: reverse, default underline is the swapped fg, the default bg */
	c = &f->cells[0];
	ck_assert(c->flags & TSM_FRAME_INVERSE);
	ck_assert_uint_eq(c->fg.r, 0);
	ck_assert_uint_eq(c->bg.r, 229);
	ck_assert_uint_eq(c->ul.r, 0);
	ck_assert(!(c->flags & TSM_FRAME_UL_SET));
	ck_assert_uint_eq(c->underline, TSM_UNDERLINE_CURLY);
	/* S: a set underline stays under reverse video */
	c = &f->cells[1];
	ck_assert(c->flags & TSM_FRAME_UL_SET);
	ck_assert_uint_eq(c->ul.r, 1);
	ck_assert_uint_eq(c->ul.g, 2);
	ck_assert_uint_eq(c->ul.b, 3);
	/* T: strike */
	ck_assert(f->cells[2].flags & TSM_FRAME_STRIKE);
	ck_assert_uint_eq(f->cells[2].underline, TSM_UNDERLINE_NONE);

	/* the default block cursor swaps the cell's colours, and its default
	 * underline follows the swapped fg */
	ck_assert(f->cursor.visible);
	ck_assert_uint_eq(f->cursor.x, 0);
	ck_assert_uint_eq(f->cursor.y, 1);
	ck_assert_uint_eq(f->cursor.cell.fg.r, 0);
	ck_assert_uint_eq(f->cursor.cell.bg.r, 229);
	ck_assert_uint_eq(f->cursor.cell.ul.r, 0);

	/* an underline cursor toggles a single underline */
	input_str(vte, "\e[4 q");
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(f->cursor.style, TSM_SCREEN_CURSOR_UNDERLINE_STEADY);
	ck_assert_uint_eq(f->cursor.cell.underline, TSM_UNDERLINE_SINGLE);
	ck_assert_uint_eq(f->cursor.cell.fg.r, 229);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* a resize flags every cell of the next frame, whatever changed shape */
START_TEST(test_vte_frame_resize)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 4);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	input_str(vte, "abc");
	tsm_screen_frame(screen);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	/* wider, same height */
	r = tsm_screen_resize(screen, 12, 4);
	ck_assert_int_eq(r, 0);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(f->cols, 12);
	ck_assert_uint_eq(frame_count_changed(f), 48);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	/* shorter, same width */
	r = tsm_screen_resize(screen, 12, 3);
	ck_assert_int_eq(r, 0);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(frame_count_changed(f), 36);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* with scrollback in view, the cursor draws on the row that shows its line,
 * or not at all, and a move flags its cells where they show */
START_TEST(test_vte_frame_cursor_scrollback)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 4);
	ck_assert_int_eq(r, 0);
	tsm_screen_set_max_sb(screen, 100);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* lines 5-8 on screen, the cursor after "8" on the last row */
	input_str(vte, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8");
	f = tsm_screen_frame(screen);
	ck_assert(f->cursor.visible);
	ck_assert_uint_eq(f->cursor.x, 1);
	ck_assert_uint_eq(f->cursor.y, 3);

	/* two lines up: rows 2 and 3 show the screen's first two lines, and
	 * the cursor's line is out of view */
	tsm_screen_sb_up(screen, 2);
	tsm_screen_frame(screen);
	f = tsm_screen_frame(screen);
	ck_assert(!f->cursor.visible);
	ck_assert_uint_eq(frame_count_changed(f), 0);

	/* to the screen's second line: row 3 */
	input_str(vte, "\e[2;5H");
	f = tsm_screen_frame(screen);
	ck_assert(f->cursor.visible);
	ck_assert_uint_eq(f->cursor.x, 4);
	ck_assert_uint_eq(f->cursor.y, 3);
	ck_assert(frame_changed(f, 4, 3));
	ck_assert_uint_eq(frame_count_changed(f), 1);

	/* along that line: the old cell and the new */
	input_str(vte, "\e[2;8H");
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(f->cursor.x, 7);
	ck_assert_uint_eq(f->cursor.y, 3);
	ck_assert(frame_changed(f, 4, 3) && frame_changed(f, 7, 3));
	ck_assert_uint_eq(frame_count_changed(f), 2);

	/* back to the live screen: line 2 is row 1 again */
	tsm_screen_sb_reset(screen);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(f->cursor.y, 1);
	ck_assert_uint_eq(frame_count_changed(f), 40);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* an erase or a write that cuts a wide character takes both its halves, and
 * both are flagged */
START_TEST(test_vte_frame_wide_cut)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	const struct tsm_frame_cell *c0, *c1;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 2);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* ECH on the leading half, then on the trailing half */
	input_str(vte, "\xe4\xb8\xadZ");		/* U+4E2D, Z */
	tsm_screen_frame(screen);
	input_str(vte, "\e[1;1H\e[X");
	f = tsm_screen_frame(screen);
	c0 = tsm_frame_cell_at(f, 0, 0);
	c1 = tsm_frame_cell_at(f, 1, 0);
	ck_assert(c0->key == 0 && c0->width == 1);
	ck_assert(c1->key == 0 && c1->width == 1);
	ck_assert(frame_changed(f, 0, 0) && frame_changed(f, 1, 0));
	ck_assert(!frame_changed(f, 2, 0));
	ck_assert(tsm_frame_cell_at(f, 2, 0)->key == 'Z');

	input_str(vte, "\e[1;1H\xe4\xb8\xad\e[1;2H\e[X");
	f = tsm_screen_frame(screen);
	c0 = tsm_frame_cell_at(f, 0, 0);
	c1 = tsm_frame_cell_at(f, 1, 0);
	ck_assert(c0->key == 0 && c0->width == 1);
	ck_assert(c1->key == 0 && c1->width == 1);
	ck_assert(frame_changed(f, 0, 0) && frame_changed(f, 1, 0));

	/* a narrow character over the leading half, then over the trailing */
	input_str(vte, "\e[1;1H\xe4\xb8\xad\e[1;1Ha");
	f = tsm_screen_frame(screen);
	c0 = tsm_frame_cell_at(f, 0, 0);
	c1 = tsm_frame_cell_at(f, 1, 0);
	ck_assert(c0->key == 'a' && c0->width == 1);
	ck_assert(c1->key == 0 && c1->width == 1);
	ck_assert(frame_changed(f, 1, 0));

	input_str(vte, "\e[1;1H\xe4\xb8\xad\e[1;2Hb");
	f = tsm_screen_frame(screen);
	c0 = tsm_frame_cell_at(f, 0, 0);
	c1 = tsm_frame_cell_at(f, 1, 0);
	ck_assert(c0->key == 0 && c0->width == 1);
	ck_assert(c1->key == 'b' && c1->width == 1);
	ck_assert(frame_changed(f, 0, 0) && frame_changed(f, 1, 0));

	/* ICH and DCH on the trailing half */
	input_str(vte, "\e[2J\e[1;1H\xe4\xb8\xadZ\e[1;2H\e[@");
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 0, 0)->key == 0);
	ck_assert_uint_eq(tsm_frame_cell_at(f, 0, 0)->width, 1);
	ck_assert(tsm_frame_cell_at(f, 1, 0)->key == 0);
	ck_assert_uint_eq(tsm_frame_cell_at(f, 1, 0)->width, 1);
	ck_assert(tsm_frame_cell_at(f, 2, 0)->key == 0);
	ck_assert_uint_eq(tsm_frame_cell_at(f, 2, 0)->width, 1);
	ck_assert(tsm_frame_cell_at(f, 3, 0)->key == 'Z');

	input_str(vte, "\e[2J\e[1;1H\xe4\xb8\xadZ\e[1;2H\e[P");
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 0, 0)->key == 0);
	ck_assert_uint_eq(tsm_frame_cell_at(f, 0, 0)->width, 1);
	ck_assert(tsm_frame_cell_at(f, 1, 0)->key == 'Z');
	ck_assert_uint_eq(tsm_frame_cell_at(f, 1, 0)->width, 1);

	/* insert mode pushes a wide character across the margin: it goes */
	input_str(vte, "\e[2J\e[1;1Habcdefgh\xe4\xb8\xad\e[1;1H\e[4hx\e[4l");
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 0, 0)->key == 'x');
	ck_assert(tsm_frame_cell_at(f, 8, 0)->key == 'h');
	ck_assert(tsm_frame_cell_at(f, 9, 0)->key == 0);
	ck_assert_uint_eq(tsm_frame_cell_at(f, 9, 0)->width, 1);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* at the pending-wrap column the cursor draws on the last cell */
START_TEST(test_vte_frame_cursor_pending_wrap)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 2);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	input_str(vte, "0123456789");
	ck_assert_uint_eq(tsm_screen_get_cursor_x(screen), 10);
	f = tsm_screen_frame(screen);
	ck_assert(f->cursor.visible);
	ck_assert_uint_eq(f->cursor.x, 9);
	ck_assert_uint_eq(f->cursor.y, 0);
	ck_assert(f->cursor.cell.key == '9');

	/* leaving it flags the last cell */
	input_str(vte, "\r");
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(f->cursor.x, 0);
	ck_assert(frame_changed(f, 9, 0) && frame_changed(f, 0, 0));
	ck_assert_uint_eq(frame_count_changed(f), 2);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* on the trailing half of a wide character the cursor covers the character
 * from its leading cell, and leaving flags both halves */
START_TEST(test_vte_frame_cursor_trailing_half)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 2);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	input_str(vte, "\xe4\xb8\xad\e[1;2H");
	f = tsm_screen_frame(screen);
	ck_assert(f->cursor.visible);
	ck_assert_uint_eq(f->cursor.x, 0);
	ck_assert(f->cursor.cell.key == 0x4e2d);
	ck_assert_uint_eq(f->cursor.cell.width, 2);

	input_str(vte, "\e[2;1H");
	f = tsm_screen_frame(screen);
	ck_assert(frame_changed(f, 0, 0) && frame_changed(f, 1, 0));
	ck_assert(frame_changed(f, 0, 1));
	ck_assert_uint_eq(frame_count_changed(f), 3);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* SGR 7 and DECSCNM toggle each other, as tsm_screen_draw applies them */
START_TEST(test_vte_frame_inverse_screen)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	const struct tsm_frame_cell *c;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 2);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_set_palette(vte, "legacy");
	ck_assert_int_eq(r, 0);

	input_str(vte, "\e[7mR\e[0mN\e[?5h");
	f = tsm_screen_frame(screen);

	/* reverse video on a reverse-video screen: normal colours */
	c = tsm_frame_cell_at(f, 0, 0);
	ck_assert(!(c->flags & TSM_FRAME_INVERSE));
	ck_assert_uint_eq(c->fg.r, 229);
	ck_assert_uint_eq(c->bg.r, 0);
	/* a plain cell on it: inverse */
	c = tsm_frame_cell_at(f, 1, 0);
	ck_assert(c->flags & TSM_FRAME_INVERSE);
	ck_assert_uint_eq(c->fg.r, 0);
	ck_assert_uint_eq(c->bg.r, 229);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* frame widths of row @y as digits, for comparing a whole row at once */
static void frame_widths(const struct tsm_frame *f, unsigned int y, char *out)
{
	unsigned int x;

	for (x = 0; x < f->cols; ++x)
		out[x] = '0' + tsm_frame_cell_at(f, x, y)->width;
	out[f->cols] = 0;
}

/* REP prints the last printed character again, as printed text: it wraps
 * at the margin, honours insert mode, repeats a wide character whole, and
 * does nothing before the first print or after a reset */
START_TEST(test_vte_repeat_last_printed)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	char w[16];
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 8, 3);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* nothing printed yet */
	input_str(vte, "\e[3b");
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 0, 0)->key == 0);
	ck_assert_uint_eq(tsm_screen_get_cursor_x(screen), 0);

	/* REP 10 after "abcdef" wraps onto the next line */
	input_str(vte, "abcdef\e[10b");
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 7, 0)->key == 'f');
	ck_assert(tsm_frame_cell_at(f, 0, 1)->key == 'f');
	ck_assert(tsm_frame_cell_at(f, 7, 1)->key == 'f');
	ck_assert_uint_eq(tsm_screen_get_cursor_y(screen), 1);

	/* at the pending wrap after "abcdefgh", REP 1 repeats h on the next
	 * line */
	input_str(vte, "\e[2J\e[1;1Habcdefgh\e[b");
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 7, 0)->key == 'h');
	ck_assert(tsm_frame_cell_at(f, 6, 0)->key == 'g');
	ck_assert(tsm_frame_cell_at(f, 0, 1)->key == 'h');

	/* a wide character repeats whole, and the copy that does not fit in
	 * the last column wraps */
	input_str(vte, "\e[2J\e[1;1H\xe4\xb8\xad\e[3b");
	f = tsm_screen_frame(screen);
	frame_widths(f, 0, w);
	ck_assert_str_eq(w, "20202020");
	ck_assert(tsm_frame_cell_at(f, 6, 0)->key == 0x4e2d);
	input_str(vte, "\e[2J\e[1;1Habcde\xe4\xb8\xad\e[b");
	f = tsm_screen_frame(screen);
	frame_widths(f, 0, w);
	ck_assert_str_eq(w, "11111201");
	frame_widths(f, 1, w);
	ck_assert_str_eq(w, "20111111");
	ck_assert(tsm_frame_cell_at(f, 0, 1)->key == 0x4e2d);

	/* insert mode: the copies push the line right */
	input_str(vte, "\e[2J\e[1;1Habc\e[1;2H\e[4hx\e[2b\e[4l");
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 0, 0)->key == 'a');
	ck_assert(tsm_frame_cell_at(f, 1, 0)->key == 'x');
	ck_assert(tsm_frame_cell_at(f, 3, 0)->key == 'x');
	ck_assert(tsm_frame_cell_at(f, 4, 0)->key == 'b');
	ck_assert(tsm_frame_cell_at(f, 5, 0)->key == 'c');

	/* RIS and DECSTR forget the character */
	input_str(vte, "x\ec\e[3b");
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 0, 0)->key == 0);
	input_str(vte, "y\e[!p\e[1;1H\e[2K\e[3b");
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 0, 0)->key == 0);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* REP repeats the codepoint that started the last printed cell, the one
 * Ghostty keeps: after a base and a combining mark, the base. Unlike Ghostty,
 * each copy takes a cell of its own, also where the break rules would join
 * it to the cell before */
START_TEST(test_vte_repeat_cluster)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	char w[16];
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 8, 3);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* e and a combining acute, then REP 2: the e twice, in new cells */
	input_str(vte, "e\xcc\x81\e[2b");
	f = tsm_screen_frame(screen);
	ck_assert_uint_ge(tsm_frame_cell_at(f, 0, 0)->key & 0xffffffffu,
			  TSM_FRAME_CLUSTER_KEY);
	ck_assert(tsm_frame_cell_at(f, 1, 0)->key == 'e');
	ck_assert(tsm_frame_cell_at(f, 2, 0)->key == 'e');
	ck_assert(tsm_frame_cell_at(f, 3, 0)->key == 0);
	ck_assert_uint_eq(tsm_screen_get_cursor_x(screen), 3);

	/* under the break rules, a regional indicator would pair with the
	 * one before it, and a prepended mark would join what follows */
	input_str(vte, "\e[?2027h\e[2;1H\xf0\x9f\x87\xba\e[2b");
	f = tsm_screen_frame(screen);
	frame_widths(f, 1, w);
	ck_assert_str_eq(w, "20202011");
	ck_assert(tsm_frame_cell_at(f, 0, 1)->key == 0x1f1fa);
	ck_assert(tsm_frame_cell_at(f, 2, 1)->key == 0x1f1fa);
	ck_assert(tsm_frame_cell_at(f, 4, 1)->key == 0x1f1fa);
	input_str(vte, "\e[3;1H\xd8\x80\e[2b");
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 0, 2)->key == 0x600);
	ck_assert(tsm_frame_cell_at(f, 1, 2)->key == 0x600);
	ck_assert(tsm_frame_cell_at(f, 2, 2)->key == 0x600);
	ck_assert_uint_eq(tsm_screen_get_cursor_x(screen), 3);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* a wide character never sits cut at the margin: with one column left it
 * wraps, or without autowrap is not printed; on a narrowed screen an edit
 * that cuts one at the margin takes both halves, so none comes back half
 * when the screen widens */
START_TEST(test_vte_wide_at_margin)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	char w[16];
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 8, 3);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* autowrap off: not printed, the cursor stays */
	input_str(vte, "\e[?7labcdefg\xe4\xb8\xad");
	f = tsm_screen_frame(screen);
	frame_widths(f, 0, w);
	ck_assert_str_eq(w, "11111111");
	ck_assert(tsm_frame_cell_at(f, 6, 0)->key == 'g');
	ck_assert(tsm_frame_cell_at(f, 7, 0)->key == 0);
	ck_assert_uint_eq(tsm_screen_get_cursor_x(screen), 7);

	/* a VS16 that would widen the last cell leaves it narrow */
	input_str(vte, "\e[2;1Habcdefg\xe2\x9d\xa4\xef\xb8\x8f");
	f = tsm_screen_frame(screen);
	frame_widths(f, 1, w);
	ck_assert_str_eq(w, "11111111");

	/* autowrap on: the last cell is cleared and the character wraps */
	input_str(vte, "\e[?7h\e[2J\e[1;1Habcdefgh\e[1;8H\xe4\xb8\xad");
	f = tsm_screen_frame(screen);
	frame_widths(f, 0, w);
	ck_assert_str_eq(w, "11111111");
	ck_assert(tsm_frame_cell_at(f, 7, 0)->key == 0);
	frame_widths(f, 1, w);
	ck_assert_str_eq(w, "20111111");
	ck_assert(tsm_frame_cell_at(f, 0, 1)->key == 0x4e2d);

	/* on 10 columns at 7-8, narrowed to 8: the character stays whole past
	 * the margin and comes back whole */
	r = tsm_screen_resize(screen, 10, 3);
	ck_assert_int_eq(r, 0);
	input_str(vte, "\e[2J\e[1;1Habcdefg\xe4\xb8\xad");
	r = tsm_screen_resize(screen, 8, 3);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 3);
	ck_assert_int_eq(r, 0);
	f = tsm_screen_frame(screen);
	frame_widths(f, 0, w);
	ck_assert_str_eq(w, "1111111201");

	/* narrowed, then a write, an erase or a DCH at the margin: no half
	 * comes back */
	r = tsm_screen_resize(screen, 8, 3);
	ck_assert_int_eq(r, 0);
	input_str(vte, "\e[1;8HZ");
	r = tsm_screen_resize(screen, 10, 3);
	ck_assert_int_eq(r, 0);
	f = tsm_screen_frame(screen);
	frame_widths(f, 0, w);
	ck_assert_str_eq(w, "1111111111");
	ck_assert(tsm_frame_cell_at(f, 7, 0)->key == 'Z');

	input_str(vte, "\e[1;8H\xe4\xb8\xad");
	r = tsm_screen_resize(screen, 8, 3);
	ck_assert_int_eq(r, 0);
	input_str(vte, "\e[1;8H\e[K");
	r = tsm_screen_resize(screen, 10, 3);
	ck_assert_int_eq(r, 0);
	f = tsm_screen_frame(screen);
	frame_widths(f, 0, w);
	ck_assert_str_eq(w, "1111111111");

	input_str(vte, "\e[1;8H\xe4\xb8\xad");
	r = tsm_screen_resize(screen, 8, 3);
	ck_assert_int_eq(r, 0);
	input_str(vte, "\e[1;2H\e[P");
	r = tsm_screen_resize(screen, 10, 3);
	ck_assert_int_eq(r, 0);
	f = tsm_screen_frame(screen);
	frame_widths(f, 0, w);
	ck_assert_str_eq(w, "1111111111");

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);

	/* one column: no room anywhere, an empty cell is printed */
	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 1, 2);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	input_str(vte, "\xe4\xb8\xad");
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 0, 0)->key == 0);
	ck_assert_uint_eq(tsm_frame_cell_at(f, 0, 0)->width, 1);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* a VS16 that widens a cell takes the cell after it, or in insert mode
 * pushes the line right one cell; a wide character it cuts goes whole, also
 * at the margin of a narrowed screen */
START_TEST(test_vte_widen_at_margin)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	char w[16];
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 4, 2);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* the heart pushes the wide character into the last column */
	input_str(vte, "\e[1;2H\xe4\xb8\xad\e[1;1H\e[4h\xe2\x9d\xa4\xef\xb8\x8f\e[4l");
	f = tsm_screen_frame(screen);
	frame_widths(f, 0, w);
	ck_assert_str_eq(w, "2011");
	ck_assert(tsm_frame_cell_at(f, 2, 0)->key == 0);
	ck_assert(tsm_frame_cell_at(f, 3, 0)->key == 0);

	/* narrowed from 6 with the wide character across the margin: the
	 * push takes its lead, and its trailing half goes too */
	r = tsm_screen_resize(screen, 6, 2);
	ck_assert_int_eq(r, 0);
	input_str(vte, "\e[2J\e[1;1H\xe2\x9d\xa4" "bc\xe4\xb8\xad");
	r = tsm_screen_resize(screen, 4, 2);
	ck_assert_int_eq(r, 0);
	input_str(vte, "\e[1;2H\e[4h\xef\xb8\x8f\e[4l");
	r = tsm_screen_resize(screen, 6, 2);
	ck_assert_int_eq(r, 0);
	f = tsm_screen_frame(screen);
	frame_widths(f, 0, w);
	ck_assert_str_eq(w, "201111");
	ck_assert(tsm_frame_cell_at(f, 4, 0)->key == 0);

	/* the same without insert mode: the heart takes the lead of the wide
	 * character after it, whose trailing half lies past the margin */
	input_str(vte, "\e[2J\e[1;4H\xe4\xb8\xad\e[1;3H\xe2\x9d\xa4");
	r = tsm_screen_resize(screen, 4, 2);
	ck_assert_int_eq(r, 0);
	input_str(vte, "\e[1;4H\xef\xb8\x8f");
	r = tsm_screen_resize(screen, 6, 2);
	ck_assert_int_eq(r, 0);
	f = tsm_screen_frame(screen);
	frame_widths(f, 0, w);
	ck_assert_str_eq(w, "112011");

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* on a narrowed screen, a wide character lies across the margin: a line that
 * scrolls, IL or DL clears is cleared past the margin too, and a wide
 * character that wraps from the last column clears both halves of the one
 * it replaces, so no half comes back when the screen widens */
START_TEST(test_vte_clear_past_margin)
{
	/* the screen mode, the edit, and the widths of both rows after */
	static const struct {
		const char *mode, *edit, *row0, *row1;
	} edits[] = {
		/* scroll up on the alternate screen */
		{ "\e[?1049h", "\e[2;1H\n", "111201", "111111" },
		/* scroll down */
		{ "", "\e[1;1H\eM", "111111", "111201" },
		/* IL */
		{ "", "\e[2;1H\e[L", "111201", "111111" },
		/* DL */
		{ "", "\e[1;1H\e[M", "111201", "111111" },
	};
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	char w[16];
	unsigned int i;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* both rows hold a wide character at 3-4; the edit recycles one of
	 * them as an empty line, then both take "xyz" */
	for (i = 0; i < sizeof(edits) / sizeof(*edits); ++i) {
		r = tsm_screen_resize(screen, 6, 2);
		ck_assert_int_eq(r, 0);
		input_str(vte, "\ec");
		input_str(vte, edits[i].mode);
		input_str(vte, "\e[1;1Habc\xe4\xb8\xad\e[2;1Habc\xe4\xb8\xad");
		r = tsm_screen_resize(screen, 4, 2);
		ck_assert_int_eq(r, 0);
		input_str(vte, edits[i].edit);
		input_str(vte, "\e[1;1Hxyz\e[2;1Hxyz");
		r = tsm_screen_resize(screen, 6, 2);
		ck_assert_int_eq(r, 0);
		f = tsm_screen_frame(screen);
		frame_widths(f, 0, w);
		ck_assert_msg(!strcmp(w, edits[i].row0), "edit %u row 0: %s", i, w);
		frame_widths(f, 1, w);
		ck_assert_msg(!strcmp(w, edits[i].row1), "edit %u row 1: %s", i, w);
	}

	/* a wide character in the last column of the narrowed screen */
	r = tsm_screen_resize(screen, 6, 2);
	ck_assert_int_eq(r, 0);
	input_str(vte, "\ec\e[1;1Habc\xe4\xb8\xad");
	r = tsm_screen_resize(screen, 4, 2);
	ck_assert_int_eq(r, 0);
	input_str(vte, "\e[1;4H\xe4\xb8\x81");
	r = tsm_screen_resize(screen, 6, 2);
	ck_assert_int_eq(r, 0);
	f = tsm_screen_frame(screen);
	frame_widths(f, 0, w);
	ck_assert_str_eq(w, "111111");
	frame_widths(f, 1, w);
	ck_assert_str_eq(w, "201111");

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* a wide character's halves are protected together, so an erase that skips
 * protected cells keeps or clears both */
START_TEST(test_vte_protect_wide)
{
	struct tsm_screen *screen;
	struct tsm_screen_attr attr;
	const struct tsm_frame *f;
	const struct tsm_frame_cell *c0, *c1;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 2);
	ck_assert_int_eq(r, 0);
	memset(&attr, 0, sizeof(attr));
	attr.fr = attr.fg = attr.fb = 255;

	/* an unprotected wide character over a protected cell: a protected
	 * erase clears both halves */
	attr.protect = 1;
	tsm_screen_move_to(screen, 1, 0);
	tsm_screen_write(screen, 'X', &attr);
	attr.protect = 0;
	tsm_screen_move_to(screen, 0, 0);
	tsm_screen_write(screen, 0x4e2d, &attr);
	tsm_screen_erase_screen(screen, true);
	f = tsm_screen_frame(screen);
	c0 = tsm_frame_cell_at(f, 0, 0);
	c1 = tsm_frame_cell_at(f, 1, 0);
	ck_assert(c0->key == 0 && c0->width == 1);
	ck_assert(c1->key == 0 && c1->width == 1);
	ck_assert(!(c1->flags & TSM_FRAME_PROTECT));

	/* a protected wide character: a protected erase from its trailing
	 * half keeps both */
	tsm_screen_erase_screen(screen, false);
	attr.protect = 1;
	tsm_screen_move_to(screen, 0, 0);
	tsm_screen_write(screen, 0x4e2d, &attr);
	tsm_screen_move_to(screen, 1, 0);
	tsm_screen_erase_cursor_to_end(screen, true);
	f = tsm_screen_frame(screen);
	c0 = tsm_frame_cell_at(f, 0, 0);
	c1 = tsm_frame_cell_at(f, 1, 0);
	ck_assert(c0->key == 0x4e2d && c0->width == 2);
	ck_assert_uint_eq(c1->width, 0);
	ck_assert(c1->flags & TSM_FRAME_PROTECT);

	tsm_screen_unref(screen);
}
END_TEST

/* the cells of rows @y0-@y1, columns @x0-@x1, are blank with colours @fg and
 * @bg and nothing else */
static void check_blank(const struct tsm_frame *f, const char *path,
			unsigned int x0, unsigned int y0,
			unsigned int x1, unsigned int y1,
			struct tsm_screen_color fg, struct tsm_screen_color bg)
{
	const uint16_t pen = TSM_FRAME_BOLD | TSM_FRAME_ITALIC | TSM_FRAME_DIM |
			     TSM_FRAME_BLINK | TSM_FRAME_INVERSE |
			     TSM_FRAME_PROTECT | TSM_FRAME_STRIKE |
			     TSM_FRAME_UL_SET;
	const struct tsm_frame_cell *c;
	unsigned int x, y;

	for (y = y0; y <= y1; ++y) {
		for (x = x0; x <= x1; ++x) {
			c = tsm_frame_cell_at(f, x, y);
			ck_assert_msg(c->key == 0, "%s %u,%u: key", path, x, y);
			ck_assert_msg(c->underline == TSM_UNDERLINE_NONE,
				      "%s %u,%u: underline %u", path, x, y,
				      c->underline);
			ck_assert_msg(!(c->flags & pen), "%s %u,%u: flags %x",
				      path, x, y, c->flags);
			ck_assert_msg(!memcmp(&c->fg, &fg, sizeof(fg)),
				      "%s %u,%u: fg", path, x, y);
			ck_assert_msg(!memcmp(&c->bg, &bg, sizeof(bg)),
				      "%s %u,%u: bg", path, x, y);
		}
	}
}

struct row_attrs {
	unsigned int y;
	struct tsm_screen_attr attr[90];
};

/* keeps the attributes tsm_screen_draw reports for each cell of row y */
static int row_draw_cb(struct tsm_screen *con, uint64_t id, const uint32_t *ch,
		       size_t len, unsigned int width, unsigned int posx,
		       unsigned int posy, const struct tsm_screen_attr *attr,
		       tsm_age_t age, void *data)
{
	struct row_attrs *out = data;

	UNUSED(con);
	UNUSED(id);
	UNUSED(ch);
	UNUSED(len);
	UNUSED(width);
	UNUSED(age);
	if (posy == out->y && posx < 90)
		out->attr[posx] = *attr;
	return 0;
}

/* every erase leaves cells with the pen's background colour only: not its
 * foreground, rendition, underline, strike or reverse */
START_TEST(test_vte_erase_colours_only)
{
	static const char pen[] =
		"\e[1;2;3;5;9;4:2;58;2;9;9;9;38;5;1;44m";
	static const char *const bgs[] = {
		"\e[0;48;5;200m",		/* an index of the 256-colour table */
		"\e[0;48;2;10;20;30m",	/* direct colour */
	};
	struct row_attrs row;
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	struct tsm_screen_color fg, bg, blue;
	unsigned int i, x, y;
	char buf[32];
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 8, 6);
	ck_assert_int_eq(r, 0);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* the default colours, and the background SGR 44 gives */
	input_str(vte, "\e[44mX\e[0m");
	f = tsm_screen_frame(screen);
	blue = tsm_frame_cell_at(f, 0, 0)->bg;
	fg = tsm_frame_cell_at(f, 1, 0)->fg;
	bg = tsm_frame_cell_at(f, 1, 0)->bg;
	ck_assert(memcmp(&blue, &bg, sizeof(bg)));

	for (y = 1; y <= 6; ++y) {
		snprintf(buf, sizeof(buf), "\e[%u;1Habcdefgh", y);
		input_str(vte, buf);
	}
	input_str(vte, pen);

	input_str(vte, "\e[1;7H\e[K");		/* EL */
	input_str(vte, "\e[2;2H\e[2X");		/* ECH */
	input_str(vte, "\e[3;1H\e[2P");		/* DCH */
	input_str(vte, "\e[4;1H\e[2@");		/* ICH */
	input_str(vte, "\e[5;1H\e[L");		/* IL */
	input_str(vte, "\e[6;5H\e[J");		/* ED */
	f = tsm_screen_frame(screen);
	check_blank(f, "EL", 6, 0, 7, 0, fg, blue);
	check_blank(f, "ECH", 1, 1, 2, 1, fg, blue);
	check_blank(f, "DCH", 6, 2, 7, 2, fg, blue);
	check_blank(f, "ICH", 0, 3, 1, 3, fg, blue);
	check_blank(f, "IL", 0, 4, 7, 4, fg, blue);
	check_blank(f, "ED", 4, 5, 7, 5, fg, blue);

	/* a line scrolled in, the cell a wrapping wide character leaves, and
	 * the cells a resize adds past the 80 columns the screen began with */
	input_str(vte, "\e[6;1H\n\e[3;8H\xe4\xb8\xad");
	r = tsm_screen_resize(screen, 90, 6);
	ck_assert_int_eq(r, 0);
	f = tsm_screen_frame(screen);
	check_blank(f, "scroll", 0, 5, 7, 5, fg, blue);
	check_blank(f, "wrap", 7, 2, 7, 2, fg, blue);
	check_blank(f, "resize", 80, 0, 89, 5, fg, blue);

	/* the alternate screen's clear */
	input_str(vte, "\e[?1049h");
	f = tsm_screen_frame(screen);
	check_blank(f, "alt", 0, 0, 89, 5, fg, blue);
	input_str(vte, "\e[?1049l");

	/* DL's fill, and the erase attribute DECRC restores with the pen */
	input_str(vte, "\e[1;1H\e[M\e7\e[0m\e8\e[3;1H\e[K");
	f = tsm_screen_frame(screen);
	check_blank(f, "DL", 0, 5, 89, 5, fg, blue);
	check_blank(f, "DECRC", 0, 2, 89, 2, fg, blue);

	/* reverse video is not applied to an erased cell */
	input_str(vte, "\e[0m\e[7m\e[1;1H\e[K\e[44m\e[2;1H\e[K");
	f = tsm_screen_frame(screen);
	check_blank(f, "SGR 7", 0, 0, 89, 0, fg, bg);
	check_blank(f, "SGR 7;44", 0, 1, 89, 1, fg, blue);

	/* a background of each other kind: an erased cell matches a cell
	 * printed with only that background, in kind and value */
	for (i = 0; i < 2; ++i) {
		snprintf(buf, sizeof(buf), "\e[%u;1H%sX\e[K", i + 3, bgs[i]);
		input_str(vte, buf);
		f = tsm_screen_frame(screen);
		check_blank(f, bgs[i] + 1, 1, i + 2, 89, i + 2, fg,
			    tsm_frame_cell_at(f, 0, i + 2)->bg);

		memset(&row, 0, sizeof(row));
		row.y = i + 2;
		tsm_screen_draw(screen, row_draw_cb, &row);
		for (x = 1; x < 90; ++x) {
			ck_assert_msg(row.attr[x].bccode == row.attr[0].bccode,
				      "%s %u: bccode %d, printed %d", bgs[i] + 1,
				      x, row.attr[x].bccode,
				      row.attr[0].bccode);
			ck_assert_msg(row.attr[x].br == row.attr[0].br &&
				      row.attr[x].bg == row.attr[0].bg &&
				      row.attr[x].bb == row.attr[0].bb,
				      "%s %u: background", bgs[i] + 1, x);
		}
	}

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* a word selection on a space selects nothing, even when an earlier
 * selection's lines have left scrollback */
START_TEST(test_vte_word_select_space)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	char *out = NULL;
	unsigned int i;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 10, 4);
	ck_assert_int_eq(r, 0);
	tsm_screen_set_max_sb(screen, 2);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	input_str(vte, "\e[?25laaa\r\nbbb\r\nccc\r\nddd");
	tsm_screen_selection_start(screen, 0, 0);
	tsm_screen_selection_target(screen, 2, 1);
	/* both ends drop out of the two-line scrollback */
	input_str(vte, "\r\n1\r\n2\r\n3\r\n4\r\n5\r\n6\r\na b");
	tsm_screen_selection_word(screen, 1, 3);

	f = tsm_screen_frame(screen);
	for (i = 0; i < f->cols * f->rows; ++i)
		ck_assert(!(tsm_frame_cell_at(f, i % f->cols, i / f->cols)->flags &
			    TSM_FRAME_INVERSE));
	r = tsm_screen_selection_copy(screen, &out);
	ck_assert_int_eq(r, -ENOENT);
	ck_assert_ptr_eq(out, NULL);

	/* a word still selects */
	tsm_screen_selection_word(screen, 2, 3);
	r = tsm_screen_selection_copy(screen, &out);
	ck_assert_int_eq(r, 1);
	ck_assert_str_eq(out, "b");
	free(out);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

/* lowering the scrollback limit while scrollback is viewed keeps the view on
 * a line that still exists, or returns it to the screen */
START_TEST(test_vte_max_sb_while_viewed)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	const struct tsm_frame *f;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);
	r = tsm_screen_resize(screen, 8, 4);
	ck_assert_int_eq(r, 0);
	tsm_screen_set_max_sb(screen, 4);
	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* lines 1-3 in scrollback, 4-6 and an empty line on screen */
	input_str(vte, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n");
	ck_assert_uint_eq(tsm_screen_sb_get_line_count(screen), 3);

	/* viewing line 3: dropping line 1 keeps it, one place higher */
	tsm_screen_sb_up(screen, 1);
	ck_assert_uint_eq(tsm_screen_sb_get_line_pos(screen), 2);
	tsm_screen_set_max_sb(screen, 2);
	ck_assert_uint_eq(tsm_screen_sb_get_line_pos(screen), 1);
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 0, 0)->key == '3');

	/* viewing line 2, the top: dropping it passes the view to line 3 */
	tsm_screen_sb_up(screen, 1);
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 0, 0)->key == '2');
	tsm_screen_set_max_sb(screen, 1);	/* raised to 2: nothing goes */
	tsm_screen_set_max_sb(screen, 0);
	ck_assert_uint_eq(tsm_screen_sb_get_line_count(screen), 0);
	ck_assert_uint_eq(tsm_screen_sb_get_line_pos(screen), 0);

	/* no scrollback left: the view is the screen */
	f = tsm_screen_frame(screen);
	ck_assert(tsm_frame_cell_at(f, 0, 0)->key == '4');
	ck_assert(tsm_frame_cell_at(f, 0, 2)->key == '6');

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

TEST_DEFINE_CASE(misc)
	TEST(test_vte_init)
	TEST(test_vte_null)
	TEST(test_vte_custom_palette)
	TEST(test_vte_get_palette_color)
	TEST(test_vte_osc_query)
	TEST(test_vte_osc4)
	TEST(test_vte_backspace_key)
	TEST(test_vte_get_flags)
	TEST(test_vte_decrqm_no_reset)
	TEST(test_vte_csi_cursor_up_down)
	TEST(test_vte_palette_recolours_text)
	TEST(test_vte_draw2_underline_color)
	TEST(test_vte_underline_color_not_bright)
	TEST(test_vte_underline_color_follows_palette)
	TEST(test_vte_underline_color_dim)
	TEST(test_vte_modkeys_not_sgr)
	TEST(test_vte_draw2_cluster_base)
	TEST(test_vte_cluster_release)
	TEST(test_vte_width_method_resets)
	TEST(test_vte_draw_id_cluster_reuse)
	TEST(test_vte_frame_cluster_key)
	TEST(test_vte_frame_changed)
	TEST(test_vte_frame_since_callers)
	TEST(test_vte_frame_since_wrap)
	TEST(test_vte_frame_keeps_draw_reset)
	TEST(test_vte_frame_width)
	TEST(test_vte_frame_colors)
	TEST(test_vte_frame_resize)
	TEST(test_vte_frame_cursor_scrollback)
	TEST(test_vte_frame_wide_cut)
	TEST(test_vte_frame_cursor_pending_wrap)
	TEST(test_vte_frame_cursor_trailing_half)
	TEST(test_vte_frame_inverse_screen)
	TEST(test_vte_repeat_last_printed)
	TEST(test_vte_repeat_cluster)
	TEST(test_vte_wide_at_margin)
	TEST(test_vte_widen_at_margin)
	TEST(test_vte_clear_past_margin)
	TEST(test_vte_protect_wide)
	TEST(test_vte_erase_colours_only)
	TEST(test_vte_word_select_space)
	TEST(test_vte_max_sb_while_viewed)
TEST_END_CASE

TEST_DEFINE_CASE(bell)
	TEST(test_vte_bell)
	TEST(test_vte_bell_no_cb)
	TEST(test_vte_bell_data)
TEST_END_CASE

static unsigned int led_cb_leds = 0;
static bool led_cb_called = false;

static void led_cb(struct tsm_vte *vte, unsigned int leds, void *data)
{
	unsigned int *out = data;
	ck_assert_ptr_ne(vte, NULL);
	if (out)
		*out = leds;
	led_cb_leds = leds;
	led_cb_called = true;
}

START_TEST(test_vte_led_decll)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	tsm_vte_set_led_cb(vte, led_cb, NULL);

	/* CSI 1 q  — turn on Scroll Lock LED */
	led_cb_called = false;
	tsm_vte_input(vte, "\033[1q", 4);
	ck_assert(led_cb_called);
	ck_assert_uint_eq(led_cb_leds, TSM_VTE_LED_SCROLL_LOCK);

	/* CSI 3 q  — turn on Caps Lock LED (cumulative) */
	led_cb_called = false;
	tsm_vte_input(vte, "\033[3q", 4);
	ck_assert(led_cb_called);
	ck_assert_uint_eq(led_cb_leds, TSM_VTE_LED_SCROLL_LOCK | TSM_VTE_LED_CAPS_LOCK);

	/* CSI 0 q  — clear all LEDs */
	led_cb_called = false;
	tsm_vte_input(vte, "\033[0q", 4);
	ck_assert(led_cb_called);
	ck_assert_uint_eq(led_cb_leds, 0);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

START_TEST(test_vte_led_no_cb)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	/* DECLL without a registered callback must not crash */
	tsm_vte_input(vte, "\033[1q", 4);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

START_TEST(test_vte_led_data)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	unsigned int out = 0;
	int r;

	r = tsm_screen_new(&screen, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	r = tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL);
	ck_assert_int_eq(r, 0);

	tsm_vte_set_led_cb(vte, led_cb, &out);

	tsm_vte_input(vte, "\033[2q", 4);
	ck_assert_uint_eq(out, TSM_VTE_LED_NUM_LOCK);

	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}
END_TEST

TEST_DEFINE_CASE(led)
	TEST(test_vte_led_decll)
	TEST(test_vte_led_no_cb)
	TEST(test_vte_led_data)
TEST_END_CASE

// clang-format off
TEST_DEFINE(
	TEST_SUITE(vte,
		TEST_CASE(misc),
		TEST_CASE(bell),
		TEST_CASE(led),
		TEST_END
	)
)
// clang-format on
