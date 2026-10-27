#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Makes throwaway .p12 certificates for sign_test, in the directory given
# (made fresh by `make test`, never kept: no private keys go in the
# repository). Password "folio" throughout.
#   rsa-aes.p12     RSA, PBES2 with AES-256 (what OpenSSL 3 makes now)
#   rsa-legacy.p12  RSA, the old way: RC2-40 certificates, 3DES key
#   rsa-3des.p12    RSA, 3DES for both
#   ec.p12          ECDSA P-256, with the CA that certified it
#   ca.pem          that CA's certificate, to trust
set -e
d=$1
mkdir -p "$d"
cd "$d"
subj() { echo "/CN=$1/O=Folio Test/C=GB"; }
openssl req -x509 -newkey rsa:2048 -nodes -keyout rsa.key -out rsa.crt -days 3650 -subj "$(subj 'Sam Signer')" 2>/dev/null
openssl pkcs12 -export -inkey rsa.key -in rsa.crt -out rsa-aes.p12 -passout pass:folio
openssl pkcs12 -export -legacy -inkey rsa.key -in rsa.crt -out rsa-legacy.p12 -passout pass:folio
openssl pkcs12 -export -inkey rsa.key -in rsa.crt -out rsa-3des.p12 -passout pass:folio \
	-certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout ca.key -out ca.pem -days 3650 \
	-subj "$(subj 'Folio Test Signing CA')" -addext basicConstraints=critical,CA:TRUE -addext keyUsage=keyCertSign 2>/dev/null
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout ec.key -out ec.csr -subj "$(subj 'Eric Curve')" 2>/dev/null
openssl x509 -req -in ec.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out ec.crt -days 3650 2>/dev/null
openssl pkcs12 -export -inkey ec.key -in ec.crt -certfile ca.pem -out ec.p12 -passout pass:folio
rm -f *.key *.csr *.srl *.crt
