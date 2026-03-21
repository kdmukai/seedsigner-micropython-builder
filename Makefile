.PHONY: docker-shell docker-build-all clean clean-purge-cache full-reset

# Local dev uses the same prebaked GHCR image as CI.
# Override IMAGE if you need a pinned tag.
IMAGE ?= ghcr.io/kdmukai-bot/seedsigner-micropython-builder-base:latest

DOCKER_RUN = docker run --rm \
	--user $(shell id -u):$(shell id -g) \
	-e HOME=/tmp/home \
	$(if $(BOARD),-e BOARD=$(BOARD)) \
	-v $(PWD):/workspace/seedsigner-micropython-builder \
	--tmpfs /tmp/home:uid=$(shell id -u),gid=$(shell id -g) \
	-v $(HOME)/.cache:/tmp/home/.cache \
	-w /workspace/seedsigner-micropython-builder

docker-shell:
	@mkdir -p $(HOME)/.cache
	$(DOCKER_RUN) -it $(IMAGE) bash

# One-liner: setup + firmware build + screenshot build inside Docker
docker-build-all:
	@mkdir -p $(HOME)/.cache
	$(DOCKER_RUN) -t $(IMAGE) bash -lc './scripts/docker_build_all.sh'

# Safe clean: remove generated build outputs only (keeps deps/ working trees)
clean:
	rm -rf \
		build \
		logs \
		deps/micropython/upstream/ports/esp32/build* \
		deps/seedsigner-c-modules/tools/screenshot_generator/build

# Destructive reset: removes all generated artifacts and resets submodule working trees.
# Requires explicit confirmation to avoid accidental loss of in-progress work.
full-reset:
	@if [ "$(CONFIRM)" != "YES" ]; then \
		echo "Refusing to run destructive reset."; \
		echo "Run: make full-reset CONFIRM=YES"; \
		exit 1; \
	fi
	rm -rf build logs
	git -C deps/micropython/upstream checkout -- .
	git -C deps/micropython/upstream clean -fd
	git -C deps/micropython/upstream submodule foreach --recursive 'git checkout -- . 2>/dev/null; git clean -fd 2>/dev/null' || true
