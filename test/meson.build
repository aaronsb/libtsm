# SPDX-License-Identifier: MIT

check_dep = dependency('check')

# For tests that depends on libtsm and all dependencies
test_deps = [shl_dep, check_dep, libtsm_test_dep, xkbcommon_dep]

test_image = executable('test_image', 'test_image.c', dependencies: test_deps)
test_htable = executable(
    'test_htable',
    'test_htable.c',
    dependencies: [shl_dep, check_dep],
)
test_screen = executable('test_screen', 'test_screen.c', dependencies: test_deps)
test_selection = executable(
    'test_selection',
    'test_selection.c',
    dependencies: test_deps,
)
test_sixel = executable(
    'test_sixel',
    'test_sixel.c',
    dependencies: test_deps,
    c_args: ['-DSIXEL_FIXTURES="@0@"'.format(meson.current_source_dir() / 'sixel')],
)
test_symbol = executable('test_symbol', 'test_symbol.c', dependencies: test_deps)
test_valgrind = executable(
    'test_valgrind',
    'test_valgrind.c',
    dependencies: [check_dep],
)
test_vte_mouse = executable(
    'test_vte_mouse',
    'test_vte_mouse.c',
    dependencies: test_deps,
)
test_vte = executable('test_vte', 'test_vte.c', dependencies: test_deps)
test_vte_modes = executable(
    'test_vte_modes',
    'test_vte_modes.c',
    dependencies: test_deps,
)
test_vte_sixel = executable(
    'test_vte_sixel',
    'test_vte_sixel.c',
    dependencies: test_deps,
)
test_vte_kitty = executable(
    'test_vte_kitty',
    'test_vte_kitty.c',
    dependencies: test_deps,
)

test('htable', test_htable)
test('image', test_image)
test('screen', test_screen)
test('selection', test_selection)
test('sixel', test_sixel)
test('symbol', test_symbol)
# In a build with -Db_sanitize=address, LeakSanitizer must catch the leak
# test_valgrind makes on purpose: a pass there means leak checking is off.
test('valgrind', test_valgrind,
     should_fail: get_option('b_sanitize').contains('address'))
test('vte_mouse', test_vte_mouse)
test('vte', test_vte)
test('vte_modes', test_vte_modes)
test('vte_sixel', test_vte_sixel)
test('vte_kitty', test_vte_kitty)

# Not a test: prints sixel decode throughput (bench-sixel [FILE.six] [SECONDS]).
executable('bench-sixel', 'bench-sixel.c', dependencies: test_deps)

# The sixel decoder's fuzz target (fuzz/fuzz-sixel.c): built as a replay of
# the seed corpus for every run, and with libFuzzer when -Dfuzz=true under
# clang (make fuzz).
fuzz_sixel_srcs = ['fuzz/fuzz-sixel.c', '../src/tsm/tsm-sixel.c']
fuzz_sixel_replay = executable(
    'fuzz-sixel-replay',
    fuzz_sixel_srcs,
    include_directories: '../src/tsm',
    c_args: ['-DFUZZ_REPLAY'],
)
test('fuzz-sixel-seeds', fuzz_sixel_replay,
     args: [meson.current_source_dir() / 'fuzz' / 'sixel-seeds'])
if get_option('fuzz')
    executable(
        'fuzz-sixel',
        fuzz_sixel_srcs,
        include_directories: '../src/tsm',
        c_args: ['-fsanitize=fuzzer'],
        link_args: ['-fsanitize=fuzzer'],
    )
endif

# Sixel through the vte (fuzz/fuzz-vte-sixel.c): the parser, decoder, budget
# and placements together, built the same two ways.
fuzz_vte_sixel_replay = executable(
    'fuzz-vte-sixel-replay',
    'fuzz/fuzz-vte-sixel.c',
    dependencies: [libtsm_test_dep],
    c_args: ['-DFUZZ_REPLAY'],
)
test('fuzz-vte-sixel-seeds', fuzz_vte_sixel_replay,
     args: [meson.current_source_dir() / 'fuzz' / 'vte-sixel-seeds'])
if get_option('fuzz')
    executable(
        'fuzz-vte-sixel',
        'fuzz/fuzz-vte-sixel.c',
        dependencies: [libtsm_test_dep],
        c_args: ['-fsanitize=fuzzer'],
        link_args: ['-fsanitize=fuzzer'],
    )
endif

# The kitty graphics protocol through the vte (fuzz/fuzz-vte-kitty.c): the
# APC parser, uploads, deletes, the budget and placements together, built
# the same two ways.
fuzz_vte_kitty_replay = executable(
    'fuzz-vte-kitty-replay',
    'fuzz/fuzz-vte-kitty.c',
    dependencies: [libtsm_test_dep],
    c_args: ['-DFUZZ_REPLAY'],
)
test('fuzz-vte-kitty-seeds', fuzz_vte_kitty_replay,
     args: [meson.current_source_dir() / 'fuzz' / 'vte-kitty-seeds'])
if get_option('fuzz')
    executable(
        'fuzz-vte-kitty',
        'fuzz/fuzz-vte-kitty.c',
        dependencies: [libtsm_test_dep],
        c_args: ['-fsanitize=fuzzer'],
        link_args: ['-fsanitize=fuzzer'],
    )
endif


# The mode table (ADR-201 in the kmscon fork): gen-modes prints fork/modes.md
# and fork/modes.tsv from src/tsm/tsm-modes.def, and modes-doc fails when the
# committed files differ (make modes rewrites them).
gen_modes = executable('gen-modes', 'gen-modes.c', dependencies: [libtsm_test_dep])
# Every DEC number 0-65535 and ANSI number 0-255: DECRQM replies 0 exactly
# for the numbers with no row, and SM and RM of those change nothing.
modes_sweep = executable('modes-sweep', 'modes-sweep.c', dependencies: [libtsm_test_dep])
test('modes-sweep', modes_sweep, timeout: 120)
# SM, RM and DECRQM consist of the table's expansions, and the mode constants
# stay in the accessors.
test(
    'modes-source',
    find_program('python3'),
    args: [files('modes-source.py'), meson.project_source_root() / 'src' / 'tsm' / 'tsm-vte.c'],
)
test(
    'modes-doc',
    find_program('python3'),
    args: [files('modes-doc.py'), gen_modes, meson.project_source_root() / 'fork'],
)

# Conformance replay (ADR-301 tier 1 in the kmscon fork): every recorded
# stream in conformance/ must reproduce its golden screen state.
tsm_replay = executable(
    'tsm-replay',
    'conformance/tsm-replay.c',
    dependencies: [libtsm_test_dep],
)
test(
    'conformance',
    find_program('python3'),
    args: [files('conformance/check.py'), tsm_replay, meson.current_source_dir() / 'conformance'],
)
# The same corpus through the draw2 view: colours resolved, as a renderer sees
# them (aaronsb/kmscon#26).
test(
    'conformance-draw2',
    find_program('python3'),
    args: [files('conformance/check.py'), tsm_replay, meson.current_source_dir() / 'conformance',
           '--draw2'],
)
# And through the frame call that replaces draw2 (ADR-102 step 4).
test(
    'conformance-frame',
    find_program('python3'),
    args: [files('conformance/check.py'), tsm_replay, meson.current_source_dir() / 'conformance',
           '--frame'],
)

# Every supported mode is set and reset by a test its row names, and every
# test a row names exists (ADR-201); the test build's mode trace shows which.
test(
    'modes-tests',
    find_program('python3'),
    args: [files('modes-tests.py'), gen_modes, meson.current_source_dir(), tsm_replay,
           meson.current_source_dir() / 'conformance', test_vte, test_vte_modes,
           test_vte_mouse, test_vte_sixel, test_vte_kitty, test_screen],
    timeout: 120,
)
