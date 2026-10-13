/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio core: PDF passwords. Opening with a password is in folio_open_password
 * (doc.c); this file keeps what was learnt then, checks owner passwords, and
 * sets up the save that puts on or takes off a password.
 *
 * A new password encrypts with AES-256 (PDF 2.0, revision 6), the same
 * password for opening and for full rights, nothing restricted. The keys
 * MuPDF makes come from its ChaCha20 generator, which is seeded from
 * /dev/urandom here just before (on RISC OS, UnixLib reads that from the
 * CryptRandom module); without that, no password is set, since a key from
 * the clock could be guessed. */
#include "folio_int.h"
#include <stdio.h>
#include <string.h>

void folio_security_init(folio_doc *doc, const char *pw)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	doc->encrypted = pdf && pdf->crypt;
	doc->owner = !doc->encrypted;
	if (!doc->encrypted)
		return;
	/* Again with the same password (or none), to learn whether it gave full
	 * rights: 4 = the owner password. */
	fz_try(ctx)
		doc->owner = (pdf_authenticate_password(ctx, pdf, pw ? pw : "") & 4) != 0;
	fz_catch(ctx) {
		fz_ignore_error(ctx);
		doc->owner = 0;
	}
}

int folio_get_security(folio_doc *doc, folio_security *sec)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	memset(sec, 0, sizeof *sec);
	if (!pdf)
		return 0;
	sec->pdf = 1;
	sec->encrypted = doc->encrypted;
	sec->needs_password = doc->needs_pw;
	sec->owner = doc->owner;
	sec->change = doc->pw_change;
	if (doc->encrypted) {
		fz_try(ctx) {
			const char *m = pdf_crypt_method(ctx, pdf->crypt);
			int bits = pdf_crypt_length(ctx, pdf->crypt);
			if (bits > 0)
				snprintf(sec->method, sizeof sec->method, "%s %d", m, bits);
			else
				snprintf(sec->method, sizeof sec->method, "%s", m);
		}
		fz_catch(ctx)
			fz_ignore_error(ctx);
	}
	return 0;
}

int folio_check_owner_password(folio_doc *doc, const char *password)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	fz_document *copy = NULL;
	int bits = 0;
	if (!pdf) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "only PDFs have passwords");
		return -1;
	}
	if (doc->owner)
		return 1;
	/* Tried on a second copy of the file: a wrong password given to the
	 * open document would leave it unable to decrypt anything. */
	fz_var(copy);
	fz_try(ctx) {
		pdf_document *p;
		copy = fz_open_document(ctx, doc->path);
		p = pdf_specifics(ctx, copy);
		if (p)
			bits = pdf_authenticate_password(ctx, p, password ? password : "");
	}
	fz_always(ctx)
		fz_drop_document(ctx, copy);
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	if (!(bits & 4))
		return 0;
	/* Right, so the open document can be given it too. */
	fz_try(ctx)
		pdf_authenticate_password(ctx, pdf, password);
	fz_catch(ctx)
		fz_ignore_error(ctx);
	doc->owner = 1;
	return 1;
}

/* Seeds MuPDF's random numbers from the system's. 0, or -1 if there's no
 * source of them. */
static int reseed(fz_context *ctx)
{
	unsigned char e[44];
	size_t n = 0;
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		n = fread(e, 1, sizeof e, f);
		fclose(f);
	}
	if (n != sizeof e)
		return -1;
	/* Counter 1: fz_memrnd seeds itself again (from the clock, on RISC OS)
	 * whenever word 12 is 0. */
	fz_chacha20_init(&ctx->seed, e, e + 32, 1);
	memset(e, 0, sizeof e);
	return 0;
}

static const char no_random[] =
	"there's no source of secure random numbers (on RISC OS, the CryptRandom module)";

int folio_set_password(folio_doc *doc, const char *password)
{
	if (!pdf_specifics(doc->ctx, doc->doc)) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "only PDFs can have a password");
		return -1;
	}
	if (!doc->owner) {
		snprintf(doc->fc->err, sizeof doc->fc->err,
			"the owner password is needed to change this document's security");
		return -1;
	}
	if (password && *password) {
		if (strlen(password) >= sizeof doc->new_pw) {
			snprintf(doc->fc->err, sizeof doc->fc->err, "the password is too long");
			return -1;
		}
		if (reseed(doc->ctx)) {
			snprintf(doc->fc->err, sizeof doc->fc->err, "%s", no_random);
			return -1;
		}
		strcpy(doc->new_pw, password);
		doc->pw_change = FOLIO_PW_SET;
	} else {
		memset(doc->new_pw, 0, sizeof doc->new_pw);
		/* Removing from a file without one just forgets a password set
		 * since it was opened. */
		doc->pw_change = doc->encrypted ? FOLIO_PW_REMOVE : FOLIO_PW_KEEP;
	}
	return 0;
}

void folio_security_write_options(folio_doc *doc, pdf_write_options *opts)
{
	fz_context *ctx = doc->ctx;
	/* Encryption changes every object, so it can't be an incremental save. */
	opts->do_incremental = 0;
	if (doc->pw_change == FOLIO_PW_SET) {
		if (reseed(ctx))
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "%s", no_random);
		opts->do_encrypt = PDF_ENCRYPT_AES_256;
		opts->permissions = ~0;
		fz_strlcpy(opts->opwd_utf8, doc->new_pw, sizeof opts->opwd_utf8);
		fz_strlcpy(opts->upwd_utf8, doc->new_pw, sizeof opts->upwd_utf8);
	} else if (doc->pw_change == FOLIO_PW_REMOVE) {
		opts->do_encrypt = PDF_ENCRYPT_NONE;
	}
}

int folio_permitted(folio_doc *doc, folio_permission what)
{
	fz_context *ctx = doc->ctx;
	fz_permission p;
	int r = 1;
	switch (what) {
	case FOLIO_PERM_PRINT: p = FZ_PERMISSION_PRINT; break;
	case FOLIO_PERM_COPY: p = FZ_PERMISSION_COPY; break;
	case FOLIO_PERM_ACCESSIBILITY: p = FZ_PERMISSION_ACCESSIBILITY; break;
	case FOLIO_PERM_ANNOTATE: p = FZ_PERMISSION_ANNOTATE; break;
	default: return 1;
	}
	fz_var(r);
	fz_try(ctx)
		r = fz_has_permission(ctx, doc->doc, p) != 0;
	fz_catch(ctx)
		r = 1;
	return r;
}
