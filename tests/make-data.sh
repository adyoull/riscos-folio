#!/bin/bash
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
var h = p.createAnnotation("Highlight");
h.setColor([1, 1, 0]);
h.setQuadPoints(p.toStructuredText().search("serendipity")[0]);
h.update();
doc.save(scriptArgs[1]);
JS
"$AT" "$MUTOOL" run fix.js plain.pdf doc.pdf
rm -f p1.txt p2.txt p3.txt fix.js
ls -l
