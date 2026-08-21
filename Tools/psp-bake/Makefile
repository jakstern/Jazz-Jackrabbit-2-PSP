# psp-bake: native Linux build, and a MinGW-w64 Windows cross build in Docker.
#
#   make            Build both
#   make linux      -> build/psp-bake
#   make windows    -> build/psp-bake.exe
#   make clean

SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
.ONESHELL:
.DELETE_ON_ERROR:

HERE  := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ROOT  := $(abspath $(HERE)/../..)
BUILD := $(HERE)/build

CMAKE      ?= cmake
DOCKER     ?= docker
BUILD_TYPE ?= Release
JOBS       ?= $(shell nproc 2>/dev/null || echo 1)
IMAGE      ?= jazz2-psp-bake-mingw:latest

.PHONY: all linux windows image clean

all: linux windows

linux:
	@"$(CMAKE)" -S "$(HERE)" -B "$(BUILD)/linux" -DCMAKE_BUILD_TYPE="$(BUILD_TYPE)"
	"$(CMAKE)" --build "$(BUILD)/linux" -j"$(JOBS)"
	cp -f "$(BUILD)/linux/psp-bake" "$(BUILD)/psp-bake"
	echo "-> $(BUILD)/psp-bake"

# The repository is mounted at its own absolute path so the CMake cache built
# inside the container stays valid outside it.
windows: image
	@"$(DOCKER)" run --rm \
		-v "$(ROOT):$(ROOT)" -w "$(ROOT)" \
		--user "$$(id -u):$$(id -g)" -e HOME=/tmp \
		"$(IMAGE)" bash -eu -o pipefail -c '\
			cmake -S "$(HERE)" -B "$(BUILD)/windows" \
				-DCMAKE_TOOLCHAIN_FILE="$(HERE)/cmake/mingw-w64-x86_64.cmake" \
				-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
				-DPSP_BAKE_STATIC_LIBXMP=ON; \
			cmake --build "$(BUILD)/windows" -j"$(JOBS)"'
	cp -f "$(BUILD)/windows/psp-bake.exe" "$(BUILD)/psp-bake.exe"
	echo "-> $(BUILD)/psp-bake.exe"

# Rebuilt when the Dockerfile changes or the image is gone.
image:
	@if ! "$(DOCKER)" image inspect "$(IMAGE)" >/dev/null 2>&1 || [[ "$(HERE)/Dockerfile.mingw" -nt "$(BUILD)/.image" ]]; then
		"$(DOCKER)" build -f "$(HERE)/Dockerfile.mingw" -t "$(IMAGE)" "$(HERE)"
		mkdir -p "$(BUILD)"
		touch "$(BUILD)/.image"
	fi

clean:
	@rm -rf "$(BUILD)"
