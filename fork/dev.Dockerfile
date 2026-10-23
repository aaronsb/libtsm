# check=skip=InvalidDefaultArgInFrom

# Build environment for this fork: what libtsm needs to build and run its
# tests and the conformance replay, plus the programs the conformance recorder
# runs. The Makefile at the repository root runs every step in this image as
# the invoking user, with the source bind-mounted.
#
# The base image and the package set are both pinned (see the pin variables at
# the top of the Makefile): BASE is a digest, ARCHIVE_DATE a snapshot of the
# Arch Linux Archive that pacman reads instead of the live mirrors.
ARG BASE
FROM ${BASE} AS dev
ARG BASE
ARG ARCHIVE_DATE
ENV DEV_IMAGE_BASE=${BASE} DEV_ARCHIVE_DATE=${ARCHIVE_DATE}

RUN echo "Server = https://archive.archlinux.org/repos/${ARCHIVE_DATE}/\$repo/os/\$arch" \
        > /etc/pacman.d/mirrorlist
RUN pacman -Syu --noconfirm --needed meson ninja git python check libxkbcommon \
        vim less tmux ncurses bash \
    && pacman -Scc --noconfirm

# make fuzz: the same image plus clang and its sanitizer and libFuzzer runtimes
FROM dev AS fuzz
RUN pacman -S --noconfirm --needed clang compiler-rt \
    && pacman -Scc --noconfirm
