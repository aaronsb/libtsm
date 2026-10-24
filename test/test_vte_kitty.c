/*
 * TSM - VTE kitty graphics tests
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
 * The kitty graphics protocol through the vte (ADR-103 step 4a in
 * aaronsb/kmscon): what the tier 1 cases cannot see. The switch and the
 * budget it needs, input split anywhere and bounded, C1 framing, the bytes a
 * pending upload holds and gives back, and the pixels it stores.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "test_common.h"

/* what a picture of @w x @h pixels costs the budget */
#define COST(w, h) ((size_t)(w) * (h) * 4 + TSM_SCREEN_IMAGE_OVERHEAD)

/* the last message logged at debug level */
static char debug_log[256];

static void log_cb(void *data, const char *file, int line, const char *func,
		   const char *subs, unsigned int sev, const char *format,
		   va_list args)
{
	UNUSED(data);
	UNUSED(file);
	UNUSED(line);
	UNUSED(func);
	UNUSED(subs);
	if (sev == LLOG_DEBUG)
		vsnprintf(debug_log, sizeof(debug_log), format, args);
}

static char reply[1024];
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

/* a 10x5 screen of 8x16 cells, a 64 MiB limit, kitty graphics on */
static void setup(void)
{
	ck_assert_int_eq(tsm_screen_new(&screen, log_cb, NULL), 0);
	ck_assert_int_eq(tsm_screen_resize(screen, 10, 5), 0);
	ck_assert_int_eq(tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL), 0);
	ck_assert_int_eq(tsm_screen_set_cell_size(screen, 8, 16), 0);
	tsm_screen_set_image_limit(screen, 64 << 20);
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

static const char b64chars[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* @len bytes at @data as base64 into @out, NUL-terminated */
static void b64(char *out, const uint8_t *data, size_t len)
{
	size_t i;
	uint32_t v;

	for (i = 0; i < len; i += 3) {
		v = data[i] << 16 | (i + 1 < len ? data[i + 1] << 8 : 0) |
		    (i + 2 < len ? data[i + 2] : 0);
		*out++ = b64chars[v >> 18];
		*out++ = b64chars[(v >> 12) & 63];
		*out++ = i + 1 < len ? b64chars[(v >> 6) & 63] : '=';
		*out++ = i + 2 < len ? b64chars[v & 63] : '=';
	}
	*out = 0;
}

/*
 * A command with @keys and a payload of @w x @h pixels of @bpp bytes, each
 * @px, in a buffer the caller frees.
 */
static char *command(const char *keys, unsigned int w, unsigned int h,
		     unsigned int bpp, const uint8_t *px)
{
	size_t n = (size_t)w * h * bpp, i;
	uint8_t *data = malloc(n + 1);
	char *out = malloc(strlen(keys) + n * 2 + 16);

	ck_assert_ptr_ne(data, NULL);
	ck_assert_ptr_ne(out, NULL);
	for (i = 0; i < n; ++i)
		data[i] = px[i % bpp];
	i = sprintf(out, "\033_G%s;", keys);
	b64(out + i, data, n);
	strcat(out, "\033\\");
	free(data);
	return out;
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
	return ((const uint32_t *)((const char *)img.pixels + y * img.stride))[x];
}

static const uint8_t RED[4] = { 255, 0, 0, 255 };

/* Off by default and without a budget: no image and no reply */
START_TEST(test_kitty_off)
{
	char *cmd = command("a=T,f=32,s=1,v=1,i=1", 1, 1, 4, RED);

	tsm_vte_set_kitty_graphics(vte, false);
	input(cmd);
	ck_assert_uint_eq(placements(), 0);
	ck_assert_uint_eq(reply_len, 0);

	tsm_vte_set_kitty_graphics(vte, true);
	tsm_screen_set_image_limit(screen, 0);
	input(cmd);
	ck_assert_uint_eq(placements(), 0);
	ck_assert_uint_eq(reply_len, 0);

	tsm_screen_set_image_limit(screen, 64 << 20);
	input(cmd);
	ck_assert_uint_eq(placements(), 1);
	ck_assert_str_eq(reply, "\033_Gi=1;OK\033\\");
	free(cmd);
}
END_TEST

/* straight alpha becomes premultiplied ARGB, and f=24 is opaque */
START_TEST(test_kitty_pixels)
{
	static const uint8_t half[4] = { 200, 100, 50, 128 };
	static const uint8_t blue[3] = { 0, 0, 255 };
	char *cmd;

	cmd = command("a=T,f=32,s=2,v=1,C=1", 2, 1, 4, half);
	input(cmd);
	free(cmd);
	cmd = command("a=T,f=24,s=1,v=1,C=1,z=1", 1, 1, 3, blue);
	input(cmd);
	free(cmd);

	ck_assert_uint_eq(placements(), 2);
	ck_assert_uint_eq(pixel(placement(0), 1, 0),
			  0x80000000u | 100u << 16 | 50u << 8 | 25u);
	ck_assert(!(placement(0)->flags & TSM_FRAME_OPAQUE));
	ck_assert_uint_eq(pixel(placement(1), 0, 0), 0xff0000ffu);
	ck_assert(placement(1)->flags & TSM_FRAME_OPAQUE);
}
END_TEST

/*
 * Input split at every byte takes the parser's path one byte at a time, and
 * gives what whole input gives through the payload's fast path
 */
START_TEST(test_kitty_split)
{
	char *cmd = command("a=T,f=24,s=3,v=2,i=4", 3, 2, 3, RED);
	size_t i;

	for (i = 0; cmd[i]; ++i)
		tsm_vte_input(vte, cmd + i, 1);
	ck_assert_uint_eq(placements(), 1);
	ck_assert_uint_eq(pixel(placement(0), 2, 1), 0xffff0000u);
	ck_assert_str_eq(reply, "\033_Gi=4;OK\033\\");
	free(cmd);
}
END_TEST

/*
 * Raw data past s x v x bytes per pixel is ignored, as kitty ignores it, in a
 * transmission and in a=q alike; data short of it is refused
 */
START_TEST(test_kitty_excess)
{
	static const uint8_t blue[3] = { 0, 0, 255 };
	char *cmd;

	debug_log[0] = 0;
	cmd = command("a=T,f=24,s=1,v=1,i=1", 3, 1, 3, blue);
	input(cmd);
	free(cmd);
	ck_assert_str_eq(reply, "\033_Gi=1;OK\033\\");
	ck_assert_str_eq(debug_log, "kitty image i=1, t=d: 6 bytes past s x v ignored");
	ck_assert_uint_eq(placements(), 1);
	ck_assert_uint_eq(pixel(placement(0), 0, 0), 0xff0000ffu);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(1, 1));

	reply_clear();
	cmd = command("a=q,f=24,s=1,v=1,i=2", 5, 1, 3, blue);
	input(cmd);
	free(cmd);
	ck_assert_str_eq(reply, "\033_Gi=2;OK\033\\");

	/* an anonymous image is named as one; its six bytes for one f=32
	 * pixel end part way through a base64 quad */
	cmd = command("a=T,f=32,s=1,v=1,C=1", 6, 1, 1, blue);
	input(cmd);
	free(cmd);
	ck_assert_str_eq(debug_log,
			 "kitty image (anonymous), t=d: 2 bytes past s x v ignored");

	reply_clear();
	cmd = command("a=q,f=24,s=2,v=1,i=3", 1, 1, 3, blue);
	input(cmd);
	free(cmd);
	ck_assert_str_eq(reply, "\033_Gi=3;EINVAL:invalid argument\033\\");
}
END_TEST

/* C1 framing, APC and ST: encoded in UTF-8, and as bytes in 8-bit mode */
START_TEST(test_kitty_c1)
{
	static const char utf8[] = "\xc2\x9fGa=T,f=24,s=1,v=1,i=2,C=1;/wAA\xc2\x9c";
	static const char bytes[] = "\x9fGa=T,f=24,s=1,v=1,i=3,C=1;/wAA\x9c";

	input(utf8);
	ck_assert_uint_eq(placements(), 1);
	ck_assert_str_eq(reply, "\033_Gi=2;OK\033\\");

	reply_clear();
	input("\033[62\"p");	/* DECSCL: VT200, 8-bit */
	input(bytes);
	ck_assert_uint_eq(placements(), 2);
	ck_assert_str_eq(reply, "\033_Gi=3;OK\033\\");
}
END_TEST

/*
 * A large upload under the bounded call: each call stops once its work is
 * done, and the rest arrives over later calls
 */
START_TEST(test_kitty_bounded)
{
	static const uint8_t grey[3] = { 9, 9, 9 };
	unsigned int w = 4096, h = 2048, calls = 0;
	char *cmd = command("a=T,f=24,s=4096,v=2048,i=1", w, h, 3, grey);
	size_t len = strlen(cmd), pos = 0, n;

	while (pos < len) {
		n = tsm_vte_input_bounded(vte, cmd + pos, len - pos);
		ck_assert_uint_gt(n, 0);
		pos += n;
		++calls;
	}
	ck_assert_uint_gt(calls, 1);
	ck_assert_uint_eq(placements(), 1);
	ck_assert_str_eq(reply, "\033_Gi=1;OK\033\\");
	free(cmd);
}
END_TEST

/*
 * A raw upload reserves its image's bytes with its first chunk; aborting it
 * any way gives them back
 */
START_TEST(test_kitty_upload_bytes)
{
	static const char first[] = "\033_Ga=T,f=32,s=4,v=4,i=1,m=1;AAAA\033\\";
	static const char rest[] = "\033_Gm=0;AAAA\033\\";

	input(first);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(4, 4));
	input("\033_Ga=d\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);

	input(first);
	tsm_vte_reset(vte);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);

	input(first);
	tsm_vte_set_kitty_graphics(vte, false);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	tsm_vte_set_kitty_graphics(vte, true);

	input(first);
	input("\033_Gm=1;AAAA\030");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);

	/* short: the bytes go back with the refusal */
	input(first);
	input(rest);
	ck_assert_str_eq(reply, "\033_Gi=1;EINVAL:invalid argument\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);

	/* and freeing the vte gives back an upload's */
	input(first);
	tsm_vte_unref(vte);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	ck_assert_int_eq(tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL), 0);
}
END_TEST

static size_t host_used;
static size_t host_budget;

static int host_reserve(struct tsm_screen *con, size_t bytes, void *data)
{
	UNUSED(con);
	UNUSED(data);
	if (host_used + bytes > host_budget)
		return -ENOSPC;
	host_used += bytes;
	return 0;
}

static void host_release(struct tsm_screen *con, size_t bytes, void *data)
{
	UNUSED(con);
	UNUSED(data);
	ck_assert_uint_ge(host_used, bytes);
	host_used -= bytes;
}

/*
 * Under a host budget: the first chunk reserves, refused it answers ENOSPC
 * at the last chunk; a replacement needs only what it adds
 */
START_TEST(test_kitty_host_budget)
{
	char *cmd;

	host_used = 0;
	host_budget = COST(4, 4);
	ck_assert_int_eq(tsm_screen_set_image_budget(screen, host_reserve, host_release,
						     NULL), 0);
	cmd = command("a=T,f=32,s=4,v=4,i=1", 4, 4, 4, RED);
	input(cmd);
	free(cmd);
	ck_assert_str_eq(reply, "\033_Gi=1;OK\033\\");
	ck_assert_uint_eq(host_used, COST(4, 4));

	/* another image does not fit beside it */
	reply_clear();
	cmd = command("a=t,f=32,s=2,v=2,i=2", 2, 2, 4, RED);
	input(cmd);
	free(cmd);
	ck_assert_str_eq(reply, "\033_Gi=2;ENOSPC:image storage is full\033\\");
	ck_assert_uint_eq(host_used, COST(4, 4));

	/* the same id, smaller, fits in its room and gives the rest back */
	reply_clear();
	cmd = command("a=t,f=32,s=2,v=2,i=1", 2, 2, 4, RED);
	input(cmd);
	free(cmd);
	ck_assert_str_eq(reply, "\033_Gi=1;OK\033\\");
	ck_assert_uint_eq(host_used, COST(2, 2));
	ck_assert_uint_eq(placements(), 0);

	tsm_vte_unref(vte);
	vte = NULL;
	tsm_screen_unref(screen);
	screen = NULL;
	ck_assert_uint_eq(host_used, 0);
	ck_assert_int_eq(tsm_screen_new(&screen, log_cb, NULL), 0);
	ck_assert_int_eq(tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL), 0);
}
END_TEST

/*
 * Rank 3 reports a kitty image's transmission in last_use: an image placed
 * again keeps its stamp, so it is named before a newer one placed earlier
 */
START_TEST(test_kitty_victim_stamp)
{
	struct tsm_image_victim v;
	char *cmd;
	uint64_t first;

	cmd = command("a=T,f=32,s=1,v=1,i=1,C=1", 1, 1, 4, RED);
	input(cmd);
	free(cmd);
	first = placement(0)->image_key;
	cmd = command("a=T,f=32,s=1,v=1,i=2,C=1", 1, 1, 4, RED);
	input("\033[1;3H");
	input(cmd);
	free(cmd);
	input("\033[2;1H\033_Ga=p,i=1,C=1\033\\");

	ck_assert(tsm_screen_image_victim(screen, &v));
	ck_assert_uint_eq(v.rank, TSM_IMAGE_PLACED_KITTY);
	ck_assert_uint_eq(v.key, first);
	ck_assert_uint_eq(v.bytes, COST(1, 1) + TSM_SCREEN_PLACEMENT_OVERHEAD);
}
END_TEST

/*
 * A flood of deletes counts as work: each scans the store and the screen's
 * lines, so the bounded call stops early rather than spend seconds on input
 * that holds no image data
 */
START_TEST(test_kitty_delete_bound)
{
	static const char del[] = "\033_Ga=d,d=x,x=99\033\\";
	struct tsm_test_image img = { .width = 1, .height = 1, .kind = TSM_IMAGE_KITTY };
	uint32_t px = 0xff000000;
	size_t n = 20000, len = n * (sizeof(del) - 1), used, i;
	uint64_t key;
	char *buf;

	img.pixels = &px;
	for (i = 1; i <= TSM_SCREEN_IMAGES_PER_STORE; ++i) {
		img.id = i;
		ck_assert_int_eq(tsm_screen_test_image(screen, &img, &key), 0);
	}
	buf = malloc(len);
	ck_assert_ptr_ne(buf, NULL);
	for (i = 0; i < n; ++i)
		memcpy(buf + i * (sizeof(del) - 1), del, sizeof(del) - 1);
	used = tsm_vte_input_bounded(vte, buf, len);
	ck_assert_uint_gt(used, 0);
	ck_assert_uint_lt(used, len);
	free(buf);
}
END_TEST

/* an uppercase N whose p matches no placement frees nothing */
START_TEST(test_kitty_delete_number_placement)
{
	char *cmd = command("a=t,f=24,s=1,v=1,I=5", 1, 1, 3, RED);

	input(cmd);
	free(cmd);
	reply_clear();
	input("\033_Ga=d,d=N,I=5,p=3\033\\\033_Ga=p,I=5,C=1\033\\");
	ck_assert_str_eq(reply, "\033_Gi=1,I=5;OK\033\\");
	ck_assert_uint_eq(placements(), 1);

	/* without p it frees the image once unplaced */
	reply_clear();
	input("\033_Ga=d,d=N,I=5\033\\\033_Ga=p,I=5\033\\");
	ck_assert_str_eq(reply, "\033_GI=5;ENOENT:image not found\033\\");
}
END_TEST

/* control keys up to 1 KiB are read; one byte more ignores the whole APC */
START_TEST(test_kitty_keys_limit)
{
	static const char head[] = "a=T,f=24,s=1,v=1,i=1,K=";
	char keys[1100], cmd[1200];
	size_t n;

	for (n = 1024; n <= 1025; ++n) {
		reply_clear();
		memset(keys, '0', sizeof(keys));
		memcpy(keys, head, sizeof(head) - 1);
		keys[n] = 0;
		snprintf(cmd, sizeof(cmd), "\033_G%s;/wAA\033\\", keys);
		input(cmd);
		if (n == 1024)
			ck_assert_str_eq(reply, "\033_Gi=1;OK\033\\");
		else
			ck_assert_uint_eq(reply_len, 0);
	}
}
END_TEST

/* a later chunk's q=0 keeps the first chunk's q, as Ghostty keeps it */
START_TEST(test_kitty_chunk_quiet)
{
	input("\033_Ga=T,f=24,s=1,v=1,i=1,q=1,m=1;/w==\033\\\033_Gm=0,q=0;AAA=\033\\");
	ck_assert_uint_eq(placements(), 1);
	ck_assert_uint_eq(reply_len, 0);
}
END_TEST

/*
 * An image whose bytes and bookkeeping would pass a 32-bit size_t is
 * refused before anything is reserved: 32767 x 32769 x 4 fits in 32 bits,
 * plus TSM_SCREEN_IMAGE_OVERHEAD it does not
 */
START_TEST(test_kitty_size_bound)
{
	ck_assert_int_eq(tsm_vte_set_graphic_max_size(vte, 65535, 65535), 0);
	input("\033_Ga=t,f=32,s=32767,v=32769,i=1\033\\");
	ck_assert_str_eq(reply, "\033_Gi=1;EINVAL:invalid argument\033\\");
}
END_TEST

TEST_DEFINE_CASE(kitty)
	CHECKED_FIXTURE(setup, teardown)
	TEST(test_kitty_off)
	TEST(test_kitty_pixels)
	TEST(test_kitty_excess)
	TEST(test_kitty_split)
	TEST(test_kitty_c1)
	TEST(test_kitty_bounded)
	TEST(test_kitty_upload_bytes)
	TEST(test_kitty_host_budget)
	TEST(test_kitty_victim_stamp)
	TEST(test_kitty_delete_bound)
	TEST(test_kitty_delete_number_placement)
	TEST(test_kitty_keys_limit)
	TEST(test_kitty_chunk_quiet)
	TEST(test_kitty_size_bound)
TEST_END_CASE

TEST_DEFINE(
	TEST_SUITE(vte_kitty,
		TEST_CASE(kitty),
		TEST_END
	)
)
