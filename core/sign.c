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
