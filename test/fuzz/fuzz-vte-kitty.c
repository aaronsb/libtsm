/*
 * fuzz-vte-kitty - fuzz target for the kitty graphics protocol through the vte
 *
 * Kitty graphics commands reach the APC parser (tsm-kitty.c) through the
 * vte, and their images the screen's store and placements, all in kmscon's
 * root process, on bytes any program sends (ADR-103, Testing). This target
 * feeds whole terminal streams to a vte with kitty graphics and sixel on, so
 * the key parser, streamed base64, chunked uploads, replies, placement,
 * cursor movement, deletes, eviction and every screen event run together.
 * Crashes, memory errors and undefined behaviour abort, and so do broken
 * invariants:
 *
 *  - the host budget is never given back more than it granted, and holds
 *    exactly the bytes the screen reports after every input call (a pending
 *    upload's reservation included);
 *  - tsm_vte_input_bounded consumes at least one byte and at most what it
 *    was given;
 *  - every placement a frame lists names an image the screen still has, and
 *    its source rectangle lies inside that image;
 *  - every reply is a whole kitty reply, a DA, DSR or other reply libtsm
 *    writes, never a fragment;
 *  - a request the host hook takes pauses the bounded call, which then
 *    consumes nothing until the reply, and only the request out is answered;
 *  - freeing the vte and the screen gives every byte back.
 *
 * The host hook takes every request, checking its terms against ADR-103's
 * What libtsm asks for; after each bounded call the target answers the one
 * out, in turn: the bytes it asks for (a PNG's image, or min_bytes of a raw
 * format), each outcome other than OK, a result that disagrees with the
 * request (a length one byte off, an unknown status), and OK with no bytes;
 * or the hook refuses the request itself, or resets the vte from inside. A
 * discard takes any of these and answers 0. Between a request and its reply the target may
 * cancel it (tsm_vte_reset, tsm_vte_hard_reset, kitty graphics off, a hook
 * change), resize the screen before answering, or free the vte with the
 * request still out.
 *
 * Input: byte 0 picks the screen size, cell size, scrollback and budget,
 * byte 1 the max-graphic-size, how the stream is fed (whole, or in chunks
 * through tsm_vte_input_bounded) and where the answers start; the rest is
 * the stream.
 *
 * Built two ways, as fuzz-vte-sixel is: with libFuzzer (meson -Dfuzz=true
 * under clang, make fuzz FUZZ_TARGET=vte-kitty), and with -DFUZZ_REPLAY as a
 * program that replays the files and directories named on its command line,
 * which the test suite uses for the seed corpus, vte-kitty-seeds/.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"

#define check(x) do { if (!(x)) { \
	fprintf(stderr, "%s:%d: invariant failed: %s\n", __FILE__, \
		__LINE__, #x); abort(); } } while (0)

/* The host's budget: small enough to be reached often, and to keep runs
 * fast; byte 0 picks a smaller one, which a single picture and its pieces
 * reach. */
static const size_t budgets[4] = { (size_t)1 << 20, 1188, 4096, 65536 };
static size_t budget;
static size_t held;

static int reserve(struct tsm_screen *con, size_t bytes, void *data)
{
	(void)data;
	/* a host evicts across its screens; this one has one */
	while (held + bytes > budget)
		if (!tsm_screen_image_evict(con))
			return -ENOSPC;
	held += bytes;
	return 0;
}

static void release(struct tsm_screen *con, size_t bytes, void *data)
{
	(void)con;
	(void)data;
	check(bytes <= held);
	held -= bytes;
}

/* a kitty reply is written whole: ESC _ G ... ESC \\ in one call */
static void write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	(void)vte;
	(void)data;
	check(len > 0);
	if (len >= 3 && !memcmp(u8, "\033_G", 3))
		check(len >= 5 && !memcmp(u8 + len - 2, "\033\\", 2) &&
		      memchr(u8, ';', len));
}

static const struct { unsigned int cols, rows; } sizes[4] = {
	{ 12, 4 }, { 1, 1 }, { 40, 12 }, { 3, 30 },
};

static const struct { unsigned int w, h; } cells[4] = {
	{ 8, 16 }, { 1, 1 }, { 6, 13 }, { 1024, 1 },
};
static const struct { unsigned int w, h; } graphic[4] = {
	{ 0, 0 }, { 64, 64 }, { 1, 1 }, { 600, 7 },
};
static const size_t chunks[4] = { 1, 7, 4096, 0 };

/* the host hook: the request out, and the answer the next one gets */
static struct {
	bool out;
	uint64_t id, last;
	unsigned int turn;
	unsigned int flags;
	uint64_t min_bytes;
} host;

static int host_cb(struct tsm_vte *vte, const struct tsm_vte_image_request *req,
		   void *data)
{
	const unsigned int known = TSM_VTE_IMAGE_INFLATE | TSM_VTE_IMAGE_PNG |
				   TSM_VTE_IMAGE_EXACT | TSM_VTE_IMAGE_DISCARD;

	(void)vte;
	(void)data;
	check(!host.out);
	check(req->id == host.last + 1);
	check(req->medium == 'd' ? req->data && !req->name : req->name && !req->data);
	check(!(req->flags & ~known));
	/* the spec's integers are 32-bit, and inline data has no bounds */
	check(req->offset <= UINT32_MAX && req->read_max <= UINT32_MAX);
	check(req->medium != 'd' || (!req->offset && !req->read_max));
	if (req->flags & TSM_VTE_IMAGE_DISCARD) {
		check(req->flags == TSM_VTE_IMAGE_DISCARD);
		check(req->medium == 't' || req->medium == 's');
		check(!req->offset && !req->read_max && !req->min_bytes);
	} else if (req->flags & TSM_VTE_IMAGE_PNG) {
		/* EXACT only for inline PNG with zlib and a non-zero S */
		check(!(req->flags & TSM_VTE_IMAGE_EXACT) ||
		      (req->medium == 'd' && (req->flags & TSM_VTE_IMAGE_INFLATE) &&
		       req->min_bytes));
		check(!req->min_bytes || (req->flags & TSM_VTE_IMAGE_EXACT));
	} else {
		/* a raw format: s x v x bpp, and inline only with zlib */
		check(!(req->flags & TSM_VTE_IMAGE_EXACT) && req->min_bytes);
		check(req->medium != 'd' || (req->flags & TSM_VTE_IMAGE_INFLATE));
	}
	host.last = req->id;
	/* every fifth request the hook refuses itself */
	if (++host.turn % 5 == 4)
		return host.turn & 8 ? -EIO : -ENOSPC;
	/* and every thirteenth it resets the vte, which drops the request */
	if (host.turn % 13 == 12) {
		tsm_vte_reset(vte);
		return 0;
	}
	host.out = true;
	host.id = req->id;
	host.flags = req->flags;
	host.min_bytes = req->min_bytes;
	return 0;
}

/* cancels the request out in one of four ways, or resizes the screen; true
 * when it is cancelled */
static bool host_disturb(struct tsm_screen *screen, struct tsm_vte *vte)
{
	switch (host.turn / 5 % 8) {
	case 3:
		tsm_vte_reset(vte);
		return true;
	case 4:
		tsm_vte_hard_reset(vte);
		return true;
	case 5:
		tsm_vte_set_kitty_graphics(vte, false);
		tsm_vte_set_kitty_graphics(vte, true);
		return true;
	case 6:
		/* answered EBADF to the client, or a discard's refusal */
		tsm_vte_set_image_cb(vte, host_cb, NULL);
		return true;
	case 7:
		check(tsm_screen_resize(screen, sizes[host.turn / 40 % 4].cols,
					sizes[host.turn / 40 % 4].rows) == 0);
		return false;
	default:
		return false;
	}
}

static int reply_with(struct tsm_vte *vte, uint64_t id, unsigned int status,
		      unsigned int width, unsigned int height, const uint8_t *data,
		      size_t len)
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

/* answers the request out: what it asks for, an outcome, or a mistake */
static void host_answer(struct tsm_screen *screen, struct tsm_vte *vte)
{
	static const uint8_t px[4 * 4 * 4] = { 255, 0, 0, 128 };
	bool png = host.flags & TSM_VTE_IMAGE_PNG;
	bool discard = host.flags & TSM_VTE_IMAGE_DISCARD;
	uint8_t *raw = NULL;
	size_t len;
	int r;

	host.out = false;
	/* a stale id changes nothing */
	check(reply_with(vte, host.id + 1, TSM_VTE_IMAGE_OK, 1, 1, px, 4) == -ENOENT);
	if (host_disturb(screen, vte)) {
		/* the request is gone: its reply finds nothing, and the next
		 * bounded call consumes input again (the caller checks) */
		check(reply_with(vte, host.id, TSM_VTE_IMAGE_FAILED, 0, 0, NULL, 0) == -ENOENT);
		return;
	}
	/* a raw format's bytes, as a host bounded by its budget sends them */
	len = png || discard ? 64 : host.min_bytes;
	if (!png && !discard && len <= 65536)
		raw = calloc(1, len + 1);
	switch (host.turn % 5) {
	case 0:
		if (png)
			r = reply_with(vte, host.id, TSM_VTE_IMAGE_OK, 4, 4, px, 64);
		else if (raw)
			r = reply_with(vte, host.id, TSM_VTE_IMAGE_OK, 0, 0, raw, len);
		else
			r = reply_with(vte, host.id, TSM_VTE_IMAGE_OVER_BUDGET, 0, 0, NULL, 0);
		break;
	case 1:
		r = reply_with(vte, host.id, TSM_VTE_IMAGE_UNREADABLE + host.turn / 5 % 6,
			       0, 0, NULL, 0);
		break;
	case 2:
		/* one byte more than asked, or an unknown status */
		if (host.turn & 8)
			r = reply_with(vte, host.id, TSM_VTE_IMAGE_FAILED + 1, 0, 0, NULL, 0);
		else if (png)
			r = reply_with(vte, host.id, TSM_VTE_IMAGE_OK, 1, 3, px, 13);
		else
			r = reply_with(vte, host.id, TSM_VTE_IMAGE_OK, 0, 0, raw ? raw : px,
				       raw ? len + 1 : 0);
		check(r == (discard ? 0 : -EINVAL));
		free(raw);
		return;
	default:
		r = reply_with(vte, host.id, TSM_VTE_IMAGE_OK, 4, 4, NULL, 64);
		check(r == (discard ? 0 : -EINVAL));
		free(raw);
		return;
	}
	free(raw);
	check(r == 0);
}

static void check_frame(struct tsm_screen *screen)
{
	const struct tsm_frame *f = tsm_screen_frame(screen);
	const struct tsm_frame_placement *p;
	struct tsm_frame_image img;
	unsigned int i, n;

	for (i = 0; i < f->placement_count; ++i) {
		p = tsm_frame_placement_at(f, i);
		check(tsm_screen_frame_image(screen, p->image_key, &img) == 0);
		check(img.width && img.height && img.pixels);
		check(p->src_x + p->src_width <= img.width);
		check(p->src_y + p->src_height <= img.height);
	}
	check(tsm_screen_get_image_bytes(screen) == held);
	/* each store holds a bounded number of images, each paying for its
	 * bookkeeping */
	n = tsm_screen_test_image_count(screen);
	check(n <= 2 * TSM_SCREEN_IMAGES_PER_STORE);
	check(held >= (size_t)n * TSM_SCREEN_IMAGE_OVERHEAD);
	n = tsm_screen_test_placement_count(screen);
	check(n <= 2 * TSM_SCREEN_PLACEMENTS_PER_STORE);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	size_t chunk, pos, n, used;
	uint8_t a, b;

	if (size < 2)
		return 0;
	a = data[0];
	b = data[1];
	data += 2;
	size -= 2;

	held = 0;
	budget = budgets[a >> 5 & 3];
	check(tsm_screen_new(&screen, NULL, NULL) == 0);
	check(tsm_screen_resize(screen, sizes[a & 3].cols, sizes[a & 3].rows) == 0);
	check(tsm_screen_set_cell_size(screen, cells[a >> 2 & 3].w,
				       cells[a >> 2 & 3].h) == 0);
	tsm_screen_set_max_sb(screen, a & 0x10 ? 20 : 0);
	check(tsm_screen_set_image_budget(screen, reserve, release, NULL) == 0);
	check(tsm_vte_new(&vte, screen, write_cb, NULL, NULL, NULL) == 0);
	tsm_vte_set_kitty_graphics(vte, true);
	memset(&host, 0, sizeof(host));
	host.turn = b >> 6;
	tsm_vte_set_image_cb(vte, host_cb, NULL);
	tsm_vte_set_sixel(vte, !(b & 4));
	check(tsm_vte_set_graphic_max_size(vte, graphic[b & 3].w,
					   graphic[b & 3].h) == 0);

	chunk = chunks[b >> 4 & 3];
	if (!chunk) {
		/* tsm_vte_input hands out no request */
		tsm_vte_input(vte, (const char *)data, size);
		check(!host.out);
		check_frame(screen);
	} else {
		for (pos = 0; pos < size; pos += used) {
			n = size - pos < chunk ? size - pos : chunk;
			used = tsm_vte_input_bounded(vte, (const char *)data + pos, n);
			check(used >= 1 && used <= n);
			check(tsm_screen_get_image_bytes(screen) == held);
			if (host.out) {
				/* paused: nothing more until the reply */
				check(tsm_vte_input_bounded(vte, "x", 1) == 0);
				/* sometimes the vte is freed with it out */
				if (host.turn % 17 == 16)
					break;
				host_answer(screen, vte);
				check(tsm_screen_get_image_bytes(screen) == held);
			}
		}
		check_frame(screen);
	}

	/* an upload or picture still streaming gives its bytes back with the
	 * vte */
	tsm_vte_unref(vte);
	check(tsm_screen_get_image_bytes(screen) == held);
	tsm_screen_unref(screen);
	check(held == 0);
	return 0;
}

#ifdef FUZZ_REPLAY
#include <dirent.h>

static int replay(const char *path, unsigned int *count)
{
	FILE *f;
	uint8_t *buf;
	long n;

	f = fopen(path, "rb");
	if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0) {
		perror(path);
		return 1;
	}
	rewind(f);
	buf = malloc(n ? n : 1);
	if (!buf || fread(buf, 1, n, f) != (size_t)n) {
		perror(path);
		return 1;
	}
	fclose(f);
	LLVMFuzzerTestOneInput(buf, n);
	free(buf);
	++*count;
	return 0;
}

int main(int argc, char **argv)
{
	unsigned int count = 0;
	struct dirent *e;
	char path[4096];
	DIR *d;
	int i;

	for (i = 1; i < argc; ++i) {
		d = opendir(argv[i]);
		if (!d) {
			if (replay(argv[i], &count))
				return 1;
			continue;
		}
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "%s/%s", argv[i], e->d_name);
			if (replay(path, &count))
				return 1;
		}
		closedir(d);
	}
	printf("%u inputs replayed\n", count);
	/* a corpus that did not load proves nothing */
	return count ? 0 : 1;
}
#endif
