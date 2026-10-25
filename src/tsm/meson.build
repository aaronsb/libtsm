# SPDX-License-Identifier: MIT

libtsm_srcs = [
    'tsm-image.c',
    'tsm-kitty.c',
    'tsm-render.c',
    'tsm-screen.c',
    'tsm-selection.c',
    'tsm-sixel.c',
    'tsm-unicode.c',
    'tsm-vte-charsets.c',
    'tsm-vte.c',
]

# The fork's ABI differs from upstream's (ADR-102 in aaronsb/kmscon): struct
# tsm_screen_attr grew from 12 to 16 bytes (fork.1), struct tsm_screen_cell
# from 12 to 16 with the underline colour (fork.2), tsm_screen_frame_since was
# added (fork.3), and struct tsm_frame grew a placement list beside the image
# calls (fork.4, ADR-103), and the vte gained sixel and a bounded input call
# (fork.5). Its own soname keeps a program built against
# upstream's header from loading it as libtsm.so.4, and the file name keeps
# an install from replacing upstream's. Raise the number after "fork" on every
# further ABI change, and keep it in step with the project version.
soversion = '4-fork.5'

# Export symbol with version
link_flags = '-Wl,--version-script=' + meson.current_source_dir() + '/libtsm.sym'
code = 'int main() { return 0; }'
cc = meson.get_compiler('c')

if not cc.links(code, args: link_flags, name: '-Wl,--version-script=...')
    link_flags = [ '-export-symbols-regex=^tsm_' ]
    if not cc.links(code, args: link_flags, name: '-Wl,--export-symbol-regex')
        link_flags = []
    endif
endif

deps = [shl_dep, xkbcommon_dep]

libtsm = library(
    'tsm',
    libtsm_srcs,
    dependencies: deps,
    install: true,
    soversion: soversion,
    link_args: link_flags,
)

libtsm_dep = declare_dependency(
    include_directories: '.',
    link_with: libtsm,
    dependencies: deps,
    sources: libtsm_srcs,
)

# The test calls of ADR-103 step 3a (tsm_screen_test_image and
# tsm_screen_test_place) build only into what links this: libtsm's tests and
# tsm-replay. The shared library neither contains nor exports them. So does
# the mode trace the modes-tests check reads (TSM_MODE_TRACE, ADR-201).
libtsm_test_dep = declare_dependency(
    include_directories: '.',
    link_with: libtsm,
    dependencies: deps,
    sources: libtsm_srcs + ['tsm-image-test.c'],
    compile_args: ['-DTSM_MODE_TRACE'],
)

install_headers('libtsm.h')

pkg = import('pkgconfig')
pkg.generate(
    libtsm,
    name: 'libtsm',
    url: 'https://github.com/kmscon/libtsm',
    description: 'terminal-emulator state machine',
)
