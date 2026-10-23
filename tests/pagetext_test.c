/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Text for reading aloud (folio_get_page_text), run under qemu by make test:
 *   pagetext_test tests/data
 * speech.pdf: a heading, then a paragraph with "exam-" / "ple" at a line
 * end; page 2 one sentence. book.epub: a chapter's text. Then PDFs made
 * here with each line a block of its own (double-spaced text): a sentence
 * running over line ends reads as one paragraph, but a heading, a list and
 * a finished paragraph stay apart. */
#include "folio.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A one-page PDF of lines: "size x y text" each, Times-Roman. */
static void write_lines(const char *path, const char *const *lines, int n)
{
	char content[4096];
	int len = 0, i;
	long off[6], xref;
	FILE *f;
	len += snprintf(content + len, sizeof content - len, "BT\n");
	for (i = 0; i < n; i++) {
		int size, x, y, k = 0;
		sscanf(lines[i], "%d %d %d %n", &size, &x, &y, &k);
		len += snprintf(content + len, sizeof content - len, "/F1 %d Tf 1 0 0 1 %d %d Tm (%s) Tj\n",
			size, x, y, lines[i] + k);
	}
	len += snprintf(content + len, sizeof content - len, "ET\n");
	f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "%%PDF-1.4\n");
	off[1] = ftell(f);
	fprintf(f, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
	off[2] = ftell(f);
	fprintf(f, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
	off[3] = ftell(f);
	fprintf(f, "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R "
		"/Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n");
	off[4] = ftell(f);
	fprintf(f, "4 0 obj\n<< /Length %d >>\nstream\n%sendstream\nendobj\n", len, content);
	off[5] = ftell(f);
	fprintf(f, "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman >>\nendobj\n");
	xref = ftell(f);
	fprintf(f, "xref\n0 6\n0000000000 65535 f \n");
	for (i = 1; i < 6; i++)
		fprintf(f, "%010ld 00000 n \n", off[i]);
	fprintf(f, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", xref);
	fclose(f);
}

static char *lines_text(folio_ctx *fc, const char *path, const char *const *lines, int n)
{
	static char out[2048];
	folio_doc *d;
	folio_page_text t;
	write_lines(path, lines, n);
	out[0] = 0;
	d = folio_open(fc, path, NULL);
	if (!d)
		return out;
	if (folio_get_page_text(d, (folio_loc){ 0, 0 }, &t) == 0) {
		snprintf(out, sizeof out, "%s", t.text);
		folio_page_text_free(&t);
	}
	folio_close(d);
	return out;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/data";
	char path[512];
	folio_ctx *fc = folio_new(64 << 20);
	folio_doc *d;
	folio_page_text t;
	const char *p;
	int i, ok;

	snprintf(path, sizeof path, "%s/speech.pdf", dir);
	d = folio_open(fc, path, NULL);
	CHECK(d != NULL, "open speech.pdf: %s", folio_error(fc));
	if (!d)
		return 1;
	CHECK(folio_get_page_text(d, (folio_loc){ 0, 0 }, &t) == 0, "page 1: %s", folio_error(fc));
	printf("page 1: [%s]\n", t.text);
	CHECK(strstr(t.text, "Chapter One") != NULL, "heading");
	CHECK(strstr(t.text, "Chapter One\n\n") != NULL, "heading is its own paragraph");
	CHECK(strstr(t.text, "an example of a word") != NULL, "hyphenated word joined");
	CHECK(strstr(t.text, "exam-") == NULL, "hyphen left in");
	CHECK(t.len == (int)strlen(t.text), "len %d", t.len);
	for (ok = 1, i = 0; i < t.len; i++)
		if (t.byte_char[i] < -1 || t.byte_char[i] >= t.nchars)
			ok = 0;
	CHECK(ok, "byte_char out of range");
	/* Every letter has a character; added spaces and breaks don't. */
	p = strstr(t.text, "quick");
	CHECK(p && t.byte_char[p - t.text] >= 0, "letters have characters");
	p = strstr(t.text, "\n\n");
	CHECK(p && t.byte_char[p - t.text] == -1, "breaks have none");
	/* "q" before "u" on the line; "ple" on the line below "exam". */
	p = strstr(t.text, "quick");
	if (p) {
		folio_quad q0 = t.quads[t.byte_char[p - t.text]], q1 = t.quads[t.byte_char[p + 1 - t.text]];
		CHECK(q0.ul.x < q1.ul.x && q0.ul.y == q1.ul.y, "q then u");
	}
	p = strstr(t.text, "example");
	if (p) {
		folio_quad qe = t.quads[t.byte_char[p - t.text]], qp = t.quads[t.byte_char[p + 4 - t.text]];
		CHECK(qp.ul.y > qe.ul.y + 5, "ple below exam (%.1f vs %.1f)", qp.ul.y, qe.ul.y);
	}
	folio_page_text_free(&t);
	CHECK(folio_get_page_text(d, (folio_loc){ 0, 1 }, &t) == 0 && strstr(t.text, "one more sentence."),
		"page 2");
	folio_page_text_free(&t);
	folio_close(d);

	snprintf(path, sizeof path, "%s/book.epub", dir);
	d = folio_open(fc, path, NULL);
	CHECK(d != NULL, "open book.epub");
	if (d) {
		folio_count_step(d, 1 << 20);
		CHECK(folio_get_page_text(d, (folio_loc){ 1, 0 }, &t) == 0 && t.len > 20 && t.nchars > 20,
			"epub text: %d bytes", t.len);
		printf("epub: [%.80s...]\n", t.text);
		folio_page_text_free(&t);
		folio_close(d);
	}
	/* Each line a block of its own (double-spaced). */
	mkdir("tests/out", 0777);
	{
		static const char *const para[] = {
			"18 72 780 Chapter Two",
			"12 72 740 The quick brown fox jumps over the lazy dog, and then it runs",
			"12 72 712 into the woods where the trees are tall and the light is an exam-",
			"12 72 684 ple of dimness; nobody follows it there. After a while it comes",
			"12 72 656 back out again and sits down by the river to rest for a while.",
			"12 72 628 The next paragraph starts here.",
		};
		static const char *const list[] = {
			"12 72 780 Things to bring on the long walk across the hills and the moors:",
			"12 90 752 a map",
			"12 90 724 some water",
			"12 90 696 a coat for when it rains",
		};
		static const char *const heads[] = {
			"12 72 780 A short line",
			"12 72 752 then a long line that fills the whole of the column from side to side",
		};
		const char *x = lines_text(fc, "tests/out/lines1.pdf", para, 6);
		printf("lines1: [%s]\n", x);
		CHECK(!strncmp(x, "Chapter Two\n\nThe quick", 22), "a heading stays apart");
		CHECK(strstr(x, "it runs into the woods") != NULL, "a sentence goes on over the line end");
		CHECK(strstr(x, "an example of dimness") != NULL, "a word hyphenated at a block's end joined");
		CHECK(strstr(x, "it comes back out") != NULL, "mid-sentence, after a full stop in the line");
		CHECK(strstr(x, "for a while.\n\nThe next paragraph") != NULL, "a finished paragraph stays apart");
		x = lines_text(fc, "tests/out/lines2.pdf", list, 4);
		printf("lines2: [%s]\n", x);
		CHECK(strstr(x, "moors:\n\na map\n\nsome water\n\na coat") != NULL, "a list stays apart");
		x = lines_text(fc, "tests/out/lines3.pdf", heads, 2);
		CHECK(strstr(x, "A short line\n\nthen") != NULL, "a short line isn't joined: [%s]", x);
	}
	folio_drop(fc);

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
