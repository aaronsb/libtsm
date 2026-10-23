/*
 * TSM - Image store and placement tests (ADR-103 step 3a in aaronsb/kmscon)
 *
 * The store's budget and eviction order, the frame's placement list with
 * each caller's changes, the image call, and the rules for freeing and
 * replacing images. What screen events do to placements is judged by the
 * tier 1 cases (test/conformance, img-*).
 *
 * SPDX-License-Identifier: MIT
 */

#include "libtsm-int.h"
#include "libtsm.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>

/* what an image of @w x @h pixels costs the budget */
#define COST(w, h) ((size_t)(w) * (h) * 4 + TSM_SCREEN_IMAGE_OVERHEAD)

static void log_cb(void *data, const char *file, int line, const char *func,
		   const char *subs, unsigned int sev, const char *format, va_list args)
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
	UNUSED(vte);
	UNUSED(u8);
	UNUSED(len);
	UNUSED(data);
}

/* a 10x5 screen with a vte and 8x16 cells */
static void setup(struct tsm_screen **screen, struct tsm_vte **vte)
{
	ck_assert_int_eq(tsm_screen_new(screen, log_cb, NULL), 0);
	ck_assert_int_eq(tsm_screen_resize(*screen, 10, 5), 0);
	ck_assert_int_eq(tsm_vte_new(vte, *screen, write_cb, NULL, log_cb, NULL), 0);
	ck_assert_int_eq(tsm_screen_set_cell_size(*screen, 8, 16), 0);
	tsm_screen_set_image_limit(*screen, 64 << 20);
}

static void teardown(struct tsm_screen *screen, struct tsm_vte *vte)
{
	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
}

static void input_str(struct tsm_vte *vte, const char *s)
{
	tsm_vte_input(vte, s, strlen(s));
}

/* a w x h image of one premultiplied colour */
static uint64_t make_image(struct tsm_screen *screen, unsigned int w, unsigned int h,
			   uint8_t kind, uint32_t id, bool transient, uint32_t argb)
{
	struct tsm_test_image img = { 0 };
	uint32_t *px;
	uint64_t key = 0;
	unsigned int i;

	px = malloc(sizeof(*px) * w * h);
	ck_assert_ptr_ne(px, NULL);
	for (i = 0; i < w * h; ++i)
		px[i] = argb;
	img.width = w;
	img.height = h;
	img.pixels = px;
	img.kind = kind;
	img.id = id;
	img.transient = transient;
	ck_assert_int_eq(tsm_screen_test_image(screen, &img, &key), 0);
	ck_assert_uint_ne(key, 0);
	free(px);
	return key;
}

static int try_image(struct tsm_screen *screen, unsigned int w, unsigned int h,
		     uint32_t id, uint64_t *key)
{
	struct tsm_test_image img = { 0 };
	uint32_t *px;
	int r;

	px = calloc((size_t)w * h, sizeof(*px));
	ck_assert_ptr_ne(px, NULL);
	img.width = w;
	img.height = h;
	img.pixels = px;
	img.kind = TSM_IMAGE_KITTY;
	img.id = id;
	r = tsm_screen_test_image(screen, &img, key);
	free(px);
	return r;
}

static uint64_t place(struct tsm_screen *screen, uint64_t image, unsigned int col,
		      unsigned int row)
{
	struct tsm_test_placement pl = { 0 };
	uint64_t key = 0;

	pl.image = image;
	pl.col = col;
	pl.row = row;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &key), 0);
	ck_assert_uint_ne(key, 0);
	return key;
}

static bool image_exists(struct tsm_screen *screen, uint64_t key)
{
	struct tsm_frame_image fi;

	return tsm_screen_frame_image(screen, key, &fi) == 0;
}

/* the frame's placement with @key, or NULL */
static const struct tsm_frame_placement *find(const struct tsm_frame *f, uint64_t key)
{
	unsigned int i;

	for (i = 0; i < f->placement_count; ++i)
		if (tsm_frame_placement_at(f, i)->key == key)
			return tsm_frame_placement_at(f, i);
	return NULL;
}

static unsigned int count_changed(const struct tsm_frame *f)
{
	unsigned int i, n = 0;

	for (i = 0; i < f->placement_count; ++i)
		if (tsm_frame_placement_at(f, i)->flags & TSM_FRAME_CHANGED)
			++n;
	return n;
}

START_TEST(test_image_store)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_test_image img = { 0 };
	struct tsm_frame_image fi;
	uint32_t px[8];
	uint64_t key, other;
	unsigned int i;

	setup(&screen, &vte);
	for (i = 0; i < 8; ++i)
		px[i] = 0xff000000 | i;
	img.width = 4;
	img.height = 2;
	img.pixels = px;
	img.kind = TSM_IMAGE_KITTY;
	img.id = 1;
	ck_assert_int_eq(tsm_screen_test_image(screen, &img, &key), 0);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(4, 2));

	ck_assert_int_eq(tsm_screen_frame_image(screen, key, &fi), 0);
	ck_assert_uint_eq(fi.width, 4);
	ck_assert_uint_eq(fi.height, 2);
	ck_assert_uint_eq(fi.stride, 16);
	ck_assert(fi.opaque);
	ck_assert_mem_eq(fi.pixels, px, sizeof(px));

	/* one pixel below full alpha: not opaque */
	px[5] = 0x80000000;
	img.id = 2;
	ck_assert_int_eq(tsm_screen_test_image(screen, &img, &other), 0);
	ck_assert_uint_ne(other, key);
	ck_assert_int_eq(tsm_screen_frame_image(screen, other, &fi), 0);
	ck_assert(!fi.opaque);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 2 * COST(4, 2));

	/* refusals */
	ck_assert_int_eq(tsm_screen_frame_image(screen, key + 12345, &fi), -ENOENT);
	ck_assert_int_eq(tsm_screen_frame_image(screen, 0, &fi), -ENOENT);
	ck_assert_int_eq(tsm_screen_frame_image(NULL, key, &fi), -EINVAL);
	img.width = 0;
	ck_assert_int_eq(tsm_screen_test_image(screen, &img, &other), -EINVAL);
	img.width = 4;
	img.kind = TSM_IMAGE_KITTY;
	img.id = 0;
	ck_assert_int_eq(tsm_screen_test_image(screen, &img, &other), -EINVAL);
	ck_assert_int_eq(tsm_screen_set_cell_size(screen, 0, 16), -EINVAL);

	teardown(screen, vte);
}
END_TEST

/*
 * Without a host budget the screen's limit applies, and the screen evicts in
 * ADR-103's order: unplaced images, transient ones first, least recently
 * used first; then images placed only in scrollback; never one placed on the
 * screen's lines, so a new image that still does not fit is refused.
 */
START_TEST(test_image_eviction_order)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_image_victim v;
	uint64_t on_screen, sb, old, transient, newer, key;
	int r;

	setup(&screen, &vte);
	tsm_screen_set_max_sb(screen, 100);
	/* 8x16 images, room for five */
	tsm_screen_set_image_limit(screen, 5 * COST(8, 16));

	sb = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	place(screen, sb, 0, 0);
	on_screen = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 2, false, 0xff00ff00);
	place(screen, on_screen, 0, 4);
	old = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 3, false, 0xffff0000);
	transient = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 4, true, 0xffffffff);
	newer = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 5, false, 0xff123456);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 5 * COST(8, 16));

	/* two lines scroll: row 0 enters scrollback, row 4 moves to row 2 */
	input_str(vte, "\e[5;1H\n\n");

	ck_assert(tsm_screen_image_victim(screen, &v));
	ck_assert_uint_eq(v.key, transient);
	ck_assert_uint_eq(v.rank, TSM_IMAGE_UNPLACED_TRANSIENT);
	ck_assert_uint_eq(v.bytes, COST(8, 16));

	/* a sixth image evicts the transient one */
	key = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 6, false, 0xff000000);
	ck_assert(!image_exists(screen, transient));
	ck_assert(image_exists(screen, old));
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 5 * COST(8, 16));

	/* then the least recently used unplaced one */
	ck_assert(tsm_screen_image_victim(screen, &v));
	ck_assert_uint_eq(v.key, old);
	ck_assert_uint_eq(v.rank, TSM_IMAGE_UNPLACED);
	ck_assert_uint_eq(tsm_screen_image_evict(screen), COST(8, 16));
	ck_assert(!image_exists(screen, old));
	ck_assert(tsm_screen_image_victim(screen, &v));
	ck_assert_uint_eq(v.key, newer);
	ck_assert_uint_eq(tsm_screen_image_evict(screen), COST(8, 16));
	ck_assert(tsm_screen_image_victim(screen, &v));
	ck_assert_uint_eq(v.key, key);
	ck_assert_uint_eq(tsm_screen_image_evict(screen), COST(8, 16));

	/* then the one placed only in scrollback, with its placement */
	ck_assert(tsm_screen_image_victim(screen, &v));
	ck_assert_uint_eq(v.key, sb);
	ck_assert_uint_eq(v.rank, TSM_IMAGE_SCROLLBACK);
	ck_assert_uint_eq(tsm_screen_image_evict(screen), COST(8, 16));
	ck_assert(!image_exists(screen, sb));

	/* never the one on the screen */
	ck_assert(!tsm_screen_image_victim(screen, &v));
	ck_assert_uint_eq(tsm_screen_image_evict(screen), 0);
	ck_assert(image_exists(screen, on_screen));
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(8, 16));

	/* a new image that cannot fit is refused, and evicts nothing */
	tsm_screen_set_image_limit(screen, 1000);
	r = try_image(screen, 8, 16, 7, &key);
	ck_assert_int_eq(r, -ENOSPC);
	ck_assert(image_exists(screen, on_screen));
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(8, 16));

	/* nor does one larger than the whole limit */
	tsm_screen_set_image_limit(screen, 4 * COST(8, 16));
	newer = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 8, false, 0xff000000);
	r = try_image(screen, 16, 64, 9, &key);
	ck_assert_int_eq(r, -ENOSPC);
	ck_assert(image_exists(screen, newer));

	teardown(screen, vte);
}
END_TEST

/* the victims a walk saw, up to @stop of them */
struct walk {
	struct tsm_image_victim seen[8];
	unsigned int n, stop;
};

static bool walk_cb(const struct tsm_image_victim *v, void *data)
{
	struct walk *w = data;

	ck_assert_uint_lt(w->n, 8);
	w->seen[w->n++] = *v;
	return w->n < w->stop;
}

/*
 * The victim walk names every image the screen may evict, in the order
 * tsm_screen_image_evict then evicts them, and evicts none itself: a host
 * learns what eviction would free before it evicts. It stops when the
 * callback says so, and never names an image placed on the screen's lines.
 */
START_TEST(test_image_victim_walk)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct walk w = { .stop = 8 };
	uint64_t on_screen, sb, old, transient, newer;
	unsigned int i;

	setup(&screen, &vte);
	tsm_screen_set_max_sb(screen, 100);
	tsm_screen_set_image_limit(screen, 5 * COST(8, 16));
	ck_assert_int_eq(tsm_screen_image_victims(NULL, walk_cb, &w), -EINVAL);
	ck_assert_int_eq(tsm_screen_image_victims(screen, NULL, &w), -EINVAL);
	ck_assert_int_eq(tsm_screen_image_victims(screen, walk_cb, &w), 0);
	ck_assert_uint_eq(w.n, 0);

	sb = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	place(screen, sb, 0, 0);
	on_screen = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 2, false, 0xff00ff00);
	place(screen, on_screen, 0, 4);
	old = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 3, false, 0xffff0000);
	transient = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 4, true, 0xffffffff);
	newer = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 5, false, 0xff123456);
	input_str(vte, "\e[5;1H\n\n");

	ck_assert_int_eq(tsm_screen_image_victims(screen, walk_cb, &w), 0);
	ck_assert_uint_eq(w.n, 4);
	ck_assert_uint_eq(w.seen[0].key, transient);
	ck_assert_uint_eq(w.seen[0].rank, TSM_IMAGE_UNPLACED_TRANSIENT);
	ck_assert_uint_eq(w.seen[1].key, old);
	ck_assert_uint_eq(w.seen[1].rank, TSM_IMAGE_UNPLACED);
	ck_assert_uint_eq(w.seen[2].key, newer);
	ck_assert_uint_eq(w.seen[3].key, sb);
	ck_assert_uint_eq(w.seen[3].rank, TSM_IMAGE_SCROLLBACK);
	for (i = 0; i < 4; ++i)
		ck_assert_uint_eq(w.seen[i].bytes, COST(8, 16));
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 5 * COST(8, 16));
	ck_assert(image_exists(screen, transient) && image_exists(screen, sb));

	/* stopped after two */
	w.n = 0;
	w.stop = 2;
	ck_assert_int_eq(tsm_screen_image_victims(screen, walk_cb, &w), 0);
	ck_assert_uint_eq(w.n, 2);
	ck_assert_uint_eq(w.seen[1].key, old);

	/* eviction follows the walk, and the image on screen is never named */
	w.n = 0;
	w.stop = 8;
	ck_assert_int_eq(tsm_screen_image_victims(screen, walk_cb, &w), 0);
	for (i = 0; i < 4; ++i) {
		ck_assert_uint_eq(tsm_screen_image_evict(screen), COST(8, 16));
		ck_assert(!image_exists(screen, w.seen[i].key));
	}
	ck_assert(image_exists(screen, on_screen));
	w.n = 0;
	ck_assert_int_eq(tsm_screen_image_victims(screen, walk_cb, &w), 0);
	ck_assert_uint_eq(w.n, 0);

	teardown(screen, vte);
}
END_TEST

/*
 * A store holds at most TSM_SCREEN_IMAGES_PER_STORE images: one more evicts
 * in the usual order, from that store, or is refused when every image there
 * is placed on the screen's lines. Each image also costs
 * TSM_SCREEN_IMAGE_OVERHEAD beside its pixels.
 */
START_TEST(test_image_store_cap)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	uint64_t first, second, key;
	unsigned int i;

	setup(&screen, &vte);
	first = make_image(screen, 1, 1, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	second = make_image(screen, 1, 1, TSM_IMAGE_KITTY, 2, false, 0xff0000ff);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 2 * COST(1, 1));
	for (i = 3; i <= TSM_SCREEN_IMAGES_PER_STORE; ++i)
		make_image(screen, 1, 1, TSM_IMAGE_KITTY, i, false, 0xff0000ff);
	ck_assert_uint_eq(tsm_screen_test_image_count(screen),
			  TSM_SCREEN_IMAGES_PER_STORE);

	/* one more evicts the least recently used unplaced image */
	key = make_image(screen, 1, 1, TSM_IMAGE_KITTY, 5000, false, 0xff0000ff);
	ck_assert(!image_exists(screen, first));
	ck_assert(image_exists(screen, second));
	ck_assert(image_exists(screen, key));
	ck_assert_uint_eq(tsm_screen_test_image_count(screen),
			  TSM_SCREEN_IMAGES_PER_STORE);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen),
			  TSM_SCREEN_IMAGES_PER_STORE * COST(1, 1));

	/* replacing an id adds no image, so it needs no room */
	ck_assert_int_eq(try_image(screen, 1, 1, 5000, &key), 0);
	ck_assert(image_exists(screen, second));

	/* the alternate screen's store has room of its own */
	input_str(vte, "\e[?1049h");
	make_image(screen, 1, 1, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	ck_assert_uint_eq(tsm_screen_test_image_count(screen),
			  TSM_SCREEN_IMAGES_PER_STORE + 1);
	input_str(vte, "\e[?1049l");

	teardown(screen, vte);

	/* every image placed on the screen: the next is refused */
	setup(&screen, &vte);
	for (i = 1; i <= TSM_SCREEN_IMAGES_PER_STORE; ++i)
		place(screen, make_image(screen, 1, 1, TSM_IMAGE_KITTY, i, false,
					 0xff0000ff), 0, 0);
	ck_assert_int_eq(try_image(screen, 1, 1, 5000, &key), -ENOSPC);
	ck_assert_uint_eq(tsm_screen_test_image_count(screen),
			  TSM_SCREEN_IMAGES_PER_STORE);
	teardown(screen, vte);
}
END_TEST

/*
 * A kitty id transmitted again while a cut piece still owes its charge: what
 * is owed is settled first, which may evict the old image, and the new one
 * then stands alone rather than replacing a freed one.
 */
START_TEST(test_image_replace_while_owed)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_screen_attr attr = { 0 };
	uint64_t key;

	setup(&screen, &vte);
	tsm_vte_set_sixel(vte, true);
	tsm_screen_set_image_limit(screen, COST(4, 4) + COST(24, 6));
	make_image(screen, 4, 4, TSM_IMAGE_KITTY, 7, false, 0xff0000ff);
	input_str(vte, "\e[1;1H\eP0;0q!24~\e\\");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen),
			  COST(4, 4) + COST(24, 6));

	/* a write over the middle cell, by the host outside an input call */
	tsm_screen_move_to(screen, 1, 0);
	tsm_screen_write(screen, 'X', &attr);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(4, 4) +
			  COST(24, 6) + TSM_SCREEN_PLACEMENT_OVERHEAD);

	/* the piece's charge evicts the unplaced kitty image before id 7 is
	 * looked up; the new id 7 then has no room beside the placed sixel */
	ck_assert_int_eq(try_image(screen, 4, 4, 7, &key), -ENOSPC);
	ck_assert_uint_eq(tsm_screen_test_image_count(screen), 1);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen),
			  COST(24, 6) + TSM_SCREEN_PLACEMENT_OVERHEAD);
	teardown(screen, vte);
}
END_TEST

/*
 * A placement that reaches from scrollback onto the screen keeps its image;
 * so does an image with one placement in scrollback and one on the screen.
 */
START_TEST(test_image_eviction_partly_on_screen)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_test_placement pl = { 0 };
	struct tsm_image_victim v;
	uint64_t tall, twice, key;

	setup(&screen, &vte);
	tsm_screen_set_max_sb(screen, 100);

	/* two rows tall at row 0; one line scrolls, its top enters scrollback */
	tall = make_image(screen, 8, 32, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	pl.image = tall;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &key), 0);
	twice = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 2, false, 0xff0000ff);
	place(screen, twice, 2, 0);
	place(screen, twice, 2, 3);
	input_str(vte, "\e[5;1H\n");
	ck_assert(!tsm_screen_image_victim(screen, &v));

	/* both wholly in scrollback now */
	input_str(vte, "\n\n\n\n");
	ck_assert(tsm_screen_image_victim(screen, &v));
	ck_assert_uint_eq(v.rank, TSM_IMAGE_SCROLLBACK);

	teardown(screen, vte);
}
END_TEST

/* placing an image is a use of it */
START_TEST(test_image_lru)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_image_victim v;
	uint64_t a, b;

	setup(&screen, &vte);
	a = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	b = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 2, false, 0xff0000ff);
	ck_assert(tsm_screen_image_victim(screen, &v));
	ck_assert_uint_eq(v.key, a);

	/* a is placed, then its placement erased by ED 2: b is now the older */
	place(screen, a, 0, 0);
	input_str(vte, "\e[2J");
	ck_assert(image_exists(screen, a));
	ck_assert(tsm_screen_image_victim(screen, &v));
	ck_assert_uint_eq(v.key, b);

	teardown(screen, vte);
}
END_TEST

struct host {
	size_t budget;
	size_t used;
	unsigned int reserves;
	unsigned int releases;
	size_t last_reserve;
	struct tsm_screen *evict_from;	/* evicts there when over budget */
};

static int host_reserve(struct tsm_screen *con, size_t bytes, void *data)
{
	struct host *h = data;

	++h->reserves;
	h->last_reserve = bytes;
	while (h->evict_from && h->used + bytes > h->budget &&
	       tsm_screen_image_evict(h->evict_from))
		;
	if (h->used + bytes > h->budget)
		return -ENOSPC;
	h->used += bytes;
	return 0;
}

static void host_release(struct tsm_screen *con, size_t bytes, void *data)
{
	struct host *h = data;

	++h->releases;
	ck_assert_uint_ge(h->used, bytes);
	h->used -= bytes;
}

/*
 * With a host budget the host is asked for every reservation, told of every
 * release, and its answer replaces the screen's limit; a host that evicts
 * in its reserve callback gets the releases before it returns.
 */
START_TEST(test_image_host_budget)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct host h = { .budget = 2 * COST(8, 16) };
	uint64_t a, b, c, key;

	setup(&screen, &vte);
	tsm_screen_set_image_limit(screen, 1);
	tsm_screen_set_image_budget(screen, host_reserve, host_release, &h);

	a = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	ck_assert_uint_eq(h.reserves, 1);
	ck_assert_uint_eq(h.last_reserve, COST(8, 16));
	b = make_image(screen, 8, 16, TSM_IMAGE_SIXEL, 0, false, 0xff0000ff);
	place(screen, b, 0, 0);
	ck_assert_uint_eq(h.used, 2 * COST(8, 16));

	/* refused by the host: nothing is stored */
	ck_assert_int_eq(try_image(screen, 8, 16, 3, &key), -ENOSPC);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 2 * COST(8, 16));
	ck_assert_uint_eq(h.used, 2 * COST(8, 16));

	/* a sixel image goes with its last placement, and is released */
	input_str(vte, "\e[2J");
	ck_assert(!image_exists(screen, b));
	ck_assert_uint_eq(h.releases, 1);
	ck_assert_uint_eq(h.used, COST(8, 16));

	/* a host that evicts makes room, releasing before it grants */
	h.evict_from = screen;
	c = make_image(screen, 8, 32, TSM_IMAGE_KITTY, 4, false, 0xff0000ff);
	ck_assert(!image_exists(screen, a));
	ck_assert(image_exists(screen, c));
	ck_assert_uint_eq(h.releases, 2);
	ck_assert_uint_eq(h.used, COST(8, 32));

	/* the screen releases what it holds when freed */
	teardown(screen, vte);
	ck_assert_uint_eq(h.used, 0);
}
END_TEST

/*
 * A host budget installed over images the screen already holds is asked for
 * their bytes, and refused, leaves the screen as it was; one removed, or
 * replaced, is given them back. Each host's books balance.
 */
START_TEST(test_image_host_budget_install)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct host a = { .budget = 4 * COST(8, 16) }, b = { .budget = COST(8, 16) };
	uint64_t key;

	setup(&screen, &vte);
	make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	make_image(screen, 8, 16, TSM_IMAGE_KITTY, 2, false, 0xff0000ff);

	/* too small for what the screen holds: refused, nothing changes */
	ck_assert_int_eq(tsm_screen_set_image_budget(screen, host_reserve, host_release, &b),
			 -ENOSPC);
	ck_assert_uint_eq(b.used, 0);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 2 * COST(8, 16));

	ck_assert_int_eq(tsm_screen_set_image_budget(screen, host_reserve, host_release, &a), 0);
	ck_assert_uint_eq(a.used, 2 * COST(8, 16));
	make_image(screen, 8, 16, TSM_IMAGE_KITTY, 3, false, 0xff0000ff);
	ck_assert_uint_eq(a.used, 3 * COST(8, 16));

	/* RIS frees all three, released to the host that reserved them */
	input_str(vte, "\ec");
	ck_assert_uint_eq(a.used, 0);

	/* moving to another host hands the bytes over */
	b.budget = 4 * COST(8, 16);
	make_image(screen, 8, 16, TSM_IMAGE_KITTY, 4, false, 0xff0000ff);
	ck_assert_int_eq(tsm_screen_set_image_budget(screen, host_reserve, host_release, &b), 0);
	ck_assert_uint_eq(a.used, 0);
	ck_assert_uint_eq(b.used, COST(8, 16));

	/* removing the host gives them back; the screen's own limit then applies */
	tsm_screen_set_image_limit(screen, 1000);
	ck_assert_int_eq(tsm_screen_set_image_budget(screen, NULL, NULL, NULL), 0);
	ck_assert_uint_eq(b.used, 0);
	ck_assert_int_eq(try_image(screen, 8, 32, 5, &key), -ENOSPC);

	/* a reserve callback without a release one is refused */
	ck_assert_int_eq(tsm_screen_set_image_budget(screen, host_reserve, NULL, &a), -EINVAL);

	teardown(screen, vte);
	ck_assert_uint_eq(a.used, 0);
	ck_assert_uint_eq(b.used, 0);
}
END_TEST

/*
 * A kitty id transmitted again replaces its image only once the new one has
 * its bytes: refused, the old image and its placements stay. The old image's
 * bytes count as free for the new one, and a host evicting in its reserve
 * callback never evicts the image being replaced.
 */
START_TEST(test_image_replace_refused)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct host h = { .budget = 2 * COST(8, 16) };
	uint64_t old, other, key;

	setup(&screen, &vte);
	tsm_screen_set_image_limit(screen, 2 * COST(8, 16));
	old = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	place(screen, old, 0, 0);
	other = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 2, false, 0xff0000ff);
	place(screen, other, 2, 0);

	/* an 8x32 id 1 cannot fit beside id 2's placed 8x16 */
	ck_assert_int_eq(try_image(screen, 8, 32, 1, &key), -ENOSPC);
	ck_assert(image_exists(screen, old));
	ck_assert_uint_eq(tsm_screen_frame(screen)->placement_count, 2);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 2 * COST(8, 16));

	/* an 8x16 fits in the old one's room */
	ck_assert_int_eq(try_image(screen, 8, 16, 1, &key), 0);
	ck_assert(!image_exists(screen, old));
	ck_assert(image_exists(screen, key));
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 2 * COST(8, 16));

	/* under a host that evicts: the image being replaced is not its victim */
	input_str(vte, "\ec");
	ck_assert_int_eq(tsm_screen_set_image_budget(screen, host_reserve, host_release, &h), 0);
	h.evict_from = screen;
	old = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	other = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 2, false, 0xff0000ff);
	place(screen, other, 0, 0);
	ck_assert_int_eq(try_image(screen, 8, 32, 1, &key), -ENOSPC);
	ck_assert(image_exists(screen, old));
	ck_assert(image_exists(screen, other));
	ck_assert_uint_eq(h.used, 2 * COST(8, 16));

	teardown(screen, vte);
	ck_assert_uint_eq(h.used, 0);
}
END_TEST

/*
 * Under a host budget as without one, a replacement needs room only for what
 * it adds: the old image's bytes pass to the new one. A same-size
 * replacement at a full budget reserves nothing; a smaller one gives the
 * difference back; a larger one is asked for the difference alone.
 */
START_TEST(test_image_replace_host)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct host h = { .budget = 2 * COST(8, 16) };
	uint64_t old, other, key;
	unsigned int reserves;

	setup(&screen, &vte);
	ck_assert_int_eq(tsm_screen_set_image_budget(screen, host_reserve, host_release, &h), 0);
	old = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	place(screen, old, 0, 0);
	other = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 2, false, 0xff0000ff);
	place(screen, other, 2, 0);
	ck_assert_uint_eq(h.used, 2 * COST(8, 16));

	reserves = h.reserves;
	ck_assert_int_eq(try_image(screen, 8, 16, 1, &key), 0);
	ck_assert(!image_exists(screen, old));
	ck_assert_uint_eq(h.reserves, reserves);
	ck_assert_uint_eq(h.used, 2 * COST(8, 16));
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 2 * COST(8, 16));

	old = key;
	ck_assert_int_eq(try_image(screen, 8, 8, 1, &key), 0);
	ck_assert(!image_exists(screen, old));
	ck_assert_uint_eq(h.used, COST(8, 16) + COST(8, 8));
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(8, 16) + COST(8, 8));

	/* 1024 bytes need 768 more: past the budget, refused, the old kept */
	old = key;
	ck_assert_int_eq(try_image(screen, 8, 32, 1, &key), -ENOSPC);
	ck_assert_uint_eq(h.last_reserve, 768);
	ck_assert(image_exists(screen, old));
	ck_assert_uint_eq(h.used, COST(8, 16) + COST(8, 8));

	/* 512 bytes, 256 more, fit */
	ck_assert_int_eq(try_image(screen, 8, 16, 1, &key), 0);
	ck_assert_uint_eq(h.last_reserve, 256);
	ck_assert_uint_eq(h.used, 2 * COST(8, 16));

	teardown(screen, vte);
	ck_assert_uint_eq(h.used, 0);
}
END_TEST

/*
 * An image whose bytes would wrap a 32-bit size_t is refused on every
 * platform: 32768 x 32768 x 4 is 2^32, which a 32-bit size_t holds as 0.
 * Neither call reads the pixels it is given.
 */
START_TEST(test_image_size_wrap)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_test_image img = { 0 };
	uint32_t px = 0xff000000;
	uint64_t key;

	setup(&screen, &vte);
	img.kind = TSM_IMAGE_KITTY;
	img.id = 1;
	img.pixels = &px;
	img.width = 32768;
	img.height = 32768;
	ck_assert((uint64_t)img.width * img.height * 4 > UINT32_MAX);
	ck_assert_int_eq(tsm_screen_test_image(screen, &img, &key), -EINVAL);

	/* one column fewer is a size, which the budget alone refuses */
	img.width = 32767;
	ck_assert((uint64_t)img.width * img.height * 4 <= UINT32_MAX);
	ck_assert_int_eq(tsm_screen_test_image(screen, &img, &key), -ENOSPC);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);

	teardown(screen, vte);
}
END_TEST

/* with neither a host budget nor a limit, a screen stores no image */
START_TEST(test_image_closed_by_default)
{
	struct tsm_screen *screen;
	uint64_t key;

	ck_assert_int_eq(tsm_screen_new(&screen, log_cb, NULL), 0);
	ck_assert_int_eq(try_image(screen, 1, 1, 1, &key), -ENOSPC);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);
	tsm_screen_unref(screen);
}
END_TEST

/* sizes are bounded, so no product of them overflows */
START_TEST(test_image_bounds)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_test_placement pl = { 0 };
	uint64_t key;

	setup(&screen, &vte);
	ck_assert_int_eq(tsm_screen_set_cell_size(screen, TSM_SCREEN_CELL_MAX + 1, 16), -EINVAL);
	ck_assert_int_eq(tsm_screen_set_cell_size(screen, 8, TSM_SCREEN_CELL_MAX + 1), -EINVAL);
	ck_assert_int_eq(try_image(screen, TSM_SCREEN_IMAGE_MAX + 1, 1, 1, &key), -EINVAL);
	ck_assert_int_eq(try_image(screen, 1, TSM_SCREEN_IMAGE_MAX + 1, 1, &key), -EINVAL);

	pl.image = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	pl.cols = TSM_SCREEN_IMAGE_MAX_CELLS + 1;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &key), -EINVAL);
	pl.cols = 0;
	pl.rows = TSM_SCREEN_IMAGE_MAX_CELLS + 1;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &key), -EINVAL);
	pl.cols = TSM_SCREEN_IMAGE_MAX_CELLS;
	pl.rows = TSM_SCREEN_IMAGE_MAX_CELLS;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &key), 0);

	teardown(screen, vte);
}
END_TEST

/*
 * A sixel image is freed with its last placement; a kitty image stays until
 * deleted or evicted, as the spec has a lowercase delete keep its data.
 */
START_TEST(test_image_lifetime)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	uint64_t sixel, kitty;

	setup(&screen, &vte);
	sixel = make_image(screen, 8, 16, TSM_IMAGE_SIXEL, 0, false, 0xff0000ff);
	place(screen, sixel, 0, 0);
	kitty = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	place(screen, kitty, 2, 0);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 2 * COST(8, 16));

	input_str(vte, "\e[2J");
	ck_assert(!image_exists(screen, sixel));
	ck_assert(image_exists(screen, kitty));
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(8, 16));
	ck_assert_uint_eq(tsm_screen_frame(screen)->placement_count, 0);

	/* the alternate screen's store is emptied when it is left */
	input_str(vte, "\e[?1049h");
	kitty = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	place(screen, kitty, 0, 0);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 2 * COST(8, 16));
	input_str(vte, "\e[?1049l");
	ck_assert(!image_exists(screen, kitty));
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(8, 16));

	/* RIS empties both */
	input_str(vte, "\ec");
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), 0);

	teardown(screen, vte);
}
END_TEST

/*
 * Transmitting a kitty id again replaces the image and deletes its
 * placements; a placement with the image's id and an existing placement id
 * replaces that placement, which keeps its key and moves.
 */
START_TEST(test_image_kitty_replace)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_test_placement pl = { 0 };
	const struct tsm_frame *f;
	const struct tsm_frame_placement *p;
	uint64_t first, second, k1, k2;

	setup(&screen, &vte);
	first = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 7, false, 0xff0000ff);
	pl.image = first;
	pl.id = 3;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &k1), 0);
	pl.col = 4;
	pl.row = 2;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &k2), 0);
	ck_assert_uint_eq(k1, k2);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(f->placement_count, 1);
	p = find(f, k1);
	ck_assert_ptr_ne(p, NULL);
	ck_assert_int_eq(p->row, 2);
	ck_assert_uint_eq(p->col, 4);

	place(screen, first, 0, 0);
	ck_assert_uint_eq(tsm_screen_frame(screen)->placement_count, 2);

	second = make_image(screen, 16, 16, TSM_IMAGE_KITTY, 7, false, 0xff00ff00);
	ck_assert_uint_ne(second, first);
	ck_assert(!image_exists(screen, first));
	ck_assert_uint_eq(tsm_screen_frame(screen)->placement_count, 0);
	ck_assert_uint_eq(tsm_screen_get_image_bytes(screen), COST(16, 16));

	teardown(screen, vte);
}
END_TEST

/*
 * The frame's placement list: draw order by band, then z, then kitty image
 * id, then creation, with sixels after kitty placements at z 0; the box,
 * offset, drawn size and source rectangle of each.
 */
START_TEST(test_image_frame_order)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_test_placement pl = { 0 };
	const struct tsm_frame *f;
	const struct tsm_frame_placement *p;
	uint64_t i5, i3, sx, keys[7];
	int32_t z[] = { 0, 1, -1, -1073741825, 0, 0, -1073741824 };
	unsigned int i;

	setup(&screen, &vte);
	i5 = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 5, false, 0xff0000ff);
	i3 = make_image(screen, 8, 16, TSM_IMAGE_KITTY, 3, false, 0xff0000ff);
	sx = make_image(screen, 8, 16, TSM_IMAGE_SIXEL, 0, false, 0x80000080);

	/* made in this order, at columns 0 to 6 */
	for (i = 0; i < 7; ++i) {
		pl.image = i == 4 ? sx : i == 5 ? i3 : i5;
		pl.col = i;
		pl.z = z[i];
		ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &keys[i]), 0);
	}

	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(f->placement_count, 7);
	ck_assert_uint_ge(f->placement_size, sizeof(struct tsm_frame_placement));
	/* band 0; band 1 by z; band 2: z 0 by image id, the sixel, z 1 */
	ck_assert_uint_eq(tsm_frame_placement_at(f, 0)->key, keys[3]);
	ck_assert_uint_eq(tsm_frame_placement_at(f, 0)->band, TSM_FRAME_BAND_BELOW_BG);
	ck_assert_uint_eq(tsm_frame_placement_at(f, 1)->key, keys[6]);
	ck_assert_uint_eq(tsm_frame_placement_at(f, 1)->band, TSM_FRAME_BAND_BELOW_TEXT);
	ck_assert_uint_eq(tsm_frame_placement_at(f, 2)->key, keys[2]);
	ck_assert_uint_eq(tsm_frame_placement_at(f, 3)->key, keys[5]);
	ck_assert_uint_eq(tsm_frame_placement_at(f, 3)->band, TSM_FRAME_BAND_ABOVE_TEXT);
	ck_assert_uint_eq(tsm_frame_placement_at(f, 4)->key, keys[0]);
	ck_assert_uint_eq(tsm_frame_placement_at(f, 5)->key, keys[4]);
	ck_assert_uint_eq(tsm_frame_placement_at(f, 5)->kind, TSM_IMAGE_SIXEL);
	ck_assert_int_eq(tsm_frame_placement_at(f, 5)->z, 0);
	ck_assert(!(tsm_frame_placement_at(f, 5)->flags & TSM_FRAME_OPAQUE));
	ck_assert(tsm_frame_placement_at(f, 4)->flags & TSM_FRAME_OPAQUE);
	ck_assert_uint_eq(tsm_frame_placement_at(f, 6)->key, keys[1]);

	/* the image key leads back to the pixels */
	p = tsm_frame_placement_at(f, 3);
	ck_assert_uint_eq(p->image_key, i3);

	teardown(screen, vte);
}
END_TEST

START_TEST(test_image_place_geometry)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_test_placement pl = { 0 };
	const struct tsm_frame_placement *p;
	uint64_t img, k[5];

	setup(&screen, &vte);
	img = make_image(screen, 40, 20, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);

	/* at its pixel size, offset 4,2 in its first cell: 44x22 pixels need
	 * 6x2 cells */
	pl.image = img;
	pl.x = 4;
	pl.y = 2;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &k[0]), 0);

	/* both c and r: fitted and centred, 40x20 into 16x32 is 16x8 */
	memset(&pl, 0, sizeof(pl));
	pl.image = img;
	pl.cols = 2;
	pl.rows = 2;
	pl.row = 2;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &k[1]), 0);

	/* c alone: the height follows, 16x8 in one row */
	pl.rows = 0;
	pl.col = 3;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &k[2]), 0);

	/* a source rectangle cut to the image */
	memset(&pl, 0, sizeof(pl));
	pl.image = img;
	pl.row = 4;
	pl.src_x = 30;
	pl.src_y = 5;
	pl.src_width = 100;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &k[3]), 0);

	/* refusals */
	pl.src_x = 40;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &k[4]), -EINVAL);
	pl.src_x = 0;
	pl.col = 10;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &k[4]), -EINVAL);
	pl.col = 0;
	pl.image = img + 1000;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &k[4]), -ENOENT);
	pl.image = img;
	pl.x = 8;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &k[4]), -EINVAL);

	/* a later cell size applies to later placements only */
	ck_assert_int_eq(tsm_screen_set_cell_size(screen, 10, 20), 0);
	pl.x = 0;
	pl.col = 8;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &k[4]), 0);

	const struct tsm_frame *f = tsm_screen_frame(screen);
	ck_assert_uint_eq(f->placement_count, 5);

	p = find(f, k[0]);
	ck_assert_int_eq(p->row, 0);
	ck_assert_uint_eq(p->col, 0);
	ck_assert_uint_eq(p->cols, 6);
	ck_assert_uint_eq(p->rows, 2);
	ck_assert_uint_eq(p->x, 4);
	ck_assert_uint_eq(p->y, 2);
	ck_assert_uint_eq(p->width, 40);
	ck_assert_uint_eq(p->height, 20);
	ck_assert_uint_eq(p->src_width, 40);
	ck_assert_uint_eq(p->src_height, 20);
	ck_assert_uint_eq(p->cell_width, 8);
	ck_assert_uint_eq(p->cell_height, 16);

	p = find(f, k[1]);
	ck_assert_uint_eq(p->cols, 2);
	ck_assert_uint_eq(p->rows, 2);
	ck_assert_uint_eq(p->x, 0);
	ck_assert_uint_eq(p->y, 12);
	ck_assert_uint_eq(p->width, 16);
	ck_assert_uint_eq(p->height, 8);
	ck_assert_uint_eq(p->src_width, 40);
	ck_assert_uint_eq(p->src_height, 20);

	p = find(f, k[2]);
	ck_assert_uint_eq(p->cols, 2);
	ck_assert_uint_eq(p->rows, 1);
	ck_assert_uint_eq(p->width, 16);
	ck_assert_uint_eq(p->height, 8);

	p = find(f, k[3]);
	ck_assert_uint_eq(p->src_x, 30);
	ck_assert_uint_eq(p->src_y, 5);
	ck_assert_uint_eq(p->src_width, 10);
	ck_assert_uint_eq(p->src_height, 15);
	ck_assert_uint_eq(p->cols, 2);
	ck_assert_uint_eq(p->rows, 1);

	/* drawn clipped at the right edge, its box kept */
	p = find(f, k[4]);
	ck_assert_uint_eq(p->cell_width, 10);
	ck_assert_uint_eq(p->cell_height, 20);
	ck_assert_uint_eq(p->col, 8);
	ck_assert_uint_eq(p->cols, 4);

	teardown(screen, vte);
}
END_TEST

/*
 * Two callers, as two displays of one console: each sees a placement's
 * change once, in its own frame. Added, cut by text, moved by a scroll; a
 * removed placement is missing by key.
 */
START_TEST(test_image_frame_changed_callers)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_test_placement pl = { 0 };
	struct tsm_frame_age a = { 0 }, b = { 0 };
	const struct tsm_frame *f;
	const struct tsm_frame_placement *p;
	uint64_t img, sixel, kitty;

	setup(&screen, &vte);
	img = make_image(screen, 24, 16, TSM_IMAGE_SIXEL, 0, false, 0xff0000ff);
	pl.image = img;
	pl.col = 2;
	pl.row = 1;
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &sixel), 0);

	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(f->placement_count, 1);
	ck_assert_uint_eq(count_changed(f), 1);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(count_changed(f), 1);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(count_changed(f), 0);

	/* a kitty placement added while b looks away */
	kitty = place(screen, make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false,
					 0xff00ff00), 0, 3);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(f->placement_count, 2);
	ck_assert_uint_eq(count_changed(f), 1);
	ck_assert(find(f, kitty)->flags & TSM_FRAME_CHANGED);

	/* text away from both changes neither */
	input_str(vte, "\e[5;1Hx");
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(count_changed(f), 0);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(count_changed(f), 1);
	ck_assert(find(f, kitty)->flags & TSM_FRAME_CHANGED);

	/* a print in the sixel's middle cell splits it: two pieces, the left
	 * one keeping the key, both changed for each caller */
	input_str(vte, "\e[2;4Hy");
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(f->placement_count, 3);
	ck_assert_uint_eq(count_changed(f), 2);
	p = find(f, sixel);
	ck_assert_ptr_ne(p, NULL);
	ck_assert_uint_eq(p->col, 2);
	ck_assert_uint_eq(p->cols, 1);
	ck_assert_uint_eq(p->src_x, 0);
	ck_assert_uint_eq(p->src_width, 8);
	ck_assert(!(find(f, kitty)->flags & TSM_FRAME_CHANGED));
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(count_changed(f), 2);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(count_changed(f), 0);

	/* a scroll moves them all */
	input_str(vte, "\e[5;1H\n");
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(count_changed(f), 3);
	ck_assert_int_eq(find(f, sixel)->row, 0);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(count_changed(f), 3);

	/* ED 2 removes them: none by key */
	input_str(vte, "\e[2J");
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(f->placement_count, 0);
	ck_assert_ptr_eq(find(f, sixel), NULL);

	/* a zeroed age redraws whatever is placed */
	place(screen, make_image(screen, 8, 16, TSM_IMAGE_KITTY, 2, false,
				 0xff00ff00), 0, 0);
	f = tsm_screen_frame_since(screen, &b);
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(count_changed(f), 0);
	memset(&b, 0, sizeof(b));
	f = tsm_screen_frame_since(screen, &b);
	ck_assert_uint_eq(count_changed(f), 1);

	teardown(screen, vte);
}
END_TEST

/*
 * A selective erase removes a sixel's part under the cells it erases, which
 * skip a protected cell: the row splits around it. The parser sets no
 * protected cells, so this one is written through the public API.
 */
START_TEST(test_image_selective_erase)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_screen_attr attr = { 0 };
	const struct tsm_frame *f;
	const struct tsm_frame_placement *p;
	uint64_t key;

	setup(&screen, &vte);
	attr.protect = 1;
	tsm_screen_move_to(screen, 3, 1);
	tsm_screen_write(screen, 'P', &attr);
	key = place(screen, make_image(screen, 24, 32, TSM_IMAGE_SIXEL, 0, false,
				       0xff0000ff), 2, 1);
	tsm_screen_move_to(screen, 0, 1);
	tsm_screen_erase_current_line(screen, true);

	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(f->placement_count, 2);
	p = find(f, key);
	ck_assert_int_eq(p->row, 1);
	ck_assert_uint_eq(p->col, 3);
	ck_assert_uint_eq(p->cols, 1);
	ck_assert_uint_eq(p->rows, 1);
	ck_assert_uint_eq(p->src_x, 8);
	ck_assert_uint_eq(p->src_y, 0);
	ck_assert_uint_eq(p->src_width, 8);
	ck_assert_uint_eq(p->src_height, 16);
	p = tsm_frame_placement_at(f, 1);
	ck_assert_uint_ne(p->key, key);
	ck_assert_int_eq(p->row, 2);
	ck_assert_uint_eq(p->col, 2);
	ck_assert_uint_eq(p->cols, 3);
	ck_assert_uint_eq(p->src_y, 16);
	ck_assert_uint_eq(p->src_height, 16);

	teardown(screen, vte);
}
END_TEST

/*
 * A resize that pulls lines back from scrollback and pushes them out again
 * leaves a placement's rows counted right: one reaching from scrollback onto
 * the screen is still on the screen's lines, so ED 2 removes it.
 */
START_TEST(test_image_sb_rows_after_resize)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_test_placement pl = { 0 };
	const struct tsm_frame *f;
	uint64_t key;

	setup(&screen, &vte);
	tsm_screen_set_max_sb(screen, 20);
	pl.image = make_image(screen, 8, 64, TSM_IMAGE_KITTY, 1, false, 0xff0000ff);
	ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &key), 0);

	/* rows -3 to 0, then back on the screen, then out again */
	input_str(vte, "\e[5;1H\n\n\n");
	ck_assert_int_eq(tsm_screen_resize(screen, 10, 7), 0);
	ck_assert_int_eq(tsm_screen_resize(screen, 10, 5), 0);
	f = tsm_screen_frame(screen);
	ck_assert_uint_eq(f->placement_count, 1);
	ck_assert_int_eq(tsm_frame_placement_at(f, 0)->row, -3);

	input_str(vte, "\e[2J");
	ck_assert_uint_eq(tsm_screen_frame(screen)->placement_count, 0);

	teardown(screen, vte);
}
END_TEST

/* after the age counter wraps, a caller draws every placement once */
START_TEST(test_image_frame_wrap)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_frame_age a = { 0 };
	const struct tsm_frame *f;

	setup(&screen, &vte);
	screen->age_cnt = (tsm_age_t)-60;
	place(screen, make_image(screen, 8, 16, TSM_IMAGE_KITTY, 1, false,
				 0xff00ff00), 0, 0);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(count_changed(f), 1);
	while (screen->age_cnt > 1000)
		input_str(vte, "\e[3;3Hx");
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(count_changed(f), 1);
	f = tsm_screen_frame_since(screen, &a);
	ck_assert_uint_eq(count_changed(f), 0);

	teardown(screen, vte);
}
END_TEST

/*
 * Random screen events over random placements, from fixed seeds. After each
 * event every placement is anchored on a line of its own buffer and listed
 * there, shows some of its picture, and the counts of placements per image,
 * of sixels per store and of bytes held agree with what is stored.
 */

static uint32_t rnd_state;

static uint32_t rnd(uint32_t n)
{
	rnd_state = rnd_state * 1103515245 + 12345;
	return (rnd_state >> 16) % n;
}

static bool line_lists(struct line *line, struct tsm_placement *p)
{
	struct shl_dlist *iter;

	shl_dlist_for_each(iter, &line->placements)
		if (shl_dlist_entry(iter, struct tsm_placement, line_link) == p)
			return true;
	return false;
}

static bool line_in_buffer(struct tsm_screen *con, struct line *line, bool alt)
{
	struct shl_dlist *iter;
	unsigned int i;

	for (i = 0; i < con->line_num; ++i)
		if ((alt ? con->alt_lines : con->main_lines)[i] == line)
			return true;
	if (alt)
		return false;
	shl_dlist_for_each(iter, &con->sb.list)
		if (shl_dlist_entry(iter, struct line, list) == line)
			return true;
	return false;
}

static void check_store(struct tsm_screen *con, struct tsm_image_store *st, bool alt,
			size_t *bytes)
{
	struct shl_dlist *iter, *it2;
	struct tsm_placement *p;
	struct tsm_image *image;
	unsigned int sixels = 0, n;
	long long bw, bh;

	shl_dlist_for_each(iter, &st->placements) {
		p = shl_dlist_entry(iter, struct tsm_placement, store_link);
		ck_assert_ptr_ne(p->line, NULL);
		ck_assert_ptr_eq(p->image->store, st);
		ck_assert(line_in_buffer(con, p->line, alt));
		ck_assert(line_lists(p->line, p));
		bw = (long long)p->cols * p->cw;
		bh = (long long)p->rows * p->ch;
		ck_assert(p->cols && p->rows);
		ck_assert(p->ox < bw && p->ox + (long long)p->dw > 0);
		ck_assert(p->oy < bh && p->oy + (long long)p->dh > 0);
		ck_assert_uint_le(p->rows, st->max_rows);
		if (p->image->kind == TSM_IMAGE_SIXEL) {
			++sixels;
			ck_assert_int_eq(p->z, 0);
			if (!st->max_rows_stale)
				ck_assert_uint_le(p->rows, st->max_sixel_rows);
		}
	}
	ck_assert_uint_eq(sixels, st->sixels);

	shl_dlist_for_each(iter, &st->images) {
		image = shl_dlist_entry(iter, struct tsm_image, list);
		n = 0;
		shl_dlist_for_each(it2, &st->placements)
			if (shl_dlist_entry(it2, struct tsm_placement, store_link)->image == image)
				++n;
		ck_assert_uint_eq(n, image->placements);
		/* a sixel's image goes with its last placement */
		if (image->kind == TSM_IMAGE_SIXEL)
			ck_assert_uint_gt(n, 0);
		*bytes += image->bytes;
	}
}

static void check_invariants(struct tsm_screen *con)
{
	size_t bytes = 0;

	ck_assert(shl_dlist_empty(&con->img.pending));
	check_store(con, &con->img.main, false, &bytes);
	check_store(con, &con->img.alt, true, &bytes);
	ck_assert_uint_eq(bytes, tsm_screen_get_image_bytes(con));
}

static void random_event(struct tsm_screen *screen, struct tsm_vte *vte,
			 struct tsm_frame_age *ages)
{
	static const char *const seqs[] = {
		"\e[K", "\e[1K", "\e[2K", "\e[J", "\e[1J", "\e[2J", "\e[3J",
		"\e[?J", "\e[?2K", "\e[3X", "\e[2@", "\e[P", "\e[2L", "\e[M",
		"\e[S", "\e[2T", "\eD", "\eM", "\n", "\r\n", "\e[?47h", "\e[?47l",
		"\e[?1047h", "\e[?1047l", "\e[?1049h", "\e[?1049l", "\e[!p",
		"\e[4h", "\e[4l", "\e[r", "\e[2;5r", "\e[3;9r", "x", "xyz",
		"\xe4\xb8\x80", "\xe2\x9d\xa4\xef\xb8\x8f", "\e[H", "\e[5;3H",
		"\e[20;1H", "\e[A", "\e[B",
	};
	struct tsm_test_placement pl = { 0 };
	struct tsm_test_image img = { 0 };
	static uint32_t px[40 * 60];
	uint64_t key;
	char buf[32];
	uint32_t op = rnd(100);
	unsigned int i;

	if (op < 30) {
		input_str(vte, seqs[rnd(sizeof(seqs) / sizeof(seqs[0]))]);
	} else if (op < 45) {
		snprintf(buf, sizeof(buf), "\e[%u;%uH", rnd(14) + 1, rnd(12) + 1);
		input_str(vte, buf);
	} else if (op < 70) {
		img.width = rnd(40) + 1;
		img.height = rnd(60) + 1;
		img.kind = rnd(2) ? TSM_IMAGE_KITTY : TSM_IMAGE_SIXEL;
		img.id = img.kind == TSM_IMAGE_KITTY ? rnd(6) + 1 : 0;
		img.transient = rnd(4) == 0;
		for (i = 0; i < img.width * img.height; ++i)
			px[i] = rnd(3) ? 0xff102030 : 0x40000000;
		img.pixels = px;
		/* refused once the screen's own placements fill the limit */
		if (tsm_screen_test_image(screen, &img, &key))
			return;
		pl.image = key;
		pl.col = rnd(tsm_screen_get_width(screen));
		pl.row = rnd(tsm_screen_get_height(screen));
		pl.x = rnd(8);
		pl.y = rnd(16);
		pl.cols = rnd(3) ? 0 : rnd(4);
		pl.rows = rnd(3) ? 0 : rnd(4);
		pl.src_x = rnd(2) ? 0 : rnd(img.width);
		pl.src_y = rnd(2) ? 0 : rnd(img.height);
		pl.z = (int32_t)rnd(5) - 2;
		pl.id = rnd(3);
		ck_assert_int_eq(tsm_screen_test_place(screen, &pl, &key), 0);
	} else if (op < 75) {
		ck_assert_int_eq(tsm_screen_resize(screen, rnd(14) + 1, rnd(12) + 1), 0);
	} else if (op < 78) {
		tsm_screen_set_max_sb(screen, rnd(6));
	} else if (op < 82) {
		tsm_screen_sb_up(screen, rnd(4) + 1);
	} else if (op < 84) {
		tsm_screen_sb_reset(screen);
	} else if (op < 86) {
		tsm_screen_image_evict(screen);
	} else if (op < 87) {
		input_str(vte, "\ec");
	} else {
		ck_assert_ptr_ne(tsm_screen_frame_since(screen, &ages[rnd(2)]), NULL);
	}
}

START_TEST(test_image_random_events)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	struct tsm_frame_age ages[2];
	const struct tsm_frame *f;
	unsigned int seed, i, j;

	for (seed = 1; seed <= 40; ++seed) {
		rnd_state = seed;
		memset(ages, 0, sizeof(ages));
		setup(&screen, &vte);
		tsm_screen_set_max_sb(screen, 4);
		tsm_screen_set_image_limit(screen, 40000);
		for (i = 0; i < 400; ++i) {
			random_event(screen, vte, ages);
			check_invariants(screen);
		}
		/* every listed placement shows something, in draw order */
		f = tsm_screen_frame(screen);
		ck_assert_ptr_ne(f, NULL);
		for (j = 0; j < f->placement_count; ++j) {
			ck_assert_uint_gt(tsm_frame_placement_at(f, j)->width, 0);
			ck_assert_uint_gt(tsm_frame_placement_at(f, j)->height, 0);
			if (j)
				ck_assert_int_le(tsm_frame_placement_at(f, j - 1)->z,
						 tsm_frame_placement_at(f, j)->z);
		}
		teardown(screen, vte);
	}
}
END_TEST

TEST_DEFINE_CASE(store)
	TEST(test_image_store)
	TEST(test_image_eviction_order)
	TEST(test_image_victim_walk)
	TEST(test_image_eviction_partly_on_screen)
	TEST(test_image_lru)
	TEST(test_image_store_cap)
	TEST(test_image_replace_while_owed)
	TEST(test_image_host_budget)
	TEST(test_image_host_budget_install)
	TEST(test_image_replace_refused)
	TEST(test_image_replace_host)
	TEST(test_image_size_wrap)
	TEST(test_image_closed_by_default)
	TEST(test_image_bounds)
	TEST(test_image_lifetime)
	TEST(test_image_kitty_replace)
TEST_END_CASE

TEST_DEFINE_CASE(frame)
	TEST(test_image_frame_order)
	TEST(test_image_place_geometry)
	TEST(test_image_frame_changed_callers)
	TEST(test_image_selective_erase)
	TEST(test_image_sb_rows_after_resize)
	TEST(test_image_frame_wrap)
TEST_END_CASE

/* 16000 events with the invariants checked after each: under a second here,
 * but close to four on a macOS runner, where check's default timeout of 4 s
 * failed it */
TEST_DEFINE_CASE(events)
	TIMEOUT(20)
	TEST(test_image_random_events)
TEST_END_CASE

TEST_DEFINE(
	TEST_SUITE(image,
		TEST_CASE(store),
		TEST_CASE(frame),
		TEST_CASE(events),
		TEST_END
	)
)
