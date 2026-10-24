/*
 * libtsm - The kitty graphics protocol
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * The kitty graphics protocol (ADR-103 in aaronsb/kmscon, step 4a), written
 * from the spec, https://sw.kovidgoyal.net/kitty/graphics-protocol/, and never
 * from kitty's code (ADR-300). Where the spec leaves a choice, ADR-103 makes
 * it, or Ghostty (MIT) is the reference, named where it is followed.
 *
 * An APC whose first byte is G is a command: control keys up to the first
 * ';', at most 1 KiB of them, then a base64 payload. The payload is decoded as
 * it streams in, never collected whole: raw f=24 and f=32 pixels into the
 * pending image, whose bytes are reserved from the screen's budget when its
 * first chunk names its size, and a file or shared memory name into a buffer
 * of 4095 bytes. Raw data must hold at least s x v x bytes per pixel; what
 * follows is ignored, as a=q ignores it too.
 *
 * Direct transmission (t=d) may be chunked: m=1 on every chunk but the last,
 * and later chunks carry only m and q. Any other graphics command aborts a
 * pending upload. m means nothing to the other media, whose commands end at
 * their own ST. A transmission that fails part way swallows its remaining
 * chunks and answers once, at its last.
 *
 * What libtsm does not do itself goes to the host hook (ADR-103's Notes of
 * 2026-10-24): files, temporary files and shared memory, and PNG or zlib
 * data, which is buffered as it arrives, each growth reserved from the
 * budget. The hook takes a request when the command ends; the vte then
 * parses nothing until tsm_kitty_reply answers it, so commands, and their
 * replies, keep their order.
 *
 * Replies follow the spec: only to a command that carries i or I, q=1
 * suppressing OK and q=2 every reply. An a=q without i, with or without I,
 * is the exception: it is dropped at its ST, nothing read and nothing
 * answered under any q, as kitty 0.49.2 run as a black box drops it
 * (ADR-107, rounds 4 to 6). The spec's Unicode placeholder section
 * reads q=2 so: images are created "in quiet mode (q=2) so that there are no
 * responses from the terminal that could confuse the host application". A
 * delete answers only an error, as Ghostty's delete does
 * (src/terminal/kitty/graphics_exec.zig).
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "shl-llog.h"
#include "tsm-kitty.h"

#define LLOG_SUBSYSTEM "tsm-kitty"

/* the control keys an APC may carry, before its payload */
#define KITTY_KEYS_MAX 1024
/* a file, temporary file or shared memory name, decoded */
#define KITTY_NAME_MAX 4095

/* a bit for each key letter */
#define KEY(c) ((c) >= 'a' ? (uint64_t)1 << ((c) - 'a') : (uint64_t)1 << ((c) - 'A' + 26))

struct kitty_cmd {
	uint64_t seen;			/* KEY() of each key given */
	int err;			/* -EINVAL for a key libtsm cannot read */
	char a, t, o, d;
	uint32_t q, f, s, v, S, O, i, I, p, m, N;
	uint32_t x, y, w, h, X, Y, c, r, C, U, P, Q;
	int32_t z, H, V;
};

struct b64 {
	uint32_t acc;
	unsigned int n;			/* sextets in @acc */
};

enum kitty_phase {
	PHASE_IGNORE,			/* not a command, or its keys ran past 1 KiB */
	PHASE_FIRST,			/* the APC's first byte decides */
	PHASE_KEYS,
	PHASE_PAYLOAD,
};

/* where the APC's payload goes */
enum kitty_target {
	TARGET_NONE,
	TARGET_UPLOAD,			/* the pending direct upload */
	TARGET_NAME,			/* a file or shared memory name */
};

enum upload_mode {
	UPLOAD_NONE,			/* no upload pending */
	UPLOAD_SKIP,			/* refused: its chunks are swallowed */
	UPLOAD_RAW,			/* f=24 or f=32 into @buf */
	UPLOAD_COUNT,			/* a=q: counted, not kept */
	UPLOAD_HOST,			/* PNG or zlib into @buf, for the host */
};

struct kitty_upload {
	enum upload_mode mode;
	struct kitty_cmd cmd;		/* its first chunk's keys */
	int err;			/* what its last chunk answers, in SKIP */
	uint8_t *buf;
	size_t len;			/* bytes decoded */
	size_t want;			/* bytes s, v and f call for */
	size_t cap;			/* HOST: @buf's size, reserved */
	size_t excess;			/* bytes past @want, ignored */
	struct tsm_kitty_reservation res;
	struct b64 b64;
};

/* the request the host hook took, until tsm_kitty_reply answers it */
struct kitty_request {
	bool out;
	uint64_t id;
	struct kitty_cmd cmd;
	unsigned int flags;		/* TSM_VTE_IMAGE_* */
	uint64_t min_bytes;
	int refusal;			/* a discard's reply, libtsm's own */
};

struct tsm_kitty {
	struct tsm_vte *vte;
	struct tsm_screen *con;
	tsm_kitty_write_cb write;
	unsigned int max_width;
	unsigned int max_height;
	uint64_t work;

	/* the APC being parsed */
	enum kitty_phase phase;
	enum kitty_target target;
	char keys[KITTY_KEYS_MAX];
	size_t nkeys;
	struct kitty_cmd cmd;

	char name[KITTY_NAME_MAX + 1];
	size_t name_len;
	int name_err;			/* -EBADF (too long, a NUL) or -EINVAL */
	struct b64 name_b64;

	struct kitty_upload up;

	tsm_vte_image_cb hook;
	void *hook_data;
	bool in_hook;
	uint64_t last_request;
	struct kitty_request req;
	bool host_ok;			/* the input call running may hand out a
					   request, set by each call */
	/* bumped by every cancel and every hook change, so a request can tell
	 * that one happened while its hook ran */
	uint64_t cancels;
	uint64_t hook_changes;
};

/*
 * Control keys
 */

static bool parse_u32(const char *s, size_t len, uint32_t *out)
{
	uint64_t v = 0;
	size_t i;

	if (!len)
		return false;
	for (i = 0; i < len; ++i) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10 + (s[i] - '0');
		/* all integers are 32-bit (the spec's control data table) */
		if (v > UINT32_MAX)
			return false;
	}
	*out = v;
	return true;
}

static bool parse_i32(const char *s, size_t len, int32_t *out)
{
	uint32_t v;
	bool neg = len && s[0] == '-';

	if (!parse_u32(s + neg, len - neg, &v) || v > (uint32_t)INT32_MAX + neg)
		return false;
	*out = neg ? (int32_t)(0 - (int64_t)v) : (int32_t)v;
	return true;
}

static uint32_t *key_u32(struct kitty_cmd *cmd, char key)
{
	switch (key) {
	case 'q': return &cmd->q;
	case 'f': return &cmd->f;
	case 's': return &cmd->s;
	case 'v': return &cmd->v;
	case 'S': return &cmd->S;
	case 'O': return &cmd->O;
	case 'i': return &cmd->i;
	case 'I': return &cmd->I;
	case 'p': return &cmd->p;
	case 'm': return &cmd->m;
	case 'N': return &cmd->N;
	case 'x': return &cmd->x;
	case 'y': return &cmd->y;
	case 'w': return &cmd->w;
	case 'h': return &cmd->h;
	case 'X': return &cmd->X;
	case 'Y': return &cmd->Y;
	case 'c': return &cmd->c;
	case 'r': return &cmd->r;
	case 'C': return &cmd->C;
	case 'U': return &cmd->U;
	case 'P': return &cmd->P;
	case 'Q': return &cmd->Q;
	default: return NULL;
	}
}

static int32_t *key_i32(struct kitty_cmd *cmd, char key)
{
	switch (key) {
	case 'z': return &cmd->z;
	case 'H': return &cmd->H;
	case 'V': return &cmd->V;
	default: return NULL;
	}
}

static char *key_char(struct kitty_cmd *cmd, char key)
{
	switch (key) {
	case 'a': return &cmd->a;
	case 't': return &cmd->t;
	case 'o': return &cmd->o;
	case 'd': return &cmd->d;
	default: return NULL;
	}
}

/*
 * Reads the comma-separated KEY=VALUE pairs of @s. Empty pairs (a leading,
 * trailing or doubled comma, which the spec leaves undefined) are skipped,
 * unknown keys ignored, and a value libtsm cannot read sets @cmd->err while
 * the other keys are still read, so a refusal can name the image.
 */
static void parse_keys(struct kitty_cmd *cmd, const char *s, size_t len)
{
	const char *item, *end = s + len, *comma;
	size_t n;
	uint32_t *u;
	int32_t *i;
	char *ch, key;

	memset(cmd, 0, sizeof(*cmd));
	cmd->a = 't';
	cmd->t = 'd';
	cmd->d = 'a';
	cmd->f = 32;

	for (item = s; item < end; item = comma + 1) {
		comma = memchr(item, ',', end - item);
		if (!comma)
			comma = end;
		n = comma - item;
		if (!n)
			continue;
		key = item[0];
		if (n < 3 || item[1] != '=' ||
		    !((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'))) {
			cmd->err = -EINVAL;
			continue;
		}
		cmd->seen |= KEY(key);
		if ((u = key_u32(cmd, key))) {
			if (!parse_u32(item + 2, n - 2, u))
				cmd->err = -EINVAL;
		} else if ((i = key_i32(cmd, key))) {
			if (!parse_i32(item + 2, n - 2, i))
				cmd->err = -EINVAL;
		} else if ((ch = key_char(cmd, key))) {
			if (n != 3)
				cmd->err = -EINVAL;
			else
				*ch = item[2];
		}
	}

	if (!strchr("tTqpdfac", cmd->a) || !strchr("dfts", cmd->t) ||
	    ((cmd->seen & KEY('o')) && cmd->o != 'z') ||
	    (cmd->f != 24 && cmd->f != 32 && cmd->f != 100) ||
	    !strchr("aAiInNcCfFpPqQrRxXyYzZ", cmd->d))
		cmd->err = -EINVAL;
}

/* a=t, a=T, a=q and a=f carry data */
static bool is_transmission(const struct kitty_cmd *cmd)
{
	return cmd->a == 't' || cmd->a == 'T' || cmd->a == 'q' || cmd->a == 'f';
}

/*
 * An a=q without i, with or without I, which gets no reply and reads nothing
 * (ADR-103's Kitty graphics protocol section)
 */
static bool is_dropped(const struct kitty_cmd *cmd)
{
	return cmd->a == 'q' && !cmd->i;
}

/*
 * What libtsm refuses in @cmd's keys before any data is read: EINVAL for a
 * key it cannot read, i with I (spec), an action or key left to step 5
 * (animation, Unicode placeholders, relative placements), a raw format
 * without s and v or past max-graphic-size, and a placement whose offset is
 * not inside its cell or whose box is past TSM_SCREEN_IMAGE_MAX_CELLS.
 */
static int refusal(struct tsm_kitty *k, const struct kitty_cmd *cmd)
{
	if (cmd->err || (cmd->i && cmd->I))
		return -EINVAL;
	if (cmd->a == 'f' || cmd->a == 'a' || cmd->a == 'c' || cmd->U ||
	    (cmd->seen & (KEY('P') | KEY('Q') | KEY('H') | KEY('V'))))
		return -EINVAL;
	/* the image's bytes and its bookkeeping fit a 32-bit size_t */
	if (is_transmission(cmd) && cmd->f != 100 &&
	    (!cmd->s || !cmd->v || cmd->s > k->max_width || cmd->v > k->max_height ||
	     (uint64_t)cmd->s * cmd->v * 4 + TSM_SCREEN_IMAGE_OVERHEAD >
	     TSM_SCREEN_IMAGE_MAX_BYTES))
		return -EINVAL;
	if ((cmd->a == 'T' || cmd->a == 'p') &&
	    (cmd->X >= k->con->img.cell_width || cmd->Y >= k->con->img.cell_height ||
	     cmd->c > TSM_SCREEN_IMAGE_MAX_CELLS || cmd->r > TSM_SCREEN_IMAGE_MAX_CELLS))
		return -EINVAL;
	return 0;
}

/*
 * Replies
 */

static const char *reply_message(int code)
{
	switch (code) {
	case 0:
		return "OK";
	case -ENOENT:
		return "ENOENT:image not found";
	case -EBADF:
		/* every failure to read: the spec's one reply, kitty's text */
		return "EBADF:Failed to read image file";
	case -ENOSPC:
		return "ENOSPC:image storage is full";
	default:
		return "EINVAL:invalid argument";
	}
}

/*
 * Answers @cmd with @code (0 or a negative errno) for image @id, 0 when there
 * is none to name: ESC _ G i=<id>[,I=<n>][,p=<pid>];<message> ESC \, each key
 * only when set, as Ghostty's Response.encode writes it.
 */
static void reply(struct tsm_kitty *k, const struct kitty_cmd *cmd, uint32_t id, int code)
{
	char buf[160];
	int len;

	if ((!cmd->i && !cmd->I) || is_dropped(cmd))
		return;
	if (code ? cmd->q >= 2 : cmd->q >= 1)
		return;
	len = snprintf(buf, sizeof(buf), "\033_G");
	if (id)
		len += snprintf(buf + len, sizeof(buf) - len, "i=%u", id);
	if (cmd->I)
		len += snprintf(buf + len, sizeof(buf) - len, "%sI=%u", id ? "," : "",
				cmd->I);
	if (cmd->p)
		len += snprintf(buf + len, sizeof(buf) - len, ",p=%u", cmd->p);
	len += snprintf(buf + len, sizeof(buf) - len, ";%s\033\\", reply_message(code));
	k->write(k->vte, buf, len);
}

/*
 * Images and placements
 */

/*
 * @n pixels of straight-alpha R, G, B[, A] at @src, @bpp bytes each, as
 * premultiplied ARGB32 at @dst; returns whether every pixel is opaque. @dst
 * may be @src's buffer when its pixels lie at its start: each pixel is read
 * before it is written, from the last to the first.
 */
static bool rgba_premultiply(uint32_t *dst, const uint8_t *src, size_t n, unsigned int bpp)
{
	bool opaque = true;
	unsigned int r, g, b, a;
	size_t i;

	for (i = n; i-- > 0; ) {
		r = src[i * bpp];
		g = src[i * bpp + 1];
		b = src[i * bpp + 2];
		a = bpp == 4 ? src[i * bpp + 3] : 255;
		if (a != 255) {
			opaque = false;
			r = (r * a + 127) / 255;
			g = (g * a + 127) / 255;
			b = (b * a + 127) / 255;
		}
		dst[i] = (uint32_t)a << 24 | r << 16 | g << 8 | b;
	}
	return opaque;
}

static int place(struct tsm_kitty *k, const struct kitty_cmd *cmd, struct tsm_image *image)
{
	struct tsm_test_placement pl = {
		.image = image->key,
		/* a placement id on an anonymous image is ignored (spec) */
		.id = image->id ? cmd->p : 0,
		.x = cmd->X,
		.y = cmd->Y,
		.cols = cmd->c,
		.rows = cmd->r,
		.src_x = cmd->x,
		.src_y = cmd->y,
		.src_width = cmd->w,
		.src_height = cmd->h,
		.z = cmd->z,
	};
	int ret;

	ret = screen_kitty_place(k->con, image, &pl, !cmd->C);
	if (ret == -ENOSPC || ret == -ENOMEM)
		return -ENOSPC;
	return ret ? -EINVAL : 0;
}

/*
 * Stores @pixels, @cmd's transmitted image of @width x @height whose bytes
 * @res holds, places it for a=T, and answers.
 */
static void store(struct tsm_kitty *k, const struct kitty_cmd *cmd, unsigned int width,
		  unsigned int height, uint32_t *pixels, bool opaque,
		  struct tsm_kitty_reservation *res)
{
	struct tsm_test_image desc = {
		.width = width,
		.height = height,
		.kind = TSM_IMAGE_KITTY,
		.id = cmd->i,
		.number = cmd->I,
		.transient = cmd->N & 1,
	};
	struct tsm_image *image;
	int ret;

	/* a number alone: the terminal picks an unused id (spec) */
	if (!desc.id && desc.number)
		desc.id = screen_kitty_free_id(k->con);
	if (desc.number && !desc.id) {
		free(pixels);
		screen_kitty_unreserve(k->con, res);
		reply(k, cmd, cmd->i, -ENOSPC);
		return;
	}

	ret = screen_kitty_new(k->con, &desc, pixels, opaque, res, &image);
	if (ret) {
		reply(k, cmd, cmd->i, ret == -ENOSPC ? -ENOSPC : -EINVAL);
		return;
	}
	if (cmd->a == 'T')
		ret = place(k, cmd, image);
	reply(k, cmd, image->id, ret);
}

/*
 * The host hook
 */

/*
 * What libtsm asks the host for, from @cmd's keys (ADR-103's Notes of
 * 2026-10-24, What libtsm asks for): where a file's read starts and the most
 * it may read (O and S), the fewest bytes the image needs after inflating (s
 * x v x bytes per pixel for a raw format), and whether to inflate or decode
 * PNG. Direct PNG with zlib holds the inflated length to a non-zero S
 * exactly, as the spec words its rule; without S, or with S=0, which the
 * spec's default makes the same, it is decoded without the check.
 */
static void request_terms(const struct kitty_cmd *cmd, struct tsm_vte_image_request *r)
{
	bool file = cmd->t != 'd';

	r->offset = file ? cmd->O : 0;
	r->read_max = file ? cmd->S : 0;
	if (cmd->o == 'z')
		r->flags |= TSM_VTE_IMAGE_INFLATE;
	if (cmd->f != 100) {
		r->min_bytes = (uint64_t)cmd->s * cmd->v * (cmd->f / 8);
		return;
	}
	r->flags |= TSM_VTE_IMAGE_PNG;
	if (!file && cmd->o == 'z' && cmd->S) {
		r->flags |= TSM_VTE_IMAGE_EXACT;
		r->min_bytes = cmd->S;
	}
}

/*
 * The kitty reply to a host's @status (What libtsm answers): too few bytes
 * read from a file or shared memory is a read failure, EBADF, as kitty
 * answers it; too few inflated, or sent inline, is EINVAL, kitty's ENODATA
 * being none of ADR-103's codes.
 */
static int status_code(const struct kitty_request *req, unsigned int status)
{
	switch (status) {
	case TSM_VTE_IMAGE_OK:
		return 0;
	case TSM_VTE_IMAGE_TOO_SHORT:
		return req->cmd.t != 'd' && !(req->flags & TSM_VTE_IMAGE_INFLATE) ?
		       -EBADF : -EINVAL;
	case TSM_VTE_IMAGE_CORRUPT:
	case TSM_VTE_IMAGE_TOO_LARGE:
		return -EINVAL;
	case TSM_VTE_IMAGE_OVER_BUDGET:
		return -ENOSPC;
	default:
		/* UNREADABLE and FAILED */
		return -EBADF;
	}
}

/*
 * The request is answered with @status and, for TSM_VTE_IMAGE_OK, the bytes
 * at @data: a PNG's @width x @height pixels of RGBA, or the min_bytes bytes
 * of a raw format, checked by tsm_kitty_reply. A discard answers with
 * libtsm's own refusal, whatever the host says.
 */
static void request_finish(struct tsm_kitty *k, unsigned int status, unsigned int width,
			   unsigned int height, const uint8_t *data)
{
	struct kitty_request *req = &k->req;
	struct kitty_cmd cmd = req->cmd;
	struct tsm_kitty_reservation res;
	unsigned int bpp = 4;
	uint32_t *pixels;
	size_t n;
	bool opaque;

	req->out = false;
	if (req->flags & TSM_VTE_IMAGE_DISCARD) {
		reply(k, &cmd, cmd.i, req->refusal);
		return;
	}
	if (status != TSM_VTE_IMAGE_OK) {
		reply(k, &cmd, cmd.i, status_code(req, status));
		return;
	}
	if (!(req->flags & TSM_VTE_IMAGE_PNG)) {
		width = cmd.s;
		height = cmd.v;
		bpp = cmd.f / 8;
	}
	/* a raw format's s x v was checked before the hook, but
	 * max-graphic-size may have been lowered since */
	if (width > k->max_width || height > k->max_height ||
	    (uint64_t)width * height * 4 + TSM_SCREEN_IMAGE_OVERHEAD >
	    TSM_SCREEN_IMAGE_MAX_BYTES) {
		reply(k, &cmd, cmd.i, -EINVAL);
		return;
	}
	if (cmd.a == 'q') {
		reply(k, &cmd, cmd.i, 0);
		return;
	}

	n = (size_t)width * height;
	if (screen_kitty_reserve(k->con, n * 4 + TSM_SCREEN_IMAGE_OVERHEAD, cmd.i, &res)) {
		reply(k, &cmd, cmd.i, -ENOSPC);
		return;
	}
	pixels = malloc(n * 4);
	if (!pixels) {
		screen_kitty_unreserve(k->con, &res);
		reply(k, &cmd, cmd.i, -ENOSPC);
		return;
	}
	opaque = rgba_premultiply(pixels, data, n, bpp);
	store(k, &cmd, width, height, pixels, opaque, &res);
}

/*
 * Hands @cmd to the host hook: a medium named k->name, or @len bytes of
 * inline data; with @discard, a refused t=t or t=s command whose object the
 * host removes, and @refusal the client's reply. Taken, the vte pauses until
 * tsm_kitty_reply; refused, it is answered now as TSM_VTE_IMAGE_FAILED.
 */
static void request(struct tsm_kitty *k, const struct kitty_cmd *cmd, bool discard,
		    int refusal, const uint8_t *data, size_t len)
{
	struct tsm_vte_image_request r = {
		.id = ++k->last_request,
		.medium = cmd->t,
		.name = cmd->t == 'd' ? NULL : k->name,
		/* an empty payload still points somewhere */
		.data = cmd->t != 'd' ? NULL : data ? data : (const uint8_t *)"",
		.data_len = len,
	};
	uint64_t cancels, hook_changes;
	int ret;

	if (discard)
		r.flags = TSM_VTE_IMAGE_DISCARD;
	else
		request_terms(cmd, &r);
	k->name[k->name_len] = 0;
	k->req.id = r.id;
	k->req.cmd = *cmd;
	k->req.flags = r.flags;
	k->req.min_bytes = r.min_bytes;
	k->req.refusal = refusal;
	cancels = k->cancels;
	hook_changes = k->hook_changes;
	k->in_hook = true;
	ret = k->hook(k->vte, &r, k->hook_data);
	k->in_hook = false;
	/* a cancel from inside the hook dropped the request, with no reply;
	 * a hook changed from inside it answers it as outside */
	if (k->cancels != cancels)
		return;
	if (k->hook_changes != hook_changes) {
		request_finish(k, TSM_VTE_IMAGE_FAILED, 0, 0, NULL);
		return;
	}
	if (!ret) {
		k->req.out = true;
		return;
	}
	request_finish(k, TSM_VTE_IMAGE_FAILED, 0, 0, NULL);
}

/*
 * Direct transmission
 */

/* whether the input call running may buffer for, and hand a request to, the
 * hook; from tsm_vte_input, or with no hook, commands answer without one */
static bool host_open(const struct tsm_kitty *k)
{
	return k->hook && k->host_ok;
}

static void upload_abort(struct tsm_kitty *k)
{
	struct kitty_upload *up = &k->up;

	if (up->mode == UPLOAD_RAW) {
		free(up->buf);
		screen_kitty_unreserve(k->con, &up->res);
	} else if (up->mode == UPLOAD_HOST) {
		free(up->buf);
		screen_images_unreserve(k->con, up->cap);
	}
	up->buf = NULL;
	up->cap = 0;
	up->mode = UPLOAD_NONE;
}

/* the upload fails with @err; the rest of its data is ignored */
static void upload_fail(struct tsm_kitty *k, int err)
{
	struct kitty_upload *up = &k->up;

	if (up->mode == UPLOAD_SKIP)
		return;
	upload_abort(k);
	up->mode = UPLOAD_SKIP;
	up->err = err;
}

/*
 * The first chunk of a direct transmission. Raw data needs its pixels'
 * bytes reserved before they are allocated (ADR-103); a=q only counts what
 * arrives.
 */
static void upload_start(struct tsm_kitty *k)
{
	struct kitty_upload *up = &k->up;
	const struct kitty_cmd *cmd = &k->cmd;
	size_t n;
	int err;

	memset(up, 0, sizeof(*up));
	up->cmd = *cmd;
	up->mode = UPLOAD_SKIP;
	/* its chunks are swallowed and its last answers nothing */
	if (is_dropped(cmd))
		return;
	err = refusal(k, cmd);
	if (err) {
		up->err = err;
		return;
	}
	/* PNG and zlib are the host's: buffered for its hook when this input
	 * call may hand it a request, else EINVAL, with nothing buffered */
	if (cmd->f == 100 || cmd->o == 'z') {
		if (host_open(k))
			up->mode = UPLOAD_HOST;
		else
			up->err = -EINVAL;
		return;
	}

	n = (size_t)cmd->s * cmd->v;
	up->want = n * (cmd->f / 8);
	if (cmd->a == 'q') {
		up->mode = UPLOAD_COUNT;
		return;
	}
	if (screen_kitty_reserve(k->con, n * 4 + TSM_SCREEN_IMAGE_OVERHEAD, cmd->i,
				 &up->res)) {
		up->err = -ENOSPC;
		return;
	}
	up->buf = malloc(n * 4);
	if (!up->buf) {
		screen_kitty_unreserve(k->con, &up->res);
		up->err = -ENOSPC;
		return;
	}
	up->mode = UPLOAD_RAW;
}

/*
 * Room in the host buffer for @n more bytes, each growth reserved before it
 * is allocated; a refused reservation fails the upload with ENOSPC.
 */
static bool upload_grow(struct tsm_kitty *k, size_t n)
{
	struct kitty_upload *up = &k->up;
	size_t cap = up->cap ? up->cap : 4096;
	uint8_t *buf;

	if (n <= up->cap - up->len)
		return true;
	while (cap - up->len < n) {
		if (cap > SIZE_MAX / 2) {
			upload_fail(k, -ENOSPC);
			return false;
		}
		cap *= 2;
	}
	if (screen_images_reserve(k->con, cap - up->cap)) {
		upload_fail(k, -ENOSPC);
		return false;
	}
	buf = realloc(up->buf, cap);
	if (!buf) {
		screen_images_unreserve(k->con, cap - up->cap);
		upload_fail(k, -ENOSPC);
		return false;
	}
	k->work += cap - up->cap;
	up->buf = buf;
	up->cap = cap;
	return true;
}

/* @n decoded bytes for the upload */
static void upload_data(struct tsm_kitty *k, const uint8_t *data, size_t n)
{
	struct kitty_upload *up = &k->up;

	if (up->mode == UPLOAD_HOST) {
		/* a later input call that cannot hand the request out (the
		 * upload begun in tsm_vte_input_bounded, continued in
		 * tsm_vte_input) buffers nothing more: the upload fails as it
		 * would have without a hook, its bytes given back */
		if (!host_open(k))
			upload_fail(k, -EINVAL);
		else if (upload_grow(k, n)) {
			memcpy(up->buf + up->len, data, n);
			up->len += n;
		}
		return;
	}

	/* data past s x v x bytes per pixel is ignored, as kitty ignores it
	 * (the operator's decision of 2026-10-24) */
	if (n > up->want - up->len) {
		up->excess += n - (up->want - up->len);
		n = up->want - up->len;
	}
	if (up->mode == UPLOAD_RAW)
		memcpy(up->buf + up->len, data, n);
	up->len += n;
}

/*
 * Data past s x v x bytes per pixel, at debug severity, since a video can send
 * it every frame. Written with llog_dprintf, which every build keeps, so the
 * host's own log level decides whether it shows.
 */
static void log_excess(struct tsm_kitty *k, const struct kitty_cmd *cmd, size_t excess,
		       char medium)
{
	char image[32];

	if (!excess)
		return;
	if (cmd->i)
		snprintf(image, sizeof(image), "i=%u", cmd->i);
	else if (cmd->I)
		snprintf(image, sizeof(image), "I=%u", cmd->I);
	else
		snprintf(image, sizeof(image), "(anonymous)");
	llog_dprintf(k->con->llog, k->con->llog_data, LLOG_DEBUG,
		     "kitty image %s, t=%c: %zu bytes past s x v ignored", image, medium,
		     excess);
}

/* its last chunk has arrived */
static void upload_finish(struct tsm_kitty *k)
{
	struct kitty_upload *up = &k->up;
	struct kitty_cmd cmd = up->cmd;
	uint32_t *pixels;
	uint8_t *buf;
	size_t n, cap;
	bool opaque;

	switch (up->mode) {
	case UPLOAD_SKIP:
		up->mode = UPLOAD_NONE;
		reply(k, &cmd, cmd.i, up->err);
		return;
	case UPLOAD_COUNT:
		up->mode = UPLOAD_NONE;
		log_excess(k, &cmd, up->excess, 'd');
		reply(k, &cmd, cmd.i, up->len < up->want ? -EINVAL : 0);
		return;
	case UPLOAD_HOST:
		/* the buffer is this call's now: a cancel from inside the hook
		 * finds no upload to free */
		buf = up->buf;
		cap = up->cap;
		up->buf = NULL;
		up->cap = 0;
		up->mode = UPLOAD_NONE;
		if (host_open(k))
			request(k, &cmd, false, 0, buf, up->len);
		else
			reply(k, &cmd, cmd.i, -EINVAL);
		/* the request's data was valid only during the hook call */
		free(buf);
		screen_images_unreserve(k->con, cap);
		return;
	case UPLOAD_RAW:
		break;
	default:
		return;
	}

	log_excess(k, &cmd, up->excess, 'd');
	/* raw data must hold at least s x v x bytes per pixel */
	if (up->len < up->want) {
		upload_abort(k);
		reply(k, &cmd, cmd.i, -EINVAL);
		return;
	}
	n = (size_t)cmd.s * cmd.v;
	pixels = (uint32_t *)up->buf;
	opaque = rgba_premultiply(pixels, up->buf, n, cmd.f / 8);
	k->work += n;
	up->buf = NULL;
	up->mode = UPLOAD_NONE;
	store(k, &cmd, cmd.s, cmd.v, pixels, opaque, &up->res);
}

/*
 * A continuation of the pending upload carries only m and q (spec); an a=f
 * upload's chunks also carry a=f, which the spec asks of animation frames.
 */
static bool is_continuation(struct tsm_kitty *k, const struct kitty_cmd *cmd)
{
	uint64_t keys = KEY('m') | KEY('q');

	if (k->up.cmd.a == 'f' && cmd->a == 'f')
		keys |= KEY('a');
	return !(cmd->seen & ~keys);
}

/*
 * Payload
 */

static int b64_value(unsigned char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

/* @n decoded bytes for the APC's target */
static void emit(struct tsm_kitty *k, const uint8_t *data, size_t n)
{
	size_t i;

	if (k->target == TARGET_UPLOAD) {
		upload_data(k, data, n);
		return;
	}
	for (i = 0; i < n && !k->name_err; ++i) {
		if (!data[i] || k->name_len == KITTY_NAME_MAX)
			k->name_err = -EBADF;
		else
			k->name[k->name_len++] = data[i];
	}
}

/* whether the target still takes data */
static bool target_open(struct tsm_kitty *k)
{
	if (k->target == TARGET_UPLOAD)
		return k->up.mode == UPLOAD_RAW || k->up.mode == UPLOAD_COUNT ||
		       k->up.mode == UPLOAD_HOST;
	return k->target == TARGET_NAME && !k->name_err;
}

static void payload_error(struct tsm_kitty *k)
{
	if (k->target == TARGET_UPLOAD)
		upload_fail(k, -EINVAL);
	else if (k->target == TARGET_NAME && !k->name_err)
		k->name_err = -EINVAL;
}

static struct b64 *target_b64(struct tsm_kitty *k)
{
	return k->target == TARGET_UPLOAD ? &k->up.b64 : &k->name_b64;
}

/* what a quad's @n sextets hold: a pad or the payload's end flushes them */
static void b64_flush(struct tsm_kitty *k, struct b64 *st)
{
	uint8_t out[2];

	if (!target_open(k)) {
		st->n = 0;
		return;
	}
	if (st->n == 1) {
		payload_error(k);
	} else if (st->n >= 2) {
		st->acc <<= 6 * (4 - st->n);
		out[0] = st->acc >> 16;
		out[1] = st->acc >> 8;
		emit(k, out, st->n - 1);
	}
	st->n = 0;
}

/* base64 decoding, a quad at a time; a pad ends the quad it is in */
static void payload(struct tsm_kitty *k, const unsigned char *s, size_t len)
{
	struct b64 *st = target_b64(k);
	uint8_t out[3];
	size_t i;
	int v;

	k->work += len;
	for (i = 0; i < len && target_open(k); ++i) {
		v = b64_value(s[i]);
		if (v < 0) {
			if (s[i] == '=')
				b64_flush(k, st);
			else
				payload_error(k);
			continue;
		}
		st->acc = st->acc << 6 | v;
		if (++st->n == 4) {
			out[0] = st->acc >> 16;
			out[1] = st->acc >> 8;
			out[2] = st->acc;
			emit(k, out, 3);
			st->n = 0;
		}
	}
}

/*
 * Commands
 */

/* the keys are read: where the payload goes */
static void command_start(struct tsm_kitty *k)
{
	struct kitty_cmd *cmd = &k->cmd;

	parse_keys(cmd, k->keys, k->nkeys);
	k->target = TARGET_NONE;
	if (k->up.mode != UPLOAD_NONE && is_continuation(k, cmd)) {
		if (cmd->err)
			upload_fail(k, -EINVAL);
		/* a chunk's q of 1 or 2 holds from then on, and q=0 keeps the
		 * first chunk's, as Ghostty keeps it; the spec is silent */
		if (cmd->q)
			k->up.cmd.q = cmd->q;
		k->target = TARGET_UPLOAD;
		return;
	}

	/* any other graphics command aborts the pending upload (spec) */
	upload_abort(k);
	if (!is_transmission(cmd))
		return;
	if (cmd->t == 'd') {
		upload_start(k);
		k->target = TARGET_UPLOAD;
	} else {
		k->name_len = 0;
		k->name_err = 0;
		memset(&k->name_b64, 0, sizeof(k->name_b64));
		k->target = TARGET_NAME;
	}
}

/*
 * A transmission from a file, temporary file or shared memory, which the
 * host reads. An empty name, a longer one or one holding NUL answers EBADF
 * with no request, as an unreadable file does. A t=t or t=s command libtsm
 * refuses is still handed over, as a discard, since the spec gives the
 * object's removal to the terminal. Without a hook (or from tsm_vte_input)
 * every one answers EBADF.
 */
static void media_finish(struct tsm_kitty *k)
{
	const struct kitty_cmd *cmd = &k->cmd;
	bool host = host_open(k);
	int ret, name_err = k->name_err;

	if (is_dropped(cmd))
		return;
	if (!name_err && !k->name_len)
		name_err = -EBADF;
	ret = refusal(k, cmd);
	if (ret) {
		if (host && !name_err && (cmd->t == 't' || cmd->t == 's'))
			request(k, cmd, true, ret, NULL, 0);
		else
			reply(k, cmd, cmd->i, ret);
		return;
	}
	if (name_err || !host) {
		reply(k, cmd, cmd->i, name_err ? name_err : -EBADF);
		return;
	}
	request(k, cmd, false, 0, NULL, 0);
}

/* a=p: the image i names, or the newest with number I, at the cursor */
static void put(struct tsm_kitty *k)
{
	const struct kitty_cmd *cmd = &k->cmd;
	struct tsm_image *image = NULL;
	int ret;

	ret = refusal(k, cmd);
	if (!ret && !cmd->i && !cmd->I)
		ret = -EINVAL;
	if (!ret) {
		image = screen_kitty_find(k->con, cmd->i, cmd->I);
		ret = image ? place(k, cmd, image) : -ENOENT;
	}
	reply(k, cmd, image ? image->id : cmd->i, ret);
}

static void delete(struct tsm_kitty *k)
{
	const struct kitty_cmd *cmd = &k->cmd;
	struct tsm_kitty_delete d = {
		.what = cmd->d,
		.id = cmd->i,
		.number = cmd->I,
		.placement = cmd->p,
		.x = cmd->x,
		.y = cmd->y,
		.z = cmd->z,
	};

	if (cmd->err || (cmd->i && cmd->I)) {
		reply(k, cmd, cmd->i, -EINVAL);
		return;
	}
	/* animation frames: nothing to delete until animation exists */
	if (d.what == 'f' || d.what == 'F')
		return;
	d.cursor_x = k->con->cursor_x < k->con->size_x ? k->con->cursor_x :
							 k->con->size_x - 1;
	d.cursor_y = k->con->cursor_y;
	screen_kitty_delete(k->con, &d);
}

static void command_finish(struct tsm_kitty *k)
{
	const struct kitty_cmd *cmd = &k->cmd;

	switch (k->target) {
	case TARGET_UPLOAD:
		b64_flush(k, &k->up.b64);
		if (!cmd->m)
			upload_finish(k);
		return;
	case TARGET_NAME:
		b64_flush(k, &k->name_b64);
		media_finish(k);
		return;
	case TARGET_NONE:
		break;
	}

	if (cmd->a == 'p')
		put(k);
	else if (cmd->a == 'd')
		delete(k);
	else
		reply(k, cmd, cmd->i, cmd->err ? cmd->err : -EINVAL);
}

/*
 * The vte's side
 */

int tsm_kitty_new(struct tsm_kitty **out, struct tsm_vte *vte, struct tsm_screen *con,
		  tsm_kitty_write_cb write)
{
	struct tsm_kitty *k;

	k = calloc(1, sizeof(*k));
	if (!k)
		return -ENOMEM;
	k->vte = vte;
	k->con = con;
	k->write = write;
	k->max_width = TSM_SCREEN_IMAGE_MAX;
	k->max_height = TSM_SCREEN_IMAGE_MAX;
	*out = k;
	return 0;
}

void tsm_kitty_free(struct tsm_kitty *k)
{
	if (!k)
		return;
	upload_abort(k);
	free(k);
}

void tsm_kitty_set_max_size(struct tsm_kitty *k, unsigned int width, unsigned int height)
{
	k->max_width = width;
	k->max_height = height;
}

bool tsm_kitty_set_host(struct tsm_kitty *k, bool host_ok)
{
	bool old = k->host_ok;

	k->host_ok = host_ok;
	return old;
}

void tsm_kitty_begin(struct tsm_kitty *k)
{
	k->phase = PHASE_FIRST;
	k->target = TARGET_NONE;
	k->nkeys = 0;
}

void tsm_kitty_feed(struct tsm_kitty *k, uint32_t c)
{
	unsigned char b = c < 0x80 ? c : 0x80;

	switch (k->phase) {
	case PHASE_FIRST:
		k->phase = c == 'G' ? PHASE_KEYS : PHASE_IGNORE;
		return;
	case PHASE_KEYS:
		if (c == ';') {
			command_start(k);
			k->phase = PHASE_PAYLOAD;
		} else if (k->nkeys == KITTY_KEYS_MAX) {
			/* a longer key section: the whole APC is ignored */
			k->phase = PHASE_IGNORE;
		} else {
			k->keys[k->nkeys++] = b;
		}
		return;
	case PHASE_PAYLOAD:
		payload(k, &b, 1);
		return;
	case PHASE_IGNORE:
		return;
	}
}

bool tsm_kitty_in_payload(struct tsm_kitty *k)
{
	return k->phase == PHASE_PAYLOAD;
}

void tsm_kitty_feed_run(struct tsm_kitty *k, const char *u8, size_t len)
{
	payload(k, (const unsigned char *)u8, len);
}

void tsm_kitty_end(struct tsm_kitty *k, uint32_t data)
{
	enum kitty_phase phase = k->phase;

	k->phase = PHASE_IGNORE;
	if (phase == PHASE_FIRST || phase == PHASE_IGNORE)
		return;
	if (phase == PHASE_KEYS)
		command_start(k);

	/* CAN and SUB abort the command, an upload it belongs to included */
	if (data == 0x18 || data == 0x1a) {
		if (k->target == TARGET_UPLOAD)
			upload_abort(k);
		return;
	}
	command_finish(k);
}

void tsm_kitty_cancel(struct tsm_kitty *k)
{
	if (!k)
		return;
	upload_abort(k);
	k->phase = PHASE_IGNORE;
	/* a request out is dropped, with no reply to the client */
	k->req.out = false;
	++k->cancels;
}

bool tsm_kitty_paused(struct tsm_kitty *k)
{
	return k->req.out;
}

void tsm_kitty_set_hook(struct tsm_kitty *k, tsm_vte_image_cb hook, void *data)
{
	if (k->req.out)
		request_finish(k, TSM_VTE_IMAGE_FAILED, 0, 0, NULL);
	k->hook = hook;
	k->hook_data = data;
	++k->hook_changes;
}

bool tsm_kitty_in_hook(struct tsm_kitty *k)
{
	return k->in_hook;
}

/* whether @len bytes are @w x @h pixels of RGBA, computed without a wrap */
static bool rgba_len(unsigned int w, unsigned int h, size_t len)
{
	uint64_t n = (uint64_t)w * h;

	return w && h && n <= UINT64_MAX / 4 && n * 4 == len;
}

int tsm_kitty_reply(struct tsm_kitty *k, uint64_t id, const struct tsm_vte_image_result *res)
{
	const struct kitty_request *req = &k->req;

	if (k->in_hook)
		return -EBUSY;
	if (!req->out || id != req->id)
		return -ENOENT;
	/* a discard takes any result, and answers with libtsm's refusal */
	if (req->flags & TSM_VTE_IMAGE_DISCARD) {
		request_finish(k, TSM_VTE_IMAGE_FAILED, 0, 0, NULL);
		return 0;
	}
	/* a result that disagrees with its request ends it with EBADF, so a
	 * host's mistake cannot leave the vte paused */
	if (!res || res->status > TSM_VTE_IMAGE_FAILED ||
	    (res->status == TSM_VTE_IMAGE_OK &&
	     (!res->data ||
	      (req->flags & TSM_VTE_IMAGE_PNG ?
	       !rgba_len(res->width, res->height, res->len) :
	       res->len != req->min_bytes)))) {
		request_finish(k, TSM_VTE_IMAGE_FAILED, 0, 0, NULL);
		return -EINVAL;
	}
	request_finish(k, res->status, res->width, res->height, res->data);
	return 0;
}

uint64_t tsm_kitty_work(struct tsm_kitty *k)
{
	return k ? k->work : 0;
}
