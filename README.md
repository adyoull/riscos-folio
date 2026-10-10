# Folio (working name): a RISC OS reader on MuPDF

A native RISC OS reader for PDF, EPUB, CBZ, XPS, FB2 and MOBI, with markup and
notebooks in the spirit of Flexcil. The design is in `docs/DESIGN.md`.

## State (2026-10-10): phase 1, the core
`core/` is MuPDF wrapped for the reader, with no Wimp code, so all of it runs
and is tested on ARM Linux under `qemu-arm-aligntrap` (RISC OS alignment
rules, Cortex-A8) as well as on RISC OS. See `core/folio.h`:

- **Documents:** open (with EPUB layout accelerator), format, title, author,
  language (EPUB `dc:language`, PDF `/Lang`).
- **Layout (re-flowed books):** page size, font size, margin, line spacing,
  publisher styles; a key for naming accelerator files.
- **Counting** a chapter at a time (for null polls), estimates until done;
  page number and location both ways; next and previous page.
- **Reading positions** (`folio_mark`) that survive re-layout and can be
  saved: chapter, fraction, and the page's first words. MuPDF's own EPUB
  bookmarks are pointers into the laid-out chapter, so they go stale.
- **Contents**, flattened, with locations.
- **Drawing tiles** straight into RISC OS 32bpp sprite pixels (&00BBGGRR),
  in normal, dark (black page, light text, pictures kept, pure black) and
  sepia (pictures kept) modes, with or without annotations.
- **Text:** selection snapping (characters, words, lines), selection quads,
  selected text, the word under the pointer (for double-click and the
  dictionary), case-insensitive find.
- **Links:** the link under the pointer and where it goes.

## The application (first test build, 0.0.1-test1)
`wimp/` is the RISC OS front end, a Toolbox application through our own
thin layer (`wimp/tbx.h`: in-memory object templates, so the Res file only
holds its header). It has an icon bar icon with Info, Help and Quit, and
document windows showing pages for continuous scrolling, drawn from a cache
of page sprites. Missing pages are drawn and ebook pages are counted in null
polls. View menu: Dark page (^N), Sepia page, zooms, fit width. Navigate:
page keys. `app/!Folio` holds the app's files; `make app` builds
`build/Folio-<version>.zip`.

## Building
Needs the riscos-crossdev 1.4 toolchain and a riscos-crossdev checkout, and
for the tests `gcc-arm-linux-gnueabihf`, `g++-arm-linux-gnueabihf`, `gawk`,
`python3`.

    mupdf/build.sh          # MuPDF 1.28.5 + mupdf/patches, for RISC OS and ARM Linux
    make RCD_DIR=...        # core for both, the test program for both
    make test               # 81 checks under qemu-arm-aligntrap
    make check              # riscos-crossdev checks on the RISC OS test program
    make app                # !Folio, checked, zipped with RISC OS filetypes

The RISC OS test program can also be run on the machine itself (CoreTest zip,
`RunCoreTest` in a TaskWindow).

## Licence
The program is **AGPL-3.0-or-later**, because it includes MuPDF (AGPL).
Our build scripts, Makefiles, tests, tools and docs are **MIT**. Details in
`LICENCES.md`; texts in `COPYING` and `LICENSE-MIT`. MuPDF is Copyright
Artifex Software.
