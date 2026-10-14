#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Makes the test documents in tests/data with the ARM Linux mutool.
#   tests/make-data.sh MUTOOL ALIGNTRAP
set -eu
MUTOOL=$1; AT=$2
D=$(cd "$(dirname "$0")" && pwd)/data
mkdir -p "$D"; cd "$D"
python3 -I ../make-epub.py book.epub
python3 -I - <<'PY'
import zlib, struct
def png(w, h):
    raw = b''.join(b'\0' + b''.join(bytes((x * 255 // w, y * 255 // h, 160)) for x in range(w)) for y in range(h))
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
open('photo.png', 'wb').write(png(64, 40))
PY
# Page 1: title, the word "serendipity" at x 72 baseline y 700 (page y 142),
# a photo at 72,400-640,560 (page y 282-442). Pages 2 and 3: text.
cat > p1.txt <<'TXT'
%%MediaBox 0 0 595 842
%%Font Helv Helvetica
%%Image Im0 photo.png
BT /Helv 18 Tf 72 760 Td (Folio test page one) Tj ET
BT /Helv 12 Tf 72 700 Td (serendipity is the word under test) Tj ET
q 240 0 0 160 72 400 cm /Im0 Do Q
TXT
for n in 2 3; do printf '%%%%MediaBox 0 0 595 842\n%%%%Font Helv Helvetica\nBT /Helv 18 Tf 72 760 Td (Folio test page %s) Tj ET\n' $n > p$n.txt; done
"$AT" "$MUTOOL" create -o plain.pdf p1.txt p2.txt p3.txt
cat > fix.js <<'JS'
var doc = Document.openDocument(scriptArgs[0]);
var root = doc.getTrailer().get("Root");
root.put("Lang", doc.newString("en-GB"));
var info = doc.getTrailer().get("Info");
if (!info || !info.isDictionary()) { info = doc.addObject(doc.newDictionary()); doc.getTrailer().put("Info", info); }
info.put("Title", doc.newString("Folio Test PDF"));
info.put("Author", doc.newString("A. Tester"));
var p = doc.loadPage(0);
p.createLink([72, 60, 300, 82], "#page=3");
p.createLink([72, 90, 300, 110], "#page=2&zoom=nan,72,300");
p.createLink([72, 120, 300, 140], "https://example.org/");
var h = p.createAnnotation("Highlight");
h.setColor([1, 1, 0]);
h.setQuadPoints(p.toStructuredText().search("serendipity")[0]);
h.update();
doc.save(scriptArgs[1]);
JS
"$AT" "$MUTOOL" run fix.js plain.pdf doc.pdf
# For reading aloud: a heading, then a paragraph with a word hyphenated at
# a line end ("exam-" / "ple"). Page 2: one more sentence.
cat > s1.txt <<'TXT'
%%MediaBox 0 0 595 842
%%Font Helv Helvetica
BT /Helv 18 Tf 72 760 Td (Chapter One) Tj ET
BT /Helv 12 Tf 72 700 Td (The quick brown fox jumps over the lazy dog. It is an exam-) Tj ET
BT /Helv 12 Tf 72 686 Td (ple of a word split at the end of a line.) Tj ET
TXT
cat > s2.txt <<'TXT'
%%MediaBox 0 0 595 842
%%Font Helv Helvetica
BT /Helv 12 Tf 72 760 Td (The second page has one more sentence.) Tj ET
TXT
"$AT" "$MUTOOL" create -o speech.pdf s1.txt s2.txt
# For OCR: speech.pdf's page 1 as a 300 dpi grey picture only (a scan),
# then its page 2 with real text.
"$AT" "$MUTOOL" draw -q -r 300 -c gray -o scanp1.png speech.pdf 1
"$AT" "$MUTOOL" convert -O compress -o scanp1.pdf scanp1.png
"$AT" "$MUTOOL" merge -O compress -o scan.pdf scanp1.pdf 1 speech.pdf 2
rm -f scanp1.png scanp1.pdf
# Two columns, scanned: for the OCR reading order.
{
	printf '%%%%MediaBox 0 0 595 842\n%%%%Font Helv Helvetica\n'
	n=0
	for w in one two three four five six seven eight; do
		y=$((760 - n * 24)); n=$((n + 1))
		printf 'BT /Helv 14 Tf 50 %d Td (Left column line %s.) Tj ET\n' $y $w
		printf 'BT /Helv 14 Tf 330 %d Td (Right column line %s.) Tj ET\n' $y $w
	done
} > cols.txt
"$AT" "$MUTOOL" create -o colsplain.pdf cols.txt
"$AT" "$MUTOOL" draw -q -r 300 -c gray -o colsp1.png colsplain.pdf 1
"$AT" "$MUTOOL" convert -O compress -o columns.pdf colsp1.png
rm -f cols.txt colsplain.pdf colsp1.png
# Pages of different sizes: A5, A4, A4 landscape.
for sz in "420 595" "595 842" "842 595"; do
	printf '%%%%MediaBox 0 0 %s\n%%%%Font Helv Helvetica\nBT /Helv 24 Tf 40 40 Td (Page of %s) Tj ET\n' "$sz" "$sz" > "m${sz%% *}.txt"
done
"$AT" "$MUTOOL" create -o mixed.pdf m420.txt m595.txt m842.txt
# Restricted: no password to open, owner password "owner"; printing and
# copying not allowed, accessibility (reading aloud) and notes allowed.
"$AT" "$MUTOOL" clean -E aes-256 -O owner -U "" -P -2080 doc.pdf restricted.pdf
rm -f m420.txt m595.txt m842.txt
rm -f p1.txt p2.txt p3.txt s1.txt s2.txt fix.js
ls -l
