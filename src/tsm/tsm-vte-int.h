/*
 * libtsm - the vte's state, shared with its image glue
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * struct tsm_vte, for tsm-vte.c and the image glue (ADR-103 in aaronsb/kmscon,
 * libtsm's images option): sixel and the kitty graphics protocol reach the vte
 * through the vte_* calls below. tsm-vte-image.c implements them with the
 * decoders, tsm-sixel.c and tsm-kitty.c; with -Dimages=false the build takes
 * tsm-vte-image-none.c in their place, which implements them without.
 */

#ifndef TSM_VTE_INT_H
#define TSM_VTE_INT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "libtsm.h"
#include "libtsm-int.h"

/* max CSI arguments */
#define CSI_ARG_MAX 16

/* max colon-separated sub-parameters of one CSI argument */
#define CSI_SUBARG_MAX 8

/* max length of an OSC code */
#define OSC_MAX_LEN 128

struct vte_saved_state {
	unsigned int cursor_x;
	unsigned int cursor_y;
	struct cell_attr cattr;
	tsm_vte_charset **gl;
	tsm_vte_charset **gr;
	enum tsm_screen_cursor_style cursor_style;
	bool wrap_mode;
	bool origin_mode;
};

struct tsm_vte {
	unsigned long ref;
	tsm_log_t llog;
	void *llog_data;
	struct tsm_screen *con;
	tsm_vte_write_cb write_cb;
	void *data;
	char *palette_name;
	bool backspace_sends_delete;
	/* the width method the terminal starts with and resets to */
	enum tsm_width_method width_method;

	struct tsm_utf8_mach *mach;
	unsigned long parse_cnt;

	unsigned int state;
	/* the DCS string in progress: enum dcs_kind in tsm-vte.c */
	unsigned int dcs;
	unsigned int csi_argc;
	int csi_argv[CSI_ARG_MAX];
	/* Sub-parameters: "4:3" is csi_argv 4 with csi_subargc 1 and
	 * csi_subargv {3}; an argument without a colon has csi_subargc 0. An
	 * empty sub-parameter is -1. Colons past CSI_SUBARG_MAX set
	 * csi_subarg_drop, which discards digits until the next ';'. */
	unsigned int csi_subargc[CSI_ARG_MAX];
	int csi_subargv[CSI_ARG_MAX][CSI_SUBARG_MAX];
	bool csi_subarg_drop;
	unsigned int csi_flags;

	tsm_vte_osc_cb osc_cb;
	void *osc_data;
	unsigned int osc_len;
	char osc_arg[OSC_MAX_LEN];

	tsm_vte_mouse_cb mouse_cb;
	void *mouse_data;
	/* the mouse protocol (9, 1000, 1002, 1003) and encoding (1006,
	 * 1016; 0 for X10), and the cell of the last motion reported, plus
	 * one, 0 for none */
	unsigned int mouse_track;
	unsigned int mouse_encoding;
	unsigned int mouse_last_col;
	unsigned int mouse_last_row;
	bool bracketed_paste;
	/* xterm's modifyOtherKeys level (XTMODKEYS 4): 0, 1 or 2 */
	unsigned int modify_other_keys;
	/* mode 1004: the host's focus changes are reported */
	bool focus_events;

	tsm_vte_bell_cb bell_cb;
	void *bell_data;

	tsm_vte_led_cb led_cb;
	void *led_data;
	unsigned int led_state;

	/* XTVERSION's text, the host's or the default */
	char version[TSM_VTE_VERSION_MAX + 1];

	uint8_t (*custom_palette_storage)[3];
	uint8_t (*palette)[3];
	struct cell_attr def_attr;
	struct cell_attr cattr;
	unsigned int flags;

	tsm_vte_charset **gl;
	tsm_vte_charset **gr;
	tsm_vte_charset **glt;
	tsm_vte_charset **grt;
	tsm_vte_charset *g0;
	tsm_vte_charset *g1;
	tsm_vte_charset *g2;
	tsm_vte_charset *g3;

	struct vte_saved_state saved_state;
	unsigned int alt_cursor_x;
	unsigned int alt_cursor_y;

	/* sixel (ADR-103): the host's settings */
	bool sixel_enabled;
	unsigned int graphic_max_w;		/* max-graphic-size */
	unsigned int graphic_max_h;
	unsigned int sixel_max_registers;	/* num-color-registers */
	bool sixel_private_default;		/* private-color-registers */
	/* the client's: XTSMGRAPHICS's register count, modes 1070, 80, 8452 */
	unsigned int sixel_registers;
	bool sixel_private;
	bool sixel_no_scroll;
	bool sixel_cursor_right;
	/* made at the first image, and again after a setting changes */
	struct tsm_sixel *sixel;
	struct tsm_sixel_palette *sixel_shared;
	bool sixel_active;			/* a DCS q streams into it */
	/* the kitty graphics protocol (ADR-103 step 4) */
	bool kitty_enabled;
	/* the host blinks the cursor in the styles that blink (mode 12) */
	bool cursor_blink;
	/* mode 2026: the host presents by it, a hold, the holds ended, and
	 * whether this input call returns at an end */
	bool sync_enabled;
	bool sync;
	uint64_t sync_frames;
	bool sync_stop;
	struct tsm_kitty *kitty;
	bool apc_kitty;				/* the APC streams into it */
	/* the image work tsm_vte_input_bounded has left in this call */
	uint64_t work_left;
};

/* tsm-vte.c: writes a reply to the client (vte_write, vte_write_raw), and
 * restores the mode rows only RIS resets (80, 1070 and 8452) */
void vte_write_debug(struct tsm_vte *vte, const char *u8, size_t len, bool raw,
		     const char *file, int line);
void vte_modes_reset_hard(struct tsm_vte *vte);

#define vte_write(_vte, _u8, _len) \
	vte_write_debug((_vte), (_u8), (_len), false, __FILE__, __LINE__)
#define vte_write_raw(_vte, _u8, _len) \
	vte_write_debug((_vte), (_u8), (_len), true, __FILE__, __LINE__)

/*
 * The image glue. vte_image_new sets the image settings' defaults and makes
 * the kitty parser, and vte_image_free frees what the glue holds.
 * vte_image_reset is RIS for the glue, and vte_image_cancel drops a string in
 * progress when the vte resets. vte_sixel_on says whether sixel is on, turned
 * on and with a budget (DA1, XTSMGRAPHICS and the HOST_SIXEL modes read it).
 * The DCS actions take a DCS q, the only DCS tsm-vte.c routes here, and
 * feed it to sixel; the APC actions feed kitty commands. vte_xtsmgraphics
 * answers XTSMGRAPHICS, and vte_sixel_run and vte_kitty_run take a run of
 * payload bytes in DCS passthrough or an APC, returning 0 when they take
 * none. vte_image_paused is true while a kitty request waits for the host's
 * reply, vte_image_busy while one waits or the host hook runs, and
 * vte_image_set_host lets kitty commands go to the host hook (returning
 * what was set before).
 */
int vte_image_new(struct tsm_vte *vte);
void vte_image_free(struct tsm_vte *vte);
void vte_image_reset(struct tsm_vte *vte);
void vte_image_cancel(struct tsm_vte *vte);
bool vte_sixel_on(struct tsm_vte *vte);
void vte_dcs_start(struct tsm_vte *vte, uint32_t data);
void vte_dcs_collect(struct tsm_vte *vte, uint32_t data);
void vte_dcs_end(struct tsm_vte *vte, uint32_t data);
void vte_apc_start(struct tsm_vte *vte);
void vte_apc_collect(struct tsm_vte *vte, uint32_t data);
void vte_apc_end(struct tsm_vte *vte, uint32_t data);
void vte_xtsmgraphics(struct tsm_vte *vte);
size_t vte_sixel_run(struct tsm_vte *vte, const char *u8, size_t len);
size_t vte_kitty_run(struct tsm_vte *vte, const char *u8, size_t len);
bool vte_image_paused(struct tsm_vte *vte);
bool vte_image_busy(struct tsm_vte *vte);
bool vte_image_set_host(struct tsm_vte *vte, bool host);

#endif /* TSM_VTE_INT_H */
