/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Folio password tests (core/security.c and folio_open_password), run under
 * qemu by `make test`:
 *   password_test tests/data
 * Puts a password on doc.pdf, opens it with no, a wrong and the right
 * password, saves changes to it, takes the password off again, and does the
 * same for a file with only an owner password. Writes tests/out/pw-*.pdf. */
#include "folio.h"
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A password with letters outside ASCII (UTF-8), as typed in Latin-1 and
 * converted by the front end. */
#define PW "S\xc3\xa9same 42"

static int pages_of(folio_doc *d)
{
	int exact = 0;
	folio_count_step(d, 1 << 20);
	return folio_page_count(d, &exact);
}

/* Draws a corner of page 1: does decryption work? */
static int draws(folio_doc *d)
{
	static uint32_t px[32 * 32];
	folio_irect t = { 0, 0, 32, 32 };
	return folio_render_tile(d, (folio_loc){ 0, 0 }, 1.0f, t, FOLIO_PAGE_NORMAL, 1, px, 32 * 4) == 0;
}

/* Does the file hold the bytes of text? */
static int file_has(const char *path, const char *text)
{
	FILE *f = fopen(path, "rb");
	static char buf[1 << 20];
	size_t n;
	if (!f)
		return 0;
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	return memmem(buf, n, text, strlen(text)) != NULL;
}

/* doc.pdf with an owner password only (anyone can open it; changing its
 * security needs "owner"), made with MuPDF directly. */
static void make_owner_only(const char *src, const char *out)
{
	fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	pdf_document *pdf = NULL;
	pdf_write_options o = pdf_default_write_options;
	fz_register_document_handlers(ctx);
	fz_try(ctx) {
		pdf = pdf_open_document(ctx, src);
		o.do_encrypt = PDF_ENCRYPT_AES_128;
		o.permissions = ~(4 | 8 | 32);   /* no printing, changing or notes */
		strcpy(o.opwd_utf8, "owner");
		strcpy(o.upwd_utf8, "");
		pdf_save_document(ctx, pdf, out, &o);
	}
	fz_always(ctx)
		pdf_drop_document(ctx, pdf);
	fz_catch(ctx)
		printf("make_owner_only: %s\n", fz_caught_message(ctx));
	fz_drop_context(ctx);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/data";
	char src[512], epub[512], longpw[200];
	const char *pw = "tests/out/pw-set.pdf", *pw2 = "tests/out/pw-changed.pdf",
		*nopw = "tests/out/pw-removed.pdf", *own = "tests/out/pw-owner.pdf",
		*own2 = "tests/out/pw-owner-removed.pdf";
	folio_ctx *fc = folio_new(64 << 20);
	folio_doc *d;
	folio_security s;
	folio_quad q[4];
	int mark[4], pages;

	mkdir("tests/out", 0777);
	snprintf(src, sizeof src, "%s/doc.pdf", dir);
	snprintf(epub, sizeof epub, "%s/book.epub", dir);

	/* A plain PDF: nothing to say. */
	d = folio_open(fc, src, NULL);
	CHECK(d != NULL, "open doc.pdf: %s", folio_error(fc));
	if (!d)
		return 1;
	pages = pages_of(d);
	CHECK(folio_get_security(d, &s) == 0 && s.pdf && !s.encrypted && !s.needs_password && s.owner &&
		s.change == FOLIO_PW_KEEP, "plain: pdf %d enc %d needs %d owner %d change %d", s.pdf,
		s.encrypted, s.needs_password, s.owner, s.change);
	CHECK(!folio_is_modified(d), "plain: not modified");

	/* Removing a password it hasn't got does nothing. */
	CHECK(folio_set_password(d, NULL) == 0 && !folio_is_modified(d), "remove from plain");
	memset(longpw, 'x', sizeof longpw - 1);
	longpw[sizeof longpw - 1] = 0;
	CHECK(folio_set_password(d, longpw) == -1, "too long accepted");

	/* Set one: modified until saved. */
	CHECK(folio_set_password(d, PW) == 0, "set: %s", folio_error(fc));
	folio_get_security(d, &s);
	CHECK(s.change == FOLIO_PW_SET && folio_is_modified(d), "set: change %d", s.change);
	CHECK(folio_save_pdf(d, src, pw) == 0, "save with password: %s", folio_error(fc));
	folio_close(d);
	CHECK(file_has(pw, "/Encrypt"), "not encrypted on disk");

	/* Opening it. */
	d = folio_open(fc, pw, NULL);
	CHECK(!d && folio_error_code(fc) == FOLIO_ERR_PASSWORD, "no password: code %d", folio_error_code(fc));
	d = folio_open_password(fc, pw, NULL, "sesame");
	CHECK(!d && folio_error_code(fc) == FOLIO_ERR_WRONG_PASSWORD, "wrong: code %d", folio_error_code(fc));
	d = folio_open_password(fc, pw, NULL, "S\xc3\xa9same 4");
	CHECK(!d && folio_error_code(fc) == FOLIO_ERR_WRONG_PASSWORD, "prefix accepted");
	d = folio_open_password(fc, pw, NULL, PW);
	CHECK(d != NULL, "right password: %s", folio_error(fc));
	if (!d)
		return 1;
	CHECK(pages_of(d) == pages && draws(d), "pages %d (want %d), drawing", pages_of(d), pages);
	folio_get_security(d, &s);
	CHECK(s.encrypted && s.needs_password && s.owner && !strncmp(s.method, "AES", 3),
		"opened: enc %d needs %d owner %d method '%s'", s.encrypted, s.needs_password, s.owner, s.method);
	CHECK(folio_find_on_page(d, (folio_loc){ 0, 1 }, "Folio test page 2", q, mark, 4) > 0,
		"text of page 2 not found");

	/* A change saved incrementally keeps the password. */
	CHECK(folio_find_on_page(d, (folio_loc){ 0, 1 }, "page", q, mark, 1) == 1, "find 'page'");
	CHECK(folio_annot_markup(d, (folio_loc){ 0, 1 }, FOLIO_ANNOT_UNDERLINE,
		(folio_point){ q[0].ul.x + 0.5f, (q[0].ul.y + q[0].ll.y) / 2 },
		(folio_point){ q[0].ur.x - 0.5f, (q[0].ur.y + q[0].lr.y) / 2 }) == 0, "underline: %s", folio_error(fc));
	CHECK(folio_save_pdf(d, pw, pw2) == 0, "incremental save: %s", folio_error(fc));
	folio_close(d);
	d = folio_open(fc, pw2, NULL);
	CHECK(!d && folio_error_code(fc) == FOLIO_ERR_PASSWORD, "changed file lost its password");
	d = folio_open_password(fc, pw2, NULL, PW);
	CHECK(d && draws(d), "changed file: %s", folio_error(fc));
	if (!d)
		return 1;

	/* Take it off. */
	CHECK(folio_set_password(d, "") == 0, "remove: %s", folio_error(fc));
	folio_get_security(d, &s);
	CHECK(s.change == FOLIO_PW_REMOVE && folio_is_modified(d), "remove: change %d", s.change);
	CHECK(folio_save_pdf(d, pw2, nopw) == 0, "save without: %s", folio_error(fc));
	folio_close(d);
	d = folio_open(fc, nopw, NULL);
	CHECK(d != NULL, "open after removing: %s", folio_error(fc));
	if (!d)
		return 1;
	folio_get_security(d, &s);
	CHECK(!s.encrypted && s.owner && pages_of(d) == pages && draws(d), "removed: enc %d", s.encrypted);
	CHECK(!file_has(nopw, "/Encrypt"), "still encrypted on disk");
	folio_close(d);

	/* An owner password only: opens without asking, but its security needs it. */
	make_owner_only(src, own);
	d = folio_open(fc, own, NULL);
	CHECK(d != NULL, "owner-only: %s", folio_error(fc));
	if (!d)
		return 1;
	folio_get_security(d, &s);
	CHECK(s.encrypted && !s.needs_password && !s.owner, "owner-only: enc %d needs %d owner %d",
		s.encrypted, s.needs_password, s.owner);
	CHECK(folio_set_password(d, "new") == -1 && folio_set_password(d, NULL) == -1,
		"changed without the owner password");
	CHECK(folio_check_owner_password(d, "Owner") == 0, "wrong owner password accepted");
	CHECK(draws(d) && pages_of(d) == pages, "a wrong owner password broke the document");
	CHECK(folio_check_owner_password(d, "owner") == 1, "owner password: %s", folio_error(fc));
	folio_get_security(d, &s);
	CHECK(s.owner && draws(d), "owner rights");
	CHECK(folio_set_password(d, NULL) == 0, "remove restrictions: %s", folio_error(fc));
	CHECK(folio_save_pdf(d, own, own2) == 0, "save: %s", folio_error(fc));
	folio_close(d);
	d = folio_open(fc, own2, NULL);
	CHECK(d != NULL, "reopen: %s", folio_error(fc));
	if (d) {
		folio_get_security(d, &s);
		CHECK(!s.encrypted && s.owner && draws(d), "restrictions left: enc %d", s.encrypted);
		folio_close(d);
	}

	/* Not a PDF. */
	d = folio_open(fc, epub, NULL);
	CHECK(d != NULL, "open epub: %s", folio_error(fc));
	if (d) {
		CHECK(folio_get_security(d, &s) == 0 && !s.pdf, "epub is a pdf?");
		CHECK(folio_set_password(d, "x") == -1, "epub given a password");
		folio_close(d);
	}

	folio_drop(fc);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
