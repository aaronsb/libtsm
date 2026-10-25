/*
 * TSM - VTE mode tests
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
 * One test per mode that has an effect (ADR-201 in aaronsb/kmscon): each
 * sets the mode, sees its effect, resets it and sees the effect go. The mode
 * table (src/tsm/tsm-modes.def) names these tests.
 */

#include <stdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "test_common.h"

static char out[256];
static size_t out_len;

static void write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	UNUSED(vte);
	UNUSED(data);
	ck_assert_uint_lt(out_len + len, sizeof(out));
	memcpy(out + out_len, u8, len);
	out_len += len;
	out[out_len] = 0;
}

static void out_clear(void)
{
	out_len = 0;
	out[0] = 0;
}

static unsigned int mouse_cb_calls;
static unsigned int mouse_cb_mode;
static bool mouse_cb_pixels;

static void mouse_cb(struct tsm_vte *vte, enum tsm_mouse_track_mode track_mode,
		     bool track_pixels, void *data)
{
	UNUSED(vte);
	UNUSED(data);
	++mouse_cb_calls;
	mouse_cb_mode = track_mode;
	mouse_cb_pixels = track_pixels;
}

static struct tsm_screen *screen;
static struct tsm_vte *vte;

static void setup(void)
{
	ck_assert_int_eq(tsm_screen_new(&screen, NULL, NULL), 0);
	ck_assert_int_eq(tsm_screen_resize(screen, 10, 5), 0);
	ck_assert_int_eq(tsm_vte_new(&vte, screen, write_cb, NULL, NULL, NULL), 0);
	tsm_vte_set_mouse_cb(vte, mouse_cb, NULL);
	mouse_cb_calls = 0;
	mouse_cb_mode = 0;
	mouse_cb_pixels = false;
	out_clear();
}

static void teardown(void)
{
	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}

static void input(const char *s)
{
	tsm_vte_input(vte, s, strlen(s));
}

static bool vte_flag(unsigned int flag)
{
	return tsm_vte_get_flags(vte) & flag;
}

static bool screen_flag(unsigned int flag)
{
	return tsm_screen_get_flags(screen) & flag;
}

/* the codepoint at @x, @y, 0 for an empty cell */
static uint64_t cell(unsigned int x, unsigned int y)
{
	return tsm_frame_cell_at(tsm_screen_frame(screen), x, y)->key;
}

static void assert_cursor(unsigned int x, unsigned int y)
{
	ck_assert_uint_eq(tsm_screen_get_cursor_x(screen), x);
	ck_assert_uint_eq(tsm_screen_get_cursor_y(screen), y);
}

/* ANSI modes */

START_TEST(test_mode_irm)
{
	input("ab\r\e[4hX");
	ck_assert(vte_flag(TSM_VTE_FLAG_INSERT_REPLACE_MODE));
	ck_assert(screen_flag(TSM_SCREEN_INSERT_MODE));
	ck_assert_uint_eq(cell(0, 0), 'X');
	ck_assert_uint_eq(cell(1, 0), 'a');
	ck_assert_uint_eq(cell(2, 0), 'b');

	input("\e[4lY");
	ck_assert(!vte_flag(TSM_VTE_FLAG_INSERT_REPLACE_MODE));
	ck_assert(!screen_flag(TSM_SCREEN_INSERT_MODE));
	ck_assert_uint_eq(cell(1, 0), 'Y');
	ck_assert_uint_eq(cell(2, 0), 'b');
}
END_TEST

START_TEST(test_mode_srm)
{
	/* set by default: what the host sends is not echoed */
	ck_assert(vte_flag(TSM_VTE_FLAG_SEND_RECEIVE_MODE));
	tsm_vte_handle_keyboard(vte, XKB_KEY_a, 'a', 0, 'a');
	ck_assert_str_eq(out, "a");
	ck_assert_uint_eq(cell(0, 0), 0);

	/* reset: local echo */
	input("\e[12l");
	ck_assert(!vte_flag(TSM_VTE_FLAG_SEND_RECEIVE_MODE));
	tsm_vte_handle_keyboard(vte, XKB_KEY_b, 'b', 0, 'b');
	ck_assert_uint_eq(cell(0, 0), 'b');

	input("\e[12h");
	ck_assert(vte_flag(TSM_VTE_FLAG_SEND_RECEIVE_MODE));
	tsm_vte_handle_keyboard(vte, XKB_KEY_c, 'c', 0, 'c');
	ck_assert_uint_eq(cell(1, 0), 0);
}
END_TEST

START_TEST(test_mode_lnm)
{
	/* set: LF returns too, and Return sends CR LF */
	input("ab\e[20h\nc");
	ck_assert(vte_flag(TSM_VTE_FLAG_LINE_FEED_NEW_LINE_MODE));
	assert_cursor(1, 1);
	tsm_vte_handle_keyboard(vte, XKB_KEY_Return, '\r', 0, '\r');
	ck_assert_str_eq(out, "\r\n");

	out_clear();
	input("\e[20l\nd");
	ck_assert(!vte_flag(TSM_VTE_FLAG_LINE_FEED_NEW_LINE_MODE));
	assert_cursor(2, 2);
	tsm_vte_handle_keyboard(vte, XKB_KEY_Return, '\r', 0, '\r');
	ck_assert_str_eq(out, "\r");
}
END_TEST

/* DEC private modes */

START_TEST(test_mode_decckm)
{
	input("\e[?1h");
	ck_assert(vte_flag(TSM_VTE_FLAG_CURSOR_KEY_MODE));
	tsm_vte_handle_keyboard(vte, XKB_KEY_Up, 0, 0, TSM_VTE_INVALID);
	ck_assert_str_eq(out, "\eOA");

	out_clear();
	input("\e[?1l");
	ck_assert(!vte_flag(TSM_VTE_FLAG_CURSOR_KEY_MODE));
	tsm_vte_handle_keyboard(vte, XKB_KEY_Up, 0, 0, TSM_VTE_INVALID);
	ck_assert_str_eq(out, "\e[A");
}
END_TEST

START_TEST(test_mode_decscnm)
{
	input("\e[?5h");
	ck_assert(vte_flag(TSM_VTE_FLAG_INVERSE_SCREEN_MODE));
	ck_assert(screen_flag(TSM_SCREEN_INVERSE));

	input("\e[?5l");
	ck_assert(!vte_flag(TSM_VTE_FLAG_INVERSE_SCREEN_MODE));
	ck_assert(!screen_flag(TSM_SCREEN_INVERSE));
}
END_TEST

START_TEST(test_mode_decom)
{
	/* set: CUP counts from the top margin */
	input("\e[2;4r\e[?6h\e[1;1H");
	ck_assert(vte_flag(TSM_VTE_FLAG_ORIGIN_MODE));
	ck_assert(screen_flag(TSM_SCREEN_REL_ORIGIN));
	assert_cursor(0, 1);

	input("\e[?6l\e[1;1H");
	ck_assert(!vte_flag(TSM_VTE_FLAG_ORIGIN_MODE));
	ck_assert(!screen_flag(TSM_SCREEN_REL_ORIGIN));
	assert_cursor(0, 0);
}
END_TEST

START_TEST(test_mode_decawm)
{
	/* set by default: the eleventh character wraps */
	ck_assert(vte_flag(TSM_VTE_FLAG_AUTO_WRAP_MODE));
	input("0123456789A");
	ck_assert_uint_eq(cell(0, 1), 'A');

	/* reset: it overwrites the last column */
	input("\e[?7l\e[3;1H0123456789B");
	ck_assert(!vte_flag(TSM_VTE_FLAG_AUTO_WRAP_MODE));
	ck_assert(!screen_flag(TSM_SCREEN_AUTO_WRAP));
	ck_assert_uint_eq(cell(9, 2), 'B');
	ck_assert_uint_eq(cell(0, 3), 0);

	input("\e[?7h\e[4;1H0123456789C");
	ck_assert(screen_flag(TSM_SCREEN_AUTO_WRAP));
	ck_assert_uint_eq(cell(0, 4), 'C');
}
END_TEST

START_TEST(test_mode_dectcem)
{
	ck_assert(!screen_flag(TSM_SCREEN_HIDE_CURSOR));
	input("\e[?25l");
	ck_assert(!vte_flag(TSM_VTE_FLAG_TEXT_CURSOR_MODE));
	ck_assert(screen_flag(TSM_SCREEN_HIDE_CURSOR));

	input("\e[?25h");
	ck_assert(vte_flag(TSM_VTE_FLAG_TEXT_CURSOR_MODE));
	ck_assert(!screen_flag(TSM_SCREEN_HIDE_CURSOR));
}
END_TEST

/* Mouse modes: one field holds 9, 1000, 1006 and 1016, another 1002 and
 * 1003 (aaronsb/libtsm#30); these pin that as it is */

START_TEST(test_mode_mouse_x10)
{
	input("\e[?9h");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 9);
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 1002);
	ck_assert_uint_eq(mouse_cb_calls, 1);
	ck_assert_uint_eq(mouse_cb_mode, 1002);

	/* a press is reported, a release is not */
	ck_assert(tsm_vte_handle_mouse(vte, 1, 2, 0, 0, 0, TSM_MOUSE_EVENT_PRESSED, 0));
	ck_assert_str_eq(out, "\e[M \"#");

	input("\e[?9l");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 0);
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 1002);
	ck_assert_uint_eq(mouse_cb_calls, 2);
}
END_TEST

START_TEST(test_mode_mouse_vt200)
{
	input("\e[?1000h");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 1000);
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 1002);
	ck_assert_uint_eq(mouse_cb_calls, 1);

	ck_assert(tsm_vte_handle_mouse(vte, 1, 2, 0, 0, 0, TSM_MOUSE_EVENT_RELEASED, 0));
	ck_assert_str_eq(out, "\e[M#\"#");

	input("\e[?1000l");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 0);
	ck_assert_uint_eq(mouse_cb_calls, 2);
}
END_TEST

START_TEST(test_mode_mouse_btn_event)
{
	/* with SGR: 1002 is the event field */
	input("\e[?1006h\e[?1002h");
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 1002);
	ck_assert_uint_eq(mouse_cb_mode, 1002);

	input("\e[?1002l");
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 0);
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 1006);

	/* with 1000, the event field stays 1002 whatever is asked */
	input("\e[?1000h\e[?1002l");
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 1002);
}
END_TEST

START_TEST(test_mode_mouse_any_event)
{
	input("\e[?1006h\e[?1003h");
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 1003);
	ck_assert_uint_eq(mouse_cb_mode, 1003);

	/* motion with no button is reported */
	ck_assert(tsm_vte_handle_mouse(vte, 1, 2, 0, 0, 3, TSM_MOUSE_EVENT_MOVED, 0));

	input("\e[?1003l");
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 0);
	out_clear();
	ck_assert(!tsm_vte_handle_mouse(vte, 1, 2, 0, 0, 3, TSM_MOUSE_EVENT_MOVED, 0));
	ck_assert_str_eq(out, "");
}
END_TEST

START_TEST(test_mode_mouse_sgr)
{
	input("\e[?1006h");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 1006);
	/* no event mode yet: the callback is not told */
	ck_assert_uint_eq(mouse_cb_calls, 0);

	ck_assert(tsm_vte_handle_mouse(vte, 1, 2, 0, 0, 0, TSM_MOUSE_EVENT_PRESSED, 0));
	ck_assert_str_eq(out, "\e[<0;2;3M");

	input("\e[?1006l");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 0);
	ck_assert_uint_eq(mouse_cb_calls, 1);
}
END_TEST

START_TEST(test_mode_mouse_pixel)
{
	input("\e[?1003h\e[?1016h");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 1016);
	ck_assert_uint_eq(mouse_cb_calls, 1);
	ck_assert(mouse_cb_pixels);

	ck_assert(tsm_vte_handle_mouse(vte, 1, 2, 17, 40, 0, TSM_MOUSE_EVENT_PRESSED, 0));
	ck_assert_str_eq(out, "\e[<0;17;40M");

	input("\e[?1016l");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 0);
	ck_assert_uint_eq(mouse_cb_calls, 2);
	ck_assert(!mouse_cb_pixels);
}
END_TEST

/* The alternate screen */

START_TEST(test_mode_altbuf_47)
{
	input("main\e[?47h");
	ck_assert(screen_flag(TSM_SCREEN_ALTERNATE));
	ck_assert_uint_eq(cell(0, 0), 0);
	assert_cursor(4, 0);
	input("\r\nalt");

	/* reset: the main screen, the cursor where the alternate left it */
	input("\e[?47l");
	ck_assert(!screen_flag(TSM_SCREEN_ALTERNATE));
	ck_assert_uint_eq(cell(0, 0), 'm');
	assert_cursor(3, 1);

	/* and the alternate screen kept its text */
	input("\e[?47h");
	ck_assert_uint_eq(cell(0, 1), 'a');
	input("\e[?47l");
}
END_TEST

START_TEST(test_mode_altbuf_1047)
{
	input("main\e[?1047halt");
	ck_assert(screen_flag(TSM_SCREEN_ALTERNATE));
	ck_assert_uint_eq(cell(4, 0), 'a');

	/* reset erases the alternate screen as it leaves */
	input("\e[?1047l");
	ck_assert(!screen_flag(TSM_SCREEN_ALTERNATE));
	ck_assert_uint_eq(cell(0, 0), 'm');
	input("\e[?47h");
	ck_assert_uint_eq(cell(4, 0), 0);
	input("\e[?47l");
}
END_TEST

START_TEST(test_mode_save_cursor_1048)
{
	input("\e[2;3H\e[?1048h\e[5;9H");
	assert_cursor(8, 4);
	input("\e[?1048l");
	assert_cursor(2, 1);
	ck_assert(!screen_flag(TSM_SCREEN_ALTERNATE));
}
END_TEST

START_TEST(test_mode_altbuf_1049)
{
	input("main\e[?47halt\e[?47l");
	input("\e[2;3H\e[?1049h");
	ck_assert(screen_flag(TSM_SCREEN_ALTERNATE));
	/* set erases the alternate screen as it enters */
	ck_assert_uint_eq(cell(0, 0), 0);
	input("\e[5;9H");

	input("\e[?1049l");
	ck_assert(!screen_flag(TSM_SCREEN_ALTERNATE));
	ck_assert_uint_eq(cell(0, 0), 'm');
	assert_cursor(2, 1);
}
END_TEST

/* Bracketed paste */

START_TEST(test_mode_bracketed_paste)
{
	input("\e[?2004h");
	tsm_vte_paste(vte, "x");
	ck_assert_str_eq(out, "\e[200~x\e[201~");

	out_clear();
	input("\e[?2004l");
	tsm_vte_paste(vte, "y");
	ck_assert_str_eq(out, "y");
}
END_TEST

TEST_DEFINE_CASE(ansi)
	CHECKED_FIXTURE(setup, teardown)
	TEST(test_mode_irm)
	TEST(test_mode_srm)
	TEST(test_mode_lnm)
TEST_END_CASE

TEST_DEFINE_CASE(dec)
	CHECKED_FIXTURE(setup, teardown)
	TEST(test_mode_decckm)
	TEST(test_mode_decscnm)
	TEST(test_mode_decom)
	TEST(test_mode_decawm)
	TEST(test_mode_dectcem)
	TEST(test_mode_mouse_x10)
	TEST(test_mode_mouse_vt200)
	TEST(test_mode_mouse_btn_event)
	TEST(test_mode_mouse_any_event)
	TEST(test_mode_mouse_sgr)
	TEST(test_mode_mouse_pixel)
	TEST(test_mode_altbuf_47)
	TEST(test_mode_altbuf_1047)
	TEST(test_mode_save_cursor_1048)
	TEST(test_mode_altbuf_1049)
	TEST(test_mode_bracketed_paste)
TEST_END_CASE

// clang-format off
TEST_DEFINE(
	TEST_SUITE(vte_modes,
		TEST_CASE(ansi),
		TEST_CASE(dec),
		TEST_END
	)
)
// clang-format on
