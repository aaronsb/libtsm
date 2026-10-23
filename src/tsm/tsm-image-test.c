/*
 * libtsm - Images made for tests
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * The test calls of ADR-103 step 3a: an image and a placement made without a
 * protocol, as the sixel and kitty parsers make them. They are left out of
 * libtsm.sym, so the shared library does not export them; the tests, which
 * compile libtsm's sources themselves, and hosts that build it as a static
 * subproject reach them through libtsm.h.
 */

#include <errno.h>
#include "libtsm.h"
#include "libtsm-int.h"

int tsm_screen_test_image(struct tsm_screen *con, const struct tsm_test_image *img,
			  uint64_t *key)
{
	struct tsm_image *image;
	int ret;

	if (!con || !img || !key)
		return -EINVAL;
	ret = screen_image_new(con, img, &image);
	if (!ret)
		*key = image->key;
	return ret;
}

int tsm_screen_test_place(struct tsm_screen *con, const struct tsm_test_placement *pl,
			  uint64_t *key)
{
	struct tsm_placement *p;
	int ret;

	if (!con || !pl || !key)
		return -EINVAL;
	ret = screen_image_place(con, screen_image_find(con, pl->image), pl, &p);
	if (!ret)
		*key = p->key;
	return ret;
}
