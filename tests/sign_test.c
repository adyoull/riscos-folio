/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Folio signature tests (core/sign.c), run under qemu by `make test`:
 *   sign_test tests/sig
 * The PDFs come from tests/make-signed.py (pyHanko), and each is checked
 * against what poppler's pdfsig says of it: rsa.pdf and ec.pdf signed by
 * keys the test CA certified (untrusted, then trusted once ca.pem is given),
 * self.pdf self-signed, tampered.pdf changed inside what was signed,
 * updated.pdf changed by a save after signing, empty.pdf unsigned.
 * With a second directory (from tests/make-p12.sh), signing is tested too:
 * each .p12 signs doc.pdf (a new field) and empty.pdf (its field), and the
 * results are checked here and, by the Makefile, with pdfsig. */
#include "folio_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static folio_ctx *fc;
static const char *dir;

static folio_signer *load_p12(const char *p12dir, const char *leaf, const char *pw)
{
	static unsigned char buf[16384];
	char path[512];
	FILE *f;
	size_t n;
	folio_signer *sg;
	snprintf(path, sizeof path, "%s/%s", p12dir, leaf);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	sg = folio_signer_open(fc, buf, n, pw);
	if (!sg)
		printf("%s: %s\n", leaf, folio_error(fc));
	return sg;
}

/* Signs in (dir/leaf, or a path) into out; field NULL makes a new one. */
static int sign_mode = FOLIO_SIGN_APPROVE;

static int sign_into(const char *in, const char *out, folio_signer *sg, const char *field)
{
	folio_doc *d = folio_open(fc, in, NULL);
	folio_loc loc = { 0, 0 };
	folio_rect where = { 340, 700, 556, 772 };
	int rc;
	if (!d)
		return -2;
	rc = folio_sign(d, loc, field, where, sg, "Approved", "Aldershot", sign_mode);
	if (rc)
		printf("sign: %s\n", folio_error(fc));
	else {
		CHECK(!folio_can_undo(d), "a signature can't be undone before it's saved");
		rc = folio_save_pdf(d, in, out);
	}
	folio_close(d);
	return rc;
}

static const char *dirdata(const char *d)
{
	static char b[512];
	snprintf(b, sizeof b, "%s/../data", d);
	return b;
}

static const char *path_out(const char *d)
{
	static char b[512];
	snprintf(b, sizeof b, "%s/../out/signed-full.pdf", d);
	return b;
}

static int sigs(const char *leaf, folio_signature *s, int max)
{
	char path[512];
	folio_doc *d;
	int n;
	snprintf(path, sizeof path, "%s/%s", dir, leaf);
	d = folio_open(fc, path, NULL);
	if (!d) {
		printf("can't open %s: %s\n", path, folio_error(fc));
		return -2;
	}
	n = folio_signatures(d, s, max);
	if (n > 0)
		printf("%s: '%s' signed %d digest %d cert %d expired %d changed %d by '%s' (%s) %s [%s] [%s]\n", leaf,
			s[0].field, s[0].is_signed, s[0].digest, s[0].cert, s[0].expired, s[0].changed_after, s[0].signer,
			s[0].organisation, s[0].date, s[0].why, s[0].cert_why);
	folio_close(d);
	return n;
}

int main(int argc, char **argv)
{
	folio_signature s[4];
	char path[512];
	FILE *f;
	dir = argc > 1 ? argv[1] : "tests/sig";
	fc = folio_new(0);

	CHECK(sigs("rsa.pdf", s, 4) == 1, "one signature");
	CHECK(s[0].is_signed && s[0].digest == FOLIO_SIG_VALID, "rsa: valid (%d) %s", s[0].digest, s[0].why);
	CHECK(s[0].cert == FOLIO_CERT_UNTRUSTED, "rsa: not trusted yet (%d)", s[0].cert);
	CHECK(!strcmp(s[0].signer, "Jane Signer") && !strcmp(s[0].organisation, "Folio Test"), "signer '%s'", s[0].signer);
	CHECK(!strcmp(s[0].field, "Signature1") && !strcmp(s[0].reason, "Testing Folio") &&
		!strcmp(s[0].location, "Aldershot"), "field, reason, location");
	CHECK(s[0].page == 0 && s[0].rect.x0 > 71 && s[0].rect.x0 < 73, "where: page %d x %g", s[0].page, s[0].rect.x0);
	CHECK(!s[0].changed_after && !s[0].expired, "not changed since; in date");
	CHECK(s[0].date[0] == 'D' && s[0].date[1] == ':', "date '%s'", s[0].date);

	CHECK(sigs("self.pdf", s, 4) == 1 && s[0].digest == FOLIO_SIG_VALID && s[0].cert == FOLIO_CERT_SELF_SIGNED,
		"self-signed: %d %d", s[0].digest, s[0].cert);
	CHECK(sigs("tampered.pdf", s, 4) == 1 && s[0].digest == FOLIO_SIG_CHANGED, "tampered: %d", s[0].digest);
	CHECK(sigs("updated.pdf", s, 4) == 1 && s[0].digest == FOLIO_SIG_VALID && s[0].changed_after,
		"updated: valid but changed after (%d, %d)", s[0].digest, s[0].changed_after);
	CHECK(sigs("empty.pdf", s, 4) == 1 && !s[0].is_signed && !strcmp(s[0].field, "SignHere"), "empty field");

	/* Trust the test CA. */
	snprintf(path, sizeof path, "%s/ca.pem", dir);
	f = fopen(path, "rb");
	CHECK(f != NULL, "ca.pem");
	if (f) {
		unsigned char buf[8192];
		size_t n = fread(buf, 1, sizeof buf, f);
		fclose(f);
		CHECK(folio_set_trust_roots(fc, buf, n) == 1, "one root added (%d now)", folio_trust_root_count());
	}
	CHECK(sigs("rsa.pdf", s, 4) == 1 && s[0].digest == FOLIO_SIG_VALID && s[0].cert == FOLIO_CERT_TRUSTED,
		"rsa: trusted now (%d) %s", s[0].cert, s[0].cert_why);
	CHECK(sigs("ec.pdf", s, 4) == 1 && s[0].digest == FOLIO_SIG_VALID && s[0].cert == FOLIO_CERT_TRUSTED &&
		!strcmp(s[0].signer, "Eve Curve"), "ECDSA: valid and trusted (%d %d) %s", s[0].digest, s[0].cert, s[0].why);
	CHECK(sigs("self.pdf", s, 4) == 1 && s[0].cert == FOLIO_CERT_SELF_SIGNED, "self-signed still");
	CHECK(folio_set_trust_roots(fc, (const unsigned char *)"not a certificate", 17) == -1, "rubbish refused");

	/* A signed PDF keeps its signature: what would change it is refused. */
	{
		folio_doc *d;
		unsigned char sel[1] = { 1 };
		folio_loc l0 = { 0, 0 };
		folio_point at = { 100, 100 };
		char out[600];
		snprintf(path, sizeof path, "%s/rsa.pdf", dir);
		d = folio_open(fc, path, NULL);
		CHECK(d && folio_is_signed(d), "rsa.pdf is signed");
		if (d) {
			CHECK(folio_rotate_pages(d, sel, 1, 90) == -1 && strstr(folio_error(fc), "signed"),
				"signed: no turning pages (%s)", folio_error(fc));
			CHECK(folio_delete_pages(d, sel, 1) == -1, "signed: no deleting pages");
			CHECK(folio_set_password(d, "secret") == -1 && strstr(folio_error(fc), "signature"),
				"signed: no new password (%s)", folio_error(fc));
			CHECK(folio_annot_note(d, l0, at, "a note") == 0, "signed: a note may be added");
			snprintf(out, sizeof out, "%s/../out/signed-note.pdf", dir);
			CHECK(folio_save_pdf(d, path, out) == 0, "signed: saved (incrementally)");
			folio_close(d);
			d = folio_open(fc, out, NULL);
			CHECK(d && folio_signatures(d, s, 4) == 1 && s[0].digest == FOLIO_SIG_VALID && s[0].changed_after,
				"signed: the signature is still good after the note");
			if (d) {
				CHECK(folio_save_pdf(d, NULL, path_out(dir)) == -1, "signed: no full save (%s)", folio_error(fc));
				folio_close(d);
			}
		}
		snprintf(path, sizeof path, "%s/certified.pdf", dir);
		d = folio_open(fc, path, NULL);
		CHECK(d && folio_is_signed(d), "certified.pdf");
		if (d) {
			int i, n = folio_field_count(d, l0), name = -1, locked = -1;
			folio_field f;
			for (i = 0; i < n; i++)
				if (folio_field_get(d, l0, i, &f) == 1) {
					if (!strcmp(f.name, "name")) name = i;
					if (!strcmp(f.name, "locked")) locked = i;
				}
			CHECK(name >= 0 && folio_field_set_text(d, l0, name, "Bea") == 0, "certified: a free field fills in");
			CHECK(locked >= 0 && folio_field_set_text(d, l0, locked, "Changed") == -1 &&
				strstr(folio_error(fc), "locked"), "certified: a locked field doesn't (%s)", folio_error(fc));
			CHECK(folio_annot_note(d, l0, at, "a note") == -1 && strstr(folio_error(fc), "certified"),
				"certified P 2: no notes (%s)", folio_error(fc));
			CHECK(folio_rotate_pages(d, sel, 1, 90) == -1, "certified: no turning pages");
			folio_close(d);
		}
	}

	/* Not a PDF, and a PDF with no signature fields. */
	{
		folio_doc *d;
		snprintf(path, sizeof path, "%s/../data/doc.pdf", dir);
		d = folio_open(fc, path, NULL);
		CHECK(d && folio_signatures(d, s, 4) == 0, "no signature fields");
		if (d)
			folio_close(d);
	}
	if (argc > 2) {
		const char *p12dir = argv[2];
		static const char *const kinds[] = { "rsa-aes.p12", "rsa-legacy.p12", "rsa-3des.p12", "ec.p12" };
		folio_signer *sg;
		char name[128], in[700], out[700];
		int i;
		CHECK(load_p12(p12dir, "rsa-aes.p12", "wrong") == NULL && strstr(folio_error(fc), "password"),
			"wrong password: %s", folio_error(fc));
		CHECK(folio_signer_open(fc, (const unsigned char *)"rubbish", 7, "folio") == NULL, "not a .p12");
		for (i = 0; i < 4; i++) {
			sg = load_p12(p12dir, kinds[i], "folio");
			CHECK(sg != NULL, "%s opens", kinds[i]);
			if (!sg)
				continue;
			folio_signer_name(sg, name, sizeof name);
			CHECK(!strcmp(name, i == 3 ? "Eric Curve" : "Sam Signer"), "%s: name '%s'", kinds[i], name);
			/* A new field on a page that had none. */
			snprintf(in, sizeof in, "%s/../data/doc.pdf", dir);
			snprintf(out, sizeof out, "%s/new-%d.pdf", p12dir, i);
			CHECK(sign_into(in, out, sg, NULL) == 0, "%s: signed a new field", kinds[i]);
			{
				folio_doc *d = folio_open(fc, out, NULL);
				CHECK(d && folio_signatures(d, s, 4) == 1 && s[0].is_signed && s[0].digest == FOLIO_SIG_VALID &&
					!s[0].changed_after && !strcmp(s[0].field, "Signature1") && !strcmp(s[0].signer, i == 3 ? "Eric Curve" : "Sam Signer") &&
					!strcmp(s[0].reason, "Approved") && !strcmp(s[0].location, "Aldershot"),
					"%s: new field verifies (%d) %s", kinds[i], d ? s[0].digest : -1, d ? s[0].why : "");
				CHECK(d && (i == 3 ? s[0].cert == FOLIO_CERT_TRUSTED || s[0].cert == FOLIO_CERT_UNTRUSTED
					: s[0].cert == FOLIO_CERT_SELF_SIGNED), "%s: certificate %d %s", kinds[i], d ? s[0].cert : -1,
					d ? s[0].cert_why : "");
				if (d)
					folio_close(d);
			}
			/* An existing empty field. */
			snprintf(in, sizeof in, "%s/empty.pdf", dir);
			snprintf(out, sizeof out, "%s/field-%d.pdf", p12dir, i);
			CHECK(sign_into(in, out, sg, "SignHere") == 0, "%s: signed SignHere", kinds[i]);
			{
				folio_doc *d = folio_open(fc, out, NULL);
				CHECK(d && folio_signatures(d, s, 4) == 1 && s[0].is_signed && s[0].digest == FOLIO_SIG_VALID &&
					!strcmp(s[0].field, "SignHere"), "%s: field verifies", kinds[i]);
				if (d)
					folio_close(d);
			}
			/* Signing a signed field is refused. */
			CHECK(sign_into(out, in, sg, "SignHere") != 0, "%s: can't sign twice", kinds[i]);
			folio_signer_drop(fc, sg);
		}
		/* A second signature after the first leaves both valid. */
		sg = load_p12(p12dir, "ec.p12", "folio");
		snprintf(in, sizeof in, "%s/new-0.pdf", p12dir);
		snprintf(out, sizeof out, "%s/twice.pdf", p12dir);
		CHECK(sg && sign_into(in, out, sg, NULL) == 0, "second signature");
		{
			folio_doc *d = folio_open(fc, out, NULL);
			int n = d ? folio_signatures(d, s, 4) : -1;
			CHECK(n == 2 && s[0].digest == FOLIO_SIG_VALID && s[0].changed_after && s[1].digest == FOLIO_SIG_VALID &&
				!s[1].changed_after && !strcmp(s[1].field, "Signature2"), "two signatures (%d) %d/%d %d/%d", n,
				s[0].digest, s[0].changed_after, s[1].digest, s[1].changed_after);
			if (d)
				folio_close(d);
		}
		folio_signer_drop(fc, sg);

		/* As Acrobat judges changes after signing. */
		sg = load_p12(p12dir, "rsa-aes.p12", "folio");
		CHECK(sg != NULL, "rsa-aes again");
		if (sg) {
			folio_doc *d;
			folio_loc l0 = { 0, 0 };
			folio_point at = { 100, 100 };
			char a[512], b[512], c[512];
			int k, n, fi = -1;
			folio_field f;
			/* An ordinary signature, then a note: allowed, listed. */
			snprintf(in, sizeof in, "%s/new-0.pdf", p12dir);
			d = folio_open(fc, in, NULL);
			snprintf(a, sizeof a, "%s/note-after.pdf", p12dir);
			CHECK(d && folio_annot_note(d, l0, at, "after") == 0 && folio_save_pdf(d, in, a) == 0, "note after");
			if (d)
				folio_close(d);
			d = folio_open(fc, a, NULL);
			CHECK(d && folio_signatures(d, s, 4) == 1 && s[0].digest == FOLIO_SIG_VALID && s[0].changed_after &&
				(s[0].changes & FOLIO_AFTER_ANNOT) && !s[0].disallowed[0] && !s[0].certified,
				"approval + note: allowed (changes %d, '%s')", d ? s[0].changes : -1, d ? s[0].disallowed : "");
			if (d)
				folio_close(d);

			/* Certified (filling in and signing) on form.pdf. */
			snprintf(in, sizeof in, "%s/form.pdf", dirdata(dir));
			snprintf(a, sizeof a, "%s/certified-forms.pdf", p12dir);
			sign_mode = FOLIO_CERTIFY_FORMS;
			CHECK(sign_into(in, a, sg, NULL) == 0, "certified (forms)");
			sign_mode = FOLIO_SIGN_APPROVE;
			d = folio_open(fc, a, NULL);
			CHECK(d && folio_signatures(d, s, 4) >= 1, "certified opens");
			if (d) {
				for (k = 0, n = folio_signatures(d, s, 4); k < n; k++)
					if (s[k].is_signed)
						break;
				CHECK(k < n && s[k].certified == 2 && s[k].digest == FOLIO_SIG_VALID && !s[k].changed_after,
					"certified P 2 (%d)", k < n ? s[k].certified : -1);
				CHECK(folio_annot_note(d, l0, at, "no") == -1, "certified: Folio adds no note");
				for (k = 0, n = folio_field_count(d, l0); k < n; k++)
					if (folio_field_get(d, l0, k, &f) == 1 && !strcmp(f.name, "name"))
						fi = k;
				snprintf(b, sizeof b, "%s/certified-filled.pdf", p12dir);
				k = fi >= 0 ? folio_field_set_text(d, l0, fi, "Bea") : -9;
				CHECK(k == 0 && folio_save_pdf(d, a, b) == 0, "certified: filled in and saved (%d %d %s)", fi, k,
					folio_error(fc));
				folio_close(d);
				d = folio_open(fc, b, NULL);
				for (k = 0, n = d ? folio_signatures(d, s, 4) : 0; k < n; k++)
					if (s[k].is_signed)
						break;
				CHECK(k < n && s[k].changed_after && (s[k].changes & FOLIO_AFTER_FORM) && !s[k].disallowed[0],
					"certified + filling in: allowed (%d '%s')", k < n ? s[k].changes : -1, k < n ? s[k].disallowed : "");
				/* Another program adds a note anyway (MuPDF directly). */
				if (d) {
					pdf_document *pdf = pdf_specifics(d->ctx, d->doc);
					pdf_page *pg = pdf_load_page(d->ctx, pdf, 0);
					pdf_begin_operation(d->ctx, pdf, "test");
					pdf_create_annot(d->ctx, pg, PDF_ANNOT_TEXT);
					pdf_end_operation(d->ctx, pdf);
					fz_drop_page(d->ctx, &pg->super);
					snprintf(c, sizeof c, "%s/certified-noted.pdf", p12dir);
					CHECK(folio_save_pdf(d, b, c) == 0, "saved with the note");
					folio_close(d);
					d = folio_open(fc, c, NULL);
					for (k = 0, n = d ? folio_signatures(d, s, 4) : 0; k < n; k++)
						if (s[k].is_signed)
							break;
					CHECK(k < n && s[k].digest == FOLIO_SIG_VALID && strstr(s[k].disallowed, "notes"),
						"certified + note: not allowed ('%s')", k < n ? s[k].disallowed : "");
					if (d)
						folio_close(d);
				}
			}

			/* A handwritten signature shown in a digital one. */
			{
				folio_point pts[5] = { { 4, 30 }, { 20, 4 }, { 40, 30 }, { 60, 4 }, { 80, 30 } };
				int cnt[1] = { 5 };
				char png[700];
				snprintf(png, sizeof png, "%s/hand.png", p12dir);
				CHECK(folio_strokes_png(fc, pts, cnt, 1, 84, 34, 0x1a2b8c, 2, png) == 0, "strokes to PNG");
				CHECK(folio_signer_set_picture(fc, sg, png, 0) == 0, "picture for the signature");
				snprintf(in, sizeof in, "%s/../data/doc.pdf", dir);
				snprintf(a, sizeof a, "%s/hand.pdf", p12dir);
				CHECK(sign_into(in, a, sg, NULL) == 0, "signed with the handwriting shown");
				CHECK(folio_signer_set_picture(fc, sg, NULL, 0) == 0, "picture off");
				d = folio_open(fc, a, NULL);
				CHECK(d && folio_signatures(d, s, 4) == 1 && s[0].digest == FOLIO_SIG_VALID, "hand.pdf verifies");
				if (d)
					folio_close(d);
			}

			/* Signed and locked: fields read-only; a change by another
			 * program breaks it. */
			snprintf(in, sizeof in, "%s/form.pdf", dirdata(dir));
			snprintf(a, sizeof a, "%s/locked.pdf", p12dir);
			sign_mode = FOLIO_SIGN_LOCK;
			CHECK(sign_into(in, a, sg, NULL) == 0, "signed and locked");
			sign_mode = FOLIO_SIGN_APPROVE;
			d = folio_open(fc, a, NULL);
			if (d) {
				for (k = 0, n = folio_field_count(d, l0), fi = -1; k < n; k++)
					if (folio_field_get(d, l0, k, &f) == 1 && !strcmp(f.name, "name"))
						fi = k;
				CHECK(fi >= 0 && folio_field_set_text(d, l0, fi, "Cy") == -1, "locked: no filling in");
				CHECK(folio_annot_note(d, l0, at, "no") == -1, "locked: no notes");
				{
					pdf_document *pdf = pdf_specifics(d->ctx, d->doc);
					pdf_obj *fld = pdf_lookup_field(d->ctx, pdf_dict_getp(d->ctx, pdf_trailer(d->ctx, pdf),
						"Root/AcroForm/Fields"), "name");
					pdf_begin_operation(d->ctx, pdf, "test");
					pdf_dict_put_text_string(d->ctx, fld, PDF_NAME(V), "Changed");
					pdf_end_operation(d->ctx, pdf);
				}
				snprintf(b, sizeof b, "%s/locked-changed.pdf", p12dir);
				CHECK(folio_save_pdf(d, a, b) == 0, "locked: changed by another program");
				folio_close(d);
				d = folio_open(fc, b, NULL);
				for (k = 0, n = d ? folio_signatures(d, s, 4) : 0; k < n; k++)
					if (s[k].is_signed)
						break;
				CHECK(k < n && s[k].digest == FOLIO_SIG_VALID && s[k].disallowed[0],
					"locked + change: not allowed ('%s')", k < n ? s[k].disallowed : "");
				if (d)
					folio_close(d);
			}
			folio_signer_drop(fc, sg);
		}
	}
	folio_drop(fc);
	printf("sign_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
