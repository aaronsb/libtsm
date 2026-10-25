/*
 * fuzz-vte-sixel - fuzz target for sixel through the vte
 *
 * Sixel strings reach the decoder through the vte, and their pictures the
 * screen's store and placements, all in kmscon's root process, on bytes any
 * program sends (ADR-103, Testing). This target feeds whole terminal streams
 * to a vte with sixel on, so the parser, the decoder, the budget, placement,
 * cursor movement and every screen event that cuts or moves a picture run
 * together. Crashes, memory errors and undefined behaviour abort, and so do
 * broken invariants:
 *
 *  - the host budget is never given back more than it granted, and holds
 *    exactly the bytes the screen reports after every input call;
 *  - tsm_vte_input_bounded consumes at least one byte and at most what it
 *    was given;
 *  - with mode 2026 offered, each bounded call ends at most one hold, and
 *    after one that ended a hold the mode reads 2 (the call stopped at the
 *    end); with it not offered, no hold ever ends;
 *  - every placement a frame lists names an image the screen still has;
 *  - no store holds more than TSM_SCREEN_IMAGES_PER_STORE images or
 *    TSM_SCREEN_PLACEMENTS_PER_STORE placements, and every image pays its
 *    TSM_SCREEN_IMAGE_OVERHEAD;
 *  - freeing the vte and the screen gives every byte back.
 *
 * Input: byte 0 picks the screen size, cell size, scrollback and budget,
 * byte 1 the max-graphic-size, the register settings, how the stream is fed
 * (whole, or in chunks through tsm_vte_input_bounded), whether mode 2026 is
 * offered (bit 6) and whether the host ends a hold every third bounded call
 * (bit 7, tsm_vte_end_sync); the rest is the stream.
 *
 * Built two ways, as fuzz-sixel is: with libFuzzer (meson -Dfuzz=true under
 * clang, make fuzz FUZZ_TARGET=vte-sixel), and with -DFUZZ_REPLAY as a
 * program that replays the files and directories named on its command line,
 * which the test suite uses for the seed corpus, vte-sixel-seeds/.
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

static void write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	(void)vte;
	(void)u8;
	(void)len;
	(void)data;
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
	uint64_t frames;
	unsigned int calls;
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
	tsm_vte_set_sixel(vte, true);
	check(tsm_vte_set_graphic_max_size(vte, graphic[b & 3].w,
					   graphic[b & 3].h) == 0);
	check(tsm_vte_set_sixel_registers(vte, b & 4 ? 2 : 0, !(b & 8)) == 0);
	if (b & 0x40)
		tsm_vte_set_sync(vte, true);

	chunk = chunks[b >> 4 & 3];
	if (!chunk) {
		tsm_vte_input(vte, (const char *)data, size);
		check_frame(screen);
	} else {
		for (pos = 0, calls = 0; pos < size; pos += used, ++calls) {
			n = size - pos < chunk ? size - pos : chunk;
			frames = tsm_vte_get_sync_frames(vte);
			used = tsm_vte_input_bounded(vte, (const char *)data + pos, n);
			check(used >= 1 && used <= n);
			check(tsm_screen_get_image_bytes(screen) == held);
			/* at most one end per bounded call, and the call stopped
			 * there, so no later hold has begun */
			check(tsm_vte_get_sync_frames(vte) - frames <= 1);
			if (tsm_vte_get_sync_frames(vte) != frames)
				check(tsm_vte_get_mode(vte, true, 2026) == 2);
			if ((b & 0x80) && calls % 3 == 2)
				tsm_vte_end_sync(vte);
		}
		check_frame(screen);
	}
	/* not offered, the mode never holds, so nothing ever ends */
	if (!(b & 0x40))
		check(tsm_vte_get_sync_frames(vte) == 0);

	/* a picture still streaming gives its bytes back with the vte */
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
