/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio core: PDF signatures, checked with mbedTLS through MuPDF's
 * pdf_pkcs7_verifier (MuPDF finds each signature's byte ranges and its CMS
 * blob; this does the cryptography).
 *
 * A PDF signature is CMS SignedData (RFC 5652), detached: the signed content
 * is the file's bytes outside the signature's own /Contents (adbe.pkcs7.
 * detached, ETSI.CAdES.detached), or, for the old adbe.pkcs7.sha1, the SHA-1
 * of those bytes carried inside. Checking it:
 *  1. digest: hash the bytes with the signer's digest algorithm. With signed
 *     attributes (nearly always), their messageDigest must equal it, and the
 *     signature is over the attributes' DER with their [0] tag made a SET;
 *     without, the signature is over the hash itself.
 *  2. signature: the signer's certificate (found by issuer and serial, or
 *     subject key identifier) verifies it: RSA PKCS#1 v1.5, RSA-PSS or ECDSA.
 *  3. certificate: the signer's certificate, through the other certificates
 *     in the signature, must lead to a trusted root: the bundle given with
 *     folio_set_trust_roots (on RISC OS, CaCertificates'), and Adobe's root.
 * mbedTLS's own PKCS#7 module doesn't handle signed attributes, so the CMS
 * structure is walked here with its ASN.1 functions. */
#include "folio_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbedtls/asn1.h"
#include "mbedtls/oid.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/platform_time.h"

#include "adobe_ca.h"

/* mbedTLS asks the platform for a millisecond clock (it can't tell how on
 * RISC OS); only used for timing, never for certificate dates. */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
	return (mbedtls_ms_time_t)time(NULL) * 1000;
}

/* ---- The trusted roots ------------------------------------------------------ */

static mbedtls_x509_crt trust;
static int trust_ready, trust_count;

static void certs_from_p7(mbedtls_x509_crt *chain, const unsigned char *p7, size_t len);

static void trust_init(void)
{
	if (trust_ready)
		return;
	trust_ready = 1;
	mbedtls_x509_crt_init(&trust);
	certs_from_p7(&trust, adobe_ca_p7c, sizeof adobe_ca_p7c);
}

int folio_set_trust_roots(folio_ctx *fc, const unsigned char *data, size_t len)
{
	unsigned char *z;
	int before = 0, after = 0, rc;
	mbedtls_x509_crt *c;
	(void)fc;
	trust_init();
	for (c = &trust; c && c->raw.len; c = c->next)
		before++;
	/* PEM wants a terminating NUL in its length. */
	z = malloc(len + 1);
	if (!z)
		return -1;
	memcpy(z, data, len);
	z[len] = 0;
	rc = mbedtls_x509_crt_parse(&trust, z, len + (len && data[0] == '-' ? 1 : 0));
	free(z);
	for (c = &trust; c && c->raw.len; c = c->next)
		after++;
	trust_count = after;
	if (rc < 0 && after == before)
		return -1;
	return after - before;
}

/* ---- CMS ------------------------------------------------------------------- */

typedef struct {
	const unsigned char *econtent;  /* encapsulated content (adbe.pkcs7.sha1), or NULL */
	size_t econtent_len;
	const unsigned char *certs;     /* [0] IMPLICIT SET OF Certificate: its contents */
	size_t certs_len;
	/* The (first) signer. */
	const unsigned char *sid_issuer, *sid_serial, *sid_ski;
	size_t sid_issuer_len, sid_serial_len, sid_ski_len;
	mbedtls_md_type_t md;
	const unsigned char *attrs;     /* signed attributes, from the [0] tag */
	size_t attrs_len;
	mbedtls_asn1_buf sig_oid, sig_params;
	const unsigned char *sig;
	size_t sig_len;
} cms;

static const unsigned char OID_SIGNED_DATA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02 };
static const unsigned char OID_MESSAGE_DIGEST[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x04 };
static const unsigned char OID_RSA_PSS[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0a };

static int oid_is(const mbedtls_asn1_buf *o, const unsigned char *want, size_t n)
{
	return o->len == n && !memcmp(o->p, want, n);
}

/* Reads one element; p moves past it. Its contents in *c, *clen. */
static int element(unsigned char **p, const unsigned char *end, int tag, const unsigned char **c, size_t *clen)
{
	size_t len;
	if (mbedtls_asn1_get_tag(p, end, &len, tag))
		return -1;
	*c = *p;
	*clen = len;
	*p += len;
	return 0;
}

static int parse_cms(const unsigned char *der, size_t der_len, cms *m)
{
	unsigned char *p = (unsigned char *)der, *end, *sd_end, *si_end;
	size_t len;
	mbedtls_asn1_buf oid;
	int v;
	memset(m, 0, sizeof *m);
	/* /Contents is padded with zeros: the first element is the whole of it. */
	if (mbedtls_asn1_get_tag(&p, der + der_len, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
		return -1;
	end = p + len;
	if (mbedtls_asn1_get_tag(&p, end, &oid.len, MBEDTLS_ASN1_OID))
		return -1;
	oid.p = p;
	p += oid.len;
	if (!oid_is(&oid, OID_SIGNED_DATA, sizeof OID_SIGNED_DATA))
		return -1;
	if (mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0) ||
			mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
		return -1;
	sd_end = p + len;
	if (mbedtls_asn1_get_int(&p, sd_end, &v))
		return -1;
	/* digestAlgorithms: skipped (the signer's own is used). */
	if (mbedtls_asn1_get_tag(&p, sd_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET))
		return -1;
	p += len;
	/* encapContentInfo: content type, and the content if it's carried. */
	{
		unsigned char *e;
		if (mbedtls_asn1_get_tag(&p, sd_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
			return -1;
		e = p + len;
		if (mbedtls_asn1_get_tag(&p, e, &len, MBEDTLS_ASN1_OID))
			return -1;
		p += len;
		if (p < e && !mbedtls_asn1_get_tag(&p, e, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0) &&
				!mbedtls_asn1_get_tag(&p, e, &len, MBEDTLS_ASN1_OCTET_STRING)) {
			m->econtent = p;
			m->econtent_len = len;
		}
		p = e;
	}
	/* certificates [0], crls [1] */
	if (p < sd_end && *p == (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0)) {
		if (element(&p, sd_end, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0, &m->certs, &m->certs_len))
			return -1;
	}
	if (p < sd_end && *p == (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 1)) {
		const unsigned char *c;
		size_t cl;
		if (element(&p, sd_end, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 1, &c, &cl))
			return -1;
	}
	/* signerInfos: the first. */
	if (mbedtls_asn1_get_tag(&p, sd_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET) ||
			mbedtls_asn1_get_tag(&p, sd_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
		return -1;
	si_end = p + len;
	if (mbedtls_asn1_get_int(&p, si_end, &v))
		return -1;
	if (*p == (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) {
		/* IssuerAndSerialNumber */
		unsigned char *ias;
		mbedtls_asn1_get_tag(&p, si_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
		ias = p + len;
		m->sid_issuer = p;
		if (mbedtls_asn1_get_tag(&p, ias, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
			return -1;
		m->sid_issuer_len = (size_t)(p + len - m->sid_issuer);
		p += len;
		if (element(&p, ias, MBEDTLS_ASN1_INTEGER, &m->sid_serial, &m->sid_serial_len))
			return -1;
		p = ias;
	} else if (element(&p, si_end, MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0, &m->sid_ski, &m->sid_ski_len))
		return -1;
	{
		mbedtls_asn1_buf alg, params;
		if (mbedtls_asn1_get_alg(&p, si_end, &alg, &params) || mbedtls_oid_get_md_alg(&alg, &m->md))
			return -1;
	}
	if (p < si_end && *p == (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0)) {
		m->attrs = p;
		if (mbedtls_asn1_get_tag(&p, si_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0))
			return -1;
		p += len;
		m->attrs_len = (size_t)(p - m->attrs);
	}
	if (mbedtls_asn1_get_alg(&p, si_end, &m->sig_oid, &m->sig_params))
		return -1;
	if (element(&p, si_end, MBEDTLS_ASN1_OCTET_STRING, &m->sig, &m->sig_len))
		return -1;
	return 0;
}

/* The certificates in a CMS blob (or a .p7c), parsed onto chain. */
static void certs_from_cms(mbedtls_x509_crt *chain, const cms *m)
{
	unsigned char *p = (unsigned char *)m->certs, *end = (unsigned char *)m->certs + m->certs_len;
	while (p < end) {
		unsigned char *start = p;
		size_t len;
		if (mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
			break;
		p += len;
		mbedtls_x509_crt_parse_der(chain, start, (size_t)(p - start));
	}
}

static void certs_from_p7(mbedtls_x509_crt *chain, const unsigned char *p7, size_t len)
{
	cms m;
	unsigned char *p = (unsigned char *)p7, *end, *sd_end;
	size_t l;
	int v;
	/* A .p7c has no signer, so parse_cms stops early: walk to the certs. */
	memset(&m, 0, sizeof m);
	if (mbedtls_asn1_get_tag(&p, p7 + len, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
		return;
	end = p + l;
	if (mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_OID))
		return;
	p += l;
	if (mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0) ||
			mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
		return;
	sd_end = p + l;
	if (mbedtls_asn1_get_int(&p, sd_end, &v) ||
			mbedtls_asn1_get_tag(&p, sd_end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET))
		return;
	p += l;
	if (mbedtls_asn1_get_tag(&p, sd_end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
		return;
	p += l;
	if (p < sd_end && *p == (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0) &&
			!element(&p, sd_end, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0, &m.certs, &m.certs_len))
		certs_from_cms(chain, &m);
}

/* The signer's certificate among those in the signature. */
static mbedtls_x509_crt *find_signer(mbedtls_x509_crt *chain, const cms *m)
{
	mbedtls_x509_crt *c;
	for (c = chain; c && c->raw.len; c = c->next) {
		if (m->sid_issuer) {
			/* The serial's DER contents may carry a leading zero. */
			const unsigned char *s = m->sid_serial;
			size_t sl = m->sid_serial_len, cl = c->serial.len;
			const unsigned char *cs = c->serial.p;
			while (sl > 1 && *s == 0) { s++; sl--; }
			while (cl > 1 && *cs == 0) { cs++; cl--; }
			if (c->issuer_raw.len == m->sid_issuer_len && !memcmp(c->issuer_raw.p, m->sid_issuer, m->sid_issuer_len) &&
					sl == cl && !memcmp(s, cs, sl))
				return c;
		} else if (m->sid_ski && c->subject_key_id.len == m->sid_ski_len &&
				!memcmp(c->subject_key_id.p, m->sid_ski, m->sid_ski_len)) {
			return c;
		}
	}
	return NULL;
}

/* The messageDigest attribute's value, from the signed attributes. */
static int message_digest(const cms *m, const unsigned char **d, size_t *dlen)
{
	unsigned char *p = (unsigned char *)m->attrs, *end = (unsigned char *)m->attrs + m->attrs_len;
	size_t len;
	if (mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0))
		return -1;
	while (p < end) {
		unsigned char *a_end;
		mbedtls_asn1_buf oid;
		if (mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
			return -1;
		a_end = p + len;
		if (mbedtls_asn1_get_tag(&p, a_end, &oid.len, MBEDTLS_ASN1_OID))
			return -1;
		oid.p = p;
		p += oid.len;
		if (oid_is(&oid, OID_MESSAGE_DIGEST, sizeof OID_MESSAGE_DIGEST)) {
			if (mbedtls_asn1_get_tag(&p, a_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET) ||
					mbedtls_asn1_get_tag(&p, a_end, &len, MBEDTLS_ASN1_OCTET_STRING))
				return -1;
			*d = p;
			*dlen = len;
			return 0;
		}
		p = a_end;
	}
	return -1;
}

/* RSASSA-PSS parameters: the hash and the salt length (defaults SHA-1, 20). */
static void pss_params(const mbedtls_asn1_buf *params, mbedtls_md_type_t *md, int *salt)
{
	unsigned char *p = params->p, *end = params->p + params->len;
	size_t len;
	*md = MBEDTLS_MD_SHA1;
	*salt = 20;
	if (!params->p || params->tag != (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
		return;
	while (p < end) {
		int tag = *p;
		if (mbedtls_asn1_get_tag(&p, end, &len, tag))
			return;
		if (tag == (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0)) {
			unsigned char *q = p;
			mbedtls_asn1_buf alg, prm;
			if (!mbedtls_asn1_get_alg(&q, p + len, &alg, &prm))
				mbedtls_oid_get_md_alg(&alg, md);
		} else if (tag == (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 2)) {
			unsigned char *q = p;
			mbedtls_asn1_get_int(&q, p + len, salt);
		}
		p += len;
	}
}

/* ---- What a check finds ------------------------------------------------------ */

typedef struct {
	pdf_pkcs7_verifier base;
	/* The last check's findings, for folio_signatures. */
	int digest_ok, sig_ok, have_cert;
	uint32_t flags;                 /* mbedTLS's verify flags from the chain check */
	int self_signed;
	char why[200];
} folio_verifier;

static int hash_stream(fz_context *ctx, fz_stream *in, mbedtls_md_type_t md, unsigned char *out)
{
	const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md);
	mbedtls_md_context_t c;
	unsigned char buf[16384];
	size_t n;
	int rc;
	if (!info)
		return -1;
	mbedtls_md_init(&c);
	rc = mbedtls_md_setup(&c, info, 0) || mbedtls_md_starts(&c);
	while (!rc && (n = fz_read(ctx, in, buf, sizeof buf)) > 0)
		rc = mbedtls_md_update(&c, buf, n);
	if (!rc)
		rc = mbedtls_md_finish(&c, out);
	mbedtls_md_free(&c);
	return rc ? -1 : 0;
}

static pdf_signature_error check_digest(fz_context *ctx, pdf_pkcs7_verifier *v, fz_stream *in,
		unsigned char *sig, size_t sig_len)
{
	folio_verifier *fv = (folio_verifier *)v;
	cms m;
	mbedtls_x509_crt chain, *signer;
	unsigned char h[MBEDTLS_MD_MAX_SIZE], hs[MBEDTLS_MD_MAX_SIZE];
	const unsigned char *md_val;
	size_t md_len, hl;
	pdf_signature_error r = PDF_SIGNATURE_ERROR_DIGEST_FAILURE;
	fv->digest_ok = fv->sig_ok = fv->have_cert = 0;
	fv->why[0] = 0;
	if (parse_cms(sig, sig_len, &m)) {
		snprintf(fv->why, sizeof fv->why, "the signature can't be read");
		return PDF_SIGNATURE_ERROR_UNKNOWN;
	}
	hl = mbedtls_md_get_size(mbedtls_md_info_from_type(m.md));
	mbedtls_x509_crt_init(&chain);
	certs_from_cms(&chain, &m);
	signer = find_signer(&chain, &m);
	fv->have_cert = signer != NULL;
	if (!signer) {
		snprintf(fv->why, sizeof fv->why, "the signer's certificate isn't in the signature");
		mbedtls_x509_crt_free(&chain);
		return PDF_SIGNATURE_ERROR_NO_CERTIFICATE;
	}
	/* 1. The document's bytes. */
	if (m.econtent) {
		/* adbe.pkcs7.sha1: the content is the bytes' SHA-1. */
		if (hash_stream(ctx, in, MBEDTLS_MD_SHA1, h) || m.econtent_len != 20 || memcmp(h, m.econtent, 20)) {
			snprintf(fv->why, sizeof fv->why, "the document has been changed since it was signed");
			goto done;
		}
		mbedtls_md(mbedtls_md_info_from_type(m.md), m.econtent, m.econtent_len, h);
	} else if (hash_stream(ctx, in, m.md, h)) {
		snprintf(fv->why, sizeof fv->why, "its digest algorithm isn't one Folio knows");
		r = PDF_SIGNATURE_ERROR_UNKNOWN;
		goto done;
	}
	/* 2. What was signed: the attributes (whose messageDigest must be h),
	 * or h itself. */
	if (m.attrs) {
		unsigned char *der;
		if (message_digest(&m, &md_val, &md_len) || md_len != hl || memcmp(md_val, h, hl)) {
			snprintf(fv->why, sizeof fv->why, "the document has been changed since it was signed");
			goto done;
		}
		der = malloc(m.attrs_len);
		if (!der)
			goto done;
		memcpy(der, m.attrs, m.attrs_len);
		der[0] = MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET;
		mbedtls_md(mbedtls_md_info_from_type(m.md), der, m.attrs_len, hs);
		free(der);
	} else {
		memcpy(hs, h, hl);
	}
	fv->digest_ok = 1;
	/* 3. The signature over it. */
	{
		int rc;
		if (oid_is(&m.sig_oid, OID_RSA_PSS, sizeof OID_RSA_PSS)) {
			mbedtls_pk_rsassa_pss_options o;
			mbedtls_md_type_t pmd;
			int salt;
			pss_params(&m.sig_params, &pmd, &salt);
			o.mgf1_hash_id = pmd;
			o.expected_salt_len = salt;
			if (pmd != m.md) {
				/* PSS hashing with another algorithm than the digest's: not
				 * something signing programs do. */
				snprintf(fv->why, sizeof fv->why, "its kind of RSA-PSS signature isn't one Folio knows");
				fv->digest_ok = 0;
				r = PDF_SIGNATURE_ERROR_UNKNOWN;
				goto done;
			}
			rc = mbedtls_pk_verify_ext(MBEDTLS_PK_RSASSA_PSS, &o, &signer->pk, pmd, hs, hl, m.sig, m.sig_len);
		} else {
			rc = mbedtls_pk_verify(&signer->pk, m.md, hs, hl, m.sig, m.sig_len);
		}
		if (rc) {
			snprintf(fv->why, sizeof fv->why, "the signature doesn't match the signer's certificate");
			fv->digest_ok = 0;
			goto done;
		}
	}
	fv->sig_ok = 1;
	r = PDF_SIGNATURE_ERROR_OKAY;
done:
	mbedtls_x509_crt_free(&chain);
	return r;
}

/* A tolerant profile: old signatures (SHA-1 in the chain, 1024-bit RSA)
 * are still read as what they are; the date is checked separately. */
static const mbedtls_x509_crt_profile tolerant = {
	0xffffff,       /* any digest */
	0xffffff,       /* any public key kind */
	0xffffff,       /* any curve */
	1024,           /* RSA bits at least */
};

static int is_self_signed(const mbedtls_x509_crt *c)
{
	return c->issuer_raw.len == c->subject_raw.len && !memcmp(c->issuer_raw.p, c->subject_raw.p, c->issuer_raw.len);
}

static pdf_signature_error check_certificate(fz_context *ctx, pdf_pkcs7_verifier *v, unsigned char *sig, size_t sig_len)
{
	folio_verifier *fv = (folio_verifier *)v;
	cms m;
	mbedtls_x509_crt all, chain, *signer, *c;
	uint32_t flags = 0;
	pdf_signature_error r;
	(void)ctx;
	fv->flags = 0;
	fv->self_signed = 0;
	trust_init();
	if (parse_cms(sig, sig_len, &m))
		return PDF_SIGNATURE_ERROR_UNKNOWN;
	mbedtls_x509_crt_init(&all);
	mbedtls_x509_crt_init(&chain);
	certs_from_cms(&all, &m);
	signer = find_signer(&all, &m);
	if (!signer) {
		mbedtls_x509_crt_free(&all);
		return PDF_SIGNATURE_ERROR_NO_CERTIFICATE;
	}
	/* The signer first, then the rest (mbedTLS takes the first as the one
	 * to check, and the rest as the way to a root). */
	mbedtls_x509_crt_parse_der(&chain, signer->raw.p, signer->raw.len);
	for (c = &all; c && c->raw.len; c = c->next)
		if (c != signer)
			mbedtls_x509_crt_parse_der(&chain, c->raw.p, c->raw.len);
	fv->self_signed = is_self_signed(signer);
	mbedtls_x509_crt_verify_with_profile(&chain, &trust, NULL, &tolerant, NULL, &flags, NULL, NULL);
	fv->flags = flags;
	/* Out of date isn't untrustworthy: the signature may be older. */
	flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE |
		MBEDTLS_X509_BADCRL_EXPIRED | MBEDTLS_X509_BADCRL_FUTURE);
	if (!flags)
		r = PDF_SIGNATURE_ERROR_OKAY;
	else if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
		r = fv->self_signed ? PDF_SIGNATURE_ERROR_SELF_SIGNED : PDF_SIGNATURE_ERROR_NOT_TRUSTED;
	else
		r = PDF_SIGNATURE_ERROR_UNKNOWN;
	mbedtls_x509_crt_free(&chain);
	mbedtls_x509_crt_free(&all);
	return r;
}

/* A name's text as UTF-8 (BMPString is UCS-2; the rest are ASCII/UTF-8). */
static char *name_text(fz_context *ctx, const mbedtls_x509_buf *v)
{
	if (v->tag == MBEDTLS_ASN1_BMP_STRING) {
		char *s = fz_malloc(ctx, v->len * 2 + 1), *d = s;
		size_t i;
		for (i = 0; i + 1 < v->len; i += 2)
			d += fz_runetochar(d, (v->p[i] << 8) | v->p[i + 1]);
		*d = 0;
		return s;
	} else {
		char *s = fz_malloc(ctx, v->len + 1);
		memcpy(s, v->p, v->len);
		s[v->len] = 0;
		return s;
	}
}

static pdf_pkcs7_distinguished_name *dn_of(fz_context *ctx, const mbedtls_x509_crt *c)
{
	pdf_pkcs7_distinguished_name *dn = fz_malloc_struct(ctx, pdf_pkcs7_distinguished_name);
	const mbedtls_x509_name *n;
	for (n = &c->subject; n; n = n->next) {
		char **slot = NULL;
		if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &n->oid) == 0) slot = &dn->cn;
		else if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_ORGANIZATION, &n->oid) == 0) slot = &dn->o;
		else if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_ORG_UNIT, &n->oid) == 0) slot = &dn->ou;
		else if (MBEDTLS_OID_CMP(MBEDTLS_OID_PKCS9_EMAIL, &n->oid) == 0) slot = &dn->email;
		else if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_COUNTRY, &n->oid) == 0) slot = &dn->c;
		if (slot && !*slot)
			*slot = name_text(ctx, &n->val);
	}
	return dn;
}

static pdf_pkcs7_distinguished_name *get_signatory(fz_context *ctx, pdf_pkcs7_verifier *v, unsigned char *sig, size_t sig_len)
{
	cms m;
	mbedtls_x509_crt all, *signer;
	pdf_pkcs7_distinguished_name *dn = NULL;
	(void)v;
	fz_var(dn);
	if (parse_cms(sig, sig_len, &m))
		return NULL;
	mbedtls_x509_crt_init(&all);
	certs_from_cms(&all, &m);
	signer = find_signer(&all, &m);
	fz_try(ctx) {
		if (signer)
			dn = dn_of(ctx, signer);
	}
	fz_always(ctx)
		mbedtls_x509_crt_free(&all);
	fz_catch(ctx)
		fz_rethrow(ctx);
	return dn;
}

static void drop_verifier(fz_context *ctx, pdf_pkcs7_verifier *v)
{
	fz_free(ctx, v);
}

static folio_verifier *new_verifier(fz_context *ctx)
{
	folio_verifier *v = fz_malloc_struct(ctx, folio_verifier);
	v->base.drop = drop_verifier;
	v->base.check_certificate = check_certificate;
	v->base.check_digest = check_digest;
	v->base.get_signatory = get_signatory;
	return v;
}

/* ---- The document's signatures ---------------------------------------------- */

static void text_of(fz_context *ctx, pdf_obj *dict, pdf_obj *key, char *out, size_t size)
{
	const char *s = pdf_dict_get_text_string(ctx, dict, key);
	snprintf(out, size, "%s", s ? s : "");
}

/* One signature field: checked, and its details filled in. */
/* ---- What was changed after a signature, and whether it was allowed --------
 * As Acrobat judges it: the updates saved after the signature are looked at
 * object by object (each xref section after the one holding the signature
 * value), and each change classed as filling in a field, signing, a note or
 * marking, a change to a page, or something else. A certification (DocMDP)
 * allows: P 1 nothing; P 2 filling in and signing; P 3 those and notes.
 * Fields the signature locks (FieldMDP, /Lock) mustn't change at all. With
 * neither, an ordinary signature allows any later changes: they're listed,
 * and the signature is still good for what was signed. */

enum { AF_FORM = 1, AF_SIGNING = 2, AF_ANNOT = 4, AF_PAGE = 8, AF_OTHER = 16, AF_LOCKED = 32 };

static int same_except(fz_context *ctx, pdf_obj *a, pdf_obj *b, const char *const *keep)
{
	int pass, i;
	for (pass = 0; pass < 2; pass++) {
		pdf_obj *x = pass ? b : a, *y = pass ? a : b;
		int n = pdf_dict_len(ctx, x);
		for (i = 0; i < n; i++) {
			pdf_obj *k = pdf_dict_get_key(ctx, x, i);
			const char *const *kk;
			int skip = 0;
			for (kk = keep; *kk; kk++)
				if (!strcmp(pdf_to_name(ctx, k), *kk))
					skip = 1;
			if (!skip && pdf_objcmp(ctx, pdf_dict_get(ctx, x, k), pdf_dict_get(ctx, y, k)))
				return 0;
		}
	}
	return 1;
}

static int in_array(fz_context *ctx, pdf_obj *arr, pdf_obj *o)
{
	int i, n = pdf_array_len(ctx, arr);
	for (i = 0; i < n; i++)
		if (!pdf_objcmp(ctx, pdf_array_get(ctx, arr, i), o))
			return 1;
	return 0;
}

static int is_widget(fz_context *ctx, pdf_obj *o)
{
	return pdf_name_eq(ctx, pdf_dict_get(ctx, o, PDF_NAME(Subtype)), PDF_NAME(Widget));
}

/* One changed object: newer at base j, older at base j + 1. */
static int classify(fz_context *ctx, pdf_document *pdf, int num, int j, pdf_locked_fields *locked)
{
	static const char *const cat_keep[] = { "AcroForm", "Metadata", "Perms", "OCProperties", NULL };
	static const char *const af_keep[] = { "Fields", "DR", "DA", "SigFlags", "NeedAppearances", "XFA", NULL };
	static const char *const page_keep[] = { "Annots", NULL };
	pdf_obj *nw = NULL, *od = NULL, *trailer, *type;
	int r = 0;
	fz_var(r);
	fz_var(nw);
	fz_var(od);
	fz_try(ctx) {
		pdf->xref_base = j + 1;
		od = pdf_load_object(ctx, pdf, num);
		pdf->xref_base = j;
		nw = pdf_load_object(ctx, pdf, num);
		trailer = pdf_trailer(ctx, pdf);
		type = pdf_dict_get(ctx, nw, PDF_NAME(Type));
		if (pdf_is_null(ctx, od) || !pdf_objcmp(ctx, od, nw))
			r = 0;              /* new (or the same again) */
		else if (num == pdf_to_num(ctx, pdf_dict_get(ctx, trailer, PDF_NAME(Info))) ||
				num == pdf_to_num(ctx, pdf_dict_getp(ctx, trailer, "Root/Metadata")) ||
				num == pdf_to_num(ctx, pdf_dict_get(ctx, trailer, PDF_NAME(Encrypt))) ||
				pdf_name_eq(ctx, type, PDF_NAME(ObjStm)) || pdf_name_eq(ctx, type, PDF_NAME(XRef)) ||
				pdf_name_eq(ctx, type, PDF_NAME(Metadata)))
			r = 0;
		else if (pdf_name_eq(ctx, type, PDF_NAME(Catalog)))
			r = same_except(ctx, od, nw, cat_keep) ? 0 : AF_OTHER;
		else if (num == pdf_to_num(ctx, pdf_dict_getp(ctx, trailer, "Root/AcroForm")))
			r = same_except(ctx, od, nw, af_keep) ? 0 : AF_OTHER;
		else if (is_widget(ctx, nw) || pdf_dict_get_inheritable(ctx, nw, PDF_NAME(FT))) {
			pdf_obj *nv = pdf_dict_get_inheritable(ctx, nw, PDF_NAME(V));
			pdf_obj *ov;
			pdf->xref_base = j + 1;
			ov = pdf_dict_get_inheritable(ctx, od, PDF_NAME(V));
			pdf->xref_base = j;
			if (pdf_objcmp(ctx, nv, ov)) {
				char *name = pdf_load_field_name(ctx, nw);
				int sig = pdf_name_eq(ctx, pdf_dict_get_inheritable(ctx, nw, PDF_NAME(FT)), PDF_NAME(Sig));
				r = sig && !ov ? AF_SIGNING : AF_FORM;
				if (locked && name && pdf_is_field_locked(ctx, locked, name))
					r |= AF_LOCKED;
				fz_free(ctx, name);
			}
		} else if (pdf_name_eq(ctx, type, PDF_NAME(Annot)) || (pdf_dict_get(ctx, nw, PDF_NAME(Subtype)) &&
				pdf_dict_get(ctx, nw, PDF_NAME(Rect))))
			r = AF_ANNOT;
		else if (pdf_name_eq(ctx, type, PDF_NAME(Page))) {
			pdf_obj *na = pdf_dict_get(ctx, nw, PDF_NAME(Annots)), *oa;
			int i;
			r = same_except(ctx, od, nw, page_keep) ? 0 : AF_PAGE;
			pdf->xref_base = j + 1;
			oa = pdf_dict_get(ctx, od, PDF_NAME(Annots));
			for (i = 0; i < pdf_array_len(ctx, oa); i++)
				if (!in_array(ctx, na, pdf_array_get(ctx, oa, i)))
					r |= is_widget(ctx, pdf_array_get(ctx, oa, i)) ? AF_PAGE : AF_ANNOT;
			pdf->xref_base = j;
			for (i = 0; i < pdf_array_len(ctx, na); i++)
				if (!in_array(ctx, oa, pdf_array_get(ctx, na, i)) && !is_widget(ctx, pdf_array_get(ctx, na, i)))
					r |= AF_ANNOT;
		} else if (pdf_name_eq(ctx, type, PDF_NAME(XObject)) || pdf_name_eq(ctx, type, PDF_NAME(Font)) ||
				pdf_name_eq(ctx, type, PDF_NAME(FontDescriptor)) || pdf_name_eq(ctx, type, PDF_NAME(Encoding)))
			r = 0;              /* appearances and their fonts */
		else
			r = AF_OTHER;       /* the page tree, page content, anything else */
	}
	fz_always(ctx) {
		pdf_drop_obj(ctx, nw);
		pdf_drop_obj(ctx, od);
	}
	fz_catch(ctx)
		r = AF_OTHER;
	return r;
}

/* The certification's P (0: not certified), from Perms/DocMDP. */
static int docmdp_p(fz_context *ctx, pdf_document *pdf)
{
	pdf_obj *refs = pdf_dict_getp(ctx, pdf_trailer(ctx, pdf), "Root/Perms/DocMDP/Reference");
	int k;
	for (k = 0; k < pdf_array_len(ctx, refs); k++) {
		pdf_obj *r = pdf_array_get(ctx, refs, k);
		if (pdf_name_eq(ctx, pdf_dict_get(ctx, r, PDF_NAME(TransformMethod)), PDF_NAME(DocMDP))) {
			pdf_obj *p = pdf_dict_getp(ctx, r, "TransformParams/P");
			int v = p ? pdf_to_int(ctx, p) : 2;
			return v >= 1 && v <= 3 ? v : 2;
		}
	}
	return 0;
}

static void judge_changes(fz_context *ctx, pdf_document *pdf, pdf_obj *field, folio_signature *s)
{
	pdf_obj *val = pdf_dict_get_inheritable(ctx, field, PDF_NAME(V));
	pdf_locked_fields *locked = NULL;
	int sv, j, what = 0, p, lockp, base;
	fz_var(locked);
	fz_var(what);
	sv = pdf_find_version_for_obj(ctx, pdf, val);
	if (sv <= pdf->num_incremental_sections)
		return;                 /* nothing saved after it */
	p = docmdp_p(ctx, pdf);
	lockp = pdf_dict_get_int(ctx, pdf_dict_get(ctx, field, PDF_NAME(Lock)), PDF_NAME(P));
	if (lockp >= 1 && lockp <= 3 && (!p || lockp < p))
		p = lockp;          /* "lock the document" (PDF 2.0) */
	pdf_start_throw_on_repair(ctx, pdf, &base);
	fz_try(ctx) {
		locked = pdf_find_locked_fields_for_sig(ctx, pdf, field);
		for (j = pdf->num_incremental_sections; j < sv && j + 1 < pdf->num_xref_sections; j++) {
			pdf_xref_subsec *sub;
			for (sub = pdf->xref_sections[j].subsec; sub; sub = sub->next) {
				int k;
				for (k = 0; k < sub->len; k++) {
					char t = sub->table[k].type;
					if (t == 'n' || t == 'o')
						what |= classify(ctx, pdf, sub->start + k, j, locked);
				}
			}
		}
	}
	fz_always(ctx) {
		pdf_drop_locked_fields(ctx, locked);
		pdf_end_throw_on_repair(ctx, pdf, base);
	}
	fz_catch(ctx) {
		fz_report_error(ctx);
		return;
	}
	s->changes = what & ~AF_LOCKED;
	if (what & AF_LOCKED)
		snprintf(s->disallowed, sizeof s->disallowed, "a field it locks has been changed");
	else if (p == 1 && what)
		snprintf(s->disallowed, sizeof s->disallowed, "the PDF was certified with no changes allowed, "
			"and it has been changed");
	else if (p && (what & (AF_PAGE | AF_OTHER)))
		snprintf(s->disallowed, sizeof s->disallowed, "the pages have been changed, which the "
			"certification doesn't allow");
	else if (p == 2 && (what & AF_ANNOT))
		snprintf(s->disallowed, sizeof s->disallowed, "notes or markings have been added, which the "
			"certification doesn't allow");
	s->certified = p;
}

static void one_field(fz_context *ctx, pdf_document *pdf, folio_verifier *v, pdf_obj *field, folio_signature *s)
{
	pdf_obj *val, *pg;
	char *name;
	memset(s, 0, sizeof *s);
	s->page = -1;
	name = pdf_load_field_name(ctx, field);
	snprintf(s->field, sizeof s->field, "%s", name ? name : "");
	fz_free(ctx, name);
	/* Its page and box (the field is usually its own widget). */
	pg = pdf_dict_get(ctx, field, PDF_NAME(P));
	if (!pg)
		pg = pdf_dict_get(ctx, pdf_array_get(ctx, pdf_dict_get(ctx, field, PDF_NAME(Kids)), 0), PDF_NAME(P));
	fz_try(ctx) {
		if (pg)
			s->page = pdf_lookup_page_number(ctx, pdf, pg);
		if (s->page >= 0) {
			fz_rect r = pdf_dict_get_rect(ctx, field, PDF_NAME(Rect)), mb;
			fz_matrix ctm;
			pdf_obj *page = pdf_lookup_page_obj(ctx, pdf, s->page);
			pdf_page_obj_transform(ctx, page, &mb, &ctm);
			r = fz_transform_rect(r, ctm);
			s->rect.x0 = r.x0;
			s->rect.y0 = r.y0;
			s->rect.x1 = r.x1;
			s->rect.y1 = r.y1;
		}
	}
	fz_catch(ctx)
		s->page = -1;
	s->is_signed = pdf_signature_is_signed(ctx, pdf, field);
	if (!s->is_signed)
		return;
	val = pdf_dict_get_inheritable(ctx, field, PDF_NAME(V));
	text_of(ctx, val, PDF_NAME(M), s->date, sizeof s->date);
	text_of(ctx, val, PDF_NAME(Reason), s->reason, sizeof s->reason);
	text_of(ctx, val, PDF_NAME(Location), s->location, sizeof s->location);
	text_of(ctx, val, PDF_NAME(Name), s->signer, sizeof s->signer);
	fz_try(ctx) {
		pdf_signature_error d = pdf_check_digest(ctx, &v->base, pdf, field);
		s->digest = d == PDF_SIGNATURE_ERROR_OKAY ? FOLIO_SIG_VALID :
			d == PDF_SIGNATURE_ERROR_DIGEST_FAILURE ? FOLIO_SIG_CHANGED : FOLIO_SIG_UNREADABLE;
		snprintf(s->why, sizeof s->why, "%s", v->why);
	}
	fz_catch(ctx) {
		s->digest = FOLIO_SIG_UNREADABLE;
		snprintf(s->why, sizeof s->why, "%s", fz_caught_message(ctx));
	}
	fz_try(ctx) {
		pdf_signature_error c = pdf_check_certificate(ctx, &v->base, pdf, field);
		s->cert = c == PDF_SIGNATURE_ERROR_OKAY ? FOLIO_CERT_TRUSTED :
			c == PDF_SIGNATURE_ERROR_SELF_SIGNED ? FOLIO_CERT_SELF_SIGNED :
			c == PDF_SIGNATURE_ERROR_NOT_TRUSTED ? FOLIO_CERT_UNTRUSTED :
			c == PDF_SIGNATURE_ERROR_NO_CERTIFICATE ? FOLIO_CERT_NONE : FOLIO_CERT_OTHER;
		s->expired = (v->flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) != 0;
		if (s->cert == FOLIO_CERT_OTHER)
			mbedtls_x509_crt_verify_info(s->cert_why, sizeof s->cert_why, "", v->flags & ~(uint32_t)
				(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE));
	}
	fz_catch(ctx)
		s->cert = FOLIO_CERT_OTHER;
	fz_try(ctx) {
		pdf_pkcs7_distinguished_name *dn = pdf_signature_get_signatory(ctx, &v->base, pdf, field);
		if (dn) {
			if (dn->cn)
				snprintf(s->signer, sizeof s->signer, "%s", dn->cn);
			snprintf(s->organisation, sizeof s->organisation, "%s", dn->o ? dn->o : "");
			snprintf(s->email, sizeof s->email, "%s", dn->email ? dn->email : "");
			pdf_signature_drop_distinguished_name(ctx, dn);
		}
	}
	fz_catch(ctx) {
		/* the name from /Name, if any */
	}
	fz_try(ctx)
		s->changed_after = pdf_signature_incremental_change_since_signing(ctx, pdf, field);
	fz_catch(ctx)
		s->changed_after = 0;
	if (s->changed_after)
		judge_changes(ctx, pdf, field, s);
	else
		s->certified = docmdp_p(ctx, pdf);
}

/* The form's fields, depth first, for signature fields (FT /Sig, which
 * a field may inherit). */
static void walk_fields(fz_context *ctx, pdf_document *pdf, folio_verifier *v, pdf_obj *fields, int depth,
		folio_signature *out, int max, int *count)
{
	int i, n = pdf_array_len(ctx, fields);
	if (depth > 16)
		return;
	for (i = 0; i < n && *count < max; i++) {
		pdf_obj *f = pdf_array_get(ctx, fields, i), *kids = pdf_dict_get(ctx, f, PDF_NAME(Kids));
		/* A field with kids that are fields (they have /T) is a branch. */
		if (pdf_is_array(ctx, kids) && pdf_dict_get(ctx, pdf_array_get(ctx, kids, 0), PDF_NAME(T)))
			walk_fields(ctx, pdf, v, kids, depth + 1, out, max, count);
		else if (pdf_name_eq(ctx, pdf_dict_get_inheritable(ctx, f, PDF_NAME(FT)), PDF_NAME(Sig)))
			one_field(ctx, pdf, v, f, &out[(*count)++]);
	}
}

int folio_signatures(folio_doc *doc, folio_signature *out, int max)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	folio_verifier *v = NULL;
	int count = 0;
	if (!pdf)
		return 0;
	fz_var(v);
	fz_var(count);
	folio_begin(doc);
	fz_try(ctx) {
		v = new_verifier(ctx);
		walk_fields(ctx, pdf, v, pdf_dict_getp(ctx, pdf_trailer(ctx, pdf), "Root/AcroForm/Fields"), 0,
			out, max, &count);
	}
	fz_always(ctx) {
		if (v)
			drop_verifier(ctx, &v->base);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return count;
}

int folio_has_signatures(folio_doc *doc)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	int n = 0;
	if (!pdf)
		return 0;
	fz_try(ctx)
		n = pdf_count_signatures(ctx, pdf);
	fz_catch(ctx)
		n = 0;
	return n;
}

int folio_trust_root_count(void)
{
	mbedtls_x509_crt *c;
	int n = 0;
	trust_init();
	for (c = &trust; c && c->raw.len; c = c->next)
		n++;
	return n;
}

/* ==== Signing ================================================================
 * A signer from a .p12/.pfx file (PKCS#12: the key and its certificates,
 * under a password), for MuPDF's pdf_pkcs7_signer: when the PDF is saved,
 * MuPDF hands create_digest the bytes to sign, and gets back CMS SignedData
 * (adbe.pkcs7.detached): SHA-256, signed attributes (content type, signing
 * time, message digest), the signature (RSA PKCS#1 v1.5, or ECDSA), and the
 * certificates. */

#include "mbedtls/asn1write.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/pkcs12.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdsa.h"

struct folio_signer {
	pdf_pkcs7_signer base;
	int refs;
	mbedtls_pk_context key;
	mbedtls_x509_crt chain;         /* the signer's certificate first */
	mbedtls_ctr_drbg_context drbg;
	int drbg_ready;
	fz_image *picture;              /* a handwritten signature to show, or NULL */
};

/* ---- Random numbers: /dev/urandom (on RISC OS, UnixLib reads CryptRandom). */

static int urandom(void *data, unsigned char *out, size_t len)
{
	FILE *f = fopen("/dev/urandom", "rb");
	size_t got = 0;
	(void)data;
	if (f) {
		got = fread(out, 1, len, f);
		fclose(f);
	}
	return got == len ? 0 : MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
}

/* ---- RC2 (RFC 2268), CBC, decryption only: old .p12 files encrypt their
 * certificates with 40-bit RC2, which mbedTLS no longer has. */

static const unsigned char rc2_pi[256] = {
	217,120,249,196,25,221,181,237,40,233,253,121,74,160,216,157,198,126,55,131,43,118,83,142,98,76,100,136,68,139,251,162,
	23,154,89,245,135,179,79,19,97,69,109,141,9,129,125,50,189,143,64,235,134,183,123,11,240,149,33,34,92,107,78,130,
	84,214,101,147,206,96,178,28,115,86,192,20,167,140,241,220,18,117,202,31,59,190,228,209,66,61,212,48,163,60,182,38,
	111,191,14,218,70,105,7,87,39,242,29,155,188,148,67,3,248,17,199,246,144,239,62,231,6,195,213,47,200,102,30,215,
	8,232,234,222,128,82,238,247,132,170,114,172,53,77,106,42,150,26,210,113,90,21,73,116,75,159,208,94,4,24,164,236,
	194,224,65,110,15,81,203,204,36,145,175,80,161,244,112,57,153,124,58,133,35,184,180,122,252,2,54,91,37,85,151,49,
	45,93,250,152,227,138,146,174,5,223,41,16,103,108,186,201,211,0,230,207,225,158,168,44,99,22,1,63,88,226,137,169,
	13,56,52,27,171,51,255,176,187,72,12,95,185,177,205,46,197,243,219,71,229,165,156,119,10,166,32,104,254,127,193,173
};

static void rc2_key(unsigned short K[64], const unsigned char *key, int len, int bits)
{
	unsigned char L[128];
	int i, T8 = (bits + 7) / 8;
	unsigned TM = 255 % (1u << (8 + bits - 8 * T8));
	memcpy(L, key, (size_t)len);
	for (i = len; i < 128; i++)
		L[i] = rc2_pi[(L[i - 1] + L[i - len]) & 255];
	L[128 - T8] = rc2_pi[L[128 - T8] & TM];
	for (i = 127 - T8; i >= 0; i--)
		L[i] = rc2_pi[L[i + 1] ^ L[i + T8]];
	for (i = 0; i < 64; i++)
		K[i] = (unsigned short)(L[2 * i] | (L[2 * i + 1] << 8));
}

static void rc2_decrypt_block(const unsigned short K[64], unsigned char b[8])
{
	unsigned short R[4];
	int i, j = 63;
	for (i = 0; i < 4; i++)
		R[i] = (unsigned short)(b[2 * i] | (b[2 * i + 1] << 8));
	for (i = 4; i >= 0; i--) {
		int r, n = i == 4 || i == 0 ? 5 : i == 1 || i == 3 ? 1 : 6;
		/* rounds: 5 mixing, mash, 6 mixing, mash, 5 mixing (backwards) */
		if (n == 1) {
			R[3] = (unsigned short)(R[3] - K[R[2] & 63]);
			R[2] = (unsigned short)(R[2] - K[R[1] & 63]);
			R[1] = (unsigned short)(R[1] - K[R[0] & 63]);
			R[0] = (unsigned short)(R[0] - K[R[3] & 63]);
			continue;
		}
		for (r = 0; r < n; r++) {
			R[3] = (unsigned short)(((R[3] << 11) | (R[3] >> 5)) - K[j--] - (R[2] & R[1]) - (~R[2] & R[0]));
			R[2] = (unsigned short)(((R[2] << 13) | (R[2] >> 3)) - K[j--] - (R[1] & R[0]) - (~R[1] & R[3]));
			R[1] = (unsigned short)(((R[1] << 14) | (R[1] >> 2)) - K[j--] - (R[0] & R[3]) - (~R[0] & R[2]));
			R[0] = (unsigned short)(((R[0] << 15) | (R[0] >> 1)) - K[j--] - (R[3] & R[2]) - (~R[3] & R[1]));
		}
	}
	for (i = 0; i < 4; i++) {
		b[2 * i] = (unsigned char)R[i];
		b[2 * i + 1] = (unsigned char)(R[i] >> 8);
	}
}

/* ---- PKCS#12 -------------------------------------------------------------- */

static const unsigned char OID_DATA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01 };
static const unsigned char OID_ENC_DATA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x06 };
static const unsigned char OID_KEY_BAG[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x0c, 0x0a, 0x01, 0x01 };
static const unsigned char OID_SHROUDED_BAG[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x0c, 0x0a, 0x01, 0x02 };
static const unsigned char OID_CERT_BAG[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x0c, 0x0a, 0x01, 0x03 };
static const unsigned char OID_PBES2[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x05, 0x0d };
static const unsigned char OID_PBE_3DES[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x0c, 0x01, 0x03 };
static const unsigned char OID_PBE_RC2_128[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x0c, 0x01, 0x05 };
static const unsigned char OID_PBE_RC2_40[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x0c, 0x01, 0x06 };

typedef struct {
	mbedtls_pk_context *key;
	mbedtls_x509_crt *certs;
	const char *pw;
	int have_key, wrong_pw, unknown_cipher;
	struct folio_signer *s;
} p12_state;

/* Decrypts EncryptedContentInfo-style data: alg (with params) and the
 * encrypted bytes. Returns a malloc'd plaintext, or NULL. */
static unsigned char *p12_decrypt(p12_state *st, const mbedtls_asn1_buf *alg, mbedtls_asn1_buf *params,
		const unsigned char *data, size_t len, size_t *out_len)
{
	unsigned char *out = malloc(len + 16);
	size_t pwl = strlen(st->pw);
	if (!out)
		return NULL;
	*out_len = 0;
	if (oid_is(alg, OID_PBES2, sizeof OID_PBES2)) {
		if (mbedtls_pkcs5_pbes2_ext(params, MBEDTLS_PKCS5_DECRYPT, (const unsigned char *)st->pw, pwl,
				data, len, out, len + 16, out_len) == 0)
			return out;
		st->wrong_pw = 1;
	} else if (oid_is(alg, OID_PBE_3DES, sizeof OID_PBE_3DES)) {
		if (mbedtls_pkcs12_pbe_ext(params, MBEDTLS_PKCS12_PBE_DECRYPT, MBEDTLS_CIPHER_DES_EDE3_CBC, MBEDTLS_MD_SHA1,
				(const unsigned char *)st->pw, pwl, data, len, out, len + 16, out_len) == 0)
			return out;
		st->wrong_pw = 1;
	} else if (oid_is(alg, OID_PBE_RC2_40, sizeof OID_PBE_RC2_40) || oid_is(alg, OID_PBE_RC2_128, sizeof OID_PBE_RC2_128)) {
		/* PKCS#12 key derivation (SHA-1, the password as BMPString), then RC2-CBC. */
		int bits = oid_is(alg, OID_PBE_RC2_40, sizeof OID_PBE_RC2_40) ? 40 : 128, iter = 1;
		unsigned char *p = params->p, *end = params->p + params->len, key[16], iv[8], prev[8], bmp[260];
		const unsigned char *salt;
		size_t sl, i, bl = 0;
		unsigned short K[64];
		for (i = 0; st->pw[i] && bl + 4 < sizeof bmp; i++) {
			bmp[bl++] = 0;
			bmp[bl++] = (unsigned char)st->pw[i];
		}
		bmp[bl++] = 0;
		bmp[bl++] = 0;
		if (mbedtls_asn1_get_tag(&p, end, &sl, MBEDTLS_ASN1_OCTET_STRING))
			goto fail;
		salt = p;
		p += sl;
		mbedtls_asn1_get_int(&p, end, &iter);
		if (mbedtls_pkcs12_derivation(key, (size_t)bits / 8, bmp, bl, salt, sl, MBEDTLS_MD_SHA1,
				MBEDTLS_PKCS12_DERIVE_KEY, iter) ||
				mbedtls_pkcs12_derivation(iv, 8, bmp, bl, salt, sl, MBEDTLS_MD_SHA1, MBEDTLS_PKCS12_DERIVE_IV, iter) ||
				len % 8)
			goto fail;
		rc2_key(K, key, bits / 8, bits);
		memcpy(prev, iv, 8);
		for (i = 0; i < len; i += 8) {
			unsigned char b[8];
			int k;
			memcpy(b, data + i, 8);
			rc2_decrypt_block(K, b);
			for (k = 0; k < 8; k++)
				out[i + k] = b[k] ^ prev[k];
			memcpy(prev, data + i, 8);
		}
		/* PKCS#7 padding */
		if (len == 0 || out[len - 1] == 0 || out[len - 1] > 8) {
			st->wrong_pw = 1;
			goto fail;
		}
		*out_len = len - out[len - 1];
		return out;
	} else {
		st->unknown_cipher = 1;
	}
fail:
	free(out);
	return NULL;
}

static void p12_safe_contents(p12_state *st, const unsigned char *der, size_t len)
{
	unsigned char *p = (unsigned char *)der, *end = (unsigned char *)der + len;
	size_t l;
	if (mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
		return;
	end = p + l;
	while (p < end) {
		unsigned char *bag_end, *val;
		mbedtls_asn1_buf oid;
		size_t vl;
		if (mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
			return;
		bag_end = p + l;
		if (mbedtls_asn1_get_tag(&p, bag_end, &oid.len, MBEDTLS_ASN1_OID))
			return;
		oid.p = p;
		p += oid.len;
		if (mbedtls_asn1_get_tag(&p, bag_end, &vl, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0)) {
			p = bag_end;
			continue;
		}
		val = p;
		if (oid_is(&oid, OID_SHROUDED_BAG, sizeof OID_SHROUDED_BAG) || oid_is(&oid, OID_KEY_BAG, sizeof OID_KEY_BAG)) {
			/* EncryptedPrivateKeyInfo, or a plain PrivateKeyInfo:
			 * mbedTLS reads both (PBES2 and PKCS#12 3DES). */
			int rc = mbedtls_pk_parse_key(st->key, val, vl, (const unsigned char *)st->pw, strlen(st->pw),
				mbedtls_ctr_drbg_random, &st->s->drbg);
			if (rc == 0)
				st->have_key = 1;
			else if (rc == MBEDTLS_ERR_PK_PASSWORD_MISMATCH)
				st->wrong_pw = 1;
		} else if (oid_is(&oid, OID_CERT_BAG, sizeof OID_CERT_BAG)) {
			/* CertBag: certId, [0] EXPLICIT OCTET STRING (an X.509 DER). */
			unsigned char *q = val, *qe = val + vl;
			size_t ql;
			if (!mbedtls_asn1_get_tag(&q, qe, &ql, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) {
				qe = q + ql;
				if (!mbedtls_asn1_get_tag(&q, qe, &ql, MBEDTLS_ASN1_OID)) {
					q += ql;
					if (!mbedtls_asn1_get_tag(&q, qe, &ql, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0) &&
							!mbedtls_asn1_get_tag(&q, qe, &ql, MBEDTLS_ASN1_OCTET_STRING))
						mbedtls_x509_crt_parse_der(st->certs, q, ql);
				}
			}
		}
		p = bag_end;
	}
}

static int p12_parse(p12_state *st, const unsigned char *der, size_t len)
{
	unsigned char *p = (unsigned char *)der, *end = (unsigned char *)der + len, *as_end;
	size_t l;
	int v;
	mbedtls_asn1_buf oid;
	/* PFX: version, authSafe (data: an OCTET STRING holding AuthenticatedSafe), macData */
	if (mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) ||
			mbedtls_asn1_get_int(&p, end, &v) ||
			mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
		return -1;
	if (mbedtls_asn1_get_tag(&p, end, &oid.len, MBEDTLS_ASN1_OID))
		return -1;
	oid.p = p;
	p += oid.len;
	if (!oid_is(&oid, OID_DATA, sizeof OID_DATA) ||
			mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0) ||
			mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_OCTET_STRING))
		return -1;
	/* AuthenticatedSafe: SEQUENCE OF ContentInfo */
	as_end = p + l;
	if (mbedtls_asn1_get_tag(&p, as_end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
		return -1;
	while (p < as_end) {
		unsigned char *ci_end;
		if (mbedtls_asn1_get_tag(&p, as_end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE))
			return -1;
		ci_end = p + l;
		if (mbedtls_asn1_get_tag(&p, ci_end, &oid.len, MBEDTLS_ASN1_OID))
			return -1;
		oid.p = p;
		p += oid.len;
		if (mbedtls_asn1_get_tag(&p, ci_end, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0)) {
			p = ci_end;
			continue;
		}
		if (oid_is(&oid, OID_DATA, sizeof OID_DATA)) {
			if (!mbedtls_asn1_get_tag(&p, ci_end, &l, MBEDTLS_ASN1_OCTET_STRING))
				p12_safe_contents(st, p, l);
		} else if (oid_is(&oid, OID_ENC_DATA, sizeof OID_ENC_DATA)) {
			/* EncryptedData: version, EncryptedContentInfo { type, alg, [0] IMPLICIT bytes } */
			unsigned char *q = p, *e;
			mbedtls_asn1_buf alg, params;
			size_t ql, plain_len;
			unsigned char *plain;
			if (mbedtls_asn1_get_tag(&q, ci_end, &ql, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) ||
					mbedtls_asn1_get_int(&q, ci_end, &v) ||
					mbedtls_asn1_get_tag(&q, ci_end, &ql, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) {
				p = ci_end;
				continue;
			}
			e = q + ql;
			if (mbedtls_asn1_get_tag(&q, e, &ql, MBEDTLS_ASN1_OID)) {
				p = ci_end;
				continue;
			}
			q += ql;
			if (mbedtls_asn1_get_alg(&q, e, &alg, &params) ||
					mbedtls_asn1_get_tag(&q, e, &ql, MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0)) {
				p = ci_end;
				continue;
			}
			plain = p12_decrypt(st, &alg, &params, q, ql, &plain_len);
			if (plain) {
				p12_safe_contents(st, plain, plain_len);
				free(plain);
			}
		}
		p = ci_end;
	}
	return 0;
}

/* ---- The CMS a signature is ---------------------------------------------------
 * Built forwards in fz_buffers: der() wraps contents in a tag and length. */

static fz_buffer *der(fz_context *ctx, int tag, fz_buffer *contents)
{
	fz_buffer *b = fz_new_buffer(ctx, contents->len + 8);
	size_t n = contents->len;
	fz_append_byte(ctx, b, tag);
	if (n < 128)
		fz_append_byte(ctx, b, (int)n);
	else if (n < 256) {
		fz_append_byte(ctx, b, 0x81);
		fz_append_byte(ctx, b, (int)n);
	} else if (n < 65536) {
		fz_append_byte(ctx, b, 0x82);
		fz_append_byte(ctx, b, (int)(n >> 8));
		fz_append_byte(ctx, b, (int)(n & 255));
	} else {
		fz_append_byte(ctx, b, 0x83);
		fz_append_byte(ctx, b, (int)(n >> 16));
		fz_append_byte(ctx, b, (int)((n >> 8) & 255));
		fz_append_byte(ctx, b, (int)(n & 255));
	}
	fz_append_buffer(ctx, b, contents);
	return b;
}

/* der() of raw bytes, appended to into, with the temporaries dropped. */
static void put(fz_context *ctx, fz_buffer *into, int tag, const unsigned char *data, size_t len)
{
	fz_buffer *c = fz_new_buffer_from_copied_data(ctx, data, len), *w = NULL;
	fz_var(w);
	fz_try(ctx) {
		w = der(ctx, tag, c);
		fz_append_buffer(ctx, into, w);
	}
	fz_always(ctx) {
		fz_drop_buffer(ctx, c);
		fz_drop_buffer(ctx, w);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

/* der() of a buffer, appended to into; the buffer is dropped. */
static void wrap(fz_context *ctx, fz_buffer *into, int tag, fz_buffer *contents)
{
	fz_buffer *w = NULL;
	fz_var(w);
	fz_try(ctx) {
		w = der(ctx, tag, contents);
		fz_append_buffer(ctx, into, w);
	}
	fz_always(ctx) {
		fz_drop_buffer(ctx, contents);
		fz_drop_buffer(ctx, w);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static const unsigned char OID_SHA256[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };
static const unsigned char OID_CONTENT_TYPE[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x03 };
static const unsigned char OID_SIGNING_TIME[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x05 };
static const unsigned char OID_ECDSA_SHA256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
static const unsigned char OID_RSA_ENC[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };

static void alg_sha256(fz_context *ctx, fz_buffer *into)
{
	fz_buffer *a = fz_new_buffer(ctx, 16);
	put(ctx, a, MBEDTLS_ASN1_OID, OID_SHA256, sizeof OID_SHA256);
	fz_append_byte(ctx, a, MBEDTLS_ASN1_NULL);
	fz_append_byte(ctx, a, 0);
	wrap(ctx, into, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE, a);
}

/* One attribute: SEQUENCE { oid, SET { value } }, as its own buffer. */
static fz_buffer *attribute(fz_context *ctx, const unsigned char *oid, size_t oid_len, int tag,
		const unsigned char *val, size_t vl)
{
	fz_buffer *a = fz_new_buffer(ctx, 64), *set = fz_new_buffer(ctx, 48), *out = fz_new_buffer(ctx, 64);
	put(ctx, a, MBEDTLS_ASN1_OID, oid, oid_len);
	put(ctx, set, tag, val, vl);
	wrap(ctx, a, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET, set);
	wrap(ctx, out, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE, a);
	return out;
}

static int buf_order(const void *a, const void *b)
{
	const fz_buffer *x = *(fz_buffer *const *)a, *y = *(fz_buffer *const *)b;
	size_t n = x->len < y->len ? x->len : y->len;
	int c = memcmp(x->data, y->data, n);
	return c ? c : (x->len > y->len) - (x->len < y->len);
}

static int signer_create_digest(fz_context *ctx, pdf_pkcs7_signer *ps, fz_stream *in, unsigned char *out,
		size_t out_len)
{
	struct folio_signer *s = (struct folio_signer *)ps;
	unsigned char h[32], ha[32], sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE], when[40];
	size_t sig_len = 0, n = 0;
	fz_buffer *attr[3] = { NULL, NULL, NULL }, *attrs = NULL, *si = NULL, *sd = NULL, *ci = NULL, *tmp = NULL;
	mbedtls_x509_crt *c;
	time_t now = time(NULL);
	struct tm *g = gmtime(&now);
	int i, ec = mbedtls_pk_get_type(&s->key) == MBEDTLS_PK_ECKEY;
	fz_var(attrs); fz_var(si); fz_var(sd); fz_var(ci); fz_var(tmp); fz_var(n);
	fz_try(ctx) {
		if (hash_stream(ctx, in, MBEDTLS_MD_SHA256, h))
			fz_throw(ctx, FZ_ERROR_GENERIC, "can't hash the document");
		/* Signed attributes, DER-sorted. */
		attr[0] = attribute(ctx, OID_CONTENT_TYPE, sizeof OID_CONTENT_TYPE, MBEDTLS_ASN1_OID, OID_DATA, sizeof OID_DATA);
		snprintf((char *)when, sizeof when, "%02d%02d%02d%02d%02d%02dZ", g->tm_year % 100, g->tm_mon + 1, g->tm_mday,
			g->tm_hour, g->tm_min, g->tm_sec);
		attr[1] = attribute(ctx, OID_SIGNING_TIME, sizeof OID_SIGNING_TIME, MBEDTLS_ASN1_UTC_TIME, when, 13);
		attr[2] = attribute(ctx, OID_MESSAGE_DIGEST, sizeof OID_MESSAGE_DIGEST, MBEDTLS_ASN1_OCTET_STRING, h, 32);
		qsort(attr, 3, sizeof attr[0], buf_order);
		tmp = fz_new_buffer(ctx, 256);
		for (i = 0; i < 3; i++)
			fz_append_buffer(ctx, tmp, attr[i]);
		/* Signed as a SET; carried as [0] IMPLICIT. */
		attrs = der(ctx, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET, tmp);
		mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), attrs->data, attrs->len, ha);
		if (mbedtls_pk_sign(&s->key, MBEDTLS_MD_SHA256, ha, 32, sig, sizeof sig, &sig_len,
				mbedtls_ctr_drbg_random, &s->drbg))
			fz_throw(ctx, FZ_ERROR_GENERIC, "the key couldn't sign");
		attrs->data[0] = MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0;

		/* SignerInfo */
		si = fz_new_buffer(ctx, 1024);
		fz_append_data(ctx, si, "\x02\x01\x01", 3);            /* version 1 */
		{
			fz_buffer *ias = fz_new_buffer(ctx, 256);
			fz_append_data(ctx, ias, s->chain.issuer_raw.p, s->chain.issuer_raw.len);
			put(ctx, ias, MBEDTLS_ASN1_INTEGER, s->chain.serial.p, s->chain.serial.len);
			wrap(ctx, si, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE, ias);
		}
		alg_sha256(ctx, si);
		fz_append_buffer(ctx, si, attrs);
		{
			fz_buffer *a = fz_new_buffer(ctx, 16);
			if (ec)
				put(ctx, a, MBEDTLS_ASN1_OID, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256);
			else {
				put(ctx, a, MBEDTLS_ASN1_OID, OID_RSA_ENC, sizeof OID_RSA_ENC);
				fz_append_byte(ctx, a, MBEDTLS_ASN1_NULL);
				fz_append_byte(ctx, a, 0);
			}
			wrap(ctx, si, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE, a);
		}
		put(ctx, si, MBEDTLS_ASN1_OCTET_STRING, sig, sig_len);

		/* SignedData */
		sd = fz_new_buffer(ctx, 4096);
		fz_append_data(ctx, sd, "\x02\x01\x01", 3);            /* version 1 */
		{
			fz_buffer *set = fz_new_buffer(ctx, 16);
			alg_sha256(ctx, set);
			wrap(ctx, sd, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET, set);
		}
		{
			fz_buffer *eci = fz_new_buffer(ctx, 16);
			put(ctx, eci, MBEDTLS_ASN1_OID, OID_DATA, sizeof OID_DATA);
			wrap(ctx, sd, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE, eci);
		}
		{
			fz_buffer *certs = fz_new_buffer(ctx, 4096);
			for (c = &s->chain; c && c->raw.len; c = c->next)
				fz_append_data(ctx, certs, c->raw.p, c->raw.len);
			wrap(ctx, sd, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0, certs);
		}
		{
			fz_buffer *set = fz_new_buffer(ctx, 1024), *seq = si;
			si = NULL;
			wrap(ctx, set, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE, seq);
			wrap(ctx, sd, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET, set);
		}

		/* ContentInfo */
		ci = fz_new_buffer(ctx, 4096);
		put(ctx, ci, MBEDTLS_ASN1_OID, OID_SIGNED_DATA, sizeof OID_SIGNED_DATA);
		{
			fz_buffer *seq = fz_new_buffer(ctx, 4096), *s2 = sd;
			sd = NULL;
			wrap(ctx, seq, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE, s2);
			wrap(ctx, ci, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0, seq);
		}
		{
			fz_buffer *top = fz_new_buffer(ctx, 4096), *c2 = ci;
			ci = NULL;
			wrap(ctx, top, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE, c2);
			ci = top;
		}
		if (ci->len > out_len)
			fz_throw(ctx, FZ_ERROR_GENERIC, "the signature is bigger than the space for it");
		memcpy(out, ci->data, ci->len);
		n = ci->len;
	}
	fz_always(ctx) {
		for (i = 0; i < 3; i++)
			fz_drop_buffer(ctx, attr[i]);
		fz_drop_buffer(ctx, tmp);
		fz_drop_buffer(ctx, attrs);
		fz_drop_buffer(ctx, si);
		fz_drop_buffer(ctx, sd);
		fz_drop_buffer(ctx, ci);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
	return (int)n;
}

static size_t signer_max_digest_size(fz_context *ctx, pdf_pkcs7_signer *ps)
{
	struct folio_signer *s = (struct folio_signer *)ps;
	mbedtls_x509_crt *c;
	size_t n = 2048 + MBEDTLS_PK_SIGNATURE_MAX_SIZE;
	(void)ctx;
	for (c = &s->chain; c && c->raw.len; c = c->next)
		n += c->raw.len + 8;
	return n;
}

static pdf_pkcs7_signer *signer_keep(fz_context *ctx, pdf_pkcs7_signer *ps)
{
	(void)ctx;
	((struct folio_signer *)ps)->refs++;
	return ps;
}

static void signer_drop(fz_context *ctx, pdf_pkcs7_signer *ps)
{
	struct folio_signer *s = (struct folio_signer *)ps;
	if (!s || --s->refs > 0)
		return;
	fz_drop_image(ctx, s->picture);
	mbedtls_pk_free(&s->key);
	mbedtls_x509_crt_free(&s->chain);
	if (s->drbg_ready)
		mbedtls_ctr_drbg_free(&s->drbg);
	(void)ctx;
	free(s);
}

static pdf_pkcs7_distinguished_name *signer_name(fz_context *ctx, pdf_pkcs7_signer *ps)
{
	return dn_of(ctx, &((struct folio_signer *)ps)->chain);
}

folio_signer *folio_signer_open(folio_ctx *fc, const unsigned char *p12, size_t len, const char *password)
{
	fz_context *ctx = fc->ctx;
	struct folio_signer *s = NULL;
	mbedtls_x509_crt certs, *c, *mine = NULL;
	p12_state st;
	static const char pers[] = "Folio signing";
	s = calloc(1, sizeof *s);
	if (!s) {
		snprintf(fc->err, sizeof fc->err, "there isn't enough memory");
		return NULL;
	}
	s->refs = 1;
	s->base.keep = signer_keep;
	s->base.drop = signer_drop;
	s->base.get_signing_name = signer_name;
	s->base.max_digest_size = signer_max_digest_size;
	s->base.create_digest = signer_create_digest;
	mbedtls_pk_init(&s->key);
	mbedtls_x509_crt_init(&s->chain);
	mbedtls_ctr_drbg_init(&s->drbg);
	s->drbg_ready = 1;
	if (mbedtls_ctr_drbg_seed(&s->drbg, urandom, NULL, (const unsigned char *)pers, sizeof pers - 1)) {
		snprintf(fc->err, sizeof fc->err, "there's no source of random numbers (CryptRandom isn't loaded)");
		signer_drop(ctx, &s->base);
		return NULL;
	}
	mbedtls_x509_crt_init(&certs);
	memset(&st, 0, sizeof st);
	st.key = &s->key;
	st.certs = &certs;
	st.pw = password ? password : "";
	st.s = s;
	if (p12_parse(&st, p12, len)) {
		snprintf(fc->err, sizeof fc->err, "it isn't a .p12 or .pfx certificate file");
		goto fail;
	}
	if (!st.have_key) {
		snprintf(fc->err, sizeof fc->err, st.wrong_pw ? "the password isn't right" :
			st.unknown_cipher ? "it's protected in a way Folio doesn't know" :
			"there's no private key in it");
		goto fail;
	}
	/* The certificate that goes with the key, first; the rest after. */
	for (c = &certs; c && c->raw.len; c = c->next)
		if (mbedtls_pk_check_pair(&c->pk, &s->key, mbedtls_ctr_drbg_random, &s->drbg) == 0) {
			mine = c;
			break;
		}
	if (!mine) {
		snprintf(fc->err, sizeof fc->err, st.wrong_pw ? "the password isn't right" :
			"there's no certificate for its key in it");
		goto fail;
	}
	mbedtls_x509_crt_parse_der(&s->chain, mine->raw.p, mine->raw.len);
	for (c = &certs; c && c->raw.len; c = c->next)
		if (c != mine)
			mbedtls_x509_crt_parse_der(&s->chain, c->raw.p, c->raw.len);
	mbedtls_x509_crt_free(&certs);
	return s;
fail:
	mbedtls_x509_crt_free(&certs);
	signer_drop(ctx, &s->base);
	return NULL;
}

int folio_signer_set_picture(folio_ctx *fc, folio_signer *s, const char *path, int clear_white)
{
	fz_context *ctx = fc->ctx;
	fz_image *img = NULL;
	fz_var(img);
	fz_drop_image(ctx, s->picture);
	s->picture = NULL;
	if (!path)
		return 0;
	fz_try(ctx) {
		img = fz_new_image_from_file(ctx, path);
		s->picture = clear_white ? folio_image_without_white(ctx, img) : fz_keep_image(ctx, img);
	}
	fz_always(ctx)
		fz_drop_image(ctx, img);
	fz_catch(ctx) {
		folio_set_error(fc);
		return -1;
	}
	return 0;
}

void folio_signer_drop(folio_ctx *fc, folio_signer *s)
{
	if (s)
		signer_drop(fc->ctx, &s->base);
}

void folio_signer_name(folio_signer *s, char *out, size_t size)
{
	const mbedtls_x509_name *n;
	out[0] = 0;
	for (n = &s->chain.subject; n; n = n->next)
		if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &n->oid) == 0) {
			size_t l = n->val.len < size - 1 ? n->val.len : size - 1;
			memcpy(out, n->val.p, l);
			out[l] = 0;
			return;
		}
}

/* Signs: the named empty signature field on the page, or (field NULL) a
 * new one there, at where (folio points). One change in the journal; the
 * signature itself is made when the PDF is saved. */
/* Other fields read-only (locking the document after signing). */
static void lock_fields(fz_context *ctx, pdf_obj *fields, pdf_obj *except, int depth)
{
	int i, n = pdf_array_len(ctx, fields);
	if (depth > 16)
		return;
	for (i = 0; i < n; i++) {
		pdf_obj *f = pdf_array_get(ctx, fields, i), *kids = pdf_dict_get(ctx, f, PDF_NAME(Kids));
		if (!pdf_objcmp(ctx, f, except))
			continue;
		if (pdf_is_array(ctx, kids) && pdf_dict_get(ctx, pdf_array_get(ctx, kids, 0), PDF_NAME(T)))
			lock_fields(ctx, kids, except, depth + 1);
		else if (!pdf_name_eq(ctx, pdf_dict_get_inheritable(ctx, f, PDF_NAME(FT)), PDF_NAME(Sig)) ||
				!pdf_dict_get_inheritable(ctx, f, PDF_NAME(V)))
			pdf_dict_put_int(ctx, f, PDF_NAME(Ff), pdf_dict_get_int(ctx, f, PDF_NAME(Ff)) | 1);
	}
}

/* The signature value (what MuPDF's pdf_signature_set_value makes, with
 * the signer's name, reason and place in it too, and certification or a
 * lock as asked), and its appearance. The bytes are signed when the PDF is
 * saved (complete_signatures). ByteRange, Contents and Filter must come
 * first, in that order: the writer looks for them. */
static void sign_value(fz_context *ctx, pdf_document *pdf, pdf_annot *w, struct folio_signer *s,
		const char *reason, const char *location, int mode)
{
	pdf_obj *wobj = pdf_annot_obj(ctx, w), *v = NULL, *indv, *refs, *form, *root;
	fz_display_list *dl = NULL;
	char who[128], *info = NULL, *buf = NULL;
	int64_t now = time(NULL);
	int vnum, sf;
	size_t max = s->base.max_digest_size(ctx, &s->base);
	fz_rect rect = pdf_annot_rect(ctx, w);
	fz_var(v);
	fz_var(dl);
	fz_var(info);
	fz_var(buf);
	folio_signer_name(s, who, sizeof who);
	root = pdf_dict_get(ctx, pdf_trailer(ctx, pdf), PDF_NAME(Root));
	fz_try(ctx) {
		if (mode == FOLIO_SIGN_LOCK) {
			pdf_obj *lock = pdf_dict_put_dict(ctx, wobj, PDF_NAME(Lock), 3);
			pdf_dict_put_name(ctx, lock, PDF_NAME(Type), "SigFieldLock");
			pdf_dict_put(ctx, lock, PDF_NAME(Action), PDF_NAME(All));
			pdf_dict_put_int(ctx, lock, PDF_NAME(P), 1);
		}
		pdf_dirty_annot(ctx, w);
		if (!fz_is_empty_rect(rect)) {
			info = pdf_signature_info(ctx, who[0] ? who : NULL, NULL, reason, location, now, 1);
			dl = pdf_signature_appearance_signed(ctx, rect, pdf_annot_language(ctx, w), s->picture,
				s->picture ? NULL : who[0] ? who : NULL, info, 0);
			pdf_set_annot_appearance_from_display_list(ctx, w, "N", NULL, fz_identity, dl);
		}
		form = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
		if (!form)
			form = pdf_dict_put_dict(ctx, root, PDF_NAME(AcroForm), 1);
		sf = pdf_dict_get_int(ctx, form, PDF_NAME(SigFlags));
		if ((sf & 3) != 3)
			pdf_dict_put_int(ctx, form, PDF_NAME(SigFlags), sf | 3);

		vnum = pdf_create_object(ctx, pdf);
		indv = pdf_new_indirect(ctx, pdf, vnum, 0);
		pdf_dict_put_drop(ctx, wobj, PDF_NAME(V), indv);
		v = pdf_new_dict(ctx, pdf, 12);
		pdf_update_object(ctx, pdf, vnum, v);
		buf = fz_calloc(ctx, max, 1);
		pdf_dict_put_array(ctx, v, PDF_NAME(ByteRange), 4);
		pdf_dict_put_string(ctx, v, PDF_NAME(Contents), buf, max);
		pdf_dict_put(ctx, v, PDF_NAME(Filter), PDF_NAME(Adobe_PPKLite));
		pdf_dict_put(ctx, v, PDF_NAME(SubFilter), PDF_NAME(adbe_pkcs7_detached));
		pdf_dict_put(ctx, v, PDF_NAME(Type), PDF_NAME(Sig));
		pdf_dict_put_date(ctx, v, PDF_NAME(M), now);
		if (who[0])
			pdf_dict_put_text_string(ctx, v, PDF_NAME(Name), who);
		if (reason && *reason)
			pdf_dict_put_text_string(ctx, v, PDF_NAME(Reason), reason);
		if (location && *location)
			pdf_dict_put_text_string(ctx, v, PDF_NAME(Location), location);
		if (mode >= FOLIO_CERTIFY_NO_CHANGES || mode == FOLIO_SIGN_LOCK) {
			pdf_obj *r, *tp;
			refs = pdf_dict_put_array(ctx, v, PDF_NAME(Reference), 1);
			r = pdf_array_put_dict(ctx, refs, 0, 4);
			pdf_dict_put(ctx, r, PDF_NAME(Type), PDF_NAME(SigRef));
			tp = pdf_dict_put_dict(ctx, r, PDF_NAME(TransformParams), 4);
			pdf_dict_put(ctx, tp, PDF_NAME(Type), PDF_NAME(TransformParams));
			pdf_dict_put(ctx, tp, PDF_NAME(V), PDF_NAME(1_2));
			if (mode == FOLIO_SIGN_LOCK) {
				pdf_dict_put(ctx, r, PDF_NAME(TransformMethod), PDF_NAME(FieldMDP));
				pdf_dict_put(ctx, r, PDF_NAME(Data), root);
				pdf_dict_put(ctx, tp, PDF_NAME(Action), PDF_NAME(All));
				pdf_dict_put_int(ctx, tp, PDF_NAME(P), 1);
			} else {
				pdf_dict_put(ctx, r, PDF_NAME(TransformMethod), PDF_NAME(DocMDP));
				pdf_dict_put_int(ctx, tp, PDF_NAME(P), mode - FOLIO_CERTIFY_NO_CHANGES + 1);
				pdf_dict_put(ctx, pdf_dict_put_dict(ctx, root, PDF_NAME(Perms), 1), PDF_NAME(DocMDP), indv);
			}
		}
		if (mode == FOLIO_SIGN_LOCK)
			lock_fields(ctx, pdf_dict_get(ctx, form, PDF_NAME(Fields)), wobj, 0);
		pdf_dict_put_int(ctx, wobj, PDF_NAME(Ff), pdf_dict_get_int(ctx, wobj, PDF_NAME(Ff)) | 1);
		/* Last: nothing may change after this (a later change goes into
		 * a section of its own, after the signature's byte range). */
		pdf_xref_store_unsaved_signature(ctx, pdf, wobj, &s->base);
	}
	fz_always(ctx) {
		pdf_drop_obj(ctx, v);
		fz_drop_display_list(ctx, dl);
		fz_free(ctx, info);
		fz_free(ctx, buf);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

int folio_sign(folio_doc *doc, folio_loc loc, const char *field, folio_rect where, folio_signer *s,
		const char *reason, const char *location, int mode)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	int op = 0;
	if (!pdf) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "only a PDF can be signed");
		return -1;
	}
	if (doc->pw_change != FOLIO_PW_KEEP) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "save the PDF with its new password first, then sign it");
		return -1;
	}
	fz_var(op);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		pdf_annot *w = NULL;
		folio_allow(doc, FOLIO_CHANGE_SIGN, NULL);
		if (mode >= FOLIO_CERTIFY_NO_CHANGES && folio_is_signed(doc))
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "only the first signature can certify a PDF, and it's signed already");
		pdf_begin_operation(ctx, pdf, "Sign");
		op = 1;
		if (field) {
			for (w = pdf_first_widget(ctx, page); w; w = pdf_next_widget(ctx, w)) {
				char *name;
				int same;
				if (pdf_widget_type(ctx, w) != PDF_WIDGET_TYPE_SIGNATURE)
					continue;
				name = pdf_load_field_name(ctx, pdf_annot_obj(ctx, w));
				same = name && !strcmp(name, field);
				fz_free(ctx, name);
				if (same)
					break;
			}
			if (!w)
				fz_throw(ctx, FZ_ERROR_ARGUMENT, "there's no signature field called '%s' there", field);
			if (pdf_widget_is_signed(ctx, w))
				fz_throw(ctx, FZ_ERROR_ARGUMENT, "it's signed already");
		} else {
			char name[32];
			int k = pdf_count_signatures(ctx, pdf) + 1;
			fz_rect r;
			snprintf(name, sizeof name, "Signature%d", k);
			w = pdf_create_signature_widget(ctx, page, name);
			/* MuPDF gives a new field Lock All; Acrobat's ordinary
			 * signature locks nothing (FOLIO_SIGN_LOCK asks for it). */
			pdf_dict_del(ctx, pdf_annot_obj(ctx, w), PDF_NAME(Lock));
			r.x0 = where.x0 + cp->bounds.x0;
			r.y0 = where.y0 + cp->bounds.y0;
			r.x1 = where.x1 + cp->bounds.x0;
			r.y1 = where.y1 + cp->bounds.y0;
			pdf_set_annot_rect(ctx, w, r);
		}
		sign_value(ctx, pdf, w, s, reason, location, mode);
		pdf_end_operation(ctx, pdf);
		op = 0;
	}
	fz_catch(ctx) {
		if (op)
			pdf_abandon_operation(ctx, pdf);
		folio_set_error(doc->fc);
		folio_forget_pages(doc);
		return -1;
	}
	folio_forget_pages(doc);
	return 0;
}

/* ==== What a signed PDF still allows =========================================
 * A signature must stay as it was signed. So, once a PDF has a signature:
 *   - signed signature fields are never changed, cleared or signed again;
 *   - fields a signature locks (its /Lock, or its FieldMDP reference:
 *     All, Include or Exclude) can't be filled in;
 *   - a certifying signature (Root/Perms/DocMDP) is obeyed: P 1 no changes
 *     at all; P 2 filling in forms and signing only; P 3 annotations too;
 *   - its pages aren't turned, deleted, moved or added to, recognised text
 *     isn't written into them, and its password isn't changed (those would
 *     change or rewrite what was signed);
 *   - it's only ever saved incrementally (folio_save_pdf).
 * folio_allow throws (inside the caller's fz_try) if a change isn't allowed. */

typedef struct {
	int signed_n;           /* signed signature fields */
	int docmdp;             /* the certifying signature's P, or 0 */
	int lock_all;           /* every other field is locked */
	pdf_obj *include[16], *exclude[16];   /* Fields arrays */
	int ni, ne;
} sig_state;

static void lock_of(fz_context *ctx, pdf_obj *lock, sig_state *st)
{
	pdf_obj *action = pdf_dict_get(ctx, lock, PDF_NAME(Action));
	int p = pdf_dict_get_int(ctx, lock, PDF_NAME(P));
	/* "Lock the document" (PDF 2.0): a P, as a certification's. */
	if (p >= 1 && p <= 3 && (!st->docmdp || p < st->docmdp))
		st->docmdp = p;
	if (!action)
		return;
	if (pdf_name_eq(ctx, action, PDF_NAME(All)))
		st->lock_all = 1;
	else if (pdf_name_eq(ctx, action, PDF_NAME(Include)) && st->ni < 16)
		st->include[st->ni++] = pdf_dict_get(ctx, lock, PDF_NAME(Fields));
	else if (pdf_name_eq(ctx, action, PDF_NAME(Exclude)) && st->ne < 16)
		st->exclude[st->ne++] = pdf_dict_get(ctx, lock, PDF_NAME(Fields));
}

static void scan_sigs(fz_context *ctx, pdf_obj *fields, int depth, sig_state *st)
{
	int i, n = pdf_array_len(ctx, fields);
	if (depth > 16)
		return;
	for (i = 0; i < n; i++) {
		pdf_obj *f = pdf_array_get(ctx, fields, i), *kids = pdf_dict_get(ctx, f, PDF_NAME(Kids));
		if (pdf_is_array(ctx, kids) && pdf_dict_get(ctx, pdf_array_get(ctx, kids, 0), PDF_NAME(T)))
			scan_sigs(ctx, kids, depth + 1, st);
		else if (pdf_name_eq(ctx, pdf_dict_get_inheritable(ctx, f, PDF_NAME(FT)), PDF_NAME(Sig))) {
			pdf_obj *v = pdf_dict_get_inheritable(ctx, f, PDF_NAME(V)), *refs;
			int k;
			if (!pdf_is_dict(ctx, v) || !pdf_dict_get(ctx, v, PDF_NAME(Contents)))
				continue;
			st->signed_n++;
			lock_of(ctx, pdf_dict_get(ctx, f, PDF_NAME(Lock)), st);
			refs = pdf_dict_get(ctx, v, PDF_NAME(Reference));
			for (k = 0; k < pdf_array_len(ctx, refs); k++) {
				pdf_obj *r = pdf_array_get(ctx, refs, k);
				if (pdf_name_eq(ctx, pdf_dict_get(ctx, r, PDF_NAME(TransformMethod)), PDF_NAME(FieldMDP)))
					lock_of(ctx, pdf_dict_get(ctx, r, PDF_NAME(TransformParams)), st);
			}
		}
	}
}

static void sig_state_of(fz_context *ctx, pdf_document *pdf, sig_state *st)
{
	pdf_obj *root = pdf_dict_get(ctx, pdf_trailer(ctx, pdf), PDF_NAME(Root)), *mdp, *refs;
	int k;
	memset(st, 0, sizeof *st);
	scan_sigs(ctx, pdf_dict_getp(ctx, root, "AcroForm/Fields"), 0, st);
	mdp = pdf_dict_getp(ctx, root, "Perms/DocMDP");
	refs = pdf_dict_get(ctx, mdp, PDF_NAME(Reference));
	for (k = 0; k < pdf_array_len(ctx, refs); k++) {
		pdf_obj *r = pdf_array_get(ctx, refs, k);
		if (pdf_name_eq(ctx, pdf_dict_get(ctx, r, PDF_NAME(TransformMethod)), PDF_NAME(DocMDP))) {
			pdf_obj *p = pdf_dict_getp(ctx, r, "TransformParams/P");
			st->docmdp = p ? pdf_to_int(ctx, p) : 2;
			if (st->docmdp < 1 || st->docmdp > 3)
				st->docmdp = 2;
		}
	}
}

static int in_fields(fz_context *ctx, pdf_obj *arr, const char *name)
{
	int i, n = pdf_array_len(ctx, arr);
	size_t l;
	for (i = 0; i < n; i++) {
		const char *s = pdf_to_text_string(ctx, pdf_array_get(ctx, arr, i));
		/* The field, or a field below it. */
		l = strlen(s);
		if (l && !strncmp(name, s, l) && (name[l] == 0 || name[l] == '.'))
			return 1;
	}
	return 0;
}

static int field_locked(fz_context *ctx, const sig_state *st, const char *name)
{
	int i;
	if (st->lock_all)
		return 1;
	for (i = 0; i < st->ni; i++)
		if (in_fields(ctx, st->include[i], name))
			return 1;
	for (i = 0; i < st->ne; i++)
		if (!in_fields(ctx, st->exclude[i], name))
			return 1;
	return 0;
}

void folio_allow(folio_doc *doc, int what, const char *field)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	sig_state st;
	if (!pdf)
		return;
	sig_state_of(ctx, pdf, &st);
	if (!st.signed_n && !st.docmdp)
		return;
	if (st.docmdp == 1)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "the PDF is certified with no changes allowed: it can't be changed");
	switch (what) {
	case FOLIO_CHANGE_PAGES:
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "the PDF is signed: changing its pages would break the signature");
	case FOLIO_CHANGE_CONTENT:
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "the PDF is signed: changing what's on its pages would break the signature");
	case FOLIO_CHANGE_ANNOT:
		if (st.docmdp == 2)
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "the PDF is certified for filling in and signing only: notes and markings can't be added");
		break;
	case FOLIO_CHANGE_FORM:
		if (field && field_locked(ctx, &st, field))
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "the field is locked by a signature");
		break;
	default:
		break;
	}
}

int folio_is_signed(folio_doc *doc)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	sig_state st;
	int n = 0;
	if (!pdf)
		return 0;
	fz_try(ctx) {
		sig_state_of(ctx, pdf, &st);
		n = st.signed_n || st.docmdp;
	}
	fz_catch(ctx)
		n = 0;
	return n;
}
