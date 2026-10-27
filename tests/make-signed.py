#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
"""Makes the signed PDFs sign_test checks, in tests/sig/ (run once; they're
kept in the repository, with the test CA's certificate, but no keys):
  ca.pem          a test certificate authority (to trust)
  rsa.pdf         signed with an RSA key the CA certified
  ec.pdf          signed with an ECDSA (P-256) key the CA certified
  self.pdf        signed with a self-signed RSA key
  tampered.pdf    rsa.pdf with a byte of the signed page changed
  updated.pdf     rsa.pdf with a change saved after the signature
  empty.pdf       an empty signature field
Needs pyHanko (pip install pyhanko). """
import datetime, io, sys
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, ec
from pyhanko.sign import signers, fields
from pyhanko.sign.signers import pdf_signer
from pyhanko.pdf_utils.incremental_writer import IncrementalPdfFileWriter
from pyhanko.pdf_utils import generic
from pyhanko_certvalidator.registry import SimpleCertificateStore
from asn1crypto import x509 as ax509, keys as akeys

out = sys.argv[1] if len(sys.argv) > 1 else "tests/sig"
now = datetime.datetime(2026, 10, 1, tzinfo=datetime.timezone.utc)

def name(cn, o="Folio Test"):
    return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn),
                      x509.NameAttribute(NameOID.ORGANIZATION_NAME, o),
                      x509.NameAttribute(NameOID.COUNTRY_NAME, "GB")])

def cert(subject, issuer, key, issuer_key, ca=False, days=3650):
    b = (x509.CertificateBuilder().subject_name(subject).issuer_name(issuer)
         .public_key(key.public_key()).serial_number(x509.random_serial_number())
         .not_valid_before(now - datetime.timedelta(days=1))
         .not_valid_after(now + datetime.timedelta(days=days))
         .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if ca:
        b = b.add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
    else:
        b = b.add_extension(x509.KeyUsage(True, True, False, False, False, False, False, False, False), critical=True)
    return b.sign(issuer_key, hashes.SHA256())

ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
ca = cert(name("Folio Test CA"), name("Folio Test CA"), ca_key, ca_key, ca=True)
open(f"{out}/ca.pem", "wb").write(ca.public_bytes(serialization.Encoding.PEM))

def to_signer(key, c, chain):
    kb = key.private_bytes(serialization.Encoding.DER, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    return signers.SimpleSigner(
        signing_cert=ax509.Certificate.load(c.public_bytes(serialization.Encoding.DER)),
        signing_key=akeys.PrivateKeyInfo.load(kb),
        cert_registry=SimpleCertificateStore.from_certs(
            [ax509.Certificate.load(x.public_bytes(serialization.Encoding.DER)) for x in chain]))

def blank_pdf():
    # A one-page PDF with some text, written by hand.
    content = b"BT /F1 24 Tf 72 700 Td (Signed test page) Tj ET"
    objs = [b"<< /Type /Catalog /Pages 2 0 R >>",
            b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
            b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream",
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
    o = b"%PDF-1.7\n"; offs = []
    for i, ob in enumerate(objs):
        offs.append(len(o)); o += b"%d 0 obj\n" % (i + 1) + ob + b"\nendobj\n"
    x = len(o)
    o += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1) + b"".join(b"%010d 00000 n \n" % f for f in offs)
    o += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)
    return o

def sign(signer, path, reason="Testing Folio"):
    w = IncrementalPdfFileWriter(io.BytesIO(blank_pdf()))
    meta = signers.PdfSignatureMetadata(field_name="Signature1", reason=reason, location="Aldershot")
    spec = fields.SigFieldSpec("Signature1", on_page=0, box=(72, 600, 272, 660))
    data = pdf_signer.PdfSigner(meta, signer=signer, new_field_spec=spec).sign_pdf(w)
    b = data.getvalue()
    open(path, "wb").write(b)
    return b

rsa_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
rsa_cert = cert(name("Jane Signer"), ca.subject, rsa_key, ca_key)
signed = sign(to_signer(rsa_key, rsa_cert, [ca]), f"{out}/rsa.pdf")

ec_key = ec.generate_private_key(ec.SECP256R1())
ec_cert = cert(name("Eve Curve"), ca.subject, ec_key, ca_key)
sign(to_signer(ec_key, ec_cert, [ca]), f"{out}/ec.pdf")

self_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
self_cert = cert(name("Sam Self"), name("Sam Self"), self_key, self_key)
sign(to_signer(self_key, self_cert, []), f"{out}/self.pdf")

# Tampered: a byte of the page's text, inside what was signed.
i = signed.index(b"Signed test page")
t = bytearray(signed)
t[i + 7] = ord("X")          # "Signed Xest page"
open(f"{out}/tampered.pdf", "wb").write(bytes(t))

# Updated: an incremental save after the signature (the catalogue changed).
w = IncrementalPdfFileWriter(io.BytesIO(signed))
w.root["/Lang"] = generic.TextStringObject("en-GB")
w.update_root()
buf = io.BytesIO(); w.write(buf)
open(f"{out}/updated.pdf", "wb").write(buf.getvalue())

# Empty: a signature field nobody has signed.
w = IncrementalPdfFileWriter(io.BytesIO(blank_pdf()))
fields.append_signature_field(w, fields.SigFieldSpec("SignHere", on_page=0, box=(72, 500, 272, 560)))
buf = io.BytesIO(); w.write(buf)
open(f"{out}/empty.pdf", "wb").write(buf.getvalue())
print("made", out)
