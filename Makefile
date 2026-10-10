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
INC := -Icore -I$(MUPDF)/include

CORE_SRC := core/doc.c core/render.c core/text.c
CORE_HDR := core/folio.h core/folio_int.h

all: build/ro/libfolio.a build/al/libfolio.a build/al/core_test build/ro/core_test.elf

build/ro/%.o: core/%.c $(CORE_HDR)
	@mkdir -p $(@D)
	$(RO_CC) $(RO_CFLAGS) $(WARN) $(INC) -c $< -o $@
build/al/%.o: core/%.c $(CORE_HDR)
	@mkdir -p $(@D)
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -c $< -o $@

build/ro/libfolio.a: $(CORE_SRC:core/%.c=build/ro/%.o)
	rm -f $@; $(RO_AR) rcs $@ $^
build/al/libfolio.a: $(CORE_SRC:core/%.c=build/al/%.o)
	rm -f $@; $(AL_AR) rcs $@ $^

build/al/core_test: tests/core_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -lmupdf -lmupdf-third -lm -o $@
# Built for RISC OS too, so the checks see the core and MuPDF linked together.
build/ro/core_test.elf: tests/core_test.c build/ro/libfolio.a
	$(RO_CC) $(RO_CFLAGS) $(WARN) $(INC) -static $< build/ro/libfolio.a \
		-L$(MU_RO) -lmupdf -lmupdf-third -lm -o $@

tests/data/stamp: tests/make-data.sh tests/make-epub.py
	tests/make-data.sh $(MU_AL)/mutool $(RCD_DIR)/qemu/aligntrap.sh
	touch $@

test: build/al/core_test tests/data/stamp
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/core_test tests/data

check: build/ro/core_test.elf
	RISCOS_TOOLCHAIN=$(RISCOS_TOOLCHAIN) $(RCD_DIR)/checks/check-all.sh build/ro/core_test.elf

clean:
	rm -rf build tests/data/*.epub tests/data/*.pdf tests/data/stamp tests/out

.PHONY: all test check clean
