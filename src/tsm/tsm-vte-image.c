/*
 * libtsm - the vte's image glue
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Sixel and the kitty graphics protocol through the vte (ADR-103 in
 * aaronsb/kmscon): the DCS q and APC G hooks, the work bound's runs,
 * XTSMGRAPHICS, the host hook and its reply, and the calls that turn the
 * protocols on and set them. tsm-vte-int.h says what the vte calls; a build
 * with -Dimages=false takes tsm-vte-image-none.c in place of this file.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "tsm-kitty.h"
#include "tsm-sixel.h"
#include "tsm-vte-int.h"
#include "shl-llog.h"

#define LLOG_SUBSYSTEM "tsm-vte"

/*
 * Sixel (ADR-103 in aaronsb/kmscon). The DCS payload streams into the
 * decoder (tsm-sixel.c), which reserves its memory from the screen's image
 * budget; at ST the picture moves into the screen's store with that
 * reservation and is placed at the cursor.
 */

/* the host's switch, and a budget to pay for the pictures */
bool vte_sixel_on(struct tsm_vte *vte)
{
	return vte->sixel_enabled && (vte->con->img.reserve || vte->con->img.limit);
}

static bool sixel_reserve(void *data, size_t old_size, size_t new_size)
{
	struct tsm_vte *vte = data;

	if (new_size > old_size)
		return !screen_images_reserve(vte->con, new_size - old_size);
	screen_images_unreserve(vte->con, old_size - new_size);
	return true;
}

/*
 * Frees the decoder, with any picture in progress, and the shared registers;
 * the next picture makes them again from the settings in force then.
 */
static void sixel_drop(struct tsm_vte *vte)
{
	tsm_sixel_free(vte->sixel);
	vte->sixel = NULL;
	tsm_sixel_palette_free(vte->sixel_shared);
	vte->sixel_shared = NULL;
	vte->sixel_active = false;
}

/* RIS: what the client set returns to what the host set, but for modes 80,
 * 1070 and 8452, which the vte resets with the other rows only RIS resets */
void vte_image_reset(struct tsm_vte *vte)
{
	sixel_drop(vte);
	vte->sixel_registers = vte->sixel_max_registers;
}

static bool sixel_ready(struct tsm_vte *vte)
{
	struct tsm_sixel_limits lim = {
		.max_width = vte->graphic_max_w,
		.max_height = vte->graphic_max_h,
		.registers = vte->sixel_registers,
		.reserve = sixel_reserve,
		.reserve_data = vte,
	};

	if (!vte->sixel && tsm_sixel_new(&vte->sixel, &lim))
		return false;
	if (!vte->sixel_private && !vte->sixel_shared &&
	    tsm_sixel_palette_new(&vte->sixel_shared, vte->sixel_registers))
		return false;
	return true;
}

/*
 * The image work done so far: the sixel decoder's, the kitty parser's, and
 * the eviction scans' the screen's budget and store limits cost
 */
static uint64_t sixel_work(struct tsm_vte *vte)
{
	return tsm_sixel_work(vte->sixel) + tsm_kitty_work(vte->kitty) +
	       vte->con->img.work;
}

/* counts @work against what this input call may still do */
static void sixel_charge(struct tsm_vte *vte, uint64_t work)
{
	vte->work_left = work < vte->work_left ? vte->work_left - work : 0;
}

/* DCS P1;P2;P3 q: sixel. tsm-vte.c routes no other DCS here. */
void vte_dcs_start(struct tsm_vte *vte, uint32_t data)
{
	if (!vte_sixel_on(vte) || !sixel_ready(vte))
		return;

	tsm_sixel_begin(vte->sixel, vte->csi_argv[0], vte->csi_argv[1],
			vte->sixel_private ? NULL : vte->sixel_shared);
	vte->sixel_active = true;
}

void vte_dcs_collect(struct tsm_vte *vte, uint32_t data)
{
	uint64_t work;
	char c = data;

	/* the payload is ASCII; anything else is ignored, as the decoder
	 * ignores bytes it has no use for */
	if (!vte->sixel_active || data >= 0x80)
		return;

	work = sixel_work(vte);
	tsm_sixel_feed(vte->sixel, &c, 1);
	sixel_charge(vte, sixel_work(vte) - work);
}

/* the end of the string: @data is what ended it */
void vte_dcs_end(struct tsm_vte *vte, uint32_t data)
{
	struct tsm_sixel_image img;
	struct tsm_image *image;
	uint64_t work;
	int ret;

	if (!vte->sixel_active)
		return;
	vte->sixel_active = false;

	/* CAN and SUB discard the picture; ST, or anything else that ends
	 * the string, shows it, unless sixel went off while it streamed (its
	 * budget removed, say). The budget would refuse that picture too, but
	 * only after the store made room for it, which can evict an image */
	if (data == 0x18 || data == 0x1a || !vte_sixel_on(vte)) {
		tsm_sixel_abort(vte->sixel);
		return;
	}

	work = sixel_work(vte);
	ret = tsm_sixel_finish(vte->sixel, &img);
	if (!ret)
		ret = screen_sixel_new(vte->con, img.width, img.height, img.pixels,
				       !img.transparent, &image);
	/* a transparent picture is read once more for its opacity */
	sixel_charge(vte, sixel_work(vte) - work +
		     (!ret && img.transparent ? (uint64_t)img.width * img.height : 0));
	if (ret) {
		llog_debug(vte, "sixel picture not stored: %d", ret);
		return;
	}

	ret = screen_sixel_place(vte->con, image, !vte->sixel_no_scroll,
				 vte->sixel_cursor_right);
	if (ret) {
		llog_debug(vte, "sixel picture %ux%u not placed: %d", img.width,
			   img.height, ret);
		screen_image_drop(vte->con, image);
	}
}

/*
 * The kitty graphics protocol (ADR-103 in aaronsb/kmscon, step 4): an APC
 * streams into the kitty parser (tsm-kitty.c) while kitty graphics are on and
 * the screen has an image budget; otherwise it is parsed only to find where
 * it ends, and gets no reply.
 */
static bool kitty_on(struct tsm_vte *vte)
{
	return vte->kitty_enabled && (vte->con->img.reserve || vte->con->img.limit);
}

static void kitty_write(struct tsm_vte *vte, const char *u8, size_t len)
{
	vte_write(vte, u8, len);
}

void vte_apc_start(struct tsm_vte *vte)
{
	vte->apc_kitty = kitty_on(vte);
	if (vte->apc_kitty)
		tsm_kitty_begin(vte->kitty);
	else
		tsm_kitty_cancel(vte->kitty);
}

void vte_apc_collect(struct tsm_vte *vte, uint32_t data)
{
	uint64_t work;

	if (!vte->apc_kitty)
		return;
	work = sixel_work(vte);
	tsm_kitty_feed(vte->kitty, data);
	sixel_charge(vte, sixel_work(vte) - work);
}

/* the end of the string: @data is what ended it */
void vte_apc_end(struct tsm_vte *vte, uint32_t data)
{
	uint64_t work;

	if (!vte->apc_kitty)
		return;
	vte->apc_kitty = false;
	/* kitty graphics went off while it streamed (its budget removed) */
	if (!kitty_on(vte)) {
		tsm_kitty_cancel(vte->kitty);
		return;
	}
	work = sixel_work(vte);
	tsm_kitty_end(vte->kitty, data);
	sixel_charge(vte, sixel_work(vte) - work);
}

/* the image settings' defaults, and the kitty parser */
int vte_image_new(struct tsm_vte *vte)
{
	int ret;

	vte->graphic_max_w = TSM_SIXEL_MAX_SIZE_DEFAULT;
	vte->graphic_max_h = TSM_SIXEL_MAX_SIZE_DEFAULT;
	vte->sixel_max_registers = TSM_SIXEL_REGISTERS_DEFAULT;
	vte->sixel_private_default = true;
	vte_image_reset(vte);

	ret = tsm_kitty_new(&vte->kitty, vte, vte->con, kitty_write);
	if (ret)
		return ret;
	tsm_kitty_set_max_size(vte->kitty, vte->graphic_max_w, vte->graphic_max_h);
	return 0;
}

void vte_image_free(struct tsm_vte *vte)
{
	sixel_drop(vte);
	tsm_kitty_free(vte->kitty);
}

/* a reset leaves a DCS without its end, discarding the picture, and a kitty
 * upload in progress goes */
void vte_image_cancel(struct tsm_vte *vte)
{
	if (vte->sixel_active) {
		tsm_sixel_abort(vte->sixel);
		vte->sixel_active = false;
	}
	tsm_kitty_cancel(vte->kitty);
	vte->apc_kitty = false;
}

/* a kitty request waits for the host's reply */
bool vte_image_paused(struct tsm_vte *vte)
{
	return tsm_kitty_paused(vte->kitty);
}

/* the host feeds nothing while its request is out, nor from inside the hook */
bool vte_image_busy(struct tsm_vte *vte)
{
	return tsm_kitty_paused(vte->kitty) || tsm_kitty_in_hook(vte->kitty);
}

bool vte_image_set_host(struct tsm_vte *vte, bool host)
{
	return tsm_kitty_set_host(vte->kitty, host);
}

/*
 * XTSMGRAPHICS, CSI ? Pi ; Pa ; Pv S (ADR-103): item 1, the sixel colour
 * registers, read, reset, set from 2 to num-color-registers, and read the
 * maximum; item 2, the sixel geometry, read and read the maximum, both
 * max-graphic-size, and a change refused (status 3), as xterm refuses one.
 * Other items, ReGIS among them, and every item while sixel is off, answer
 * status 1.
 */
void vte_xtsmgraphics(struct tsm_vte *vte)
{
	int item = vte->csi_argv[0], action = vte->csi_argv[1];
	int value = vte->csi_argv[2];
	unsigned int status = 0, nres = 0, res[2] = { 0, 0 }, count;
	char buf[64];
	int len;

	if (!vte_sixel_on(vte) || (item != 1 && item != 2)) {
		status = 1;
	} else if (item == 1) {
		count = vte->sixel_registers;
		switch (action) {
		case 1: /* read */
			break;
		case 2: /* reset */
			count = vte->sixel_max_registers;
			break;
		case 3: /* set */
			if (value >= TSM_SIXEL_REGISTERS_MIN &&
			    (unsigned int)value <= vte->sixel_max_registers)
				count = value;
			else
				status = 3;
			break;
		case 4: /* read the maximum */
			res[0] = vte->sixel_max_registers;
			nres = 1;
			break;
		default:
			status = 2;
			break;
		}
		if (!status && !nres) {
			/* registers made at the next picture take the count */
			if (count != vte->sixel_registers) {
				sixel_drop(vte);
				vte->sixel_registers = count;
			}
			res[0] = count;
			nres = 1;
		}
	} else {
		switch (action) {
		case 1: /* read */
		case 4: /* read the maximum */
			res[0] = vte->graphic_max_w;
			res[1] = vte->graphic_max_h;
			nres = 2;
			break;
		case 2: /* reset */
		case 3: /* set */
			status = 3;
			break;
		default:
			status = 2;
			break;
		}
	}

	if (item < 0)
		item = 0;
	if (nres == 2)
		len = snprintf(buf, sizeof(buf), "\e[?%d;%u;%u;%uS", item,
			       status, res[0], res[1]);
	else if (nres)
		len = snprintf(buf, sizeof(buf), "\e[?%d;%u;%uS", item, status,
			       res[0]);
	else
		len = snprintf(buf, sizeof(buf), "\e[?%d;%uS", item, status);
	if (len > 0 && (size_t)len < sizeof(buf))
		vte_write(vte, buf, len);
}

/* a byte that DCS passthrough hands the decoder as it is, in every mode */
static inline bool sixel_byte(unsigned char c)
{
	return c < 0x7f && c != 0x18 && c != 0x1a && c != 0x1b;
}

/*
 * Feeds the run of sixel bytes at @u8 straight to the decoder, as the parser
 * would hand them over one at a time; returns how many it took, or 0 when no
 * picture streams, @u8 does not start with such a byte, or the UTF-8 machine
 * is part way through a character.
 */
size_t vte_sixel_run(struct tsm_vte *vte, const char *u8, size_t len)
{
	uint64_t work, start, limit;
	size_t n;

	if (!vte->sixel_active || !sixel_byte(u8[0]) ||
	    !((vte->flags & (TSM_VTE_FLAG_7BIT_MODE | TSM_VTE_FLAG_8BIT_MODE)) ||
	      tsm_utf8_mach_idle(vte->mach)))
		return 0;

	work = sixel_work(vte);
	start = tsm_sixel_work(vte->sixel);
	for (n = 1; n < len && sixel_byte(u8[n]); ++n)
		;
	limit = start + vte->work_left < start ? UINT64_MAX : start + vte->work_left;
	n = tsm_sixel_feed_some(vte->sixel, u8, n, limit);
	sixel_charge(vte, sixel_work(vte) - work);
	/* the UTF-8 machine ends where those bytes would have left it */
	if (!(vte->flags & (TSM_VTE_FLAG_7BIT_MODE | TSM_VTE_FLAG_8BIT_MODE)))
		tsm_utf8_mach_feed(vte->mach, u8[n - 1]);
	return n;
}

/* a byte a kitty payload's run takes as it is, in every mode */
static inline bool kitty_byte(unsigned char c)
{
	return c >= 0x20 && c < 0x7f;
}

/*
 * Feeds the run of payload bytes at @u8 straight to the kitty parser, as the
 * parser would hand them over one at a time, at most as many as the work
 * left allows (each byte is a unit of work); returns how many it took, or 0
 * when no command streams, the parser is outside a payload, @u8 does not
 * start with such a byte, or the UTF-8 machine is part way through a
 * character.
 */
size_t vte_kitty_run(struct tsm_vte *vte, const char *u8, size_t len)
{
	uint64_t work;
	size_t n;

	if (!vte->apc_kitty || !kitty_byte(u8[0]) ||
	    !tsm_kitty_in_payload(vte->kitty) ||
	    !((vte->flags & (TSM_VTE_FLAG_7BIT_MODE | TSM_VTE_FLAG_8BIT_MODE)) ||
	      tsm_utf8_mach_idle(vte->mach)))
		return 0;

	work = sixel_work(vte);
	for (n = 1; n < len && n < vte->work_left && kitty_byte(u8[n]); ++n)
		;
	tsm_kitty_feed_run(vte->kitty, u8, n);
	sixel_charge(vte, sixel_work(vte) - work);
	if (!(vte->flags & (TSM_VTE_FLAG_7BIT_MODE | TSM_VTE_FLAG_8BIT_MODE)))
		tsm_utf8_mach_feed(vte->mach, u8[n - 1]);
	return n;
}

SHL_EXPORT
void tsm_vte_set_image_cb(struct tsm_vte *vte, tsm_vte_image_cb image_cb,
			  void *image_data)
{
	if (!vte)
		return;

	++vte->parse_cnt;
	tsm_kitty_set_hook(vte->kitty, image_cb, image_data);
	--vte->parse_cnt;
}

SHL_EXPORT
int tsm_vte_image_reply(struct tsm_vte *vte, uint64_t id,
			const struct tsm_vte_image_result *res)
{
	int ret;

	if (!vte)
		return -EINVAL;

	/* the client's reply is written as one to its input */
	++vte->parse_cnt;
	ret = tsm_kitty_reply(vte->kitty, id, res);
	screen_images_settle(vte->con);
	--vte->parse_cnt;
	return ret;
}

SHL_EXPORT
void tsm_vte_set_sixel(struct tsm_vte *vte, bool enable)
{
	if (!vte)
		return;

	vte->sixel_enabled = enable;
	if (!enable)
		sixel_drop(vte);
}

SHL_EXPORT
int tsm_vte_set_graphic_max_size(struct tsm_vte *vte, unsigned int width,
				 unsigned int height)
{
	if (!vte)
		return -EINVAL;

	vte->graphic_max_w = !width ? TSM_SIXEL_MAX_SIZE_DEFAULT :
			     width > TSM_SCREEN_IMAGE_MAX ? TSM_SCREEN_IMAGE_MAX : width;
	vte->graphic_max_h = !height ? TSM_SIXEL_MAX_SIZE_DEFAULT :
			     height > TSM_SCREEN_IMAGE_MAX ? TSM_SCREEN_IMAGE_MAX : height;
	sixel_drop(vte);
	tsm_kitty_set_max_size(vte->kitty, vte->graphic_max_w, vte->graphic_max_h);
	return 0;
}

SHL_EXPORT
void tsm_vte_set_kitty_graphics(struct tsm_vte *vte, bool enable)
{
	if (!vte)
		return;

	vte->kitty_enabled = enable;
	if (!enable) {
		tsm_kitty_cancel(vte->kitty);
		vte->apc_kitty = false;
	}
}

SHL_EXPORT
int tsm_vte_set_sixel_registers(struct tsm_vte *vte, unsigned int count,
				bool private_registers)
{
	if (!vte || (count && (count < TSM_SIXEL_REGISTERS_MIN ||
			       count > TSM_SIXEL_REGISTERS_MAX)))
		return -EINVAL;

	vte->sixel_max_registers = count ? count : TSM_SIXEL_REGISTERS_DEFAULT;
	vte->sixel_private_default = private_registers;
	vte_image_reset(vte);
	vte_modes_reset_hard(vte);
	return 0;
}
