# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Folio: build the core for RISC OS and for ARM Linux (the qemu test twin).
#
#   make            core libraries for both targets + the test program
#   make test       run the core tests under qemu-arm-aligntrap (RISC OS rules)
#   make check      riscos-crossdev's checks on the RISC OS test program
#
# MuPDF comes from mupdf/build.sh (mupdf/src). RCD_DIR is a riscos-crossdev
# checkout (for checks/ and qemu/aligntrap.sh).

RCD_DIR ?= ../riscos-crossdev
RISCOS_TOOLCHAIN ?= /opt/xd/riscos-crossdev-toolchain-1.4-x86_64-linux
export PATH := $(RISCOS_TOOLCHAIN)/bin:$(PATH)
MUPDF ?= $(CURDIR)/mupdf/src
MU_RO := $(MUPDF)/build/ro-release-tofu_cjk
MU_AL := $(MUPDF)/build/armlinux-release-tofu_cjk

RO_CC := $(RISCOS_TOOLCHAIN)/bin/arm-riscos-gnueabihf-gcc
RO_AR := $(RISCOS_TOOLCHAIN)/bin/arm-riscos-gnueabihf-ar
RO_CFLAGS := -O2 -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -D_FILE_OFFSET_BITS=64
AL_CC := arm-linux-gnueabihf-gcc
AL_AR := arm-linux-gnueabihf-ar
AL_CFLAGS := -O2 -g -marm -mcpu=cortex-a8 -mfpu=vfpv3 -mfloat-abi=hard \
	-mno-unaligned-access -fstack-clash-protection
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -std=gnu99
INC := -Icore -Ithird/cjson -I$(MUPDF)/include

CORE_SRC := core/doc.c core/render.c core/text.c core/dict.c core/annot.c
CORE_HDR := core/folio.h core/folio_int.h core/dict.h
CORE_OBJ = $(CORE_SRC:core/%.c=build/$(1)/%.o) build/$(1)/cJSON.o

WIMP_SRC := wimp/main.c wimp/docwin.c wimp/tbx.c wimp/worker.c wimp/clip.c wimp/dict.c wimp/contents.c wimp/find.c wimp/toolbar.c wimp/thumbs.c wimp/annotui.c
WIMP_HDR := wimp/app.h wimp/tbx.h wimp/worker.h core/folio.h core/dict.h
VERSION ?= 0.0.1-test13

all: build/ro/libfolio.a build/al/libfolio.a build/al/core_test build/ro/core_test.elf build/ro/folio.elf

build/ro/%.o: core/%.c $(CORE_HDR)
	@mkdir -p $(@D)
	$(RO_CC) $(RO_CFLAGS) $(WARN) $(INC) -c $< -o $@
build/al/%.o: core/%.c $(CORE_HDR)
	@mkdir -p $(@D)
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -c $< -o $@

# cJSON (MIT), for the dictionary's JSON.
build/ro/cJSON.o: third/cjson/cJSON.c third/cjson/cJSON.h
	@mkdir -p $(@D)
	$(RO_CC) $(RO_CFLAGS) -c $< -o $@
build/al/cJSON.o: third/cjson/cJSON.c third/cjson/cJSON.h
	@mkdir -p $(@D)
	$(AL_CC) $(AL_CFLAGS) -c $< -o $@

build/ro/libfolio.a: $(call CORE_OBJ,ro)
	rm -f $@; $(RO_AR) rcs $@ $^
build/al/libfolio.a: $(call CORE_OBJ,al)
	rm -f $@; $(AL_AR) rcs $@ $^

build/al/core_test: tests/core_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -lmupdf -lmupdf-third -lm -lpthread -o $@
# Built for RISC OS too, so the checks see the core and MuPDF linked together.
build/ro/core_test.elf: tests/core_test.c build/ro/libfolio.a
	$(RO_CC) $(RO_CFLAGS) $(WARN) $(INC) -static $< build/ro/libfolio.a \
		-L$(MU_RO) -lmupdf -lmupdf-third -lm -lpthread -o $@

build/ro/wimp/%.o: wimp/%.c $(WIMP_HDR)
	@mkdir -p $(@D)
	$(RO_CC) $(RO_CFLAGS) $(WARN) $(INC) -Iwimp -c $< -o $@

# The application (RISC OS only).
build/ro/folio.elf: $(WIMP_SRC:wimp/%.c=build/ro/wimp/%.o) build/ro/libfolio.a
	$(RO_CC) $(RO_CFLAGS) -static $(WIMP_SRC:wimp/%.c=build/ro/wimp/%.o) build/ro/libfolio.a \
		-L$(MU_RO) -lmupdf -lmupdf-third -lm -lpthread -o $@

# !Folio in build/app, and a zip with RISC OS filetypes.
app: build/ro/folio.elf
	RISCOS_TOOLCHAIN=$(RISCOS_TOOLCHAIN) $(RCD_DIR)/checks/check-all.sh build/ro/folio.elf
	rm -rf build/app && mkdir -p build/app
	cp -r app/!Folio build/app/
	$(RISCOS_TOOLCHAIN)/bin/arm-riscos-gnueabihf-strip -o build/ro/folio.stripped build/ro/folio.elf
	$(RISCOS_TOOLCHAIN)/bin/elf2aif -e build/ro/folio.stripped 'build/app/!Folio/!RunImage,ff8'
	python3 -I tools/mksprites.py 'build/app/!Folio/!Sprites,ff9'
	python3 -I tools/mktoolsprites.py 'build/app/!Folio/Sprites,ff9'
	cp COPYING 'build/app/!Folio/COPYING,fff'
	cp third/cjson/LICENSE 'build/app/!Folio/cJSONLicence,fff'
	cp $(RISCOS_TOOLCHAIN)/riscos/PThrTicker,ffa 'build/app/!Folio/PThrTicker,ffa'
	cp $(RISCOS_TOOLCHAIN)/riscos/PThreadTicker-Licence,fff 'build/app/!Folio/PThreadTickerLicence,fff'
	python3 $(RCD_DIR)/package/rozip.py build/Folio-$(VERSION).zip -C build/app '!Folio'
	@ls -l build/Folio-$(VERSION).zip

tests/data/stamp: tests/make-data.sh tests/make-epub.py
	tests/make-data.sh $(MU_AL)/mutool $(RCD_DIR)/qemu/aligntrap.sh
	touch $@

# The page worker, with a stand-in for the one RISC OS call it makes.
build/al/worker_test: tests/worker_test.c wimp/worker.c wimp/worker.h build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -Iwimp -Itests/stub -no-pie tests/worker_test.c wimp/worker.c \
		build/al/libfolio.a -L$(MU_AL) -lmupdf -lmupdf-third -lm -lpthread -o $@

build/al/dict_test: tests/dict_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -lmupdf -lmupdf-third -lm -lpthread -o $@

build/al/annot_test: tests/annot_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -lmupdf -lmupdf-third -lm -lpthread -o $@

# Find, with stand-ins for the Toolbox and the document window.
build/al/find_test: tests/find_test.c wimp/find.c wimp/worker.c $(WIMP_HDR) build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -Iwimp -Itests/stub-wimp -no-pie tests/find_test.c wimp/find.c \
		wimp/worker.c build/al/libfolio.a -L$(MU_AL) -lmupdf -lmupdf-third -lm -lpthread -o $@

test: build/al/core_test build/al/worker_test build/al/dict_test build/al/find_test build/al/annot_test tests/data/stamp
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/core_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/worker_test tests/data/doc.pdf
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/dict_test tests/dict
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/find_test tests/data/doc.pdf
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/annot_test tests/data

check: build/ro/core_test.elf
	RISCOS_TOOLCHAIN=$(RISCOS_TOOLCHAIN) $(RCD_DIR)/checks/check-all.sh build/ro/core_test.elf

clean:
	rm -rf build tests/data/*.epub tests/data/*.pdf tests/data/stamp tests/out

.PHONY: all app test check clean
