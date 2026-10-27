/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Folio's mbedTLS configuration: only what checking and making PDF
 * signatures needs (no TLS, no networking, no PSA). */
#ifndef FOLIO_MBEDTLS_CONFIG_H
#define FOLIO_MBEDTLS_CONFIG_H

/* System */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_MS_TIME_ALT     /* mbedtls_ms_time: core/sign.c */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_NO_PLATFORM_ENTROPY      /* RISC OS: seeded by Folio from /dev/urandom */

/* Numbers and encodings */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PEM_WRITE_C

/* Digests */
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* Public keys */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_GENPRIME
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_BP256R1_ENABLED
#define MBEDTLS_ECP_DP_BP384R1_ENABLED
#define MBEDTLS_ECP_DP_BP512R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

/* Certificates */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

/* Keys in .p12 / .pfx files (encrypted PKCS#8, PBES2 and PKCS#12 PBE) */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_DES_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_PKCS5_C
#define MBEDTLS_PKCS12_C
#define MBEDTLS_CIPHER_PADDING_PKCS7

/* Random numbers for signing */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C

#endif
