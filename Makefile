# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Folio: build the core for RISC OS and for ARM Linux (the qemu test twin).
#
#   make            core libraries for both targets + the test program
#   make test       run the core tests under qemu-arm-aligntrap (RISC OS rules)
#   make check      riscos-crossdev's checks on the RISC OS test program
#
# MuPDF comes from mupdf/build.sh (mupdf/src). RCD_DIR is a riscos-crossdev
# checkout (for checks/ and qemu/aligntrap.sh); RISCOS_TOOLCHAIN is the
# riscos-crossdev 1.4 toolchain. Set them on the command line, in the
# environment, or in config.mk (not in git), e.g.
#   RCD_DIR = $(HOME)/riscos-crossdev
#   RISCOS_TOOLCHAIN = /opt/riscos-crossdev-toolchain-1.4-x86_64-linux

-include config.mk
RCD_DIR ?= ../riscos-crossdev
RISCOS_TOOLCHAIN ?= /opt/riscos-crossdev-toolchain-1.4-x86_64-linux
# riscos-pvoice (Piper voices for reading aloud): built there with `make`
# (after its tools/build-espeak.sh); Folio links its build/ro/libpvoice.a.
PVOICE_DIR ?= ../riscos-pvoice
PV_INC := -I$(PVOICE_DIR)/src
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

CORE_SRC := core/doc.c core/render.c core/text.c core/dict.c core/annot.c core/security.c
CORE_HDR := core/folio.h core/folio_int.h core/dict.h
CORE_OBJ = $(CORE_SRC:core/%.c=build/$(1)/%.o) build/$(1)/cJSON.o

WIMP_SRC := wimp/main.c wimp/docwin.c wimp/tbx.c wimp/worker.c wimp/clip.c wimp/dict.c wimp/contents.c wimp/find.c wimp/toolbar.c wimp/thumbs.c wimp/annotui.c wimp/passui.c wimp/bookshelf.c wimp/shelfdata.c \
	wimp/readtext.c wimp/speaker.c wimp/speech.c wimp/print.c wimp/printlay.c
WIMP_HDR := wimp/app.h wimp/tbx.h wimp/worker.h wimp/shelfdata.h wimp/readtext.h wimp/speaker.h \
	core/folio.h core/dict.h
VERSION ?= 0.0.1-test21

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
	$(RO_CC) $(RO_CFLAGS) $(WARN) $(INC) $(PV_INC) -Iwimp -c $< -o $@

# The application (RISC OS only).
build/ro/folio.elf: $(WIMP_SRC:wimp/%.c=build/ro/wimp/%.o) build/ro/libfolio.a
	$(RO_CC) $(RO_CFLAGS) -static $(WIMP_SRC:wimp/%.c=build/ro/wimp/%.o) build/ro/libfolio.a \
		$(PVOICE_DIR)/build/ro/libpvoice.a -L$(MU_RO) -lmupdf -lmupdf-third -lm -lpthread -o $@

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
	cp $(PVOICE_DIR)/third/espeak-ng/COPYING 'build/app/!Folio/eSpeakLicence,fff'
	mkdir -p 'build/app/!Folio/Plugins'
	cp app/Plugins-ReadMe 'build/app/!Folio/Plugins/ReadMe,fff'
	python3 $(RCD_DIR)/package/rozip.py build/Folio-$(VERSION).zip -C build/app '!Folio'
	@ls -l build/Folio-$(VERSION).zip

# The Speech plugin: a voice, eSpeak NG's English data and LoadSound, in a
# Speech folder for !Folio.Plugins (or <Choices$Write>.Folio.Plugins).
SPEECH_VOICE ?= $(PVOICE_DIR)/voices/en_GB-southern_english_female-low.voice
plugin:
	rm -rf build/plugin && mkdir -p build/plugin/Speech/Voices
	cp -r app/Speech/* build/plugin/Speech/
	cp $(SPEECH_VOICE) "build/plugin/Speech/Voices/$$(basename $(SPEECH_VOICE) .voice),ffd"
	cp -r $(PVOICE_DIR)/build/espeak-ng-data build/plugin/Speech/espeak-ng-data
	for f in phontab phonindex phondata intonations en_dict; do \
		mv build/plugin/Speech/espeak-ng-data/$$f build/plugin/Speech/espeak-ng-data/$$f,ffd; done
	for f in build/plugin/Speech/espeak-ng-data/lang/gmw/*; do mv "$$f" "$$f,fff"; done
	cp $(PVOICE_DIR)/third/espeak-ng/COPYING build/plugin/Speech/eSpeakLicence,fff
	cp $(PVOICE_DIR)/docs/VOICES.txt build/plugin/Speech/VoiceLicence,fff
	python3 $(RCD_DIR)/package/rozip.py build/FolioSpeech-$(VERSION).zip -C build/plugin Speech
	@ls -l build/FolioSpeech-$(VERSION).zip

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

build/al/printlay_test: tests/printlay_test.c wimp/printlay.c wimp/printlay.h
	$(AL_CC) $(AL_CFLAGS) $(WARN) -Iwimp -no-pie tests/printlay_test.c wimp/printlay.c -lm -o $@
build/al/readtext_test: tests/readtext_test.c wimp/readtext.c wimp/readtext.h
	$(AL_CC) $(AL_CFLAGS) $(WARN) -Icore -Iwimp -no-pie tests/readtext_test.c wimp/readtext.c -o $@

# The speech thread with the real voice (slow under qemu: a minute or two).
build/al/speaker_test: tests/speaker_test.c wimp/speaker.c wimp/speaker.h
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(PV_INC) -Iwimp -no-pie tests/speaker_test.c wimp/speaker.c \
		$(PVOICE_DIR)/build/al/libpvoice.a -lm -lpthread -o $@

build/al/pagetext_test: tests/pagetext_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -lmupdf -lmupdf-third -lm -lpthread -o $@

build/al/password_test: tests/password_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -lmupdf -lmupdf-third -lm -lpthread -o $@

build/al/shelf_test: tests/shelf_test.c wimp/shelfdata.c wimp/shelfdata.h
	$(AL_CC) $(AL_CFLAGS) $(WARN) -Iwimp -no-pie tests/shelf_test.c wimp/shelfdata.c -o $@

# Find, with stand-ins for the Toolbox and the document window.
build/al/find_test: tests/find_test.c wimp/find.c wimp/worker.c $(WIMP_HDR) build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -Iwimp -Itests/stub-wimp -no-pie tests/find_test.c wimp/find.c \
		wimp/worker.c build/al/libfolio.a -L$(MU_AL) -lmupdf -lmupdf-third -lm -lpthread -o $@

test: build/al/core_test build/al/worker_test build/al/dict_test build/al/find_test build/al/annot_test build/al/password_test build/al/pagetext_test build/al/readtext_test build/al/printlay_test build/al/speaker_test build/al/shelf_test tests/data/stamp
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/core_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/worker_test tests/data/doc.pdf
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/dict_test tests/dict
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/find_test tests/data/doc.pdf
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/annot_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/password_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/pagetext_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/readtext_test
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/printlay_test
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/speaker_test \
		$(PVOICE_DIR)/voices/en_GB-southern_english_female-low.voice $(PVOICE_DIR)/build/espeak-ng-data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/shelf_test

check: build/ro/core_test.elf
	RISCOS_TOOLCHAIN=$(RISCOS_TOOLCHAIN) $(RCD_DIR)/checks/check-all.sh build/ro/core_test.elf

clean:
	rm -rf build tests/data/*.epub tests/data/*.pdf tests/data/stamp tests/out

.PHONY: all app plugin test check clean
