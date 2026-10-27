/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Folio signature tests (core/sign.c), run under qemu by `make test`:
 *   sign_test tests/sig
 * The PDFs come from tests/make-signed.py (pyHanko), and each is checked
 * against what poppler's pdfsig says of it: rsa.pdf and ec.pdf signed by
 * keys the test CA certified (untrusted, then trusted once ca.pem is given),
 * self.pdf self-signed, tampered.pdf changed inside what was signed,
 * updated.pdf changed by a save after signing, empty.pdf unsigned. */
#include "folio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static folio_ctx *fc;
static const char *dir;

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

	/* Not a PDF, and a PDF with no signature fields. */
	{
		folio_doc *d;
		snprintf(path, sizeof path, "%s/../data/doc.pdf", dir);
		d = folio_open(fc, path, NULL);
		CHECK(d && folio_signatures(d, s, 4) == 0, "no signature fields");
		if (d)
			folio_close(d);
	}
	folio_drop(fc);
	printf("sign_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
