/*
 * libtsm - the vte without images
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * The image glue of a build with -Dimages=false, in place of tsm-vte-image.c,
 * tsm-sixel.c and tsm-kitty.c (ADR-103 in aaronsb/kmscon, libtsm's images
 * option; a departure from upstream libtsm under ADR-303, which builds every
 * source in every build). The vte takes a DCS q as upstream libtsm takes
 * one, consumed with no effect, and an APC is ignored to its end. Sixel is
 * never on: DA1 omits attribute 4, XTSMGRAPHICS answers status 1, and DECRQM
 * of 80, 1070 and 8452 answers their fixed state. The public image calls
 * accept their arguments and do nothing, returning 0 where they return a
 * value (libtsm.h), but for private-color-registers, which sets mode 1070's
 * default as it does with images.
 */

#include <stdio.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "tsm-sixel.h"
#include "tsm-vte-int.h"

/* mode 1070's default, which its fixed state reports */
int vte_image_new(struct tsm_vte *vte)
{
	vte->sixel_private_default = true;
	return 0;
}

void vte_image_free(struct tsm_vte *vte)
{
}

void vte_image_reset(struct tsm_vte *vte)
{
}

void vte_image_cancel(struct tsm_vte *vte)
{
}

bool vte_sixel_on(struct tsm_vte *vte)
{
	return false;
}

void vte_dcs_start(struct tsm_vte *vte, uint32_t data)
{
}

void vte_dcs_collect(struct tsm_vte *vte, uint32_t data)
{
}

void vte_dcs_end(struct tsm_vte *vte, uint32_t data)
{
}

void vte_apc_start(struct tsm_vte *vte)
{
}

void vte_apc_collect(struct tsm_vte *vte, uint32_t data)
{
}

void vte_apc_end(struct tsm_vte *vte, uint32_t data)
{
}

/* XTSMGRAPHICS, CSI ? Pi ; Pa ; Pv S: status 1 for every item */
void vte_xtsmgraphics(struct tsm_vte *vte)
{
	int item = vte->csi_argv[0] < 0 ? 0 : vte->csi_argv[0];
	char buf[32];
	int len;

	len = snprintf(buf, sizeof(buf), "\e[?%d;1S", item);
	if (len > 0 && (size_t)len < sizeof(buf))
		vte_write(vte, buf, len);
}

size_t vte_sixel_run(struct tsm_vte *vte, const char *u8, size_t len)
{
	return 0;
}

size_t vte_kitty_run(struct tsm_vte *vte, const char *u8, size_t len)
{
	return 0;
}

bool vte_image_paused(struct tsm_vte *vte)
{
	return false;
}

bool vte_image_busy(struct tsm_vte *vte)
{
	return false;
}

bool vte_image_set_host(struct tsm_vte *vte, bool host)
{
	return false;
}

SHL_EXPORT
void tsm_vte_set_image_cb(struct tsm_vte *vte, tsm_vte_image_cb image_cb,
			  void *image_data)
{
}

SHL_EXPORT
int tsm_vte_image_reply(struct tsm_vte *vte, uint64_t id,
			const struct tsm_vte_image_result *res)
{
	return 0;
}

SHL_EXPORT
void tsm_vte_set_sixel(struct tsm_vte *vte, bool enable)
{
}

SHL_EXPORT
int tsm_vte_set_graphic_max_size(struct tsm_vte *vte, unsigned int width,
				 unsigned int height)
{
	return 0;
}

SHL_EXPORT
void tsm_vte_set_kitty_graphics(struct tsm_vte *vte, bool enable)
{
}

/* private-color-registers still sets mode 1070's default, which its fixed
 * state reports, so 1070 answers as an images build under --no-sixel does; a
 * count an images build refuses changes nothing here either */
SHL_EXPORT
int tsm_vte_set_sixel_registers(struct tsm_vte *vte, unsigned int count,
				bool private_registers)
{
	if (!vte || (count && (count < TSM_SIXEL_REGISTERS_MIN ||
			       count > TSM_SIXEL_REGISTERS_MAX)))
		return 0;

	vte->sixel_private_default = private_registers;
	vte_modes_reset_hard(vte);
	return 0;
}
