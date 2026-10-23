/*
 * libtsm - Sixel Decoder
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
 * Sixel Decoder
 * Written from DEC STD 070 and the VT330/VT340 Programmer Reference Manual
 * (sixel graphics, the default colour map, HLS with blue at 0 degrees) and
 * xterm's ctlseqs, as ADR-103 (aaronsb/kmscon) and ADR-300 require. foot's
 * sixel.c (MIT, Daniel Eklöf) was read as a reference, and two rules follow
 * its behaviour: a pixel takes its register's colour when it is drawn, unset
 * pixels taking register 0 at the end unless P2 = 1; and rows of the last
 * band past its lowest set bit are not part of the picture. No code was taken
 * from it.
 *
 * The picture is drawn straight into ARGB32, 0 meaning no sixel set the
 * pixel (every register colour is opaque). Its rows are cap_w pixels apart.
 * Raster attributes size it, and reserve its memory, before any sixel; the
 * first sixel allocates it, and it grows as sixels land outside it. One
 * sixel dot is one pixel; the pixel aspect of P1 and of raster attributes is
 * not applied (see tsm-sixel.h).
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "tsm-sixel.h"

/* Parameters saturate here; every use clamps far below it. */
#define SIXEL_PARAM_MAX (0xffffffu / 10)
#define SIXEL_PARAMS 5

struct tsm_sixel_palette {
	unsigned int count;
	uint32_t colors[];
};

enum sixel_state {
	SIXEL_IDLE,		/* no image in progress */
	SIXEL_DATA,		/* sixel data and single-byte controls */
	SIXEL_REPEAT,		/* ! Pn */
	SIXEL_COLOR,		/* # Pc ; Pu ; Px ; Py ; Pz */
	SIXEL_RASTER,		/* " Pan ; Pad ; Ph ; Pv */
	SIXEL_FAILED,		/* memory refused; the rest is ignored */
};

struct tsm_sixel {
	unsigned int max_w;
	unsigned int max_h;
	tsm_sixel_reserve_cb reserve;
	void *reserve_data;
	struct tsm_sixel_palette *own;	/* private registers */
	struct tsm_sixel_palette *pal;	/* registers of the image in progress */

	enum sixel_state state;
	unsigned int params[SIXEL_PARAMS];
	unsigned int nparams;	/* SIXEL_PARAMS + 1 once extras arrive */

	bool transparent;
	bool data_seen;		/* raster attributes count only before data */
	unsigned int ra_w;	/* raster extent in output pixels, clamped */
	unsigned int ra_h;

	uint32_t color;		/* the current register's colour */
	unsigned int x;		/* graphics cursor, at most max_w */
	unsigned int y;		/* top of the current band, at most max_h */
	unsigned int width;	/* furthest column reached by data */
	unsigned int height;	/* one past the lowest row a sixel set */

	uint32_t *buf;		/* NULL until the first sixel */
	unsigned int cap_w;
	unsigned int cap_h;
	size_t reserved;	/* bytes the reserve callback granted */

	uint64_t work;		/* bytes read and pixels touched, ever */
};

/*
 * The VT340's default colour map (VT330/VT340 Programmer Reference Manual,
 * sixel colour map), as RGB percentages.
 */
static const uint8_t vt340_colors[16][3] = {
	{  0,  0,  0 },		/* black */
	{ 20, 20, 80 },		/* blue */
	{ 80, 13, 13 },		/* red */
	{ 20, 80, 20 },		/* green */
	{ 80, 20, 80 },		/* magenta */
	{ 20, 80, 80 },		/* cyan */
	{ 80, 80, 20 },		/* yellow */
	{ 53, 53, 53 },		/* gray 50% */
	{ 26, 26, 26 },		/* gray 25% */
	{ 33, 33, 60 },		/* blue, less saturated */
	{ 60, 26, 26 },		/* red, less saturated */
	{ 33, 60, 33 },		/* green, less saturated */
	{ 60, 33, 60 },		/* magenta, less saturated */
	{ 33, 60, 60 },		/* cyan, less saturated */
	{ 60, 60, 33 },		/* yellow, less saturated */
	{ 80, 80, 80 },		/* gray 75% */
};

static unsigned int pct_to_8bit(unsigned int pct)
{
	if (pct > 100)
		pct = 100;
	return (pct * 255 + 50) / 100;
}

static uint32_t argb(unsigned int r, unsigned int g, unsigned int b)
{
	return 0xff000000u | r << 16 | g << 8 | b;
}

static uint32_t rgb_pct(unsigned int r, unsigned int g, unsigned int b)
{
	return argb(pct_to_8bit(r), pct_to_8bit(g), pct_to_8bit(b));
}

static unsigned int unit_to_8bit(double v)
{
	if (v <= 0)
		return 0;
	if (v >= 1)
		return 255;
	return (unsigned int)(v * 255 + 0.5);
}

/*
 * HLS as DEC defines it: hue in degrees with blue at 0, red at 120 and green
 * at 240; lightness and saturation in percent. Rotating the hue by 240
 * degrees gives the usual HLS, red at 0.
 */
static uint32_t hls_pct(unsigned int h, unsigned int l, unsigned int s)
{
	double hue, light, sat, c, x, m, r, g, b, f;
	unsigned int sector;

	if (h > 360)
		h = 360;
	if (l > 100)
		l = 100;
	if (s > 100)
		s = 100;

	hue = ((h + 240) % 360) / 60.0;
	light = l / 100.0;
	sat = s / 100.0;

	c = (1 - (light > 0.5 ? 2 * light - 1 : 1 - 2 * light)) * sat;
	sector = (unsigned int)hue;
	f = hue - sector;
	x = c * ((sector & 1) ? 1 - f : f);
	m = light - c / 2;

	switch (sector) {
	case 0: r = c; g = x; b = 0; break;
	case 1: r = x; g = c; b = 0; break;
	case 2: r = 0; g = c; b = x; break;
	case 3: r = 0; g = x; b = c; break;
	case 4: r = x; g = 0; b = c; break;
	default: r = c; g = 0; b = x; break;
	}

	return argb(unit_to_8bit(r + m), unit_to_8bit(g + m), unit_to_8bit(b + m));
}

static unsigned int clamp_registers(unsigned int registers)
{
	if (!registers)
		return TSM_SIXEL_REGISTERS_DEFAULT;
	if (registers < TSM_SIXEL_REGISTERS_MIN)
		return TSM_SIXEL_REGISTERS_MIN;
	if (registers > TSM_SIXEL_REGISTERS_MAX)
		return TSM_SIXEL_REGISTERS_MAX;
	return registers;
}

int tsm_sixel_palette_new(struct tsm_sixel_palette **out, unsigned int registers)
{
	struct tsm_sixel_palette *pal;

	if (!out)
		return -EINVAL;

	registers = clamp_registers(registers);
	pal = malloc(sizeof(*pal) + registers * sizeof(pal->colors[0]));
	if (!pal)
		return -ENOMEM;

	pal->count = registers;
	tsm_sixel_palette_reset(pal);
	*out = pal;
	return 0;
}

void tsm_sixel_palette_free(struct tsm_sixel_palette *pal)
{
	free(pal);
}

void tsm_sixel_palette_reset(struct tsm_sixel_palette *pal)
{
	unsigned int i;

	if (!pal)
		return;

	for (i = 0; i < pal->count; ++i) {
		if (i < 16)
			pal->colors[i] = rgb_pct(vt340_colors[i][0],
						 vt340_colors[i][1],
						 vt340_colors[i][2]);
		else
			pal->colors[i] = argb(0, 0, 0);
	}
}

static unsigned int clamp_size(unsigned int size)
{
	if (!size)
		return TSM_SIXEL_MAX_SIZE_DEFAULT;
	if (size > TSM_SIXEL_MAX_SIZE_LIMIT)
		return TSM_SIXEL_MAX_SIZE_LIMIT;
	return size;
}

int tsm_sixel_new(struct tsm_sixel **out, const struct tsm_sixel_limits *limits)
{
	struct tsm_sixel *sixel;
	int ret;

	/* every picture is paid for from a budget */
	if (!out || !limits || !limits->reserve)
		return -EINVAL;

	sixel = calloc(1, sizeof(*sixel));
	if (!sixel)
		return -ENOMEM;

	sixel->max_w = clamp_size(limits->max_width);
	sixel->max_h = clamp_size(limits->max_height);
	sixel->reserve = limits->reserve;
	sixel->reserve_data = limits->reserve_data;

	ret = tsm_sixel_palette_new(&sixel->own, limits->registers);
	if (ret) {
		free(sixel);
		return ret;
	}

	sixel->state = SIXEL_IDLE;
	*out = sixel;
	return 0;
}

static void release(struct tsm_sixel *sixel)
{
	free(sixel->buf);
	sixel->buf = NULL;
	sixel->cap_w = 0;
	sixel->cap_h = 0;
	if (sixel->reserved)
		sixel->reserve(sixel->reserve_data, sixel->reserved, 0);
	sixel->reserved = 0;
}

void tsm_sixel_free(struct tsm_sixel *sixel)
{
	if (!sixel)
		return;

	release(sixel);
	tsm_sixel_palette_free(sixel->own);
	free(sixel);
}

/* Asks the reserve callback to move the reservation to @size bytes. */
static bool reserve_bytes(struct tsm_sixel *sixel, size_t size)
{
	if (!sixel->reserve(sixel->reserve_data, sixel->reserved, size) &&
	    size > sixel->reserved)
		return false;

	sixel->reserved = size;
	return true;
}

static void fail(struct tsm_sixel *sixel)
{
	release(sixel);
	sixel->state = SIXEL_FAILED;
}

void tsm_sixel_begin(struct tsm_sixel *sixel, int p1, int p2,
		     struct tsm_sixel_palette *shared)
{
	(void)p1;

	if (!sixel)
		return;

	release(sixel);

	sixel->transparent = p2 == 1;
	if (shared) {
		sixel->pal = shared;
	} else {
		sixel->pal = sixel->own;
		tsm_sixel_palette_reset(sixel->own);
	}

	sixel->state = SIXEL_DATA;
	sixel->nparams = 0;
	sixel->data_seen = false;
	sixel->ra_w = 0;
	sixel->ra_h = 0;
	sixel->color = sixel->pal->colors[0];
	sixel->x = 0;
	sixel->y = 0;
	sixel->width = 0;
	sixel->height = 0;
}

void tsm_sixel_abort(struct tsm_sixel *sixel)
{
	if (!sixel)
		return;

	release(sixel);
	sixel->state = SIXEL_IDLE;
}

static unsigned int min_u(unsigned int a, unsigned int b)
{
	return a < b ? a : b;
}

static unsigned int max_u(unsigned int a, unsigned int b)
{
	return a > b ? a : b;
}

/*
 * Moves the picture into a buffer of @w x @h pixels, both at least the current
 * size, keeping what is drawn. Both buffers are reserved while the rows move
 * across, so memory never exceeds the reservation; the first buffer takes
 * what raster attributes reserved. Returns false when the memory is refused
 * or unavailable.
 */
static bool resize(struct tsm_sixel *sixel, unsigned int w, unsigned int h)
{
	uint32_t *buf;
	size_t size, old;
	unsigned int row;

	if ((size_t)w > SIZE_MAX / 2 / sizeof(*buf) / (h ? h : 1))
		return false;
	size = (size_t)w * h * sizeof(*buf);
	old = (size_t)sixel->cap_w * sixel->cap_h * sizeof(*buf);
	if (old + size > sixel->reserved && !reserve_bytes(sixel, old + size))
		return false;

	buf = calloc((size_t)w * h, sizeof(*buf));
	if (!buf)
		return false;
	/* rows at and below height hold nothing yet */
	for (row = 0; row < sixel->height; ++row)
		memcpy(buf + (size_t)row * w,
		       sixel->buf + (size_t)row * sixel->cap_w,
		       sixel->cap_w * sizeof(*buf));
	free(sixel->buf);
	sixel->work += (uint64_t)w * h + (uint64_t)sixel->height * sixel->cap_w;

	sixel->buf = buf;
	sixel->cap_w = w;
	sixel->cap_h = h;
	reserve_bytes(sixel, size);
	return true;
}

/*
 * Grows the picture to hold columns below @need_w and rows below @need_h. Each
 * dimension grows by at least a quarter, so a picture that outgrows its
 * raster attributes, or has none, is copied O(log size) times.
 */
static bool grow(struct tsm_sixel *sixel, unsigned int need_w,
		 unsigned int need_h)
{
	/* the first buffer is at least the raster area */
	unsigned int w = sixel->buf ? sixel->cap_w : sixel->ra_w;
	unsigned int h = sixel->buf ? sixel->cap_h : sixel->ra_h;

	if (need_w > w)
		w = max_u(need_w, min_u(w + w / 4, sixel->max_w));
	if (need_h > h)
		h = max_u(need_h, min_u(h + h / 4, sixel->max_h));

	return resize(sixel, max_u(w, need_w), max_u(h, need_h));
}

/*
 * Draws sixel @bits (bit 0 on top) @count times at the graphics cursor with
 * the current colour, and moves the cursor past them.
 */
static void put(struct tsm_sixel *sixel, unsigned int bits, unsigned int count)
{
	unsigned int x0 = sixel->x, x1, end, bit, row, col, stride;
	uint32_t color, *line;

	sixel->data_seen = true;
	if (sixel->y >= sixel->max_h)
		return;

	x1 = x0 + min_u(count, sixel->max_w);
	if (x1 > sixel->max_w)
		x1 = sixel->max_w;
	sixel->x = x1;
	if (x1 > sixel->width)
		sixel->width = x1;
	if (!bits || x0 >= x1)
		return;

	/* one past the lowest row the top bit set reaches */
	end = 32 - __builtin_clz(bits);
	end = min_u(sixel->y + end, sixel->max_h);
	if ((x1 > sixel->cap_w || end > sixel->cap_h) && !grow(sixel, x1, end)) {
		fail(sixel);
		return;
	}
	if (end > sixel->height)
		sixel->height = end;

	color = sixel->color;
	stride = sixel->cap_w;
	for (bit = 0; bits; ++bit, bits >>= 1) {
		if (!(bits & 1))
			continue;
		row = sixel->y + bit;
		if (row >= end)
			break;
		line = sixel->buf + (size_t)row * stride;
		for (col = x0; col < x1; ++col)
			line[col] = color;
		sixel->work += x1 - x0;
	}
}

/* Sets *@p to @color if bit 0 of @bit is set. */
static inline void put_masked(uint32_t *p, unsigned int bit, uint32_t color)
{
	uint32_t m = -(uint32_t)(bit & 1);

	*p = (*p & ~m) | (color & m);
}

/*
 * put() for one sixel inside the grid, the bulk of every encoder's
 * output. Returns false, having done nothing, for put() to handle it.
 */
static inline bool put_one(struct tsm_sixel *sixel, unsigned int bits)
{
	unsigned int x = sixel->x, y = sixel->y, stride = sixel->cap_w, end;
	uint32_t color, *p;

	if (x >= stride || y + 6 > sixel->cap_h)
		return false;

	sixel->data_seen = true;
	sixel->x = x + 1;
	if (x + 1 > sixel->width)
		sixel->width = x + 1;
	if (!bits)
		return true;

	end = y + 32 - __builtin_clz(bits);
	if (end > sixel->height)
		sixel->height = end;

	/* all six rows, masked: cheaper than a branch per bit */
	sixel->work += 6;
	color = sixel->color;
	p = sixel->buf + (size_t)y * stride + x;
	put_masked(p, bits, color);
	put_masked(p + stride, bits >> 1, color);
	put_masked(p + 2 * stride, bits >> 2, color);
	put_masked(p + 3 * stride, bits >> 3, color);
	put_masked(p + 4 * stride, bits >> 4, color);
	put_masked(p + 5 * stride, bits >> 5, color);
	return true;
}

/* Acts on the parameters collected for @state, which a byte just ended. */
static void end_params(struct tsm_sixel *sixel, enum sixel_state state)
{
	unsigned int *p = sixel->params, n = sixel->nparams, reg;
	struct tsm_sixel_palette *pal = sixel->pal;

	if (state == SIXEL_COLOR) {
		reg = min_u(p[0], pal->count - 1);
		if (n >= 5 && p[1] == 1)
			pal->colors[reg] = hls_pct(p[2], p[3], p[4]);
		else if (n >= 5 && p[1] == 2)
			pal->colors[reg] = rgb_pct(p[2], p[3], p[4]);
		sixel->color = pal->colors[reg];
	} else if (state == SIXEL_RASTER && !sixel->data_seen) {
		/* Pan and Pad are read and not applied */
		sixel->ra_w = n >= 3 ? min_u(p[2], sixel->max_w) : 0;
		sixel->ra_h = n >= 4 ? min_u(p[3], sixel->max_h) : 0;

		/* the raster area is reserved now, so a picture that cannot
		 * fit is refused before it is decoded; the first sixel
		 * allocates it, so raster attributes alone cost no memory */
		release(sixel);
		if (sixel->ra_w && sixel->ra_h &&
		    !reserve_bytes(sixel, (size_t)sixel->ra_w * sixel->ra_h *
				   sizeof(*sixel->buf)))
			fail(sixel);
	}
}

static void start_params(struct tsm_sixel *sixel, enum sixel_state state)
{
	memset(sixel->params, 0, sizeof(sixel->params));
	sixel->nparams = 1;
	sixel->state = state;
}

static void data(struct tsm_sixel *sixel, unsigned char c)
{
	if (c >= '?' && c <= '~') {
		if (!put_one(sixel, c - '?'))
			put(sixel, c - '?', 1);
		return;
	}

	switch (c) {
	case '!':
		start_params(sixel, SIXEL_REPEAT);
		break;
	case '#':
		start_params(sixel, SIXEL_COLOR);
		break;
	case '"':
		start_params(sixel, SIXEL_RASTER);
		break;
	case '$':
		sixel->x = 0;
		break;
	case '-':
		sixel->x = 0;
		sixel->y = min_u(sixel->y + 6, sixel->max_h);
		break;
	default:
		/* everything else is ignored */
		break;
	}
}

static void step(struct tsm_sixel *sixel, unsigned char c)
{
	enum sixel_state state = sixel->state;
	unsigned int *p;

	switch (state) {
	case SIXEL_IDLE:
	case SIXEL_FAILED:
		return;
	case SIXEL_DATA:
		data(sixel, c);
		return;
	case SIXEL_REPEAT:
	case SIXEL_COLOR:
	case SIXEL_RASTER:
		break;
	}

	if (c >= '0' && c <= '9') {
		/* extra parameters are read and dropped */
		if (sixel->nparams > SIXEL_PARAMS)
			return;
		p = &sixel->params[sixel->nparams - 1];
		if (*p < SIXEL_PARAM_MAX)
			*p = *p * 10 + (c - '0');
		return;
	}
	if (c == ';' && state != SIXEL_REPEAT) {
		if (sixel->nparams <= SIXEL_PARAMS)
			++sixel->nparams;
		return;
	}

	sixel->state = SIXEL_DATA;
	if (state == SIXEL_REPEAT) {
		/* a repeat applies only to the sixel right after it */
		if (c >= '?' && c <= '~')
			put(sixel, c - '?', sixel->params[0] ? sixel->params[0] : 1);
		else
			data(sixel, c);
		return;
	}

	end_params(sixel, state);
	/* raster attributes refused their memory */
	if (sixel->state == SIXEL_FAILED)
		return;
	data(sixel, c);
}

size_t tsm_sixel_feed_some(struct tsm_sixel *sixel, const char *buf,
			   size_t len, uint64_t limit)
{
	const unsigned char *p = (const unsigned char *)buf;
	size_t i;

	if (!sixel || !buf)
		return len;

	for (i = 0; i < len; ) {
		++sixel->work;
		if (!(sixel->state == SIXEL_DATA && p[i] >= '?' && p[i] <= '~' &&
		      put_one(sixel, p[i] - '?')))
			step(sixel, p[i]);
		if (++i < len && sixel->work >= limit)
			break;
	}
	return i;
}

void tsm_sixel_feed(struct tsm_sixel *sixel, const char *buf, size_t len)
{
	tsm_sixel_feed_some(sixel, buf, len, UINT64_MAX);
}

uint64_t tsm_sixel_work(const struct tsm_sixel *sixel)
{
	return sixel ? sixel->work : 0;
}

int tsm_sixel_finish(struct tsm_sixel *sixel, struct tsm_sixel_image *image)
{
	unsigned int w, h, row;
	size_t n, i, size;
	uint32_t bg, *px;
	enum sixel_state state;

	if (!sixel || !image)
		return -EINVAL;

	if (sixel->state != SIXEL_DATA && sixel->state != SIXEL_IDLE &&
	    sixel->state != SIXEL_FAILED)
		/* parameters ended by ST still act, as any other end */
		step(sixel, '\0');
	state = sixel->state;
	sixel->state = SIXEL_IDLE;
	if (state == SIXEL_IDLE)
		return -EINVAL;
	if (state == SIXEL_FAILED)
		return -ENOMEM;

	w = max_u(sixel->width, sixel->ra_w);
	h = max_u(sixel->height, sixel->ra_h);
	if (!w || !h) {
		release(sixel);
		return -ENODATA;
	}

	/* '?' columns past the last set bit widen the picture at the end */
	if ((w > sixel->cap_w || h > sixel->cap_h) &&
	    !resize(sixel, max_u(w, sixel->cap_w), max_u(h, sixel->cap_h))) {
		release(sixel);
		return -ENOMEM;
	}

	px = sixel->buf;
	n = (size_t)w * h;
	sixel->work += n;
	if (w != sixel->cap_w)
		for (row = 1; row < h; ++row)
			memmove(px + (size_t)row * w,
				px + (size_t)row * sixel->cap_w,
				w * sizeof(*px));

	if (!sixel->transparent) {
		bg = sixel->pal->colors[0];
		for (i = 0; i < n; ++i)
			if (!px[i])
				px[i] = bg;
	}

	size = n * sizeof(*px);
	if (size < sixel->reserved) {
		/* shrinking returns the tail of the block; nothing is copied
		 * beside it */
		px = realloc(px, size);
		if (!px)
			px = sixel->buf;
		reserve_bytes(sixel, size);
	}

	image->width = w;
	image->height = h;
	image->pixels = px;
	image->transparent = sixel->transparent;

	/* the pixels and their reservation now belong to the caller */
	sixel->buf = NULL;
	sixel->cap_w = 0;
	sixel->cap_h = 0;
	sixel->reserved = 0;
	return 0;
}
