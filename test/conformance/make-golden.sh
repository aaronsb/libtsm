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
# goldens, and any other REF writes the fork-sourced ones. Neither run touches
# the other set.
#
# Usage: test/conformance/make-golden.sh [REF]
# Run from the repository root, inside the dev container (see Makefile).
#
# SPDX-License-Identifier: MIT
set -eu

UPSTREAM=v4.8.0
REF=${1:-$UPSTREAM}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'git worktree remove --force "$WORK/proj/subprojects/libtsm" 2>/dev/null || true; rm -rf "$WORK"' EXIT

mkdir -p "$WORK/proj/subprojects"
git worktree add --detach "$WORK/proj/subprojects/libtsm" "$REF" >/dev/null
cp "$HERE/tsm-replay.c" "$WORK/proj/"
cat > "$WORK/proj/meson.build" <<'EOF'
project('tsm-replay-reference', 'c')
libtsm = dependency('libtsm', fallback: ['libtsm', 'libtsm_dep'],
                    default_options: ['default_library=static', 'tests=false'])
executable('tsm-replay', 'tsm-replay.c', dependencies: libtsm)
EOF

meson setup "$WORK/build" "$WORK/proj" --force-fallback-for=libtsm >/dev/null
meson compile -C "$WORK/build" >/dev/null

# A case directory holding only the chosen set, sharing cases/ and golden/.
if [ "$(git rev-parse "$REF^{commit}")" = "$(git rev-parse "$UPSTREAM^{commit}")" ]; then
	SET=upstream-sourced FORK=0
else
	SET=fork-sourced FORK=1
fi
mkdir "$WORK/cases"
ln -s "$HERE/cases" "$WORK/cases/cases"
ln -s "$HERE/golden" "$WORK/cases/golden"
awk -v fork=$FORK '{ sub(/#.*/, "") } !NF { next }
	FILENAME == ARGV[1] { listed[$1] = 1; next }
	($1 in listed) == fork' "$HERE/fork-goldens.list" "$HERE/cases.list" \
	> "$WORK/cases/cases.list"

python3 "$HERE/check.py" "$WORK/build/tsm-replay" "$WORK/cases" --update
echo "$SET goldens from libtsm $REF ($(git rev-parse --short "$REF"))"
