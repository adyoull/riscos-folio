/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* eSCL scanning and mDNS scanner discovery: see escl.h.
 *
 * eSCL in four requests (Mopria eSCL; as sane-airscan and NAPS2 use it):
 *   GET  /<root>/ScannerCapabilities   what it can do (XML)
 *   POST /<root>/ScanJobs              ScanSettings XML; 201 Created with
 *                                       Location: the job
 *   GET  <job>/NextDocument            a page (JPEG...) each time; 404 when
 *                                       there are no more; 503 while it's
 *                                       warming up (ask again)
 *   DELETE <job>                       to cancel
 * Each request is its own connection ("Connection: close"), made and read
 * a step at a time with a non-blocking socket. */
#include "escl.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <expat.h>

static char last_error[200];
static void (*log_fn)(const char *);

void escl_set_log(void (*fn)(const char *))
{
	log_fn = fn;
}

static void elog(const char *fmt, ...)
{
	char buf[300];
	va_list ap;
	if (!log_fn)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	log_fn(buf);
}

const char *escl_last_error(void)
{
	return last_error;
}

/* Centiseconds since some fixed time. */
static unsigned now_cs(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned)tv.tv_sec * 100u + (unsigned)tv.tv_usec / 10000u;
}

static int would_block(void)
{
	return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EALREADY ||
		errno == EINTR;
}

static int test_blocking;

void escl_test_blocking(int on)
{
	test_blocking = on;
}

/* Both ways UnixLib offers; 0 if either took. */
static int nonblocking(int fd)
{
	if (test_blocking)
		return 0;           /* the tests: a socket that stays blocking */
	int on = 1, a, b, fl;
	a = ioctl(fd, FIONBIO, &on);
	fl = fcntl(fd, F_GETFL, 0);
	b = fl == -1 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	return a == 0 || b == 0 ? 0 : -1;
}

/* Whether fd can be read (want_write 0) or written (1) now, without
 * waiting: send() and recv() are only called when it can, so nothing
 * blocks even if a socket didn't become non-blocking. */
static int ready(int fd, int want_write)
{
	fd_set s;
	struct timeval tv = { 0, 0 };
	FD_ZERO(&s);
	FD_SET(fd, &s);
	return select(fd + 1, want_write ? NULL : &s, want_write ? &s : NULL, NULL, &tv) > 0;
}

/* ---- ScannerCapabilities ------------------------------------------------- */

typedef struct {
	escl_caps *c;
	char path[8][40];           /* the elements we're in (local names) */
	int depth;
	int source;                 /* -1, or the source whose caps we're in */
	char text[256];
	size_t tlen;
} caps_parse;

static const char *local(const char *name)
{
	const char *c = strrchr(name, ':');
	return c ? c + 1 : name;
}

static int inside(const caps_parse *p, const char *name)
{
	int i;
	for (i = 0; i < p->depth && i < 8; i++)
		if (!strcmp(p->path[i], name))
			return 1;
	return 0;
}

static void XMLCALL caps_start(void *ud, const XML_Char *name, const XML_Char **atts)
{
	caps_parse *p = ud;
	const char *n = local(name);
	if (p->depth < 8)
		snprintf(p->path[p->depth], sizeof p->path[0], "%s", n);
	p->depth++;
	p->tlen = 0;
	p->text[0] = 0;
	if (!strcmp(n, "Platen")) {
		p->c->has[ESCL_PLATEN] = 1;
		p->source = ESCL_PLATEN;
	} else if (!strcmp(n, "Adf")) {
		p->c->has[ESCL_FEEDER] = 1;
		p->source = ESCL_FEEDER;
	} else if (!strcmp(n, "AdfDuplexInputCaps")) {
		p->c->duplex = 1;
	}
	(void)atts;
}

static void add_res(escl_caps *c, int r)
{
	int i, j;
	if (r <= 0 || c->nres >= (int)(sizeof c->res / sizeof c->res[0]))
		return;
	for (i = 0; i < c->nres; i++)
		if (c->res[i] == r)
			return;
	for (i = 0; i < c->nres && c->res[i] < r; i++)
		;
	for (j = c->nres; j > i; j--)
		c->res[j] = c->res[j - 1];
	c->res[i] = r;
	c->nres++;
}

static void XMLCALL caps_end(void *ud, const XML_Char *name)
{
	caps_parse *p = ud;
	escl_caps *c = p->c;
	const char *n = local(name);
	const char *t = p->text;
	while (*t == ' ' || *t == '\n' || *t == '\r' || *t == '\t')
		t++;
	if (!strcmp(n, "MakeAndModel") && !c->model[0]) {
		snprintf(c->model, sizeof c->model, "%.127s", t);
	} else if (!strcmp(n, "Version") && p->depth == 2) {
		snprintf(c->version, sizeof c->version, "%.15s", t);
	} else if (!strcmp(n, "MaxWidth") && p->source >= 0) {
		if (atoi(t) > c->max_w[p->source])
			c->max_w[p->source] = atoi(t);
	} else if (!strcmp(n, "MaxHeight") && p->source >= 0) {
		if (atoi(t) > c->max_h[p->source])
			c->max_h[p->source] = atoi(t);
	} else if (!strcmp(n, "ColorMode")) {
		if (!strcmp(t, "RGB24"))
			c->rgb = 1;
		else if (!strcmp(t, "Grayscale8"))
			c->gray = 1;
		else if (!strcmp(t, "BlackAndWhite1"))
			c->bw = 1;
	} else if (!strcmp(n, "DocumentFormat") || !strcmp(n, "DocumentFormatExt")) {
		if (!strcmp(t, "image/jpeg"))
			c->jpeg = 1;
		else if (!strcmp(t, "application/pdf"))
			c->pdf = 1;
		else if (!strcmp(t, "image/png"))
			c->png = 1;
	} else if (!strcmp(n, "XResolution") && inside(p, "DiscreteResolution")) {
		add_res(c, atoi(t));
	} else if (!strcmp(n, "Platen") || !strcmp(n, "Adf")) {
		p->source = -1;
	}
	p->depth--;
	p->tlen = 0;
	p->text[0] = 0;
}

static void XMLCALL caps_text(void *ud, const XML_Char *s, int len)
{
	caps_parse *p = ud;
	if (len > (int)(sizeof p->text - 1 - p->tlen))
		len = (int)(sizeof p->text - 1 - p->tlen);
	if (len <= 0)
		return;
	memcpy(p->text + p->tlen, s, (size_t)len);
	p->tlen += (size_t)len;
	p->text[p->tlen] = 0;
}

int escl_parse_caps(const char *xml, size_t len, escl_caps *c)
{
	XML_Parser x = XML_ParserCreate(NULL);
	caps_parse p;
	int ok;
	memset(c, 0, sizeof *c);
	memset(&p, 0, sizeof p);
	p.c = c;
	p.source = -1;
	if (!x)
		return -1;
	XML_SetUserData(x, &p);
	XML_SetElementHandler(x, caps_start, caps_end);
	XML_SetCharacterDataHandler(x, caps_text);
	ok = XML_Parse(x, xml, (int)len, 1) == XML_STATUS_OK;
	XML_ParserFree(x);
	/* Something that is eSCL: a source, or at least a model. */
	return ok && (c->has[0] || c->has[1] || c->model[0]) ? 0 : -1;
}

/* ---- ScanSettings ----------------------------------------------------------- */

/* The resolution it offers nearest dpi (dpi itself if it doesn't list any). */
static int nearest_dpi(const escl_caps *c, int dpi)
{
	int i, best;
	if (dpi <= 0)
		dpi = 300;
	if (c->nres <= 0)
		return dpi;
	best = c->res[0];
	for (i = 0; i < c->nres; i++)
		if (abs(c->res[i] - dpi) < abs(best - dpi))
			best = c->res[i];
	return best;
}

int escl_settings_xml(const escl_settings *s, const escl_caps *c, char *buf, size_t size)
{
	int src = s->source == ESCL_FEEDER ? ESCL_FEEDER : ESCL_PLATEN;
	int dpi = nearest_dpi(c, s->dpi), w, h, n, ext;
	const char *mode, *fmt, *ver = c->version[0] ? c->version : "2.0";
	/* The area: as asked, else the largest; never more than it can. */
	w = s->width > 0 ? s->width : c->max_w[src] > 0 ? c->max_w[src] : 2480;
	h = s->height > 0 ? s->height : c->max_h[src] > 0 ? c->max_h[src] : 3508;
	if (c->max_w[src] > 0 && w > c->max_w[src])
		w = c->max_w[src];
	if (c->max_h[src] > 0 && h > c->max_h[src])
		h = c->max_h[src];
	/* Colour or grey, as offered. */
	if (s->color == ESCL_GRAY)
		mode = c->gray || !c->rgb ? "Grayscale8" : "RGB24";
	else
		mode = c->rgb || !c->gray ? "RGB24" : "Grayscale8";
	/* JPEG if it can (the pages go into the PDF as they are), else PDF. */
	fmt = c->jpeg || (!c->pdf && !c->png) ? "image/jpeg" : c->pdf ? "application/pdf" : "image/png";
	ext = strcmp(ver, "2.1") >= 0;
	n = snprintf(buf, size,
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<scan:ScanSettings xmlns:scan=\"http://schemas.hp.com/imaging/escl/2011/05/03\" "
		"xmlns:pwg=\"http://www.pwg.org/schemas/2010/12/sm\">\n"
		"  <pwg:Version>%s</pwg:Version>\n"
		"  <pwg:ScanRegions>\n"
		"    <pwg:ScanRegion>\n"
		"      <pwg:ContentRegionUnits>escl:ThreeHundredthsOfInches</pwg:ContentRegionUnits>\n"
		"      <pwg:XOffset>0</pwg:XOffset>\n"
		"      <pwg:YOffset>0</pwg:YOffset>\n"
		"      <pwg:Width>%d</pwg:Width>\n"
		"      <pwg:Height>%d</pwg:Height>\n"
		"    </pwg:ScanRegion>\n"
		"  </pwg:ScanRegions>\n"
		"  <pwg:InputSource>%s</pwg:InputSource>\n"
		"%s"
		"  <scan:ColorMode>%s</scan:ColorMode>\n"
		"  <scan:XResolution>%d</scan:XResolution>\n"
		"  <scan:YResolution>%d</scan:YResolution>\n"
		"  <pwg:DocumentFormat>%s</pwg:DocumentFormat>\n"
		"%s%s%s"
		"</scan:ScanSettings>\n",
		ver, w, h, src == ESCL_FEEDER ? "Feeder" : "Platen",
		src == ESCL_FEEDER ? (s->duplex && c->duplex ? "  <scan:Duplex>true</scan:Duplex>\n" :
			"  <scan:Duplex>false</scan:Duplex>\n") : "",
		mode, dpi, dpi, fmt,
		ext ? "  <scan:DocumentFormatExt>" : "", ext ? fmt : "", ext ? "</scan:DocumentFormatExt>\n" : "");
	return n > 0 && (size_t)n < size ? n : -1;
}

/* ---- HTTP, a step at a time ---------------------------------------------- */

enum { H_IDLE, H_CONNECT, H_SEND, H_RECV, H_DONE, H_ERROR };

typedef struct {
	int state, fd;
	char *out;
	size_t out_len, out_pos;
	unsigned char *in;          /* everything received */
	size_t in_len, in_cap;
	size_t head;                /* bytes of the head (status line and headers), 0 until whole */
	int status;
	long clen;                  /* Content-Length, or -1 */
	int chunked;
	size_t cpos;                /* chunked: where the next chunk's size line starts */
	unsigned char *body;        /* the body (chunks joined) */
	size_t body_len, body_cap;
	char location[512];
	char ctype[64];
	unsigned deadline;          /* centiseconds: give up if nothing happens by then */
	char err[160];
} http_req;

static void http_close(http_req *h)
{
	if (h->fd >= 0)
		close(h->fd);
	h->fd = -1;
}

static void http_free(http_req *h)
{
	http_close(h);
	free(h->out);
	free(h->in);
	free(h->body);
	memset(h, 0, sizeof *h);
	h->fd = -1;
}

static int http_fail(http_req *h, const char *fmt, const char *what)
{
	snprintf(h->err, sizeof h->err, fmt, what);
	http_close(h);
	h->state = H_ERROR;
	return -1;
}

static int http_start(http_req *h, unsigned addr, int port, const char *method, const char *path,
		const char *host_hdr, const char *body, size_t blen)
{
	struct sockaddr_in sa;
	size_t need;
	http_free(h);
	h->clen = -1;
	need = strlen(method) + strlen(path) + strlen(host_hdr) + 200 + blen;
	h->out = malloc(need);
	if (!h->out)
		return http_fail(h, "%s", "not enough memory");
	if (body)
		h->out_len = (size_t)snprintf(h->out, need, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Folio\r\n"
			"Accept: */*\r\nConnection: close\r\nContent-Type: text/xml\r\nContent-Length: %lu\r\n\r\n",
			method, path, host_hdr, (unsigned long)blen);
	else
		h->out_len = (size_t)snprintf(h->out, need, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Folio\r\n"
			"Accept: */*\r\nConnection: close\r\n\r\n", method, path, host_hdr);
	if (body && blen) {
		memcpy(h->out + h->out_len, body, blen);
		h->out_len += blen;
	}
	h->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (h->fd < 0)
		return http_fail(h, "no socket (%s)", strerror(errno));
	if (nonblocking(h->fd)) {
		elog("escl: the socket can't be made non-blocking (%s)", strerror(errno));
		return http_fail(h, "%s", "the network stack wouldn't let Folio wait for the scanner in the background");
	}
	elog("escl: %s %s: connecting to %u.%u.%u.%u:%d", method, path, addr >> 24, (addr >> 16) & 255,
		(addr >> 8) & 255, addr & 255, port);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	sa.sin_addr.s_addr = htonl(addr);
	if (connect(h->fd, (struct sockaddr *)&sa, sizeof sa) < 0 && !would_block()) {
		elog("escl: connect: %s", strerror(errno));
		return http_fail(h, "can't connect to the scanner (%s)", strerror(errno));
	}
	h->state = H_CONNECT;
	h->deadline = now_cs() + 1500;   /* 15 s to answer */
	return 0;
}

/* Appends to the body. */
static int body_add(http_req *h, const unsigned char *p, size_t n)
{
	if (h->body_len + n + 1 > h->body_cap) {
		size_t cap = h->body_cap ? h->body_cap : 65536;
		unsigned char *b;
		while (cap < h->body_len + n + 1)
			cap *= 2;
		b = realloc(h->body, cap);
		if (!b)
			return -1;
		h->body = b;
		h->body_cap = cap;
	}
	memcpy(h->body + h->body_len, p, n);
	h->body_len += n;
	h->body[h->body_len] = 0;
	return 0;
}

static const char *header(const http_req *h, const char *name, char *buf, size_t size)
{
	const char *p = (const char *)h->in, *end = p + h->head;
	size_t nl = strlen(name);
	while (p < end) {
		const char *e = memchr(p, '\n', (size_t)(end - p));
		if (!e)
			break;
		if ((size_t)(e - p) > nl + 1 && !strncasecmp(p, name, nl) && p[nl] == ':') {
			const char *v = p + nl + 1;
			size_t n;
			while (*v == ' ' || *v == '\t')
				v++;
			n = (size_t)(e - v);
			while (n && (v[n - 1] == '\r' || v[n - 1] == ' '))
				n--;
			if (n >= size)
				n = size - 1;
			memcpy(buf, v, n);
			buf[n] = 0;
			return buf;
		}
		p = e + 1;
	}
	return NULL;
}

/* The head has arrived: status, length, chunking, Location. */
static int parse_head(http_req *h)
{
	char v[512];
	if (sscanf((const char *)h->in, "HTTP/%*d.%*d %d", &h->status) != 1)
		return -1;
	if (header(h, "Content-Length", v, sizeof v))
		h->clen = atol(v);
	if (header(h, "Transfer-Encoding", v, sizeof v) && strstr(v, "chunked"))
		h->chunked = 1, h->clen = -1;
	if (header(h, "Location", v, sizeof v))
		snprintf(h->location, sizeof h->location, "%s", v);
	if (header(h, "Content-Type", v, sizeof v)) {
		char *semi = strchr(v, ';');
		if (semi)
			*semi = 0;
		snprintf(h->ctype, sizeof h->ctype, "%.63s", v);
	}
	h->cpos = h->head;
	return 0;
}

/* Joins whole chunks; 1 when the last (size 0) has come. */
static int unchunk(http_req *h)
{
	for (;;) {
		const unsigned char *p = h->in + h->cpos, *end = h->in + h->in_len, *nl;
		unsigned long size;
		nl = memchr(p, '\n', (size_t)(end - p));
		if (!nl)
			return 0;
		size = strtoul((const char *)p, NULL, 16);
		if (size == 0)
			return 1;
		if ((size_t)(end - (nl + 1)) < size + 2)
			return 0;
		if (body_add(h, nl + 1, size))
			return -1;
		h->cpos = (size_t)(nl + 1 + size + 2 - h->in);
	}
}

/* 0 still going, 1 finished (status, body), -1 failed (err). */
static int http_step(http_req *h)
{
	unsigned now = now_cs();
	if (h->state == H_DONE)
		return 1;
	if (h->state == H_ERROR)
		return -1;
	if ((int)(now - h->deadline) > 0)
		return http_fail(h, "%s", h->state == H_CONNECT ? "the scanner didn't answer" :
			"the scanner stopped answering");
	if (h->state == H_CONNECT) {
		fd_set w;
		struct timeval tv = { 0, 0 };
		int err = 0;
		socklen_t el = sizeof err;
		FD_ZERO(&w);
		FD_SET(h->fd, &w);
		if (select(h->fd + 1, NULL, &w, NULL, &tv) <= 0)
			return 0;
		if (getsockopt(h->fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err) {
			elog("escl: connect: %s", strerror(err));
			return http_fail(h, "can't connect to the scanner (%s)", strerror(err));
		}
		elog("escl: connected");
		h->state = H_SEND;
	}
	if (h->state == H_SEND) {
		while (h->out_pos < h->out_len) {
			ssize_t n;
			if (!ready(h->fd, 1))
				return 0;
			n = send(h->fd, h->out + h->out_pos, h->out_len - h->out_pos, 0);
			if (n < 0) {
				if (would_block() || errno == ENOTCONN)
					return 0;
				return http_fail(h, "couldn't send to the scanner (%s)", strerror(errno));
			}
			h->out_pos += (size_t)n;
		}
		elog("escl: sent %lu bytes", (unsigned long)h->out_len);
		h->state = H_RECV;
		/* A page can take a while: the scanner warms up and moves. */
		h->deadline = now + 18000;
	}
	if (h->state == H_RECV) {
		int closed = 0, reads;
		for (reads = 0; reads < 32; reads++) {
			ssize_t n;
			if (!ready(h->fd, 0))
				break;
			if (h->in_cap - h->in_len < 32768) {
				size_t cap = h->in_cap ? h->in_cap * 2 : 65536;
				unsigned char *b = realloc(h->in, cap + 1);
				if (!b)
					return http_fail(h, "%s", "not enough memory");
				h->in = b;
				h->in_cap = cap;
			}
			n = recv(h->fd, h->in + h->in_len, h->in_cap - h->in_len, 0);
			if (n < 0) {
				if (would_block())
					break;
				return http_fail(h, "couldn't read from the scanner (%s)", strerror(errno));
			}
			if (n == 0) {
				closed = 1;
				break;
			}
			h->in_len += (size_t)n;
			h->in[h->in_len] = 0;
			h->deadline = now + 18000;
		}
		if (!h->head && h->in_len) {
			unsigned char *e = NULL;
			size_t i;
			for (i = 0; i + 3 < h->in_len; i++)
				if (h->in[i] == '\r' && h->in[i + 1] == '\n' && h->in[i + 2] == '\r' && h->in[i + 3] == '\n') {
					e = h->in + i + 4;
					break;
				}
			if (e) {
				h->head = (size_t)(e - h->in);
				if (parse_head(h))
					return http_fail(h, "%s", "the scanner's answer wasn't HTTP");
				elog("escl: HTTP %d, %s%ld bytes", h->status, h->chunked ? "chunked, " : "", h->clen);
			}
		}
		if (h->head) {
			int done = 0;
			if (h->chunked) {
				int r = unchunk(h);
				if (r < 0)
					return http_fail(h, "%s", "not enough memory");
				done = r == 1;
			} else if (h->clen >= 0) {
				if (h->in_len - h->head >= (size_t)h->clen)
					done = 1;
			} else {
				done = closed;
			}
			/* 1xx, 204, 304 and HEAD have no body. */
			if (h->status == 204 || h->status == 304 || (h->status >= 100 && h->status < 200))
				done = 1;
			if (done || closed) {
				if (!h->chunked) {
					size_t n = h->in_len - h->head;
					if (h->clen >= 0 && n > (size_t)h->clen)
						n = (size_t)h->clen;
					if (body_add(h, h->in + h->head, n))
						return http_fail(h, "%s", "not enough memory");
				}
				if (!done && h->chunked)
					return http_fail(h, "%s", "the scanner's answer was cut short");
				if (!done && h->clen >= 0)
					return http_fail(h, "%s", "the scanner's answer was cut short");
				http_close(h);
				h->state = H_DONE;
				elog("escl: answer complete, %lu bytes", (unsigned long)h->body_len);
				return 1;
			}
		} else if (closed) {
			return http_fail(h, "%s", "the scanner closed the connection without answering");
		}
	}
	return 0;
}

/* ---- A scan job ------------------------------------------------------------ */

enum { J_CAPS, J_POST, J_NEXT, J_WAIT, J_DELETE, J_DONE, J_ERROR };

struct escl_job {
	int state, after_wait;
	unsigned addr;
	int port;
	char host_hdr[80];
	char root[80];              /* "/eSCL" ("" for the top) */
	char job[512];              /* the job's path */
	escl_settings s;
	escl_caps caps;
	http_req h;
	int tries;
	unsigned wait_until;
	int pages;
	int cancelled;
	unsigned char *page;
	size_t page_len;
	char page_mime[64];
	char status[120];
	char err[240];
};

static int resolve(const char *host, unsigned *addr)
{
	struct in_addr ia;
	struct hostent *he;
	if (inet_aton(host, &ia)) {
		*addr = ntohl(ia.s_addr);
		return 0;
	}
	he = gethostbyname(host);
	if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0])
		return -1;
	memcpy(&ia, he->h_addr_list[0], sizeof ia);
	*addr = ntohl(ia.s_addr);
	return 0;
}

static void request(escl_job *j, const char *method, const char *path, const char *body)
{
	if (http_start(&j->h, j->addr, j->port, method, path, j->host_hdr, body, body ? strlen(body) : 0))
		j->h.state = H_ERROR;
}

escl_job *escl_job_new(const char *host, int port, const char *root, const escl_settings *s)
{
	escl_job *j = calloc(1, sizeof *j);
	char path[600];
	const char *r = root ? root : "eSCL";
	/* A scanner that closes its end while Folio is still sending would
	 * otherwise stop Folio with SIGPIPE: send() returns EPIPE instead. */
	signal(SIGPIPE, SIG_IGN);
	if (!j) {
		snprintf(last_error, sizeof last_error, "not enough memory");
		return NULL;
	}
	j->h.fd = -1;
	if (resolve(host, &j->addr)) {
		snprintf(last_error, sizeof last_error, "Folio can't find '%.100s' on the network", host);
		free(j);
		return NULL;
	}
	j->port = port > 0 ? port : 80;
	snprintf(j->host_hdr, sizeof j->host_hdr, j->port == 80 ? "%s" : "%s:%d", host, j->port);
	while (*r == '/')
		r++;
	snprintf(j->root, sizeof j->root, "%s%.70s", *r ? "/" : "", r);
	while (j->root[0] && j->root[strlen(j->root) - 1] == '/')
		j->root[strlen(j->root) - 1] = 0;
	j->s = *s;
	snprintf(path, sizeof path, "%s/ScannerCapabilities", j->root);
	snprintf(j->status, sizeof j->status, "Asking the scanner what it can do");
	request(j, "GET", path, NULL);
	j->state = J_CAPS;
	return j;
}

static int job_fail(escl_job *j, const char *msg)
{
	snprintf(j->err, sizeof j->err, "%s", msg);
	http_free(&j->h);
	j->state = J_ERROR;
	return ESCL_ERROR;
}

static void job_wait(escl_job *j, int cs, int then)
{
	http_free(&j->h);
	j->wait_until = now_cs() + (unsigned)cs;
	j->after_wait = then;
	j->state = J_WAIT;
}

static void next_document(escl_job *j)
{
	char path[600];
	snprintf(path, sizeof path, "%.500s/NextDocument", j->job);
	snprintf(j->status, sizeof j->status, "Scanning page %d", j->pages + 1);
	request(j, "GET", path, NULL);
	j->state = J_NEXT;
}

static void post_job(escl_job *j)
{
	char xml[2048], path[600];
	if (escl_settings_xml(&j->s, &j->caps, xml, sizeof xml) < 0) {
		job_fail(j, "Folio couldn't write the scan settings");
		return;
	}
	snprintf(path, sizeof path, "%s/ScanJobs", j->root);
	snprintf(j->status, sizeof j->status, "Starting the scan");
	request(j, "POST", path, xml);
	j->state = J_POST;
}

/* The job's path from Location (a whole URL, or a path). */
static void job_path(escl_job *j, const char *loc)
{
	const char *p = loc;
	if (!strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8)) {
		p = strchr(p + 8, '/');
		if (!p)
			p = "/";
	}
	snprintf(j->job, sizeof j->job, "%s%.500s", *p == '/' ? "" : "/", p);
	while (j->job[0] && j->job[strlen(j->job) - 1] == '/')
		j->job[strlen(j->job) - 1] = 0;
}

int escl_step(escl_job *j)
{
	int r;
	char m[240];
	switch (j->state) {
	case J_DONE:
		return ESCL_DONE;
	case J_ERROR:
		return ESCL_ERROR;
	case J_WAIT:
		if ((int)(now_cs() - j->wait_until) < 0)
			return ESCL_BUSY;
		if (j->after_wait == J_POST)
			post_job(j);
		else
			next_document(j);
		return ESCL_BUSY;
	default:
		break;
	}
	r = http_step(&j->h);
	if (r == 0)
		return ESCL_BUSY;
	if (r < 0) {
		if (j->state == J_DELETE) {
			j->state = J_DONE;
			return ESCL_DONE;
		}
		snprintf(m, sizeof m, "%s%.159s", j->h.err[0] ? "" : "the scanner didn't answer", j->h.err);
		m[0] = (char)toupper((unsigned char)m[0]);
		return job_fail(j, m);
	}
	switch (j->state) {
	case J_CAPS:
		if (j->h.status != 200 || escl_parse_caps((const char *)j->h.body, j->h.body_len, &j->caps)) {
			snprintf(m, sizeof m, j->h.status == 200 ? "That doesn't answer as an eSCL scanner" :
				"That doesn't answer as an eSCL scanner (HTTP %d)", j->h.status);
			return job_fail(j, m);
		}
		if (j->s.source == ESCL_FEEDER && !j->caps.has[ESCL_FEEDER] && j->caps.has[ESCL_PLATEN])
			return job_fail(j, "This scanner has no document feeder: choose the glass");
		if (j->s.source == ESCL_PLATEN && !j->caps.has[ESCL_PLATEN] && j->caps.has[ESCL_FEEDER])
			return job_fail(j, "This scanner has only a document feeder: choose the feeder");
		post_job(j);
		return ESCL_BUSY;
	case J_POST:
		if ((j->h.status == 201 || j->h.status == 200) && j->h.location[0]) {
			job_path(j, j->h.location);
			j->tries = 0;
			next_document(j);
			return ESCL_BUSY;
		}
		if (j->h.status == 503 && ++j->tries < 15) {
			snprintf(j->status, sizeof j->status, "Waiting for the scanner (it's busy)");
			job_wait(j, 200, J_POST);
			return ESCL_BUSY;
		}
		if (j->h.status == 409)
			return job_fail(j, j->s.source == ESCL_FEEDER ? "There's no paper in the feeder" :
				"The scanner is busy, or the lid or feeder needs attention");
		snprintf(m, sizeof m, "The scanner turned the scan down (HTTP %d)", j->h.status);
		return job_fail(j, m);
	case J_NEXT:
		if (j->h.status == 200 && j->h.body_len > 0) {
			free(j->page);
			j->page = j->h.body;
			j->page_len = j->h.body_len;
			snprintf(j->page_mime, sizeof j->page_mime, "%.63s", j->h.ctype[0] ? j->h.ctype :
				j->caps.jpeg ? "image/jpeg" : "application/pdf");
			j->h.body = NULL;
			j->h.body_len = j->h.body_cap = 0;
			j->pages++;
			j->tries = 0;
			if (j->cancelled) {
				j->state = J_DONE;
				return ESCL_PAGE;
			}
			/* The glass: one page. The feeder: ask until there are no more. */
			if (j->s.source == ESCL_PLATEN) {
				http_free(&j->h);
				snprintf(j->status, sizeof j->status, "Scanned");
				j->state = J_DONE;
			} else {
				next_document(j);
			}
			return ESCL_PAGE;
		}
		if (j->h.status == 503 && ++j->tries < 120) {
			snprintf(j->status, sizeof j->status, "Waiting for the scanner (page %d)", j->pages + 1);
			job_wait(j, 100, J_NEXT);
			return ESCL_BUSY;
		}
		if ((j->h.status == 404 || j->h.status == 410 || j->h.status == 409) && j->pages > 0) {
			http_free(&j->h);
			snprintf(j->status, sizeof j->status, "Scanned %d page%s", j->pages, j->pages == 1 ? "" : "s");
			j->state = J_DONE;
			return ESCL_DONE;
		}
		if (j->h.status == 404 || j->h.status == 409)
			return job_fail(j, j->s.source == ESCL_FEEDER ? "There's no paper in the feeder" :
				"The scanner sent no pages");
		snprintf(m, sizeof m, "The scanner stopped (HTTP %d)", j->h.status);
		return job_fail(j, m);
	case J_DELETE:
		http_free(&j->h);
		j->state = J_DONE;
		return ESCL_DONE;
	}
	return ESCL_BUSY;
}

unsigned char *escl_take_page(escl_job *j, size_t *len, const char **mime)
{
	unsigned char *p = j->page;
	*len = j->page_len;
	*mime = j->page_mime;
	j->page = NULL;
	j->page_len = 0;
	return p;
}

void escl_cancel(escl_job *j)
{
	char path[600];
	if (j->state == J_DONE || j->state == J_ERROR)
		return;
	j->cancelled = 1;
	snprintf(j->status, sizeof j->status, "Stopping");
	if (j->job[0]) {
		snprintf(path, sizeof path, "%.590s", j->job);
		request(j, "DELETE", path, NULL);
		j->state = J_DELETE;
	} else {
		http_free(&j->h);
		j->state = J_DONE;
	}
}

const char *escl_status(escl_job *j) { return j->status; }
const char *escl_error(escl_job *j) { return j->err; }
const escl_caps *escl_job_caps(escl_job *j) { return &j->caps; }
int escl_pages(escl_job *j) { return j->pages; }
int escl_job_dpi(escl_job *j) { return nearest_dpi(&j->caps, j->s.dpi); }

void escl_job_free(escl_job *j)
{
	if (!j)
		return;
	http_free(&j->h);
	free(j->page);
	free(j);
}

/* ---- mDNS: finding scanners ---------------------------------------------- */

#define FIND_MAX 32

struct escl_find {
	int fd, port;
	unsigned *addrs;
	int n, sent;
	unsigned end;               /* stop listening at */
	int wait_cs;
	unsigned short id;
	escl_found found[FIND_MAX];
	int nfound;
};

/* The question: _uscan._tcp.local PTR, "unicast reply please" (QU). */
static size_t mdns_query(unsigned char *q, unsigned short id)
{
	static const char *const labels[] = { "_uscan", "_tcp", "local", NULL };
	size_t n = 12;
	int i;
	memset(q, 0, 12);
	q[0] = (unsigned char)(id >> 8);
	q[1] = (unsigned char)id;
	q[5] = 1;                   /* one question */
	for (i = 0; labels[i]; i++) {
		size_t l = strlen(labels[i]);
		q[n++] = (unsigned char)l;
		memcpy(q + n, labels[i], l);
		n += l;
	}
	q[n++] = 0;
	q[n++] = 0; q[n++] = 12;            /* PTR */
	q[n++] = 0x80; q[n++] = 1;          /* IN, unicast response */
	return n;
}

/* Reads the name at off into out (dotted); returns the offset after it
 * where it is (pointers followed for the text only), or -1. */
static int dns_name(const unsigned char *p, size_t n, size_t off, char *out, size_t size)
{
	size_t o = off, len = 0;
	int jumped = 0, after = -1, hops = 0;
	if (size)
		out[0] = 0;
	for (;;) {
		unsigned c;
		if (o >= n)
			return -1;
		c = p[o];
		if (c == 0) {
			if (!jumped)
				after = (int)(o + 1);
			break;
		}
		if ((c & 0xc0) == 0xc0) {
			if (o + 1 >= n || ++hops > 32)
				return -1;
			if (!jumped)
				after = (int)(o + 2);
			jumped = 1;
			o = ((c & 0x3f) << 8) | p[o + 1];
			continue;
		}
		if (c > 63 || o + 1 + c > n)
			return -1;
		if (len + c + 2 < size) {
			if (len)
				out[len++] = '.';
			memcpy(out + len, p + o + 1, c);
			len += c;
			out[len] = 0;
		}
		o += 1 + c;
	}
	return after;
}

static int ends_with(const char *s, const char *tail)
{
	size_t a = strlen(s), b = strlen(tail);
	return a >= b && !strcasecmp(s + a - b, tail);
}

int escl_parse_mdns(const unsigned char *p, size_t n, const char *from, escl_found *out)
{
	char owner[256], instance[256] = "", ty[128] = "", rs[64] = "", txt_owner[256] = "";
	int qd, an, ns, ar, i, off, port = 0, srv_any = 0, have_rs = 0;
	if (n < 12 || !(p[2] & 0x80))
		return 0;               /* not an answer */
	qd = (p[4] << 8) | p[5];
	an = (p[6] << 8) | p[7];
	ns = (p[8] << 8) | p[9];
	ar = (p[10] << 8) | p[11];
	off = 12;
	for (i = 0; i < qd; i++) {
		off = dns_name(p, n, (size_t)off, owner, sizeof owner);
		if (off < 0 || (size_t)off + 4 > n)
			return 0;
		off += 4;
	}
	for (i = 0; i < an + ns + ar; i++) {
		int type, rdlen, rd;
		off = dns_name(p, n, (size_t)off, owner, sizeof owner);
		if (off < 0 || (size_t)off + 10 > n)
			break;
		type = (p[off] << 8) | p[off + 1];
		rdlen = (p[off + 8] << 8) | p[off + 9];
		rd = off + 10;
		if ((size_t)rd + (size_t)rdlen > n)
			break;
		if (type == 12 && ends_with(owner, "_uscan._tcp.local")) {
			dns_name(p, n, (size_t)rd, instance, sizeof instance);
		} else if (type == 33 && rdlen >= 7) {
			/* SRV: priority, weight, port, target. Ours if it's the
			 * instance's (or the only one). */
			int prt = (p[rd + 4] << 8) | p[rd + 5];
			if (!strcasecmp(owner, instance) || ends_with(owner, "_uscan._tcp.local"))
				port = prt;
			else if (!srv_any)
				srv_any = prt;
		} else if (type == 16 && ends_with(owner, "_uscan._tcp.local")) {
			int k = rd;
			snprintf(txt_owner, sizeof txt_owner, "%s", owner);
			while (k < rd + rdlen) {
				int l = p[k];
				char kv[256];
				if (k + 1 + l > rd + rdlen)
					break;
				memcpy(kv, p + k + 1, (size_t)l);
				kv[l] = 0;
				if (!strncasecmp(kv, "ty=", 3))
					snprintf(ty, sizeof ty, "%.127s", kv + 3);
				else if (!strncasecmp(kv, "rs=", 3)) {
					snprintf(rs, sizeof rs, "%.63s", kv + 3);
					have_rs = 1;
				}
				k += 1 + l;
			}
		}
		off = rd + rdlen;
	}
	if (!instance[0] && !txt_owner[0])
		return 0;
	memset(out, 0, sizeof *out);
	if (ty[0]) {
		snprintf(out->name, sizeof out->name, "%.127s", ty);
	} else {
		/* The service name's first label ("Brother MFC-L2710DW._uscan..."). */
		const char *src = instance[0] ? instance : txt_owner;
		const char *dot = strstr(src, "._uscan");
		int l = dot ? (int)(dot - src) : (int)strlen(src);
		snprintf(out->name, sizeof out->name, "%.*s", l, src);
	}
	snprintf(out->host, sizeof out->host, "%s", from);
	out->port = port ? port : srv_any ? srv_any : 80;
	{
		const char *r = have_rs ? rs : "eSCL";
		while (*r == '/')
			r++;
		snprintf(out->root, sizeof out->root, "%.63s", r);
	}
	return 1;
}

escl_find *escl_find_new(const unsigned *addrs, int n, int port, int wait_cs)
{
	escl_find *f = calloc(1, sizeof *f);
	struct sockaddr_in sa;
	if (!f)
		return NULL;
	f->addrs = malloc(sizeof *addrs * (size_t)(n > 0 ? n : 1));
	if (!f->addrs) {
		free(f);
		return NULL;
	}
	memcpy(f->addrs, addrs, sizeof *addrs * (size_t)n);
	f->n = n;
	f->port = port;
	f->wait_cs = wait_cs;
	f->id = (unsigned short)(now_cs() * 2654435761u >> 16);
	f->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (f->fd < 0) {
		snprintf(last_error, sizeof last_error, "no socket (%s)", strerror(errno));
		free(f->addrs);
		free(f);
		return NULL;
	}
	if (nonblocking(f->fd))
		elog("escl: mDNS socket isn't non-blocking (%s); reading only when ready", strerror(errno));
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	bind(f->fd, (struct sockaddr *)&sa, sizeof sa);
	return f;
}

int escl_find_step(escl_find *f)
{
	unsigned char buf[4096];
	int i;
	/* Ask 32 more. */
	if (f->sent < f->n) {
		unsigned char q[64];
		size_t ql = mdns_query(q, f->id);
		for (i = 0; i < 32 && f->sent < f->n; i++, f->sent++) {
			struct sockaddr_in sa;
			memset(&sa, 0, sizeof sa);
			sa.sin_family = AF_INET;
			sa.sin_port = htons((unsigned short)f->port);
			sa.sin_addr.s_addr = htonl(f->addrs[f->sent]);
			sendto(f->fd, q, ql, 0, (struct sockaddr *)&sa, sizeof sa);
		}
		if (f->sent >= f->n)
			f->end = now_cs() + (unsigned)f->wait_cs;
	}
	/* Read what's come. */
	for (i = 0; i < 64 && ready(f->fd, 0); i++) {
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		ssize_t got = recvfrom(f->fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		escl_found one;
		char host[48];
		if (got < 0)
			break;
		if (got < 12)
			continue;
		/* Ours: our ID, or 0 (some responders send that). */
		if (((buf[0] << 8) | buf[1]) != f->id && ((buf[0] << 8) | buf[1]) != 0)
			continue;
		snprintf(host, sizeof host, "%s", inet_ntoa(from.sin_addr));
		if (escl_parse_mdns(buf, (size_t)got, host, &one)) {
			int k, dup = 0;
			for (k = 0; k < f->nfound; k++)
				if (!strcmp(f->found[k].host, one.host) && f->found[k].port == one.port)
					dup = 1;
			if (!dup && f->nfound < FIND_MAX)
				f->found[f->nfound++] = one;
		}
	}
	return f->sent < f->n || (int)(now_cs() - f->end) < 0;
}

int escl_find_results(escl_find *f, const escl_found **list)
{
	*list = f->found;
	return f->nfound;
}

void escl_find_free(escl_find *f)
{
	if (!f)
		return;
	if (f->fd >= 0)
		close(f->fd);
	free(f->addrs);
	free(f);
}

unsigned escl_local_address(unsigned towards)
{
	struct sockaddr_in sa;
	socklen_t l = sizeof sa;
	unsigned a = 0;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return 0;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(9);
	sa.sin_addr.s_addr = htonl(towards);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0 &&
			getsockname(fd, (struct sockaddr *)&sa, &l) == 0)
		a = ntohl(sa.sin_addr.s_addr);
	close(fd);
	return a;
}
