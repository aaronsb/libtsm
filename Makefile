# libtsm fork — development build and conformance replay
#
# Every step runs in the fork/dev.Dockerfile image as the invoking user, with
# this checkout bind-mounted, so the host needs only docker and make.
#
#   make test          build, then run the unit tests and the conformance replay
#   make cases         rewrite the synthetic streams from gen-synthetic.py
#   make record        re-record the real-program streams (record-programs.py)
#   make golden        rewrite the goldens from upstream libtsm REF (default v4.8.0)
#   make fuzz          fuzz the sixel decoder for FUZZ_TIME seconds (ASan, UBSan)
#   make clean         remove fork/build
#
# A rewritten golden is a change to the judge: commit it on its own, with the
# diff reviewed, never alongside the code it judges (ADR-301 in the kmscon fork).

# Dev image pins (moving one is a one-line commit; see docs). DEV_BASE is the
# archlinux:base-devel digest, DEV_ARCHIVE the Arch Linux Archive snapshot that
# pacman installs from. Both are recorded in the conformance reports.
DEV_BASE    := archlinux@sha256:ee205c220399524a683cf495d411691b921baed8ab47cdc6d732efa782fae484
DEV_ARCHIVE := 2026/10/21

IMAGE ?= libtsm-dev
FUZZ_IMAGE ?= libtsm-fuzz
# make fuzz: seconds per run, and parallel workers
FUZZ_TIME ?= 600
FUZZ_JOBS ?= 4
BUILD := fork/build
REF   ?= v4.8.0
CONF  := test/conformance

RUN := docker run --rm --user $(shell id -u):$(shell id -g) -e HOME=/tmp \
	-v $(CURDIR):/src -w /src $(IMAGE)
RUN_FUZZ := docker run --rm --user $(shell id -u):$(shell id -g) -e HOME=/tmp \
	-v $(CURDIR):/src -w /src $(FUZZ_IMAGE)

.DEFAULT_GOAL := help

.PHONY: help
help: ## Show this help
	@awk 'BEGIN{FS=":.*##"} /^[a-zA-Z0-9_-]+:.*##/ {printf "  \033[36m%-8s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)

.PHONY: image
image: ## Build the dev image
	docker build -t $(IMAGE) --target dev --build-arg BASE=$(DEV_BASE) --build-arg ARCHIVE_DATE=$(DEV_ARCHIVE) \
		-f fork/dev.Dockerfile fork

.PHONY: image-fuzz
image-fuzz: ## Build the dev image with clang, for make fuzz
	docker build -t $(FUZZ_IMAGE) --target fuzz --build-arg BASE=$(DEV_BASE) --build-arg ARCHIVE_DATE=$(DEV_ARCHIVE) \
		-f fork/dev.Dockerfile fork

.PHONY: test
test: image ## Build, then run unit tests and the conformance replay
	$(RUN) sh -c '[ -f $(BUILD)/build.ninja ] || meson setup $(BUILD); meson compile -C $(BUILD) && meson test -C $(BUILD) --print-errorlogs'

.PHONY: asan
asan: image ## Build with ASan and UBSan into fork/build-asan and run every suite (leak control must fail)
	$(RUN) sh -c '[ -f $(BUILD)-asan/build.ninja ] || meson setup $(BUILD)-asan -Db_sanitize=address,undefined; meson compile -C $(BUILD)-asan && meson test -C $(BUILD)-asan --setup asan --print-errorlogs'

# libFuzzer with ASan and UBSan over the seed corpus (test/fuzz/sixel-seeds);
# what it finds goes to fork/build-fuzz/sixel-corpus, crashes to
# fork/build-fuzz/crash-*. ADR-103 runs it for a fixed budget before a change
# to the decoder lands.
.PHONY: fuzz
fuzz: image-fuzz ## Fuzz the sixel decoder (FUZZ_TIME seconds, FUZZ_JOBS workers)
	$(RUN_FUZZ) sh -c '[ -f $(BUILD)-fuzz/build.ninja ] || CC=clang meson setup $(BUILD)-fuzz -Dfuzz=true \
		-Db_sanitize=address,undefined -Db_lundef=false; meson compile -C $(BUILD)-fuzz test/fuzz-sixel && \
		mkdir -p $(BUILD)-fuzz/sixel-corpus && cd $(BUILD)-fuzz && \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./test/fuzz-sixel -max_total_time=$(FUZZ_TIME) \
		-jobs=$(FUZZ_JOBS) -workers=$(FUZZ_JOBS) -print_final_stats=1 sixel-corpus ../../test/fuzz/sixel-seeds'

.PHONY: cases
cases: ## Rewrite the synthetic streams (list printed for cases.list)
	python3 $(CONF)/gen-synthetic.py $(CONF)/cases

.PHONY: record
record: image ## Re-record the real-program streams in the dev image
	$(RUN) python3 $(CONF)/record-programs.py $(CONF)/cases

.PHONY: golden
golden: image ## Rewrite goldens from upstream libtsm REF
	$(RUN) sh -c 'git config --global --add safe.directory /src && $(CONF)/make-golden.sh $(REF)'

.PHONY: clean
clean: ## Remove fork/build
	rm -rf $(BUILD)
