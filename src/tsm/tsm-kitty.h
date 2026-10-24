/*
 * libtsm - The kitty graphics protocol
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * The kitty graphics protocol's commands (ADR-103 in aaronsb/kmscon, step 4a;
 * tsm-kitty.c), fed by the vte one APC at a time: tsm_kitty_begin when an APC
 * starts, its bytes through tsm_kitty_feed or, in its payload, tsm_kitty_feed
 * _run, and tsm_kitty_end with the byte that ended it. A chunked upload spans
 * several APCs and lives here between them, and so does a request to the
 * host hook until it is answered. Replies go out through the write callback
 * the vte gives.
 */

#ifndef TSM_KITTY_H
#define TSM_KITTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "libtsm.h"

struct tsm_kitty;

/* sends @len bytes of a reply to the client, as the vte's own replies go */
typedef void (*tsm_kitty_write_cb)(struct tsm_vte *vte, const char *u8, size_t len);

int tsm_kitty_new(struct tsm_kitty **out, struct tsm_vte *vte, struct tsm_screen *con,
		  tsm_kitty_write_cb write);
/* drops an upload in progress, giving its bytes back to the screen */
void tsm_kitty_free(struct tsm_kitty *k);

/* max-graphic-size: the largest image in pixels, each dimension */
void tsm_kitty_set_max_size(struct tsm_kitty *k, unsigned int width, unsigned int height);

/*
 * Set by each input call, and restored when it returns: with @host_ok a
 * command may be buffered for, and handed to, the hook. Returns the flag it
 * replaces.
 */
bool tsm_kitty_set_host(struct tsm_kitty *k, bool host_ok);

/* an APC begins */
void tsm_kitty_begin(struct tsm_kitty *k);
/* one codepoint of the APC, after its introducer */
void tsm_kitty_feed(struct tsm_kitty *k, uint32_t c);
/* whether the APC is in a payload that tsm_kitty_feed_run takes */
bool tsm_kitty_in_payload(struct tsm_kitty *k);
/* @len payload bytes, each 0x20 to 0x7e, as tsm_kitty_feed would take them */
void tsm_kitty_feed_run(struct tsm_kitty *k, const char *u8, size_t len);
/* the APC ends with @data: CAN and SUB abort the command, anything else
 * completes it, handing the host hook a request when the input call may */
void tsm_kitty_end(struct tsm_kitty *k, uint32_t data);

/* a reset: drops an upload in progress and a request that is out, and
 * ignores the rest of an APC */
void tsm_kitty_cancel(struct tsm_kitty *k);

/*
 * The host hook (TSM_VTE_IMAGE_HOST): while a request is out the vte parses
 * nothing, and tsm_kitty_reply answers it, with tsm_vte_image_reply's
 * arguments and returns. Setting the hook answers a request out as
 * TSM_VTE_IMAGE_FAILED.
 */
bool tsm_kitty_paused(struct tsm_kitty *k);
/* whether the hook is running now */
bool tsm_kitty_in_hook(struct tsm_kitty *k);
void tsm_kitty_set_hook(struct tsm_kitty *k, tsm_vte_image_cb hook, void *data);
int tsm_kitty_reply(struct tsm_kitty *k, uint64_t id, const struct tsm_vte_image_result *res);

/* the image work done so far, in pixels and payload bytes, which the vte
 * counts against an input call's bound */
uint64_t tsm_kitty_work(struct tsm_kitty *k);

#endif /* TSM_KITTY_H */
