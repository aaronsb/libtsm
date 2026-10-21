# Build environment for this fork: what libtsm needs to build and run its
# tests and the conformance replay, plus the programs the conformance recorder
# runs. The Makefile at the repository root runs every step in this image as
# the invoking user, with the source bind-mounted.
FROM archlinux:base-devel

RUN pacman -Syu --noconfirm --needed meson ninja git python check libxkbcommon \
        vim less tmux ncurses bash \
    && pacman -Scc --noconfirm
