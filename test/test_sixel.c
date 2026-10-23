/*
 * TSM - Sixel Decoder Tests
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
 * The expected pixels are written from DEC STD 070 and the VT340 manual:
 * sixel bit 0 is the top row, '?' is 0x3f, registers default to the VT340
 * colour map, HLS puts blue at 0 degrees. Percentages map to 8 bits as
 * round(pct * 255 / 100). The fixtures in sixel/ are libsixel's output for
 * the PPMs beside them (gen-fixtures.sh), compared within the encoder's
 * quantisation.
 */

#include <stdio.h>
#include <string.h>
#include "tsm-sixel.h"
#include "test_common.h"

#define BLACK 0xff000000u
#define WHITE 0xffffffffu
#define RED 0xffff0000u
#define GREEN 0xff00ff00u
#define BLUE 0xff0000ffu
#define CLEAR 0x00000000u
/* VT340 register 1, blue (20, 20, 80) percent */
#define VT_BLUE 0xff3333ccu
/* VT340 register 2, red (80, 13, 13) */
#define VT_RED 0xffcc2121u
/* VT340 register 3, green (20, 80, 20) */
#define VT_GREEN 0xff33cc33u

/* Reservations outstanding, checked against what the decoder hands over. */
static size_t reserved;
static size_t budget;

static bool reserve_cb(void *data, size_t old_size, size_t new_size)
{
	UNUSED(data);
	ck_assert_uint_ge(reserved, old_size);
	if (new_size > old_size && budget &&
	    reserved - old_size + new_size > budget)
		return false;
	reserved = reserved - old_size + new_size;
	return true;
}

static struct tsm_sixel *new_decoder(unsigned int w, unsigned int h,
				     unsigned int registers)
{
	struct tsm_sixel_limits lim = {
		.max_width = w,
		.max_height = h,
		.registers = registers,
		.reserve = reserve_cb,
	};
	struct tsm_sixel *s;

	reserved = 0;
	budget = 0;
	ck_assert_int_eq(tsm_sixel_new(&s, &lim), 0);
	return s;
}

static int decode_with(struct tsm_sixel *s, int p1, int p2,
		       struct tsm_sixel_palette *shared, const char *data,
		       struct tsm_sixel_image *img)
{
	int ret;

	memset(img, 0, sizeof(*img));
	tsm_sixel_begin(s, p1, p2, shared);
	tsm_sixel_feed(s, data, strlen(data));
	ret = tsm_sixel_finish(s, img);
	if (ret == 0)
		ck_assert_uint_eq(reserved, (size_t)img->width * img->height * 4);
	else
		ck_assert_uint_eq(reserved, 0);
	return ret;
}

/* Decodes with 1:1 pixels (P1 = 7) and transparent background (P2 = 1). */
static int decode(const char *data, struct tsm_sixel_image *img)
{
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	int ret;

	ret = decode_with(s, 7, 1, NULL, data, img);
	tsm_sixel_free(s);
	return ret;
}

static void image_free(struct tsm_sixel_image *img)
{
	if (img->pixels)
		reserved -= (size_t)img->width * img->height * 4;
	free(img->pixels);
	img->pixels = NULL;
}

static uint32_t px(const struct tsm_sixel_image *img, unsigned int x,
		   unsigned int y)
{
	ck_assert_uint_lt(x, img->width);
	ck_assert_uint_lt(y, img->height);
	return img->pixels[(size_t)y * img->width + x];
}

/* Asserts one column, top to bottom, against @n expected pixels. */
static void assert_column(const struct tsm_sixel_image *img, unsigned int x,
			  const uint32_t *want, unsigned int n)
{
	unsigned int y;

	ck_assert_uint_eq(img->height, n);
	for (y = 0; y < n; ++y)
		ck_assert_msg(px(img, x, y) == want[y],
			      "pixel %u,%u is %08x, want %08x", x, y,
			      px(img, x, y), want[y]);
}

START_TEST(test_sixel_bits)
{
	struct tsm_sixel_image img;
	/* '~' is 0x7e, 0x3f plus all six bits */
	const uint32_t full[] = { VT_BLUE, VT_BLUE, VT_BLUE, VT_BLUE, VT_BLUE,
				  VT_BLUE };
	/* 'A' is 0x41, bit 1: the second row */
	const uint32_t second[] = { CLEAR, VT_BLUE };
	/* 'B' is 0x42, bits 0 and 1 */
	const uint32_t top_two[] = { VT_BLUE, VT_BLUE };
	/* '_' is 0x5f, bit 5: the bottom row */
	const uint32_t bottom[] = { CLEAR, CLEAR, CLEAR, CLEAR, CLEAR, VT_BLUE };

	ck_assert_int_eq(decode("#1~", &img), 0);
	ck_assert_uint_eq(img.width, 1);
	assert_column(&img, 0, full, 6);
	ck_assert(img.transparent);
	image_free(&img);

	ck_assert_int_eq(decode("#1A", &img), 0);
	assert_column(&img, 0, second, 2);
	image_free(&img);

	ck_assert_int_eq(decode("#1B", &img), 0);
	assert_column(&img, 0, top_two, 2);
	image_free(&img);

	ck_assert_int_eq(decode("#1_", &img), 0);
	assert_column(&img, 0, bottom, 6);
	image_free(&img);

	/* a '?' column has no pixels but takes its place */
	ck_assert_int_eq(decode("#1?@", &img), 0);
	ck_assert_uint_eq(img.width, 2);
	ck_assert_uint_eq(img.height, 1);
	ck_assert_uint_eq(px(&img, 0, 0), CLEAR);
	ck_assert_uint_eq(px(&img, 1, 0), VT_BLUE);
	image_free(&img);
}
END_TEST

START_TEST(test_sixel_all_sixels)
{
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	struct tsm_sixel_image img;
	char data[256];
	unsigned int c, k, x, y, i, scale;
	size_t len;

	/* every sixel value over a band of register 1: bit k of column c
	 * shows register 2 in row k, at 1:1 and with each dot 2x2 pixels */
	for (i = 0; i < 2; ++i) {
		scale = i + 1;
		len = sprintf(data, i ? "\"2;1;64;6" : "\"1;1;64;6");
		len += sprintf(data + len, "#1!64~$#2");
		for (c = 0; c < 64; ++c)
			data[len++] = '?' + c;
		data[len] = 0;

		ck_assert_int_eq(decode_with(s, 0, 1, NULL, data, &img), 0);
		ck_assert_uint_eq(img.width, 64);
		ck_assert_uint_eq(img.height, 6 * scale);
		for (x = 0; x < 64; ++x) {
			for (k = 0; k < 6; ++k) {
				for (y = k * scale; y < (k + 1) * scale; ++y)
					ck_assert_msg(px(&img, x, y) ==
						      (x >> k & 1 ? VT_RED : VT_BLUE),
						      "scale %u: sixel %u row %u", scale,
						      x, y);
			}
		}
		image_free(&img);
	}
	tsm_sixel_free(s);
}
END_TEST

START_TEST(test_sixel_default_register)
{
	struct tsm_sixel_image img;

	/* With no colour introducer, sixels draw with register 0 (black) */
	ck_assert_int_eq(decode("@", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), BLACK);
	image_free(&img);
}
END_TEST

START_TEST(test_sixel_repeat)
{
	struct tsm_sixel_image img;
	unsigned int x;

	ck_assert_int_eq(decode("#2!5@", &img), 0);
	ck_assert_uint_eq(img.width, 5);
	ck_assert_uint_eq(img.height, 1);
	for (x = 0; x < 5; ++x)
		ck_assert_uint_eq(px(&img, x, 0), VT_RED);
	image_free(&img);

	/* 0 and an omitted count both mean once */
	ck_assert_int_eq(decode("#2!0@!@", &img), 0);
	ck_assert_uint_eq(img.width, 2);
	image_free(&img);

	/* a repeat applies only to the sixel right after it */
	ck_assert_int_eq(decode("#2!3#3@", &img), 0);
	ck_assert_uint_eq(img.width, 1);
	ck_assert_uint_eq(px(&img, 0, 0), VT_GREEN);
	image_free(&img);

	/* a repeat followed by another sixel draws that one once */
	ck_assert_int_eq(decode("#2!3@@", &img), 0);
	ck_assert_uint_eq(img.width, 4);
	image_free(&img);
}
END_TEST

START_TEST(test_sixel_carriage_return)
{
	struct tsm_sixel_image img;
	const uint32_t col0[] = { VT_RED, VT_BLUE, VT_BLUE, VT_BLUE, VT_BLUE,
				  VT_BLUE };

	/* '$' returns to the band's start: the second colour overprints only
	 * where its sixel sets bits */
	ck_assert_int_eq(decode("#1~~$#2@", &img), 0);
	ck_assert_uint_eq(img.width, 2);
	assert_column(&img, 0, col0, 6);
	ck_assert_uint_eq(px(&img, 1, 0), VT_BLUE);
	image_free(&img);
}
END_TEST

START_TEST(test_sixel_new_line)
{
	struct tsm_sixel_image img;
	const uint32_t col0[] = { VT_BLUE, VT_BLUE, VT_BLUE, VT_BLUE, VT_BLUE,
				  VT_BLUE, VT_RED };
	const uint32_t col1[] = { CLEAR, CLEAR, CLEAR, CLEAR, CLEAR, CLEAR,
				  VT_RED };

	/* '-' starts the next band, six rows down, at column 0 */
	ck_assert_int_eq(decode("#1~-#2!2@", &img), 0);
	ck_assert_uint_eq(img.width, 2);
	assert_column(&img, 0, col0, 7);
	assert_column(&img, 1, col1, 7);
	image_free(&img);

	/* an empty band between two keeps its rows */
	ck_assert_int_eq(decode("#1@--@", &img), 0);
	ck_assert_uint_eq(img.height, 13);
	ck_assert_uint_eq(px(&img, 0, 6), CLEAR);
	ck_assert_uint_eq(px(&img, 0, 12), VT_BLUE);
	image_free(&img);

	/* a trailing '-' adds no rows */
	ck_assert_int_eq(decode("#1~-", &img), 0);
	ck_assert_uint_eq(img.height, 6);
	image_free(&img);
}
END_TEST

START_TEST(test_sixel_rgb)
{
	struct tsm_sixel_image img;

	ck_assert_int_eq(decode("#5;2;100;0;0@#6;2;0;100;0@#7;2;0;0;100@"
				"#8;2;50;50;50@#9;2;100;100;100@#10;2;0;0;0@",
				&img), 0);
	ck_assert_uint_eq(img.width, 6);
	ck_assert_uint_eq(px(&img, 0, 0), RED);
	ck_assert_uint_eq(px(&img, 1, 0), GREEN);
	ck_assert_uint_eq(px(&img, 2, 0), BLUE);
	ck_assert_uint_eq(px(&img, 3, 0), 0xff808080u);
	ck_assert_uint_eq(px(&img, 4, 0), WHITE);
	ck_assert_uint_eq(px(&img, 5, 0), BLACK);
	image_free(&img);

	/* values above 100 percent clamp */
	ck_assert_int_eq(decode("#5;2;250;0;900@", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), 0xffff00ffu);
	image_free(&img);
}
END_TEST

START_TEST(test_sixel_hls)
{
	struct tsm_sixel_image img;

	/* DEC hue: blue 0, red 120, green 240; lightness 50 and saturation
	 * 100 are the pure hues */
	ck_assert_int_eq(decode("#5;1;0;50;100@#6;1;120;50;100@"
				"#7;1;240;50;100@#8;1;60;50;100@"
				"#9;1;0;100;0@#10;1;0;0;0@#11;1;300;50;0@"
				"#12;1;360;50;100@",
				&img), 0);
	ck_assert_uint_eq(img.width, 8);
	ck_assert_uint_eq(px(&img, 0, 0), BLUE);
	ck_assert_uint_eq(px(&img, 1, 0), RED);
	ck_assert_uint_eq(px(&img, 2, 0), GREEN);
	/* halfway from blue to red */
	ck_assert_uint_eq(px(&img, 3, 0), 0xffff00ffu);
	ck_assert_uint_eq(px(&img, 4, 0), WHITE);
	ck_assert_uint_eq(px(&img, 5, 0), BLACK);
	/* no saturation is grey at the lightness, whatever the hue */
	ck_assert_uint_eq(px(&img, 6, 0), 0xff808080u);
	/* 360 degrees is 0 */
	ck_assert_uint_eq(px(&img, 7, 0), BLUE);
	image_free(&img);

	/* VT340 register 1 in HLS (0, 50, 60) is its RGB (20, 20, 80) */
	ck_assert_int_eq(decode("#5;1;0;50;60@", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), VT_BLUE);
	image_free(&img);
}
END_TEST

START_TEST(test_sixel_incomplete_colour)
{
	struct tsm_sixel_image img;

	/* fewer than five parameters only select; unknown Pu only selects */
	ck_assert_int_eq(decode("#2;2;100;100@#1;3;100;100;100@", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), VT_RED);
	ck_assert_uint_eq(px(&img, 1, 0), VT_BLUE);
	image_free(&img);

	/* parameters past the fifth are dropped */
	ck_assert_int_eq(decode("#2;2;100;0;0;55;66@", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), RED);
	image_free(&img);
}
END_TEST

START_TEST(test_sixel_redefine_after_draw)
{
	struct tsm_sixel_image img;

	/* pixels hold register numbers until ST, as on a VT340 */
	ck_assert_int_eq(decode("#1@#1;2;0;100;0", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), GREEN);
	image_free(&img);
}
END_TEST

START_TEST(test_sixel_background)
{
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	struct tsm_sixel_image img;

	/* P2 = 1: unset pixels are transparent */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "#1A", &img), 0);
	ck_assert(img.transparent);
	ck_assert_uint_eq(px(&img, 0, 0), CLEAR);
	image_free(&img);

	/* P2 = 0, 2 and omitted: unset pixels take register 0 */
	ck_assert_int_eq(decode_with(s, 7, 0, NULL, "#1A", &img), 0);
	ck_assert(!img.transparent);
	ck_assert_uint_eq(px(&img, 0, 0), BLACK);
	image_free(&img);
	ck_assert_int_eq(decode_with(s, 7, 2, NULL, "#1A", &img), 0);
	ck_assert(!img.transparent);
	ck_assert_uint_eq(px(&img, 0, 0), BLACK);
	image_free(&img);
	ck_assert_int_eq(decode_with(s, 7, -1, NULL, "#1A", &img), 0);
	ck_assert(!img.transparent);
	ck_assert_uint_eq(px(&img, 0, 0), BLACK);
	image_free(&img);

	/* ... as register 0 stands at the end of the image */
	ck_assert_int_eq(decode_with(s, 7, 0, NULL, "#1A#0;2;0;0;100", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), BLUE);
	ck_assert_uint_eq(px(&img, 0, 1), VT_BLUE);
	image_free(&img);

	tsm_sixel_free(s);
}
END_TEST

START_TEST(test_sixel_aspect)
{
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	struct tsm_sixel_image img;
	const struct { int p1; unsigned int rows; } table[] = {
		{ -1, 12 }, { 0, 12 }, { 1, 12 }, { 2, 30 }, { 3, 18 },
		{ 4, 18 }, { 5, 12 }, { 6, 12 }, { 7, 6 }, { 8, 6 }, { 9, 6 },
		{ 10, 12 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
		ck_assert_int_eq(decode_with(s, table[i].p1, 1, NULL, "#1~", &img), 0);
		ck_assert_msg(img.height == table[i].rows,
			      "P1=%d gives %u rows, want %u", table[i].p1,
			      img.height, table[i].rows);
		ck_assert_uint_eq(img.width, 1);
		image_free(&img);
	}

	/* 2:1 doubles each dot's rows: bit 1 covers rows 2 and 3 */
	ck_assert_int_eq(decode_with(s, 0, 1, NULL, "#1A", &img), 0);
	ck_assert_uint_eq(img.height, 4);
	ck_assert_uint_eq(px(&img, 0, 1), CLEAR);
	ck_assert_uint_eq(px(&img, 0, 2), VT_BLUE);
	ck_assert_uint_eq(px(&img, 0, 3), VT_BLUE);
	image_free(&img);

	/* raster attributes override P1 */
	ck_assert_int_eq(decode_with(s, 2, 1, NULL, "\"1;1#1~", &img), 0);
	ck_assert_uint_eq(img.height, 6);
	image_free(&img);
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "\"3;1#1~", &img), 0);
	ck_assert_uint_eq(img.height, 18);
	image_free(&img);
	/* Pad above Pan widens the dots */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "\"1;2#1!3~", &img), 0);
	ck_assert_uint_eq(img.width, 6);
	ck_assert_uint_eq(img.height, 6);
	image_free(&img);
	/* the ratio counts, not its terms */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "\"2;2#1~", &img), 0);
	ck_assert_uint_eq(img.width, 1);
	ck_assert_uint_eq(img.height, 6);
	image_free(&img);
	/* ratios past 5:1 stop there */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "\"100;1#1~", &img), 0);
	ck_assert_uint_eq(img.height, 30);
	image_free(&img);

	/* after the first sixel, raster attributes are ignored */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "#1~\"3;1;50;50~", &img), 0);
	ck_assert_uint_eq(img.width, 2);
	ck_assert_uint_eq(img.height, 6);
	image_free(&img);

	tsm_sixel_free(s);
}
END_TEST

START_TEST(test_sixel_raster_size)
{
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	struct tsm_sixel_image img;
	unsigned int x, y;

	/* Ph and Pv extend the picture past its sixels */
	ck_assert_int_eq(decode_with(s, 0, 1, NULL, "\"1;1;10;20#1~", &img), 0);
	ck_assert_uint_eq(img.width, 10);
	ck_assert_uint_eq(img.height, 20);
	ck_assert_uint_eq(px(&img, 0, 5), VT_BLUE);
	ck_assert_uint_eq(px(&img, 1, 0), CLEAR);
	ck_assert_uint_eq(px(&img, 0, 6), CLEAR);
	image_free(&img);

	/* opaque, the rest of the area takes register 0 */
	ck_assert_int_eq(decode_with(s, 0, 0, NULL, "\"1;1;3;4#1@", &img), 0);
	ck_assert_uint_eq(img.width, 3);
	ck_assert_uint_eq(img.height, 4);
	for (y = 0; y < 4; ++y)
		for (x = 0; x < 3; ++x)
			ck_assert_uint_eq(px(&img, x, y),
					  x || y ? BLACK : VT_BLUE);
	image_free(&img);

	/* raster attributes alone give a picture of register 0 */
	ck_assert_int_eq(decode_with(s, 0, 0, NULL, "\"1;1;2;2", &img), 0);
	ck_assert_uint_eq(img.width, 2);
	ck_assert_uint_eq(img.height, 2);
	ck_assert_uint_eq(px(&img, 1, 1), BLACK);
	image_free(&img);

	/* sixels reach past a smaller raster area */
	ck_assert_int_eq(decode_with(s, 0, 1, NULL, "\"1;1;1;4#1!2~", &img), 0);
	ck_assert_uint_eq(img.width, 2);
	ck_assert_uint_eq(img.height, 6);
	image_free(&img);

	/* the last band's unset rows inside the raster area stay; below it
	 * they are not part of the picture (a 5-row picture in one band) */
	ck_assert_int_eq(decode_with(s, 0, 0, NULL, "\"1;1;1;5#1N", &img), 0);
	ck_assert_uint_eq(img.height, 5);
	image_free(&img);

	/* the raster size counts the dots' scale */
	ck_assert_int_eq(decode_with(s, 0, 1, NULL, "\"2;1;3;4#1@", &img), 0);
	ck_assert_uint_eq(img.width, 3);
	ck_assert_uint_eq(img.height, 8);
	image_free(&img);

	tsm_sixel_free(s);
}
END_TEST

START_TEST(test_sixel_empty)
{
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	struct tsm_sixel_image img;

	ck_assert_int_eq(decode_with(s, 0, 0, NULL, "", &img), -ENODATA);
	ck_assert_ptr_eq(img.pixels, NULL);
	/* zero-size raster */
	ck_assert_int_eq(decode_with(s, 0, 0, NULL, "\"1;1;0;0", &img), -ENODATA);
	ck_assert_int_eq(decode_with(s, 0, 0, NULL, "\"1;1;0;5", &img), -ENODATA);
	ck_assert_int_eq(decode_with(s, 0, 0, NULL, "\"1;1;7", &img), -ENODATA);
	/* only controls and colours */
	ck_assert_int_eq(decode_with(s, 0, 0, NULL, "#1;2;1;2;3$--$", &img),
			 -ENODATA);
	/* '?' alone has a width but no rows */
	ck_assert_int_eq(decode_with(s, 0, 1, NULL, "???", &img), -ENODATA);

	tsm_sixel_free(s);
}
END_TEST

START_TEST(test_sixel_ignored_bytes)
{
	struct tsm_sixel_image img;
	const char data[] = "#1@ \r\n\t\x01\x7f\x80\xff%&'()*+,./:;<=>@";

	ck_assert_int_eq(decode(data, &img), 0);
	ck_assert_uint_eq(img.width, 2);
	ck_assert_uint_eq(img.height, 1);
	image_free(&img);

	/* any byte ends a parameter string; then it acts as itself */
	ck_assert_int_eq(decode("#1 @!3 @", &img), 0);
	ck_assert_uint_eq(img.width, 2);
	ck_assert_uint_eq(px(&img, 0, 0), VT_BLUE);
	image_free(&img);
}
END_TEST

START_TEST(test_sixel_streaming)
{
	const char *data = "\"1;1;7;9#7;2;10;20;30#3!3~$#7AB-#3!4N";
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	struct tsm_sixel_image whole, split;
	size_t i, len = strlen(data);

	ck_assert_int_eq(decode_with(s, 0, 1, NULL, data, &whole), 0);

	/* one byte at a time gives the same picture */
	memset(&split, 0, sizeof(split));
	tsm_sixel_begin(s, 0, 1, NULL);
	for (i = 0; i < len; ++i)
		tsm_sixel_feed(s, data + i, 1);
	ck_assert_int_eq(tsm_sixel_finish(s, &split), 0);

	ck_assert_uint_eq(split.width, whole.width);
	ck_assert_uint_eq(split.height, whole.height);
	ck_assert_mem_eq(split.pixels, whole.pixels,
			 (size_t)whole.width * whole.height * 4);
	image_free(&split);
	image_free(&whole);
	tsm_sixel_free(s);
}
END_TEST

START_TEST(test_sixel_shared_registers)
{
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	struct tsm_sixel_palette *shared;
	struct tsm_sixel_image img;

	ck_assert_int_eq(tsm_sixel_palette_new(&shared, 16), 0);

	/* a definition in one image persists into the next */
	ck_assert_int_eq(decode_with(s, 7, 1, shared, "#3;2;100;0;0@", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), RED);
	image_free(&img);
	ck_assert_int_eq(decode_with(s, 7, 1, shared, "#3@", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), RED);
	image_free(&img);

	/* private registers start from the defaults each image */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "#3;2;0;0;100@", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), BLUE);
	image_free(&img);
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "#3@", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), VT_GREEN);
	image_free(&img);

	/* and leave the shared table alone */
	ck_assert_int_eq(decode_with(s, 7, 1, shared, "#3@", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), RED);
	image_free(&img);

	/* RIS resets the shared table */
	tsm_sixel_palette_reset(shared);
	ck_assert_int_eq(decode_with(s, 7, 1, shared, "#3@", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), VT_GREEN);
	image_free(&img);

	tsm_sixel_palette_free(shared);
	tsm_sixel_free(s);
}
END_TEST

START_TEST(test_sixel_default_palette)
{
	struct tsm_sixel_image img;
	const uint32_t want[16] = {
		0xff000000, 0xff3333cc, 0xffcc2121, 0xff33cc33,
		0xffcc33cc, 0xff33cccc, 0xffcccc33, 0xff878787,
		0xff424242, 0xff545499, 0xff994242, 0xff549954,
		0xff995499, 0xff549999, 0xff999954, 0xffcccccc,
	};
	char data[256] = "";
	unsigned int i;

	for (i = 0; i < 18; ++i)
		sprintf(data + strlen(data), "#%u@", i);
	ck_assert_int_eq(decode(data, &img), 0);
	ck_assert_uint_eq(img.width, 18);
	for (i = 0; i < 16; ++i)
		ck_assert_msg(px(&img, i, 0) == want[i],
			      "register %u is %08x, want %08x", i,
			      px(&img, i, 0), want[i]);
	/* past the VT340's sixteen, registers start black */
	ck_assert_uint_eq(px(&img, 16, 0), BLACK);
	ck_assert_uint_eq(px(&img, 17, 0), BLACK);
	image_free(&img);
}
END_TEST

START_TEST(test_sixel_register_overflow)
{
	struct tsm_sixel *s = new_decoder(0, 0, 16);
	struct tsm_sixel_image img;

	/* a register past the table is its last one */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL,
				     "#20;2;100;0;0@#15@#99999999999999999999@",
				     &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), RED);
	ck_assert_uint_eq(px(&img, 1, 0), RED);
	ck_assert_uint_eq(px(&img, 2, 0), RED);
	image_free(&img);
	tsm_sixel_free(s);

	/* the table has at least two registers */
	s = new_decoder(0, 0, 1);
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "#1;2;0;100;0@#0@#5@", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), GREEN);
	ck_assert_uint_eq(px(&img, 1, 0), BLACK);
	ck_assert_uint_eq(px(&img, 2, 0), GREEN);
	image_free(&img);
	tsm_sixel_free(s);
}
END_TEST

START_TEST(test_sixel_max_size)
{
	struct tsm_sixel *s = new_decoder(4, 8, 0);
	struct tsm_sixel_image img;

	/* columns and rows past max-graphic-size are discarded */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "#1!10~-~-~", &img), 0);
	ck_assert_uint_eq(img.width, 4);
	ck_assert_uint_eq(img.height, 8);
	ck_assert_uint_eq(px(&img, 3, 0), VT_BLUE);
	ck_assert_uint_eq(px(&img, 0, 7), VT_BLUE);
	ck_assert_uint_eq(px(&img, 1, 7), CLEAR);
	image_free(&img);

	/* so is raster area past it */
	ck_assert_int_eq(decode_with(s, 7, 0, NULL, "\"1;1;100;100#1@", &img), 0);
	ck_assert_uint_eq(img.width, 4);
	ck_assert_uint_eq(img.height, 8);
	image_free(&img);

	/* and a picture that starts past it is empty */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "#1--~", &img), -ENODATA);

	tsm_sixel_free(s);
}
END_TEST

START_TEST(test_sixel_repeat_overflow)
{
	struct tsm_sixel *s = new_decoder(100, 100, 0);
	struct tsm_sixel_image img;

	/* a count past every integer saturates and stops at the width */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL,
				     "#1!99999999999999999999999~!4294967297~",
				     &img), 0);
	ck_assert_uint_eq(img.width, 100);
	ck_assert_uint_eq(img.height, 6);
	ck_assert_uint_eq(px(&img, 99, 5), VT_BLUE);
	image_free(&img);

	/* the same with the widest dots */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "\"1;5#1!4294967295~", &img), 0);
	ck_assert_uint_eq(img.width, 100);
	image_free(&img);

	tsm_sixel_free(s);
}
END_TEST

START_TEST(test_sixel_huge_raster)
{
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	struct tsm_sixel_image img;

	/* a raster area at the default limit, 10000x10000, needs 400 MB: a
	 * 16 MB budget refuses it, and the image is dropped */
	budget = 16 << 20;
	ck_assert_int_eq(decode_with(s, 7, 0, NULL,
				     "\"1;1;99999999;99999999#1~", &img),
			 -ENOMEM);
	ck_assert_ptr_eq(img.pixels, NULL);
	/* raster attributes alone allocate nothing until ST */
	ck_assert_int_eq(decode_with(s, 7, 0, NULL, "\"1;1;10000;10000", &img),
			 -ENOMEM);

	/* the decoder takes the next image as usual */
	ck_assert_int_eq(decode_with(s, 7, 0, NULL, "\"1;1;100;100#1~", &img), 0);
	ck_assert_uint_eq(img.width, 100);
	image_free(&img);

	/* refused while growing, partway through the payload */
	budget = 4096;
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "#1~-~-~-~-~-~-!2000~-~",
				     &img), -ENOMEM);
	ck_assert_ptr_eq(img.pixels, NULL);

	tsm_sixel_free(s);
}
END_TEST

START_TEST(test_sixel_truncated)
{
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	struct tsm_sixel_image img;

	/* ST inside a colour introducer still defines it */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "#1@#1;2;100;0;0", &img), 0);
	ck_assert_uint_eq(px(&img, 0, 0), RED);
	image_free(&img);

	/* ST after a repeat count drops the repeat */
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, "#1@!5", &img), 0);
	ck_assert_uint_eq(img.width, 1);
	image_free(&img);

	/* ST inside raster attributes still sets them */
	ck_assert_int_eq(decode_with(s, 7, 0, NULL, "\"1;1;3;2", &img), 0);
	ck_assert_uint_eq(img.width, 3);
	ck_assert_uint_eq(img.height, 2);
	image_free(&img);

	tsm_sixel_free(s);
}
END_TEST

START_TEST(test_sixel_abort)
{
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	struct tsm_sixel_image img;

	tsm_sixel_begin(s, 7, 1, NULL);
	tsm_sixel_feed(s, "#1!50~-!50~", 11);
	ck_assert_uint_gt(reserved, 0);
	tsm_sixel_abort(s);
	ck_assert_uint_eq(reserved, 0);
	ck_assert_int_eq(tsm_sixel_finish(s, &img), -EINVAL);

	/* a new begin discards the image in progress */
	tsm_sixel_begin(s, 7, 1, NULL);
	tsm_sixel_feed(s, "#1!50~", 6);
	ck_assert_uint_gt(reserved, 0);
	tsm_sixel_begin(s, 7, 1, NULL);
	ck_assert_uint_eq(reserved, 0);
	ck_assert_int_eq(tsm_sixel_finish(s, &img), -ENODATA);

	/* bytes with no image in progress are ignored */
	tsm_sixel_feed(s, "#1~", 3);
	ck_assert_uint_eq(reserved, 0);

	tsm_sixel_free(s);

	/* freeing mid-image releases its memory */
	s = new_decoder(0, 0, 0);
	tsm_sixel_begin(s, 7, 1, NULL);
	tsm_sixel_feed(s, "#1!50~", 6);
	tsm_sixel_free(s);
	ck_assert_uint_eq(reserved, 0);
}
END_TEST

START_TEST(test_sixel_growth)
{
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	struct tsm_sixel_image img;
	char data[4096];
	unsigned int i, x, y;
	size_t len = 0;

	/* without raster attributes the picture grows in both directions and
	 * keeps what it held: a diagonal of 40 bands, each one wider */
	len += sprintf(data + len, "#1");
	for (i = 0; i < 40; ++i)
		len += sprintf(data + len, i ? "!%u?@-" : "@-", i * 7);
	ck_assert_int_eq(decode_with(s, 7, 1, NULL, data, &img), 0);
	ck_assert_uint_eq(img.width, 39 * 7 + 1);
	ck_assert_uint_eq(img.height, 39 * 6 + 1);
	for (y = 0; y < img.height; ++y)
		for (x = 0; x < img.width; ++x)
			ck_assert_uint_eq(px(&img, x, y),
					  y % 6 == 0 && x == y / 6 * 7 ?
					  VT_BLUE : CLEAR);
	image_free(&img);
	tsm_sixel_free(s);
}
END_TEST

/* Fixtures */

struct ppm {
	unsigned int w, h;
	unsigned char *rgb;
};

static void *read_file(const char *name, size_t *len)
{
	char path[1024];
	FILE *f;
	char *buf;
	long n;

	snprintf(path, sizeof(path), "%s/%s", SIXEL_FIXTURES, name);
	f = fopen(path, "rb");
	ck_assert_msg(f != NULL, "cannot open %s", path);
	ck_assert_int_eq(fseek(f, 0, SEEK_END), 0);
	n = ftell(f);
	ck_assert_int_ge(n, 0);
	rewind(f);
	buf = malloc(n + 1);
	ck_assert_ptr_ne(buf, NULL);
	ck_assert_uint_eq(fread(buf, 1, n, f), (size_t)n);
	buf[n] = 0;
	fclose(f);
	*len = n;
	return buf;
}

static struct ppm read_ppm(const char *name)
{
	struct ppm ppm;
	unsigned int max;
	size_t len;
	int off = 0;
	char *buf = read_file(name, &len);

	ck_assert_int_eq(sscanf(buf, "P6 %u %u %u%n", &ppm.w, &ppm.h, &max, &off), 3);
	ck_assert_uint_eq(max, 255);
	ck_assert_uint_eq(len, off + 1 + (size_t)ppm.w * ppm.h * 3);
	ppm.rgb = malloc((size_t)ppm.w * ppm.h * 3);
	memcpy(ppm.rgb, buf + off + 1, (size_t)ppm.w * ppm.h * 3);
	free(buf);
	return ppm;
}

/*
 * Decodes a recorded DCS sixel string: ESC P P1;P2;P3 q payload ESC \, the
 * payload fed in chunks of @chunk bytes.
 */
static void decode_file(const char *name, size_t chunk,
			struct tsm_sixel_image *img)
{
	struct tsm_sixel *s = new_decoder(0, 0, 0);
	size_t len, i, end;
	int p[3] = { -1, -1, -1 }, n = 0;
	char *buf = read_file(name, &len), *q;

	ck_assert(len > 4 && buf[0] == '\033' && buf[1] == 'P');
	for (q = buf + 2; *q != 'q'; ++q) {
		ck_assert(*q == ';' || (*q >= '0' && *q <= '9'));
		if (*q == ';')
			++n;
		else if (n < 3)
			p[n] = (p[n] < 0 ? 0 : p[n] * 10) + (*q - '0');
	}
	++q;
	end = len - (q - buf);
	ck_assert(end >= 2 && q[end - 2] == '\033' && q[end - 1] == '\\');
	end -= 2;

	tsm_sixel_begin(s, p[0], p[1], NULL);
	for (i = 0; i < end; i += chunk)
		tsm_sixel_feed(s, q + i, end - i < chunk ? end - i : chunk);
	ck_assert_int_eq(tsm_sixel_finish(s, img), 0);
	free(buf);
	tsm_sixel_free(s);
}

/*
 * Compares a decoded fixture with libsixel's own decoding of it (NAME.ref.ppm,
 * sixel2png) and its source picture (NAME.ppm). The size is libsixel's; over
 * the source's area every channel is within @ref_err of libsixel's and,
 * unless @max_err is negative, within @max_err of the source, with the mean
 * error within @mean_err. (libsixel fills pixels outside the raster area that
 * no sixel set with a colour of its own, so they are not compared.)
 */
static void compare_fixture(const char *base, unsigned int ref_err,
			    int max_err, double mean_err)
{
	char name[256];
	struct tsm_sixel_image img, img1;
	struct ppm src, ref;
	unsigned int x, y, c, e, worst = 0, worst_ref = 0;
	double total = 0;

	snprintf(name, sizeof(name), "%s.six", base);
	decode_file(name, 4096, &img);
	snprintf(name, sizeof(name), "%s.ppm", base);
	src = read_ppm(name);
	snprintf(name, sizeof(name), "%s.ref.ppm", base);
	ref = read_ppm(name);

	ck_assert_uint_eq(img.width, ref.w);
	ck_assert_uint_eq(img.height, ref.h);
	ck_assert_uint_eq(img.width, src.w);
	ck_assert_uint_ge(img.height, src.h);

	for (y = 0; y < src.h; ++y) {
		for (x = 0; x < img.width; ++x) {
			uint32_t v = px(&img, x, y);
			size_t off = ((size_t)y * img.width + x) * 3;
			unsigned int got[3] = { v >> 16 & 0xff, v >> 8 & 0xff,
						v & 0xff };

			ck_assert_uint_eq(v >> 24, 0xff);
			for (c = 0; c < 3; ++c) {
				e = abs((int)got[c] - ref.rgb[off + c]);
				ck_assert_msg(e <= ref_err,
					      "%s: %u,%u channel %u is %u, libsixel %u",
					      base, x, y, c, got[c], ref.rgb[off + c]);
				worst_ref = e > worst_ref ? e : worst_ref;
				if (max_err < 0)
					continue;
				e = abs((int)got[c] - src.rgb[off + c]);
				ck_assert_msg(e <= (unsigned int)max_err,
					      "%s: %u,%u channel %u is %u, source %u",
					      base, x, y, c, got[c], src.rgb[off + c]);
				worst = e > worst ? e : worst;
				total += e;
			}
		}
	}
	total /= (double)src.w * src.h * 3;
	ck_assert_msg(max_err < 0 || total <= mean_err, "%s: mean error %.2f, worst %u", base,
		      total, worst);
	fprintf(stderr, "%s: %ux%u, libsixel within %u, source within %u, "
		"mean %.2f\n", base, img.width, img.height, worst_ref, worst,
		total);

	/* one byte at a time decodes the same */
	snprintf(name, sizeof(name), "%s.six", base);
	decode_file(name, 1, &img1);
	ck_assert_mem_eq(img1.pixels, img.pixels,
			 (size_t)img.width * img.height * 4);
	image_free(&img1);

	image_free(&img);
	free(src.rgb);
	free(ref.rgb);
}

START_TEST(test_sixel_fixture_palette16)
{
	/* 16 flat colours encoded with 16 registers and no dithering: the
	 * error is libsixel's colour averaging and percent rounding */
	compare_fixture("palette16", 1, 8, 3.0);
}
END_TEST

START_TEST(test_sixel_fixture_gradient)
{
	/* a 256-register dithered gradient: each pixel near its source */
	compare_fixture("gradient", 1, 24, 6.0);
}
END_TEST

START_TEST(test_sixel_fixture_highcolor)
{
	/* img2sixel -I, 15-bit colour, redefines registers band after band.
	 * libsixel 1.10.5 encodes this mode wrongly: its colours stray from
	 * the source, a sixth band lands below the 30 rows, and the bytes
	 * differ from run to run. Its own decoder reads the stream the same
	 * way, so only that comparison holds. */
	compare_fixture("highcolor", 1, -1, 0);
}
END_TEST

TEST_DEFINE_CASE(decode)
	TEST(test_sixel_bits)
	TEST(test_sixel_all_sixels)
	TEST(test_sixel_default_register)
	TEST(test_sixel_repeat)
	TEST(test_sixel_carriage_return)
	TEST(test_sixel_new_line)
	TEST(test_sixel_rgb)
	TEST(test_sixel_hls)
	TEST(test_sixel_incomplete_colour)
	TEST(test_sixel_redefine_after_draw)
	TEST(test_sixel_background)
	TEST(test_sixel_aspect)
	TEST(test_sixel_raster_size)
	TEST(test_sixel_streaming)
	TEST(test_sixel_shared_registers)
	TEST(test_sixel_default_palette)
	TEST(test_sixel_growth)
TEST_END_CASE

TEST_DEFINE_CASE(edges)
	TEST(test_sixel_empty)
	TEST(test_sixel_ignored_bytes)
	TEST(test_sixel_register_overflow)
	TEST(test_sixel_max_size)
	TEST(test_sixel_repeat_overflow)
	TEST(test_sixel_huge_raster)
	TEST(test_sixel_truncated)
	TEST(test_sixel_abort)
TEST_END_CASE

TEST_DEFINE_CASE(fixtures)
	TEST(test_sixel_fixture_palette16)
	TEST(test_sixel_fixture_gradient)
	TEST(test_sixel_fixture_highcolor)
TEST_END_CASE

TEST_DEFINE(
	TEST_SUITE(sixel,
		TEST_CASE(decode),
		TEST_CASE(edges),
		TEST_CASE(fixtures),
		TEST_END
	)
)
