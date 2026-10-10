#!/usr/bin/env python3
"""Writes tests/data/book.epub: an EPUB 3 test book with known content.
   - language en-GB, title "Folio Test Book", author "A. Tester"
   - 4 chapters; chapter 1 links to an anchor in chapter 3; chapter 2 has a
     red PNG picture; chapter 3 is long and contains the word "zephyr" in
     paragraphs 5 and 40; nested contents (chapter 3 has a sub-entry).
   python3 -I make-epub.py OUT.epub"""
import sys, zipfile, zlib, struct

def png(w, h, rgb):
    raw = b''.join(b'\0' + bytes(rgb) * w for _ in range(h))
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def page(title, body):
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en-GB" lang="en-GB">'
            f'<head><title>{title}</title><link rel="stylesheet" href="s.css"/></head>'
            f'<body><h1>{title}</h1>{body}</body></html>')

filler = ("The quick brown fox jumps over the lazy dog while the reader turns the page. ")
c1 = '<p>Opening words. <a href="c3.xhtml#target">Go to the target</a>.</p>' + \
     ''.join(f'<p>One {i}. {filler}</p>' for i in range(8))
c2 = '<p>A picture follows.</p><p><img src="red.png" alt="red" style="width:200px;height:120px"/></p>' + \
     ''.join(f'<p>Two {i}. {filler}</p>' for i in range(4))
c3 = ''.join((f'<p id="target">Target paragraph {i}. {filler}</p>' if i == 20 else
              f'<p>Three {i}. {"A zephyr blew. " if i in (5, 40) else ""}{filler * 2}</p>')
             for i in range(60))
c4 = '<p>The end.</p>'

z = zipfile.ZipFile(sys.argv[1], 'w')
z.writestr(zipfile.ZipInfo('mimetype'), 'application/epub+zip')
z.writestr('META-INF/container.xml', '<?xml version="1.0"?><container version="1.0" '
           'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>'
           '<rootfile full-path="OEBPS/c.opf" media-type="application/oebps-package+xml"/>'
           '</rootfiles></container>')
z.writestr('OEBPS/c.opf', '<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" '
           'version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
           '<dc:identifier id="id">folio-test-1</dc:identifier><dc:title>Folio Test Book</dc:title>'
           '<dc:creator>A. Tester</dc:creator><dc:language>en-GB</dc:language></metadata><manifest>'
           '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
           '<item id="css" href="s.css" media-type="text/css"/>'
           '<item id="red" href="red.png" media-type="image/png"/>'
           + ''.join(f'<item id="c{n}" href="c{n}.xhtml" media-type="application/xhtml+xml"/>' for n in range(1, 5)) +
           '</manifest><spine>' + ''.join(f'<itemref idref="c{n}"/>' for n in range(1, 5)) +
           '</spine></package>')
z.writestr('OEBPS/nav.xhtml', '<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml" '
           'xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol>'
           '<li><a href="c1.xhtml">Chapter One</a></li><li><a href="c2.xhtml">Chapter Two</a></li>'
           '<li><a href="c3.xhtml">Chapter Three</a><ol><li><a href="c3.xhtml#target">The Target</a></li></ol></li>'
           '<li><a href="c4.xhtml">Chapter Four</a></li></ol></nav></body></html>')
z.writestr('OEBPS/s.css', 'body { font-family: serif; text-align: justify } h1 { color: #036 }')
z.writestr('OEBPS/red.png', png(20, 12, (220, 30, 30)))
for n, (t, b) in enumerate([('Chapter One', c1), ('Chapter Two', c2), ('Chapter Three', c3), ('Chapter Four', c4)], 1):
    z.writestr(f'OEBPS/c{n}.xhtml', page(t, b))
z.close()
