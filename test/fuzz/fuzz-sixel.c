/*
 * fuzz-sixel - fuzz target for the sixel decoder
 *
 * The decoder runs in kmscon's root process on bytes any program sends
 * (ADR-103, Testing), so it is fuzzed for crashes, memory errors, undefined
 * behaviour, and broken invariants, which abort:
 *
 *  - every reservation the decoder asks for or gives back matches what it
 *    holds; a finished picture carries exactly width * height * 4 bytes of
 *    it, and nothing is held after an error, an abort, or free;
 *  - a picture is non-empty and within the size limits, and each pixel is
 *    opaque or fully transparent, opaque throughout unless P2 = 1.
 *
 * Input: byte 0 picks P1, byte 1 P2, byte 2 the limits, the chunk size the
 * payload is fed in and whether registers are shared; the rest is payload,
 * where ESC ends the picture (ST) and starts another, and CAN aborts it and
 * starts another.
 *
 * Built two ways: with libFuzzer (meson -Dfuzz=true under clang, make fuzz),
 * and with -DFUZZ_REPLAY as a program that runs the files and directories
 * named on its command line, which the test suite uses to replay the seed
 * corpus, sixel-seeds/ (gen-seeds.py).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tsm-sixel.h"

#define check(x) do { if (!(x)) { \
	fprintf(stderr, "%s:%d: invariant failed: %s\n", __FILE__, \
		__LINE__, #x); abort(); } } while (0)

/* Bytes the decoder holds, as the reserve callback saw it. */
static size_t held;
/* The budget, small enough to be reached often, and to keep runs fast */
#define BUDGET ((size_t)1 << 20)

static bool reserve(void *data, size_t old_size, size_t new_size)
{
	(void)data;
	check(old_size == held);
	if (new_size > old_size && new_size > BUDGET)
		return false;
	held = new_size;
	return true;
}

static const struct { unsigned int w, h; } limits[4] = {
	{ 0, 0 }, { 64, 64 }, { 1, 1 }, { 300, 7 },
};
static const size_t chunks[8] = { 1, 2, 3, 7, 64, 4096, 65536, (size_t)-1 };
static const unsigned int registers[2] = { 0, 2 };

/* Coverage of the per-pixel scan tells the fuzzer nothing and costs most of
 * its time. */
#if defined(__clang__) && __clang_major__ >= 13
__attribute__((no_sanitize("coverage")))
#endif
static void check_pixels(const struct tsm_sixel_image *img)
{
	size_t i, n = (size_t)img->width * img->height;
	uint32_t a;

	for (i = 0; i < n; ++i) {
		a = img->pixels[i] >> 24;
		check(a == 0xff ||
		      (a == 0 && img->pixels[i] == 0 && img->transparent));
	}
}

static void finish(struct tsm_sixel *s, unsigned int max_w, unsigned int max_h)
{
	struct tsm_sixel_image img;
	int ret;

	ret = tsm_sixel_finish(s, &img);
	if (ret) {
		check(held == 0);
		return;
	}

	check(img.width > 0 && img.height > 0 && img.pixels);
	check(img.width <= max_w && img.height <= max_h);
	check(held == (size_t)img.width * img.height * 4);
	check_pixels(&img);
	free(img.pixels);
	held = 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct tsm_sixel_limits lim = { .reserve = reserve };
	struct tsm_sixel_palette *shared = NULL;
	struct tsm_sixel *s;
	unsigned int max_w, max_h;
	size_t chunk, i, start;
	int p1, p2;

	if (size < 3)
		return 0;

	p1 = data[0] % 12 - 1;
	p2 = data[1] % 4 - 1;
	lim.max_width = limits[data[2] & 3].w;
	lim.max_height = limits[data[2] & 3].h;
	lim.registers = registers[data[2] >> 6 & 1];
	chunk = chunks[data[2] >> 2 & 7];
	max_w = lim.max_width ? lim.max_width : TSM_SIXEL_MAX_SIZE_DEFAULT;
	max_h = lim.max_height ? lim.max_height : TSM_SIXEL_MAX_SIZE_DEFAULT;
	data += 3;
	size -= 3;

	held = 0;
	check(tsm_sixel_new(&s, &lim) == 0);
	if (data[-1] & 0x80)
		check(tsm_sixel_palette_new(&shared, 16) == 0);

	tsm_sixel_begin(s, p1, p2, shared);
	start = 0;
	for (i = 0; i <= size; ++i) {
		/* feed what precedes each control byte, in chunks */
		if (i < size && data[i] != 0x1b && data[i] != 0x18)
			continue;
		while (start < i) {
			size_t n = i - start < chunk ? i - start : chunk;

			tsm_sixel_feed(s, (const char *)data + start, n);
			start += n;
		}
		start = i + 1;
		if (i == size)
			break;
		if (data[i] == 0x1b) {
			finish(s, max_w, max_h);
		} else {
			tsm_sixel_abort(s);
			check(held == 0);
		}
		tsm_sixel_begin(s, p1, p2, shared);
	}
	finish(s, max_w, max_h);

	/* freeing mid-picture gives everything back */
	tsm_sixel_begin(s, p1, p2, shared);
	tsm_sixel_feed(s, (const char *)data, size);
	tsm_sixel_free(s);
	check(held == 0);
	tsm_sixel_palette_free(shared);
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
