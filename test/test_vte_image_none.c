/*
 * TSM - VTE tests for a build without images
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * The vte of a libtsm built with -Dimages=false (ADR-103 in aaronsb/kmscon,
 * libtsm's images option; tsm-vte-image-none.c): with sixel and kitty
 * graphics turned on and a budget given, as kmscon would, a sixel string is
 * consumed with no effect and an APC ignored to its end, DA1 omits attribute
 * 4, XTSMGRAPHICS answers status 1, a kitty query gets no reply, DECRQM of
 * 80, 1070 and 8452 answers their fixed state, and the image calls return 0.
 */

#include <stdio.h>
#include <string.h>
#include "libtsm.h"
#include "test_common.h"

static char reply[256];
static size_t reply_len;

static void write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	UNUSED(vte);
	UNUSED(data);
	ck_assert_uint_lt(reply_len + len, sizeof(reply));
	memcpy(reply + reply_len, u8, len);
	reply_len += len;
	reply[reply_len] = 0;
}

static void reply_clear(void)
{
	reply_len = 0;
	reply[0] = 0;
}

static struct tsm_screen *screen;
static struct tsm_vte *vte;

/* everything a host with images would turn on */
static void setup(void)
{
	ck_assert_int_eq(tsm_screen_new(&screen, NULL, NULL), 0);
	ck_assert_int_eq(tsm_screen_resize(screen, 20, 5), 0);
	ck_assert_int_eq(tsm_vte_new(&vte, screen, write_cb, NULL, NULL, NULL), 0);
	ck_assert_int_eq(tsm_screen_set_cell_size(screen, 8, 16), 0);
	tsm_screen_set_image_limit(screen, 64 << 20);
	tsm_vte_set_sixel(vte, true);
	tsm_vte_set_kitty_graphics(vte, true);
	reply_clear();
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

START_TEST(test_image_none_sixel_string)
{
	/* a 1x6 red picture between two characters */
	input("a\033Pq#0;2;100;0;0#0~\033\\b");
	ck_assert_uint_eq(tsm_screen_get_cursor_x(screen), 2);
	ck_assert_uint_eq(tsm_screen_get_cursor_y(screen), 0);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	ck_assert_str_eq(reply, "");
}
END_TEST

START_TEST(test_image_none_kitty)
{
	/* a query, then DA1, as a client's detection sends them */
	input("a\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\b\033[c");
	ck_assert_uint_eq(tsm_screen_get_cursor_x(screen), 2);
	ck_assert_str_eq(reply, "\033[?60;1;6;9;15c");
	reply_clear();
	/* an upload and a placement */
	input("\033_Ga=T,f=24,s=1,v=1;AAAA\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	ck_assert_str_eq(reply, "");
}
END_TEST

START_TEST(test_image_none_replies)
{
	input("\033[c");
	ck_assert_str_eq(reply, "\033[?60;1;6;9;15c");
	reply_clear();
	input("\033[?1;1S\033[?2;1S\033[?1;4S\033[?3;1S");
	ck_assert_str_eq(reply, "\033[?1;1S\033[?2;1S\033[?1;1S\033[?3;1S");
}
END_TEST

START_TEST(test_image_none_decrqm)
{
	input("\033[?80$p\033[?1070$p\033[?8452$p");
	ck_assert_str_eq(reply, "\033[?80;4$y\033[?1070;3$y\033[?8452;4$y");
	reply_clear();
	input("\033[?80;8452h\033[?1070l\033[?80$p\033[?1070$p\033[?8452$p");
	ck_assert_str_eq(reply, "\033[?80;4$y\033[?1070;3$y\033[?8452;4$y");
	ck_assert_int_eq(tsm_vte_get_mode(vte, true, 80), 4);
	ck_assert_int_eq(tsm_vte_get_mode(vte, true, 1070), 3);
	ck_assert_int_eq(tsm_vte_get_mode(vte, true, 8452), 4);
}
END_TEST

START_TEST(test_image_none_calls)
{
	char big[4096];
	size_t len;

	ck_assert_int_eq(tsm_vte_set_graphic_max_size(vte, 1, 1), 0);
	ck_assert_int_eq(tsm_vte_set_sixel_registers(vte, 16, false), 0);
	ck_assert_int_eq(tsm_vte_set_sixel_registers(vte, 1, false), 0);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 1, NULL), 0);
	tsm_vte_set_image_cb(vte, NULL, NULL);
	/* 1070's fixed state stays its default, set */
	ck_assert_int_eq(tsm_vte_get_mode(vte, true, 1070), 3);

	/* a bounded call has no image work to bound: it takes a long sixel
	 * string whole */
	len = snprintf(big, sizeof(big), "\033Pq#0;2;100;0;0#0!3000~\033\\");
	ck_assert_uint_lt(len, sizeof(big));
	ck_assert_uint_eq(tsm_vte_input_bounded(vte, big, len), len);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
}
END_TEST

TEST_DEFINE_CASE(image_none)
	CHECKED_FIXTURE(setup, teardown)
	TEST(test_image_none_sixel_string)
	TEST(test_image_none_kitty)
	TEST(test_image_none_replies)
	TEST(test_image_none_decrqm)
	TEST(test_image_none_calls)
TEST_END_CASE

// clang-format off
TEST_DEFINE(
	TEST_SUITE(vte_image_none,
		TEST_CASE(image_none),
		TEST_END
	)
)
// clang-format on
