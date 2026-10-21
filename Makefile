# libtsm fork — development build and conformance replay
#
# Every step runs in the fork/dev.Dockerfile image as the invoking user, with
# this checkout bind-mounted, so the host needs only docker and make.
#
#   make test          build, then run the unit tests and the conformance replay
#   make cases         rewrite the synthetic streams from gen-synthetic.py
#   make record        re-record the real-program streams (record-programs.py)
#   make golden        rewrite the goldens from upstream libtsm REF (default v4.8.0)
#   make clean         remove fork/build
#
# A rewritten golden is a change to the judge: commit it on its own, with the
# diff reviewed, never alongside the code it judges (ADR-301 in the kmscon fork).

IMAGE ?= libtsm-dev
BUILD := fork/build
REF   ?= v4.8.0
CONF  := test/conformance

RUN := docker run --rm --user $(shell id -u):$(shell id -g) -e HOME=/tmp \
	-v $(CURDIR):/src -w /src $(IMAGE)

.DEFAULT_GOAL := help

.PHONY: help
help: ## Show this help
	@awk 'BEGIN{FS=":.*##"} /^[a-zA-Z0-9_-]+:.*##/ {printf "  \033[36m%-8s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)

.PHONY: image
image: ## Build the dev image
	docker build -t $(IMAGE) -f fork/dev.Dockerfile fork

.PHONY: test
test: image ## Build, then run unit tests and the conformance replay
	$(RUN) sh -c '[ -f $(BUILD)/build.ninja ] || meson setup $(BUILD); meson compile -C $(BUILD) && meson test -C $(BUILD) --print-errorlogs'

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
