/*
 * libtsm - Images and their placements
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Images (ADR-103 in aaronsb/kmscon, step 3a)
 *
 * Each screen buffer, main and alternate, has a store of images and of the
 * placements that show them. Images cost width x height x 4 bytes, reserved
 * from a budget before they are allocated: the host's, or the screen's own
 * limit. Over the budget, images go in the kitty spec's storage-quota order
 * (ADR-103, step 4): unplaced, then placed only in scrollback, then kitty
 * images placed on a screen's own lines, oldest transmission first. A sixel
 * placed on a screen's own lines is never evicted.
 *
 * A placement is anchored to the line holding its top row and listed on that
 * line, so it travels with the text: up and down the screen, into scrollback
 * and back. The screen tells this module of every other event that moves or
 * clears lines and cells, and each does what ADR-103's table says. A sixel is
 * cell content: text written or erased over it removes that part. A kitty
 * placement follows the kitty spec: of the erase commands only ED 2 touches
 * it, and a region scroll moves it only when it lies wholly inside.
 *
 * Removing part of a placement splits it into the pieces left over. Each
 * piece is a placement of the same picture with a smaller box; the picture
 * itself is never cut (see struct tsm_placement), so the frame derives the
 * source rectangle a piece shows from the box alone.
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "shl-llog.h"
#include "shl_dlist.h"

#define LLOG_SUBSYSTEM "tsm-image"

/* a placement with z below this draws under cell backgrounds */
#define Z_BELOW_BG (-1073741824)

/*
 * The clock an image's last use reads, shared by every screen in the process,
 * so a host can order the victims of its screens against each other.
 */
static uint64_t image_clock;

static struct tsm_image_store *cur_store(struct tsm_screen *con)
{
	if (con->alt_lines && con->lines == con->alt_lines)
		return &con->img.alt;
	return &con->img.main;
}

static void store_init(struct tsm_image_store *st)
{
	shl_dlist_init(&st->images);
	shl_dlist_init(&st->placements);
	st->sixels = 0;
	st->max_rows = 0;
	st->max_sixel_rows = 0;
}

/* the sb_id of the last scrollback line, from which scrollback rows count */
static uint64_t sb_last_id(struct tsm_screen *con)
{
	if (shl_dlist_empty(&con->sb.list))
		return 0;
	return shl_dlist_last(&con->sb.list, struct line, list)->sb_id;
}

/* the screen row of @line, in scrollback: -1 for its last line */
static int sb_row(struct tsm_screen *con, struct line *line)
{
	return -(int)(sb_last_id(con) - line->sb_id + 1);
}

/* the line at screen row @row of the buffer in use, scrollback above it */
static struct line *line_at_row(struct tsm_screen *con, int row)
{
	struct line *line;

	if (row >= 0)
		return (unsigned int)row < con->size_y ? con->lines[row] : NULL;
	if (con->lines != con->main_lines || shl_dlist_empty(&con->sb.list))
		return NULL;
	line = shl_dlist_last(&con->sb.list, struct line, list);
	while (line && ++row < 0)
		line = shl_dlist_prev(line, &con->sb.list, list);
	return line;
}

/* the line above @line, at screen row @row, or NULL; @row moves up with it */
static struct line *line_above(struct tsm_screen *con, struct line *line, int *row)
{
	if (*row > 0) {
		--*row;
		return con->lines[*row];
	}
	if (*row == 0) {
		if (con->lines != con->main_lines || shl_dlist_empty(&con->sb.list))
			return NULL;
		--*row;
		return shl_dlist_last(&con->sb.list, struct line, list);
	}
	--*row;
	return shl_dlist_prev(line, &con->sb.list, list);
}

/*
 * Budget
 */

static size_t evictable_bytes(struct tsm_screen *con);

/*
 * Reserves room for a new image of @bytes. @keep, the image a replacement
 * will free, passes its bytes to the new one: only what the new image adds is
 * reserved, from the host or under the screen's limit, and @keep is never
 * evicted to make room. A request that cannot fit evicts nothing. Returns the
 * bytes reserved in @added; screen_image_new hands @keep's share over once
 * the new image exists.
 */
static int budget_reserve(struct tsm_screen *con, size_t bytes, struct tsm_image *keep,
			  size_t *added)
{
	struct tsm_images *img = &con->img;
	size_t credit, need, room;
	int ret = 0;

	/* only what the budget granted @keep passes on; what its pieces owe
	 * was never granted */
	credit = keep ? keep->bytes - keep->owed : 0;
	need = bytes > credit ? bytes - credit : 0;
	img->keep = keep;
	if (img->reserve) {
		if (need && img->reserve(con, need, img->budget_data) < 0)
			ret = -ENOSPC;
	} else if (img->bytes - credit + bytes > img->limit) {
		/* what the screen holds once @keep and every image it may
		 * evict are gone; a request that fits as things are needs no
		 * such scan */
		room = img->bytes - credit - evictable_bytes(con);
		if (bytes > img->limit || room + bytes > img->limit)
			ret = -ENOSPC;
		while (!ret && img->bytes - credit + bytes > img->limit)
			if (!tsm_screen_image_evict(con))
				ret = -ENOSPC;
	}
	img->keep = NULL;
	if (!ret) {
		img->bytes += need;
		*added = need;
	}
	return ret;
}

static void budget_release(struct tsm_screen *con, size_t bytes)
{
	con->img.bytes -= bytes;
	if (con->img.release && bytes)
		con->img.release(con, bytes, con->img.budget_data);
}

/*
 * @bytes @image no longer holds: what its pieces owed goes first, and only
 * the rest, which the budget granted, goes back to it.
 */
static void image_release(struct tsm_screen *con, struct tsm_image *image,
			  size_t bytes)
{
	size_t owed = bytes < image->owed ? bytes : image->owed;

	image->owed -= owed;
	con->img.owed -= owed;
	budget_release(con, bytes - owed);
}

/*
 * Image keys: a slot and its generation, so a key names one image and is
 * not reused while a frame might still name it.
 */

static int slot_take(struct tsm_images *img, struct tsm_image *image)
{
	struct tsm_image_slot *slots;
	unsigned int i, n;

	for (i = img->slot_free; i < img->slot_count; ++i)
		if (!img->slots[i].image)
			break;
	if (i == img->slot_count) {
		n = img->slot_count ? img->slot_count * 2 : 16;
		slots = realloc(img->slots, n * sizeof(*slots));
		if (!slots)
			return -ENOMEM;
		memset(&slots[img->slot_count], 0,
		       (n - img->slot_count) * sizeof(*slots));
		img->slots = slots;
		img->slot_count = n;
	}

	img->slots[i].image = image;
	++img->slots[i].generation;
	image->key = (uint64_t)img->slots[i].generation << 32 | (i + 1);
	img->slot_free = i + 1;
	return 0;
}

static void slot_release(struct tsm_images *img, struct tsm_image *image)
{
	unsigned int i = (uint32_t)image->key - 1;

	img->slots[i].image = NULL;
	if (i < img->slot_free)
		img->slot_free = i;
}

static struct tsm_image *image_find(struct tsm_images *img, uint64_t key)
{
	uint32_t i = (uint32_t)key;
	struct tsm_image_slot *slot;

	if (!i || i > img->slot_count)
		return NULL;
	slot = &img->slots[i - 1];
	if (!slot->image || slot->generation != (uint32_t)(key >> 32))
		return NULL;
	return slot->image;
}

/*
 * Placements
 */

static void placement_unlink_line(struct tsm_placement *p)
{
	if (p->line_link.next)
		shl_dlist_unlink(&p->line_link);
	p->line = NULL;
}

static void placement_attach(struct tsm_placement *p, struct line *line)
{
	struct tsm_image_store *st = p->image->store;

	if (p->rows > st->max_rows)
		st->max_rows = p->rows;
	if (p->image->kind == TSM_IMAGE_SIXEL && p->rows > st->max_sixel_rows)
		st->max_sixel_rows = p->rows;
	if (p->line == line)
		return;
	placement_unlink_line(p);
	p->line = line;
	shl_dlist_link_tail(&line->placements, &p->line_link);
}

/* detached until screen_images_scroll_done anchors it at screen row @row */
static void placement_pend(struct tsm_screen *con, struct tsm_placement *p, int row)
{
	placement_unlink_line(p);
	p->row = row;
	shl_dlist_link_tail(&con->img.pending, &p->line_link);
}

static void image_free(struct tsm_screen *con, struct tsm_image *image);

static void placement_free(struct tsm_screen *con, struct tsm_placement *p,
			   bool free_sixel)
{
	struct tsm_image *image = p->image;

	con->img.removed_age = con->age_cnt;
	placement_unlink_line(p);
	shl_dlist_unlink(&p->store_link);
	if (image->kind == TSM_IMAGE_SIXEL)
		--image->store->sixels;
	--image->store->nplacements;
	image->store->max_rows_stale = true;
	--image->placements;
	free(p);
	/* the first placement is paid by the image; the others give their
	 * charge back, unless the image is going whole */
	if (image->placements && !image->dying) {
		image->bytes -= TSM_SCREEN_PLACEMENT_OVERHEAD;
		image_release(con, image, TSM_SCREEN_PLACEMENT_OVERHEAD);
	}

	/* a sixel's image is anonymous: nothing can place it again */
	if (free_sixel && image->kind == TSM_IMAGE_SIXEL && !image->placements)
		image_free(con, image);
}

static struct tsm_placement *placement_new(struct tsm_screen *con,
					   struct tsm_image *image)
{
	struct tsm_placement *p;

	p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->image = image;
	p->key = ++con->img.last_placement_key;
	p->age = con->age_cnt;
	/* at the head: a screen event walking the store from its head never
	 * meets the pieces it makes (draw order is the frame's to sort) */
	shl_dlist_link(&image->store->placements, &p->store_link);
	if (image->kind == TSM_IMAGE_SIXEL)
		++image->store->sixels;
	++image->store->nplacements;
	++image->placements;
	return p;
}

/* whether any of the picture lies inside the box */
static bool placement_visible(const struct tsm_placement *p)
{
	long long bw = (long long)p->cols * p->cw, bh = (long long)p->rows * p->ch;

	return p->cols && p->rows &&
	       p->ox < bw && p->ox + (long long)p->dw > 0 &&
	       p->oy < bh && p->oy + (long long)p->dh > 0;
}

/*
 * A piece of a placement: box rows @r0 to @r1 and columns @c0 to @c1, kept
 * on @line, or, without one, pending until lines have moved and anchored at
 * screen row @row.
 */
struct piece {
	unsigned int r0, r1, c0, c1;
	struct line *line;
	int row;
};

/*
 * Replaces @p by @pieces: @p becomes the first piece that shows any of the
 * picture, keeping its key, and the others are new placements. A placement
 * with no such piece is removed.
 */
static void placement_split(struct tsm_screen *con, struct tsm_placement *p,
			    const struct piece *pieces, unsigned int n)
{
	const struct tsm_placement o = *p;
	struct tsm_image_store *st = o.image->store;
	struct tsm_placement g, *q;
	struct piece shown[4], t;
	bool kept = false;
	unsigned int i, j, m = 0, room;

	for (i = 0; i < n; ++i) {
		g = o;
		g.cols = pieces[i].c1 - pieces[i].c0 + 1;
		g.rows = pieces[i].r1 - pieces[i].r0 + 1;
		g.ox = o.ox - (int)(pieces[i].c0 * o.cw);
		g.oy = o.oy - (int)(pieces[i].r0 * o.ch);
		if (placement_visible(&g))
			shown[m++] = pieces[i];
	}

	/* past the store's placements, the largest pieces stay and the
	 * smallest go, as if their cells were erased */
	room = TSM_SCREEN_PLACEMENTS_PER_STORE - st->nplacements;
	if (m > room + 1) {
		for (i = 1; i < m; ++i)
			for (j = i; j > 0 &&
			     (shown[j].r1 - shown[j].r0 + 1) * (shown[j].c1 - shown[j].c0 + 1) >
			     (shown[j - 1].r1 - shown[j - 1].r0 + 1) *
			     (shown[j - 1].c1 - shown[j - 1].c0 + 1); --j) {
				t = shown[j];
				shown[j] = shown[j - 1];
				shown[j - 1] = t;
			}
		m = room + 1;
	}

	for (i = 0; i < m; ++i) {
		g.col = o.col + shown[i].c0;
		g.cols = shown[i].c1 - shown[i].c0 + 1;
		g.rows = shown[i].r1 - shown[i].r0 + 1;
		g.ox = o.ox - (int)(shown[i].c0 * o.cw);
		g.oy = o.oy - (int)(shown[i].r0 * o.ch);

		if (!kept) {
			q = p;
			kept = true;
		} else {
			q = placement_new(con, o.image);
			if (!q) {
				llog_warning(con, "cannot allocate a placement piece");
				continue;
			}
			/* charged now, granted once the event is over */
			o.image->bytes += TSM_SCREEN_PLACEMENT_OVERHEAD;
			o.image->owed += TSM_SCREEN_PLACEMENT_OVERHEAD;
			con->img.owed += TSM_SCREEN_PLACEMENT_OVERHEAD;
			q->seq = o.seq;
			q->id = o.id;
			q->dw = o.dw;
			q->dh = o.dh;
			q->sx = o.sx;
			q->sy = o.sy;
			q->sw = o.sw;
			q->sh = o.sh;
			q->cw = o.cw;
			q->ch = o.ch;
			q->z = o.z;
		}
		q->col = g.col;
		q->cols = g.cols;
		q->rows = g.rows;
		q->ox = g.ox;
		q->oy = g.oy;
		q->age = con->age_cnt;
		if (shown[i].line)
			placement_attach(q, shown[i].line);
		else
			placement_pend(con, q, shown[i].row);
	}

	if (!kept)
		placement_free(con, p, true);
}

/*
 * Removes box rows @er0 to @er1 and columns @ec0 to @ec1 of @p, whose top is
 * at screen row @row: what is left above stays on @p's line, what is left
 * beside and below is anchored on the screen's lines. A piece below the
 * screen has no line to hold it and goes.
 */
static void placement_cut(struct tsm_screen *con, struct tsm_placement *p, int row,
			  unsigned int er0, unsigned int er1,
			  unsigned int ec0, unsigned int ec1)
{
	struct piece pieces[4];
	struct line *mid = con->lines[row + (int)er0];
	unsigned int n = 0;

	if (er0)
		pieces[n++] = (struct piece){ 0, er0 - 1, 0, p->cols - 1, p->line, 0 };
	if (ec0)
		pieces[n++] = (struct piece){ er0, er1, 0, ec0 - 1, mid, 0 };
	if (ec1 + 1 < p->cols)
		pieces[n++] = (struct piece){ er0, er1, ec1 + 1, p->cols - 1, mid, 0 };
	if (er1 + 1 < p->rows && row + (int)er1 + 1 < (int)con->size_y)
		pieces[n++] = (struct piece){ er1 + 1, p->rows - 1, 0, p->cols - 1,
					      con->lines[row + er1 + 1], 0 };
	placement_split(con, p, pieces, n);
}

/*
 * At least the tallest placement's rows, and the tallest sixel's, which bound
 * the lines a search for the placements covering a row walks up; counted
 * again only after a placement has gone.
 */
static void store_count_rows(struct tsm_image_store *st)
{
	struct shl_dlist *iter;
	struct tsm_placement *p;

	if (!st->max_rows_stale)
		return;
	st->max_rows_stale = false;
	st->max_rows = 0;
	st->max_sixel_rows = 0;
	shl_dlist_for_each(iter, &st->placements) {
		p = shl_dlist_entry(iter, struct tsm_placement, store_link);
		if (p->rows > st->max_rows)
			st->max_rows = p->rows;
		if (p->image->kind == TSM_IMAGE_SIXEL && p->rows > st->max_sixel_rows)
			st->max_sixel_rows = p->rows;
	}
}

static unsigned int store_max_rows(struct tsm_image_store *st)
{
	store_count_rows(st);
	return st->max_rows;
}

/* each placement's screen row in p->row, for the store in use */
static void store_rows(struct tsm_screen *con, struct tsm_image_store *st)
{
	struct shl_dlist *iter;
	struct tsm_placement *p;
	unsigned int i;

	shl_dlist_for_each(iter, &st->placements) {
		p = shl_dlist_entry(iter, struct tsm_placement, store_link);
		p->row = p->line && p->line->sb_id ? sb_row(con, p->line) : INT_MAX;
	}
	for (i = 0; i < con->line_num; ++i) {
		shl_dlist_for_each(iter, &con->lines[i]->placements) {
			p = shl_dlist_entry(iter, struct tsm_placement, line_link);
			p->row = i;
		}
	}
}

static void store_clear(struct tsm_screen *con, struct tsm_image_store *st)
{
	struct shl_dlist *iter, *tmp;

	shl_dlist_for_each_safe(iter, tmp, &st->placements)
		placement_free(con, shl_dlist_entry(iter, struct tsm_placement,
						    store_link), false);
	shl_dlist_for_each_safe(iter, tmp, &st->images)
		image_free(con, shl_dlist_entry(iter, struct tsm_image, list));
	st->max_rows = 0;
	st->max_sixel_rows = 0;
}

/*
 * Images
 */

static void image_free(struct tsm_screen *con, struct tsm_image *image)
{
	struct shl_dlist *iter, *tmp;
	struct tsm_placement *p;

	/* its bytes, placements' included, go back in one release */
	image->dying = true;
	shl_dlist_for_each_safe(iter, tmp, &image->store->placements) {
		p = shl_dlist_entry(iter, struct tsm_placement, store_link);
		if (p->image == image)
			placement_free(con, p, false);
	}
	shl_dlist_unlink(&image->list);
	--image->store->count;
	slot_release(&con->img, image);
	image_release(con, image, image->bytes);
	free(image->pixels);
	free(image);
}

/* the kitty image @id names in @st; an anonymous one (id 0) is never found */
static struct tsm_image *store_find_id(struct tsm_image_store *st, uint32_t id)
{
	struct shl_dlist *iter;
	struct tsm_image *image;

	if (!id)
		return NULL;
	shl_dlist_for_each(iter, &st->images) {
		image = shl_dlist_entry(iter, struct tsm_image, list);
		if (image->kind == TSM_IMAGE_KITTY && image->id == id)
			return image;
	}
	return NULL;
}

/*
 * Marks the images that are never evicted: every sixel with a placement not
 * wholly in scrollback, every image held while it is placed, and the image a
 * replacement is reserving room to free. A kitty image with a placement not
 * wholly in scrollback is marked on_screen instead: under the storage-quota
 * rule it is evicted, after every other image, as rank 3.
 */
static void images_mark_pinned(struct tsm_screen *con)
{
	struct tsm_image_store *stores[] = { &con->img.main, &con->img.alt };
	struct tsm_image *image;
	struct tsm_placement *p;
	struct shl_dlist *iter;
	uint64_t last = sb_last_id(con);
	unsigned int i;

	for (i = 0; i < 2; ++i) {
		/* this pass, and the caller's over the images after it */
		con->img.work += 2 * stores[i]->count;
		shl_dlist_for_each(iter, &stores[i]->images) {
			image = shl_dlist_entry(iter, struct tsm_image, list);
			image->pinned = image->hold;
			image->on_screen = false;
		}
	}
	for (i = 0; i < 2; ++i) {
		shl_dlist_for_each(iter, &stores[i]->placements) {
			++con->img.work;
			p = shl_dlist_entry(iter, struct tsm_placement, store_link);
			/* wholly in scrollback: its rows end above the screen */
			if (p->line && p->line->sb_id &&
			    last - p->line->sb_id + 1 >= p->rows)
				continue;
			if (p->image->kind == TSM_IMAGE_SIXEL)
				p->image->pinned = true;
			else
				p->image->on_screen = true;
		}
	}
	if (con->img.keep)
		con->img.keep->pinned = true;
}

/* the bytes of every image the screen may evict */
static size_t evictable_bytes(struct tsm_screen *con)
{
	struct tsm_image_store *stores[] = { &con->img.main, &con->img.alt };
	struct tsm_image *image;
	struct shl_dlist *iter;
	size_t bytes = 0;
	unsigned int i;

	images_mark_pinned(con);
	for (i = 0; i < 2; ++i) {
		shl_dlist_for_each(iter, &stores[i]->images) {
			image = shl_dlist_entry(iter, struct tsm_image, list);
			if (!image->pinned)
				bytes += image->bytes;
		}
	}
	return bytes;
}

/* after images_mark_pinned */
static unsigned int image_rank(const struct tsm_image *image)
{
	if (image->on_screen)
		return TSM_IMAGE_PLACED_KITTY;
	if (image->placements)
		return TSM_IMAGE_SCROLLBACK;
	return image->transient ? TSM_IMAGE_UNPLACED_TRANSIENT : TSM_IMAGE_UNPLACED;
}

/*
 * An image's order within its rank, on the clock every screen shares: when
 * it was transmitted for rank 3, since every placement a frame draws has its
 * last use renewed in draw order; when it was last created, placed or drawn
 * for the others. Each value is unique within its rank.
 */
static uint64_t image_order(const struct tsm_image *image, unsigned int rank)
{
	return rank == TSM_IMAGE_PLACED_KITTY ? image->transmitted : image->last_use;
}

/*
 * The image a screen evicts next: no placement, transient first; then placed
 * only in scrollback; each least recently used first; then kitty images
 * placed on a screen's own lines, oldest transmission first. NULL when every
 * image is pinned. With @only, from that store alone.
 */
static struct tsm_image *image_victim(struct tsm_screen *con, unsigned int *rank,
				      struct tsm_image_store *only)
{
	struct tsm_image_store *stores[] = { &con->img.main, &con->img.alt };
	struct tsm_image *image, *best = NULL;
	struct shl_dlist *iter;
	unsigned int i, r, best_rank = 0;

	images_mark_pinned(con);
	for (i = 0; i < 2; ++i) {
		if (only && stores[i] != only)
			continue;
		shl_dlist_for_each(iter, &stores[i]->images) {
			image = shl_dlist_entry(iter, struct tsm_image, list);
			if (image->pinned)
				continue;
			r = image_rank(image);
			if (!best || r < best_rank ||
			    (r == best_rank && image_order(image, r) <
					       image_order(best, best_rank))) {
				best = image;
				best_rank = r;
			}
		}
	}
	if (rank)
		*rank = best_rank;
	return best;
}

/*
 * Screen events
 */

void screen_images_init(struct tsm_screen *con)
{
	store_init(&con->img.main);
	store_init(&con->img.alt);
	shl_dlist_init(&con->img.pending);
	con->img.cell_width = 8;
	con->img.cell_height = 16;
}

void screen_images_destroy(struct tsm_screen *con)
{
	store_clear(con, &con->img.main);
	store_clear(con, &con->img.alt);
	free(con->img.slots);
	free(con->img.frame);
}

void screen_images_line_free(struct tsm_screen *con, struct line *line)
{
	struct shl_dlist *iter, *tmp;

	shl_dlist_for_each_safe(iter, tmp, &line->placements)
		placement_free(con, shl_dlist_entry(iter, struct tsm_placement,
						    line_link), true);
}

void screen_images_discard_line(struct tsm_screen *con, struct line *line,
				struct line *next)
{
	struct shl_dlist *iter, *tmp;
	struct tsm_placement *p;
	struct piece piece;

	shl_dlist_for_each_safe(iter, tmp, &line->placements) {
		p = shl_dlist_entry(iter, struct tsm_placement, line_link);
		if (p->rows < 2 || !next) {
			placement_free(con, p, true);
			continue;
		}
		piece = (struct piece){ 1, p->rows - 1, 0, p->cols - 1, next, 0 };
		placement_split(con, p, &piece, 1);
	}
}

/*
 * A scroll of rows @top to @bottom by @num. Whole-screen scrolls carry
 * placements with their lines, into scrollback when the screen has one. In a
 * region (and at the edges of the screen) a sixel splits at the band's edges:
 * the part inside moves and is clipped where it leaves, the parts outside
 * stay. A kitty placement moves only when wholly inside, and is clipped the
 * same way; one across an edge stays where it is. What moves to a new line is
 * pending until screen_images_scroll_done, once the lines have moved.
 */
void screen_images_scroll(struct tsm_screen *con, unsigned int top,
			  unsigned int bottom, unsigned int num, bool up)
{
	struct tsm_image_store *st = cur_store(con);
	struct shl_dlist *iter, *tmp;
	struct tsm_placement *p;
	struct piece pieces[3];
	unsigned int n;
	int lo, hi, clip, d, r0, r1, ia, ib, na, nb;
	bool carry;

	if (shl_dlist_empty(&st->placements) || bottom < top)
		return;
	if (num > bottom - top + 1)
		num = bottom - top + 1;

	/* lines leaving the top of the screen enter scrollback: the band
	 * reaches up through it, and nothing is pushed out at the top */
	carry = up && top == 0 && st == &con->img.main && con->sb.max > 0;
	lo = carry ? INT_MIN : (int)top;
	/* nor is the screen's bottom an edge: rows reaching past it move with
	 * their lines, and only a scroll down pushes rows out there */
	hi = bottom + 1 == con->size_y ? INT_MAX : (int)bottom;
	clip = up ? hi : (int)bottom;
	d = up ? -(int)num : (int)num;

	store_rows(con, st);
	shl_dlist_for_each_safe(iter, tmp, &st->placements) {
		p = shl_dlist_entry(iter, struct tsm_placement, store_link);
		r0 = p->row;
		r1 = r0 + (int)p->rows - 1;
		if (r0 == INT_MAX || r1 < (int)top || r0 > (int)bottom)
			continue;

		n = 0;
		if (p->image->kind == TSM_IMAGE_KITTY && (r0 < lo || r1 > hi)) {
			/* across an edge: stays where it is, so an anchor that
			 * moves is replaced by the line taking its row */
			if (r0 >= (int)top || carry)
				placement_pend(con, p, r0);
			continue;
		}

		if (r0 < lo)
			pieces[n++] = (struct piece){ 0, lo - 1 - r0, 0, p->cols - 1,
						      p->line, 0 };

		/* the part inside the band moves, clipped to the band */
		ia = (r0 < lo ? lo : r0) - r0;
		ib = (r1 > hi ? hi : r1) - r0;
		na = r0 + ia + d < lo ? lo - (r0 + d) : ia;
		nb = r0 + ib + d > clip ? clip - (r0 + d) : ib;
		if (na <= nb) {
			pieces[n] = (struct piece){ na, nb, 0, p->cols - 1, NULL,
						    r0 + d + na };
			/* its own anchor moves with it */
			if (!na)
				pieces[n].line = p->line;
			++n;
		}

		if (r1 > hi && hi + 1 < (int)con->size_y)
			pieces[n++] = (struct piece){ hi + 1 - r0, p->rows - 1, 0,
						      p->cols - 1, con->lines[hi + 1], 0 };
		placement_split(con, p, pieces, n);
	}
}

void screen_images_scroll_done(struct tsm_screen *con)
{
	struct shl_dlist *iter, *tmp;
	struct tsm_placement *p;
	struct line *line;

	shl_dlist_for_each_safe(iter, tmp, &con->img.pending) {
		p = shl_dlist_entry(iter, struct tsm_placement, line_link);
		line = line_at_row(con, p->row);
		if (line)
			placement_attach(p, line);
		else
			placement_free(con, p, true);
	}
}

void screen_images_cut(struct tsm_screen *con, unsigned int y,
		       unsigned int x0, unsigned int x1)
{
	struct tsm_image_store *st = cur_store(con);
	struct {
		struct tsm_placement *p;
		unsigned int k;
	} hits[16];
	struct shl_dlist *iter;
	struct tsm_placement *p;
	struct line *line;
	unsigned int n, k, i, max;
	int row;

	if (!st->sixels || y >= con->size_y)
		return;
	if (x1 >= con->size_x)
		x1 = con->size_x - 1;
	store_count_rows(st);
	max = st->max_sixel_rows;

	/* the sixels covering row @y are anchored at most max - 1 lines
	 * above it; a cut piece no longer covers the cells, so a search
	 * again finds what one pass had no room for */
	do {
		n = 0;
		row = y;
		line = con->lines[y];
		for (k = 0; line && k < max && n < 16; ++k) {
			shl_dlist_for_each(iter, &line->placements) {
				p = shl_dlist_entry(iter, struct tsm_placement, line_link);
				if (p->image->kind != TSM_IMAGE_SIXEL || p->rows <= k ||
				    p->col > x1 || p->col + p->cols <= x0)
					continue;
				hits[n].p = p;
				hits[n].k = k;
				if (++n == 16)
					break;
			}
			line = line_above(con, line, &row);
		}
		for (i = 0; i < n; ++i) {
			p = hits[i].p;
			k = hits[i].k;
			placement_cut(con, p, (int)y - (int)k, k, k,
				      (x0 > p->col ? x0 : p->col) - p->col,
				      (x1 < p->col + p->cols - 1 ? x1 : p->col + p->cols - 1) -
				      p->col);
		}
	} while (n == 16);
}

void screen_images_erase_screen(struct tsm_screen *con)
{
	struct tsm_image_store *st = cur_store(con);
	struct shl_dlist *iter, *tmp;
	struct tsm_placement *p;

	if (shl_dlist_empty(&st->placements))
		return;
	store_rows(con, st);
	shl_dlist_for_each_safe(iter, tmp, &st->placements) {
		p = shl_dlist_entry(iter, struct tsm_placement, store_link);
		if (p->image->kind == TSM_IMAGE_KITTY && p->row != INT_MAX &&
		    p->row + (int)p->rows > 0 && p->row < (int)con->size_y)
			placement_free(con, p, false);
	}
}

void screen_images_clear_alt(struct tsm_screen *con)
{
	store_clear(con, &con->img.alt);
}

void screen_images_reset(struct tsm_screen *con)
{
	store_clear(con, &con->img.main);
	store_clear(con, &con->img.alt);
}

void screen_images_age_wrap(struct tsm_screen *con)
{
	struct tsm_image_store *stores[] = { &con->img.main, &con->img.alt };
	struct shl_dlist *iter;
	unsigned int i;

	for (i = 0; i < 2; ++i) {
		shl_dlist_for_each(iter, &stores[i]->placements)
			shl_dlist_entry(iter, struct tsm_placement, store_link)->age = 1;
		shl_dlist_for_each(iter, &stores[i]->images)
			shl_dlist_entry(iter, struct tsm_image, list)->age = 1;
	}
	con->img.removed_age = 1;
}

/* a placement was added, moved by an event that ages it, given a new
 * picture, or removed since @since */
bool screen_images_changed_since(struct tsm_screen *con, const struct tsm_frame_age *since)
{
	struct tsm_image_store *stores[] = { &con->img.main, &con->img.alt };
	struct tsm_placement *p;
	struct shl_dlist *iter;
	unsigned int i;

	if (con->img.removed_age > since->age)
		return true;
	for (i = 0; i < 2; ++i) {
		shl_dlist_for_each(iter, &stores[i]->placements) {
			p = shl_dlist_entry(iter, struct tsm_placement, store_link);
			if (p->age > since->age || p->image->age > since->age)
				return true;
		}
	}
	return false;
}

/*
 * The frame
 */

struct frame_rec {
	struct tsm_placement *p;
	int row;
};

static int frame_rec_cmp(const void *a, const void *b)
{
	const struct tsm_placement *p = ((const struct frame_rec *)a)->p;
	const struct tsm_placement *q = ((const struct frame_rec *)b)->p;

	/* z chooses the band; at one z, kitty placements by image id before
	 * sixels, then each in creation order, its pieces by key */
	if (p->z != q->z)
		return p->z < q->z ? -1 : 1;
	if (p->image->kind != q->image->kind)
		return p->image->kind == TSM_IMAGE_KITTY ? -1 : 1;
	if (p->image->id != q->image->id)
		return p->image->id < q->image->id ? -1 : 1;
	if (p->seq != q->seq)
		return p->seq < q->seq ? -1 : 1;
	return p->key < q->key ? -1 : p->key > q->key;
}

static uint8_t band_of(int32_t z)
{
	if (z < Z_BELOW_BG)
		return TSM_FRAME_BAND_BELOW_BG;
	if (z < 0)
		return TSM_FRAME_BAND_BELOW_TEXT;
	return TSM_FRAME_BAND_ABOVE_TEXT;
}

/* @rec's placement as the frame lists it */
static void frame_fill(struct tsm_frame_placement *out, const struct frame_rec *rec)
{
	const struct tsm_placement *p = rec->p;
	long long x0, y0, x1, y1;

	x0 = p->ox > 0 ? p->ox : 0;
	y0 = p->oy > 0 ? p->oy : 0;
	x1 = (long long)p->ox + p->dw;
	y1 = (long long)p->oy + p->dh;
	if (x1 > (long long)p->cols * p->cw)
		x1 = (long long)p->cols * p->cw;
	if (y1 > (long long)p->rows * p->ch)
		y1 = (long long)p->rows * p->ch;

	memset(out, 0, sizeof(*out));
	out->key = p->key;
	out->image_key = p->image->key;
	out->row = rec->row;
	out->col = p->col;
	out->cols = p->cols;
	out->rows = p->rows;
	out->x = x0;
	out->y = y0;
	out->width = x1 - x0;
	out->height = y1 - y0;
	/* the source pixels under the visible part: exact at 1:1, otherwise
	 * every source pixel any visible pixel shows */
	out->src_x = p->sx + (x0 - p->ox) * p->sw / p->dw;
	out->src_y = p->sy + (y0 - p->oy) * p->sh / p->dh;
	out->src_width = p->sx + ((x1 - p->ox) * p->sw + p->dw - 1) / p->dw - out->src_x;
	out->src_height = p->sy + ((y1 - p->oy) * p->sh + p->dh - 1) / p->dh - out->src_y;
	out->cell_width = p->cw;
	out->cell_height = p->ch;
	out->z = p->z;
	out->band = band_of(p->z);
	out->kind = p->image->kind;
	if (p->image->opaque)
		out->flags |= TSM_FRAME_OPAQUE;
}

/* adds @line's placements with a row in view, @line's top at view row @row */
static int frame_collect(struct tsm_screen *con, struct line *line, int row,
			 struct frame_rec **recs, unsigned int *n, unsigned int *cap)
{
	struct shl_dlist *iter;
	struct tsm_placement *p;
	struct frame_rec *tmp;

	shl_dlist_for_each(iter, &line->placements) {
		p = shl_dlist_entry(iter, struct tsm_placement, line_link);
		if (p->col >= con->size_x || row + (int)p->rows <= 0)
			continue;
		if (*n == *cap) {
			*cap = *cap ? *cap * 2 : 16;
			tmp = realloc(*recs, *cap * sizeof(**recs));
			if (!tmp)
				return -ENOMEM;
			*recs = tmp;
		}
		(*recs)[*n].p = p;
		(*recs)[*n].row = row;
		++*n;
	}
	return 0;
}

int screen_images_frame(struct tsm_screen *con, const struct tsm_frame_age *since,
			bool all_changed)
{
	struct tsm_images *img = &con->img;
	struct frame_rec *recs = NULL;
	struct tsm_frame_placement *out;
	struct line *line, *first, *next;
	unsigned int n = 0, cap = 0, i, k, max;
	int row, ret = 0;
	bool changed;

	con->frame.placement_size = sizeof(struct tsm_frame_placement);
	con->frame.placement_count = 0;
	con->frame.placements = img->frame;
	if (shl_dlist_empty(&img->main.placements) &&
	    shl_dlist_empty(&img->alt.placements))
		return 0;

	/* the visible lines, as the frame shows them */
	next = con->sb.pos;
	k = 0;
	first = NULL;
	for (i = 0; i < con->size_y; ++i) {
		if (next) {
			line = next;
			next = shl_dlist_next(next, &con->sb.list, list);
		} else {
			line = con->lines[k++];
		}
		if (!first)
			first = line;
		ret = frame_collect(con, line, i, &recs, &n, &cap);
		if (ret)
			goto out;
	}

	/* and above them, whatever reaches down into view */
	max = store_max_rows(&img->main);
	if (store_max_rows(&img->alt) > max)
		max = img->alt.max_rows;
	if (con->sb.pos) {
		line = shl_dlist_prev(con->sb.pos, &con->sb.list, list);
		row = -1;
	} else {
		row = 0;
		line = line_above(con, first, &row);
	}
	/* lines above the view are scrollback */
	for (k = 1; line && k < max; ++k) {
		ret = frame_collect(con, line, row, &recs, &n, &cap);
		if (ret)
			goto out;
		line = shl_dlist_prev(line, &con->sb.list, list);
		--row;
	}

	if (!n)
		goto out;
	if (n > img->frame_size) {
		out = realloc(img->frame, n * sizeof(*out));
		if (!out) {
			ret = -ENOMEM;
			goto out;
		}
		img->frame = out;
		img->frame_size = n;
	}

	qsort(recs, n, sizeof(*recs), frame_rec_cmp);
	changed = all_changed || con->age > since->age;
	for (i = 0; i < n; ++i) {
		frame_fill(&img->frame[i], &recs[i]);
		if (changed || recs[i].p->age > since->age ||
		    recs[i].p->image->age > since->age)
			img->frame[i].flags |= TSM_FRAME_CHANGED;
		recs[i].p->image->last_use = ++image_clock;
	}
	con->frame.placements = img->frame;
	con->frame.placement_count = n;

out:
	free(recs);
	return ret;
}

/*
 * Public calls
 */

SHL_EXPORT
int tsm_screen_frame_image(struct tsm_screen *con, uint64_t key,
			   struct tsm_frame_image *out)
{
	struct tsm_image *image;

	if (!con || !out)
		return -EINVAL;
	image = image_find(&con->img, key);
	if (!image)
		return -ENOENT;
	out->width = image->width;
	out->height = image->height;
	out->stride = (size_t)image->width * 4;
	out->pixels = image->pixels;
	out->opaque = image->opaque;
	return 0;
}

SHL_EXPORT
int tsm_screen_set_cell_size(struct tsm_screen *con, unsigned int width,
			     unsigned int height)
{
	if (!con || !width || !height || width > TSM_SCREEN_CELL_MAX ||
	    height > TSM_SCREEN_CELL_MAX)
		return -EINVAL;
	con->img.cell_width = width;
	con->img.cell_height = height;
	return 0;
}

SHL_EXPORT
int tsm_screen_set_image_budget(struct tsm_screen *con,
				tsm_screen_image_reserve_cb reserve,
				tsm_screen_image_release_cb release,
				void *data)
{
	struct tsm_images *img;
	size_t held;

	if (!con || !reserve != !release)
		return -EINVAL;
	img = &con->img;

	/* the new host takes on what the screen holds; it may evict from
	 * this screen to make room, releasing to the old host meanwhile */
	held = img->bytes;
	if (reserve && held && reserve(con, held, data) < 0)
		return -ENOSPC;
	if (img->release && img->bytes)
		img->release(con, img->bytes, img->budget_data);
	img->reserve = reserve;
	img->release = release;
	img->budget_data = data;
	if (release && held > img->bytes)
		release(con, held - img->bytes, data);
	return 0;
}

SHL_EXPORT
void tsm_screen_set_image_limit(struct tsm_screen *con, size_t bytes)
{
	if (!con)
		return;
	con->img.limit = bytes;
}

SHL_EXPORT
size_t tsm_screen_get_image_bytes(struct tsm_screen *con)
{
	return con ? con->img.bytes + con->img.owed : 0;
}

SHL_EXPORT
bool tsm_screen_image_victim(struct tsm_screen *con, struct tsm_image_victim *out)
{
	struct tsm_image *image;
	unsigned int rank;

	if (!con)
		return false;
	image = image_victim(con, &rank, NULL);
	if (!image)
		return false;
	if (out) {
		out->key = image->key;
		out->rank = rank;
		out->last_use = image_order(image, rank);
		out->bytes = image->bytes - image->owed;
	}
	return true;
}

SHL_EXPORT
size_t tsm_screen_image_evict(struct tsm_screen *con)
{
	struct tsm_image *image;
	size_t bytes;

	if (!con)
		return 0;
	image = image_victim(con, NULL, NULL);
	if (!image)
		return 0;
	bytes = image->bytes;
	image_free(con, image);
	return bytes;
}

/* eviction order: rank, then last_use, which image_order keeps unique within
 * a rank: least recently used, or for rank 3 oldest transmission, first */
static int victim_cmp(const void *a, const void *b)
{
	const struct tsm_image_victim *x = a, *y = b;

	if (x->rank != y->rank)
		return x->rank < y->rank ? -1 : 1;
	if (x->last_use != y->last_use)
		return x->last_use < y->last_use ? -1 : 1;
	return 0;
}

SHL_EXPORT
int tsm_screen_image_victims(struct tsm_screen *con, tsm_image_victim_cb cb, void *data)
{
	struct tsm_image_store *stores[2];
	struct tsm_image_victim *v;
	struct tsm_image *image;
	struct shl_dlist *iter;
	unsigned int i, n = 0;

	if (!con || !cb)
		return -EINVAL;
	stores[0] = &con->img.main;
	stores[1] = &con->img.alt;
	if (!con->img.main.count && !con->img.alt.count)
		return 0;
	v = malloc((con->img.main.count + con->img.alt.count) * sizeof(*v));
	if (!v)
		return -ENOMEM;
	images_mark_pinned(con);
	for (i = 0; i < 2; ++i) {
		shl_dlist_for_each(iter, &stores[i]->images) {
			image = shl_dlist_entry(iter, struct tsm_image, list);
			if (image->pinned)
				continue;
			v[n].key = image->key;
			v[n].rank = image_rank(image);
			v[n].last_use = image_order(image, v[n].rank);
			v[n].bytes = image->bytes - image->owed;
			++n;
		}
	}
	qsort(v, n, sizeof(*v), victim_cmp);
	for (i = 0; i < n && cb(&v[i], data); ++i)
		;
	free(v);
	return 0;
}

/*
 * Images and placements as the protocols make them (and the test calls,
 * tsm-image-test.c)
 */

struct tsm_image *screen_image_find(struct tsm_screen *con, uint64_t key)
{
	return image_find(&con->img, key);
}

/*
 * Fills in and stores @image, whose @bytes are reserved and whose pixels are
 * set, in the store in use; @old, a kitty image of the same id, is replaced.
 */
static void image_store(struct tsm_screen *con, struct tsm_image *image,
			const struct tsm_test_image *desc, size_t bytes,
			struct tsm_image *old, size_t added)
{
	struct tsm_image_store *st = cur_store(con);

	image->store = st;
	image->id = desc->kind == TSM_IMAGE_KITTY ? desc->id : 0;
	image->number = desc->number;
	image->width = desc->width;
	image->height = desc->height;
	image->bytes = bytes;
	image->kind = desc->kind;
	image->transient = desc->transient;
	image->last_use = ++image_clock;
	image->transmitted = image->last_use;
	image->age = con->age_cnt;
	/* the old image's bytes beyond what was added pass to the new one;
	 * freeing it releases only what is left over */
	if (old) {
		old->bytes -= bytes - added;
		image_free(con, old);
	}
	shl_dlist_link_tail(&st->images, &image->list);
	++st->count;
}

/*
 * Makes room in store @st for one more image: past
 * TSM_SCREEN_IMAGES_PER_STORE, its images go in the eviction order. Returns
 * -ENOSPC when none left may be evicted.
 */
static int store_make_room(struct tsm_screen *con, struct tsm_image_store *st)
{
	struct tsm_image *victim;

	while (st->count >= TSM_SCREEN_IMAGES_PER_STORE) {
		victim = image_victim(con, NULL, st);
		if (!victim)
			return -ENOSPC;
		image_free(con, victim);
	}
	return 0;
}

/*
 * Makes room in store @st for one more placement of @image, and reserves its
 * charge when @image already has one. -ENOSPC when neither can be had.
 */
static int placement_make_room(struct tsm_screen *con, struct tsm_image_store *st,
			       struct tsm_image *image)
{
	struct tsm_image *victim;
	size_t added;
	int ret;

	while (st->nplacements >= TSM_SCREEN_PLACEMENTS_PER_STORE) {
		victim = image_victim(con, NULL, st);
		if (!victim)
			return -ENOSPC;
		image_free(con, victim);
	}
	if (!image->placements)
		return 0;
	screen_images_settle(con);
	ret = budget_reserve(con, TSM_SCREEN_PLACEMENT_OVERHEAD, NULL, &added);
	if (!ret)
		image->bytes += TSM_SCREEN_PLACEMENT_OVERHEAD;
	return ret;
}

static int placement_area_cmp(const void *a, const void *b)
{
	const struct tsm_placement *p = *(struct tsm_placement *const *)a;
	const struct tsm_placement *q = *(struct tsm_placement *const *)b;
	unsigned long long x = (unsigned long long)p->cols * p->rows;
	unsigned long long y = (unsigned long long)q->cols * q->rows;

	return x < y ? -1 : x > y;
}

void screen_images_settle(struct tsm_screen *con)
{
	struct tsm_image_store *stores[] = { &con->img.main, &con->img.alt };
	struct tsm_images *img = &con->img;
	struct tsm_placement *p, **small;
	struct shl_dlist *iter;
	size_t owed, added, n, k, drop = 1;
	struct tsm_image *image;
	unsigned int i;

	while (img->owed) {
		/* asked for as a reservation, which may evict */
		owed = img->owed;
		if (!budget_reserve(con, owed, NULL, &added)) {
			/* evicted images took their pieces' debts with them:
			 * what was reserved for those goes back */
			budget_release(con, owed - img->owed);
			for (i = 0; i < 2; ++i)
				shl_dlist_for_each(iter, &stores[i]->images) {
					++img->work;
					shl_dlist_entry(iter, struct tsm_image, list)->owed = 0;
				}
			img->owed = 0;
			return;
		}

		/*
		 * Refused: the smallest placements of images that owe go,
		 * 1, 2, 4... a round, so the rounds stay few, each sorting the
		 * candidates once.
		 */
		n = 0;
		for (i = 0; i < 2; ++i)
			n += stores[i]->nplacements;
		small = malloc(n * sizeof(*small));
		if (!small)
			return;
		n = 0;
		for (i = 0; i < 2; ++i) {
			shl_dlist_for_each(iter, &stores[i]->placements) {
				++img->work;
				p = shl_dlist_entry(iter, struct tsm_placement, store_link);
				if (p->image->owed && !p->image->hold)
					small[n++] = p;
			}
		}
		qsort(small, n, sizeof(*small), placement_area_cmp);
		img->work += n;
		/* a placement goes alone unless it is its image's last, and
		 * then it had no other piece in the list */
		for (k = 0; k < n && k < drop && img->owed; ++k) {
			image = small[k]->image;
			if (image->owed)
				placement_free(con, small[k], true);
		}
		free(small);
		if (!n)
			return;
		drop *= 2;
	}
}

static bool pixels_opaque(const uint32_t *px, size_t n)
{
	size_t i;

	for (i = 0; i < n; ++i)
		if ((px[i] >> 24) != 0xff)
			return false;
	return true;
}

int screen_image_new(struct tsm_screen *con, const struct tsm_test_image *desc,
		     struct tsm_image **out)
{
	struct tsm_image_store *st;
	struct tsm_image *image, *old = NULL;
	size_t n, bytes, added;
	int ret;

	if (!desc->width || !desc->height || !desc->pixels ||
	    desc->width > TSM_SCREEN_IMAGE_MAX || desc->height > TSM_SCREEN_IMAGE_MAX ||
	    (uint64_t)desc->width * desc->height * 4 > TSM_SCREEN_IMAGE_MAX_BYTES ||
	    desc->kind > TSM_IMAGE_KITTY ||
	    (desc->kind == TSM_IMAGE_KITTY && !desc->id))
		return -EINVAL;
	st = cur_store(con);
	n = (size_t)desc->width * desc->height;
	bytes = n * 4 + TSM_SCREEN_IMAGE_OVERHEAD;

	/* transmitting an id again replaces that image, with its placements,
	 * once the new one has its bytes; refused, the old one stays. It is
	 * held meanwhile, so settling cannot evict it (libtsm#37). */
	if (desc->kind == TSM_IMAGE_KITTY)
		old = store_find_id(st, desc->id);
	if (old)
		++old->hold;
	/* what pieces owe is asked for first, so what follows sees the room
	 * really left, and nothing it finds is evicted after */
	screen_images_settle(con);
	screen_inc_age(con);

	ret = old ? 0 : store_make_room(con, st);
	if (!ret)
		ret = budget_reserve(con, bytes, old, &added);
	if (old)
		--old->hold;
	if (ret)
		return ret;
	image = calloc(1, sizeof(*image));
	if (image)
		image->pixels = malloc(n * 4);
	if (!image || !image->pixels || slot_take(&con->img, image)) {
		if (image)
			free(image->pixels);
		free(image);
		budget_release(con, added);
		return -ENOMEM;
	}

	memcpy(image->pixels, desc->pixels, n * 4);
	image->opaque = pixels_opaque(image->pixels, n);
	image_store(con, image, desc, bytes, old, added);

	*out = image;
	return 0;
}

int screen_images_reserve(struct tsm_screen *con, size_t bytes)
{
	size_t added;

	screen_images_settle(con);
	return budget_reserve(con, bytes, NULL, &added);
}

void screen_images_unreserve(struct tsm_screen *con, size_t bytes)
{
	budget_release(con, bytes);
}

int screen_sixel_new(struct tsm_screen *con, unsigned int width, unsigned int height,
		     uint32_t *pixels, bool opaque, struct tsm_image **out)
{
	struct tsm_test_image desc = {
		.width = width,
		.height = height,
		.pixels = pixels,
		.kind = TSM_IMAGE_SIXEL,
	};
	struct tsm_image *image;
	size_t bytes = (size_t)width * height * 4, added;
	int ret;

	/* the decoder's limits keep it within these, unless they are set past
	 * them */
	if (!width || !height || width > TSM_SCREEN_IMAGE_MAX ||
	    height > TSM_SCREEN_IMAGE_MAX ||
	    (uint64_t)width * height * 4 > TSM_SCREEN_IMAGE_MAX_BYTES) {
		free(pixels);
		budget_release(con, bytes);
		return -EINVAL;
	}
	/* room in the store, and the bookkeeping's bytes beside the pixels' */
	screen_images_settle(con);
	ret = store_make_room(con, cur_store(con));
	if (!ret)
		ret = budget_reserve(con, TSM_SCREEN_IMAGE_OVERHEAD, NULL, &added);
	if (ret) {
		free(pixels);
		budget_release(con, bytes);
		return ret;
	}
	bytes += TSM_SCREEN_IMAGE_OVERHEAD;
	image = calloc(1, sizeof(*image));
	if (!image || slot_take(&con->img, image)) {
		free(image);
		free(pixels);
		budget_release(con, bytes);
		return -ENOMEM;
	}

	screen_inc_age(con);
	image->pixels = pixels;
	image->opaque = opaque || pixels_opaque(pixels, (size_t)width * height);
	image_store(con, image, &desc, bytes, NULL, bytes);

	*out = image;
	return 0;
}

void screen_image_drop(struct tsm_screen *con, struct tsm_image *image)
{
	image_free(con, image);
}

/*
 * Kitty images (the kitty graphics protocol, tsm-kitty.c)
 */

struct tsm_image *screen_kitty_find(struct tsm_screen *con, uint32_t id,
				    uint32_t number)
{
	struct tsm_image_store *st = cur_store(con);
	struct tsm_image *image, *newest = NULL;
	struct shl_dlist *iter;

	/* the scan below, or store_find_id's, counted against the input
	 * call's work as images_mark_pinned counts its own */
	con->img.work += st->count;
	if (id)
		return store_find_id(st, id);
	if (!number)
		return NULL;
	/* images are listed oldest first, a replacement as a new one */
	shl_dlist_for_each(iter, &st->images) {
		image = shl_dlist_entry(iter, struct tsm_image, list);
		if (image->kind == TSM_IMAGE_KITTY && image->number == number)
			newest = image;
	}
	return newest;
}

static int id_cmp(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

/*
 * The lowest id no image in the store in use has, for an image sent with a
 * number alone, as Ghostty's ImageStorage.nextImageId picks an explicit one
 * (src/terminal/kitty/graphics_storage.zig, MIT). 0 when memory runs out.
 */
uint32_t screen_kitty_free_id(struct tsm_screen *con)
{
	struct tsm_image_store *st = cur_store(con);
	struct tsm_image *image;
	struct shl_dlist *iter;
	uint32_t *ids, id = 1;
	unsigned int n = 0, i;

	if (!st->count)
		return 1;
	/* the scan and the sort */
	con->img.work += 2 * st->count;
	ids = malloc(st->count * sizeof(*ids));
	if (!ids)
		return 0;
	shl_dlist_for_each(iter, &st->images) {
		image = shl_dlist_entry(iter, struct tsm_image, list);
		if (image->kind == TSM_IMAGE_KITTY && image->id)
			ids[n++] = image->id;
	}
	qsort(ids, n, sizeof(*ids), id_cmp);
	for (i = 0; i < n && ids[i] <= id; ++i)
		if (ids[i] == id)
			++id;
	free(ids);
	return id;
}

/*
 * Reserves @bytes for a kitty image of @id. An image of that id in the store
 * in use is the one the new image will replace: its bytes count towards the
 * new one's, so only the difference is reserved, and it is held until
 * screen_kitty_new or screen_kitty_unreserve, so nothing evicts it meanwhile
 * (libtsm#37).
 */
int screen_kitty_reserve(struct tsm_screen *con, size_t bytes, uint32_t id,
			 struct tsm_kitty_reservation *r)
{
	struct tsm_image *old = store_find_id(cur_store(con), id);
	size_t added;
	int ret;

	r->bytes = 0;
	r->old = 0;
	if (old)
		++old->hold;
	screen_images_settle(con);
	ret = budget_reserve(con, bytes, old, &added);
	if (ret) {
		if (old)
			--old->hold;
		return ret;
	}
	r->bytes = added;
	r->old = old ? old->key : 0;
	return 0;
}

/* the image @r holds, still in the store in use, or NULL; held no more */
static struct tsm_image *reservation_release_old(struct tsm_screen *con,
						 struct tsm_kitty_reservation *r)
{
	struct tsm_image *old = r->old ? image_find(&con->img, r->old) : NULL;

	r->old = 0;
	if (!old)
		return NULL;
	--old->hold;
	return old->store == cur_store(con) ? old : NULL;
}

void screen_kitty_unreserve(struct tsm_screen *con, struct tsm_kitty_reservation *r)
{
	reservation_release_old(con, r);
	budget_release(con, r->bytes);
	r->bytes = 0;
}

int screen_kitty_new(struct tsm_screen *con, const struct tsm_test_image *desc,
		     uint32_t *pixels, bool opaque, struct tsm_kitty_reservation *r,
		     struct tsm_image **out)
{
	struct tsm_image_store *st = cur_store(con);
	struct tsm_image *image, *old;
	size_t bytes = (size_t)desc->width * desc->height * 4 + TSM_SCREEN_IMAGE_OVERHEAD;
	size_t added = 0, credit;
	int ret = 0;

	/* the image reserved against may have gone (a reset, the other
	 * screen) or been replaced; whichever has the id now is replaced */
	old = reservation_release_old(con, r);
	if (!old || old->id != desc->id)
		old = store_find_id(st, desc->id);
	if (old)
		++old->hold;
	if (r->bytes > bytes) {
		budget_release(con, r->bytes - bytes);
		r->bytes = bytes;
	}

	screen_images_settle(con);
	screen_inc_age(con);
	if (!old)
		ret = store_make_room(con, st);
	/* what is still missing: the reservation may have counted on an old
	 * image that has gone, or whose placements gave charges back */
	if (!ret && r->bytes < bytes)
		ret = budget_reserve(con, bytes - r->bytes, old, &added);
	image = ret ? NULL : calloc(1, sizeof(*image));
	if (!ret && (!image || slot_take(&con->img, image))) {
		free(image);
		budget_release(con, added);
		ret = -ENOMEM;
	}
	if (old)
		--old->hold;
	if (ret) {
		free(pixels);
		budget_release(con, r->bytes);
		r->bytes = 0;
		return ret;
	}

	/* image_store passes @old the bytes neither reservation granted */
	credit = r->bytes + added;
	r->bytes = 0;
	image->pixels = pixels;
	image->opaque = opaque;
	image_store(con, image, desc, bytes, old, credit);
	*out = image;
	return 0;
}

/* whether @p, at screen row p->row (store_rows), has a row on the screen */
static bool placement_on_screen(struct tsm_screen *con, const struct tsm_placement *p)
{
	return p->row != INT_MAX && p->row + (int)p->rows > 0 &&
	       p->row < (int)con->size_y;
}

static bool placement_covers(const struct tsm_placement *p, long long x, long long y)
{
	return p->row != INT_MAX && y >= p->row && y < (long long)p->row + p->rows &&
	       x >= p->col && x < (long long)p->col + p->cols;
}

/*
 * A kitty delete, as the spec's table says: lowercase removes placements,
 * uppercase also frees each image it touched that is left with no placement
 * anywhere. An image named by i, I or r is touched; with a placement id
 * given too, only when it lost that placement. Placements are chosen on the screen's own lines for a, c, p, q
 * and y, and in the whole store for i, n, r, x and z, as Ghostty's
 * ImageStorage.delete chooses them (src/terminal/kitty/graphics_storage.zig,
 * MIT). Sixel placements are never touched.
 */
void screen_kitty_delete(struct tsm_screen *con, const struct tsm_kitty_delete *d)
{
	struct tsm_image_store *st = cur_store(con);
	struct shl_dlist *iter, *tmp;
	struct tsm_placement *p;
	struct tsm_image *image;
	char what = d->what | 0x20;
	bool free_data = d->what >= 'A' && d->what <= 'Z';
	long long x = (long long)d->x - 1, y = (long long)d->y - 1;
	bool by_image = what == 'i' || what == 'n' || what == 'r', hit;
	/* with a placement id, an image is chosen by the placement it loses */
	bool by_placement = (what == 'i' || what == 'n') && d->placement;

	/*
	 * Every pass below counts against the input call's work, as the
	 * eviction scans do: the images twice, the placements twice (here and
	 * in store_rows) and the screen's lines once.
	 */
	con->img.work += 2 * st->count + 2 * st->nplacements + con->line_num;
	screen_inc_age(con);
	shl_dlist_for_each(iter, &st->images) {
		image = shl_dlist_entry(iter, struct tsm_image, list);
		image->doomed = false;
		image->chosen = false;
		if (image->kind != TSM_IMAGE_KITTY || !by_image)
			continue;
		if (what == 'i')
			image->chosen = d->id && image->id == d->id;
		else if (what == 'r')
			image->chosen = image->id && image->id >= d->x &&
					image->id <= d->y;
		image->doomed = image->chosen && !by_placement;
	}
	if (what == 'n') {
		image = screen_kitty_find(con, 0, d->number);
		if (image) {
			image->chosen = true;
			image->doomed = !by_placement;
		}
	}

	store_rows(con, st);
	shl_dlist_for_each_safe(iter, tmp, &st->placements) {
		p = shl_dlist_entry(iter, struct tsm_placement, store_link);
		if (p->image->kind != TSM_IMAGE_KITTY)
			continue;
		switch (what) {
		case 'a':
			hit = placement_on_screen(con, p);
			break;
		case 'i':
		case 'n':
			hit = p->image->chosen && (!d->placement || p->id == d->placement);
			break;
		case 'r':
			hit = p->image->chosen;
			break;
		case 'c':
			hit = placement_covers(p, d->cursor_x, d->cursor_y);
			break;
		case 'p':
			hit = placement_covers(p, x, y);
			break;
		case 'q':
			hit = placement_covers(p, x, y) && p->z == d->z;
			break;
		case 'x':
			hit = d->x && x >= p->col && x < (long long)p->col + p->cols;
			break;
		case 'y':
			hit = d->y && placement_on_screen(con, p) &&
			      placement_covers(p, p->col, y);
			break;
		case 'z':
			hit = p->z == d->z;
			break;
		default:
			hit = false;
			break;
		}
		if (!hit)
			continue;
		p->image->doomed = true;
		placement_free(con, p, false);
	}

	if (!free_data)
		return;
	shl_dlist_for_each_safe(iter, tmp, &st->images) {
		image = shl_dlist_entry(iter, struct tsm_image, list);
		if (image->doomed && !image->placements)
			image_free(con, image);
	}
}

static unsigned int div_up(unsigned long long a, unsigned int b)
{
	return (a + b - 1) / b;
}

int screen_image_place(struct tsm_screen *con, struct tsm_image *image,
		       const struct tsm_test_placement *pl, struct tsm_placement **out)
{
	struct tsm_image_store *st;
	struct tsm_placement *p = NULL;
	struct shl_dlist *iter;
	unsigned int cw, ch, sw, sh, cols, rows, r, last;
	unsigned long long bw, bh, dw, dh;
	int ox, oy, ret;

	st = cur_store(con);
	if (!image || image->store != st)
		return -ENOENT;
	cw = con->img.cell_width;
	ch = con->img.cell_height;
	if (pl->col >= con->size_x || pl->row >= con->size_y || pl->x >= cw ||
	    pl->y >= ch || pl->src_x >= image->width || pl->src_y >= image->height ||
	    pl->cols > TSM_SCREEN_IMAGE_MAX_CELLS || pl->rows > TSM_SCREEN_IMAGE_MAX_CELLS)
		return -EINVAL;

	/* the source rectangle, cut to the image */
	sw = image->width - pl->src_x;
	if (pl->src_width && pl->src_width < sw)
		sw = pl->src_width;
	sh = image->height - pl->src_y;
	if (pl->src_height && pl->src_height < sh)
		sh = pl->src_height;

	/* the box, and the picture in it */
	ox = pl->x;
	oy = pl->y;
	if (pl->cols && pl->rows) {
		/* fitted and centred in the box, less the offset */
		cols = pl->cols;
		rows = pl->rows;
		bw = (unsigned long long)cols * cw - ox;
		bh = (unsigned long long)rows * ch - oy;
		if (bw * sh <= bh * sw) {
			dw = bw;
			dh = (bw * sh + sw / 2) / sw;
		} else {
			dh = bh;
			dw = (bh * sw + sh / 2) / sh;
		}
		ox += (bw - dw) / 2;
		oy += (bh - dh) / 2;
	} else if (pl->cols) {
		cols = pl->cols;
		dw = (unsigned long long)cols * cw;
		dh = (dw * sh + sw / 2) / sw;
		rows = 0;
	} else if (pl->rows) {
		rows = pl->rows;
		dh = (unsigned long long)rows * ch;
		dw = (dh * sw + sh / 2) / sh;
		cols = 0;
	} else {
		dw = sw;
		dh = sh;
		cols = 0;
		rows = 0;
	}
	/* a picture scaled to a sliver keeps a pixel; one scaled past the
	 * largest box is refused, so every pixel quantity of a placement and
	 * its pieces fits an int */
	if (!dw)
		dw = 1;
	if (!dh)
		dh = 1;
	if (dw > (unsigned long long)TSM_SCREEN_IMAGE_MAX_CELLS * cw ||
	    dh > (unsigned long long)TSM_SCREEN_IMAGE_MAX_CELLS * ch)
		return -EINVAL;
	if (!cols)
		cols = div_up(ox + dw, cw);
	if (!rows)
		rows = div_up(oy + dh, ch);

	screen_inc_age(con);
	/* a kitty placement id names a placement of its image to replace */
	if (image->kind == TSM_IMAGE_KITTY && pl->id) {
		con->img.work += st->nplacements;
		shl_dlist_for_each(iter, &st->placements) {
			p = shl_dlist_entry(iter, struct tsm_placement, store_link);
			if (p->image == image && p->id == pl->id)
				break;
			p = NULL;
		}
	}

	/* a new placement needs room in the store, and past the image's
	 * first, its charge; the image is held so neither evicts it */
	if (!p) {
		++image->hold;
		ret = placement_make_room(con, st, image);
		--image->hold;
		if (ret)
			return ret;
	}

	/* an opaque sixel covers older sixels as text written there would */
	if (image->kind == TSM_IMAGE_SIXEL && image->opaque) {
		last = pl->row + rows < con->size_y ? pl->row + rows : con->size_y;
		for (r = pl->row; r < last; ++r)
			screen_images_cut(con, r, pl->col, pl->col + cols - 1);
	}

	if (!p) {
		p = placement_new(con, image);
		if (!p) {
			if (image->placements) {
				image->bytes -= TSM_SCREEN_PLACEMENT_OVERHEAD;
				budget_release(con, TSM_SCREEN_PLACEMENT_OVERHEAD);
			}
			return -ENOMEM;
		}
		p->seq = ++con->img.last_seq;
		p->id = pl->id;
	}
	p->col = pl->col;
	p->cols = cols;
	p->rows = rows;
	p->ox = ox;
	p->oy = oy;
	p->dw = dw;
	p->dh = dh;
	p->sx = pl->src_x;
	p->sy = pl->src_y;
	p->sw = sw;
	p->sh = sh;
	p->cw = cw;
	p->ch = ch;
	p->z = image->kind == TSM_IMAGE_SIXEL ? 0 : pl->z;
	p->age = con->age_cnt;
	placement_attach(p, con->lines[pl->row]);
	image->last_use = ++image_clock;

	*out = p;
	return 0;
}
