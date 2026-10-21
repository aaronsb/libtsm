#!/bin/sh
# Produce the conformance goldens from a reference libtsm.
#
# Checks out REF (default: the upstream release this fork started from) into a
# throwaway worktree, builds tsm-replay against it through a scratch meson
# project that uses it as a subproject, and writes golden/*.txt.
# Review the resulting diff as its own change before committing it (ADR-301).
#
# Usage: test/conformance/make-golden.sh [REF]
# Run from the repository root, inside the dev container (see Makefile).
#
# SPDX-License-Identifier: MIT
set -eu

REF=${1:-v4.8.0}
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

python3 "$HERE/check.py" "$WORK/build/tsm-replay" "$HERE" --update
echo "goldens from libtsm $REF ($(git rev-parse --short "$REF"))"
