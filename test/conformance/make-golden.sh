#!/bin/sh
# Produce the conformance goldens from a reference libtsm.
#
# Checks out REF (default: the upstream release this fork started from) into a
# throwaway worktree, builds tsm-replay against it through a scratch meson
# project that uses it as a subproject, and writes golden/*.txt.
# Review the resulting diff as its own change before committing it (ADR-301).
#
# Each golden has one source. Cases named in fork-goldens.list judge behaviour
# the fork changed on purpose, and their goldens come from a fork commit; every
# other case judges preserved behaviour, and its golden comes from upstream.
# REF chooses the set: the upstream release writes the upstream-sourced
# goldens, and a fork commit, one that carries fork-goldens.list, writes the
# fork-sourced ones. Neither run touches the other set, and any other REF is
# refused, as is a listed name that is not in cases.list.
#
# golden-draw2/ and golden-frame/ hold every case through tsm-replay's draw2
# and frame views, which only the fork's libtsm has: a fork REF writes all of
# them, the upstream REF none.
#
# Usage: test/conformance/make-golden.sh [REF]
# Run from the repository root, inside the dev container (see Makefile).
#
# SPDX-License-Identifier: MIT
set -eu

UPSTREAM=v4.8.0
REF=${1:-$UPSTREAM}
HERE=$(cd "$(dirname "$0")" && pwd)

fail() {
	echo "make-golden: $*" >&2
	exit 1
}

# A misspelt name would quietly leave its case in the upstream set.
unknown=$(awk '{ sub(/#.*/, "") } !NF { next }
	FILENAME == ARGV[1] { cases[$1] = 1; next }
	!($1 in cases) { print $1 }' "$HERE/cases.list" "$HERE/fork-goldens.list")
[ -z "$unknown" ] || fail "fork-goldens.list names cases not in cases.list:" $unknown

# The upstream release writes the upstream set. A commit that carries
# fork-goldens.list has the fork's code and tooling and writes the fork set;
# testing for the file, not for one commit's ancestry, survives a squash merge.
# Any other REF, such as an older upstream tag, has neither role.
commit=$(git rev-parse --verify --quiet "$REF^{commit}") || fail "unknown REF $REF"
if [ "$commit" = "$(git rev-parse "$UPSTREAM^{commit}")" ]; then
	SET=upstream-sourced FORK=0
elif git cat-file -e "$commit:test/conformance/fork-goldens.list" 2>/dev/null; then
	SET=fork-sourced FORK=1
else
	fail "$REF is neither upstream $UPSTREAM nor a fork commit (no test/conformance/fork-goldens.list); refusing to write goldens"
fi

WORK=$(mktemp -d)
trap 'git worktree remove --force "$WORK/proj/subprojects/libtsm" 2>/dev/null || true; rm -rf "$WORK"' EXIT

mkdir -p "$WORK/proj/subprojects"
git worktree add --detach "$WORK/proj/subprojects/libtsm" "$REF" >/dev/null
cp "$HERE/tsm-replay.c" "$WORK/proj/"
cat > "$WORK/proj/meson.build" <<'EOF'
project('tsm-replay-reference', 'c')
sp = subproject('libtsm', default_options: ['default_library=static', 'tests=false'])
# the fork's test calls build only through libtsm_test_dep; upstream and fork
# commits from before it have libtsm_dep alone
libtsm = sp.get_variable('libtsm_test_dep', sp.get_variable('libtsm_dep'))
executable('tsm-replay', 'tsm-replay.c', dependencies: libtsm)
EOF

meson setup "$WORK/build" "$WORK/proj" --force-fallback-for=libtsm >/dev/null
meson compile -C "$WORK/build" >/dev/null

# A case directory holding only the chosen set, sharing cases/ and golden/.
mkdir "$WORK/cases"
ln -s "$HERE/cases" "$WORK/cases/cases"
ln -s "$HERE/golden" "$WORK/cases/golden"
awk -v fork=$FORK '{ sub(/#.*/, "") } !NF { next }
	FILENAME == ARGV[1] { listed[$1] = 1; next }
	($1 in listed) == fork' "$HERE/fork-goldens.list" "$HERE/cases.list" \
	> "$WORK/cases/cases.list"

python3 "$HERE/check.py" "$WORK/build/tsm-replay" "$WORK/cases" --update
echo "$SET goldens from libtsm $REF ($(git rev-parse --short "$REF"))"

if [ "$FORK" = 1 ]; then
	mkdir -p "$WORK/all"
	ln -s "$HERE/cases" "$WORK/all/cases"
	cp "$HERE/cases.list" "$WORK/all/cases.list"
	for view in draw2 frame; do
		mkdir -p "$HERE/golden-$view"
		ln -s "$HERE/golden-$view" "$WORK/all/golden-$view"
		python3 "$HERE/check.py" "$WORK/build/tsm-replay" "$WORK/all" --$view --update
		echo "$view goldens from libtsm $REF ($(git rev-parse --short "$REF"))"
	done
fi
