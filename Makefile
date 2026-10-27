# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Folio: build the core for RISC OS and for ARM Linux (the qemu test twin).
#
#   make            core libraries for both targets + the test program
#   make test       run the core tests under qemu-arm-aligntrap (RISC OS rules)
#   make check      riscos-crossdev's checks on the RISC OS test program
#   make ci         all of it from a clean checkout, as GitHub does in the
#                   riscos-crossdev image (.github/workflows): MuPDF,
#                   fontconfig, pvoice and Tesseract's data fetched and
#                   checked, every build, the tests and the four zips
#
# MuPDF comes from mupdf/build.sh (mupdf/src). RCD_DIR is a riscos-crossdev
# checkout (for checks/ and qemu/aligntrap.sh); RISCOS_TOOLCHAIN is its
# toolchain. The riscos-crossdev image sets both; otherwise set them on the
# command line, in the environment, or in config.mk (not in git), e.g.
#   RCD_DIR = $(HOME)/riscos-crossdev
#   RISCOS_TOOLCHAIN = $(HOME)/riscos-crossdev/toolchain/env

-include config.mk
RCD_DIR ?= ../riscos-crossdev
RISCOS_TOOLCHAIN ?= $(RCD_DIR)/toolchain/env
# riscos-pvoice (Piper voices for reading aloud): the devkit of the release
# .riscos-deps names, from tools/get-pvoice.sh; or a riscos-pvoice checkout
# built with `make`. Folio links its build/ro/libpvoice.a.
PVOICE_TAG := $(shell awk '$$1 == "riscos-pvoice" {print $$2}' .riscos-deps)
PVOICE_DIR ?= deps/riscos-pvoice-$(PVOICE_TAG:v%=%)
PV_INC := -I$(PVOICE_DIR)/src
# Tesseract's English data for the OCR plugin and its test, from
# tools/get-tessdata.sh: $(TESSDATA)/standard and $(TESSDATA)/fast.
TESSDATA ?= deps/tessdata
export PATH := $(RISCOS_TOOLCHAIN)/bin:$(PATH)
# Python for the sprite tools: build/pydeps (make ci's Pillow) first.
PY_PILLOW := PYTHONPATH=$(CURDIR)/build/pydeps python3
MUPDF ?= $(CURDIR)/mupdf/src
MU_RO := $(MUPDF)/build/ro-release-tesseract-tofu_cjk
MU_AL := $(MUPDF)/build/armlinux-release-tesseract-tofu_cjk
# MuPDF with Tesseract (OCR) is part C++.
MU_LIBS := -lfontconfig -lexpat -lftextra -lmupdf -lmupdf-third -lstdc++ -lm -lpthread

RO_CC := $(RISCOS_TOOLCHAIN)/bin/arm-riscos-gnueabihf-gcc
RO_AR := $(RISCOS_TOOLCHAIN)/bin/arm-riscos-gnueabihf-ar
RO_CFLAGS := -O2 -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -D_FILE_OFFSET_BITS=64
AL_CC := arm-linux-gnueabihf-gcc
AL_AR := arm-linux-gnueabihf-ar
AL_CFLAGS := -O2 -g -marm -mcpu=cortex-a8 -mfpu=vfpv3 -mfloat-abi=hard \
	-mno-unaligned-access -fstack-clash-protection
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -std=gnu99
INC := -Icore -Ithird/cjson -I$(MUPDF)/include

CORE_SRC := core/doc.c core/render.c core/text.c core/dict.c core/annot.c core/security.c core/drawfile.c \
	core/ocr.c core/info.c core/content.c core/form.c core/fonts.c core/pages.c core/escl.c
CORE_HDR := core/folio.h core/folio_int.h core/dict.h core/escl.h
CORE_OBJ = $(CORE_SRC:core/%.c=build/$(1)/%.o) build/$(1)/cJSON.o

WIMP_SRC := wimp/main.c wimp/docwin.c wimp/tbx.c wimp/worker.c wimp/clip.c wimp/dict.c wimp/contents.c wimp/find.c wimp/toolbar.c wimp/thumbs.c wimp/annotui.c wimp/passui.c wimp/bookshelf.c wimp/shelfdata.c \
	wimp/readtext.c wimp/speaker.c wimp/speech.c wimp/print.c wimp/printlay.c wimp/export.c wimp/ocrui.c wimp/gotopage.c wimp/docinfo.c wimp/choices.c wimp/ask.c wimp/formui.c wimp/fontsetup.c wimp/pagesave.c wimp/combine.c wimp/scanui.c wimp/pageorg.c wimp/annotlist.c
WIMP_HDR := wimp/app.h wimp/tbx.h wimp/worker.h wimp/shelfdata.h wimp/readtext.h wimp/speaker.h \
	core/folio.h core/dict.h
VERSION ?= 0.0.1-test61
# The plugins and the font pack have versions of their own, raised only
# when what's in them changes (not with each Folio build).
SPEECH_VERSION ?= 1.0
OCR_VERSION ?= 1.0
CJK_VERSION ?= 1.0

all: build/ro/libfolio.a build/al/libfolio.a build/al/core_test build/ro/core_test.elf build/ro/folio.elf

# System fonts through fontconfig (fontconfig/build.sh).
build/ro/fonts.o: core/fonts.c $(CORE_HDR)
	@mkdir -p $(dir $@)
	$(RO_CC) $(RO_CFLAGS) $(WARN) $(INC) -DFOLIO_FONTCONFIG -Ifontconfig/out/ro/include -c $< -o $@
build/al/fonts.o: core/fonts.c $(CORE_HDR)
	@mkdir -p $(dir $@)
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -DFOLIO_FONTCONFIG -Ifontconfig/out/al/include -c $< -o $@

# Scanning (eSCL): expat for the scanner's XML, as fontconfig has it.
build/ro/escl.o: core/escl.c core/escl.h
	@mkdir -p $(dir $@)
	$(RO_CC) $(RO_CFLAGS) $(WARN) -Ifontconfig/out/ro/include -c $< -o $@
build/al/escl.o: core/escl.c core/escl.h
	@mkdir -p $(dir $@)
	$(AL_CC) $(AL_CFLAGS) $(WARN) -Ifontconfig/out/al/include -c $< -o $@

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
		-L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@
# Built for RISC OS too, so the checks see the core and MuPDF linked together.
build/ro/core_test.elf: tests/core_test.c build/ro/libfolio.a build/ro/rostubs.o
	$(RO_CC) $(RO_CFLAGS) $(WARN) $(INC) -static $< build/ro/libfolio.a build/ro/rostubs.o \
		-L$(MU_RO) -Lfontconfig/out/ro/lib $(MU_LIBS) -o $@

build/ro/wimp/%.o: wimp/%.c $(WIMP_HDR)
	@mkdir -p $(@D)
	$(RO_CC) $(RO_CFLAGS) $(WARN) $(INC) $(PV_INC) -Iwimp -c $< -o $@

# The application (RISC OS only).
build/ro/folio.elf: $(WIMP_SRC:wimp/%.c=build/ro/wimp/%.o) build/ro/libfolio.a build/ro/rostubs.o
	$(RO_CC) $(RO_CFLAGS) -static $(WIMP_SRC:wimp/%.c=build/ro/wimp/%.o) build/ro/libfolio.a build/ro/rostubs.o \
		$(PVOICE_DIR)/build/ro/libpvoice.a -L$(MU_RO) -Lfontconfig/out/ro/lib $(MU_LIBS) -o $@

# !Folio in build/app, and a zip with RISC OS filetypes.
app: build/ro/folio.elf
	RISCOS_TOOLCHAIN=$(RISCOS_TOOLCHAIN) $(RCD_DIR)/checks/check-all.sh build/ro/folio.elf
	rm -rf build/app && mkdir -p build/app
	cp -r app/!Folio build/app/
	$(RISCOS_TOOLCHAIN)/bin/arm-riscos-gnueabihf-strip -o build/ro/folio.stripped build/ro/folio.elf
	$(RISCOS_TOOLCHAIN)/bin/elf2aif -e build/ro/folio.stripped 'build/app/!Folio/!RunImage,ff8'
	$(PY_PILLOW) tools/mksprites.py 'build/app/!Folio/!Sprites,ff9'
	$(PY_PILLOW) tools/mktoolsprites.py 'build/app/!Folio/Sprites,ff9'
	cp COPYING 'build/app/!Folio/COPYING,fff'
	cp third/cjson/LICENSE 'build/app/!Folio/cJSONLicence,fff'
	cp $(RISCOS_TOOLCHAIN)/riscos/PThrTicker,ffa 'build/app/!Folio/PThrTicker,ffa'
	cp $(RISCOS_TOOLCHAIN)/riscos/PThreadTicker-Licence,fff 'build/app/!Folio/PThreadTickerLicence,fff'
	cp $(PVOICE_DIR)/third/espeak-ng/COPYING 'build/app/!Folio/eSpeakLicence,fff'
	cp fontconfig/src/ro/fontconfig-2.14.1/COPYING 'build/app/!Folio/FontconfigLicence,fff'
	cp fontconfig/src/ro/libexpat-R_2_2_9/expat/COPYING 'build/app/!Folio/ExpatLicence,fff'
	mkdir -p 'build/app/!Folio/Plugins'
	cp app/Plugins-ReadMe 'build/app/!Folio/Plugins/ReadMe,fff'
	python3 $(RCD_DIR)/package/rozip.py build/Folio-$(VERSION).zip -C build/app '!Folio'
	@ls -l build/Folio-$(VERSION).zip

# The Speech plugin: a voice, eSpeak NG's English data and LoadSound, in a
# Speech folder for !Folio.Plugins (or <Choices$Write>.Folio.Plugins).
SPEECH_VOICE ?= $(PVOICE_DIR)/voices/en_GB-southern_english_female-low.voice
# FolioSpeech-<version>.zip: a !Folio holding just Plugins.Speech, to drag
# over the reader's own !Folio (the Filer merges them), as OpenTTD's sound
# packs are; and a ReadMe saying so.
plugin:
	rm -rf build/plugin && mkdir -p build/plugin/!Folio/Plugins/Speech/Voices
	cp -r app/Speech/* build/plugin/!Folio/Plugins/Speech/
	cp $(SPEECH_VOICE) "build/plugin/!Folio/Plugins/Speech/Voices/$$(basename $(SPEECH_VOICE) .voice),ffd"
	cp -r $(PVOICE_DIR)/build/espeak-ng-data build/plugin/!Folio/Plugins/Speech/espeak-ng-data
	for f in phontab phonindex phondata intonations en_dict; do \
		mv build/plugin/!Folio/Plugins/Speech/espeak-ng-data/$$f build/plugin/!Folio/Plugins/Speech/espeak-ng-data/$$f,ffd; done
	for f in build/plugin/!Folio/Plugins/Speech/espeak-ng-data/lang/gmw/*; do mv "$$f" "$$f,fff"; done
	cp $(PVOICE_DIR)/third/espeak-ng/COPYING build/plugin/!Folio/Plugins/Speech/eSpeakLicence,fff
	cp $(PVOICE_DIR)/docs/VOICES.txt build/plugin/!Folio/Plugins/Speech/VoiceLicence,fff
	cp app/Speech-Install build/plugin/ReadMe,fff
	python3 $(RCD_DIR)/package/rozip.py build/FolioSpeech-$(SPEECH_VERSION).zip -C build/plugin '!Folio' 'ReadMe,fff'
	@ls -l build/FolioSpeech-$(SPEECH_VERSION).zip

# The OCR plugin: Tesseract's English data in two sizes, Accurate (the
# standard set without its unused Tesseract 3 part, about 5 MB) and Fast
# (about 4 MB), in an OCR folder for !Folio.Plugins; zipped as the Speech
# plugin is, to drag over the reader's own !Folio.
ocrplugin:
	rm -rf build/ocrplugin && mkdir -p build/ocrplugin/!Folio/Plugins/OCR/Accurate build/ocrplugin/!Folio/Plugins/OCR/Fast
	python3 -I tools/strip-tessdata.py $(TESSDATA)/standard/eng.traineddata 'build/ocrplugin/!Folio/Plugins/OCR/Accurate/eng,ffd'
	cp $(TESSDATA)/fast/eng.traineddata 'build/ocrplugin/!Folio/Plugins/OCR/Fast/eng,ffd'
	cp app/OCR/ReadMe 'build/ocrplugin/!Folio/Plugins/OCR/ReadMe,fff'
	cp $(TESSDATA)/standard/LICENSE 'build/ocrplugin/!Folio/Plugins/OCR/DataLicence,fff'
	cp app/OCR-Install build/ocrplugin/ReadMe,fff
	python3 $(RCD_DIR)/package/rozip.py build/FolioOCR-$(OCR_VERSION).zip -C build/ocrplugin '!Folio' 'ReadMe,fff'
	@ls -l build/FolioOCR-$(OCR_VERSION).zip

# The CJK font pack: Droid Sans Fallback (from MuPDF's sources) for
# !Folio.Fonts, for text in Chinese, Japanese and Korean that isn't embedded.
cjkpack:
	rm -rf build/cjkpack && mkdir -p build/cjkpack/!Folio/Fonts
	cp $(MUPDF)/resources/fonts/droid/DroidSansFallbackFull.ttf 'build/cjkpack/!Folio/Fonts/DroidSansFallbackFull.ttf'
	cp $(MUPDF)/resources/fonts/droid/NOTICE 'build/cjkpack/!Folio/Fonts/FontLicence,fff'
	cp app/CJK-Install build/cjkpack/ReadMe,fff
	python3 $(RCD_DIR)/package/rozip.py build/FolioCJK-$(CJK_VERSION).zip -C build/cjkpack '!Folio' 'ReadMe,fff'
	@ls -l build/FolioCJK-$(CJK_VERSION).zip

tests/data/stamp: tests/make-data.sh tests/make-epub.py tests/make-form.py
	tests/make-data.sh $(MU_AL)/mutool $(RCD_DIR)/qemu/aligntrap.sh
	touch $@

# The page worker, with a stand-in for the one RISC OS call it makes.
build/al/worker_test: tests/worker_test.c wimp/worker.c wimp/worker.h build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -Iwimp -Itests/stub -no-pie tests/worker_test.c wimp/worker.c \
		build/al/libfolio.a -L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@

build/al/dict_test: tests/dict_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@

build/al/annot_test: tests/annot_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@

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
		-L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@

build/al/draw_test: tests/draw_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@

build/al/password_test: tests/password_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@

build/al/ocr_test: tests/ocr_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@

build/al/form_test: tests/form_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@

build/al/escl_test: tests/escl_test.c build/al/escl.o
	$(AL_CC) $(AL_CFLAGS) $(WARN) -Icore -no-pie $< build/al/escl.o -Lfontconfig/out/al/lib -lexpat -o $@

build/al/pages_test: tests/pages_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@

build/al/info_test: tests/info_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@

build/al/fonts_test: tests/fonts_test.c build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -no-pie $< build/al/libfolio.a \
		-L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@

build/al/shelf_test: tests/shelf_test.c wimp/shelfdata.c wimp/shelfdata.h
	$(AL_CC) $(AL_CFLAGS) $(WARN) -Iwimp -no-pie tests/shelf_test.c wimp/shelfdata.c -o $@

# Find, with stand-ins for the Toolbox and the document window.
build/al/find_test: tests/find_test.c wimp/find.c wimp/worker.c $(WIMP_HDR) build/al/libfolio.a
	$(AL_CC) $(AL_CFLAGS) $(WARN) $(INC) -Iwimp -Itests/stub-wimp -no-pie tests/find_test.c wimp/find.c \
		wimp/worker.c build/al/libfolio.a -L$(MU_AL) -Lfontconfig/out/al/lib $(MU_LIBS) -o $@

test: build/al/core_test build/al/worker_test build/al/dict_test build/al/find_test build/al/annot_test build/al/password_test build/al/draw_test build/al/pagetext_test build/al/readtext_test build/al/printlay_test build/al/speaker_test build/al/shelf_test build/al/ocr_test build/al/form_test build/al/fonts_test build/al/pages_test build/al/escl_test build/al/info_test tests/data/stamp
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/core_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/worker_test tests/data/doc.pdf
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/dict_test tests/dict
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/find_test tests/data/doc.pdf
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/annot_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/password_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/draw_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/pagetext_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/readtext_test
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/printlay_test
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/speaker_test \
		$(PVOICE_DIR)/voices/en_GB-southern_english_female-low.voice $(PVOICE_DIR)/build/espeak-ng-data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/shelf_test
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/ocr_test tests/data $(TESSDATA)/fast
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/form_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/fonts_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/pages_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(RCD_DIR)/qemu/aligntrap.sh build/al/info_test tests/data
	PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH tests/escl-test.sh $(RCD_DIR)/qemu/aligntrap.sh build/al/escl_test

check: build/ro/core_test.elf
	RISCOS_TOOLCHAIN=$(RISCOS_TOOLCHAIN) $(RCD_DIR)/checks/check-all.sh build/ro/core_test.elf

# Everything from a clean checkout (GitHub, in the riscos-crossdev image).
# The sprite tools need Pillow: if python3 hasn't got it, Pillow 12.3.0 is
# installed into build/pydeps with pip.
ci:
	tools/ci-host-deps.sh
	$(PY_PILLOW) -c 'import PIL' 2>/dev/null || \
		python3 -m pip install --quiet --no-cache-dir --disable-pip-version-check --target build/pydeps pillow==12.3.0
	mupdf/build.sh
	fontconfig/build.sh
	tools/get-pvoice.sh
	tools/get-tessdata.sh $(TESSDATA)
	$(MAKE) all
	$(MAKE) check
	$(MAKE) app plugin ocrplugin cjkpack
	$(MAKE) test

clean:
	rm -rf build tests/data/*.epub tests/data/*.pdf tests/data/stamp tests/out

.PHONY: all app plugin ocrplugin cjkpack test check ci clean
