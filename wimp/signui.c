/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Signatures on the desktop (core/sign.c checks them):
 *  - the roots Folio trusts: Adobe's (built in), CaCertificates' bundle if
 *    it's installed (the one PackMan uses: <CaCertificates$Dir>.ca-
 *    certificates/crt), and any certificates in Choices:Folio.Trusted (a PEM
 *    file); read by their RISC OS names, once, when first needed;
 *  - what a signature says, in words (Document info, and a click on it);
 *  - a warning on opening a PDF whose signature has been broken. */
#include "app.h"
#include "worker.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

static int roots_loaded;

/* A file's bytes by its RISC OS name (UnixLib names with <...$Dir> don't
 * open here: test48's fonts). */
static unsigned char *read_native(const char *ro, int *len)
{
	int type = 0, n = 0;
	unsigned char *b;
	if (_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUT(4), 17, ro, &type, &n) || type != 1 || n <= 0)
		return NULL;
	b = malloc((size_t)n);
	if (b && _swix(OS_File, _INR(0, 3), 16, ro, b, 0)) {
		free(b);
		b = NULL;
	}
	*len = n;
	return b;
}

void signui_roots(void)
{
	static const char *const files[] = {
		"<CaCertificates$Dir>.ca-certificates/crt",
		"Choices:Folio.Trusted",
	};
	int i;
	if (roots_loaded)
		return;
	roots_loaded = 1;
	for (i = 0; i < (int)(sizeof files / sizeof files[0]); i++) {
		int len = 0, n;
		unsigned char *b = read_native(files[i], &len);
		if (!b)
			continue;
		n = folio_set_trust_roots(app.fc, b, (size_t)len);
		free(b);
		app_log("signatures: %s: %d certificate%s trusted", files[i], n, n == 1 ? "" : "s");
	}
	app_log("signatures: %d trusted roots in all", folio_trust_root_count());
}

/* What a signature says, in a sentence or three (UTF-8). */
void signui_describe(const folio_signature *s, char *out, size_t size)
{
	char when[64], who[300];
	const char *verdict, *cert, *after = "";
	if (!s->is_signed) {
		snprintf(out, size, "'%s' is a place for a signature; it hasn't been signed.", s->field);
		return;
	}
	when[0] = 0;
	if (s->date[0])
		folio_pdf_date(s->date, when, sizeof when);
	if (s->organisation[0])
		snprintf(who, sizeof who, "%s (%s)", s->signer[0] ? s->signer : "someone", s->organisation);
	else
		snprintf(who, sizeof who, "%s", s->signer[0] ? s->signer : "someone unnamed");
	verdict = s->digest == FOLIO_SIG_VALID ? "The signature is good" :
		s->digest == FOLIO_SIG_CHANGED ? "The signature is BROKEN: the document has been changed since it was signed" :
		"Folio can't check the signature";
	cert = s->digest != FOLIO_SIG_VALID ? "" :
		s->cert == FOLIO_CERT_TRUSTED ? ", and the signer's certificate comes from a body Folio trusts" :
		s->cert == FOLIO_CERT_SELF_SIGNED ? ", but the signer vouches for themselves: nobody Folio trusts says who they are" :
		s->cert == FOLIO_CERT_UNTRUSTED ? ", but the signer's certificate doesn't come from a body Folio trusts, so who signed it can't be confirmed" :
		s->cert == FOLIO_CERT_NONE ? ", but there's no certificate to say who signed it" :
		", but there's something wrong with the signer's certificate";
	if (s->digest == FOLIO_SIG_VALID && s->changed_after)
		after = " The document has had changes saved since it was signed (forms filled in or notes added, perhaps); what was signed is as it was.";
	snprintf(out, size, "Signed by %s%s%s. %s%s.%s%s%s%s%s%s%s", who, when[0] ? " on " : "", when,
		verdict, cert, after,
		s->expired && s->digest == FOLIO_SIG_VALID ? " The certificate is out of date now." : "",
		s->digest == FOLIO_SIG_UNREADABLE && s->why[0] ? " (" : "",
		s->digest == FOLIO_SIG_UNREADABLE ? s->why : "",
		s->digest == FOLIO_SIG_UNREADABLE && s->why[0] ? ")" : "",
		s->reason[0] ? " Reason: " : "", s->reason);
}

/* A short form for Document info's rows. */
void signui_brief(const folio_signature *s, char *out, size_t size)
{
	char when[64];
	when[0] = 0;
	if (!s->is_signed) {
		snprintf(out, size, "%s: not signed", s->field);
		return;
	}
	if (s->date[0])
		folio_pdf_date(s->date, when, sizeof when);
	snprintf(out, size, "%s%s%s: %s%s", s->signer[0] ? s->signer : "(no name)", when[0] ? ", " : "", when,
		s->digest == FOLIO_SIG_VALID ? (s->cert == FOLIO_CERT_TRUSTED ? "good, trusted" :
			s->cert == FOLIO_CERT_SELF_SIGNED ? "good, self-signed" : "good, signer not confirmed") :
		s->digest == FOLIO_SIG_CHANGED ? "BROKEN (changed since)" : "can't be checked",
		s->digest == FOLIO_SIG_VALID && s->changed_after ? "; changed since" : "");
}

/* A signature field clicked: say what it is. */
void signui_click(docwin *w, int page, const char *field)
{
	folio_signature s[16];
	char text[700], *d;
	int n, i;
	signui_roots();
	_swix(Hourglass_On, 0);
	core_now();
	n = folio_signatures(docwin_doc(w), s, 16);
	core_unlock();
	_swix(Hourglass_Off, 0);
	for (i = 0; i < n; i++)
		if (s[i].page == page && !strcmp(s[i].field, field))
			break;
	if (i >= n) {
		tbx_report("Folio couldn't find that signature.");
		return;
	}
	signui_describe(&s[i], text, sizeof text);
	app_log("signature '%s': digest %d cert %d changed %d (%s)", field, s[i].digest, s[i].cert,
		s[i].changed_after, s[i].why);
	d = app_desktop_text(text);
	ask_show("Signature", d ? d : text, "OK", NULL, NULL, NULL);
	free(d);
}

/* On opening: a broken signature is worth saying at once. */
void signui_on_open(docwin *w)
{
	folio_signature s[16];
	char text[800], line[300], *d;
	int n, i, broken = 0;
	core_now();
	n = folio_has_signatures(docwin_doc(w));
	core_unlock();
	if (n <= 0)
		return;
	signui_roots();
	core_now();
	n = folio_signatures(docwin_doc(w), s, 16);
	core_unlock();
	snprintf(text, sizeof text, "This PDF's signature has been broken: it has been changed since it was signed.");
	for (i = 0; i < n; i++) {
		app_log("signature '%s': signed %d digest %d cert %d changed %d", s[i].field, s[i].is_signed,
			s[i].digest, s[i].cert, s[i].changed_after);
		if (s[i].is_signed && s[i].digest == FOLIO_SIG_CHANGED) {
			broken++;
			if (broken == 1 && s[i].signer[0]) {
				snprintf(line, sizeof line, " (signed by %s)", s[i].signer);
				strncat(text, line, sizeof text - strlen(text) - 1);
			}
		}
	}
	if (!broken)
		return;
	d = app_desktop_text(text);
	ask_show("Signature broken", d ? d : text, "OK", NULL, NULL, NULL);
	free(d);
}
