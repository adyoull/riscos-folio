# A RISC OS reader on MuPDF, with Flexcil's best ideas — design (2026-10-10, rev 5)

Working name **!Folio** (the owner chooses the real name). Engine: MuPDF 1.28.5, built with
riscos-crossdev 1.4. The front end is a native Wimp program in C. No SDL, no GTK.

Rev 5 (2026-10-10): fonts through fontconfig 2.14.1 (§6, owner's choice). Licence: the
program is AGPL, our scripts, tests and tools are MIT.

Rev 4 (2026-10-10): §3.8 addendum: ^Z undo and ^Y redo, as on other systems (owner's
decision; departs from SG p73).

Rev 3 (2026-10-10): §3 rewritten against the Style Guide and the Toolbox manual, which the
owner added to `PRMs/`.

Rev 2 (owner, 2026-10-10):
- the dictionary and the CJK/script font pack move into phase 1;
- the app follows the RISC OS Style Guide (§3);
- a dark mode (black page, white text) is added (§4).

## 1. What the spikes showed (2026-10-10)

- **MuPDF cross-builds with one patch.** UnixLib has no `getentropy()`, so `fz_memrnd` uses
  MuPDF's own time/random fallback (`patches/0001`). Everything else in MuPDF and its
  bundled libraries compiles as it is: freetype, harfbuzz, zlib, libjpeg, openjpeg,
  jbig2dec, lcms2, gumbo, mujs, brotli and extract.
- **riscos-crossdev checks all pass** on `mutool` and `muprobe`:
  - static linking;
  - 47 stack frames of 4 KB or more, all probed;
  - no VFPv4 instructions;
  - UnixLib's 640-byte ticker block.
- **Size:** `mutool,ff8` is 18 MB built with `tofu_cjk=yes`. MuPDF's font files are:
  - Source Han CJK, 24 MB;
  - Droid CJK fallback, 8 MB;
  - Noto for other scripts, 14 MB;
  - URW base 14, 3.5 MB;
  - SIL, 1.9 MB.

  §6 moves the large ones into an optional pack.
- **The same code runs as an ARM Linux build** under `qemu-arm-aligntrap`, which emulates a
  Cortex-A8 with RISC OS alignment rules:
  - EPUB re-flows, draws, and its contents list resolves to pages;
  - a PDF gets a highlight, an ink stroke and a sticky note, is saved incrementally and
    reloads;
  - `muprobe` draws a page straight into a valid 32bpp sprite (type 6, 90 dpi;
    PRM p1177–1178);
  - **dark mode** (§4) works on both PDF and EPUB.
- **Every API function the plan relies on is in 1.28.5:**
  - `pdf_undo` / `pdf_redo` and the journal;
  - annotation creation, quad points and ink lists;
  - page rearrange, graft, insert and delete;
  - the outline iterator;
  - `fz_make_bookmark` / `fz_lookup_bookmark`;
  - `fz_search_page`, `fz_highlight_selection` and `fz_copy_selection`;
  - `fz_style_document`;
  - `fz_run_page_contents` / `fz_run_page_annots`;
  - `fz_invert_pixmap_luminance` and `fz_tint_pixmap`;
  - `fz_install_load_system_font_funcs`.
- **Not yet known:** speed on a Pi (`MuPDFTest-spike2.zip` answers it). Also,
  `hyphens:auto` didn't hyphenate in the test book (probably needs the language set).

## 2. Flexcil's best features and how each maps to RISC OS

Flexcil is an iPad/Android PDF study app: pen gestures, a notebook beside the PDF, and
dragging clips from one to the other. A RISC OS desktop has a three-button mouse, a
keyboard, many windows and drag-and-drop between programs. Each feature below is
expressed in RISC OS terms (§3), not copied from a touch screen.

| Flexcil feature | Keep? | RISC OS form |
|---|---|---|
| **Document + notebook side by side, drag clips across** (the headline feature) | **Yes, core** | Two windows. Drag a selection (text or area) from the document into a notebook. The clip keeps a **link back** to its source page: click it to open the document there. The same drag into Edit, Paint or the Filer saves text or a sprite (data transfer protocol). |
| Pen gestures (draw to highlight, scribble to erase, lasso to select) | Yes, adapted | A tool pane (select, highlighter, pen, eraser, lasso, note, text box, shapes). **Select** uses the current tool; **Adjust** adjusts, as everywhere on RISC OS: it extends or shrinks the text selection, adds to or removes from a lasso selection, and ends a multi-point shape (as in Draw). Every tool is also in the Menu-button menu. Graphics tablets act as a mouse (no pressure). |
| Vector ink and highlighter | Yes | Real PDF annotations (Ink, Highlight, Underline, StrikeOut, Squiggly). Files stay readable in any PDF reader. "Highlight" also works as a menu action on the current selection. |
| Highlights list / annotation summary | Yes | An "Annotations" window listing every note and highlight with its text. Click to go there; Save (F3) or drag its save icon to export as text. |
| Lasso: move, copy, resize, recolour marks | Yes | Lasso picks annotations. Drag to move; handles resize; colour and width are in the menu. |
| Sticky notes, text boxes, shapes | Yes | Text, FreeText, Square, Circle, Line and Arrow annotations. |
| Undo / redo | Yes | MuPDF's journal, one `pdf_begin_operation` per action. ^Z / ^Y (§3.8), with F8 / F9 also working. |
| Multi-page views, horizontal/vertical flipping | Yes | Continuous scroll (default), single page, two-up, page grid. Zoom: fit width, fit page, %. |
| Pop-up page | Yes | **New view**, as in Draw: a second window onto the same document. |
| Tabs | Adapted | Separate windows (the RISC OS way), plus "Recent" on the icon bar menu. |
| Page thumbnails: reorder, insert, delete, merge, extract | Yes | Page grid window. Drag to reorder; Delete key; drop a PDF to insert its pages; drag pages out to the Filer to save a new PDF. |
| Outline and bookmark editing | Yes | Contents window, editable for PDFs. |
| Notebooks with paper templates | Yes | A notebook **is a PDF**: new pages from built-in templates (lined, grid, dotted). |
| Masking pen | Yes, later | Opaque mask annotation: click to reveal; "Show all / Hide all". |
| Reference links between documents | Yes, later | Link annotation to another file and page. |
| Search | Yes | F4 opens Find (as in Edit); results listed by page. |
| **Dictionary** | **Yes, phase 1** | §5: Wiktionary / Free Dictionary API through URL_Fetcher, shown in a pop-up. |
| Reading EPUBs comfortably | Yes | Font size, margins, line spacing; themes: day, sepia, **dark** (§4). The position survives re-layout (`fz_make_bookmark`). |
| Audio recording; cloud sync; planner store; handwriting to text | No | Out of scope. Notebooks are plain PDFs, so any sync tool works. |

### Annotations on EPUB and other re-flowed books
MuPDF saves annotations only into PDFs. For an EPUB, FB2 or MOBI, highlights and notes go into
a **side file** in the reader's Choices, keyed by the book's identifier. Each one records:
- the chapter;
- the text offset of its start and end within the chapter;
- a short quote of the text, to find it again if the book changes.

They're redrawn after each re-layout.

## 3. RISC OS interaction rules (Style Guide, checked 2026-10-10)

Checked against the **RISC OS Style Guide, Issue 3 (ROOL, 2015)**, `PRMs/Style.pdf`, and the
**User Interface Toolbox** manual, `PRMs/ToolBox.pdf`. Page numbers are the Guide's own:
"SG p28" is Style Guide page 28.

### 3.1 Toolbox, not hand-built Wimp (recommendation)
The Toolbox's standard objects already behave as the Style Guide requires, and its modules
are in the RISC OS 5 ROM. The reader can use:

| Toolbox object | Used for |
|---|---|
| **Iconbar** | The icon bar icon. |
| **Menu** | All menus: ticks, greying, shortcuts, and a help message per entry. |
| **ProgInfo** | The Info box: version, licence type field and web-site button (ToolBox.pdf ch. 12; SG p20 asks for the licence). |
| **FileInfo** | File ▸ Info. |
| **SaveAs** | Save and Export: draggable icon, Selection option, the whole data transfer protocol including RAM transfer. |
| **DCS** | Discard / Cancel / Save. |
| **Quit** | Quitting, and Message_PreQuit with unsaved work. |
| **PrintDbox** | Print. |
| **Scale** | View scale. |
| **ColourDbox** | Pen and highlighter colours. |
| **Window with toolbars** | The tool pane and status bar. |

Resource files are documented (ToolBox.pdf p511), so they can be generated on the host from a
text description and still opened in ResEd. The crossdev toolchain has **no OSLib or
toolboxlib**, so the choice is between:
- a thin `_swix` layer of our own (a few hundred lines);
- asking riscos-crossdev to add OSLib (a handoff).

The page area itself is a Toolbox window that the app redraws (tiles, §7).

### 3.2 Mouse (SG p13–15)
- **Select** makes a selection, uses tools and drags objects.
- **Menu** opens the window's menu tree.
- **Adjust**:
  - alters a selection;
  - reverses scroll arrows, adjuster arrows and page-turn buttons;
  - chooses a menu item without closing the menu;
  - on a document's Close icon, closes the window and opens its directory (SG p29).
- Select must do all the main work; Adjust is a shortcut, never required.
- **Text** (SG p77):
  - Select drag selects;
  - Adjust click or drag extends or shrinks the selection, either way;
  - double-click selects a word, triple-click a line or paragraph;
  - the selection is shown reversed out of its background (SG p85), in dark mode too;
  - when focus moves to another window, the selection is redrawn shaded (SG p80).
- **Annotations as objects** (SG p78–79):
  - Select click selects one; Adjust toggles;
  - a Select drag on empty page area box-selects (with Shift, only objects wholly inside);
  - double-click cycles through stacked objects;
  - selected objects get a red bounding box.
- **Drag and drop** (SG p33–34, p81–82):
  - a Select drag on an existing selection drags it;
  - **between windows the default is copy** (which suits clips into a notebook);
  - within a window the default is move, and Shift swaps copy and move;
  - Escape cancels the drag;
  - the pointer shows the Drop shape, with a ghost caret or bounding box at the target;
  - windows auto-scroll near their edges;
  - a drop on a Filer window creates a file named as in the Save box.
- **Pointers:** only the established shapes: write, hand, auto-scroll, menu, drop
  (SG p32–33). Changed only over our own work area, and reset on mode change (PRM p1400).

### 3.3 Starting, documents and windows (SG p19–30, p105)
- **Starting:** double-clicking the app (Select or Adjust) puts its icon on the icon bar and
  opens nothing.
- **Icon bar icon:** Select creates a **new notebook** (SG p21: an editor's icon creates a
  document).
- **Icon bar menu:** **Info**, **Help…**, Choices…, Recent ⇒, **Quit** (last) (SG p46).
  - The Info box shows the version, a hyphenated release date and the licence (AGPL).
- **Opening documents:** by double-click, by dragging to the icon bar, or by dragging from
  another app's Save box. A document that is already open is **brought to the front**, not
  opened twice; extra views come from **New view** (SG p22).
- **Inserting:** dragging a PDF into a notebook or PDF window inserts its pages. A type that
  can't be inserted gets an error box (SG p23).
- **Filetypes:** claim only types the reader is the main editor for (SG p23). Register the
  app name and filetypes with ROOL (SG p94).
- **Window titles** (SG p28):
  - the file's **leafname** (the full path is in File ▸ Info);
  - " *" while modified, removed only when the whole document is saved;
  - the view number and scale may be added;
  - an unsaved notebook is titled e.g. "Notebook".
- **Window placement** (SG p105):
  - the first window is centred, no bigger than a quarter of the screen;
  - each later one 48 OS units lower;
  - size and position may be remembered as a choice, or saved with the document's state
    (SG p24).
- **Closing** (SG p20, p63):
  - an unmodified window closes at once, as does a view while another view of the same
    document stays open;
  - otherwise the DCS box appears, with **Save as the default**;
  - closed documents never linger in memory (SG p24).
- **Quitting:** close everything; warn with the Quit box if anything is unsaved, both from
  the menu and on Message_PreQuit (SG p21).
- **Saving** (SG p23–24, p58–59):
  - the SaveAs box shows the full path once saved;
  - the name field takes up to 255 characters, validation `A~ `;
  - Adjust on Save keeps the box open;
  - an unmodified document keeps its date stamp;
  - errors appear in an error box;
  - window state (scale, page, dark mode) is saved with the document where the format
    allows (PDF: in the reader's own side data, never altering the content).
- **Full screen reading** is allowed only as an option, with the icon bar icon still
  present. F12 or Escape return to the desktop, and the menu entry is "Return to desktop"
  (SG p34–35).

### 3.4 Menus (SG p37–47, p106)
- **One menu tree per window type**, context-sensitive. Unavailable items are greyed, not
  hidden; items that lead to submenus stay black.
- An ellipsis (…) marks an item that opens a **persistent** dialogue; an arrow marks a
  submenu or transient dialogue. Never both.
- Toggles are **ticked items** (e.g. "Dark page", "Tool pane", "Two pages").
- Text: initial capital only ("Dark page", not "Dark Page"); shortcuts right-aligned with
  ^ for Ctrl and ⇑ for Shift.
- Placement: menus open 64 OS units left of the pointer; icon bar menus have their base
  96 OS units up.
- The document window's menu tree:

  | Menu | Items |
  |---|---|
  | **File ⇒** | Info ⇒ · Save ⇒ (F3; Selection option) · Export ⇒ (Text, Sprite, PDF pages) · Print… (Print) |
  | **Edit ⇒** | **Undo (^Z) · Redo (^Y) first** (SG p40; §3.8) · Cut (^X) · Copy (^C) · Paste (^V) · Delete (^K) · Select all (^A) · Clear selection · Highlight · Underline · Strike out |
  | **View ⇒** | Scale view… (with drag-a-box zoom, SG p60) · Fit width · Fit page · Two pages ✓ · **Dark page ✓** · Sepia page ✓ · Text ⇒ (size, margins, spacing for re-flowed books) · Tool pane ✓ · New view |
  | **Navigate ⇒** | Go to page… (F5) · Contents · Thumbnails · Back · Forward |
  | **Find…** | F4; case-insensitive by default (SG p60) |
  | **Define "word"** | Context item, present when the selection is one word (§5) |
  | **Annotations…** | The list window |

### 3.5 Dialogue boxes and the tool pane (SG p49–68, p107–109)
- Prefer **small transient** dialogues opened from menus. Use persistent ones only when they
  need menus, panes or drops.
- Persistent dialogues have **no Close icon**, only Cancel.
- Dialogues use delayed action: nothing happens until OK, Save or another action button.
- **Return** performs the default action; the default button is bottom right, the only one
  with the thick border. **Escape** equals Cancel. Adjust on an action button acts and keeps
  the box open.
- Dialogues take the input focus when opened. Arrow keys and Tab move between fields.
- Labels are imperative verbs (Save, Print), never Yes/No. Sizes follow SG p108, with at most
  800×600 OS units.
- The **tool pane** (pen, highlighter etc.) is a Toolbox toolbar attached to the window.
  - It has 3D buttons and acts instantly (SG p67–68).
  - "Tool pane" is a ticked View item, and the pane disappears with its window.
  - Pen colours come from the standard **ColourDbox** (percentages, SG p61).

### 3.6 Keyboard (SG p69–75)
- Unknown keys go to `Wimp_ProcessKey`.
- No **Alt** shortcuts, and the extra Windows/menu keys are left alone.
- A window gains the caret only on a click, never on pointer movement, and doesn't jump to
  the front when it does.
- **Keys:**

  | Key | Action |
  |---|---|
  | F1 | Help |
  | F2 | Load named document |
  | ⇑F2 | Insert named document |
  | ^F2 | Close window |
  | F3 | Save |
  | F4 | Find |
  | F5 | Go to page |
  | ^Z (also F8) | Undo (see §3.8) |
  | ^Y (also F9) | Redo (see §3.8) |
  | ^C | Copy |
  | ^A | Select all |
  | Escape | Clear selection, when no drag or dialogue is active (see §3.8) |
  | Print Scrn | Print |
  | Esc | Cancel |
  | Page Up / Page Down, ⇑↑ / ⇑↓ | Scroll by a screen |
  | Home / End, ^↑ / ^↓ | Start / end of document |
  | ^X / ^V | Cut / paste annotations (notebooks and PDFs) |
  | Delete / Backspace | Cut the selected annotation, as ^X (SG p73–74) |
  | ^K | Delete the selection without touching the clipboard |
- The F12 family is the OS's. ^D, ^T, ^U, ^B, ^I keep their standard meanings if ever
  used (e.g. in notebook text boxes).
- **The dark page shortcut is ^N** (Night; owner, 2026-10-10). It avoids every key above.

### 3.7 Colour, choices, help and speed
- **Colour** (SG p83–85, p105–106):
  - window frames, menus and dialogues use only the standard Wimp colours;
  - colour appears only in the work area (the pages);
  - colour never carries meaning alone: the Annotations list names each type;
  - the design is checked in greyscale;
  - it must work in every mode and palette (ColourTrans for the tiles).
- **Choices** (SG p91–93):
  - opened by Choices… on the icon bar menu, with **Default / Save / Cancel / Set**;
  - read from `Choices:Folio`, written to `<Choices$Write>.Folio`; never read through
    Choices$Write;
  - stored as readable text, with built-in defaults when absent;
  - Configure settings are never changed.
- **Help:** interactive help for every menu entry and gadget (SG p107), plus a !Help file run
  from the Help… item.
- **Speed** (SG p103):
  - redraws are fast, with source-level clipping;
  - the window's image is kept as sprites (the tile cache does exactly this);
  - drags and auto-scroll are timed, not tied to CPU speed;
  - everything is measured in OS units.
- **App directory** (SG p115–122): !Boot, !Run, !Sprites (large and small icons, plus a file
  icon matching the app), Messages, Res, !Help.

### 3.8 Addendum: deliberate departures from the Style Guide (owner, 2026-10-10)

The owner's decision is to follow the general convention of other operating systems for undo
and redo. This is a knowing exception to the Style Guide, recorded here so it isn't
"corrected" later.

| Key | Style Guide | This app |
|---|---|---|
| **^Z** | Clear selection (SG p73, "must use … if you provide a shortcut") | **Undo** |
| **^Y** | Not assigned | **Redo** |
| **F8 / F9** | Undo / Redo (SG p72) | **Still Undo / Redo**: kept, so RISC OS habits keep working and the SG function keys stay honoured |
| Clear selection | ^Z | **Escape** when no drag, menu or dialogue is active, plus the Edit menu item (no Ctrl shortcut) |

- **Menus show ^Z and ^Y** next to Undo and Redo, since those are the primary shortcuts.
  Interactive help for both items mentions F8 / F9 too.
- **The same keys apply in notebook text boxes and annotation contents.** In a writable field
  the Wimp's own editing is unaffected; ^Z / ^Y act on the document's journal.
- **Escape keeps its Style Guide meanings first:** it cancels a drag (SG p34), closes a menu
  and equals Cancel in a dialogue (SG p53). Only when none of those apply does it clear the
  selection.
- **No option to switch back.** ^Z is always Undo; there is no Choices setting for the
  Style Guide's ^Z (owner, 2026-10-10).
- **Why:** most users also work on Windows, macOS or Linux daily, where ^Z / ^Y are reflexes.
  An accidental ^Z clearing a selection, instead of undoing a highlight or pen stroke,
  would be a frequent irritation. (The Style Guide itself anticipates shortcuts more
  compatible with other systems, though it reserves Alt for that, SG p71; Alt stays unused
  here.)
- **Nothing else departs from the Style Guide.** Any future exception is added to this
  section with the owner's reason.

## 4. Dark mode: black page, white text (phase 1)

A per-window **View ▸ Dark page** option (ticked menu entry, shortcut ^N), plus a
default in Choices. It is **off by default**: the Style Guide notes that reversed-out text
reads well but "is not a good match for the style of the RISC OS desktop" (SG p85). It stays
an opt-in reading aid, applied only to the work area. Text selection in dark mode is still
shown reversed out (light background, dark text), as SG p85 requires. It changes only what is drawn on screen: never the file, the
saved annotations or printing. Thumbnails and the page grid follow it. The gutter around
pages turns dark grey; the window frames and tool panes stay in the desktop's own colours.

Both methods below are tested in `muprobe` (`spike/dark-pdf.png`, `spike/dark-epub.png`):

- **Re-flowed books (EPUB, FB2, MOBI, HTML):**
  - user CSS `* { color: #e8e8e8 !important; background-color: transparent !important }`,
    with links in light blue;
  - the page is drawn onto a black pixmap;
  - pictures keep their own colours;
  - the book's own heading colours are overridden (checked).
- **Fixed-layout pages (PDF, XPS):**
  1. Draw the page content.
  2. Apply `fz_invert_pixmap_luminance`: white ↔ black, hues kept, so blue text becomes
     light blue.
  3. Put pictures back from a normal render: their areas come from structured text with
     `FZ_STEXT_PRESERVE_IMAGES`, so photos are not negatives. Pages without pictures skip
     the second render.
  4. Draw annotations last, not inverted.

  Comics (CBZ) are all picture, so they stay as they are.
- **Seen in the test:**
  - inverted white is (4,4,4), not (0,0,0), so a final levels step will make it pure
    black;
  - a yellow highlight shows as **yellow text on black**: readable, but subtler than on
    white. Check on the Pi whether a translucent band reads better.
- **Sepia** uses `fz_tint_pixmap` in the same place.
- The tile cache keys include the mode, so switching modes redraws from the display lists
  without re-reading the file.

## 5. Dictionary (phase 1)

- **How the user looks a word up:** double-click a word (the Style Guide's word selection,
  SG p77), then choose the context item **Define "word"** from the window menu. It is greyed
  unless exactly one word is selected. The text under the pointer comes from MuPDF's structured text (PDF and EPUB
  alike).
- **Sources** (both checked working on 2026-10-10, no key needed):
  1. **Wiktionary REST:** `https://en.wiktionary.org/api/rest_v1/page/definition/<word>`.
     - English definitions of words in **any language**, grouped by language code.
     - Definitions are small HTML fragments.
     - The book's language picks the section (EPUB `dc:language`, PDF `/Lang`, or a menu
       override).
  2. **Free Dictionary API:** `https://api.dictionaryapi.dev/api/v2/entries/en/<word>`.
     - English only; simple JSON (phonetics, part of speech, definitions, synonyms).
     - Used as the fallback, since Wikimedia is retiring parts of its older REST API.
  - Both are Wiktionary data under **CC BY-SA**, so the pop-up shows "From Wiktionary
    (CC BY-SA)" with a link.
  - Requests send the User-Agent `Folio/<version> (RISC OS; https://github.com/adyoull/riscos-folio)`,
    as Wikimedia asks; the contact is the project page, not an email address (owner, 2026-10-10).
- **Fetching without threads:** the **URL_Fetcher** module (AcornHTTP + AcornSSL for HTTPS):
  1. `URL_Register`;
  2. `URL_GetURL`, with R6 set to the User-Agent;
  3. `URL_Status` / `URL_ReadData` in null polls;
  4. `URL_Deregister`.

  These are in the PRM, p684–697. The desktop never waits. With no network, or with the
  modules missing, the pop-up says so: it is never an error box.
- **Display:** a small **transient** dialogue near the word (SG p50: removed by a click
  elsewhere), with a Copy action button and the attribution line. The HTML definitions are laid out by **MuPDF's own HTML engine** (no second renderer)
  and follow dark mode. JSON is parsed with mujs (already linked) or cJSON (MIT).
- **Cache:** recent lookups are kept in Choices (a small file), so repeats are instant and
  work offline.
- **Later:** WordNet (English, licence allows bundling, about 30 MB) as an offline pack.

## 6. Fonts: lean program, other fonts through fontconfig (phase 1; owner, 2026-10-10)

- **The program stays lean:** MuPDF is built with `tofu_cjk=yes` (and later without the
  Noto script fonts too). It keeps the URW base 14 and the Latin, Greek and Cyrillic
  fonts it always needs.
- **Every other font comes through fontconfig**, via MuPDF's
  `fz_install_load_system_font_funcs` hooks:
  - **CJK** (`ordering` → `lang` ja / zh-cn / zh-tw / ko, serif or sans) → `FcFontMatch`
    → file and face index → `fz_new_font_from_file`;
  - **fallback** (script and language from MuPDF) → the matching `lang`, or a charset with
    the needed character;
  - **by name**, for fonts a PDF names without embedding them (beyond the base 14).
- **Which fontconfig:** **2.14.1**, the version PackMan ships, so it reads UnixFC's
  `fonts.conf` and shares its cache (cache-8), as the Warzone 2100 port does
  (`Warzone2100/riscos-warzone2100/build/build-deps.sh`; sources in
  `Warzone2100/dl-cache`). It needs expat 2.2.9.
  - The GCCSDK `fcint.p` change (`;` as the search path separator) and the **read()
    stack-page patch** (`patches/fontconfig/fontconfig-2.14.1-riscos.diff`): memset the
    buffer before `read()` in `fcxml.c` and `fccache.c`.
- **Setup, process-local only** (the Warzone lesson: a global variable without `$` leaks
  into every UnixLib program):
  - with **UnixFC** installed (`UnixFC$Dir` set and `fonts/fonts.conf` readable):
    `FONTCONFIG_FILE` and `FONTCONFIG_PATH` point at UnixFC's, so the reader uses the
    system's fonts, mapping and cache like any other fontconfig program;
  - without it: Folio's own `fonts.conf`, covering `<Folio$Dir>.Fonts` and
    `UnixFont:truetype` if present, with its cache in `<Choices$Write>.Folio.FontCache`;
  - set with `setenv()` in a priority constructor, before anything starts fontconfig;
    `FONTCONFIG_SYSROOT` unset.
- **One FreeType:** fontconfig is built against **MuPDF's bundled FreeType** (headers and
  objects in `libmupdf-third`), so the program never has two FreeTypes. fontconfig needs a
  few FreeType parts MuPDF leaves out (e.g. `ftbdf.c`, `ftfntfmt.c`, `ftmm.c`,
  `ftsnames.c`); those are compiled from MuPDF's FreeType source with its flags. The link
  shows exactly which.
- **The fonts themselves:** CJK and other script fonts (Noto Sans/Serif CJK, Noto for other
  scripts; SIL OFL) go into **`UnixFont:truetype`**, where UnixFC's fontconfig finds them
  for every program. A PackMan package for them is still to decide.
- **Without any such font:** PDFs with embedded fonts look normal. Only text in a font
  that isn't installed shows empty boxes, and a one-time note says which fonts to install.
- **First run:** fontconfig builds its cache for new fonts, which is slow on SD cards. The
  hourglass shows during start-up if that happens, and the shared UnixFC cache usually
  avoids it.

## 7. Architecture

```
!Folio
  core/     MuPDF wrapper: open, layout, page cache, render (incl. dark/sepia), text,
            annots, journal, fonts loader (no Wimp)
  wimp/     windows, menus, drags, clipboard, messages, choices, help
  render/   tile renderer → sprite cache → plot
  net/      URL_Fetcher client, dictionary sources, cache
  notes/    side files for re-flowed books
  tests/    host tests of core/ under qemu-arm-aligntrap (same code, Linux ARM)
```

- **Toolbox application in C** (§3.1): Res file plus a thin `_swix` layer (or OSLib), with
  Wimp_Poll via Toolbox events.
- **Rendering:**
  - Each page becomes a display list once.
  - The visible area is drawn in tiles (e.g. 256×256) into 32bpp sprites, kept in a
    dynamic area.
  - Wimp redraws plot the cached tiles with `OS_SpriteOp 52`, using ColourTrans tables in
    modes below 32bpp.
  - Missing tiles get a placeholder in the page colour (white, or black in dark mode) and
    are drawn in null polls.
  - No threads; a tile can be stopped with `fz_cookie`.
- **Memory:**
  - MuPDF's store limit is set from free memory.
  - The tile cache has its own LRU limit.
  - UnixLib's heap is in its dynamic area.
- **Annotations:** MuPDF owns the truth. Each change runs inside a journal operation, then
  that page's display list and tiles are thrown away.
- **Saving:**
  - incremental by default;
  - Save as does a full rewrite;
  - `pdf_check_document` runs when a PDF is opened for editing.
- **No fork, popen or system.** File types are set with `OS_File 18`.

## 8. Phases

0. **Spikes: done.** Engine, checks, the sprite path, dark mode, and the Pi test zip.
1. **Reader.**
   - Open PDF, EPUB, CBZ, XPS, FB2 and MOBI by every route in §3.
   - Continuous, single and two-up views; zoom; fit width/page.
   - Contents and thumbnails; links.
   - Find (F4).
   - Text selection and copy via the clipboard and drag-and-drop (§3.2).
   - EPUB font size, margins and spacing.
   - **Dark and sepia modes** (§4).
   - **Dictionary pop-up** (§5).
   - **Lean build, other fonts through fontconfig** (§6).
   - Remember the position per book.
   - Toolbox objects (§3.1), interactive help, Messages, Choices (§3.7).
   - Tests: core/ under qemu with the alignment trap; the URL_Fetcher client against a
     host stub; a Pi checklist.
2. **Markup (PDF).**
   - Tool pane: highlighter, underline/strike, pen, eraser, lasso, note, text box, shapes.
   - Annotations window.
   - Undo/redo (^Z/^Y, plus F8/F9; §3.8).
   - SaveAs box (with Selection), incremental save, Export.
3. **Study.**
   - Notebooks (PDF with paper templates).
   - Clip dragging with back-links.
   - New view.
   - Page grid: reorder, insert, delete, merge, extract.
   - Masking.
4. **Polish.**
   - Side-file annotations for EPUB.
   - Export highlights.
   - Links between documents.
   - Printing.
   - Offline WordNet pack.
   - Speed work from the Pi numbers.

## 9. Risks and open questions

- **Speed on a Pi (measured 2026-10-10):**
  - a 422-page novel lays out in **1.3 s** (peak 24 MB), and pages draw in **40–50 ms**;
  - so: (1) save MuPDF's EPUB **accelerator** per book and layout (in
    `<Choices$Write>.Folio.Layouts`, read through `Choices:`), which makes reopening
    instant; (2) on first open, or after changing font size or margins, count pages
    **chapter by chapter in null polls** (`fz_count_chapter_pages`), showing the current
    chapter at once and "page N of ~M" until counting finishes;
  - heavy PDF pages are still to be measured. Dark mode renders pages with pictures twice;
    measure the cost.
- **Licence (decided 2026-10-10):**
  - **the program is AGPL-3.0-or-later.** MuPDF is AGPL, so a program that includes it must
    be too, unless Artifex's commercial licence is bought;
  - **our build scripts, Makefiles, tests, tools and docs are MIT** (owner), as in
    riscos-crossdev;
  - third-party code keeps its own licence: MuPDF AGPL; fontconfig and expat MIT-style;
    FreeType FTL;
  - dictionary data is CC BY-SA, which only needs attribution in the pop-up.
- **Wiktionary's REST endpoint** may be retired; the fallback source and the cache soften
  that.
- **Owner decisions:**
  - the name;
  - Toolbox (recommended, §3.1), and whether to ask riscos-crossdev for OSLib or write a thin
    `_swix` layer;
  - whether EPUB annotations matter early;
  - filetypes for EPUB and notebooks;
  - registering the app name and filetypes with ROOL (SG p94).
