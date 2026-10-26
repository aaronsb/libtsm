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
#include <stdlib.h>
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

/* Mouse modes: two sets, as ctlseqs has them (aaronsb/libtsm#30). The
 * protocol, 9, 1000, 1002 or 1003, says which events are reported, and the
 * encoding, 1006 or 1016 (X10 when neither), how. The sets are independent;
 * within one, SM of a mode replaces the other and RM of any clears it. The
 * reports below are at column 1, row 2, pixels 9 and 40, which 1016 reports
 * from 1, as 10 and 41 (aaronsb/libtsm#67). */

/* sends the event at @col and row 2, pixels @col * 8 + 1 and 40, and
 * returns whether anything was written */
static bool mouse_sends(unsigned int col, unsigned int button, unsigned int event)
{
	out_clear();
	tsm_vte_handle_mouse(vte, col, 2, col * 8 + 1, 40, button, event, 0);
	return out_len > 0;
}

START_TEST(test_mode_mouse_x10)
{
	input("\e[?9h");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 9);
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 1002);
	ck_assert_uint_eq(mouse_cb_calls, 1);
	ck_assert_uint_eq(mouse_cb_mode, 1002);

	/* a press is reported, without its modifiers; a release, a drag and
	 * motion are not */
	ck_assert(tsm_vte_handle_mouse(vte, 1, 2, 0, 0, 0, TSM_MOUSE_EVENT_PRESSED,
				       TSM_MOUSE_MODIFIER_SHIFT));
	ck_assert_str_eq(out, "\e[M \"#");
	ck_assert(!mouse_sends(1, 0, TSM_MOUSE_EVENT_RELEASED));
	ck_assert(!mouse_sends(3, 32, TSM_MOUSE_EVENT_MOVED));
	ck_assert(!mouse_sends(5, 0, TSM_MOUSE_EVENT_MOVED));
	/* nor the wheel, which X10 has no code for */
	ck_assert(!mouse_sends(1, TSM_MOUSE_BUTTON_WHEEL_UP, TSM_MOUSE_EVENT_PRESSED));

	/* the same under the SGR encoding, set after it */
	input("\e[?1006h");
	ck_assert_uint_eq(mouse_cb_calls, 2);
	ck_assert(mouse_sends(1, 0, TSM_MOUSE_EVENT_PRESSED));
	ck_assert_str_eq(out, "\e[<0;2;3M");
	ck_assert(!mouse_sends(1, 0, TSM_MOUSE_EVENT_RELEASED));

	/* reset, nothing is reported, and the encoding stays */
	input("\e[?9l");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 0);
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 0);
	ck_assert_uint_eq(mouse_cb_calls, 3);
	ck_assert_uint_eq(mouse_cb_mode, 0);
	ck_assert(!mouse_sends(1, 0, TSM_MOUSE_EVENT_PRESSED));
	ck_assert_int_eq(tsm_vte_get_mode(vte, true, 1006), 1);

	/* the host hears only of a change: an RM with nothing tracked, or an
	 * SM of the mode already set, tells it nothing (kmscon drops the
	 * selection when told) */
	input("\e[?9l\e[?1003l\e[?1016l");
	ck_assert_uint_eq(mouse_cb_calls, 3);
	input("\e[?9h\e[?9h");
	ck_assert_uint_eq(mouse_cb_calls, 4);
}
END_TEST

START_TEST(test_mode_mouse_vt200)
{
	input("\e[?1000h");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 1000);
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 1002);
	ck_assert_uint_eq(mouse_cb_calls, 1);

	/* presses with their modifiers and releases; no motion */
	ck_assert(tsm_vte_handle_mouse(vte, 1, 2, 0, 0, 0, TSM_MOUSE_EVENT_PRESSED,
				       TSM_MOUSE_MODIFIER_SHIFT));
	ck_assert_str_eq(out, "\e[M$\"#");
	ck_assert(mouse_sends(1, 0, TSM_MOUSE_EVENT_RELEASED));
	ck_assert_str_eq(out, "\e[M#\"#");
	ck_assert(!mouse_sends(3, 32, TSM_MOUSE_EVENT_MOVED));
	ck_assert(!mouse_sends(5, 0, TSM_MOUSE_EVENT_MOVED));

	/* the wheel is a press of 64 or 65; its release is not reported */
	ck_assert(mouse_sends(1, TSM_MOUSE_BUTTON_WHEEL_UP, TSM_MOUSE_EVENT_PRESSED));
	ck_assert_str_eq(out, "\e[M`\"#");
	ck_assert(!mouse_sends(1, TSM_MOUSE_BUTTON_WHEEL_UP, TSM_MOUSE_EVENT_RELEASED));

	input("\e[?1000l");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 0);
	ck_assert_uint_eq(mouse_cb_calls, 2);
	ck_assert(!mouse_sends(1, 0, TSM_MOUSE_EVENT_PRESSED));
}
END_TEST

START_TEST(test_mode_mouse_btn_event)
{
	/* in the X10 encoding: a drag is its button plus 32, once a cell */
	input("\e[?1002h");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 1000);
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 1002);
	ck_assert_uint_eq(mouse_cb_mode, 1002);
	ck_assert(mouse_sends(3, 32, TSM_MOUSE_EVENT_MOVED));
	ck_assert_str_eq(out, "\e[M@$#");
	ck_assert(!mouse_sends(3, 32, TSM_MOUSE_EVENT_MOVED));
	ck_assert(!mouse_sends(5, 0, TSM_MOUSE_EVENT_MOVED));
	ck_assert(mouse_sends(1, 0, TSM_MOUSE_EVENT_PRESSED));
	ck_assert_str_eq(out, "\e[M \"#");

	/* a cell is reported once, whatever reported it last: motion within
	 * the cell of a press is not sent, and a drag back into the cell of
	 * an earlier motion is, after a press elsewhere */
	input("\e[?1006h");
	ck_assert(mouse_sends(5, 0, TSM_MOUSE_EVENT_PRESSED));
	ck_assert(!mouse_sends(5, 32, TSM_MOUSE_EVENT_MOVED));
	ck_assert(mouse_sends(6, 32, TSM_MOUSE_EVENT_MOVED));
	ck_assert(mouse_sends(6, 0, TSM_MOUSE_EVENT_RELEASED));
	ck_assert(mouse_sends(5, 0, TSM_MOUSE_EVENT_PRESSED));
	ck_assert(mouse_sends(6, 32, TSM_MOUSE_EVENT_MOVED));
	ck_assert_str_eq(out, "\e[<32;7;3M");
	input("\e[?1006l");

	input("\e[?1002l");
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 0);
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 0);
	ck_assert(!mouse_sends(4, 32, TSM_MOUSE_EVENT_MOVED));
}
END_TEST

START_TEST(test_mode_mouse_any_event)
{
	/* in the X10 encoding: motion with no button is 35, with the
	 * modifiers (aaronsb/libtsm#30 sent nothing) */
	input("\e[?1003h");
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 1003);
	ck_assert_uint_eq(mouse_cb_mode, 1003);
	ck_assert(mouse_sends(1, 0, TSM_MOUSE_EVENT_MOVED));
	ck_assert_str_eq(out, "\e[MC\"#");
	out_clear();
	ck_assert(tsm_vte_handle_mouse(vte, 2, 2, 0, 0, 0, TSM_MOUSE_EVENT_MOVED,
				       TSM_MOUSE_MODIFIER_CTRL));
	ck_assert_str_eq(out, "\e[MS##");

	input("\e[?1003l");
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 0);
	ck_assert(!mouse_sends(5, 0, TSM_MOUSE_EVENT_MOVED));
}
END_TEST

START_TEST(test_mode_mouse_sgr)
{
	/* the encoding alone reports nothing and tells the host nothing */
	input("\e[?1006h");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 0);
	ck_assert_uint_eq(mouse_cb_calls, 0);
	ck_assert_int_eq(tsm_vte_get_mode(vte, true, 1006), 1);
	ck_assert(!mouse_sends(1, 0, TSM_MOUSE_EVENT_PRESSED));

	/* tracking set after it reports in it */
	input("\e[?1000h");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 1006);
	ck_assert(mouse_sends(1, 0, TSM_MOUSE_EVENT_PRESSED));
	ck_assert_str_eq(out, "\e[<0;2;3M");
	ck_assert(mouse_sends(1, 0, TSM_MOUSE_EVENT_RELEASED));
	ck_assert_str_eq(out, "\e[<0;2;3m");

	/* reset, the X10 encoding is back and tracking stays */
	input("\e[?1006l");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 1000);
	ck_assert(mouse_sends(1, 0, TSM_MOUSE_EVENT_PRESSED));
	ck_assert_str_eq(out, "\e[M \"#");

	/* and resetting tracking keeps the encoding */
	input("\e[?1006h\e[?1000l");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 0);
	ck_assert_int_eq(tsm_vte_get_mode(vte, true, 1006), 1);
	input("\e[?1003h");
	ck_assert(mouse_sends(1, 0, TSM_MOUSE_EVENT_MOVED));
	ck_assert_str_eq(out, "\e[<35;2;3M");

	/* RM of the encoding that is not set leaves the one that is */
	input("\e[?1016l");
	ck_assert_int_eq(tsm_vte_get_mode(vte, true, 1006), 1);
	ck_assert(mouse_sends(2, 0, TSM_MOUSE_EVENT_MOVED));
	ck_assert_str_eq(out, "\e[<35;3;3M");
}
END_TEST

START_TEST(test_mode_mouse_pixel)
{
	input("\e[?1003h\e[?1016h");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 1016);
	ck_assert_uint_eq(mouse_cb_calls, 2);
	ck_assert(mouse_cb_pixels);
	ck_assert(tsm_vte_handle_mouse(vte, 1, 2, 17, 40, 0, TSM_MOUSE_EVENT_PRESSED, 0));
	ck_assert_str_eq(out, "\e[<0;18;41M");

	/* the top left pixel is 1,1, as xterm reports it (aaronsb/libtsm#67) */
	out_clear();
	ck_assert(tsm_vte_handle_mouse(vte, 0, 0, 0, 0, 0, TSM_MOUSE_EVENT_PRESSED, 0));
	ck_assert_str_eq(out, "\e[<0;1;1M");

	/* a wheel step in pixels */
	ck_assert(mouse_sends(1, TSM_MOUSE_BUTTON_WHEEL_UP, TSM_MOUSE_EVENT_PRESSED));
	ck_assert_str_eq(out, "\e[<64;10;41M");

	/* reset, cells in the X10 encoding, still tracking */
	input("\e[?1016l");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 1000);
	ck_assert_uint_eq(mouse_cb_calls, 3);
	ck_assert(!mouse_cb_pixels);
	ck_assert(mouse_sends(1, 0, TSM_MOUSE_EVENT_PRESSED));
	ck_assert_str_eq(out, "\e[M \"#");

	input("\e[?1003l");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 0);
}
END_TEST

/* every protocol under every encoding: a press and a release at column 1,
 * a drag to column 3, motion with no button to column 5 */
struct mouse_case {
	const char *track, *encoding;
	const char *press, *release, *drag, *hover;
};

static const struct mouse_case mouse_cases[] = {
	{ "9", "", "\e[M \"#", "", "", "" },
	{ "9", "1006", "\e[<0;2;3M", "", "", "" },
	{ "9", "1016", "\e[<0;10;41M", "", "", "" },
	{ "1000", "", "\e[M \"#", "\e[M#\"#", "", "" },
	{ "1000", "1006", "\e[<0;2;3M", "\e[<0;2;3m", "", "" },
	{ "1000", "1016", "\e[<0;10;41M", "\e[<0;10;41m", "", "" },
	{ "1002", "", "\e[M \"#", "\e[M#\"#", "\e[M@$#", "" },
	{ "1002", "1006", "\e[<0;2;3M", "\e[<0;2;3m", "\e[<32;4;3M", "" },
	{ "1002", "1016", "\e[<0;10;41M", "\e[<0;10;41m", "\e[<32;26;41M", "" },
	{ "1003", "", "\e[M \"#", "\e[M#\"#", "\e[M@$#", "\e[MC&#" },
	{ "1003", "1006", "\e[<0;2;3M", "\e[<0;2;3m", "\e[<32;4;3M", "\e[<35;6;3M" },
	{ "1003", "1016", "\e[<0;10;41M", "\e[<0;10;41m", "\e[<32;26;41M", "\e[<35;42;41M" },
};

static void assert_mouse_sends(const char *state, unsigned int col, unsigned int button,
			       unsigned int event, const char *want)
{
	mouse_sends(col, button, event);
	ck_assert_msg(!strcmp(out, want), "%s: event %u at %u sent \"%s\", want \"%s\"",
		      state, event, col, out + (out_len > 0), want + (*want != 0));
}

/* the order the two are set in does not matter, and a later SM in either
 * set replaces the earlier (aaronsb/libtsm#30) */
START_TEST(test_mode_mouse_order)
{
	static const unsigned int tracks[] = { 9, 1000, 1002, 1003 };
	char state[64], track[16], encoding[16];
	unsigned int i, order, t;

	for (i = 0; i < sizeof(mouse_cases) / sizeof(*mouse_cases); ++i) {
		const struct mouse_case *c = &mouse_cases[i];

		snprintf(track, sizeof(track), "\e[?%sh", c->track);
		snprintf(encoding, sizeof(encoding), *c->encoding ? "\e[?%sh" : "%s",
			 c->encoding);
		for (order = 0; order < 3; ++order) {
			if (order == 0)
				snprintf(state, sizeof(state), "%s%s", track, encoding);
			else if (order == 1)
				snprintf(state, sizeof(state), "%s%s", encoding, track);
			else
				snprintf(state, sizeof(state), "\e[?1003h%s%s%s",
					 *c->encoding ? "\e[?1016h\e[?1006h" : "",
					 encoding, track);
			input("\ec");
			input(state);
			assert_mouse_sends(state + 1, 1, 0, TSM_MOUSE_EVENT_PRESSED, c->press);
			assert_mouse_sends(state + 1, 1, 0, TSM_MOUSE_EVENT_RELEASED, c->release);
			assert_mouse_sends(state + 1, 3, 32, TSM_MOUSE_EVENT_MOVED, c->drag);
			assert_mouse_sends(state + 1, 5, 0, TSM_MOUSE_EVENT_MOVED, c->hover);

			for (t = 0; t < sizeof(tracks) / sizeof(*tracks); ++t)
				ck_assert_msg(tsm_vte_get_mode(vte, true, tracks[t]) ==
					      (tracks[t] == (unsigned int)atoi(c->track) ? 1 : 2),
					      "%s: DECRQM %u", state + 1, tracks[t]);
			ck_assert_msg(tsm_vte_get_mode(vte, true, 1006) ==
				      (!strcmp(c->encoding, "1006") ? 1 : 2), "%s: DECRQM 1006",
				      state + 1);
			ck_assert_msg(tsm_vte_get_mode(vte, true, 1016) ==
				      (!strcmp(c->encoding, "1016") ? 1 : 2), "%s: DECRQM 1016",
				      state + 1);
		}
	}
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

/* RIS and DECSTR turn bracketed paste off (aaronsb/libtsm#44) */
START_TEST(test_mode_bracketed_paste_resets)
{
	input("\e[?2004h\ec");
	tsm_vte_paste(vte, "x");
	ck_assert_str_eq(out, "x");

	out_clear();
	input("\e[?2004h\e[!p");
	tsm_vte_paste(vte, "y");
	ck_assert_str_eq(out, "y");
}
END_TEST

/* Focus events */

START_TEST(test_mode_focus)
{
	/* off at start: the host's calls write nothing */
	ck_assert(!tsm_vte_handle_focus(vte, true));
	ck_assert(!tsm_vte_handle_focus(vte, false));
	ck_assert_uint_eq(out_len, 0);

	/* setting the mode writes nothing of its own */
	input("\e[?1004h");
	ck_assert_uint_eq(out_len, 0);
	ck_assert(tsm_vte_handle_focus(vte, true));
	ck_assert_str_eq(out, "\e[I");
	out_clear();
	ck_assert(tsm_vte_handle_focus(vte, false));
	ck_assert_str_eq(out, "\e[O");

	/* libtsm drops no repeat: the host does */
	out_clear();
	ck_assert(tsm_vte_handle_focus(vte, false));
	ck_assert_str_eq(out, "\e[O");

	out_clear();
	input("\e[?1004l");
	ck_assert(!tsm_vte_handle_focus(vte, true));
	ck_assert(!tsm_vte_handle_focus(vte, false));
	ck_assert_uint_eq(out_len, 0);
}
END_TEST

/* RIS and DECSTR turn focus events off */
START_TEST(test_mode_focus_resets)
{
	input("\e[?1004h\ec");
	out_clear();
	ck_assert(!tsm_vte_handle_focus(vte, true));
	ck_assert_uint_eq(out_len, 0);

	input("\e[?1004h\e[!p");
	out_clear();
	ck_assert(!tsm_vte_handle_focus(vte, false));
	ck_assert_uint_eq(out_len, 0);

	/* and so do the host's resets */
	input("\e[?1004h");
	tsm_vte_reset(vte);
	ck_assert(!tsm_vte_handle_focus(vte, true));
	input("\e[?1004h");
	tsm_vte_hard_reset(vte);
	ck_assert(!tsm_vte_handle_focus(vte, true));
	ck_assert_uint_eq(out_len, 0);
}
END_TEST

/* XTRESTORE (CSI ? Pm r) is not DECSTBM: the scroll region stays
 * (aaronsb/libtsm#44) */
START_TEST(test_mode_xtrestore_not_margins)
{
	/* rows 2 to 4 scroll: a line feed on row 4 stays on row 4 */
	input("\e[2;4r\e[?1049;2004r\e[?r\e[4;1H\n");
	assert_cursor(0, 3);
	ck_assert(!screen_flag(TSM_SCREEN_ALTERNATE));
}
END_TEST

/* XTSMPOINTER (CSI > Ps p) sets when xterm hides the pointer; it is not
 * DECSTR, and neither is a p final under another prefix or intermediate
 * (aaronsb/libtsm#59) */
START_TEST(test_mode_xtsmpointer_not_reset)
{
	input("\e[?1h\e[?2004h\e[2;4r");
	input("\e[>1p\e[>0p\e[>p\e[>2;3p\e[?1p\e[1 p\e[1+p\e[1*p\e[1'p");
	/* XTPUSHSGR's alias, CSI Pm # p */
	input("\e[#p\e[1;2#p");
	ck_assert(vte_flag(TSM_VTE_FLAG_CURSOR_KEY_MODE));
	tsm_vte_paste(vte, "x");
	ck_assert_str_eq(out, "\e[200~x\e[201~");
	/* rows 2 to 4 still scroll: a line feed on row 4 stays on row 4 */
	input("\e[4;1H\n");
	assert_cursor(0, 3);

	/* DECSTR still resets */
	out_clear();
	input("\e[!p");
	ck_assert(!vte_flag(TSM_VTE_FLAG_CURSOR_KEY_MODE));
	tsm_vte_paste(vte, "y");
	ck_assert_str_eq(out, "y");
	input("\e[4;1H\n");
	assert_cursor(0, 4);

	/* and so does DECSCL, with its " or without it, as upstream takes it */
	input("\e[?1h\e[62;1\"p");
	ck_assert(!vte_flag(TSM_VTE_FLAG_CURSOR_KEY_MODE));
	input("\e[?1h\e[62p");
	ck_assert(!vte_flag(TSM_VTE_FLAG_CURSOR_KEY_MODE));
}
END_TEST

/* KAM, DECARM and DECNRCM have no effect, but SM and RM still toggle their
 * flags in tsm_vte_get_flags, as upstream's API has them */
START_TEST(test_mode_ignored_legacy_flags)
{
	ck_assert(!vte_flag(TSM_VTE_FLAG_KEYBOARD_ACTION_MODE));
	ck_assert(vte_flag(TSM_VTE_FLAG_AUTO_REPEAT_MODE));
	ck_assert(!vte_flag(TSM_VTE_FLAG_NATIONAL_CHARSET_MODE));

	input("\e[2h\e[?8l\e[?42h");
	ck_assert(vte_flag(TSM_VTE_FLAG_KEYBOARD_ACTION_MODE));
	ck_assert(!vte_flag(TSM_VTE_FLAG_AUTO_REPEAT_MODE));
	ck_assert(vte_flag(TSM_VTE_FLAG_NATIONAL_CHARSET_MODE));

	input("\e[2l\e[?8h\e[?42l");
	ck_assert(!vte_flag(TSM_VTE_FLAG_KEYBOARD_ACTION_MODE));
	ck_assert(vte_flag(TSM_VTE_FLAG_AUTO_REPEAT_MODE));
	ck_assert(!vte_flag(TSM_VTE_FLAG_NATIONAL_CHARSET_MODE));

	/* and RIS restores them */
	input("\e[2h\e[?8l\e[?42h\ec");
	ck_assert(!vte_flag(TSM_VTE_FLAG_KEYBOARD_ACTION_MODE));
	ck_assert(vte_flag(TSM_VTE_FLAG_AUTO_REPEAT_MODE));
	ck_assert(!vte_flag(TSM_VTE_FLAG_NATIONAL_CHARSET_MODE));
}
END_TEST

/* DECRQM and tsm_vte_get_mode (aaronsb/libtsm#31) */

struct mode_reply {
	bool dec;
	unsigned int number;
	unsigned int value;
};

/* every row in its default state, and numbers with no row; sixel is off,
 * so its modes, 80, 1070 and 8452, reply their fixed state */
static const struct mode_reply mode_defaults[] = {
	{ false, 2, 4 }, { false, 4, 2 }, { false, 12, 1 }, { false, 20, 2 },
	{ true, 1, 2 }, { true, 2, 3 }, { true, 3, 4 }, { true, 4, 4 },
	{ true, 5, 2 }, { true, 6, 2 }, { true, 7, 1 }, { true, 8, 3 },
	{ true, 9, 2 }, { true, 12, 1 }, { true, 18, 4 }, { true, 19, 4 },
	{ true, 25, 1 }, { true, 42, 4 }, { true, 47, 2 }, { true, 80, 4 },
	{ true, 1000, 2 }, { true, 1002, 2 }, { true, 1003, 2 },
	{ true, 1004, 2 }, { true, 1006, 2 }, { true, 1016, 2 },
	{ true, 1047, 2 },
	{ true, 1048, 2 }, { true, 1049, 2 }, { true, 1070, 3 },
	{ true, 2004, 2 }, { true, 2026, 4 }, { true, 2027, 1 },
	{ true, 8452, 4 },
	/* not recognised */
	{ false, 0, 0 }, { false, 1, 0 }, { false, 3, 0 }, { false, 25, 0 },
	{ false, 2004, 0 }, { true, 0, 0 }, { true, 10, 0 }, { true, 1005, 0 },
	{ true, 1015, 0 }, { true, 65535, 0 },
};

static void assert_decrqm(bool dec, unsigned int number, unsigned int value)
{
	char query[32], expect[32];

	snprintf(query, sizeof(query), "\e[%s%u$p", dec ? "?" : "", number);
	snprintf(expect, sizeof(expect), "\e[%s%u;%u$y", dec ? "?" : "",
		 number, value);
	out_clear();
	input(query);
	ck_assert_str_eq(out, expect);
	ck_assert_int_eq(tsm_vte_get_mode(vte, dec, number), value);
}

START_TEST(test_mode_decrqm_defaults)
{
	unsigned int i;

	for (i = 0; i < sizeof(mode_defaults) / sizeof(*mode_defaults); ++i)
		assert_decrqm(mode_defaults[i].dec, mode_defaults[i].number,
			      mode_defaults[i].value);
}
END_TEST

START_TEST(test_mode_decrqm_set)
{
	/* each supported row reports set after SM, reset after RM; sixel's rows
	 * report their fixed state here, with sixel off
	 * (test_mode_decrqm_sixel_off), and set and reset with it on in
	 * test_vte_sixel.c (test_sixel_decrqm) */
	input("\e[4;20h\e[?1;5;6;1004;2004h");
	assert_decrqm(false, 4, 1);
	assert_decrqm(false, 20, 1);
	assert_decrqm(true, 1, 1);
	assert_decrqm(true, 5, 1);
	assert_decrqm(true, 6, 1);
	assert_decrqm(true, 1004, 1);
	assert_decrqm(true, 2004, 1);
	input("\e[12;4;20l\e[?7;25;1004;2027l");
	assert_decrqm(false, 12, 2);
	assert_decrqm(false, 4, 2);
	assert_decrqm(true, 7, 2);
	assert_decrqm(true, 25, 2);
	assert_decrqm(true, 1004, 2);
	assert_decrqm(true, 2027, 2);

	/* ignored and refused rows keep their fixed state */
	input("\e[2h\e[?2;3;4;12;18;19;42h\e[?8l");
	assert_decrqm(false, 2, 4);
	assert_decrqm(true, 2, 3);
	assert_decrqm(true, 3, 4);
	assert_decrqm(true, 8, 3);
	assert_decrqm(true, 12, 1);
	assert_decrqm(true, 42, 4);

	/* the alternate screen modes share one flag; 1048 holds no state */
	input("\e[?1049h");
	assert_decrqm(true, 47, 1);
	assert_decrqm(true, 1047, 1);
	assert_decrqm(true, 1049, 1);
	input("\e[?1049l\e[?1048h");
	assert_decrqm(true, 1049, 2);
	assert_decrqm(true, 1048, 2);

	/* the mouse rows: a protocol replaces the protocol, an encoding the
	 * encoding, and neither touches the other set (aaronsb/libtsm#30) */
	input("\e[?1000h");
	assert_decrqm(true, 1000, 1);
	assert_decrqm(true, 1002, 2);
	input("\e[?1006h");
	assert_decrqm(true, 1000, 1);
	assert_decrqm(true, 1006, 1);
	input("\e[?1003h");
	assert_decrqm(true, 1003, 1);
	assert_decrqm(true, 1000, 2);
	assert_decrqm(true, 1006, 1);
	input("\e[?1016h");
	assert_decrqm(true, 1016, 1);
	assert_decrqm(true, 1006, 2);
	assert_decrqm(true, 1003, 1);
	input("\e[?9h");
	assert_decrqm(true, 9, 1);
	assert_decrqm(true, 1003, 2);
	assert_decrqm(true, 1016, 1);
}
END_TEST

/*
 * Sixel's modes reply their fixed state while sixel is off, in every build:
 * not turned on, or turned on with no image budget, and after an SM or RM,
 * which they still store (ADR-103, Replies stay truthful). With sixel on,
 * test_vte_sixel.c (test_sixel_decrqm); in a build without images,
 * test_vte_image_none.c.
 */
START_TEST(test_mode_decrqm_sixel_off)
{
	input("\e[?80;8452h\e[?1070l");
	assert_decrqm(true, 80, 4);
	assert_decrqm(true, 1070, 3);
	assert_decrqm(true, 8452, 4);

	/* on, with no budget */
	tsm_vte_set_sixel(vte, true);
	assert_decrqm(true, 80, 4);
	assert_decrqm(true, 1070, 3);
	assert_decrqm(true, 8452, 4);
}
END_TEST

/*
 * The mouse rows' DECRQM against what tsm_vte_handle_mouse sends: 9 is set
 * when presses are reported and releases are not, 1000 when releases are and
 * drags are not, 1002 when drags are and motion with no button is not, 1003
 * when that is; 1006 and 1016 when the reports are SGR in cells or in pixels,
 * seen with 1000 set where nothing tracks.
 */
static const char *const mouse_states[] = {
	"", "\e[?9h", "\e[?1000h", "\e[?1002h", "\e[?1003h", "\e[?1006h",
	"\e[?1016h", "\e[?1000h\e[?1006h", "\e[?1006h\e[?1000h",
	"\e[?1006h\e[?1002h", "\e[?1006h\e[?1003h", "\e[?1003h\e[?1016h",
	"\e[?1000h\e[?1003h", "\e[?1000h\e[?1000l", "\e[?1006h\e[?1003h\e[?1003l",
	"\e[?1016h\e[?1002h\e[?1006h", "\e[?9h\e[?1006h\e[?9l",
	"\e[?1003h\e[?1000l", "\e[?1006h\e[?1016l\e[?1000h", "\e[?1003h\e[?9h",
};

START_TEST(test_mode_mouse_decrqm_matches_reports)
{
	bool press, release, drag, hover, sgr, pixel;
	unsigned int i;

	for (i = 0; i < sizeof(mouse_states) / sizeof(*mouse_states); ++i) {
		input("\ec");
		input(mouse_states[i]);
		press = mouse_sends(1, 0, TSM_MOUSE_EVENT_PRESSED);
		release = mouse_sends(1, 0, TSM_MOUSE_EVENT_RELEASED);
		drag = mouse_sends(3, 32, TSM_MOUSE_EVENT_MOVED);
		hover = mouse_sends(5, 3, TSM_MOUSE_EVENT_MOVED);

		ck_assert_msg(tsm_vte_get_mode(vte, true, 9) == (press && !release ? 1 : 2),
			      "state %u: 9", i);
		ck_assert_msg(tsm_vte_get_mode(vte, true, 1000) == (release && !drag ? 1 : 2),
			      "state %u: 1000", i);
		ck_assert_msg(tsm_vte_get_mode(vte, true, 1002) == (drag && !hover ? 1 : 2),
			      "state %u: 1002", i);
		ck_assert_msg(tsm_vte_get_mode(vte, true, 1003) == (hover ? 1 : 2),
			      "state %u: 1003", i);

		if (!press) {
			ck_assert_msg(!release && !drag && !hover, "state %u", i);
			input("\e[?1000h");
			ck_assert(mouse_sends(1, 0, TSM_MOUSE_EVENT_PRESSED));
		} else {
			mouse_sends(1, 0, TSM_MOUSE_EVENT_PRESSED);
		}
		sgr = !strcmp(out, "\e[<0;2;3M");
		pixel = !strcmp(out, "\e[<0;10;41M");
		ck_assert_msg(sgr || pixel || !strcmp(out, "\e[M \"#"), "state %u", i);
		ck_assert_msg(tsm_vte_get_mode(vte, true, 1006) == (sgr ? 1 : 2),
			      "state %u: 1006", i);
		ck_assert_msg(tsm_vte_get_mode(vte, true, 1016) == (pixel ? 1 : 2),
			      "state %u: 1016", i);
	}
}
END_TEST

/* RIS and DECSTR clear both sets directly, telling the host nothing, as
 * before the table */
START_TEST(test_mode_mouse_resets)
{
	unsigned int calls;

	input("\e[?1006h\e[?1003h");
	calls = mouse_cb_calls;
	input("\ec");
	ck_assert_uint_eq(tsm_vte_get_mouse_mode(vte), 0);
	ck_assert_uint_eq(tsm_vte_get_mouse_event(vte), 0);
	ck_assert_uint_eq(mouse_cb_calls, calls);

	input("\e[?1003h\ec\e[?1006h");
	ck_assert(!mouse_sends(5, 3, TSM_MOUSE_EVENT_MOVED));
	assert_decrqm(true, 1003, 2);

	input("\ec\e[?1003h\e[!p\e[?1006h");
	ck_assert(!mouse_sends(5, 3, TSM_MOUSE_EVENT_MOVED));
	assert_decrqm(true, 1003, 2);

	/* the encoding goes too */
	input("\e[?1016h\e[!p\e[?1000h");
	ck_assert(mouse_sends(1, 0, TSM_MOUSE_EVENT_PRESSED));
	ck_assert_str_eq(out, "\e[M \"#");
	assert_decrqm(true, 1016, 2);
}
END_TEST

/* mode 12 reports whether the cursor style blinks, as kmscon draws it: the
 * default and the odd DECSCUSR styles; SM and RM 12 do nothing yet
 * (aaronsb/libtsm#57) */
START_TEST(test_mode_cursor_blink)
{
	assert_decrqm(true, 12, 1);
	input("\e[2 q");
	assert_decrqm(true, 12, 2);
	input("\e[?12h");
	assert_decrqm(true, 12, 2);
	input("\e[5 q");
	assert_decrqm(true, 12, 1);
	input("\e[?12l");
	assert_decrqm(true, 12, 1);
	input("\e[6 q");
	assert_decrqm(true, 12, 2);
	input("\e[0 q");
	assert_decrqm(true, 12, 1);
	ck_assert_uint_eq(tsm_screen_get_cursor_style(screen), TSM_SCREEN_CURSOR_DEFAULT);
}
END_TEST

START_TEST(test_mode_decrqm_form)
{
	/* the first number is answered; a missing one is 0 */
	out_clear();
	input("\e[?25;7$p");
	ck_assert_str_eq(out, "\e[?25;1$y");
	out_clear();
	input("\e[?$p");
	ck_assert_str_eq(out, "\e[?0;0$y");
	out_clear();
	input("\e[$p");
	ck_assert_str_eq(out, "\e[0;0$y");

	/* a number past the parser's limit is not recognised */
	out_clear();
	input("\e[?9999999$p");
	ck_assert_int_eq(strncmp(out, "\e[?", 3), 0);
	ck_assert_int_eq(strcmp(out + strlen(out) - 4, ";0$y"), 0);

	ck_assert_int_eq(tsm_vte_get_mode(NULL, true, 25), -EINVAL);
}
END_TEST

/* a host that never blinks the cursor (kmscon's blink=false) says so, and
 * mode 12 then reports reset for every style */
START_TEST(test_mode_cursor_blink_host)
{
	tsm_vte_set_cursor_blink(vte, false);
	assert_decrqm(true, 12, 2);
	input("\e[5 q");
	assert_decrqm(true, 12, 2);

	tsm_vte_set_cursor_blink(vte, true);
	assert_decrqm(true, 12, 1);
	input("\e[6 q");
	assert_decrqm(true, 12, 2);

	tsm_vte_set_cursor_blink(NULL, false);
}
END_TEST

/*
 * Mode 2026, synchronized output (contour's synchronized-output spec): SM
 * starts a hold and RM ends it, while libtsm keeps parsing and replying.
 * The host learns of each end by tsm_vte_get_sync_frames, which counts the
 * holds that ended, once each.
 */
START_TEST(test_mode_sync)
{
	uint64_t frames;

	tsm_vte_set_sync(vte, true);
	frames = tsm_vte_get_sync_frames(vte);
	assert_decrqm(true, 2026, 2);

	input("\e[?2026h");
	assert_decrqm(true, 2026, 1);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), frames);

	/* the screen and the replies go on during a hold */
	out_clear();
	input("ab\e[5n");
	ck_assert_uint_eq(cell(0, 0), 'a');
	ck_assert_uint_eq(cell(1, 0), 'b');
	ck_assert_str_eq(out, "\e[0n");

	/* a set during a hold changes nothing */
	input("\e[?2026h");
	assert_decrqm(true, 2026, 1);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), frames);

	input("\e[?2026l");
	assert_decrqm(true, 2026, 2);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), frames + 1);

	/* a reset with no hold ends nothing */
	input("\e[?2026l");
	assert_decrqm(true, 2026, 2);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), frames + 1);

	/* two frames in one call are two ends */
	input("\e[?2026hc\e[?2026l\e[?2026hd\e[?2026l");
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), frames + 3);
	ck_assert_uint_eq(cell(3, 0), 'd');
}
END_TEST

/*
 * A host that does not present by mode 2026 leaves it off, the default:
 * DECRQM replies 4 (permanently reset), which the spec reads as not
 * supported, and SM changes nothing, so no program is told its frames are
 * held when they are not.
 */
START_TEST(test_mode_sync_host)
{
	assert_decrqm(true, 2026, 4);
	input("\e[?2026h");
	assert_decrqm(true, 2026, 4);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 0);

	/* the SM before it was turned on left no hold behind */
	tsm_vte_set_sync(vte, true);
	assert_decrqm(true, 2026, 2);

	/* turning it off ends a hold, and counts as its end */
	input("\e[?2026h");
	tsm_vte_set_sync(vte, false);
	assert_decrqm(true, 2026, 4);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 1);
	input("\e[?2026l");
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 1);
	tsm_vte_set_sync(vte, true);
	assert_decrqm(true, 2026, 2);

	tsm_vte_set_sync(NULL, true);
	tsm_vte_end_sync(NULL);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(NULL), 0);
}
END_TEST

/*
 * RIS, DECSTR, a host's tsm_vte_reset and tsm_vte_end_sync end a hold, each
 * counted once; the alternate screen does not, since the spec ties the hold
 * to nothing but its own SM and RM.
 */
START_TEST(test_mode_sync_resets)
{
	tsm_vte_set_sync(vte, true);

	input("\e[?2026h\e[!p");
	assert_decrqm(true, 2026, 2);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 1);

	input("\e[?2026h\ec");
	assert_decrqm(true, 2026, 2);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 2);

	input("\e[?2026h");
	tsm_vte_reset(vte);
	assert_decrqm(true, 2026, 2);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 3);

	input("\e[?2026h");
	tsm_vte_end_sync(vte);
	assert_decrqm(true, 2026, 2);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 4);

	/* with no hold, none of them counts */
	tsm_vte_end_sync(vte);
	input("\e[!p\ec");
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 4);

	/* the host's setting survives both resets */
	assert_decrqm(true, 2026, 2);

	input("\e[?2026h\e[?1049h");
	assert_decrqm(true, 2026, 1);
	input("\e[?1049l\e[?47h\e[?47l");
	assert_decrqm(true, 2026, 1);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 4);
}
END_TEST

static size_t input_bounded(const char *s, size_t len)
{
	return tsm_vte_input_bounded(vte, s, len);
}

/*
 * tsm_vte_input_bounded returns right after the byte that ends a hold, so
 * the host sees each completed frame before any byte of the next;
 * tsm_vte_input parses through.
 */
START_TEST(test_mode_sync_bounded)
{
	static const char frames[] = "a\e[?2026hbc\e[?2026ld\e[?2026hef\e[?2026lg";
	const char *end1 = strstr(frames, "l") + 1;
	const char *end2 = strrchr(frames, 'l') + 1;
	size_t len = sizeof(frames) - 1, n;

	tsm_vte_set_sync(vte, true);
	n = input_bounded(frames, len);
	ck_assert_uint_eq(n, (size_t)(end1 - frames));
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 1);
	ck_assert_uint_eq(cell(2, 0), 'c');
	ck_assert_uint_eq(cell(3, 0), 0);

	n += input_bounded(frames + n, len - n);
	ck_assert_uint_eq(n, (size_t)(end2 - frames));
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 2);
	ck_assert_uint_eq(cell(5, 0), 'f');
	ck_assert_uint_eq(cell(6, 0), 0);

	n += input_bounded(frames + n, len - n);
	ck_assert_uint_eq(n, len);
	ck_assert_uint_eq(cell(6, 0), 'g');

	/* a frame end split across calls stops at its last byte */
	input("\e[?2026h");
	ck_assert_uint_eq(input_bounded("\e[?20", 5), 5);
	ck_assert_uint_eq(input_bounded("26lxy", 5), 3);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 3);

	/* RIS and DECSTR end a frame too */
	ck_assert_uint_eq(input_bounded("\e[?2026hz\ecz", 12), 11);
	ck_assert_uint_eq(input_bounded("\e[?2026hz\e[!pz", 14), 13);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 5);

	/* a start alone does not stop it */
	ck_assert_uint_eq(input_bounded("\e[?2026hzz", 10), 10);
	assert_decrqm(true, 2026, 1);

	/* tsm_vte_input takes every frame in one call */
	input("\ec");
	input(frames);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 8);
	ck_assert_uint_eq(cell(6, 0), 'g');

	/* off, a frame end is not one, and nothing stops */
	tsm_vte_set_sync(vte, false);
	ck_assert_uint_eq(input_bounded(frames, len), len);
	ck_assert_uint_eq(tsm_vte_get_sync_frames(vte), 8);
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
	TEST(test_mode_mouse_order)
	TEST(test_mode_altbuf_47)
	TEST(test_mode_altbuf_1047)
	TEST(test_mode_save_cursor_1048)
	TEST(test_mode_altbuf_1049)
	TEST(test_mode_bracketed_paste)
	TEST(test_mode_bracketed_paste_resets)
	TEST(test_mode_focus)
	TEST(test_mode_focus_resets)
	TEST(test_mode_ignored_legacy_flags)
TEST_END_CASE

TEST_DEFINE_CASE(report)
	CHECKED_FIXTURE(setup, teardown)
	TEST(test_mode_decrqm_defaults)
	TEST(test_mode_decrqm_set)
	TEST(test_mode_decrqm_sixel_off)
	TEST(test_mode_decrqm_form)
	TEST(test_mode_mouse_decrqm_matches_reports)
	TEST(test_mode_mouse_resets)
	TEST(test_mode_cursor_blink)
	TEST(test_mode_cursor_blink_host)
	TEST(test_mode_xtrestore_not_margins)
	TEST(test_mode_xtsmpointer_not_reset)
	TEST(test_mode_sync)
	TEST(test_mode_sync_host)
	TEST(test_mode_sync_resets)
	TEST(test_mode_sync_bounded)
TEST_END_CASE

// clang-format off
TEST_DEFINE(
	TEST_SUITE(vte_modes,
		TEST_CASE(ansi),
		TEST_CASE(dec),
		TEST_CASE(report),
		TEST_END
	)
)
// clang-format on
