/*
 * TSM - Valgrind Verification
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

/*
 * Dummy which just leaks memory. Used to verify valgrind memcheck, and as the
 * positive control of a sanitized build, where LeakSanitizer must fail it.
 * The pointer goes through a volatile, so an optimizing build keeps the
 * malloc, and is then overwritten, so no copy of it stays reachable.
 */

#include "test_common.h"

static void *volatile leaked;

START_TEST(test_valgrind)
{
	leaked = malloc(0x100);
	ck_assert_ptr_ne(leaked, NULL);
	leaked = NULL;
}
END_TEST

TEST_DEFINE_CASE(misc)
	TEST(test_valgrind)
TEST_END_CASE

TEST_DEFINE(
	TEST_SUITE(valgrind,
		TEST_CASE(misc),
		TEST_END
	)
)
