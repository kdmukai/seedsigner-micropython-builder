.PHONY: docker-shell docker-build-all

# Local dev uses the same prebaked GHCR image as CI.
# Override IMAGE if you need a pinned tag.
IMAGE ?= ghcr.io/kdmukai-bot/seedsigner-micropython-builder-base:latest

docker-shell:
	docker run --rm -it \
		-v $(PWD):/workspace/seedsigner-micropython-builder \
		-w /workspace/seedsigner-micropython-builder \
		$(IMAGE) bash

# One-liner: setup + firmware build + screenshot build inside Docker
docker-build-all:
	docker run --rm -t \
		-v $(PWD):/workspace/seedsigner-micropython-builder \
		-w /workspace/seedsigner-micropython-builder \
		$(IMAGE) bash -lc './scripts/docker_build_all.sh'
