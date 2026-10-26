/*
 * gen-modes - print the mode table (ADR-201 in aaronsb/kmscon)
 *
 *   gen-modes md     fork/modes.md, the reference page
 *   gen-modes tsv    fork/modes.tsv, the same rows for other tools
 *
 * Both come from src/tsm/tsm-modes.def. The DECRQM column is what a vte with
 * libtsm's default settings answers: at start, and the values it gives in
 * the states the probes below reach, then the values it gives with the
 * host's settings that offer a mode turned on (tsm_vte_set_sync, and sixel
 * with an image budget). `make
 * modes` rewrites both files, and the modes-doc test fails when they differ
 * from what this prints.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "libtsm.h"

struct mode_row {
	bool dec;
	unsigned int number;
	const char *name;
	const char *status;
	const char *dflt;
	const char *reset;
	const char *host;
	const char *terminfo;
	const char *spec;
	const char *tests;
};

#define MODE_KIND_ANSI false
#define MODE_KIND_DEC true

static const struct mode_row rows[] = {
#define MODE(kind, number, name, status, dflt, reset, host, terminfo, spec, tests) \
	{ MODE_KIND_##kind, number, #name, #status, #dflt, #reset, #host,      \
	  terminfo, spec, tests },
#include "tsm-modes.def"
#undef MODE
};

#define ROW_COUNT (sizeof(rows) / sizeof(*rows))

static void write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	(void)vte;
	(void)u8;
	(void)len;
	(void)data;
}

/*
 * The states a row's DECRQM is read in: each context, then nothing, the
 * row's SM, or its SM and RM. The context reaches what SM alone does not: a
 * steady cursor (DECSCUSR 2), which mode 12 reports.
 */
static const char *const contexts[] = { "", "\e[2 q" };

/* bit v set for each DECRQM value v the row gives in those states, with
 * the host's offering settings on when @host */
static unsigned int probe(const struct mode_row *r, bool host)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	unsigned int seen = 0, c, step;
	char seq[32];
	int v;

	for (c = 0; c < sizeof(contexts) / sizeof(*contexts); ++c) {
		for (step = 0; step < 3; ++step) {
			if (tsm_screen_new(&screen, NULL, NULL) ||
			    tsm_vte_new(&vte, screen, write_cb, NULL, NULL, NULL))
				return 0;
			if (host) {
				tsm_vte_set_sync(vte, true);
				tsm_screen_set_image_limit(screen, 1 << 20);
				tsm_vte_set_sixel(vte, true);
			}
			tsm_vte_input(vte, contexts[c], strlen(contexts[c]));
			if (step >= 1) {
				snprintf(seq, sizeof(seq), "\e[%s%uh", r->dec ? "?" : "", r->number);
				tsm_vte_input(vte, seq, strlen(seq));
			}
			if (step >= 2) {
				snprintf(seq, sizeof(seq), "\e[%s%ul", r->dec ? "?" : "", r->number);
				tsm_vte_input(vte, seq, strlen(seq));
			}
			v = tsm_vte_get_mode(vte, r->dec, r->number);
			if (v >= 0 && v < 8)
				seen |= 1u << v;
			tsm_vte_unref(vte);
			tsm_screen_unref(screen);
		}
	}
	return seen;
}

/* lower case, after the prefix: RESET_SOFT is "soft", HOST_INPUT "input" */
static void print_lower(const char *s, const char *prefix)
{
	size_t n = strlen(prefix);

	if (!strncmp(s, prefix, n))
		s += n;
	for (; *s; ++s)
		putchar(*s >= 'A' && *s <= 'Z' ? *s - 'A' + 'a' : *s);
}

/* true, false, or CONFIGURED(expression): the host's setting */
static void print_default(const char *dflt, bool md)
{
	size_t len = strlen(dflt);

	if (!strcmp(dflt, "true")) {
		fputs("set", stdout);
	} else if (!strcmp(dflt, "false")) {
		fputs("reset", stdout);
	} else if (!strncmp(dflt, "CONFIGURED(", 11) && dflt[len - 1] == ')') {
		printf(md ? "host setting, `%.*s`" : "host:%.*s",
		       (int)(len - 12), dflt + 11);
	} else {
		fputs(dflt, stdout);
	}
}

static void print_decrqm_md(int value, unsigned int seen, unsigned int hosted)
{
	if (seen != hosted && hosted == (1u << 1 | 1u << 2))
		printf("1 or 2 once the host offers it; %d before, and at start", value);
	else if (seen == (1u << 1 | 1u << 2))
		printf("1 or 2; %d at start", value);
	else if (value == 3)
		fputs("3, permanently set", stdout);
	else if (value == 4)
		fputs("4, permanently reset", stdout);
	else
		printf("%d", value);
}

/* each test name in backticks, or "none" */
static void print_tests_md(const char *tests)
{
	const char *p = tests, *end;
	bool first = true;

	if (!*tests) {
		fputs("none", stdout);
		return;
	}
	while (*p) {
		while (*p == ' ')
			++p;
		if (!*p)
			break;
		end = strchr(p, ' ');
		if (!end)
			end = p + strlen(p);
		printf("%s`%.*s`", first ? "" : ", ", (int)(end - p), p);
		first = false;
		p = end;
	}
}

static void print_table_md(bool dec, const int *values, const unsigned int *seen,
			   const unsigned int *hosted)
{
	size_t i;

	printf("| Mode | Name | Status | DECRQM | Default | Reset | Host | Terminfo | Spec | Tests |\n");
	printf("|---:|---|---|---|---|---|---|---|---|---|\n");
	for (i = 0; i < ROW_COUNT; ++i) {
		const struct mode_row *r = &rows[i];

		if (r->dec != dec)
			continue;
		printf("| %s%u | %s | ", dec ? "?" : "", r->number, r->name);
		print_lower(r->status, "");
		fputs(" | ", stdout);
		print_decrqm_md(values[i], seen[i], hosted[i]);
		fputs(" | ", stdout);
		print_default(r->dflt, true);
		fputs(" | ", stdout);
		print_lower(r->reset, "RESET_");
		fputs(" | ", stdout);
		print_lower(r->host, "HOST_");
		printf(" | %s | %s | ", *r->terminfo ? r->terminfo : "", r->spec);
		print_tests_md(r->tests);
		fputs(" |\n", stdout);
	}
}

static void print_md(const int *values, const unsigned int *seen,
		     const unsigned int *hosted)
{
	static const char *const statuses[] = {
		"SUPPORTED", "PARTIAL", "IGNORED", "REFUSED",
	};
	unsigned int count[4] = { 0 };
	size_t i, j;

	for (i = 0; i < ROW_COUNT; ++i)
		for (j = 0; j < 4; ++j)
			if (!strcmp(rows[i].status, statuses[j]))
				++count[j];

	printf("# The modes libtsm recognises\n\n");
	printf("Generated by `test/gen-modes.c` from `src/tsm/tsm-modes.def` (ADR-201 in aaronsb/kmscon); `make modes` rewrites this page and `modes.tsv`, and the `modes-doc` test fails when either differs from the table. Edit the table, not this page.\n\n");
	printf("%zu modes: %u supported, %u partial, %u ignored, %u refused.\n\n",
	       ROW_COUNT, count[0], count[1], count[2], count[3]);
	printf("- **Status.** Supported: set and reset do what the spec says. Partial: part of it, and the spec column names the issue for the rest. Ignored: accepted with no effect; the spec column says why, or names the issue for an effect that is wanted. Refused: deliberately not implemented; the spec column says why.\n");
	printf("- **DECRQM.** The reply to `CSI ? Ps $ p` (DEC) or `CSI Ps $ p` (ANSI), as `tsm_vte_get_mode` also gives it: 1 set, 2 reset, 3 permanently set, 4 permanently reset. \"At start\" is a new vte with libtsm's default settings; \"1 or 2\" means both were seen after the mode's SM and RM, and under a steady cursor; a single value is the only one seen. \"Once the host offers it\": the host's setting that says it does what the mode needs is on: `tsm_vte_set_sync` for mode 2026, until which SM does nothing; sixel turned on with an image budget for 80, 1070 and 8452, whose SM and RM are stored meanwhile. Until then the mode replies its fixed state. A number with no row replies 0, not recognised, and SM and RM log it at debug level.\n");
	printf("- **Reset.** Which resets restore the default: soft (DECSTR and RIS), hard (RIS only), or none. For an ignored or refused row, which resets restore its legacy bit in `tsm_vte_get_flags`.\n");
	printf("- **Host.** What the program embedding libtsm must do for the mode to work: input (report pointer or focus events, or stop repeating a held key), paste (paste through `tsm_vte_paste`), draw (present frames by the mode), sixel (turn sixel on and give the screen an image budget), or none.\n");
	printf("- **Terminfo.** The capabilities whose strings set or reset the mode.\n");
	printf("- **Tests.** The unit tests (`START_TEST` names) and tier 1 cases that set and reset the mode.\n\n");
	printf("## ANSI modes (SM, RM)\n\n");
	print_table_md(false, values, seen, hosted);
	printf("\n## DEC private modes (DECSET, DECRST)\n\n");
	print_table_md(true, values, seen, hosted);
}

/* decrqm_seen: the values seen with and without the host's settings */
static void print_tsv(const int *values, const unsigned int *seen,
		      const unsigned int *hosted)
{
	size_t i;

	printf("kind\tnumber\tname\tstatus\tdecrqm_start\tdecrqm_seen\tdefault\treset\thost\tterminfo\tspec\ttests\n");
	for (i = 0; i < ROW_COUNT; ++i) {
		const struct mode_row *r = &rows[i];
		unsigned int v;
		bool first;

		printf("%s\t%u\t%s\t", r->dec ? "dec" : "ansi", r->number, r->name);
		print_lower(r->status, "");
		printf("\t%d\t", values[i]);
		for (v = 0, first = true; v < 8; ++v)
			if ((seen[i] | hosted[i]) & (1u << v)) {
				printf("%s%u", first ? "" : " ", v);
				first = false;
			}
		putchar('\t');
		print_default(r->dflt, false);
		putchar('\t');
		print_lower(r->reset, "RESET_");
		putchar('\t');
		print_lower(r->host, "HOST_");
		printf("\t%s\t%s\t%s\n", r->terminfo, r->spec, r->tests);
	}
}

int main(int argc, char **argv)
{
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int values[ROW_COUNT];
	unsigned int seen[ROW_COUNT], hosted[ROW_COUNT];
	size_t i;

	if (argc != 2 || (strcmp(argv[1], "md") && strcmp(argv[1], "tsv"))) {
		fprintf(stderr, "usage: gen-modes md|tsv\n");
		return 2;
	}

	if (tsm_screen_new(&screen, NULL, NULL) ||
	    tsm_vte_new(&vte, screen, write_cb, NULL, NULL, NULL)) {
		fprintf(stderr, "gen-modes: cannot make a vte\n");
		return 1;
	}
	for (i = 0; i < ROW_COUNT; ++i)
		values[i] = tsm_vte_get_mode(vte, rows[i].dec, rows[i].number);
	tsm_vte_unref(vte);
	tsm_screen_unref(screen);
	for (i = 0; i < ROW_COUNT; ++i) {
		seen[i] = probe(&rows[i], false);
		hosted[i] = probe(&rows[i], true);
	}

	if (!strcmp(argv[1], "md"))
		print_md(values, seen, hosted);
	else
		print_tsv(values, seen, hosted);
	return 0;
}
