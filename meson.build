# SPDX-License-Identifier: MIT

project(
    'libtsm',
    'c',
    version: '4.8.0-fork.5',
    license: 'MIT',
    meson_version: '>=1.1',
    default_options: [
        'warning_level=1',
        'werror=true',
        'buildtype=debugoptimized',
        'c_std=gnu99',
    ],
)

# meson test --setup asan, in a build configured with
# -Db_sanitize=address,undefined (make asan): leak checking on explicitly,
# and any sanitizer report fails its test.
add_test_setup(
    'asan',
    env: [
        'ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1:print_summary=1',
        'UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_summary=1:print_stacktrace=1',
    ],
)

# The feature macros are also passed to the configure checks below, so a check
# sees the declarations the build sees.
feature_args = ['-D_GNU_SOURCE', '-D_POSIX_C_SOURCE=200809L']

add_project_arguments(
    '-ffast-math',
    '-fno-strict-aliasing',
    '-ffunction-sections',
    '-fdata-sections',
    feature_args,
    language: 'c',
)

#
# Optional xkbcommon dependency, otherwise use the provided xbkcommon-keysyms.h
#
xkbcommon_dep = dependency('xkbcommon', version: '>=0.5.0', required: false)
if not xkbcommon_dep.found()
    xkbcommon_dep = declare_dependency(
        include_directories: include_directories('external'),
    )
endif

#
# Add a config.h which can define BUILD_ENABLE_DEBUG for extra debugging
#
config = configuration_data()
config.set('BUILD_ENABLE_DEBUG', get_option('extra_debug'))

# The kernel randomness that keys the cluster table (tsm_entropy in
# tsm-unicode.c): getrandom on Linux and FreeBSD, getentropy on macOS and
# OpenBSD, arc4random_buf on the other BSDs.
cc = meson.get_compiler('c')
entropy_prefix = '#include <stdlib.h>\n#include <unistd.h>\n'
if cc.has_header('sys/random.h', args: feature_args)
    config.set('HAVE_SYS_RANDOM_H', true)
    entropy_prefix += '#include <sys/random.h>\n'
endif
config.set('HAVE_GETRANDOM',
    cc.has_function('getrandom', prefix: entropy_prefix, args: feature_args) and
    cc.has_header_symbol('sys/random.h', 'GRND_NONBLOCK', args: feature_args))
config.set('HAVE_GETENTROPY',
    cc.has_function('getentropy', prefix: entropy_prefix, args: feature_args))
config.set('HAVE_ARC4RANDOM_BUF',
    cc.has_function('arc4random_buf', prefix: entropy_prefix, args: feature_args))
config_h = configure_file(configuration: config, output: 'config.h')
abs_config_h = meson.current_build_dir() / '@0@'.format('config.h')
add_project_arguments('-include', abs_config_h, language: 'c')

subdir('external')
subdir('src')

if get_option('tests')
    subdir('test')
endif
