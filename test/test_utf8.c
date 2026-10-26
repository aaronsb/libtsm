/*
 * TSM - UTF-8 State Machine Tests
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
 * The decoder's contract:
 * 1. A well-formed sequence (Unicode Table 3-7) decodes to its scalar value.
 * 2. Each maximal subpart of an ill-formed sequence is one U+FFFD (Unicode
 *    3.9); a byte that can neither start nor continue a sequence (80-BF
 *    alone, C0, C1, F5-FF) is one U+FFFD.
 * 3. The byte that ends an ill-formed sequence is not consumed: it is fed
 *    again from the start state.
 * 4. Overlongs, surrogates and values above U+10FFFF are ill-formed.
 * 5. The result does not depend on the signedness of char.
 * 6. C2 80-9F still decode to the C1 controls.
 */

#include <stdio.h>
#include <string.h>
#include "test_common.h"
#include "libtsm.h"
#include "libtsm-int.h"

#define FFFD TSM_UCS4_REPLACEMENT
#define OUT_MAX 16

/*
 * Feeds @len bytes as the vte does, from the start state, and stores what
 * comes out; returns how many codepoints that is. A sequence the input
 * leaves unfinished gives nothing.
 */
static size_t decode(struct tsm_utf8_mach *mach, const uint8_t *in, size_t len,
		     uint32_t *out)
{
	size_t i = 0, n = 0;
	int state;

	tsm_utf8_mach_reset(mach);
	while (i < len) {
		state = tsm_utf8_mach_feed(mach, (char)in[i]);
		if (state != TSM_UTF8_RETRY)
			++i;
		if (state == TSM_UTF8_ACCEPT || state == TSM_UTF8_REJECT ||
		    state == TSM_UTF8_RETRY) {
			/* check marks each assertion that passes, which the
			 * exhaustive test cannot afford here */
			if (n == OUT_MAX)
				ck_abort_msg("more than %d codepoints", OUT_MAX);
			out[n++] = tsm_utf8_mach_get(mach);
		}
	}
	return n;
}

static void hex(char *buf, size_t size, const uint8_t *in, size_t len)
{
	size_t i, pos = 0;

	buf[0] = 0;
	for (i = 0; i < len && pos + 4 < size; ++i)
		pos += snprintf(buf + pos, size - pos, "%s%02X", i ? " " : "", in[i]);
}

static void expect_decode(const char *in, size_t len, const uint32_t *want,
			  size_t nwant)
{
	struct tsm_utf8_mach *mach;
	uint32_t out[OUT_MAX];
	char buf[64];
	size_t n, i;

	ck_assert_int_eq(tsm_utf8_mach_new(&mach), 0);
	n = decode(mach, (const uint8_t *)in, len, out);
	tsm_utf8_mach_free(mach);

	hex(buf, sizeof(buf), (const uint8_t *)in, len);
	ck_assert_msg(n == nwant, "%s: %zu codepoints, expected %zu", buf, n, nwant);
	for (i = 0; i < n; ++i)
		ck_assert_msg(out[i] == want[i], "%s: codepoint %zu is U+%04X, expected U+%04X",
			      buf, i, (unsigned)out[i], (unsigned)want[i]);
}

/* EXPECT("bytes", codepoints...) */
#define EXPECT(_in, ...)                                                  \
	expect_decode((_in), sizeof(_in) - 1, (const uint32_t[]){ __VA_ARGS__ }, \
		      sizeof((const uint32_t[]){ __VA_ARGS__ }) / sizeof(uint32_t))

/* Rule 1: Table 3-7 at each row's bounds */
START_TEST(test_utf8_well_formed)
{
	EXPECT("\x00", 0x00);
	EXPECT("\x7f", 0x7f);
	EXPECT("\xc2\x80", 0x80);
	EXPECT("\xdf\xbf", 0x7ff);
	EXPECT("\xe0\xa0\x80", 0x800);
	EXPECT("\xe0\xbf\xbf", 0xfff);
	EXPECT("\xe1\x80\x80", 0x1000);
	EXPECT("\xec\xbf\xbf", 0xcfff);
	EXPECT("\xed\x80\x80", 0xd000);
	EXPECT("\xed\x9f\xbf", 0xd7ff);
	EXPECT("\xee\x80\x80", 0xe000);
	EXPECT("\xef\xbf\xbf", 0xffff);
	EXPECT("\xf0\x90\x80\x80", 0x10000);
	EXPECT("\xf0\xbf\xbf\xbf", 0x3ffff);
	EXPECT("\xf1\x80\x80\x80", 0x40000);
	EXPECT("\xf3\xbf\xbf\xbf", 0xfffff);
	EXPECT("\xf4\x80\x80\x80", 0x100000);
	EXPECT("\xf4\x8f\xbf\xbf", 0x10ffff);
	EXPECT("a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80z", 'a', 0xe9, 0x20ac, 0x1f600, 'z');
}
END_TEST

/* Rule 2: one U+FFFD per maximal subpart, and per byte that starts nothing */
START_TEST(test_utf8_maximal_subparts)
{
	unsigned int b;
	char in[2] = { 0 };

	for (b = 0x80; b <= 0xbf; ++b) {
		in[0] = (char)b;
		expect_decode(in, 1, (const uint32_t[]){ FFFD }, 1);
	}
	EXPECT("\xc0", FFFD);
	EXPECT("\xc1", FFFD);
	for (b = 0xf5; b <= 0xff; ++b) {
		in[0] = (char)b;
		expect_decode(in, 1, (const uint32_t[]){ FFFD }, 1);
	}
	EXPECT("\x80\x80\xbf", FFFD, FFFD, FFFD);
	/* a truncated sequence is one U+FFFD, whatever its length */
	EXPECT("\xc3" "a", FFFD, 'a');
	EXPECT("\xe2\x82" "a", FFFD, 'a');
	EXPECT("\xf0\x9f\x98" "a", FFFD, 'a');
	EXPECT("\xe2\x82\xe2\x82\xac", FFFD, 0x20ac);
	/* Unicode 3.9, Table 3-8 */
	EXPECT("\x61\xf1\x80\x80\xe1\x80\xc2\x62\x80\x63\x80\xbf\x64",
	       0x61, FFFD, FFFD, FFFD, 0x62, FFFD, 0x63, FFFD, FFFD, 0x64);
}
END_TEST

/* Rule 3: the byte that ends an ill-formed sequence is processed again */
START_TEST(test_utf8_breaking_byte_kept)
{
	EXPECT("\xe2\x1b", FFFD, 0x1b);
	EXPECT("\xe2\x41", FFFD, 'A');
	EXPECT("\xf0\x9f\x41", FFFD, 'A');
	EXPECT("\xc3\x1b[", FFFD, 0x1b, '[');
	/* a lead byte breaks the sequence before it and starts its own */
	EXPECT("\xe2\xc3\xa9", FFFD, 0xe9);
	EXPECT("\xf0\x9f\xf0\x9f\x98\x80", FFFD, 0x1f600);
}
END_TEST

/* Rule 4: overlongs */
START_TEST(test_utf8_overlong)
{
	EXPECT("\xc0\x9b", FFFD, FFFD);
	EXPECT("\xc0\x80", FFFD, FFFD);
	EXPECT("\xc1\xbf", FFFD, FFFD);
	EXPECT("\xe0\x80\x9b", FFFD, FFFD, FFFD);
	EXPECT("\xe0\x9f\xbf", FFFD, FFFD, FFFD);
	EXPECT("\xf0\x80\x80\x9b", FFFD, FFFD, FFFD, FFFD);
	EXPECT("\xf0\x8f\xbf\xbf", FFFD, FFFD, FFFD, FFFD);
}
END_TEST

/* Rule 4: surrogates */
START_TEST(test_utf8_surrogate)
{
	EXPECT("\xed\xa0\x80", FFFD, FFFD, FFFD);
	EXPECT("\xed\xaf\xbf", FFFD, FFFD, FFFD);
	EXPECT("\xed\xb0\x80", FFFD, FFFD, FFFD);
	EXPECT("\xed\xbf\xbf", FFFD, FFFD, FFFD);
	EXPECT("\xed\xa0\xbd\xed\xb8\x80", FFFD, FFFD, FFFD, FFFD, FFFD, FFFD);
}
END_TEST

/* Rule 4: values above U+10FFFF */
START_TEST(test_utf8_above_max)
{
	EXPECT("\xf4\x90\x80\x80", FFFD, FFFD, FFFD, FFFD);
	EXPECT("\xf4\xbf\xbf\xbf", FFFD, FFFD, FFFD, FFFD);
	EXPECT("\xf5\x80\x80\x80", FFFD, FFFD, FFFD, FFFD);
	EXPECT("\xf7\xbf\xbf\xbf", FFFD, FFFD, FFFD, FFFD);
	EXPECT("\xf8\x88\x80\x80\x80", FFFD, FFFD, FFFD, FFFD, FFFD);
	EXPECT("\xfc\x84\x80\x80\x80\x80", FFFD, FFFD, FFFD, FFFD, FFFD, FFFD);
	EXPECT("\xfe\xff", FFFD, FFFD);
}
END_TEST

/* Rule 6: C2 80-9F are the C1 controls */
START_TEST(test_utf8_c1)
{
	unsigned int b;
	char in[2] = { (char)0xc2, 0 };

	for (b = 0x80; b <= 0x9f; ++b) {
		in[1] = (char)b;
		expect_decode(in, 2, (const uint32_t[]){ b }, 1);
	}
}
END_TEST

/*
 * The rows of Unicode Table 3-7: the lead bytes, then the range of each
 * byte that follows
 */
static const struct {
	uint8_t lead_lo, lead_hi;
	size_t follow;
	uint8_t lo[3], hi[3];
} table_3_7[] = {
	{ 0xc2, 0xdf, 1, { 0x80 }, { 0xbf } },
	{ 0xe0, 0xe0, 2, { 0xa0, 0x80 }, { 0xbf, 0xbf } },
	{ 0xe1, 0xec, 2, { 0x80, 0x80 }, { 0xbf, 0xbf } },
	{ 0xed, 0xed, 2, { 0x80, 0x80 }, { 0x9f, 0xbf } },
	{ 0xee, 0xef, 2, { 0x80, 0x80 }, { 0xbf, 0xbf } },
	{ 0xf0, 0xf0, 3, { 0x90, 0x80, 0x80 }, { 0xbf, 0xbf, 0xbf } },
	{ 0xf1, 0xf3, 3, { 0x80, 0x80, 0x80 }, { 0xbf, 0xbf, 0xbf } },
	{ 0xf4, 0xf4, 3, { 0x80, 0x80, 0x80 }, { 0x8f, 0xbf, 0xbf } },
};

/*
 * The contract worked out from the table over the whole input: at each
 * position, a well-formed sequence gives its value, a maximal subpart one
 * U+FFFD, and an unfinished sequence at the end nothing
 */
static size_t oracle(const uint8_t *in, size_t len, uint32_t *out)
{
	size_t i = 0, n = 0, r, k;
	uint32_t cp;

	while (i < len) {
		for (r = 0; r < sizeof(table_3_7) / sizeof(*table_3_7); ++r)
			if (in[i] >= table_3_7[r].lead_lo && in[i] <= table_3_7[r].lead_hi)
				break;
		if (in[i] < 0x80) {
			out[n++] = in[i++];
			continue;
		}
		if (r == sizeof(table_3_7) / sizeof(*table_3_7)) {
			out[n++] = FFFD;
			++i;
			continue;
		}
		cp = in[i] & (0x7f >> (table_3_7[r].follow + 1));
		for (k = 0; k < table_3_7[r].follow && i + 1 + k < len; ++k) {
			if (in[i + 1 + k] < table_3_7[r].lo[k] ||
			    in[i + 1 + k] > table_3_7[r].hi[k])
				break;
			cp = cp << 6 | (in[i + 1 + k] & 0x3f);
		}
		if (k == table_3_7[r].follow) {
			out[n++] = cp;
		} else if (i + 1 + k == len) {
			break;
		} else {
			out[n++] = FFFD;
		}
		i += 1 + k;
	}
	return n;
}

static bool is_control(uint32_t cp)
{
	return cp < 0x20 || (cp >= 0x7f && cp <= 0x9f);
}

/*
 * Compares the machine with the oracle on @in, and counts the controls it
 * gave against the controls @in encodes well-formed: each byte below 0x80
 * that is one, and each C2 80-9F
 */
static void check_input(struct tsm_utf8_mach *mach, const uint8_t *in, size_t len)
{
	uint32_t out[OUT_MAX], want[OUT_MAX];
	size_t n, nwant, i, controls = 0, encoded = 0, escs = 0;
	char buf[64];

	n = decode(mach, in, len, out);
	nwant = oracle(in, len, want);
	for (i = 0; i < n; ++i) {
		controls += is_control(out[i]);
		escs += out[i] == 0x1b;
	}
	for (i = 0; i < len; ++i) {
		encoded += in[i] < 0x80 && is_control(in[i]);
		encoded += in[i] == 0xc2 && i + 1 < len && in[i + 1] >= 0x80 &&
			   in[i + 1] <= 0x9f;
		escs -= in[i] == 0x1b;
	}
	if (n == nwant && !memcmp(out, want, n * sizeof(*out)) &&
	    controls == encoded && !escs)
		return;

	hex(buf, sizeof(buf), in, len);
	ck_assert_msg(escs == 0, "%s: an ESC no 0x1B byte encodes", buf);
	ck_assert_msg(controls == encoded, "%s: %zu controls, %zu encoded", buf,
		      controls, encoded);
	ck_assert_msg(n == nwant, "%s: %zu codepoints, expected %zu", buf, n, nwant);
	for (i = 0; i < n; ++i)
		ck_assert_msg(out[i] == want[i], "%s: codepoint %zu is U+%04X, expected U+%04X",
			      buf, i, (unsigned)out[i], (unsigned)want[i]);
}

/*
 * Every input of one, two and three bytes, and every four bytes that start
 * F0-F4 80-BF 80-BF: the machine agrees with the oracle, and no ill-formed
 * input yields ESC or any other C0 or C1 control
 */
START_TEST(test_utf8_exhaustive)
{
	struct tsm_utf8_mach *mach;
	uint8_t in[4];
	unsigned int a, b, c, d;

	ck_assert_int_eq(tsm_utf8_mach_new(&mach), 0);
	for (a = 0; a < 256; ++a) {
		in[0] = a;
		check_input(mach, in, 1);
		for (b = 0; b < 256; ++b) {
			in[1] = b;
			check_input(mach, in, 2);
			for (c = 0; c < 256; ++c) {
				in[2] = c;
				check_input(mach, in, 3);
			}
		}
	}
	for (a = 0xf0; a <= 0xf4; ++a)
		for (b = 0x80; b <= 0xbf; ++b)
			for (c = 0x80; c <= 0xbf; ++c)
				for (d = 0; d < 256; ++d) {
					in[0] = a;
					in[1] = b;
					in[2] = c;
					in[3] = d;
					check_input(mach, in, 4);
				}
	tsm_utf8_mach_free(mach);
}
END_TEST

/* a sequence split across calls decodes as one; idle only between them */
START_TEST(test_utf8_idle)
{
	struct tsm_utf8_mach *mach;

	ck_assert_int_eq(tsm_utf8_mach_new(&mach), 0);
	ck_assert(tsm_utf8_mach_idle(mach));
	ck_assert_int_eq(tsm_utf8_mach_feed(mach, (char)0xe2), TSM_UTF8_EXPECT2);
	ck_assert(!tsm_utf8_mach_idle(mach));
	ck_assert_int_eq(tsm_utf8_mach_feed(mach, (char)0x82), TSM_UTF8_EXPECT1);
	ck_assert_int_eq(tsm_utf8_mach_feed(mach, (char)0xac), TSM_UTF8_ACCEPT);
	ck_assert_uint_eq(tsm_utf8_mach_get(mach), 0x20ac);
	ck_assert(tsm_utf8_mach_idle(mach));

	ck_assert_int_eq(tsm_utf8_mach_feed(mach, (char)0xe2), TSM_UTF8_EXPECT2);
	ck_assert_int_eq(tsm_utf8_mach_feed(mach, 'A'), TSM_UTF8_RETRY);
	ck_assert_uint_eq(tsm_utf8_mach_get(mach), FFFD);
	ck_assert(tsm_utf8_mach_idle(mach));
	ck_assert_int_eq(tsm_utf8_mach_feed(mach, 'A'), TSM_UTF8_ACCEPT);
	ck_assert_uint_eq(tsm_utf8_mach_get(mach), 'A');

	ck_assert_int_eq(tsm_utf8_mach_feed(mach, (char)0xc0), TSM_UTF8_REJECT);
	ck_assert_uint_eq(tsm_utf8_mach_get(mach), FFFD);
	ck_assert(tsm_utf8_mach_idle(mach));

	tsm_utf8_mach_feed(mach, (char)0xf0);
	tsm_utf8_mach_reset(mach);
	ck_assert(tsm_utf8_mach_idle(mach));
	ck_assert_int_eq(tsm_utf8_mach_feed(mach, 'A'), TSM_UTF8_ACCEPT);
	tsm_utf8_mach_free(mach);
}
END_TEST

TEST_DEFINE_CASE(contract)
	TIMEOUT(60)
	TEST(test_utf8_well_formed)
	TEST(test_utf8_maximal_subparts)
	TEST(test_utf8_breaking_byte_kept)
	TEST(test_utf8_overlong)
	TEST(test_utf8_surrogate)
	TEST(test_utf8_above_max)
	TEST(test_utf8_c1)
	TEST(test_utf8_exhaustive)
	TEST(test_utf8_idle)
TEST_END_CASE

// clang-format off
TEST_DEFINE(
	TEST_SUITE(utf8,
		TEST_CASE(contract),
		TEST_END
	)
)
// clang-format on
