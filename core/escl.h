/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Scanning over the network with eSCL ("AirScan", Mopria): plain HTTP and
 * XML, which most network scanners and multifunction printers of the last
 * ten years speak. No driver is needed. And finding scanners with
 * multicast DNS (Bonjour), as NetStats finds names: a query for
 * _uscan._tcp.local sent to each address, answered straight back.
 *
 * Nothing here waits: a job or a search is moved on by calling its step
 * function, which does what it can without blocking (non-blocking sockets)
 * and returns. The desktop calls it on null polls; the tests in a loop.
 * No MuPDF or Wimp code, so it runs (and is tested) on Linux too. */
#ifndef ESCL_H
#define ESCL_H

#include <stddef.h>

/* ---- What a scanner can do (ScannerCapabilities) ------------------------ */

enum { ESCL_PLATEN = 0, ESCL_FEEDER = 1 };

typedef struct {
	char model[128];        /* MakeAndModel, desktop text (UTF-8) */
	char version[16];       /* the eSCL version it speaks, e.g. "2.63" */
	int has[2];             /* [ESCL_PLATEN], [ESCL_FEEDER]: it has one */
	int duplex;             /* the feeder can do both sides */
	int max_w[2], max_h[2]; /* largest area, 1/300 inch */
	int res[32], nres;      /* resolutions offered (dpi), smallest first */
	int rgb, gray, bw;      /* colour modes offered */
	int jpeg, pdf, png;     /* page formats offered */
} escl_caps;

/* Reads ScannerCapabilities XML. 0, or -1 if it isn't that. */
int escl_parse_caps(const char *xml, size_t len, escl_caps *c);

/* ---- A scan ---------------------------------------------------------------- */

enum { ESCL_RGB = 0, ESCL_GRAY = 1 };

typedef struct {
	int source;             /* ESCL_PLATEN or ESCL_FEEDER */
	int duplex;             /* with the feeder: both sides */
	int color;              /* ESCL_RGB or ESCL_GRAY */
	int dpi;                /* the nearest the scanner offers is used */
	int width, height;      /* 1/300 inch; 0 = the largest it can */
} escl_settings;

/* The ScanSettings XML for s with the scanner's caps (dpi, size and format
 * fitted to what it offers), in buf. Returns its length, or -1. */
int escl_settings_xml(const escl_settings *s, const escl_caps *c, char *buf, size_t size);

typedef struct escl_job escl_job;

/* What escl_step returns. */
enum { ESCL_BUSY = 0, ESCL_PAGE = 1, ESCL_DONE = 2, ESCL_ERROR = -1 };

/* A scan from the scanner at host (an address or a name), port, with
 * root the path of its eSCL service ("eSCL" for most). NULL if host can't
 * be found (escl_last_error says why). */
escl_job *escl_job_new(const char *host, int port, const char *root, const escl_settings *s);
/* Moves it on. ESCL_PAGE: a page has arrived (escl_take_page); call again
 * for the next. ESCL_DONE: no more pages (or cancelled). */
int escl_step(escl_job *j);
/* The page that arrived: malloc'd bytes (the caller frees them), how many,
 * and what they are ("image/jpeg", "application/pdf", "image/png"). */
unsigned char *escl_take_page(escl_job *j, size_t *len, const char **mime);
/* What it's doing, for showing: "Scanning page 2"... */
const char *escl_status(escl_job *j);
const char *escl_error(escl_job *j);
const escl_caps *escl_job_caps(escl_job *j);
int escl_pages(escl_job *j);
/* The resolution scanned at (once the capabilities are in). */
int escl_job_dpi(escl_job *j);
/* Stops it: the scanner is told to drop the job; escl_step then returns
 * ESCL_DONE. */
void escl_cancel(escl_job *j);
void escl_job_free(escl_job *j);
const char *escl_last_error(void);
/* A function given each step of the network conversation, for the log. */
void escl_set_log(void (*fn)(const char *));
/* For the tests: sockets left blocking, as a stack that ignores the
 * request would leave them (nothing may wait even so). */
void escl_test_blocking(int on);

/* ---- Finding scanners (mDNS, _uscan._tcp) --------------------------------- */

typedef struct {
	char name[128];         /* the scanner's own name for itself (TXT ty, or the service name) */
	char host[48];          /* its address, dotted */
	int port;               /* SRV port (80 if it didn't say) */
	char root[64];          /* TXT rs ("eSCL" if it didn't say) */
} escl_found;

typedef struct escl_find escl_find;

/* Asks each of the n IPv4 addresses (host order) at port (5353 for real
 * mDNS; 224.0.0.251 may be one of them) for _uscan._tcp services, and
 * listens for answers for wait_cs centiseconds after the last is sent. */
escl_find *escl_find_new(const unsigned *addrs, int n, int port, int wait_cs);
/* 1 while it's still asking or listening, 0 when it's finished. */
int escl_find_step(escl_find *f);
int escl_find_results(escl_find *f, const escl_found **list);
void escl_find_free(escl_find *f);

/* Parses one mDNS answer from 'from' (dotted) into *out. 1 if it named an
 * eSCL scanner, 0 if not. (For the tests.) */
int escl_parse_mdns(const unsigned char *p, size_t n, const char *from, escl_found *out);
/* This machine's IPv4 address (host order), as the stack would send from
 * to reach 'towards' (host order); 0 if none. */
unsigned escl_local_address(unsigned towards);

#endif
