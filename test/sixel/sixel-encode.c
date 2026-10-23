/*
 * sixel-encode - encode a binary PPM (P6) to sixel with libsixel's encoder
 *
 * The fixtures in this directory come from libsixel (MIT), the encoder
 * img2sixel and mpv use. img2sixel itself loads pictures through gdk-pixbuf,
 * which cannot run inside every sandbox, so this tool reads the PPM itself
 * and hands the pixels to the same encoder, with the options img2sixel takes:
 *
 *   sixel-encode IN.ppm [-d none] [-p 16] [-I] ... > OUT.six
 *
 * Built and run on a host with libsixel by gen-fixtures.sh; not part of the
 * build.
 */

#include <sixel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	sixel_encoder_t *enc;
	unsigned char *px;
	unsigned int w, h, max;
	FILE *f;
	int i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s IN.ppm [img2sixel options]\n", argv[0]);
		return 2;
	}

	f = fopen(argv[1], "rb");
	if (!f || fscanf(f, "P6 %u %u %u", &w, &h, &max) != 3 || max != 255 ||
	    fgetc(f) == EOF) {
		fprintf(stderr, "%s: not a P6 PPM with maxval 255\n", argv[1]);
		return 1;
	}
	px = malloc((size_t)w * h * 3);
	if (!px || fread(px, 3, (size_t)w * h, f) != (size_t)w * h) {
		fprintf(stderr, "%s: short read\n", argv[1]);
		return 1;
	}
	fclose(f);

	if (SIXEL_FAILED(sixel_encoder_new(&enc, NULL)))
		return 1;
	for (i = 2; i < argc; ++i) {
		const char *opt = argv[i];
		const char *arg = NULL;

		if (opt[0] != '-' || !opt[1] || opt[2]) {
			fprintf(stderr, "bad option %s\n", opt);
			return 2;
		}
		if (strchr("dpbBsqlE", opt[1]) && i + 1 < argc)
			arg = argv[++i];
		if (SIXEL_FAILED(sixel_encoder_setopt(enc, opt[1], arg))) {
			fprintf(stderr, "option %s refused\n", opt);
			return 2;
		}
	}

	if (SIXEL_FAILED(sixel_encoder_encode_bytes(enc, px, w, h,
						     SIXEL_PIXELFORMAT_RGB888,
						     NULL, 0)))
		return 1;

	sixel_encoder_unref(enc);
	free(px);
	return 0;
}
