/*
 * TSM - Symbol Table Tests
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@gmail.com>
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

#include <stdio.h>
#include "test_common.h"
#include "libtsm.h"
#include "libtsm-int.h"
#include "tsm-grapheme-break-test.h"

START_TEST(test_symbol_null)
{
	int r;
	tsm_symbol_t sym = 0, s;
	const tsm_symbol_t *sp;
	unsigned int n;

	r = tsm_symbol_table_new(NULL);
	ck_assert(r == -EINVAL);

	tsm_symbol_table_ref(NULL);
	tsm_symbol_table_unref(NULL);

	sp = tsm_symbol_get(NULL, &sym, NULL);
	ck_assert(sp == &sym);

	s = tsm_symbol_append(NULL, sym, 'a');
	ck_assert(s == sym);

	tsm_symbol_ref(NULL, sym);
	tsm_symbol_unref(NULL, sym);
	ck_assert_uint_eq(tsm_symbol_table_count(NULL), 0);

	n = tsm_symbol_get_width(NULL, 'a', TSM_WIDTH_UNICODE);
	ck_assert_uint_eq(n, 1);
}
END_TEST

START_TEST(test_symbol_init)
{
	struct tsm_symbol_table *t;
	int r;

	r = tsm_symbol_table_new(&t);
	ck_assert(!r);

	tsm_symbol_table_unref(t);
	t = NULL;
}
END_TEST

/* a cluster is interned by content and lives while a cell holds it */
START_TEST(test_symbol_cluster_refcount)
{
	struct tsm_symbol_table *t;
	tsm_symbol_t e1, e2, other, reused;
	const uint32_t *cps;
	size_t len;
	int r;

	r = tsm_symbol_table_new(&t);
	ck_assert(!r);

	e1 = tsm_symbol_append(t, 'e', 0x301);
	ck_assert_uint_ge(e1, TSM_CLUSTER_BASE);
	e2 = tsm_symbol_append(t, 'e', 0x301);
	ck_assert_uint_eq(e1, e2);
	cps = tsm_symbol_get(t, &e1, &len);
	ck_assert_uint_eq(len, 2);
	ck_assert_uint_eq(cps[0], 'e');
	ck_assert_uint_eq(cps[1], 0x301);

	/* two cells hold it, then neither does */
	tsm_symbol_ref(t, e1);
	tsm_symbol_ref(t, e1);
	ck_assert_uint_eq(tsm_symbol_table_count(t), 1);
	tsm_symbol_unref(t, e1);
	ck_assert_uint_eq(tsm_symbol_table_count(t), 1);
	tsm_symbol_unref(t, e1);
	ck_assert_uint_eq(tsm_symbol_table_count(t), 0);

	/* its key goes to the next new cluster */
	other = tsm_symbol_append(t, 'a', 0x300);
	tsm_symbol_ref(t, other);
	ck_assert_uint_eq(other, e1);
	reused = tsm_symbol_append(t, 'e', 0x301);
	ck_assert_uint_ne(reused, other);
	tsm_symbol_ref(t, reused);
	ck_assert_uint_eq(tsm_symbol_table_count(t), 2);

	/* codepoints are not counted */
	tsm_symbol_ref(t, 'x');
	tsm_symbol_unref(t, 'x');
	ck_assert_uint_eq(tsm_symbol_table_count(t), 2);

	tsm_symbol_table_unref(t);
}
END_TEST

/* extenders past TSM_CLUSTER_MAX codepoints are dropped */
START_TEST(test_symbol_cluster_max)
{
	struct tsm_symbol_table *t;
	tsm_symbol_t sym = 'a', next;
	size_t len;
	unsigned int i;
	int r;

	r = tsm_symbol_table_new(&t);
	ck_assert(!r);

	for (i = 1; i < TSM_CLUSTER_MAX + 4; ++i) {
		next = tsm_symbol_append(t, sym, 0x300);
		if (i >= TSM_CLUSTER_MAX)
			ck_assert_uint_eq(next, sym);
		sym = next;
	}
	tsm_symbol_get(t, &sym, &len);
	ck_assert_uint_eq(len, TSM_CLUSTER_MAX);

	tsm_symbol_table_unref(t);
}
END_TEST

static tsm_symbol_t cluster(struct tsm_symbol_table *t, uint32_t base, uint32_t ext)
{
	return tsm_symbol_append(t, base, ext);
}

/* width: the first codepoint decides; VS16 widens only a listed base, and only
 * with the unicode method; VS15 never changes width */
START_TEST(test_symbol_width)
{
	struct tsm_symbol_table *t;
	int r;

	r = tsm_symbol_table_new(&t);
	ck_assert(!r);

	ck_assert_uint_eq(tsm_ucs4_get_width('a'), 1);
	ck_assert_uint_eq(tsm_ucs4_get_width(0x301), 0);
	ck_assert_uint_eq(tsm_ucs4_get_width(0x200d), 0);
	ck_assert_uint_eq(tsm_ucs4_get_width(0xfe0f), 0);
	ck_assert_uint_eq(tsm_ucs4_get_width(0x6f22), 2);	/* 漢 */
	ck_assert_uint_eq(tsm_ucs4_get_width(0x1f600), 2);	/* 😀 */
	ck_assert_uint_eq(tsm_ucs4_get_width(0x1f1fa), 2);	/* regional indicator U */
	ck_assert_uint_eq(tsm_ucs4_get_width(0x2764), 1);	/* ❤ */
	ck_assert_uint_eq(tsm_ucs4_get_width(0x00ad), 1);	/* soft hyphen */

	ck_assert_uint_eq(tsm_symbol_get_width(t, cluster(t, 'e', 0x301),
					       TSM_WIDTH_UNICODE), 1);
	ck_assert_uint_eq(tsm_symbol_get_width(t, cluster(t, 0x2764, 0xfe0f),
					       TSM_WIDTH_UNICODE), 2);
	ck_assert_uint_eq(tsm_symbol_get_width(t, cluster(t, 0x2764, 0xfe0f),
					       TSM_WIDTH_WCWIDTH), 1);
	ck_assert_uint_eq(tsm_symbol_get_width(t, cluster(t, 0x2764, 0xfe0e),
					       TSM_WIDTH_UNICODE), 1);
	ck_assert_uint_eq(tsm_symbol_get_width(t, cluster(t, '1', 0xfe0f),
					       TSM_WIDTH_UNICODE), 2);
	/* 'A' has no emoji variation sequence */
	ck_assert_uint_eq(tsm_symbol_get_width(t, cluster(t, 'A', 0xfe0f),
					       TSM_WIDTH_UNICODE), 1);
	/* a wide base stays wide under VS15 */
	ck_assert_uint_eq(tsm_symbol_get_width(t, cluster(t, 0x231a, 0xfe0e),
					       TSM_WIDTH_UNICODE), 2);

	tsm_symbol_table_unref(t);
}
END_TEST

/* every case of the UCD's GraphemeBreakTest.txt */
START_TEST(test_symbol_grapheme_breaks)
{
	const struct grapheme_break_test *c;
	struct tsm_gb_state st;
	unsigned int i, j, failed = 0;
	bool cont;

	for (i = 0; i < sizeof(grapheme_break_tests) / sizeof(*grapheme_break_tests); ++i) {
		c = &grapheme_break_tests[i];
		tsm_gb_start(&st, c->cps[0]);
		for (j = 1; j < c->len; ++j) {
			cont = tsm_gb_continues(&st, c->cps[j]);
			if (cont == c->brk[j]) {
				if (failed++ < 10)
					fprintf(stderr, "GraphemeBreakTest case %u, position %u: %s\n",
						i, j, cont ? "joined" : "broke");
			}
		}
	}
	ck_assert_uint_eq(failed, 0);
}
END_TEST

TEST_DEFINE_CASE(misc)
	TEST(test_symbol_null)
	TEST(test_symbol_init)
	TEST(test_symbol_cluster_refcount)
	TEST(test_symbol_cluster_max)
	TEST(test_symbol_width)
	TEST(test_symbol_grapheme_breaks)
TEST_END_CASE

TEST_DEFINE(
	TEST_SUITE(symbol,
		TEST_CASE(misc),
		TEST_END
	)
)
