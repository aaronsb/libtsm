/*
 * bench-sixel - sixel decode throughput
 *
 * Decodes one sixel picture repeatedly and prints the payload throughput in
 * MB/s and the frames per second. mpv --vo=sixel streams one picture per
 * frame, so a full screen at 1920x1080 is the case this measures.
 *
 *   bench-sixel [FILE.six] [SECONDS]
 *
 * FILE.six is a recorded DCS string (ESC P ... q payload ESC \), such as
 * img2sixel or test/sixel/sixel-encode.c write. Without one, the bench builds
 * a 1920x1080 frame of the same shape libsixel writes: 256 registers, and per
 * band one pass per colour, run-length coded, each pass ended by '$'. The
 * picture is a gradient with noise, so runs stay short, as in video.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tsm-sixel.h"

struct buf {
	char *data;
	size_t len, cap;
};

__attribute__((format(printf, 2, 3)))
static void put(struct buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	for (;;) {
		va_start(ap, fmt);
		n = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
		va_end(ap);
		if (n >= 0 && (size_t)n < b->cap - b->len)
			break;
		b->cap = b->cap ? b->cap * 2 : 1 << 20;
		b->data = realloc(b->data, b->cap);
		if (!b->data)
			abort();
	}
	b->len += n;
}

/* Emits @count copies of sixel @c, as libsixel does: '!' from four up. */
static void run(struct buf *b, int c, unsigned int count)
{
	if (count >= 4)
		put(b, "!%u%c", count, c);
	else
		while (count--)
			put(b, "%c", c);
}

static struct buf synthesize(unsigned int w, unsigned int h)
{
	struct buf b = { 0 };
	unsigned char *idx = malloc((size_t)w * h);
	unsigned int x, y, k, c, band, r, g, bl, seed = 1;
	unsigned char used[256];

	/* a 6x7x6 colour cube, 252 registers */
	for (y = 0; y < h; ++y) {
		for (x = 0; x < w; ++x) {
			seed = seed * 1103515245 + 12345;
			r = (x * 6 / w + (seed >> 16) % 2) % 6;
			g = (y * 7 / h + (seed >> 18) % 2) % 7;
			bl = ((x + y) * 6 / (w + h) + (seed >> 20) % 2) % 6;
			idx[(size_t)y * w + x] = r * 42 + g * 6 + bl;
		}
	}

	put(&b, "\033Pq\"1;1;%u;%u", w, h);
	for (c = 0; c < 252; ++c)
		put(&b, "#%u;2;%u;%u;%u", c, c / 42 * 20, c / 6 % 7 * 100 / 6,
		    c % 6 * 20);

	for (band = 0; band < h; band += 6) {
		memset(used, 0, sizeof(used));
		for (y = band; y < band + 6 && y < h; ++y)
			for (x = 0; x < w; ++x)
				used[idx[(size_t)y * w + x]] = 1;
		for (c = 0; c < 252; ++c) {
			int last = -1;
			unsigned int count = 0;

			if (!used[c])
				continue;
			put(&b, "#%u", c);
			for (x = 0; x < w; ++x) {
				int six = 0;

				for (k = 0; k < 6 && band + k < h; ++k)
					if (idx[(size_t)(band + k) * w + x] == c)
						six |= 1 << k;
				six += '?';
				if (six == last) {
					++count;
					continue;
				}
				if (count)
					run(&b, last, count);
				last = six;
				count = 1;
			}
			/* trailing empty sixels are left out */
			if (last != '?')
				run(&b, last, count);
			put(&b, "$");
		}
		put(&b, "-");
	}
	put(&b, "\033\\");
	free(idx);
	return b;
}

static struct buf load(const char *path)
{
	struct buf b = { 0 };
	FILE *f = fopen(path, "rb");
	size_t n;

	if (!f) {
		perror(path);
		exit(1);
	}
	b.cap = 1 << 20;
	b.data = malloc(b.cap);
	while ((n = fread(b.data + b.len, 1, b.cap - b.len, f)) > 0) {
		b.len += n;
		if (b.len == b.cap)
			b.data = realloc(b.data, b.cap *= 2);
	}
	fclose(f);
	return b;
}

/* The bench measures decoding; its budget admits everything. */
static bool reserve(void *data, size_t old_size, size_t new_size)
{
	(void)data;
	(void)old_size;
	(void)new_size;
	return true;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	struct buf b = argc > 1 ? load(argv[1]) : synthesize(1920, 1080);
	double secs = argc > 2 ? atof(argv[2]) : 3, start, t, fin = 0;
	const char *payload, *end;
	struct tsm_sixel_image img;
	struct tsm_sixel *s;
	unsigned long frames = 0;
	size_t len;

	payload = memchr(b.data, 'q', b.len);
	if (!payload || b.len < 4 || b.data[0] != '\033') {
		fprintf(stderr, "not a DCS sixel string\n");
		return 1;
	}
	++payload;
	end = b.data + b.len;
	if (end[-2] == '\033' && end[-1] == '\\')
		end -= 2;
	len = end - payload;

	struct tsm_sixel_limits lim = { .reserve = reserve };

	if (tsm_sixel_new(&s, &lim))
		return 1;

	start = now();
	do {
		tsm_sixel_begin(s, -1, -1, NULL);
		tsm_sixel_feed(s, payload, len);
		t = now();
		if (tsm_sixel_finish(s, &img)) {
			fprintf(stderr, "decode failed\n");
			return 1;
		}
		fin += now() - t;
		free(img.pixels);
		++frames;
	} while (now() - start < secs);
	t = now() - start;

	printf("%ux%u, %zu payload bytes, %lu frames in %.2fs: "
	       "%.1f MB/s, %.1f frames/s, %.2f ms/frame (%.2f ms at ST)\n",
	       img.width, img.height, len, frames, t,
	       len * frames / t / 1e6, frames / t, t * 1e3 / frames,
	       fin * 1e3 / frames);

	tsm_sixel_free(s);
	free(b.data);
	return 0;
}
