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
	int action;			/* what else the hook does, HOOK_* */
	size_t fed;			/* what input from inside the hook took */
} hook;

enum {
	HOOK_NOTHING,
	HOOK_RESET,			/* tsm_vte_reset from inside the hook */
	HOOK_KITTY_OFF,			/* kitty graphics off, then on */
	HOOK_REPLACE,			/* installs itself again */
	HOOK_INPUT,			/* feeds input */
};

/* answers request @id with @status and, for TSM_VTE_IMAGE_OK, @len bytes */
static int answer(uint64_t id, unsigned int status, unsigned int width,
		  unsigned int height, const uint8_t *data, size_t len)
{
	struct tsm_vte_image_result res = {
		.status = status,
		.width = width,
		.height = height,
		.data = data,
		.len = len,
	};

	return tsm_vte_image_reply(vte, id, &res);
}

/* a decoded PNG of @w x @h RGBA pixels */
static int answer_png(uint64_t id, unsigned int w, unsigned int h, const uint8_t *px)
{
	return answer(id, TSM_VTE_IMAGE_OK, w, h, px, (size_t)w * h * 4);
}

/* an outcome other than OK */
static int answer_status(uint64_t id, unsigned int status)
{
	return answer(id, status, 0, 0, NULL, 0);
}

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
		hook.inside = answer_png(req->id, 1, 1, px);
	switch (hook.action) {
	case HOOK_RESET:
		tsm_vte_reset(vte);
		break;
	case HOOK_KITTY_OFF:
		tsm_vte_set_kitty_graphics(vte, false);
		tsm_vte_set_kitty_graphics(vte, true);
		break;
	case HOOK_REPLACE:
		tsm_vte_set_image_cb(vte, hook_cb, &hook);
		break;
	case HOOK_INPUT:
		hook.fed = tsm_vte_input_bounded(vte, "x", 1);
		tsm_vte_input(vte, "y", 1);
		break;
	}
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

static const char *const EBADF_REPLY = "\033_Gi=1;EBADF:Failed to read image file\033\\";
static const char *const EINVAL_REPLY = "\033_Gi=1;EINVAL:invalid argument\033\\";

/* each medium reaches the hook, and inline raw data does not */
START_TEST(test_hook_request)
{
	static const uint8_t rgb6[6] = { 255, 0, 0, 0, 0, 255 };

	/* "/tmp/x" */
	bounded("\033_Ga=T,t=f,f=24,s=2,v=1,O=3,S=6,i=1;L3RtcC94\033\\");
	ck_assert_uint_eq(hook.calls, 1);
	ck_assert_uint_eq(hook.req.id, 1);
	ck_assert_uint_eq(hook.req.medium, TSM_VTE_IMAGE_FILE);
	ck_assert_str_eq(hook.name, "/tmp/x");
	ck_assert_ptr_eq(hook.req.data, NULL);
	ck_assert_int_eq(answer(1, TSM_VTE_IMAGE_OK, 0, 0, rgb6, 6), 0);
	ck_assert_str_eq(reply, "\033_Gi=1;OK\033\\");
	ck_assert_uint_eq(pixel(placement(0), 0, 0), 0xffff0000u);
	ck_assert_uint_eq(pixel(placement(0), 1, 0), 0xff0000ffu);

	/* inline PNG, then zlib: the payload, decoded */
	bounded("\033_Ga=t,f=100,i=2;iVBORw==\033\\");
	ck_assert_uint_eq(hook.calls, 2);
	ck_assert_uint_eq(hook.req.id, 2);
	ck_assert_uint_eq(hook.req.medium, TSM_VTE_IMAGE_DIRECT);
	ck_assert_ptr_eq(hook.req.name, NULL);
	ck_assert_uint_eq(hook.req.data_len, 4);
	ck_assert_mem_eq(hook.data, "\x89PNG", 4);
	ck_assert_int_eq(answer_status(2, TSM_VTE_IMAGE_CORRUPT), 0);
	bounded("\033_Ga=t,f=32,s=1,v=1,o=z,t=d,i=3;eJw=\033\\");
	ck_assert_uint_eq(hook.req.data_len, 2);
	ck_assert_int_eq(answer_status(3, TSM_VTE_IMAGE_CORRUPT), 0);

	/* t=t and t=s */
	bounded("\033_Ga=t,t=t,f=100,i=4;L3RtcC94\033\\");
	ck_assert_uint_eq(hook.req.medium, TSM_VTE_IMAGE_TEMP);
	ck_assert_int_eq(answer_status(4, TSM_VTE_IMAGE_UNREADABLE), 0);
	bounded("\033_Ga=t,t=s,f=100,i=5;L3NobQ==\033\\");
	ck_assert_uint_eq(hook.req.medium, TSM_VTE_IMAGE_SHM);
	ck_assert_str_eq(hook.name, "/shm");
	ck_assert_int_eq(answer_status(5, TSM_VTE_IMAGE_UNREADABLE), 0);

	/* inline raw data stays in libtsm */
	bounded("\033_Ga=T,f=24,s=1,v=1,i=6;/wAA\033\\");
	ck_assert_uint_eq(hook.calls, 5);
	ck_assert_uint_eq(placements(), 2);
}
END_TEST

/*
 * The request each row of ADR-103's What libtsm asks for produces: flags,
 * offset, read_max and min_bytes, from O, S, s, v, f and o
 */
START_TEST(test_hook_terms)
{
	static const struct {
		const char *keys;
		unsigned int medium, flags;
		uint64_t offset, read_max, min_bytes;
	} rows[] = {
		{ "a=t,t=f,f=24,s=2,v=3,O=5,S=9", 'f', 0, 5, 9, 18 },
		{ "a=t,t=t,f=32,s=2,v=3,o=z,O=1,S=100", 't', TSM_VTE_IMAGE_INFLATE, 1, 100, 24 },
		{ "a=t,t=s,f=100,O=2,S=50", 's', TSM_VTE_IMAGE_PNG, 2, 50, 0 },
		/* S bounds the read; no EXACT for a file */
		{ "a=t,t=f,f=100,o=z,S=40", 'f', TSM_VTE_IMAGE_PNG | TSM_VTE_IMAGE_INFLATE,
		  0, 40, 0 },
		/* the largest O and S the spec's 32-bit integers allow */
		{ "a=t,t=f,f=24,s=1,v=1,O=4294967295,S=4294967295", 'f', 0,
		  4294967295u, 4294967295u, 3 },
		{ "a=t,f=24,s=2,v=2,o=z,O=7,S=8", 'd', TSM_VTE_IMAGE_INFLATE, 0, 0, 12 },
		{ "a=t,f=100,S=99", 'd', TSM_VTE_IMAGE_PNG, 0, 0, 0 },
		{ "a=t,f=100,o=z,S=30", 'd',
		  TSM_VTE_IMAGE_PNG | TSM_VTE_IMAGE_INFLATE | TSM_VTE_IMAGE_EXACT, 0, 0, 30 },
		/* without S, or with S=0, no EXACT */
		{ "a=t,f=100,o=z", 'd', TSM_VTE_IMAGE_PNG | TSM_VTE_IMAGE_INFLATE, 0, 0, 0 },
		{ "a=t,f=100,o=z,S=0", 'd', TSM_VTE_IMAGE_PNG | TSM_VTE_IMAGE_INFLATE, 0, 0, 0 },
		/* refused: a discard, the bounds all 0 */
		{ "a=t,t=t,f=24,O=4,S=9", 't', TSM_VTE_IMAGE_DISCARD, 0, 0, 0 },
		{ "a=f,t=s,f=100,o=z,S=9", 's', TSM_VTE_IMAGE_DISCARD, 0, 0, 0 },
	};
	char cmd[128];
	size_t i;

	for (i = 0; i < sizeof(rows) / sizeof(*rows); ++i) {
		/* "/tmp/x", or inline "x" + 0x9c */
		snprintf(cmd, sizeof(cmd), "\033_G%s;%s\033\\", rows[i].keys,
			 rows[i].medium == 'd' ? "eJw=" : "L3RtcC94");
		bounded(cmd);
		ck_assert_msg(hook.calls == i + 1, "row %zu reached no hook", i);
		ck_assert_uint_eq(hook.req.medium, rows[i].medium);
		ck_assert_msg(hook.req.flags == rows[i].flags, "row %zu: flags %#x", i,
			      hook.req.flags);
		ck_assert_uint_eq(hook.req.offset, rows[i].offset);
		ck_assert_uint_eq(hook.req.read_max, rows[i].read_max);
		ck_assert_uint_eq(hook.req.min_bytes, rows[i].min_bytes);
		ck_assert_int_eq(answer_status(hook.req.id, TSM_VTE_IMAGE_UNREADABLE), 0);
	}

	/* refused before the hook: O or S past 32 bits, s=0, s past
	 * max-graphic-size, a refused t=f, a refused inline PNG */
	reply_clear();
	bounded("\033_Ga=t,t=f,f=24,s=1,v=1,O=4294967296,i=1;L3RtcC94\033\\"
		"\033_Ga=t,t=f,f=24,s=1,v=1,S=4294967296,i=1;L3RtcC94\033\\"
		"\033_Ga=t,t=f,f=24,s=0,v=1,i=1;L3RtcC94\033\\"
		"\033_Ga=t,t=f,f=24,s=10001,v=1,i=1;L3RtcC94\033\\"
		"\033_Ga=t,t=f,f=24,i=1;L3RtcC94\033\\"
		"\033_Ga=t,f=100,i=1,I=2;iVBORw==\033\\");
	ck_assert_uint_eq(hook.calls, sizeof(rows) / sizeof(*rows));
	ck_assert_str_eq(reply, "\033_Gi=1;EINVAL:invalid argument\033\\"
				"\033_Gi=1;EINVAL:invalid argument\033\\"
				"\033_Gi=1;EINVAL:invalid argument\033\\"
				"\033_Gi=1;EINVAL:invalid argument\033\\"
				"\033_Gi=1;EINVAL:invalid argument\033\\"
				"\033_Gi=1,I=2;EINVAL:invalid argument\033\\");
}
END_TEST

/*
 * The reply each outcome produces (ADR-103's What libtsm answers): a file
 * read without inflating answers too few bytes EBADF, one inflated or sent
 * inline EINVAL
 */
START_TEST(test_hook_outcomes)
{
	static const char *const cmds[] = {
		"\033_Ga=t,t=f,f=24,s=1,v=1,i=1;L3RtcC94\033\\",
		"\033_Ga=t,t=f,f=100,i=1;L3RtcC94\033\\",
		"\033_Ga=t,t=s,f=24,s=1,v=1,o=z,i=1;L3NobQ==\033\\",
		"\033_Ga=t,f=100,i=1;iVBORw==\033\\",
	};
	/* per command, the reply to UNREADABLE, TOO_SHORT, CORRUPT,
	 * TOO_LARGE, OVER_BUDGET and FAILED */
	static const char *const codes[][6] = {
		{ "EBADF", "EBADF", "EINVAL", "EINVAL", "ENOSPC", "EBADF" },
		{ "EBADF", "EBADF", "EINVAL", "EINVAL", "ENOSPC", "EBADF" },
		{ "EBADF", "EINVAL", "EINVAL", "EINVAL", "ENOSPC", "EBADF" },
		{ "EBADF", "EINVAL", "EINVAL", "EINVAL", "ENOSPC", "EBADF" },
	};
	unsigned int c, s;

	for (c = 0; c < 4; ++c) {
		for (s = TSM_VTE_IMAGE_UNREADABLE; s <= TSM_VTE_IMAGE_FAILED; ++s) {
			reply_clear();
			bounded(cmds[c]);
			ck_assert_int_eq(answer_status(hook.req.id, s), 0);
			ck_assert_msg(!strncmp(reply, "\033_Gi=1;", 7) &&
				      !strncmp(reply + 7, codes[c][s - 1],
					       strlen(codes[c][s - 1])) &&
				      reply[7 + strlen(codes[c][s - 1])] == ':',
				      "command %u, status %u answered %s", c, s, reply + 1);
		}
	}
	ck_assert_uint_eq(placements(), 0);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
}
END_TEST

/* the min_bytes bytes of a raw format convert as inline data does */
START_TEST(test_hook_raw)
{
	static const uint8_t rgb3[3] = { 0, 0, 255 };
	static const uint8_t rgba8[8] = { 200, 100, 50, 128, 0, 255, 0, 255 };

	bounded("\033_Ga=T,t=f,f=24,s=1,v=1,S=47,C=1,i=1;L3RtcC94\033\\");
	ck_assert_uint_eq(hook.req.min_bytes, 3);
	ck_assert_int_eq(answer(1, TSM_VTE_IMAGE_OK, 0, 0, rgb3, 3), 0);
	bounded("\033_Ga=T,t=s,f=32,s=2,v=1,o=z,C=1,i=2;L3NobQ==\033\\");
	ck_assert_uint_eq(hook.req.min_bytes, 8);
	ck_assert_int_eq(answer(2, TSM_VTE_IMAGE_OK, 0, 0, rgba8, 8), 0);
	ck_assert_str_eq(reply, "\033_Gi=1;OK\033\\\033_Gi=2;OK\033\\");
	ck_assert_uint_eq(placements(), 2);
	ck_assert_uint_eq(pixel(placement(0), 0, 0), 0xff0000ffu);
	ck_assert(placement(0)->flags & TSM_FRAME_OPAQUE);
	ck_assert_uint_eq(pixel(placement(1), 0, 0),
			  0x80000000u | 100u << 16 | 50u << 8 | 25u);
	ck_assert_uint_eq(pixel(placement(1), 1, 0), 0xff00ff00u);
	ck_assert(!(placement(1)->flags & TSM_FRAME_OPAQUE));
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
	ck_assert_int_eq(answer_png(1, 2, 2, PX2), 0);
	ck_assert_uint_eq(bounded(in + n), strlen(in + n));
	ck_assert_str_eq(reply, "\033_Gi=1;OK\033\\\033_Gi=1;OK\033\\\033[1;3R");
	ck_assert_uint_eq(placements(), 1);
	ck_assert_uint_eq(tsm_screen_get_cursor_x(screen), 2);

	/* a C1 ST ends the command at its own byte */
	reply_clear();
	ck_assert_uint_eq(bounded("\xc2\x9fGa=t,t=f,f=100,i=2;L3RtcC94\xc2\x9c" "Z"),
			  strlen("\xc2\x9fGa=t,t=f,f=100,i=2;L3RtcC94\xc2\x9c"));
	ck_assert_int_eq(answer_status(2, TSM_VTE_IMAGE_UNREADABLE), 0);
	ck_assert_str_eq(reply, "\033_Gi=2;EBADF:Failed to read image file\033\\");
}
END_TEST

/*
 * Any negative return from the hook answers at once as
 * TSM_VTE_IMAGE_FAILED, EBADF, and parsing goes on in the same call
 */
START_TEST(test_hook_errors)
{
	static const int rets[] = { -EBADF, -EINVAL, -ENOSPC, -EIO, -1 };
	static const char in[] = "\033_Ga=t,t=f,f=100,i=1;L3RtcC94\033\\\033[6n";
	char want[128];
	unsigned int i;

	snprintf(want, sizeof(want), "%s\033[1;1R", EBADF_REPLY);
	for (i = 0; i < sizeof(rets) / sizeof(*rets); ++i) {
		reply_clear();
		hook.ret = rets[i];
		ck_assert_uint_eq(bounded(in), strlen(in));
		ck_assert_str_eq(reply, want);
		/* the request is not out */
		ck_assert_int_eq(answer_status(hook.req.id, TSM_VTE_IMAGE_UNREADABLE),
				 -ENOENT);
	}
}
END_TEST

/*
 * A stale id, a reply with nothing out, and one from inside the hook change
 * nothing; a result that disagrees with its request ends it with EBADF and
 * returns -EINVAL
 */
START_TEST(test_hook_reply_misuse)
{
	static const char png[] = "\033_Ga=t,t=f,f=100,i=1;L3RtcC94\033\\";
	static const char raw[] = "\033_Ga=t,t=f,f=24,s=1,v=1,i=1;L3RtcC94\033\\";
	static const uint8_t rgb4[4] = { 1, 2, 3, 4 };
	uint64_t id = 0;

	ck_assert_int_eq(answer_png(1, 2, 2, PX2), -ENOENT);
	bounded(png);
	++id;
	ck_assert_int_eq(answer_png(7, 2, 2, PX2), -ENOENT);
	ck_assert_uint_eq(reply_len, 0);

	/* an unknown status */
	ck_assert_int_eq(answer_status(id, TSM_VTE_IMAGE_FAILED + 1), -EINVAL);
	ck_assert_str_eq(reply, EBADF_REPLY);
	ck_assert_uint_gt(bounded("x"), 0);

	/* no result at all */
	reply_clear();
	bounded(png);
	ck_assert_int_eq(tsm_vte_image_reply(vte, ++id, NULL), -EINVAL);
	ck_assert_str_eq(reply, EBADF_REPLY);

	/* OK with no bytes, a zero PNG size, a PNG length one byte off */
	reply_clear();
	bounded(png);
	ck_assert_int_eq(answer(++id, TSM_VTE_IMAGE_OK, 2, 2, NULL, 16), -EINVAL);
	bounded(png);
	ck_assert_int_eq(answer(++id, TSM_VTE_IMAGE_OK, 0, 2, PX2, 0), -EINVAL);
	bounded(png);
	ck_assert_int_eq(answer(++id, TSM_VTE_IMAGE_OK, 2, 2, PX2, 15), -EINVAL);
	/* a PNG size whose length would wrap to 0 in 64 bits */
	bounded(png);
	ck_assert_int_eq(answer(++id, TSM_VTE_IMAGE_OK, 0x80000000u, 0x80000000u,
				PX2, 0), -EINVAL);
	/* a raw format's bytes other than min_bytes, either way */
	bounded(raw);
	ck_assert_int_eq(answer(++id, TSM_VTE_IMAGE_OK, 0, 0, rgb4, 2), -EINVAL);
	bounded(raw);
	ck_assert_int_eq(answer(++id, TSM_VTE_IMAGE_OK, 0, 0, rgb4, 4), -EINVAL);
	ck_assert_str_eq(reply, "\033_Gi=1;EBADF:Failed to read image file\033\\"
				"\033_Gi=1;EBADF:Failed to read image file\033\\"
				"\033_Gi=1;EBADF:Failed to read image file\033\\"
				"\033_Gi=1;EBADF:Failed to read image file\033\\"
				"\033_Gi=1;EBADF:Failed to read image file\033\\"
				"\033_Gi=1;EBADF:Failed to read image file\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);

	/* -EBUSY from inside the hook leaves the request out */
	reply_clear();
	hook.reply_inside = true;
	bounded(png);
	++id;
	ck_assert_int_eq(hook.inside, -EBUSY);
	ck_assert_uint_eq(bounded("x"), 0);
	ck_assert_int_eq(answer_png(id, 2, 2, PX2), 0);
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
		ck_assert_int_eq(answer_png(hook.req.id, 2, 2, PX2), -ENOENT);
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
	ck_assert_str_eq(reply, EBADF_REPLY);
	ck_assert_uint_eq(bounded("x"), 1);
	tsm_vte_set_image_cb(vte, hook_cb, &hook);
	reply_clear();
	bounded(in);
	tsm_vte_set_image_cb(vte, hook_cb, &hook);
	ck_assert_str_eq(reply, EBADF_REPLY);
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
 * gets libtsm's refusal, once, whatever the host does: any result, a
 * negative hook return, the hook changed while it is out; a refused t=f is
 * not handed over
 */
START_TEST(test_hook_discard)
{
	static const char shm[] = "\033_Ga=f,t=s,s=2,v=2,i=1;L3NobQ==\033\\";
	static const char tmp[] = "\033_Ga=T,t=t,f=24,U=1,s=1,v=1,i=1;L3RtcC94\033\\";
	struct tsm_vte_image_result odd = { .status = 99 };

	/* the host answers OK with an image */
	bounded(shm);
	ck_assert_uint_eq(hook.calls, 1);
	ck_assert_uint_eq(hook.req.flags, TSM_VTE_IMAGE_DISCARD);
	ck_assert_uint_eq(hook.req.medium, TSM_VTE_IMAGE_SHM);
	ck_assert_int_eq(answer_png(1, 2, 2, PX2), 0);
	ck_assert_str_eq(reply, EINVAL_REPLY);
	ck_assert_uint_eq(tsm_screen_test_image_count(screen), 0);

	/* an unknown status, and no result, are accepted and ignored */
	reply_clear();
	bounded(tmp);
	ck_assert_uint_eq(hook.req.medium, TSM_VTE_IMAGE_TEMP);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 2, &odd), 0);
	bounded(shm);
	ck_assert_int_eq(tsm_vte_image_reply(vte, 3, NULL), 0);
	ck_assert_str_eq(reply, "\033_Gi=1;EINVAL:invalid argument\033\\"
				"\033_Gi=1;EINVAL:invalid argument\033\\");

	/* a negative return: the refusal alone, once */
	reply_clear();
	hook.ret = -EACCES;
	bounded(tmp);
	ck_assert_uint_eq(hook.req.flags, TSM_VTE_IMAGE_DISCARD);
	ck_assert_str_eq(reply, EINVAL_REPLY);
	ck_assert_int_eq(answer_status(hook.req.id, TSM_VTE_IMAGE_FAILED), -ENOENT);
	ck_assert_str_eq(reply, EINVAL_REPLY);
	hook.ret = 0;

	/* the hook removed, or installed, while a discard is out */
	reply_clear();
	bounded(shm);
	tsm_vte_set_image_cb(vte, NULL, NULL);
	tsm_vte_set_image_cb(vte, hook_cb, &hook);
	bounded(tmp);
	tsm_vte_set_image_cb(vte, hook_cb, &hook);
	ck_assert_str_eq(reply, "\033_Gi=1;EINVAL:invalid argument\033\\"
				"\033_Gi=1;EINVAL:invalid argument\033\\");

	/* and from inside the hook */
	reply_clear();
	hook.action = HOOK_REPLACE;
	bounded(shm);
	ck_assert_str_eq(reply, EINVAL_REPLY);
	hook.action = HOOK_NOTHING;

	reply_clear();
	ck_assert_uint_eq(hook.calls, 7);
	bounded("\033_Ga=f,t=f,i=3;L3RtcC94\033\\");
	ck_assert_uint_eq(hook.calls, 7);
	ck_assert_str_eq(reply, "\033_Gi=3;EINVAL:invalid argument\033\\");
}
END_TEST

/*
 * An a=q without i, with I or with neither, never reaches the hook, not even
 * as a discard, and buffers no inline data
 */
START_TEST(test_hook_query_number)
{
	/* neither i nor I, on each medium, refused or not, and chunked */
	bounded("\033_Ga=q,t=t,f=24,s=1,v=1;L3RtcC94\033\\"
		"\033_Ga=q,t=s,f=24,s=1,v=1,q=1;L3NobQ==\033\\"
		"\033_Ga=q,t=f,f=100,q=2;L3RtcC94\033\\"
		"\033_Ga=q,t=s,f=24;L3NobQ==\033\\"
		"\033_Ga=q,f=100,m=1;iVBORw==\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	bounded("\033_Gm=0;iVBORw==\033\\\033_Ga=q,f=24,s=1,v=1,o=z;eJw=\033\\");
	ck_assert_uint_eq(hook.calls, 0);
	ck_assert_uint_eq(reply_len, 0);

	bounded("\033_Ga=q,t=t,f=24,s=1,v=1,I=52;L3RtcC94\033\\"
		"\033_Ga=q,t=s,f=24,s=1,v=1,I=52,q=1;L3NobQ==\033\\"
		"\033_Ga=q,t=f,f=100,I=52,q=2;L3RtcC94\033\\"
		/* refused with i: would be a discard */
		"\033_Ga=q,t=t,f=24,I=52;L3RtcC94\033\\"
		"\033_Ga=q,f=100,I=52,m=1;iVBORw==\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	bounded("\033_Gm=0;iVBORw==\033\\\033_Ga=q,f=24,s=1,v=1,o=z,I=52;eJw=\033\\");
	ck_assert_uint_eq(hook.calls, 0);
	ck_assert_uint_eq(reply_len, 0);

	/* with i it does */
	bounded("\033_Ga=q,t=t,f=24,s=1,v=1,i=51;L3RtcC94\033\\");
	ck_assert_uint_eq(hook.calls, 1);
	ck_assert_int_eq(answer_status(hook.req.id, TSM_VTE_IMAGE_UNREADABLE), 0);
	ck_assert_str_eq(reply, "\033_Gi=51;EBADF:Failed to read image file\033\\");
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
	ck_assert_int_eq(answer_png(1, 2, 2, PX2), 0);
	ck_assert_str_eq(reply, "\033_Gi=2;OK\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(2, 2));
	ck_assert_uint_eq(pixel(placement(0), 1, 0), 0xff00ff00u);
	ck_assert_uint_eq(pixel(placement(0), 1, 1), 0x80050505u);

	/* past the budget */
	reply_clear();
	bounded("\033_Ga=t,t=f,f=100,i=3;L3RtcC94\033\\");
	big = calloc(1, 64 * 64 * 4);
	ck_assert_int_eq(answer_png(2, 64, 64, big), 0);
	free(big);
	ck_assert_str_eq(reply, "\033_Gi=3;ENOSPC:image storage is full\033\\");

	/* a=q stores nothing */
	reply_clear();
	bounded("\033_Ga=q,t=f,f=100,i=4;L3RtcC94\033\\");
	ck_assert_int_eq(answer_png(3, 2, 2, PX2), 0);
	ck_assert_str_eq(reply, "\033_Gi=4;OK\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(2, 2));

	/* a PNG past max-graphic-size, either way */
	reply_clear();
	ck_assert_int_eq(tsm_vte_set_graphic_max_size(vte, 1, 2), 0);
	bounded("\033_Ga=t,t=f,f=100,i=5;L3RtcC94\033\\");
	ck_assert_int_eq(answer_png(4, 2, 2, PX2), 0);
	bounded("\033_Ga=t,t=f,f=100,i=6;L3RtcC94\033\\");
	ck_assert_int_eq(answer(5, TSM_VTE_IMAGE_OK, 1, 4, PX2, 16), 0);
	ck_assert_str_eq(reply, "\033_Gi=5;EINVAL:invalid argument\033\\"
				"\033_Gi=6;EINVAL:invalid argument\033\\");

	/* a raw image whose max-graphic-size was lowered while it was out */
	reply_clear();
	bounded("\033_Ga=t,t=f,f=24,s=1,v=2,i=7;L3RtcC94\033\\");
	ck_assert_int_eq(tsm_vte_set_graphic_max_size(vte, 1, 1), 0);
	ck_assert_int_eq(answer(6, TSM_VTE_IMAGE_OK, 0, 0, PX2, 6), 0);
	ck_assert_str_eq(reply, "\033_Gi=7;EINVAL:invalid argument\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(2, 2));
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

/*
 * The bounded call with @n copies of @cmd consumes part of them: what each
 * costs is counted as work
 */
static void assert_bounded(const char *cmd, size_t n)
{
	size_t cl = strlen(cmd), len = n * cl, used, i;
	char *buf = malloc(len);

	ck_assert_ptr_ne(buf, NULL);
	for (i = 0; i < n; ++i)
		memcpy(buf + i * cl, cmd, cl);
	used = tsm_vte_input_bounded(vte, buf, len);
	ck_assert_uint_gt(used, 0);
	ck_assert_uint_lt(used, len);
	free(buf);
}

/*
 * A transmission with i looks for the image it replaces, and the scan of a
 * full store counts as work, as a find's does: a flood of short
 * transmissions stops the bounded call early
 */
START_TEST(test_kitty_reserve_bound)
{
	struct tsm_test_image img = { .width = 1, .height = 1, .kind = TSM_IMAGE_KITTY };
	uint32_t px = 0xff000000;
	uint64_t key;
	unsigned int i;

	img.pixels = &px;
	for (i = 1; i < TSM_SCREEN_IMAGES_PER_STORE; ++i) {
		img.id = i;
		ck_assert_int_eq(tsm_screen_test_image(screen, &img, &key), 0);
	}
	assert_bounded("\033_Ga=t,f=24,s=1,v=1,i=5000,q=2;\033\\", 30000);
}
END_TEST

/*
 * Replacing an image frees the old one, whose scan for its placements
 * covers every placement in the store and counts as work: with a full
 * store of another image's placements, a flood of replacements stops the
 * bounded call early
 */
START_TEST(test_kitty_replace_bound)
{
	struct tsm_test_image img = { .width = 1, .height = 1, .kind = TSM_IMAGE_KITTY,
				      .id = 1 };
	struct tsm_test_placement pl = { 0 };
	uint32_t px = 0xff000000;
	uint64_t key;
	unsigned int i;

	img.pixels = &px;
	ck_assert_int_eq(tsm_screen_test_image(screen, &img, &pl.image), 0);
	for (i = 1; i < TSM_SCREEN_PLACEMENTS_PER_STORE; ++i)
		ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &key), 0);
	assert_bounded("\033_Ga=t,f=24,s=1,v=1,i=2,q=2;/wAA\033\\", 30000);
}
END_TEST

/*
 * p and q choose a cell on the screen's own lines: without y they choose
 * nothing, so a placement wholly in scrollback stays, where the missing y
 * would have been row -1, its line. With x and y they delete as before.
 */
START_TEST(test_kitty_delete_scrollback)
{
	static const char *const dels[] = {
		"\033_Ga=d,d=p,x=1\033\\",
		"\033_Ga=d,d=P,x=1\033\\",
		"\033_Ga=d,d=q,x=1\033\\",
		"\033_Ga=d,d=Q,x=1\033\\",
	};
	char *cmd = command("a=T,f=24,s=1,v=1,i=1,C=1", 1, 1, 3, RED);
	unsigned int i;

	tsm_screen_set_max_sb(screen, 100);
	input("\033[5;1H");
	input(cmd);
	input("\r\n\n\n\n\n");
	ck_assert_uint_eq(placements(), 0);
	for (i = 0; i < sizeof(dels) / sizeof(*dels); ++i) {
		input(dels[i]);
		ck_assert_msg(tsm_screen_test_placement_count(screen) == 1,
			      "delete %u removed the scrollback placement", i);
		ck_assert_uint_eq(tsm_screen_test_image_count(screen), 1);
	}

	/* the control: on the screen's own lines, with x and y */
	input("\033[1;1H");
	input(cmd);
	free(cmd);
	ck_assert_uint_eq(placements(), 1);
	input("\033_Ga=d,d=Q,x=1,y=1\033\\");
	ck_assert_uint_eq(tsm_screen_test_placement_count(screen), 0);
	ck_assert_uint_eq(tsm_screen_test_image_count(screen), 0);
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

/*
 * From inside the hook: a cancel drops the request with no reply, the
 * inline buffer going with it; a hook change answers EBADF; input is refused
 */
START_TEST(test_hook_inside)
{
	static const char png[] = "\033_Ga=t,f=100,i=1;iVBORw==\033\\";
	static const char file[] = "\033_Ga=t,t=f,f=100,i=2;L3RtcC94\033\\";
	unsigned int action, x;

	for (action = HOOK_RESET; action <= HOOK_KITTY_OFF; ++action) {
		reply_clear();
		hook.action = action;
		ck_assert_uint_eq(bounded(png), strlen(png));
		ck_assert_uint_eq(reply_len, 0);
		ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
		ck_assert_int_eq(answer_png(hook.req.id, 2, 2, PX2), -ENOENT);
		ck_assert_uint_eq(bounded("x"), 1);
	}

	reply_clear();
	hook.action = HOOK_REPLACE;
	bounded(file);
	ck_assert_str_eq(reply, "\033_Gi=2;EBADF:Failed to read image file\033\\");
	ck_assert_uint_eq(bounded("x"), 1);

	reply_clear();
	hook.action = HOOK_INPUT;
	x = tsm_screen_get_cursor_x(screen);
	bounded(file);
	ck_assert_uint_eq(hook.fed, 0);
	ck_assert_int_eq(answer_status(hook.req.id, TSM_VTE_IMAGE_UNREADABLE), 0);
	ck_assert_str_eq(reply, "\033_Gi=2;EBADF:Failed to read image file\033\\");
	/* neither "x" nor "y" reached the screen */
	ck_assert_uint_eq(tsm_screen_get_cursor_x(screen), x);
}
END_TEST

/*
 * tsm_vte_input buffers no inline PNG for the hook it cannot use, nor goes on
 * buffering one begun in tsm_vte_input_bounded: the command answers EINVAL,
 * as without a hook, its bytes given back
 */
START_TEST(test_hook_unbounded_buffer)
{
	input("\033_Ga=t,f=100,i=1,m=1;iVBORw==\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	input("\033_Gm=0;iVBORw==\033\\");
	ck_assert_uint_eq(hook.calls, 0);
	ck_assert_str_eq(reply, EINVAL_REPLY);

	/* the APC begun in a bounded call, its payload through tsm_vte_input */
	reply_clear();
	bounded("\033_Ga=t,f=100,i=2");
	input(";iVBORw==");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	input("\033\\");
	ck_assert_uint_eq(hook.calls, 0);
	ck_assert_str_eq(reply, "\033_Gi=2;EINVAL:invalid argument\033\\");

	/* a chunked one, its first chunk buffered in a bounded call */
	reply_clear();
	bounded("\033_Ga=t,f=100,i=3,m=1;iVBORw==\033\\");
	ck_assert_uint_gt(tsm_screen_get_image_bytes(screen), 0);
	input("\033_Gm=1;iVBORw==\033\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	bounded("\033_Gm=0;iVBORw==\033\\");
	ck_assert_uint_eq(hook.calls, 0);
	ck_assert_str_eq(reply, "\033_Gi=3;EINVAL:invalid argument\033\\");
}
END_TEST

/*
 * An a=q without i, with I or with neither, is dropped at its ST under every
 * q and on every medium, as kitty 0.49.2 drops it (ADR-107): no reply,
 * nothing stored, its data not even counted (no excess logged), its chunks
 * swallowed; with i it answers
 */
START_TEST(test_kitty_query_number)
{
	static const char *const cmds[] = {
		"\033_Ga=q,f=24,s=1,v=1,I=52;/wAA\033\\",
		"\033_Ga=q,f=24,s=1,v=1,I=52,q=1;/wAA\033\\",
		"\033_Ga=q,f=24,s=1,v=1,I=52,q=2;/wAA\033\\",
		/* too short, an error with i */
		"\033_Ga=q,f=24,s=2,v=1,I=52;/wAA\033\\",
		/* a key it cannot read, an error with i */
		"\033_Ga=q,f=24,s=x,v=1,I=52;/wAA\033\\",
		"\033_Ga=q,t=t,f=24,s=1,v=1,I=52;L3RtcC90dHktZ3JhcGhpY3MtcHJvdG9jb2wtMQ==\033\\",
		"\033_Ga=q,t=t,f=24,s=1,v=1,I=52,q=1;L3RtcC90dHktZ3JhcGhpY3MtcHJvdG9jb2wtMQ==\033\\",
		"\033_Ga=q,t=s,f=24,s=1,v=1,I=52,q=2;L2tpdHR5LXNobQ==\033\\",
		/* chunked: the continuation is swallowed, not read as a=t */
		"\033_Ga=q,f=24,s=1,v=1,I=52,m=1;/w==\033\\\033_Gm=0;AAA=\033\\",
		"\033_Ga=q,f=24,s=2,v=1,I=52,q=0;/wAA\033\\",
		"\033_Ga=q,f=24,s=2,v=1,I=52,q=1;/wAA\033\\",
		"\033_Ga=q,f=24,s=2,v=1,I=52,q=2;/wAA\033\\",
		/* neither i nor I, under every q, loading and too short */
		"\033_Ga=q,f=24,s=1,v=1;/wAA\033\\",
		"\033_Ga=q,f=24,s=1,v=1,q=0;/wAA\033\\",
		"\033_Ga=q,f=24,s=1,v=1,q=1;/wAA\033\\",
		"\033_Ga=q,f=24,s=1,v=1,q=2;/wAA\033\\",
		"\033_Ga=q,f=24,s=2,v=1;/wAA\033\\",
		"\033_Ga=q,f=24,s=2,v=1,q=0;/wAA\033\\",
		"\033_Ga=q,f=24,s=2,v=1,q=1;/wAA\033\\",
		"\033_Ga=q,f=24,s=2,v=1,q=2;/wAA\033\\",
		"\033_Ga=q,t=t,f=24,s=1,v=1;L3RtcC90dHktZ3JhcGhpY3MtcHJvdG9jb2wtMQ==\033\\",
		"\033_Ga=q,t=s,f=24,s=1,v=1,q=2;L2tpdHR5LXNobQ==\033\\",
		"\033_Ga=q,f=24,s=1,v=1,m=1;/w==\033\\\033_Gm=0;AAA=\033\\",
		/* data past s x v: a dropped query counts none of it */
		"\033_Ga=q,f=24,s=1,v=1;/wAA/wAA\033\\",
		"\033_Ga=q,f=24,s=1,v=1,I=52;/wAA/wAA\033\\",
	};
	size_t i;

	debug_log[0] = 0;
	for (i = 0; i < sizeof(cmds) / sizeof(*cmds); ++i) {
		input(cmds[i]);
		ck_assert_msg(reply_len == 0, "command %zu answered %s", i, reply + 1);
	}
	ck_assert_str_eq(debug_log, "");
	ck_assert_uint_eq(placements(), 0);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);

	/* nothing was stored under number 52 */
	input("\033_Ga=p,I=52\033\\");
	ck_assert_str_eq(reply, "\033_GI=52;ENOENT:image not found\033\\");

	reply_clear();
	input("\033_Ga=q,f=24,s=1,v=1,i=51;/wAA\033\\");
	ck_assert_str_eq(reply, "\033_Gi=51;OK\033\\");
}
END_TEST

TEST_DEFINE_CASE(kitty)
	CHECKED_FIXTURE(setup, teardown)
	TEST(test_kitty_query_number)
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
	TEST(test_kitty_delete_scrollback)
	TEST(test_kitty_reserve_bound)
	TEST(test_kitty_replace_bound)
	TEST(test_kitty_delete_number_placement)
	TEST(test_kitty_keys_limit)
	TEST(test_kitty_chunk_quiet)
	TEST(test_kitty_size_bound)
TEST_END_CASE

TEST_DEFINE_CASE(host)
	CHECKED_FIXTURE(hook_setup, teardown)
	TEST(test_hook_request)
	TEST(test_hook_terms)
	TEST(test_hook_outcomes)
	TEST(test_hook_raw)
	TEST(test_hook_pause)
	TEST(test_hook_errors)
	TEST(test_hook_reply_misuse)
	TEST(test_hook_cancel)
	TEST(test_hook_unbounded)
	TEST(test_hook_discard)
	TEST(test_hook_query_number)
	TEST(test_hook_budget)
	TEST(test_hook_inside)
	TEST(test_hook_unbounded_buffer)
TEST_END_CASE

TEST_DEFINE(
	TEST_SUITE(vte_kitty,
		TEST_CASE(kitty),
		TEST_CASE(host),
		TEST_END
	)
)
