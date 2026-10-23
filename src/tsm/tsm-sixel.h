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
 * Sixel Decoder (internal)
 *
 * A streaming decoder for the payload of DCS P1;P2;P3 q ... ST, as ADR-103
 * (aaronsb/kmscon) specifies it. The vte hands it the DCS parameters, then the
 * payload bytes as they arrive, then asks for the picture at ST. The payload is
 * never buffered: each byte updates the decoder's state and its picture, and
 * the work per byte is bounded by the image limits.
 *
 * The picture is kept as colour register numbers while it is decoded and
 * turned into premultiplied ARGB32 (ADR-102's colour glyph format) at the end,
 * so a register redefined after it was drawn with shows its last definition,
 * as on a VT340, whose pixels hold register numbers.
 *
 * Image size: the width is the furthest column a sixel data character reached;
 * the height ends at the lowest pixel row a sixel set. Raster attributes
 * extend both. Rows of a sixel band below its lowest set bit are not part of
 * the picture unless the raster attributes cover them, so a picture whose
 * height is not a multiple of six keeps its height (foot does the same).
 */

#ifndef TSM_SIXEL_H
#define TSM_SIXEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* max-graphic-size and num-color-registers defaults (ADR-103, Settings) */
#define TSM_SIXEL_MAX_SIZE_DEFAULT 10000
#define TSM_SIXEL_REGISTERS_DEFAULT 1024

/* Bounds on the limits themselves: larger values are clamped to these. */
#define TSM_SIXEL_MAX_SIZE_LIMIT 65535
#define TSM_SIXEL_REGISTERS_MIN 2
#define TSM_SIXEL_REGISTERS_MAX 65535

/*
 * A set of colour registers. With mode 1070 reset, one table is shared by
 * every image and keeps its definitions until RIS; with it set, each image
 * starts from the defaults in the decoder's own table.
 */
struct tsm_sixel_palette;

/* Registers 0 to 15 start from the VT340's colour map, the rest black. */
int tsm_sixel_palette_new(struct tsm_sixel_palette **out, unsigned int registers);
void tsm_sixel_palette_free(struct tsm_sixel_palette *pal);
void tsm_sixel_palette_reset(struct tsm_sixel_palette *pal);

/*
 * Asked before the decoder's pixel buffer changes size, from @old_size to
 * @new_size bytes. Returning false refuses a growth, and the image is then
 * dropped. A shrink is reported the same way and its answer is ignored.
 */
typedef bool (*tsm_sixel_reserve_cb)(void *data, size_t old_size,
				     size_t new_size);

struct tsm_sixel_limits {
	unsigned int max_width;		/* max-graphic-size; 0 for the default */
	unsigned int max_height;
	unsigned int registers;		/* num-color-registers; 0 for the default */
	tsm_sixel_reserve_cb reserve;	/* NULL: only the size limits apply */
	void *reserve_data;
};

struct tsm_sixel_image {
	unsigned int width;
	unsigned int height;
	/* width * height premultiplied ARGB32, rows top to bottom, no padding.
	 * The caller owns it, frees it with free(), and owns its reservation
	 * of width * height * 4 bytes. */
	uint32_t *pixels;
	/* P2 = 1: pixels no sixel set are transparent; otherwise they take
	 * colour register 0 and the whole picture is opaque. */
	bool transparent;
};

struct tsm_sixel;

int tsm_sixel_new(struct tsm_sixel **out, const struct tsm_sixel_limits *limits);
void tsm_sixel_free(struct tsm_sixel *sixel);

/*
 * Starts an image from the DCS parameters; -1 is an omitted parameter. P3 is
 * ignored, as xterm ignores it. @shared is the shared register table (mode
 * 1070 reset), or NULL for registers private to this image; a shared table
 * must stay alive until tsm_sixel_finish() or tsm_sixel_abort(). An image
 * still in progress is discarded.
 */
void tsm_sixel_begin(struct tsm_sixel *sixel, int p1, int p2,
		     struct tsm_sixel_palette *shared);

/* Payload bytes, the bytes between the final 'q' and ST. */
void tsm_sixel_feed(struct tsm_sixel *sixel, const char *buf, size_t len);

/*
 * Ends the image (ST). Returns 0 and fills @image, or -ENODATA for an empty
 * picture, -ENOMEM when its memory was refused or could not be allocated, or
 * -EINVAL with no image in progress. The decoder is idle afterwards.
 */
int tsm_sixel_finish(struct tsm_sixel *sixel, struct tsm_sixel_image *image);

/* Discards the image in progress (CAN, SUB, or a reset). */
void tsm_sixel_abort(struct tsm_sixel *sixel);

#endif /* TSM_SIXEL_H */
