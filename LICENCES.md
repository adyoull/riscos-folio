# Licences

| Part | Licence | Text |
|---|---|---|
| **The program**: `core/` and the Wimp front end (`wimp/`), and every Folio binary, which includes MuPDF | **AGPL-3.0-or-later** | `COPYING` |
| **Our build scripts, Makefiles, tests, tools and docs**: `Makefile`, `mupdf/build.sh`, `fontconfig/build.sh`, `tests/`, `tools/`, `docs/`, `README.md`, this file | **MIT** | `LICENSE-MIT` |
| Our patches to MuPDF (`mupdf/patches/`) | AGPL-3.0-or-later, as MuPDF | `COPYING` |
| The fontconfig patch (`fontconfig/patches/`, from the Warzone 2100 port) | MIT, as fontconfig | `LICENSE-MIT` |
| MuPDF 1.28.5 (Artifex Software), fetched by `mupdf/build.sh` | AGPL-3.0-or-later | MuPDF's `COPYING` |
| MuPDF's bundled libraries (FreeType, HarfBuzz, zlib, libjpeg, OpenJPEG, jbig2dec, lcms2, gumbo, MuJS, brotli, extract) | Their own licences | in MuPDF's `thirdparty/` |
| cJSON 1.7.18 (Dave Gamble and contributors), in `third/cjson/` | MIT | `third/cjson/LICENSE` |
| riscos-pvoice (the Piper voice engine) and eSpeak NG, linked into the program for reading aloud | GPL-3.0-or-later (combined with the AGPL under its section 13) | riscos-pvoice's `COPYING`; `eSpeakLicence` in !Folio |
| The Speech plugin's voice (Piper, trained on OpenSLR SLR83) | CC BY-SA 4.0 | `VoiceLicence` in the plugin |
| fontconfig 2.14.1, expat 2.2.9 | MIT-style | !Folio.FontconfigLicence, !Folio.ExpatLicence |

**Why the program is AGPL:** MuPDF is AGPL-3.0. A program that includes it can be
distributed only under the AGPL (or under Artifex's commercial licence, which
this project doesn't have). So Folio's binaries, and the source of the parts
built into them, are AGPL, and the full source (including `mupdf/patches/` and
the build scripts) is published with every release.

**Why the scripts are MIT:** they don't become part of the program, so they
can be reused freely, as in riscos-crossdev.

The test programs (`tests/*.c`) are MIT as source. A *built* test program
includes MuPDF, so a distributed binary of one (e.g. the CoreTest zip) is
covered by the AGPL like any other Folio binary.

Each source file starts with an SPDX line saying which applies.
