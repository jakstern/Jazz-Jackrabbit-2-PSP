# Jazz Jackrabbit 2 - PSP release package.
#
#   make          Build the EBOOT and assemble build/psp/JAZZ2-PSP
#   make clean    Remove build/
#
# The result is a complete PSP/GAME folder. The player supplies only their own
# Jazz Jackrabbit 2 data: either pre-baked on a PC into Cache/, or dropped into
# Source/ for the PSP to bake on first launch.
#
# Day-to-day development (deploy, rebake, remote sync) lives in the untracked
# Makefile.local:  make -f Makefile.local <target>

SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
.ONESHELL:
.DELETE_ON_ERROR:

ROOT    := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD   := $(ROOT)/build
ENGINE  := $(BUILD)/engine
PACKAGE := $(BUILD)/psp/JAZZ2-PSP

DOCKER       ?= docker
DOCKER_IMAGE ?= pspdev/pspdev:latest
BUILD_TYPE   ?= Release
JOBS         ?= $(shell nproc 2>/dev/null || echo 1)
BUILD_STAMP  ?= $(shell date '+%Y%m%d-%H%M%S')

.PHONY: all eboot package clean

all: package

# The EBOOT is built with the PSPDEV toolchain in Docker. The repository is
# mounted at its own absolute path so the CMake cache stays valid outside it.
eboot:
	@command -v "$(DOCKER)" >/dev/null 2>&1 || { echo "ERROR: $(DOCKER) is required to build the EBOOT." >&2; exit 1; }
	"$(DOCKER)" run --rm \
		-v "$(ROOT):$(ROOT)" -w "$(ROOT)" \
		--user "$$(id -u):$$(id -g)" -e HOME=/tmp \
		"$(DOCKER_IMAGE)" bash -eu -o pipefail -c '\
			export PSPDEV=/usr/local/pspdev; \
			export PATH=$$PSPDEV/bin:$$PSPDEV/psp/bin:$$PATH; \
			psp-cmake -S "$(ROOT)" -B "$(ENGINE)" \
				-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
				-DPSP_BUILD_STAMP="$(BUILD_STAMP)"; \
			cmake --build "$(ENGINE)" -j"$(JOBS)"'
	test -f "$(ENGINE)/dist/EBOOT.PBP" || { echo "ERROR: build produced no EBOOT.PBP" >&2; exit 1; }

# Source/ and Cache/ ship empty on purpose: they are the two folders the player
# fills in, and their presence documents where the data goes.
package: eboot
	@rm -rf "$(PACKAGE)"
	mkdir -p "$(PACKAGE)/Source" "$(PACKAGE)/Cache"
	install -m 0644 "$(ENGINE)/dist/EBOOT.PBP" "$(PACKAGE)/EBOOT.PBP"
	cp -a "$(ROOT)/Content" "$(PACKAGE)/Content"
	install -m 0644 "$(ROOT)/App/Psp/Package/PackageReadme.txt" "$(PACKAGE)/README.txt"
	install -m 0644 "$(ROOT)/LICENSE" "$(PACKAGE)/LICENSE.txt"
	echo "-> $(PACKAGE)"
	du -sh "$(PACKAGE)"

clean:
	@rm -rf "$(BUILD)"
