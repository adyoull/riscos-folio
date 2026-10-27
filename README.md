# Folio: a reader for RISC OS

Folio reads PDF, EPUB, FB2, MOBI, CBZ and XPS files on the RISC OS desktop.
It's built on [MuPDF](https://mupdf.com) with a native front end that follows
the RISC OS Style Guide, and takes its ideas for markup and reading from
Flexcil. "Folio" is a working name.

Status: **test builds** (0.0.1-test61). It's used day to day on a Raspberry Pi
running RISC OS 5, but expect rough edges. The design is in
[`docs/DESIGN.md`](docs/DESIGN.md).

## What it does

- **Reading:** pages for continuous scrolling, drawn in a background thread,
  so the desktop never waits; ebooks laid out and counted in the background;
  Folio reopens each document where you left it.
- **View:** zoom, fit width, **two pages side by side** (with the first
  page alone, as in a book, or not), **dark page** (black page, light text,
  pictures kept) and sepia, each with a toolbar button; a **thumbnail
  drawer** down the left of the window; a **default scale** for new
  documents, for anyone who reads better larger.
- **Navigating:** **links** (to other pages, with Back; web links open in the
  browser), a **Contents** window, **Find** (F4), page keys, a page number to
  type into on the toolbar; open a document at a given page from a script
  (`Set Folio$OpenPage 12`, then `Filer_Run` the file).
- **Export page as Draw:** the page as a Draw file, with text as outlines
  and each picture as a sprite of its own.
- **Pages out, pictures in:** *Save pages as PDF* makes a new PDF of chosen
  pages (1-10, or 1-3, 20, 45-50), a PDF's pages copied as they are; a CBZ's
  JPEGs go in unchanged. *Save page pictures* writes the chosen pages as
  pictures in a directory: a scanned page's own JPEG, byte for byte, else a
  PNG. *Save pages as text* writes their text as a text file, a paragraph
  a line (scanned pages' recognised text included). Drag a **directory of pictures** (JPEG, PNG, GIF, BMP, TIFF,
  PBM/PGM/PPM) to Folio to make a PDF of them, a page each in name order,
  JPEGs not compressed again.
- **Scan** (icon bar menu): scan from a network scanner with eSCL (AirScan),
  no driver needed: find it on the network or type its address, choose
  glass or feeder (both sides), colour or grey, resolution, size, brightness
  and contrast (done by the scanner), and the
  pages open as a new PDF with the scanner's JPEGs as they came.
- **Pages** (PDFs): turn the page under the pointer (Ctrl-R, Ctrl-L) or every
  page; delete pages; move them; insert the pages of another PDF, picture or
  document. Each is undoable and kept when saved; recognised text goes with
  its page.
- **Annotations:** a list of every annotation in the document (go to one by
  clicking it), and a choice of colour for new markings, or to recolour one.
- **Create PDF** (icon bar menu): drag PDFs, pictures and other documents
  into a list, put them in order, and join them into one new PDF (PDF pages
  copied as they are, JPEGs unchanged).
- **Printing** through the printer drivers (pages, copies, scale, sideways,
  draft), in the background, with the window's title showing the page.
- **Text:** select by dragging, double-click a word, Copy to the global
  clipboard, and **Define**: a word's meanings from Wiktionary (with the
  Free Dictionary API as a fallback), fetched through URL_Fetcher and kept
  for offline use.
- **Annotations (PDFs):** highlight, underline, strike out and notes, with
  undo and redo (Ctrl-Z, Ctrl-Y), saved into the PDF (incrementally, so
  signatures survive); Discard/Cancel/Save on closing with changes.
- **Passwords (PDFs):** Folio asks for the password of a protected PDF,
  and can put a password on a PDF (AES-256) or take one off.
  It keeps to a PDF's "no printing", "no copying" and "no filling in"
  restrictions (the owner password lifts them; the Choices window can turn
  this off).
- **Forms (PDFs):** fill in text fields (typed over the page), check boxes,
  radio buttons, lists and drop-downs, with undo, and save them into the
  PDF. A form's JavaScript (totals, checks as you type) runs only if
  Choices allow it, through MuPDF's sandbox; forms are never sent anywhere.
- **Security:** Folio asks before following a link out of a document,
  showing where it goes, never starts other programs, and says when a PDF
  it opens has JavaScript, attached files or actions that would.
- **Full screen** reading (F11; F12 stays the command line).
- **Choices window:** how new documents open, reading-aloud speed, text
  recognition and the security choices.
- **Fonts through fontconfig:** for PDFs that don't embed their fonts and
  for scripts Folio has no font of its own for (Chinese, Japanese, Korean),
  the computer's fonts: PackMan's FontConfig (UnixFC) setup if it's
  installed, else the fonts in `!Folio.Fonts`.
- **Document info:** the file, title, author, dates, PDF version, page
  size, security and permissions, forms, attachments, JavaScript,
  bookmarks, layers, pictures, and every font the PDF uses, with its kind
  and whether it's embedded (whole or a subset).
- **Reading aloud** (with the Speech plugin, a separate download: drag the
  !Folio in it over yours, as with OpenTTD's sound packs): Piper's
  natural-sounding voices run by
  [riscos-pvoice](https://github.com/adyoull/riscos-pvoice), with eSpeak NG
  for the phonemes; the sentence being read is shown and the pages turn.
  Without the plugin Folio has no speech button and loads nothing for it.
  Speech needs NEON (a Raspberry Pi 2 or later; it keeps up on a Pi 4) and
  John Duffell's SharedSoundBuffer and StreamManager modules.
- **Scanned pages** (with the OCR plugin, a separate download dropped in
  the same way): Tesseract, through MuPDF, reads the text in scanned pages
  in the background, as you come to them or the whole document, so Find,
  selecting, Copy, Define and reading aloud work on scans. Two sets of
  English data: Accurate (Tesseract's standard set without its unused
  Tesseract 3 part, about 5 MB) and Fast (about 4 MB). What's recognised is
  kept, so it isn't done again.
- **Bookshelf:** Select on the icon bar icon. Covers, progress, sorting by
  recent, title or author; drag books to it, or drag directories to have
  Folio watch them for new documents when it starts and on Refresh.

Not yet: annotations in ebooks, bookmarks, exporting pages as SVG, text,
HTML or PDF.

## Running it

Needs RISC OS 5 on ARMv7 or later (a Raspberry Pi 2 or newer, for example),
a 32 thousand or more colour screen mode, and the SharedUnixLibrary,
ARMEABISupport and PThreadTicker modules (PThreadTicker is in the zip).
Looking words up needs the network; Folio loads URL_Fetcher, AcornHTTP and
AcornSSL from !System when it first needs them.

Unzip `Folio-<version>.zip` (with filetypes) and double-click `!Folio`.
`!Folio.!Help` describes everything. If something goes wrong,
`<Wimp$ScrapDir>.FolioLog` says what Folio did (it starts afresh each run). Report problems, with the log, in Folio's thread on the [RISC OS Open forum](https://www.riscosopen.org/forum/) or as a [GitHub issue](https://github.com/adyoull/riscos-folio/issues).

## Building

Folio is cross-built on Linux (x86-64) with the
[riscos-crossdev](https://github.com/adyoull/riscos-crossdev) toolchain (GCC 10.2,
UnixLib, ELF to AIF). The tests run on ARM Linux under
`qemu-arm-aligntrap`, which applies RISC OS's alignment rules.

The simplest way is riscos-crossdev's container image, which has the
toolchain and everything else:

    docker run --rm -v "$PWD":/src ghcr.io/adyoull/riscos-crossdev:1.8.1 make ci

`make ci` does all of it from a clean checkout: MuPDF 1.28.5 (patched),
expat and fontconfig, the [riscos-pvoice](https://github.com/adyoull/riscos-pvoice)
devkit of the release `.riscos-deps` names, and Tesseract's English data,
each fetched at a fixed version and checked by sha256; then the builds,
riscos-crossdev's checks, the four zips in `build/` and the tests. GitHub
runs the same (`.github/workflows/riscos-build.yml`). `.riscos-deps` names
the riscos-crossdev and riscos-pvoice releases Folio is built with;
`riscos-deps-update.yml` opens a pull request when either has a newer one
(`ci/riscos-deps-hook.sh` moves pvoice's checksum in `tools/pvoice.sha256`).

Without the image you need a riscos-crossdev checkout and its toolchain,
`gcc-arm-linux-gnueabihf`, `g++-arm-linux-gnueabihf`, `gawk`, `gperf`,
`git`, `curl` and `python3` with Pillow, and the steps one at a time:

    # config.mk (not in git), or pass these on the command line
    RCD_DIR = /path/to/riscos-crossdev
    RISCOS_TOOLCHAIN = /path/to/riscos-crossdev/toolchain/env
    # PVOICE_DIR = /path/to/riscos-pvoice   # a built checkout, instead of the release

    mupdf/build.sh         # fetches MuPDF 1.28.5, applies mupdf/patches, builds for RISC OS and ARM Linux
    fontconfig/build.sh    # expat 2.2.9 and fontconfig 2.14.1 against MuPDF's FreeType
    tools/get-pvoice.sh    # the riscos-pvoice devkit, in deps/
    tools/get-tessdata.sh  # Tesseract's English data, in deps/tessdata
    make test              # the tests, under qemu-arm-aligntrap
    make app               # !Folio, checked, zipped with RISC OS filetypes: build/Folio-<version>.zip
    make plugin            # the Speech plugin: build/FolioSpeech-<version>.zip
    make ocrplugin         # the OCR plugin: build/FolioOCR-<version>.zip
    make cjkpack           # the CJK font pack: build/FolioCJK-<version>.zip

MuPDF is built with its bundled Tesseract and Leptonica (OCR); Tesseract's
NEON code is compiled with `-mfpu=neon` and chosen at run time, the rest
of the program stays VFPv3.

## Layout

| Directory | What's there |
|---|---|
| `core/` | MuPDF wrapped for the reader, with no Wimp code: documents, layout and counting, reading positions, drawing tiles into 32bpp sprite pixels (normal, dark, sepia), text selection and find, links, annotations and saving, passwords, the dictionary's URLs and HTML, text recognition (OCR) and keeping its results, forms, what a PDF holds, system fonts through fontconfig |
| `wimp/` | The RISC OS front end: a Toolbox application through a thin layer of our own (`tbx.h`, objects from in-memory templates), the page worker thread, document windows, toolbar, thumbnail drawer, Contents, Find, the dictionary pop-up, annotation and password dialogues and the Bookshelf |
| `app/!Folio` | The application's files |
| `tests/` | Tests for the core, the worker, the dictionary, Find, annotations, passwords and the Bookshelf's data, run under qemu |
| `tools/` | The icon and toolbar sprites, drawn by script |
| `mupdf/` | MuPDF's build script and patches |
| `fontconfig/` | fontconfig and expat's build script and patch (the Warzone 2100 port's RISC OS changes) |
| `third/cjson` | cJSON 1.7.18 (MIT), for the dictionary's JSON |

## Licence

Folio is **AGPL-3.0-or-later**, because it includes MuPDF, which is AGPL.
The build scripts, Makefiles, tests, tools and docs are **MIT**. Details are
in [`LICENCES.md`](LICENCES.md); the texts are in `COPYING` and
`LICENSE-MIT`. MuPDF is Copyright Artifex Software. Dictionary definitions
come from Wiktionary, under CC BY-SA.
