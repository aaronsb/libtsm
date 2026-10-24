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
 * pending upload holds and gives back, and the pixels it stores; and the host
 * hook (the Notes of 2026-10-24): the request, the pause, each error from the
 * hook and the reply, a stale id, a reply from inside the hook, and each way
 * a request is cancelled.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "test_common.h"

/* what a picture of @w x @h pixels costs the budget */
#define COST(w, h) ((size_t)(w) * (h) * 4 + TSM_SCREEN_IMAGE_OVERHEAD)

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
 * The host hook
 */

/* what the hook saw, and what it answers */
static struct {
	unsigned int calls;
	struct tsm_vte_image_request req;
	char name[4096];
	uint8_t data[256];
	int ret;			/* the hook's return */
	bool reply_inside;		/* the hook calls tsm_vte_image_reply */
	int inside;			/* what that call returned */
} hook;

static int hook_cb(struct tsm_vte *v, const struct tsm_vte_image_request *req,
		   void *data)
{
	static const uint8_t px[4] = { 1, 2, 3, 255 };

	ck_assert_ptr_eq(v, vte);
	ck_assert_ptr_eq(data, &hook);
	++hook.calls;
	hook.req = *req;
	hook.name[0] = 0;
	if (req->name)
		snprintf(hook.name, sizeof(hook.name), "%s", req->name);
	if (req->data) {
		ck_assert_uint_le(req->data_len, sizeof(hook.data));
		memcpy(hook.data, req->data, req->data_len);
	}
	if (hook.reply_inside)
		hook.inside = tsm_vte_image_reply(vte, req->id, 0, 1, 1, px);
	return hook.ret;
}

static void hook_setup(void)
{
	setup();
	memset(&hook, 0, sizeof(hook));
	tsm_vte_set_image_cb(vte, hook_cb, &hook);
}

static size_t bounded(const char *s)
{
	return tsm_vte_input_bounded(vte, s, strlen(s));
}

static const uint8_t PX2[16] = {
	255, 0, 0, 255,  0, 255, 0, 255,  0, 0, 255, 255,  9, 9, 9, 128,
};

/* each medium reaches the hook with its keys, and inline raw data does not */
START_TEST(test_hook_request)
{
	/* "/tmp/x" */
	bounded("\033_Ga=T,t=f,f=24,s=2,v=1,O=3,S=6,i=1;L3RtcC94\033\\");
	ck_assert_uint_eq(hook.calls, 1);
	ck_assert_uint_eq(hook.req.id, 1);
	ck_assert_uint_eq(hook.req.medium, TSM_VTE_IMAGE_FILE);
	ck_assert_uint_eq(hook.req.format, 24);
	ck_assert(!hook.req.zlib && !hook.req.discard);
	ck_assert_uint_eq(hook.req.width, 2);
	ck_assert_uint_eq(hook.req.height, 1);
	ck_assert_uint_eq(hook.req.offset, 3);
	ck_assert_uint_eq(hook.req.size, 6);
	ck_assert_str_eq(hook.name, "/tmp/x");
	ck_assert_ptr_eq(hook.req.data, NULL);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 1, 0, 2, 1, PX2), 0);
	ck_assert_str_eq(reply, "\033_Gi=1;OK\033\\");

	/* inline PNG, then zlib: the payload, decoded */
	bounded("\033_Ga=t,f=100,i=2;iVBORw==\033\\");
	ck_assert_uint_eq(hook.calls, 2);
	ck_assert_uint_eq(hook.req.id, 2);
	ck_assert_uint_eq(hook.req.medium, TSM_VTE_IMAGE_DIRECT);
	ck_assert_ptr_eq(hook.req.name, NULL);
	ck_assert_uint_eq(hook.req.data_len, 4);
	ck_assert_mem_eq(hook.data, "\x89PNG", 4);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 2, -EINVAL, 0, 0, NULL), 0);
	bounded("\033_Ga=t,f=32,s=1,v=1,o=z,t=d,i=3;eJw=\033\\");
	ck_assert(hook.req.zlib);
	ck_assert_uint_eq(hook.req.data_len, 2);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 3, -EBADF, 0, 0, NULL), 0);

	/* t=t and t=s */
	bounded("\033_Ga=t,t=t,f=100,i=4;L3RtcC94\033\\");
	ck_assert_uint_eq(hook.req.medium, TSM_VTE_IMAGE_TEMP);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 4, -EBADF, 0, 0, NULL), 0);
	bounded("\033_Ga=t,t=s,f=100,i=5;L3NobQ==\033\\");
	ck_assert_uint_eq(hook.req.medium, TSM_VTE_IMAGE_SHM);
	ck_assert_str_eq(hook.name, "/shm");
	ck_assert_int_eq(tsm_vte_image_reply(vte, 5, -EBADF, 0, 0, NULL), 0);

	/* inline raw data stays in libtsm */
	bounded("\033_Ga=T,f=24,s=1,v=1,i=6;/wAA\033\\");
	ck_assert_uint_eq(hook.calls, 5);
	ck_assert_uint_eq(placements(), 2);
}
END_TEST

/*
 * The pause: the bounded call returns the bytes through the ESC that ended
 * the command and then nothing until the reply; the rest, the ST's
 * backslash first, is parsed after it, in order
 */
START_TEST(test_hook_pause)
{
	static const char in[] = "\033_Ga=t,t=f,f=100,i=1;L3RtcC94\033\\\033_Ga=p,i=1\033\\X\033[6n";
	const char *end = strchr(in, '\\');
	size_t n;

	n = bounded(in);
	ck_assert_uint_eq(n, (size_t)(end - in));
	ck_assert_uint_eq(bounded(in + n), 0);
	ck_assert_uint_eq(reply_len, 0);
	/* tsm_vte_input drops bytes while the request is out */
	input("Y");
	ck_assert_int_eq(tsm_vte_image_reply(vte, 1, 0, 2, 2, PX2), 0);
	ck_assert_uint_eq(bounded(in + n), strlen(in + n));
	ck_assert_str_eq(reply, "\033_Gi=1;OK\033\\\033_Gi=1;OK\033\\\033[1;3R");
	ck_assert_uint_eq(placements(), 1);
	ck_assert_uint_eq(tsm_screen_get_cursor_x(screen), 2);

	/* a C1 ST ends the command at its own byte */
	reply_clear();
	ck_assert_uint_eq(bounded("\xc2\x9fGa=t,t=f,f=100,i=2;L3RtcC94\xc2\x9c" "Z"),
			  strlen("\xc2\x9fGa=t,t=f,f=100,i=2;L3RtcC94\xc2\x9c"));
	ck_assert_int_eq(tsm_vte_image_reply(vte, 2, -EBADF, 0, 0, NULL), 0);
	ck_assert_str_eq(reply, "\033_Gi=2;EBADF:Failed to read image file\033\\");
}
END_TEST

/* the hook's error answers at once, and parsing goes on in the same call */
START_TEST(test_hook_errors)
{
	static const int errs[] = { -EBADF, -EINVAL, -ENOSPC, -EIO };
	static const char *const answers[] = {
		"\033_Gi=1;EBADF:Failed to read image file\033\\",
		"\033_Gi=1;EINVAL:invalid argument\033\\",
		"\033_Gi=1;ENOSPC:image storage is full\033\\",
		"\033_Gi=1;EBADF:Failed to read image file\033\\",
	};
	static const char in[] = "\033_Ga=t,t=f,f=100,i=1;L3RtcC94\033\\\033[6n";
	unsigned int i;
	char want[128];

	for (i = 0; i < 4; ++i) {
		reply_clear();
		hook.ret = errs[i];
		ck_assert_uint_eq(bounded(in), strlen(in));
		snprintf(want, sizeof(want), "%s\033[1;1R", answers[i]);
		ck_assert_str_eq(reply, want);
		/* the request is not out */
		ck_assert_int_eq(tsm_vte_image_reply(vte, hook.req.id, -EBADF, 0, 0, NULL),
				 -ENOENT);
	}

	/* the reply's errors, any other negative one answering EBADF */
	hook.ret = 0;
	for (i = 0; i < 4; ++i) {
		reply_clear();
		bounded(in);
		ck_assert_int_eq(tsm_vte_image_reply(vte, hook.req.id, errs[i], 0, 0, NULL), 0);
		ck_assert_str_eq(reply, answers[i]);
	}
}
END_TEST

/*
 * A stale id, a reply with nothing out, and one from inside the hook change
 * nothing; a reply with no pixels ends the request with EBADF
 */
START_TEST(test_hook_reply_misuse)
{
	static const char in[] = "\033_Ga=t,t=f,f=100,i=1;L3RtcC94\033\\";

	ck_assert_int_eq(tsm_vte_image_reply(vte, 1, 0, 2, 2, PX2), -ENOENT);
	bounded(in);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 7, 0, 2, 2, PX2), -ENOENT);
	ck_assert_uint_eq(reply_len, 0);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 1, 0, 2, 2, NULL), -EINVAL);
	ck_assert_str_eq(reply, "\033_Gi=1;EBADF:Failed to read image file\033\\");
	ck_assert_uint_gt(bounded("x"), 0);

	reply_clear();
	bounded(in);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 2, 0, 0, 2, PX2), -EINVAL);
	ck_assert_str_eq(reply, "\033_Gi=1;EBADF:Failed to read image file\033\\");

	/* -EBUSY from inside the hook leaves the request out */
	reply_clear();
	hook.reply_inside = true;
	bounded(in);
	ck_assert_int_eq(hook.inside, -EBUSY);
	ck_assert_uint_eq(bounded("x"), 0);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 3, 0, 2, 2, PX2), 0);
	ck_assert_str_eq(reply, "\033_Gi=1;OK\033\\");
}
END_TEST

/*
 * Every way a request is cancelled drops it with no reply, and a later reply
 * finds nothing; changing the hook answers it EBADF
 */
START_TEST(test_hook_cancel)
{
	static const char in[] = "\033_Ga=t,t=f,f=100,i=1;L3RtcC94\033\\";
	unsigned int way;

	for (way = 0; way < 3; ++way) {
		reply_clear();
		bounded(in);
		ck_assert_uint_eq(bounded("x"), 0);
		if (way == 0)
			tsm_vte_reset(vte);
		else if (way == 1)
			tsm_vte_hard_reset(vte);
		else
			tsm_vte_set_kitty_graphics(vte, false);
		ck_assert_int_eq(tsm_vte_image_reply(vte, hook.req.id, 0, 2, 2, PX2),
				 -ENOENT);
		ck_assert_uint_eq(reply_len, 0);
		ck_assert_uint_eq(bounded("x"), 1);
		tsm_vte_set_kitty_graphics(vte, true);
	}

	/* freeing the vte with a request out */
	bounded(in);
	tsm_vte_unref(vte);
	ck_assert_int_eq(tsm_vte_new(&vte, screen, write_cb, NULL, log_cb, NULL), 0);
	tsm_vte_set_kitty_graphics(vte, true);
	tsm_vte_set_image_cb(vte, hook_cb, &hook);

	/* removing the hook, then installing one, each answer EBADF */
	reply_clear();
	bounded(in);
	tsm_vte_set_image_cb(vte, NULL, NULL);
	ck_assert_str_eq(reply, "\033_Gi=1;EBADF:Failed to read image file\033\\");
	ck_assert_uint_eq(bounded("x"), 1);
	tsm_vte_set_image_cb(vte, hook_cb, &hook);
	reply_clear();
	bounded(in);
	tsm_vte_set_image_cb(vte, hook_cb, &hook);
	ck_assert_str_eq(reply, "\033_Gi=1;EBADF:Failed to read image file\033\\");
}
END_TEST

/* tsm_vte_input never hands out a request: there the commands answer as
 * without a hook */
START_TEST(test_hook_unbounded)
{
	input("\033_Ga=t,t=f,f=100,i=1;L3RtcC94\033\\\033_Ga=t,f=100,i=2;iVBORw==\033\\"
	      "\033_Ga=f,t=s,i=3;L3NobQ==\033\\");
	ck_assert_uint_eq(hook.calls, 0);
	ck_assert_str_eq(reply, "\033_Gi=1;EBADF:Failed to read image file\033\\"
				"\033_Gi=2;EINVAL:invalid argument\033\\"
				"\033_Gi=3;EINVAL:invalid argument\033\\");
}
END_TEST

/*
 * A refused t=t or t=s command is handed over as a discard, and the client
 * gets libtsm's refusal whatever the host says; a refused t=f is not
 */
START_TEST(test_hook_discard)
{
	bounded("\033_Ga=f,t=s,s=1,v=1,i=1;L3NobQ==\033\\");
	ck_assert_uint_eq(hook.calls, 1);
	ck_assert(hook.req.discard);
	ck_assert_uint_eq(hook.req.medium, TSM_VTE_IMAGE_SHM);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 1, 0, 2, 2, PX2), 0);
	ck_assert_str_eq(reply, "\033_Gi=1;EINVAL:invalid argument\033\\");
	ck_assert_uint_eq(placements(), 0);

	/* the hook refusing it changes nothing either */
	reply_clear();
	hook.ret = -ENOSPC;
	bounded("\033_Ga=T,t=t,f=24,U=1,s=1,v=1,i=2;L3RtcC94\033\\");
	ck_assert(hook.req.discard);
	ck_assert_str_eq(reply, "\033_Gi=2;EINVAL:invalid argument\033\\");

	reply_clear();
	bounded("\033_Ga=f,t=f,i=3;L3RtcC94\033\\");
	ck_assert_uint_eq(hook.calls, 2);
	ck_assert_str_eq(reply, "\033_Gi=3;EINVAL:invalid argument\033\\");
}
END_TEST

/*
 * Inline PNG data reserves each growth of its buffer, refused ENOSPC; the
 * buffer is freed when the hook returns. The reply's image reserves its own
 * bytes, a=q none, and its size is checked
 */
START_TEST(test_hook_budget)
{
	uint8_t *big;
	char *cmd;

	tsm_screen_set_image_limit(screen, 8192);
	cmd = command("a=t,f=100,i=1", 9000, 1, 1, RED);
	bounded(cmd);
	free(cmd);
	ck_assert_uint_eq(hook.calls, 0);
	ck_assert_str_eq(reply, "\033_Gi=1;ENOSPC:image storage is full\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);

	reply_clear();
	cmd = command("a=T,f=100,i=2", 100, 1, 1, RED);
	bounded(cmd);
	free(cmd);
	ck_assert_uint_eq(hook.calls, 1);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 1, 0, 2, 2, PX2), 0);
	ck_assert_str_eq(reply, "\033_Gi=2;OK\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(2, 2));
	ck_assert_uint_eq(pixel(placement(0), 1, 0), 0xff00ff00u);
	ck_assert_uint_eq(pixel(placement(0), 1, 1), 0x80050505u);

	/* past the budget */
	reply_clear();
	bounded("\033_Ga=t,t=f,f=100,i=3;L3RtcC94\033\\");
	big = calloc(1, 64 * 64 * 4);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 2, 0, 64, 64, big), 0);
	free(big);
	ck_assert_str_eq(reply, "\033_Gi=3;ENOSPC:image storage is full\033\\");

	/* a=q stores nothing */
	reply_clear();
	bounded("\033_Ga=q,t=f,f=100,i=4;L3RtcC94\033\\");
	ck_assert_int_eq(tsm_vte_image_reply(vte, 3, 0, 2, 2, PX2), 0);
	ck_assert_str_eq(reply, "\033_Gi=4;OK\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(2, 2));

	/* past max-graphic-size, and unlike s and v */
	reply_clear();
	ck_assert_int_eq(tsm_vte_set_graphic_max_size(vte, 1, 1), 0);
	bounded("\033_Ga=t,t=f,f=100,i=5;L3RtcC94\033\\");
	ck_assert_int_eq(tsm_vte_image_reply(vte, 4, 0, 2, 2, PX2), 0);
	ck_assert_int_eq(tsm_vte_set_graphic_max_size(vte, 0, 0), 0);
	bounded("\033_Ga=t,t=f,f=32,s=1,v=1,i=6;L3RtcC94\033\\");
	ck_assert_int_eq(tsm_vte_image_reply(vte, 5, 0, 2, 2, PX2), 0);
	ck_assert_str_eq(reply, "\033_Gi=5;EINVAL:invalid argument\033\\"
				"\033_Gi=6;EINVAL:invalid argument\033\\");
}
END_TEST

TEST_DEFINE_CASE(kitty)
	CHECKED_FIXTURE(setup, teardown)
	TEST(test_kitty_off)
	TEST(test_kitty_pixels)
	TEST(test_kitty_split)
	TEST(test_kitty_c1)
	TEST(test_kitty_bounded)
	TEST(test_kitty_upload_bytes)
	TEST(test_kitty_host_budget)
	TEST(test_kitty_victim_stamp)
TEST_END_CASE

TEST_DEFINE_CASE(host)
	CHECKED_FIXTURE(hook_setup, teardown)
	TEST(test_hook_request)
	TEST(test_hook_pause)
	TEST(test_hook_errors)
	TEST(test_hook_reply_misuse)
	TEST(test_hook_cancel)
	TEST(test_hook_unbounded)
	TEST(test_hook_discard)
	TEST(test_hook_budget)
TEST_END_CASE

TEST_DEFINE(
	TEST_SUITE(vte_kitty,
		TEST_CASE(kitty),
		TEST_CASE(host),
		TEST_END
	)
)
