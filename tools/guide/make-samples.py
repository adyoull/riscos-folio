#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
"""Writes the sample documents the User Guide's screenshots are taken with
(all words and pictures made here, so the guide's pictures are free to use):
  fells.pdf       "Walking in the Fells": an 8-page A4 booklet with a
                  contents list, bookmarks, links inside it and to the web,
                  and pictures
  lighthouse.epub "The Lighthouse Keeper": a short ebook in 6 chapters
  hills-*.jpg     pictures, for Create PDF and stamps
  notes.txt       a text file, for Create PDF
  initials.pdf    initials and a date at the top right of an A4 page, for Overlay
python3 make-samples.py OUTDIR   (needs reportlab and Pillow)"""
import os, sys, zipfile, random, io
from PIL import Image, ImageDraw, ImageFilter

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def landscape(w, h, seed, sky=((70, 120, 190), (230, 210, 170))):
    random.seed(seed)
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / h
        d.line([(0, y), (w, y)], fill=tuple(int(sky[0][i] + (sky[1][i] - sky[0][i]) * t) for i in range(3)))
    d.ellipse([w * 0.7, h * 0.12, w * 0.7 + h * 0.12, h * 0.24], fill=(255, 240, 200))
    layers = [((95, 110, 140), 0.45), ((70, 95, 90), 0.55), ((50, 80, 60), 0.66), ((35, 60, 40), 0.78)]
    for colour, base in layers:
        pts, x, y = [(0, h)], 0, h * base
        while x <= w:
            y = max(h * (base - 0.2), min(h * (base + 0.08), y + random.uniform(-h * 0.05, h * 0.05)))
            pts.append((x, y))
            x += w / 30
        pts += [(w, h)]
        d.polygon(pts, fill=colour)
    d.rectangle([0, h * 0.86, w, h], fill=(60, 90, 120))
    for i in range(40):
        y = random.uniform(h * 0.87, h)
        x = random.uniform(0, w)
        d.line([(x, y), (x + random.uniform(10, 60), y)], fill=(150, 180, 200))
    return im.filter(ImageFilter.SMOOTH)

for i, seed in enumerate((3, 11, 27)):
    landscape(1600, 1100, seed).save(os.path.join(out, "hills-%d.jpg" % (i + 1)), quality=88)

# ---- fells.pdf -------------------------------------------------------------
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.pdfgen import canvas
from reportlab.lib.utils import ImageReader

W, H = A4
sections = [
    ("Before you set out", "before", [
        "A good day on the fells starts the night before. Look at the forecast, choose a walk that suits the "
        "slowest walker in the group, and tell someone where you are going and when you expect to be back.",
        "Pack a waterproof jacket even on a fine morning: the weather on the tops can change in an hour. "
        "Take more water than you think you need, something to eat, a map, a compass and a small torch.",
        "Good boots matter more than anything else you carry. Wear them in on short walks first, and keep "
        "a spare pair of socks in your bag."]),
    ("Three easy walks", "easy", [
        "Round the tarn: two miles on a level path, about an hour. The path keeps close to the water, with "
        "benches at the far end and a view of the high fells across the tarn.",
        "The old drove road: four miles, two hours, with one steady climb at the start. It follows a walled "
        "lane over the shoulder of the hill and comes down through the wood to the village.",
        "Along the river: three miles of riverside path, flat all the way. Good on a wet day, when the "
        "tops are in cloud and the waterfalls are at their best."]),
    ("A harder day", "harder", [
        "The ridge walk is eight miles with 900 metres of climbing, a full day for a fit walker. The ascent "
        "is steep and rough in places; the ridge itself is broad and easy to follow in clear weather.",
        "In mist the ridge is a different place. The path splits near the summit cairn, and the wrong branch "
        "leads to steep ground. Turn back if the cloud comes down before you reach the top.",
        "Allow seven hours, and start early: the last bus from the valley leaves at six."]),
    ("The weather", "weather", [
        "Mountain weather is colder and windier than the valley. As a rule it is one degree colder for every "
        "150 metres you climb, and the wind on a ridge can be twice as strong as at the car park.",
        "Read the mountain forecast, not the town forecast. It gives the cloud base, the wind on the summits "
        "and how it will feel, which is what decides whether a walk is wise."]),
    ("Maps and paths", "maps", [
        "Carry the map and know how to read it. A phone is useful, but batteries go flat in the cold, and "
        "there is often no signal in the valleys.",
        "Paths on the map are rights of way, not always paths on the ground; and paths on the ground are "
        "not always rights of way. Keep to the paths, close gates behind you and keep dogs on a lead near sheep."]),
    ("Staying safe", "safe", [
        "If someone is hurt, keep them warm, note where you are, and call 999 or 112 and ask for the police, "
        "then mountain rescue. Stay together and stay where you are unless it is unsafe.",
        "If you are lost, stop. Look at the map, think about where you last knew you were, and take a bearing "
        "back to it. Going down is not always the way out: it can lead to crags.",
        "Most days on the fells end with tired legs and a good tea. Plan well and yours will too."]),
]
c = canvas.Canvas(os.path.join(out, "fells.pdf"), pagesize=A4)
c.setTitle("Walking in the Fells")
c.setAuthor("Hill Walking Club")
c.setSubject("A short guide for new walkers")
c.setKeywords("walking, fells, safety")
c.setCreator("Folio guide samples")

def footer(n):
    c.setFont("Helvetica", 9)
    c.setFillColorRGB(0.4, 0.4, 0.4)
    c.drawCentredString(W / 2, 15 * mm, "Walking in the Fells  -  page %d" % n)
    c.setFillColorRGB(0, 0, 0)

def para(text, x, y, width, size=11, lead=15):
    c.setFont("Times-Roman", size)
    words, line = text.split(), ""
    for w in words:
        if c.stringWidth(line + " " + w, "Times-Roman", size) > width and line:
            c.drawString(x, y, line)
            y -= lead
            line = w
        else:
            line = (line + " " + w).strip()
    if line:
        c.drawString(x, y, line)
        y -= lead
    return y - 8

# Page 1: cover
c.drawImage(ImageReader(os.path.join(out, "hills-1.jpg")), 20 * mm, 120 * mm, W - 40 * mm, (W - 40 * mm) * 1100 / 1600)
c.setFont("Helvetica-Bold", 34)
c.drawString(20 * mm, 95 * mm, "Walking in the Fells")
c.setFont("Helvetica", 16)
c.drawString(20 * mm, 82 * mm, "A short guide for new walkers")
c.setFont("Helvetica", 11)
c.drawString(20 * mm, 40 * mm, "Hill Walking Club")
c.bookmarkPage("cover")
c.addOutlineEntry("Walking in the Fells", "cover", 0)
c.showPage()

# Page 2: contents
c.setFont("Helvetica-Bold", 22)
c.drawString(20 * mm, H - 30 * mm, "Contents")
y = H - 50 * mm
for i, (title, key, _) in enumerate(sections):
    c.setFont("Times-Roman", 13)
    c.drawString(25 * mm, y, title)
    c.drawRightString(W - 25 * mm, y, str(i + 3))
    c.linkAbsolute("", key, (25 * mm, y - 3, W - 25 * mm, y + 12))
    y -= 11 * mm
c.setFont("Times-Italic", 11)
para("Read it at home before you go: the pictures show the walks in clear weather, and the links in "
     "the contents take you straight to each part.", 25 * mm, y - 10 * mm, W - 50 * mm)
c.bookmarkPage("contents")
c.addOutlineEntry("Contents", "contents", 0)
footer(2)
c.showPage()

# Pages 3-8: sections
for i, (title, key, paras) in enumerate(sections):
    c.bookmarkPage(key)
    c.addOutlineEntry(title, key, 0)
    c.setFont("Helvetica-Bold", 20)
    c.drawString(20 * mm, H - 30 * mm, title)
    y = H - 45 * mm
    if i in (1, 2):
        img = "hills-%d.jpg" % (i + 1)
        ih = (W - 40 * mm) * 1100 / 1600 * 0.62
        c.drawImage(ImageReader(os.path.join(out, img)), 20 * mm, y - ih, W - 40 * mm, ih, preserveAspectRatio=True)
        y -= ih + 10 * mm
    for p in paras:
        y = para(p, 20 * mm, y, W - 40 * mm)
    if key == "weather":
        c.setFont("Times-Roman", 11)
        c.setFillColorRGB(0.1, 0.3, 0.7)
        t = "Mountain forecasts: www.mwis.org.uk"
        c.drawString(20 * mm, y, t)
        c.linkURL("https://www.mwis.org.uk/", (20 * mm, y - 3, 20 * mm + c.stringWidth(t, "Times-Roman", 11), y + 11))
        c.setFillColorRGB(0, 0, 0)
    if key == "safe":
        c.setFont("Times-Roman", 11)
        t = "Back to the contents"
        c.setFillColorRGB(0.1, 0.3, 0.7)
        c.drawString(20 * mm, y - 6, t)
        c.linkAbsolute("", "contents", (20 * mm, y - 9, 20 * mm + c.stringWidth(t, "Times-Roman", 11), y + 5))
        c.setFillColorRGB(0, 0, 0)
    footer(i + 3)
    c.showPage()
c.save()

# ---- initials.pdf ------------------------------------------------------------
c = canvas.Canvas(os.path.join(out, "initials.pdf"), pagesize=A4)
c.setFillColorRGB(0.1, 0.25, 0.65)
c.setStrokeColorRGB(0.1, 0.25, 0.65)
c.setFont("Helvetica-Bold", 13)
c.drawRightString(W - 18 * mm, H - 16 * mm, "Checked  AY  13 Nov 2026")
c.setLineWidth(1.5)
c.line(W - 95 * mm, H - 19 * mm, W - 18 * mm, H - 19 * mm)
c.save()

# ---- notes.txt -----------------------------------------------------------------
open(os.path.join(out, "notes.txt"), "wb").write(
    b"Notes from the walk, 8 November\n\n"
    b"Started from the car park at 9.15. Cloud on the tops until eleven.\n"
    b"\tRound the tarn:   1 h 05\n\tUp to the ridge:  2 h 20\n\tBack down:        1 h 40\n\n"
    b"\x94A grand day out\x95, as the man at the stile said \x8c\n")

# ---- lighthouse.epub -------------------------------------------------------------
chapters = [
    ("The Island", ["The island was a mile long and half a mile wide, and the lighthouse stood at its northern end, "
                    "white against the grey sea. Tom had kept the light for eleven years.",
                    "Every evening at dusk he climbed the hundred and twelve steps, trimmed the wick, wound the "
                    "clockwork and watched the beam begin its slow turn across the water."]),
    ("The Boat", ["The supply boat came on the first Monday of every month, weather allowing. In winter the weather "
                  "seldom allowed, and Tom learnt to make a sack of flour last.",
                  "The boatman, a quiet man called Hughes, brought letters, paraffin and news, in that order of "
                  "importance."]),
    ("The Storm", ["It began as a long swell from the west, the kind that comes before a gale. By noon the sky had "
                   "turned the colour of slate, and by evening the spray was breaking over the gallery.",
                   "Tom kept the light all night. At three in the morning he saw, between the waves, the lights of "
                   "a ship far closer to the rocks than any ship should be."]),
    ("The Ship", ["He fired the signal rocket, and then another. For a long minute nothing changed; then the ship's "
                  "lights swung slowly away to the north, into deeper water.",
                  "In the morning the sea was calm and empty, as if the night had never happened."]),
    ("The Letter", ["A month later Hughes brought a letter from the shipping company. The ship had been carrying "
                    "two hundred passengers; her captain wished to thank the keeper of the light.",
                    "Tom read it twice, folded it into his logbook, and went up to trim the wick."]),
    ("The Light", ["The light is automatic now. It turns all night with no one to watch it, and the island belongs "
                   "to the seabirds.",
                   "But in the logbook, still kept in the museum on the mainland, there is a letter folded between "
                   "two pages, and a single line in Tom's hand: Ship passed safely. Wind easing."]),
]
def xhtml(title, body):
    return ('<?xml version="1.0" encoding="utf-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en-GB" '
            'lang="en-GB"><head><title>%s</title><link rel="stylesheet" href="s.css"/></head><body>%s</body></html>'
            % (title, body))
z = zipfile.ZipFile(os.path.join(out, "lighthouse.epub"), "w")
z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
z.writestr("META-INF/container.xml", '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:'
           'opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/book.opf" media-type="application/'
           'oebps-package+xml"/></rootfiles></container>', zipfile.ZIP_DEFLATED)
cover = io.BytesIO(); landscape(600, 900, 41, sky=((40, 50, 90), (200, 150, 120))).save(cover, "JPEG", quality=85)
z.writestr("OEBPS/cover.jpg", cover.getvalue())
z.writestr("OEBPS/s.css", "body{font-family:serif} h1{font-family:sans-serif;font-size:1.6em} p{text-indent:1.2em;margin:0 0 .4em}")
items, spine, navli = [], [], []
z.writestr("OEBPS/title.xhtml", xhtml("The Lighthouse Keeper", '<p style="text-align:center"><img src="cover.jpg" '
           'alt="cover" style="width:80%"/></p><h1 style="text-align:center">The Lighthouse Keeper</h1>'
           '<p style="text-align:center;text-indent:0">A short story</p>'))
items.append('<item id="title" href="title.xhtml" media-type="application/xhtml+xml"/>')
spine.append('<itemref idref="title"/>')
for i, (t, ps) in enumerate(chapters):
    body = "<h1>%d. %s</h1>" % (i + 1, t) + "".join("<p>%s</p>" % p for p in ps * 3)
    z.writestr("OEBPS/c%d.xhtml" % i, xhtml(t, body))
    items.append('<item id="c%d" href="c%d.xhtml" media-type="application/xhtml+xml"/>' % (i, i))
    spine.append('<itemref idref="c%d"/>' % i)
    navli.append('<li><a href="c%d.xhtml">%d. %s</a></li>' % (i, i + 1, t))
z.writestr("OEBPS/nav.xhtml", '<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml" '
           'xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head><body><nav epub:type="toc">'
           '<ol>%s</ol></nav></body></html>' % "".join(navli))
z.writestr("OEBPS/book.opf", '<?xml version="1.0" encoding="utf-8"?><package xmlns="http://www.idpf.org/2007/opf" '
           'version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
           '<dc:identifier id="id">folio-guide-lighthouse</dc:identifier><dc:title>The Lighthouse Keeper</dc:title>'
           '<dc:creator>Folio guide samples</dc:creator><dc:language>en-GB</dc:language>'
           '<meta property="dcterms:modified">2026-11-13T00:00:00Z</meta><meta name="cover" content="cover"/></metadata><manifest>'
           '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
           '<item id="css" href="s.css" media-type="text/css"/>'
           '<item id="cover" href="cover.jpg" media-type="image/jpeg" properties="cover-image"/>%s</manifest>'
           '<spine>%s</spine></package>' % ("".join(items), "".join(spine)))
z.close()
print("samples in", out)
