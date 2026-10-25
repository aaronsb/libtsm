/*
 * TSM - VTE sixel tests
 *
 * Copyright (c) 2026 Aaron Bockelie <aaronsb@gmail.com>
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
 * Sixel through the vte (ADR-103 step 3c in aaronsb/kmscon): the DCS reaches
 * the decoder, the picture the store, and the placement the cursor; the
 * modes, the replies, the budget and the bound on one input call's work.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "test_common.h"

/* what a picture of @w x @h pixels costs the budget */
#define COST(w, h) ((size_t)(w) * (h) * 4 + TSM_SCREEN_IMAGE_OVERHEAD)

/* VT340 register 1, blue (20, 20, 80) percent */
#define VT_BLUE 0xff3333ccu
#define BLACK 0xff000000u
#define RED 0xffff0000u

static void log_cb(void *data, const char *file, int line, const char *func,
		   const char *subs, unsigned int sev, const char *format,
		   va_list args)
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

/* a @cols x @rows screen of 8x16 cells, a 64 MiB limit, sixel on */
static void setup_size(unsigned int cols, unsigned int rows)
{
	ck_assert_int_eq(tsm_screen_new(&screen, log_cb, NULL), 0);
	ck_assert_int_eq(tsm_screen_resize(screen, cols, rows), 0);
	ck_assert_int_eq(tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL), 0);
	ck_assert_int_eq(tsm_screen_set_cell_size(screen, 8, 16), 0);
	tsm_screen_set_image_limit(screen, 64 << 20);
	tsm_vte_set_sixel(vte, true);
	reply_clear();
}

static void setup(void)
{
	setup_size(10, 5);
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

/*
 * A DCS for a @w x @h picture of register @reg, all opaque (P2 0), written
 * band by band into @buf.
 */
static const char *solid(char *buf, size_t size, unsigned int w, unsigned int h,
			 unsigned int reg)
{
	size_t len;
	unsigned int y, rows;

	len = snprintf(buf, size, "\033P0;0;0q\"1;1;%u;%u#%u", w, h, reg);
	for (y = 0; y < h; y += 6) {
		rows = h - y < 6 ? h - y : 6;
		len += snprintf(buf + len, size - len, "!%u%c-", w,
				'?' + (1 << rows) - 1);
	}
	len += snprintf(buf + len, size - len, "\033\\");
	ck_assert_uint_lt(len, size);
	return buf;
}

static void show(unsigned int w, unsigned int h, unsigned int reg)
{
	char buf[1024];

	input(solid(buf, sizeof(buf), w, h, reg));
}

static const struct tsm_frame_placement *placement(unsigned int i)
{
	const struct tsm_frame *f = tsm_screen_frame(screen);

	ck_assert_uint_lt(i, f->placement_count);
	return tsm_frame_placement_at(f, i);
}

static unsigned int placements(void)
{
	return tsm_screen_frame(screen)->placement_count;
}

static uint32_t pixel(const struct tsm_frame_placement *p, unsigned int x,
		      unsigned int y)
{
	struct tsm_frame_image img;

	ck_assert_int_eq(tsm_screen_frame_image(screen, p->image_key, &img), 0);
	ck_assert_uint_lt(x, img.width);
	ck_assert_uint_lt(y, img.height);
	return img.pixels[(size_t)y * img.width + x];
}

static void assert_cursor(unsigned int x, unsigned int y)
{
	ck_assert_uint_eq(tsm_screen_get_cursor_x(screen), x);
	ck_assert_uint_eq(tsm_screen_get_cursor_y(screen), y);
}

/* Fail closed */

START_TEST(test_sixel_off)
{
	char buf[1024];

	/* no switch: no attribute 4, no picture, no graphics reported */
	ck_assert_int_eq(tsm_screen_new(&screen, log_cb, NULL), 0);
	ck_assert_int_eq(tsm_screen_resize(screen, 10, 5), 0);
	ck_assert_int_eq(tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL), 0);
	tsm_screen_set_image_limit(screen, 64 << 20);
	reply_clear();
	input("\033[c");
	ck_assert_str_eq(reply, "\033[?60;1;6;9;15c");
	input(solid(buf, sizeof(buf), 8, 16, 1));
	ck_assert_uint_eq(placements(), 0);
	assert_cursor(0, 0);
	reply_clear();
	input("\033[?1;1S\033[?2;1S");
	ck_assert_str_eq(reply, "\033[?1;1S\033[?2;1S");

	/* the switch with no budget: still off */
	tsm_screen_set_image_limit(screen, 0);
	tsm_vte_set_sixel(vte, true);
	reply_clear();
	input("\033[c");
	ck_assert_str_eq(reply, "\033[?60;1;6;9;15c");
	input(solid(buf, sizeof(buf), 8, 16, 1));
	ck_assert_uint_eq(placements(), 0);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);

	/* both: on */
	tsm_screen_set_image_limit(screen, 64 << 20);
	reply_clear();
	input("\033[c");
	ck_assert_str_eq(reply, "\033[?60;1;4;6;9;15c");
	input(solid(buf, sizeof(buf), 8, 16, 1));
	ck_assert_uint_eq(placements(), 1);

	/* and off again */
	tsm_vte_set_sixel(vte, false);
	reply_clear();
	input("\033[c");
	ck_assert_str_eq(reply, "\033[?60;1;6;9;15c");
	teardown();
}
END_TEST

/* Placement and cursor */

START_TEST(test_sixel_at_cursor)
{
	const struct tsm_frame_placement *p;

	/* 16x40 pixels is 2x3 cells; the cursor ends on the row holding the
	 * last pixel row, at the picture's left column */
	input("\033[2;3H");
	show(16, 40, 1);
	ck_assert_uint_eq(placements(), 1);
	p = placement(0);
	ck_assert_int_eq(p->row, 1);
	ck_assert_uint_eq(p->col, 2);
	ck_assert_uint_eq(p->cols, 2);
	ck_assert_uint_eq(p->rows, 3);
	ck_assert_uint_eq(p->kind, TSM_IMAGE_SIXEL);
	ck_assert(p->flags & TSM_FRAME_OPAQUE);
	ck_assert_uint_eq(pixel(p, 0, 0), VT_BLUE);
	ck_assert_uint_eq(pixel(p, 15, 39), VT_BLUE);
	assert_cursor(2, 3);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(16, 40));

	/* one row of pixels: the cursor stays on its row */
	input("\033[1;1H");
	show(8, 1, 1);
	assert_cursor(0, 0);
}
END_TEST

START_TEST(test_sixel_cursor_right)
{
	/* mode 8452: the column after the right edge, on the last row */
	input("\033[?8452h\033[2;3H");
	show(16, 40, 1);
	assert_cursor(4, 3);

	/* clamped to the last column; a picture past the edge stays whole */
	input("\033[1;8H");
	show(40, 16, 1);
	assert_cursor(9, 0);
	ck_assert_uint_eq(placement(1)->cols, 5);
	ck_assert_uint_eq(placement(1)->col, 7);

	/* reset, it ends at the left column again */
	input("\033[?8452l\033[1;1H");
	show(16, 16, 1);
	assert_cursor(0, 0);
}
END_TEST

START_TEST(test_sixel_pending_wrap)
{
	/* a pending wrap leaves the cursor past the last column; the picture
	 * starts in the last one */
	input("\033[1;1H0123456789");
	show(8, 16, 1);
	ck_assert_uint_eq(placement(0)->col, 9);
	assert_cursor(9, 0);
}
END_TEST

START_TEST(test_sixel_scrolls)
{
	const struct tsm_frame_placement *p;

	/* 3 rows from row 4 of 5: the screen scrolls 2, as index would, and
	 * the picture's top lands on row 2 */
	input("top\033[5;1H");
	show(8, 48, 1);
	p = placement(0);
	ck_assert_int_eq(p->row, 2);
	ck_assert_uint_eq(p->rows, 3);
	assert_cursor(0, 4);
	ck_assert_uint_eq(tsm_screen_get_cursor_y(screen), 4);

	/* taller than the screen: the top goes into scrollback with its line */
	tsm_screen_set_max_sb(screen, 10);
	input("\033[1;1H");
	show(8, 16 * 7, 1);
	p = placement(placements() - 1);
	ck_assert_int_eq(p->row, -2);
	ck_assert_uint_eq(p->rows, 7);
	assert_cursor(0, 4);

	/* with no scrollback, the lines leaving the top take their rows */
	tsm_screen_set_max_sb(screen, 0);
	tsm_screen_clear_sb(screen);
	input("\033[2J\033[1;1H");
	show(8, 16 * 7, 1);
	p = placement(placements() - 1);
	ck_assert_int_eq(p->row, 0);
	ck_assert_uint_eq(p->rows, 5);
	ck_assert_uint_eq(p->src_y, 32);
	assert_cursor(0, 4);
}
END_TEST

START_TEST(test_sixel_region)
{
	const struct tsm_frame_placement *p;
	unsigned int i;

	/* rows 1 to 3 scroll; 2 rows from row 3 scroll the region by one, so
	 * the picture moves whole and its top lands on row 2 */
	input("\033[2;4r\033[4;1H");
	show(8, 32, 1);
	ck_assert_uint_eq(placements(), 1);
	p = placement(0);
	ck_assert_int_eq(p->row, 2);
	ck_assert_uint_eq(p->rows, 2);
	ck_assert_uint_eq(p->y, 0);
	ck_assert_uint_eq(p->height, 32);
	assert_cursor(0, 3);

	/* taller than the region: scrolled to its top, then by index, the
	 * part leaving the region cut away */
	input("\033[3;3H");
	show(8, 64, 1);
	for (i = 0; i < placements(); ++i)
		ck_assert_int_ge(placement(i)->row, 0);
	assert_cursor(2, 3);
}
END_TEST

START_TEST(test_sixel_display_mode)
{
	const struct tsm_frame_placement *p;

	/* DECSDM set: the top left of the screen, cut to it, cursor still */
	input("\033[?80h\033[3;4H");
	show(120, 100, 1);
	p = placement(0);
	ck_assert_int_eq(p->row, 0);
	ck_assert_uint_eq(p->col, 0);
	ck_assert_uint_eq(p->src_width, 80);
	ck_assert_uint_eq(p->src_height, 80);
	ck_assert_uint_eq(p->cols, 10);
	ck_assert_uint_eq(p->rows, 5);
	assert_cursor(3, 2);

	/* reset, scrolling again */
	input("\033[?80l\033[1;1H");
	show(8, 16, 1);
	/* the new one covers the first's top left cell, which splits it */
	ck_assert_uint_eq(placements(), 3);
	assert_cursor(0, 0);
}
END_TEST

START_TEST(test_sixel_text_over)
{
	/* text over a sixel removes the cells it covers, as for any sixel */
	input("\033[1;1H");
	show(24, 16, 1);
	input("\033[1;2Hx");
	ck_assert_uint_eq(placements(), 2);
}
END_TEST

/* Ends of the string */

START_TEST(test_sixel_cancel)
{
	char buf[1024];
	size_t len;

	/* CAN and SUB discard the picture and its memory */
	solid(buf, sizeof(buf), 16, 16, 1);
	len = strlen(buf);
	buf[len - 2] = 0x18;
	buf[len - 1] = 0;
	input(buf);
	ck_assert_uint_eq(placements(), 0);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	buf[len - 2] = 0x1a;
	input(buf);
	ck_assert_uint_eq(placements(), 0);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);

	/* ESC ending the string shows it, whatever follows */
	buf[len - 2] = 0x1b;
	input(buf);
	input("[H");
	ck_assert_uint_eq(placements(), 1);

	/* an intermediate makes another DCS, which stays ignored */
	input("\033P$q\"p\033\\");
	ck_assert_uint_eq(placements(), 1);

	/* a picture with no pixels makes nothing */
	input("\033Pq\033\\\033Pq#1\033\\");
	ck_assert_uint_eq(placements(), 1);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(16, 16));
}
END_TEST

START_TEST(test_sixel_reset_mid_string)
{
	/* a reset in the middle of a picture gives its memory back */
	input("\033Pq#1!100~-!100~");
	ck_assert_uint_gt(tsm_screen_get_image_bytes(screen), 0);
	tsm_vte_reset(vte);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	input("\033\\");
	ck_assert_uint_eq(placements(), 0);

	/* and so does freeing the vte */
	input("\033Pq#1!100~-!100~");
	ck_assert_uint_gt(tsm_screen_get_image_bytes(screen), 0);
	tsm_vte_unref(vte);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	ck_assert_int_eq(tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL), 0);
}
END_TEST

START_TEST(test_sixel_off_mid_string)
{
	uint32_t px = 0xff0000ff;
	struct tsm_test_image k = {
		.width = 1, .height = 1, .pixels = &px, .kind = TSM_IMAGE_KITTY,
	};
	uint64_t key;

	/* a picture whose budget goes while it streams is dropped at its end,
	 * with every byte it held, as sixel is then off */
	input("\033P0;1q\"1;1;100;100#1~~~~");
	ck_assert_uint_gt(tsm_screen_get_image_bytes(screen), 0);
	tsm_screen_set_image_limit(screen, 0);
	input("~~\033\\");
	ck_assert_uint_eq(placements(), 0);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);

	/* dropped before the store makes room for it: a full store keeps the
	 * images it had, though the budget would refuse the picture after an
	 * eviction */
	tsm_screen_set_image_limit(screen, 64 << 20);
	for (k.id = 1; k.id <= TSM_SCREEN_IMAGES_PER_STORE; ++k.id)
		ck_assert_int_eq(tsm_screen_test_image(screen, &k, &key), 0);
	input("\033P0;1q\"1;1;100;100#1~~~~");
	tsm_screen_set_image_limit(screen, 0);
	input("~~\033\\");
	ck_assert_uint_eq(tsm_screen_test_image_count(screen),
			  TSM_SCREEN_IMAGES_PER_STORE);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen),
			  TSM_SCREEN_IMAGES_PER_STORE * COST(1, 1));
}
END_TEST

/* Registers */

START_TEST(test_sixel_registers)
{
	/* private (mode 1070 set, the default): a register defined in one
	 * picture is not seen by the next; register 20 starts black */
	input("\033Pq#20;2;100;0;0#20~\033\\");
	ck_assert_uint_eq(pixel(placement(0), 0, 0), RED);
	input("\033[1;3H\033Pq#20~\033\\");
	ck_assert_uint_eq(pixel(placement(1), 0, 0), BLACK);

	/* shared: definitions last from picture to picture */
	input("\033[?1070l\033[1;5H\033Pq#20;2;100;0;0#20~\033\\");
	input("\033[1;7H\033Pq#20~\033\\");
	ck_assert_uint_eq(pixel(placement(3), 0, 0), RED);

	/* until RIS, which also returns 1070 to the host's setting */
	input("\033c\033Pq#20~\033\\");
	ck_assert_uint_eq(pixel(placement(0), 0, 0), BLACK);
	input("\033[1;3H\033Pq#20;2;100;0;0#20~\033\\\033[1;5H\033Pq#20~\033\\");
	ck_assert_uint_eq(pixel(placement(2), 0, 0), BLACK);

	/* the host's setting: shared from the start */
	ck_assert_int_eq(tsm_vte_set_sixel_registers(vte, 0, false), 0);
	input("\033c\033Pq#20;2;100;0;0#20~\033\\\033[1;3H\033Pq#20~\033\\");
	ck_assert_uint_eq(pixel(placement(1), 0, 0), RED);

	/* set: private again, whatever the host's setting */
	input("\033[?1070h\033[1;5H\033Pq#20~\033\\");
	ck_assert_uint_eq(pixel(placement(2), 0, 0), BLACK);
	ck_assert_int_eq(tsm_vte_set_sixel_registers(vte, 1, false), -EINVAL);
	ck_assert_int_eq(tsm_vte_set_sixel_registers(vte, 65536, false), -EINVAL);
}
END_TEST

START_TEST(test_sixel_max_size)
{
	/* max-graphic-size bounds each dimension */
	ck_assert_int_eq(tsm_vte_set_graphic_max_size(vte, 20, 10), 0);
	show(40, 40, 1);
	ck_assert_uint_eq(placement(0)->src_width, 20);
	ck_assert_uint_eq(placement(0)->src_height, 10);
	reply_clear();
	input("\033[?2;1S\033[?2;4S");
	ck_assert_str_eq(reply, "\033[?2;0;20;10S\033[?2;0;20;10S");
}
END_TEST

/* Budget */

static size_t host_held;
static size_t host_budget;

/* a host that evicts from the screen to make room, as kmscon's will */
static bool host_evicts;

static int host_reserve(struct tsm_screen *con, size_t bytes, void *data)
{
	UNUSED(data);
	while (host_evicts && host_held + bytes > host_budget &&
	       tsm_screen_image_evict(con))
		;
	if (host_held + bytes > host_budget)
		return -ENOSPC;
	host_held += bytes;
	return 0;
}

static void host_release(struct tsm_screen *con, size_t bytes, void *data)
{
	UNUSED(con);
	UNUSED(data);
	ck_assert_uint_ge(host_held, bytes);
	host_held -= bytes;
}

START_TEST(test_sixel_budget)
{
	char buf[4096];

	host_evicts = false;
	/* the host's budget pays for the picture as it decodes, and the
	 * store takes the same bytes over */
	host_held = 0;
	host_budget = COST(16, 16);
	ck_assert_int_eq(tsm_screen_set_image_budget(screen, host_reserve,
						     host_release, NULL), 0);
	show(16, 16, 1);
	ck_assert_uint_eq(placements(), 1);
	ck_assert_uint_eq(host_held, COST(16, 16));

	/* a picture past the budget is dropped, its bytes given back; the
	 * placed one stays */
	input("\033[2;1H");
	input(solid(buf, sizeof(buf), 16, 32, 1));
	ck_assert_uint_eq(placements(), 1);
	ck_assert_uint_eq(host_held, COST(16, 16));
	assert_cursor(0, 1);

	/* erased, the first frees its bytes, and the second fits */
	input("\033[2J");
	ck_assert_uint_eq(host_held, 0);
	input("\033[1;1H");
	show(16, 16, 1);
	ck_assert_uint_eq(host_held, COST(16, 16));

	ck_assert_int_eq(tsm_screen_set_image_budget(screen, NULL, NULL, NULL), 0);
	ck_assert_uint_eq(host_held, 0);
}
END_TEST

/*
 * Many tiny pictures: each costs its bookkeeping as well as its pixels, a
 * store holds at most TSM_SCREEN_IMAGES_PER_STORE, and the scans that look
 * for room count against one input call's work.
 */
START_TEST(test_sixel_tiny_many)
{
	static const char tiny[] = "\033P0;1qA\033\\";
	const size_t n = 20000;
	char *data;
	size_t i, len, used;

	/* 1x2 transparent pictures stacked on one cell: none cuts another,
	 * so all stay placed, and past the cap the next is refused */
	for (i = 0; i < 3000; ++i)
		input(tiny);
	ck_assert_uint_eq(tsm_screen_test_image_count(screen),
			  TSM_SCREEN_IMAGES_PER_STORE);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen),
			  TSM_SCREEN_IMAGES_PER_STORE * COST(1, 2));
	ck_assert_uint_eq(placements(), TSM_SCREEN_IMAGES_PER_STORE);

	/* the alternate screen's store has its own room */
	input("\033[?1049h");
	input(tiny);
	ck_assert_uint_eq(tsm_screen_test_image_count(screen),
			  TSM_SCREEN_IMAGES_PER_STORE + 1);
	input("\033[?1049l");

	/* at the cap, every picture costs a scan for room: one bounded call
	 * stops long before 20000 of them */
	data = malloc(sizeof(tiny) * n);
	ck_assert_ptr_ne(data, NULL);
	for (i = 0, len = 0; i < n; ++i, len += sizeof(tiny) - 1)
		memcpy(data + len, tiny, sizeof(tiny) - 1);
	used = tsm_vte_input_bounded(vte, data, len);
	ck_assert_uint_lt(used, len / 2);
	free(data);
}
END_TEST

/*
 * Text in a checkerboard over many pictures cuts each into pieces: each piece
 * past a picture's first is charged, a store holds a bounded number, and
 * pieces the budget refuses go, smallest first.
 */
static void checkerboard(unsigned int cols, unsigned int rows)
{
	char buf[32];
	unsigned int x, y;

	for (y = 0; y < rows; ++y)
		for (x = y % 2; x < cols; x += 2) {
			snprintf(buf, sizeof(buf), "\033[%u;%uHx", y + 1, x + 1);
			input(buf);
		}
}

/* @n transparent 320x192 pictures, all over the 40x12 screen */
static void stack(unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; ++i)
		input("\033[H\033P0;1q\"1;1;320;192#1~\033\\");
}

START_TEST(test_sixel_pieces_bounded)
{
	unsigned int images, pieces;

	teardown();
	setup_size(40, 12);
	stack(40);
	checkerboard(40, 12);
	images = tsm_screen_test_image_count(screen);
	pieces = tsm_screen_test_placement_count(screen);
	ck_assert_uint_eq(images, 40);
	ck_assert_uint_eq(pieces, TSM_SCREEN_PLACEMENTS_PER_STORE);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen),
			  images * COST(320, 192) +
			  (size_t)(pieces - images) * TSM_SCREEN_PLACEMENT_OVERHEAD);
}
END_TEST

START_TEST(test_sixel_pieces_budget)
{
	const size_t limit = 10 * COST(320, 192) + 50 * TSM_SCREEN_PLACEMENT_OVERHEAD;
	unsigned int images, pieces;

	teardown();
	setup_size(40, 12);
	tsm_screen_set_image_limit(screen, limit);
	stack(10);
	checkerboard(40, 12);
	images = tsm_screen_test_image_count(screen);
	pieces = tsm_screen_test_placement_count(screen);
	ck_assert_uint_le(tsm_screen_get_image_bytes(screen), limit);
	ck_assert_uint_le(pieces, images + 50);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen),
			  images * COST(320, 192) +
			  (size_t)(pieces - images) * TSM_SCREEN_PLACEMENT_OVERHEAD);
}
END_TEST

/*
 * A piece cut from a picture owes its charge until the input call ends; a
 * picture evicted with a piece still owed gives the host back only what the
 * host granted.
 */
START_TEST(test_sixel_owed_evicted)
{
	teardown();
	setup_size(10, 3);
	tsm_screen_set_max_sb(screen, 100);
	host_held = 0;
	host_evicts = true;
	host_budget = COST(24, 6) + 100;
	ck_assert_int_eq(tsm_screen_set_image_budget(screen, host_reserve,
						     host_release, NULL), 0);

	/* one call: the picture, a cut in its middle cell, and enough lines
	 * to carry it into scrollback, where the piece's charge evicts it */
	input("\033[1;1H\033P0;0q!24~\033\\\033[1;2HX\033[3;1H\n\n\n\n");
	ck_assert_uint_eq(host_held, tsm_screen_get_image_bytes(screen));
	ck_assert_uint_eq(tsm_screen_test_image_count(screen), 0);
	ck_assert_uint_eq(host_held, 0);
	ck_assert_int_eq(tsm_screen_set_image_budget(screen, NULL, NULL, NULL), 0);
	host_evicts = false;
}
END_TEST

/*
 * A budget that refuses what pieces owe drops the smallest piece first: a
 * picture cut into one cell and three keeps the three.
 */
START_TEST(test_sixel_owed_smallest_first)
{
	const struct tsm_frame_placement *p;

	tsm_screen_set_image_limit(screen, COST(40, 6));
	input("\033[1;1H\033P0;0q!40~\033\\");
	ck_assert_uint_eq(placements(), 1);
	ck_assert_uint_eq(placement(0)->cols, 5);

	/* the piece the cut makes owes a charge the limit refuses */
	input("\033[1;2HX");
	ck_assert_uint_eq(placements(), 1);
	p = placement(0);
	ck_assert_uint_eq(p->col, 2);
	ck_assert_uint_eq(p->cols, 3);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(40, 6));
}
END_TEST

/* Work per call */

START_TEST(test_sixel_bounded)
{
	static const char cmd[] = "!10000~$";
	const size_t n = 1 << 13;
	char *data;
	size_t len, pos, used, calls = 0;
	struct tsm_frame_image img;

	/* 8192 repeats of 60000 pixels each, about 64 KiB of input */
	data = malloc(sizeof(cmd) * n + 16);
	ck_assert_ptr_ne(data, NULL);
	len = sprintf(data, "\033Pq#1");
	for (pos = 0; pos < n; ++pos)
		len += sprintf(data + len, "%s", cmd);
	len += sprintf(data + len, "\033\\");

	/* one call stops after about 16 million pixels: some 280 repeats */
	for (pos = 0; pos < len; pos += used) {
		used = tsm_vte_input_bounded(vte, data + pos, len - pos);
		ck_assert_uint_ge(used, 1);
		ck_assert_uint_le(used, 8 * 300);
		++calls;
	}
	ck_assert_uint_ge(calls, n / 300);

	/* fed in pieces, it makes the picture one call would */
	ck_assert_uint_eq(placements(), 1);
	ck_assert_int_eq(tsm_screen_frame_image(screen, placement(0)->image_key,
						&img), 0);
	ck_assert_uint_eq(img.width, 10000);
	ck_assert_uint_eq(img.height, 6);
	ck_assert_uint_eq(img.pixels[9999], VT_BLUE);

	/* text costs no image work: one call takes it all */
	ck_assert_uint_eq(tsm_vte_input_bounded(vte, data + 5, 8 * 100), 8 * 100);
	free(data);
}
END_TEST

START_TEST(test_sixel_split_input)
{
	char buf[1024];
	struct tsm_frame_image a, b;
	size_t len, i;

	/* the whole string at once, and one byte at a time, through the
	 * parser's slow path, make the same picture */
	snprintf(buf, sizeof(buf),
		 "\033P0;1q\"1;1;20;14#1!20~-#2;2;0;100;0!5N#3;1;120;50;100$!3o\r\n"
		 "!7@-#1~~~~\033\\");
	len = strlen(buf);
	input(buf);
	input("\033[1;5H");
	for (i = 0; i < len; ++i)
		tsm_vte_input(vte, buf + i, 1);
	ck_assert_uint_eq(placements(), 2);
	ck_assert_int_eq(tsm_screen_frame_image(screen, placement(0)->image_key, &a), 0);
	ck_assert_int_eq(tsm_screen_frame_image(screen, placement(1)->image_key, &b), 0);
	ck_assert_uint_eq(a.width, b.width);
	ck_assert_uint_eq(a.height, b.height);
	ck_assert(!memcmp(a.pixels, b.pixels, (size_t)a.width * a.height * 4));

	/* a UTF-8 character split across calls before the string does not
	 * disturb it */
	input("\033[4;1H\xc3");
	input("\xa9\033Pq#1~\033\\");
	ck_assert_uint_eq(placements(), 3);
}
END_TEST

/* Replies */

START_TEST(test_sixel_xtsmgraphics)
{
	input("\033[?1;1S");
	ck_assert_str_eq(reply, "\033[?1;0;1024S");
	reply_clear();
	input("\033[?1;3;256S\033[?1;1S");
	ck_assert_str_eq(reply, "\033[?1;0;256S\033[?1;0;256S");
	reply_clear();
	/* outside 2 to num-color-registers: refused */
	input("\033[?1;3;1S\033[?1;3;1025S\033[?1;1S");
	ck_assert_str_eq(reply, "\033[?1;3S\033[?1;3S\033[?1;0;256S");
	reply_clear();
	input("\033[?1;4S\033[?1;2S\033[?1;1S");
	ck_assert_str_eq(reply, "\033[?1;0;1024S\033[?1;0;1024S\033[?1;0;1024S");
	reply_clear();
	input("\033[?1;9S");
	ck_assert_str_eq(reply, "\033[?1;2S");
	reply_clear();

	/* geometry: max-graphic-size, and no change */
	input("\033[?2;1S\033[?2;4S\033[?2;3;100;100S\033[?2;2S");
	ck_assert_str_eq(reply, "\033[?2;0;10000;10000S\033[?2;0;10000;10000S"
				"\033[?2;3S\033[?2;3S");
	reply_clear();

	/* no ReGIS */
	input("\033[?3;1S\033[?4;1S");
	ck_assert_str_eq(reply, "\033[?3;1S\033[?4;1S");

	/* a count set with XTSMGRAPHICS bounds the next picture's registers */
	input("\033[?1;3;2S\033Pq#1;2;100;0;0#1~\033\\");
	ck_assert_uint_eq(pixel(placement(0), 0, 0), RED);
	input("\033[1;3H\033Pq#5;2;0;100;0#5~\033\\");
	/* register 5 is register 1, the last of two */
	ck_assert_uint_eq(pixel(placement(1), 0, 0), 0xff00ff00u);
}
END_TEST

START_TEST(test_sixel_xtwinops)
{
	/* 14: the text area, 16: the cell, in pixels; 18: in cells */
	input("\033[14t\033[16t\033[18t");
	ck_assert_str_eq(reply, "\033[4;80;80t\033[6;16;8t\033[8;5;10t");
	reply_clear();
	ck_assert_int_eq(tsm_screen_set_cell_size(screen, 10, 20), 0);
	input("\033[14t\033[16t");
	ck_assert_str_eq(reply, "\033[4;100;100t\033[6;20;10t");
}
END_TEST

TEST_DEFINE_CASE(off)
	TEST(test_sixel_off)
TEST_END_CASE

TEST_DEFINE_CASE(placement)
	CHECKED_FIXTURE(setup, teardown)
	TEST(test_sixel_at_cursor)
	TEST(test_sixel_cursor_right)
	TEST(test_sixel_pending_wrap)
	TEST(test_sixel_scrolls)
	TEST(test_sixel_region)
	TEST(test_sixel_display_mode)
	TEST(test_sixel_text_over)
TEST_END_CASE

TEST_DEFINE_CASE(strings)
	CHECKED_FIXTURE(setup, teardown)
	TEST(test_sixel_cancel)
	TEST(test_sixel_reset_mid_string)
	TEST(test_sixel_off_mid_string)
	TEST(test_sixel_registers)
	TEST(test_sixel_max_size)
	TEST(test_sixel_budget)
	TEST(test_sixel_tiny_many)
	TEST(test_sixel_pieces_bounded)
	TEST(test_sixel_pieces_budget)
	TEST(test_sixel_owed_evicted)
	TEST(test_sixel_owed_smallest_first)
	TEST(test_sixel_bounded)
	TEST(test_sixel_split_input)
TEST_END_CASE

TEST_DEFINE_CASE(replies)
	CHECKED_FIXTURE(setup, teardown)
	TEST(test_sixel_xtsmgraphics)
	TEST(test_sixel_xtwinops)
TEST_END_CASE

TEST_DEFINE(
	TEST_SUITE(vte_sixel,
		TEST_CASE(off),
		TEST_CASE(placement),
		TEST_CASE(strings),
		TEST_CASE(replies),
		TEST_END
	)
)
