#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
"""A pretend eSCL scanner and mDNS responder for tests/escl_test.c.

    tests/fake-escl.py OUTDIR

Listens on 127.0.0.1 (ports chosen by the system) and writes them to
OUTDIR/ports as "HTTP_PORT MDNS_PORT". Behaves as a real scanner does where
it matters:
  - ScannerCapabilities is sent chunked;
  - /eSCL/ScanJobs answers 201 with an absolute Location;
  - the first NextDocument of each job answers 503 (warming up);
  - the glass gives one page; the feeder two, then 404;
  - /empty/ScanJobs answers 409 (no paper in the feeder);
  - DELETE of a job is recorded in OUTDIR/deleted;
  - the last ScanSettings posted is kept in OUTDIR/settings.xml.
mDNS: a query for _uscan._tcp.local gets PTR, SRV and TXT (ty, rs), with
name compression, from the port it was asked on, with the query's ID.
Stops when OUTDIR/stop appears (or after 120 s)."""
import os
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

OUT = sys.argv[1]
CAPS = b"""<?xml version="1.0" encoding="UTF-8"?>
<scan:ScannerCapabilities xmlns:scan="http://schemas.hp.com/imaging/escl/2011/05/03" xmlns:pwg="http://www.pwg.org/schemas/2010/12/sm">
  <pwg:Version>2.63</pwg:Version>
  <pwg:MakeAndModel>Folio Test Scanner 9000</pwg:MakeAndModel>
  <scan:Platen>
    <scan:PlatenInputCaps>
      <scan:MinWidth>16</scan:MinWidth>
      <scan:MaxWidth>2550</scan:MaxWidth>
      <scan:MaxHeight>3508</scan:MaxHeight>
      <scan:SettingProfiles>
        <scan:SettingProfile>
          <scan:ColorModes><scan:ColorMode>RGB24</scan:ColorMode><scan:ColorMode>Grayscale8</scan:ColorMode></scan:ColorModes>
          <scan:DocumentFormats><pwg:DocumentFormat>application/pdf</pwg:DocumentFormat><pwg:DocumentFormat>image/jpeg</pwg:DocumentFormat>
            <scan:DocumentFormatExt>image/jpeg</scan:DocumentFormatExt></scan:DocumentFormats>
          <scan:SupportedResolutions><scan:DiscreteResolutions>
            <scan:DiscreteResolution><scan:XResolution>600</scan:XResolution><scan:YResolution>600</scan:YResolution></scan:DiscreteResolution>
            <scan:DiscreteResolution><scan:XResolution>75</scan:XResolution><scan:YResolution>75</scan:YResolution></scan:DiscreteResolution>
            <scan:DiscreteResolution><scan:XResolution>300</scan:XResolution><scan:YResolution>300</scan:YResolution></scan:DiscreteResolution>
            <scan:DiscreteResolution><scan:XResolution>150</scan:XResolution><scan:YResolution>150</scan:YResolution></scan:DiscreteResolution>
          </scan:DiscreteResolutions></scan:SupportedResolutions>
        </scan:SettingProfile>
      </scan:SettingProfiles>
    </scan:PlatenInputCaps>
  </scan:Platen>
  <scan:BrightnessSupport><scan:Min>0</scan:Min><scan:Max>2000</scan:Max><scan:Normal>1000</scan:Normal><scan:Step>1</scan:Step></scan:BrightnessSupport>
  <scan:Adf>
    <scan:AdfSimplexInputCaps>
      <scan:MaxWidth>2550</scan:MaxWidth>
      <scan:MaxHeight>4200</scan:MaxHeight>
    </scan:AdfSimplexInputCaps>
    <scan:AdfDuplexInputCaps>
      <scan:MaxWidth>2550</scan:MaxWidth>
      <scan:MaxHeight>4200</scan:MaxHeight>
    </scan:AdfDuplexInputCaps>
  </scan:Adf>
</scan:ScannerCapabilities>
"""

jobs = {}           # path -> {"feeder": bool, "served": int, "waited": bool}
lock = threading.Lock()


def page_bytes(n):
    """A page's bytes: JPEG markers round recognisable text."""
    return b"\xff\xd8\xff\xe0FAKE JPEG PAGE %d " % n + bytes(range(256)) * 40 + b"\xff\xd9"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body=b"", ctype="text/xml", headers=()):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        if body:
            self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path
        if p.endswith("/ScannerCapabilities"):
            # Chunked, in uneven pieces.
            self.send_response(200)
            self.send_header("Content-Type", "text/xml")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            for i in range(0, len(CAPS), 700):
                c = CAPS[i:i + 700]
                self.wfile.write(b"%x\r\n" % len(c) + c + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            return
        if p.endswith("/NextDocument"):
            job = p[: -len("/NextDocument")]
            with lock:
                j = jobs.get(job)
                if j is None:
                    return self.send(404)
                if not j["waited"]:
                    j["waited"] = True
                    return self.send(503)
                limit = 2 if j["feeder"] else 1
                if j["served"] >= limit:
                    return self.send(404)
                j["served"] += 1
                n = j["served"]
            if "slow" in self.path or os.path.exists(os.path.join(OUT, "slow")):
                time.sleep(1.5)     # the scanner taking its time over a page
            return self.send(200, page_bytes(n), "image/jpeg")
        self.send(404)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        with open(os.path.join(OUT, "settings.xml"), "wb") as f:
            f.write(body)
        if self.path.startswith("/empty/"):
            return self.send(409)
        if self.path.endswith("/ScanJobs"):
            with lock:
                job = "/eSCL/ScanJobs/job%d" % (len(jobs) + 1)
                jobs[job] = {"feeder": b"Feeder" in body, "served": 0, "waited": False}
            host = "127.0.0.1:%d" % self.server.server_address[1]
            return self.send(201, headers=[("Location", "http://%s%s" % (host, job))])
        self.send(404)

    def do_DELETE(self):
        with open(os.path.join(OUT, "deleted"), "a") as f:
            f.write(self.path + "\n")
        self.send(200)


def name(*labels):
    return b"".join(bytes([len(l)]) + l.encode() for l in labels) + b"\0"


def mdns(sock, http_port):
    while True:
        try:
            q, addr = sock.recvfrom(2048)
        except OSError:
            return
        if len(q) < 12 or b"_uscan" not in q:
            continue
        qid = q[:2]
        # Answer: PTR _uscan._tcp.local -> Folio Test._uscan._tcp.local;
        # SRV and TXT for that, names compressed against the PTR's.
        svc = name("_uscan", "_tcp", "local")
        head = qid + struct.pack(">HHHHH", 0x8400, 0, 1, 0, 2)
        ptr_owner_off = len(head)
        inst = bytes([10]) + b"Folio Test" + bytes([0xC0, ptr_owner_off])
        ptr = svc + struct.pack(">HHIH", 12, 1, 120, len(inst)) + inst
        inst_off = len(head) + len(svc) + 10      # where the instance name starts
        ref = bytes([0xC0, inst_off])
        srv_rd = struct.pack(">HHH", 0, 0, http_port) + name("fake", "local")
        srv = ref + struct.pack(">HHIH", 33, 0x8001, 120, len(srv_rd)) + srv_rd
        txt_rd = b"".join(bytes([len(s)]) + s for s in (b"txtvers=1", b"ty=Folio Test Scanner", b"rs=eSCL"))
        txt = ref + struct.pack(">HHIH", 16, 0x8001, 120, len(txt_rd)) + txt_rd
        sock.sendto(head + ptr + srv + txt, addr)


def main():
    httpd = HTTPServer(("127.0.0.1", 0), Handler)
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("127.0.0.1", 0))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    threading.Thread(target=mdns, args=(u, httpd.server_address[1]), daemon=True).start()
    with open(os.path.join(OUT, "ports.tmp"), "w") as f:
        f.write("%d %d\n" % (httpd.server_address[1], u.getsockname()[1]))
    os.rename(os.path.join(OUT, "ports.tmp"), os.path.join(OUT, "ports"))
    t0 = time.time()
    while not os.path.exists(os.path.join(OUT, "stop")) and time.time() - t0 < 120:
        time.sleep(0.1)


main()
