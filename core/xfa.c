/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* XFA forms (Adobe's "LiveCycle" forms), filled in as a list of fields.
 *
 * An XFA form keeps its design in the template packet and its values in
 * the datasets packet, in the AcroForm's XFA entry (one stream, or an
 * array of name, stream pairs). MuPDF can't lay out an XFA design, so a
 * dynamic form shows only its "Please wait..." page; a static ("hybrid")
 * form also has ordinary fields, which Folio fills in on the page.
 *
 * Here the datasets are read for the values (one per leaf element, by its
 * dotted path: form1.Page1.Name, form1.Item[2].Price), and the template
 * for each field's caption, kind (text, number, date, check box, choice
 * list, radio group) and choices, matched by the same path (normal
 * binding: named subforms and fields; unnamed subforms are transparent;
 * bind match="none" fields aren't kept; dataRef binding by its $./$record.
 * path; global by name). Template fields with no data yet are listed too,
 * and their elements are made when they're first filled in.
 *
 * Values are written back by editing the datasets packet's text where it
 * stands, so the rest of the XML is left byte for byte (MuPDF's XML
 * writer would drop the namespaces). On a hybrid form the matching
 * ordinary field is set too, and a change to an ordinary field is copied
 * into the datasets (folio_xfa_sync, from form.c), as Adobe Reader takes
 * an XFA form's values from its datasets. */
#include "folio_int.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- A small XML reader that remembers where things are ------------------- */

typedef struct xnode xnode;
struct xnode {
	char name[64];              /* local name (no prefix) */
	size_t start;               /* '<' of the start tag */
	size_t open_end;            /* just after the start tag's '>' */
	size_t close_start;         /* '<' of the end tag (== open_end if empty) */
	size_t close_end;           /* just after the end tag (or the empty tag) */
	int empty;                  /* <name/> */
	size_t att, att_len;        /* the attributes, as written */
	xnode *parent, *first, *last, *next;
};

typedef struct {
	const char *s;
	size_t n;
	xnode *nodes;
	int count, cap;
	int bad;
} xdoc;

static xnode *xnew(xdoc *d)
{
	if (d->count == d->cap) {
		int cap = d->cap ? d->cap * 2 : 256;
		xnode *n = realloc(d->nodes, (size_t)cap * sizeof *n);
		if (!n) {
			d->bad = 1;
			return NULL;
		}
		/* Re-point the links into the moved array. */
		if (n != d->nodes) {
			int i;
			ptrdiff_t delta = (char *)n - (char *)d->nodes;
			for (i = 0; i < d->count; i++) {
#define FIX(p) if (p) p = (xnode *)((char *)(p) + delta)
				FIX(n[i].parent); FIX(n[i].first); FIX(n[i].last); FIX(n[i].next);
#undef FIX
			}
		}
		d->nodes = n;
		d->cap = cap;
	}
	memset(&d->nodes[d->count], 0, sizeof d->nodes[0]);
	return &d->nodes[d->count++];
}

static const char *find(const char *s, size_t from, size_t n, const char *what)
{
	size_t wl = strlen(what), i;
	for (i = from; i + wl <= n; i++)
		if (s[i] == what[0] && !memcmp(s + i, what, wl))
			return s + i;
	return NULL;
}

/* Parses s[from, to). Nodes are kept by index while parsing (the array
 * moves), then linked. */
static int xparse(xdoc *d, const char *s, size_t n)
{
	size_t i = 0;
	int cur = -1;
	int *par;
	memset(d, 0, sizeof *d);
	d->s = s;
	d->n = n;
	par = NULL;
	while (i < n && !d->bad) {
		const char *e;
		if (s[i] != '<') {
			i++;
			continue;
		}
		if (i + 1 < n && (s[i + 1] == '?')) {
			e = find(s, i, n, "?>");
			i = e ? (size_t)(e - s) + 2 : n;
		} else if (i + 3 < n && !memcmp(s + i, "<!--", 4)) {
			e = find(s, i, n, "-->");
			i = e ? (size_t)(e - s) + 3 : n;
		} else if (i + 8 < n && !memcmp(s + i, "<![CDATA[", 9)) {
			e = find(s, i, n, "]]>");
			i = e ? (size_t)(e - s) + 3 : n;
		} else if (i + 1 < n && s[i + 1] == '!') {
			e = find(s, i, n, ">");
			i = e ? (size_t)(e - s) + 1 : n;
		} else if (i + 1 < n && s[i + 1] == '/') {
			/* An end tag: closes the current element. */
			e = find(s, i, n, ">");
			if (cur >= 0) {
				d->nodes[cur].close_start = i;
				d->nodes[cur].close_end = e ? (size_t)(e - s) + 1 : n;
				cur = par[cur];
			}
			i = e ? (size_t)(e - s) + 1 : n;
		} else {
			/* A start tag: name, attributes (quotes may hold '>'), > or />. */
			size_t j = i + 1, k, nl;
			char q = 0;
			xnode *x;
			int me;
			while (j < n && !isspace((unsigned char)s[j]) && s[j] != '>' && s[j] != '/')
				j++;
			k = j;
			while (k < n && (q || s[k] != '>')) {
				if (q && s[k] == q)
					q = 0;
				else if (!q && (s[k] == '"' || s[k] == '\''))
					q = s[k];
				k++;
			}
			if (k >= n) {
				d->bad = 1;
				break;
			}
			x = xnew(d);
			if (!x)
				break;
			me = d->count - 1;
			{
				int *np = realloc(par, (size_t)d->cap * sizeof *np);
				if (!np) {
					d->bad = 1;
					break;
				}
				par = np;
			}
			par[me] = cur;
			{
				const char *nm = s + i + 1, *colon;
				nl = j - (i + 1);
				colon = memchr(nm, ':', nl);
				if (colon) {
					nl -= (size_t)(colon + 1 - nm);
					nm = colon + 1;
				}
				if (nl >= sizeof x->name)
					nl = sizeof x->name - 1;
				memcpy(x->name, nm, nl);
				x->name[nl] = 0;
			}
			x->start = i;
			x->open_end = k + 1;
			x->att = j;
			x->att_len = k - j;
			x->empty = s[k - 1] == '/';
			if (x->empty) {
				x->att_len--;
				x->close_start = x->close_end = k + 1;
			} else {
				cur = me;
			}
			i = k + 1;
		}
	}
	/* Link: parent, children in order. */
	if (!d->bad) {
		int m;
		for (m = 0; m < d->count; m++) {
			xnode *x = &d->nodes[m];
			if (par[m] >= 0) {
				xnode *p = &d->nodes[par[m]];
				x->parent = p;
				if (p->last)
					p->last->next = x;
				else
					p->first = x;
				p->last = x;
			}
			if (!x->empty && x->close_start == 0)
				x->close_start = x->close_end = n;      /* unclosed: to the end */
		}
	}
	free(par);
	return d->bad ? -1 : 0;
}

static void xfree(xdoc *d)
{
	free(d->nodes);
	d->nodes = NULL;
}

/* An attribute's raw value by local name (no unescaping needed for the
 * names and keywords this looks at). */
static int xatt(const xdoc *d, const xnode *x, const char *name, char *out, size_t size)
{
	const char *s = d->s + x->att, *end = s + x->att_len;
	size_t nl = strlen(name);
	out[0] = 0;
	while (s < end) {
		const char *k, *ke;
		char q;
		while (s < end && isspace((unsigned char)*s))
			s++;
		k = s;
		while (s < end && *s != '=' && !isspace((unsigned char)*s))
			s++;
		ke = s;
		while (s < end && (isspace((unsigned char)*s) || *s == '='))
			s++;
		if (s >= end || (*s != '"' && *s != '\''))
			return 0;
		q = *s++;
		{
			const char *v = s, *colon = memchr(k, ':', (size_t)(ke - k));
			while (s < end && *s != q)
				s++;
			if (colon && (size_t)(ke - colon - 1) == nl && !memcmp(colon + 1, name, nl) &&
					strncmp(k, "xmlns", 5)) {
				goto match;
			}
			if ((size_t)(ke - k) == nl && !memcmp(k, name, nl)) {
match:
				{
					size_t l = (size_t)(s - v) < size - 1 ? (size_t)(s - v) : size - 1;
					memcpy(out, v, l);
					out[l] = 0;
					return 1;
				}
			}
			s++;
		}
	}
	return 0;
}

static xnode *xchild(xnode *x, const char *name)
{
	for (x = x ? x->first : NULL; x; x = x->next)
		if (!strcmp(x->name, name))
			return x;
	return NULL;
}

static int has_children(const xnode *x)
{
	return x->first != NULL;
}

/* Text content, unescaped (entities, CDATA), into out (UTF-8). */
static void xtext(const xdoc *d, const xnode *x, char *out, size_t size)
{
	size_t i = x->open_end, end = x->empty ? x->open_end : x->close_start, o = 0;
	const char *s = d->s;
	while (i < end && o + 5 < size) {
		if (!memcmp(s + i, "<![CDATA[", 9 < end - i ? 9 : end - i) && end - i >= 9) {
			const char *e = find(s, i + 9, end, "]]>");
			size_t ce = e ? (size_t)(e - s) : end;
			i += 9;
			while (i < ce && o + 1 < size)
				out[o++] = s[i++];
			i = ce + 3;
		} else if (s[i] == '<') {
			const char *e = find(s, i, end, ">");     /* markup inside (rich text): skipped */
			i = e ? (size_t)(e - s) + 1 : end;
		} else if (s[i] == '&') {
			const char *e = memchr(s + i, ';', end - i < 12 ? end - i : 12);
			unsigned c = 0;
			if (e) {
				size_t l = (size_t)(e - (s + i));
				if (l == 4 && !memcmp(s + i, "&amp", 4)) c = '&';
				else if (l == 3 && !memcmp(s + i, "&lt", 3)) c = '<';
				else if (l == 3 && !memcmp(s + i, "&gt", 3)) c = '>';
				else if (l == 5 && !memcmp(s + i, "&quot", 5)) c = '"';
				else if (l == 5 && !memcmp(s + i, "&apos", 5)) c = '\'';
				else if (s[i + 1] == '#')
					c = (unsigned)strtoul(s + i + 2 + (s[i + 2] == 'x'), NULL, s[i + 2] == 'x' ? 16 : 10);
			}
			if (c && c < 0x110000) {
				if (c < 0x80) out[o++] = (char)c;
				else if (c < 0x800) { out[o++] = (char)(0xc0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
				else if (c < 0x10000) { out[o++] = (char)(0xe0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 63));
					out[o++] = (char)(0x80 | (c & 63)); }
				else { out[o++] = (char)(0xf0 | c >> 18); out[o++] = (char)(0x80 | ((c >> 12) & 63));
					out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
				i = (size_t)(e - s) + 1;
			} else {
				out[o++] = s[i++];
			}
		} else {
			out[o++] = s[i++];
		}
	}
	out[o] = 0;
}

/* ---- The XFA packets -------------------------------------------------------- */

typedef struct {
	pdf_obj *xfa;               /* AcroForm/XFA */
	pdf_obj *stream;            /* the stream holding datasets (or the whole XFA) */
	fz_buffer *buf;             /* that stream's bytes */
	size_t ds_from, ds_to;      /* the datasets packet in buf (whole stream if separate) */
	fz_buffer *tmpl;            /* the template packet's bytes, or NULL */
} packets;

static void drop_packets(fz_context *ctx, packets *p)
{
	fz_drop_buffer(ctx, p->buf);
	fz_drop_buffer(ctx, p->tmpl);
	memset(p, 0, sizeof *p);
}

/* The bytes of a packet in a single-stream XFA: from <...name to its end. */
static int packet_range(const unsigned char *s, size_t n, const char *name, size_t *from, size_t *to)
{
	size_t i, nl = strlen(name);
	for (i = 0; i + nl + 2 < n; i++) {
		size_t j;
		if (s[i] != '<' || s[i + 1] == '/')
			continue;
		j = i + 1;
		while (j < n && s[j] != ':' && s[j] != '>' && !isspace(s[j]) && s[j] != '/')
			j++;
		if (j < n && s[j] == ':')
			j++;
		else
			j = i + 1;
		if (j + nl <= n && !memcmp(s + j, name, nl) && (isspace(s[j + nl]) || s[j + nl] == '>' || s[j + nl] == '/')) {
			/* Its end: the matching close tag (packets don't nest themselves). */
			size_t k;
			*from = i;
			for (k = j + nl; k + nl + 2 < n; k++)
				if (s[k] == '<' && s[k + 1] == '/') {
					size_t m = k + 2;
					while (m < n && s[m] != ':' && s[m] != '>')
						m++;
					m = s[m] == ':' ? m + 1 : k + 2;
					if (m + nl <= n && !memcmp(s + m, name, nl) && s[m + nl] == '>') {
						*to = m + nl + 1;
						return 0;
					}
				}
			/* <xfa:datasets .../> */
			*to = n;
			return 0;
		}
	}
	return -1;
}

static int load_packets(fz_context *ctx, pdf_document *pdf, packets *p)
{
	memset(p, 0, sizeof *p);
	p->xfa = pdf_dict_getp(ctx, pdf_trailer(ctx, pdf), "Root/AcroForm/XFA");
	if (pdf_is_array(ctx, p->xfa)) {
		int i, n = pdf_array_len(ctx, p->xfa);
		for (i = 0; i + 1 < n; i += 2) {
			const char *name = pdf_to_text_string(ctx, pdf_array_get(ctx, p->xfa, i));
			pdf_obj *st = pdf_array_get(ctx, p->xfa, i + 1);
			if (!pdf_is_stream(ctx, st))
				continue;
			if (!strcmp(name, "datasets") && !p->buf) {
				p->stream = st;
				p->buf = pdf_load_stream(ctx, st);
				p->ds_from = 0;
				p->ds_to = p->buf->len;
			} else if (!strcmp(name, "template") && !p->tmpl) {
				p->tmpl = pdf_load_stream(ctx, st);
			}
		}
		return p->buf || p->tmpl ? 0 : -1;
	}
	if (pdf_is_stream(ctx, p->xfa)) {
		size_t a, b;
		p->stream = p->xfa;
		p->buf = pdf_load_stream(ctx, p->xfa);
		if (packet_range(p->buf->data, p->buf->len, "template", &a, &b) == 0)
			p->tmpl = fz_new_buffer_from_copied_data(ctx, p->buf->data + a, b - a);
		if (packet_range(p->buf->data, p->buf->len, "datasets", &p->ds_from, &p->ds_to)) {
			p->ds_from = p->ds_to = 0;
			p->stream = NULL;           /* nowhere to keep values */
		}
		return 0;
	}
	return -1;
}

int folio_xfa_kind(folio_doc *doc)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	int kind = FOLIO_XFA_NONE;
	if (!pdf)
		return FOLIO_XFA_NONE;
	fz_try(ctx) {
		pdf_obj *acro = pdf_dict_getp(ctx, pdf_trailer(ctx, pdf), "Root/AcroForm");
		pdf_obj *xfa = pdf_dict_get(ctx, acro, PDF_NAME(XFA));
		if (pdf_is_array(ctx, xfa) || pdf_is_stream(ctx, xfa)) {
			pdf_obj *fields = pdf_dict_get(ctx, acro, PDF_NAME(Fields));
			int needs = pdf_to_bool(ctx, pdf_dict_getp(ctx, pdf_trailer(ctx, pdf), "Root/NeedsRendering"));
			kind = needs || pdf_array_len(ctx, fields) == 0 ? FOLIO_XFA_DYNAMIC : FOLIO_XFA_HYBRID;
		}
	}
	fz_catch(ctx)
		kind = FOLIO_XFA_NONE;
	return kind;
}

/* ---- The template: each field's caption, kind and choices, by path -------- */

typedef struct {
	char path[256];             /* data path without indices: form1.Page1.Name */
	char name[64];
	int global;
	folio_xfa_field f;          /* label, type, read_only, options; value = default */
} tfield;

typedef struct {
	tfield *v;
	int n, cap;
} tlist;

static void caption_of(const xdoc *d, xnode *field, char *out, size_t size)
{
	xnode *c = xchild(field, "caption"), *v = c ? xchild(c, "value") : NULL;
	out[0] = 0;
	if (v && v->first)
		xtext(d, v->first, out, size);
	/* Collapse white space; a long caption is cut. */
	{
		char *r = out, *w = out;
		int sp = 1;
		for (; *r; r++) {
			if (isspace((unsigned char)*r)) {
				if (!sp)
					*w++ = ' ';
				sp = 1;
			} else {
				*w++ = *r;
				sp = 0;
			}
		}
		while (w > out && w[-1] == ' ')
			w--;
		*w = 0;
	}
}

/* <items> lists: the first is what's shown; one with save="1" what's kept. */
static void items_of(const xdoc *d, xnode *field, folio_xfa_field *f)
{
	xnode *it, *shown = NULL, *saved = NULL, *x;
	char a[8];
	int i;
	for (it = field->first; it; it = it->next)
		if (!strcmp(it->name, "items")) {
			if (xatt(d, it, "save", a, sizeof a) && a[0] == '1')
				saved = it;
			else if (!shown)
				shown = it;
			else if (!saved)
				saved = it;
		}
	if (!shown)
		shown = saved;
	if (!saved)
		saved = shown;
	f->nopts = 0;
	for (x = shown ? shown->first : NULL, i = 0; x && i < FOLIO_XFA_OPTS; x = x->next, i++)
		xtext(d, x, f->opt[i], sizeof f->opt[i]);
	f->nopts = i;
	for (x = saved ? saved->first : NULL, i = 0; x && i < f->nopts; x = x->next, i++)
		xtext(d, x, f->save[i], sizeof f->save[i]);
	for (; i < f->nopts; i++)
		memcpy(f->save[i], f->opt[i], sizeof f->save[i]);
}

static void add_tfield(tlist *t, const tfield *tf)
{
	if (t->n == t->cap) {
		int cap = t->cap ? t->cap * 2 : 32;
		tfield *v = realloc(t->v, (size_t)cap * sizeof *v);
		if (!v)
			return;
		t->v = v;
		t->cap = cap;
	}
	t->v[t->n++] = *tf;
}

static void join(char *out, size_t size, const char *scope, const char *name)
{
	size_t a = strlen(scope), b = strlen(name);
	if (a + b + 2 > size) {
		out[0] = 0;             /* too deep to keep: never matches */
		return;
	}
	if (a) {
		memmove(out, scope, a);
		out[a++] = '.';
	}
	memmove(out + a, name, b + 1);
}

/* A dataRef's path, relative to scope: "$.a.b" (scope), "$record.a.b" or
 * "$data.root.a" (the data root), "a.b" (scope). Indices ([*], [0]) go. */
static void ref_path(const char *ref, const char *scope, const char *root, char *out, size_t size)
{
	char tmp[256], *w = tmp;
	const char *s = ref, *base = scope;
	if (!strncmp(s, "$record", 7)) {
		s += 7;
		base = root;
	} else if (!strncmp(s, "$data.", 6)) {
		s += 6;
		base = "";
	} else if (!strncmp(s, "$.", 2) || !strcmp(s, "$")) {
		s += 1;
	}
	if (*s == '.')
		s++;
	for (; *s && w < tmp + sizeof tmp - 1; s++) {
		if (*s == '[') {
			while (*s && *s != ']')
				s++;
			if (!*s)
				break;
			continue;
		}
		*w++ = *s;
	}
	*w = 0;
	join(out, size, base, tmp);
}

static void walk_template(const xdoc *d, xnode *x, const char *scope, const char *root, int ro, tlist *t)
{
	for (; x; x = x->next) {
		char name[64], access[24], path[256], bind_ref[200];
		xnode *bind = xchild(x, "bind");
		int bro = ro, global = 0, none = 0;
		xatt(d, x, "name", name, sizeof name);
		if (xatt(d, x, "access", access, sizeof access) && strcmp(access, "open"))
			bro = 1;
		bind_ref[0] = 0;
		if (bind) {
			char m[16];
			xatt(d, bind, "match", m, sizeof m);
			if (!strcmp(m, "none"))
				none = 1;
			else if (!strcmp(m, "global"))
				global = 1;
			else if (!strcmp(m, "dataRef"))
				xatt(d, bind, "ref", bind_ref, sizeof bind_ref);
		}
		if (!strcmp(x->name, "subform") || !strcmp(x->name, "subformSet") || !strcmp(x->name, "area")) {
			const char *inner = scope;
			if (bind_ref[0]) {
				ref_path(bind_ref, scope, root, path, sizeof path);
				inner = path;
			} else if (name[0] && !none && strcmp(x->name, "area")) {
				join(path, sizeof path, scope, name);
				inner = path;
			}
			walk_template(d, x->first, inner, root[0] ? root : inner, bro, t);
		} else if ((!strcmp(x->name, "field") || !strcmp(x->name, "exclGroup")) && !none && name[0]) {
			tfield tf;
			xnode *ui = xchild(x, "ui"), *u = ui ? ui->first : NULL, *v;
			memset(&tf, 0, sizeof tf);
			snprintf(tf.name, sizeof tf.name, "%s", name);
			tf.global = global;
			if (bind_ref[0])
				ref_path(bind_ref, scope, root, tf.path, sizeof tf.path);
			else
				join(tf.path, sizeof tf.path, scope, name);
			caption_of(d, x, tf.f.label, sizeof tf.f.label);
			if (!tf.f.label[0]) {
				/* No caption: its tool tip, or its name. */
				xnode *as = xchild(x, "assist"), *tt = as ? xchild(as, "toolTip") : NULL;
				if (tt)
					xtext(d, tt, tf.f.label, sizeof tf.f.label);
				if (!tf.f.label[0])
					snprintf(tf.f.label, sizeof tf.f.label, "%s", name);
			}
			tf.f.read_only = bro;
			tf.f.chosen = -1;
			if (!strcmp(x->name, "exclGroup")) {
				/* Radio buttons: each field's caption, and its on value. */
				xnode *c;
				int i = 0;
				tf.f.type = FOLIO_XFA_CHOICE;
				for (c = x->first; c && i < FOLIO_XFA_OPTS; c = c->next) {
					folio_xfa_field one;
					if (strcmp(c->name, "field"))
						continue;
					memset(&one, 0, sizeof one);
					items_of(d, c, &one);
					caption_of(d, c, tf.f.opt[i], sizeof tf.f.opt[i]);
					snprintf(tf.f.save[i], sizeof tf.f.save[i], "%s", one.nopts ? one.save[0] : "");
					if (!tf.f.opt[i][0])
						memcpy(tf.f.opt[i], tf.f.save[i], sizeof tf.f.opt[i]);
					i++;
				}
				tf.f.nopts = i;
			} else {
				const char *k = u ? u->name : "textEdit";
				char ml[8];
				if (!strcmp(k, "button") || !strcmp(k, "signature") || !strcmp(k, "imageEdit") ||
						!strcmp(k, "barcode"))
					continue;       /* nothing to type */
				tf.f.type = !strcmp(k, "numericEdit") ? FOLIO_XFA_NUMBER :
					!strcmp(k, "dateTimeEdit") ? FOLIO_XFA_DATE :
					!strcmp(k, "checkButton") ? FOLIO_XFA_CHECK :
					!strcmp(k, "choiceList") ? FOLIO_XFA_CHOICE :
					!strcmp(k, "passwordEdit") ? FOLIO_XFA_PASSWORD :
					u && xatt(d, u, "multiLine", ml, sizeof ml) && ml[0] == '1' ? FOLIO_XFA_MULTILINE :
					FOLIO_XFA_TEXT;
				items_of(d, x, &tf.f);
				if (tf.f.type == FOLIO_XFA_CHECK && tf.f.nopts == 0) {
					tf.f.nopts = 2;
					strcpy(tf.f.opt[0], "1"); strcpy(tf.f.save[0], "1");
					strcpy(tf.f.opt[1], "0"); strcpy(tf.f.save[1], "0");
				}
			}
			/* The template's default value. */
			v = xchild(x, "value");
			if (v && v->first)
				xtext(d, v->first, tf.f.value, sizeof tf.f.value);
			add_tfield(t, &tf);
		}
	}
}

/* Path compare ignoring [n] indices in a. */
static int same_path(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a == '[') {
			while (*a && *a != ']')
				a++;
			if (*a)
				a++;
			continue;
		}
		if (*a != *b)
			return 0;
		a++;
		b++;
	}
	while (*a == '[') {
		while (*a && *a != ']')
			a++;
		if (*a)
			a++;
	}
	return !*a && !*b;
}

static const char *leaf_name(const char *path)
{
	const char *dot = strrchr(path, '.');
	return dot ? dot + 1 : path;
}

static tfield *match_template(tlist *t, const char *path)
{
	int i;
	char leaf[128], *b;
	for (i = 0; i < t->n; i++)
		if (same_path(path, t->v[i].path))
			return &t->v[i];
	snprintf(leaf, sizeof leaf, "%s", leaf_name(path));
	if ((b = strchr(leaf, '[')))
		*b = 0;
	for (i = 0; i < t->n; i++)
		if (t->v[i].global && !strcmp(t->v[i].name, leaf))
			return &t->v[i];
	return NULL;
}

/* ---- The data ----------------------------------------------------------------- */

static xnode *data_root(xdoc *d)
{
	int i;
	for (i = 0; i < d->count; i++)
		if (!strcmp(d->nodes[i].name, "data") && d->nodes[i].parent &&
				!strcmp(d->nodes[i].parent->name, "datasets"))
			return &d->nodes[i];
	return NULL;
}

/* The element's path below xfa:data, with [n] for the second and later
 * of the same name. */
static __attribute__((noinline)) void data_path(xnode *x, xnode *root, char *out, size_t size)
{
	char tmp[256];
	out[0] = 0;
	for (; x && x != root; x = x->parent) {
		int k = 0;
		xnode *s;
		for (s = x->parent ? x->parent->first : NULL; s && s != x; s = s->next)
			if (!strcmp(s->name, x->name))
				k++;
		if (k)
			snprintf(tmp, sizeof tmp, "%s[%d]%s%s", x->name, k, out[0] ? "." : "", out);
		else
			snprintf(tmp, sizeof tmp, "%s%s%s", x->name, out[0] ? "." : "", out);
		snprintf(out, size, "%s", tmp);
	}
}

/* An element by path (form1.Item[2].Price), or NULL; *deepest is the last
 * one found on the way, and *used how much of the path it covers. */
static __attribute__((noinline)) xnode *data_find(xnode *root, const char *path, xnode **deepest, size_t *used)
{
	xnode *x = root;
	const char *p = path;
	*deepest = root;
	*used = 0;
	while (*p) {
		char seg[64];
		int idx = 0, k = 0;
		size_t l = strcspn(p, ".[");
		xnode *c;
		if (l >= sizeof seg)
			return NULL;
		memcpy(seg, p, l);
		seg[l] = 0;
		p += l;
		if (*p == '[') {
			const char *q = strchr(p, ']');
			idx = atoi(p + 1);
			p = q ? q + 1 : p + strlen(p);
		}
		for (c = x->first; c; c = c->next)
			if (!strcmp(c->name, seg) && k++ == idx)
				break;
		if (!c)
			return NULL;
		x = c;
		if (*p == '.')
			p++;
		*deepest = x;
		*used = (size_t)(p - path);
	}
	return x;
}

static void copy_tf(folio_xfa_field *out, const tfield *tf, const char *path, const char *value)
{
	char p[sizeof out->path];
	snprintf(p, sizeof p, "%s", path);
	*out = tf->f;
	memcpy(out->path, p, sizeof p);
	if (value)
		snprintf(out->value, sizeof out->value, "%s", value);
}

static void choose(folio_xfa_field *f)
{
	int i;
	f->chosen = -1;
	for (i = 0; i < f->nopts; i++)
		if (!strcmp(f->save[i], f->value)) {
			f->chosen = i;
			break;
		}
}

typedef struct {
	packets p;
	xdoc data, tmpl;
	tlist t;
} xfa_state;

static void drop_state(fz_context *ctx, xfa_state *st)
{
	drop_packets(ctx, &st->p);
	xfree(&st->data);
	xfree(&st->tmpl);
	free(st->t.v);
	memset(st, 0, sizeof *st);
}

static int load_state(fz_context *ctx, pdf_document *pdf, xfa_state *st)
{
	memset(st, 0, sizeof *st);
	if (load_packets(ctx, pdf, &st->p))
		return -1;
	if (st->p.buf && st->p.ds_to > st->p.ds_from)
		xparse(&st->data, (const char *)st->p.buf->data + st->p.ds_from, st->p.ds_to - st->p.ds_from);
	if (st->p.tmpl && xparse(&st->tmpl, (const char *)st->p.tmpl->data, st->p.tmpl->len) == 0) {
		int i;
		for (i = 0; i < st->tmpl.count; i++)
			if (!strcmp(st->tmpl.nodes[i].name, "template")) {
				walk_template(&st->tmpl, st->tmpl.nodes[i].first, "", "", 0, &st->t);
				break;
			}
	}
	return 0;
}

int folio_xfa_fields(folio_doc *doc, folio_xfa_field *out, int max)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	xfa_state st;
	int n = 0;
	if (!pdf || folio_xfa_kind(doc) == FOLIO_XFA_NONE)
		return 0;
	memset(&st, 0, sizeof st);
	fz_try(ctx) {
		xnode *root;
		unsigned char *used_t;
		int i;
		if (load_state(ctx, pdf, &st))
			fz_throw(ctx, FZ_ERROR_FORMAT, "the form's XFA can't be read");
		root = st.data.count ? data_root(&st.data) : NULL;
		used_t = calloc((size_t)st.t.n + 1, 1);
		/* Values in the data, in order, with what the template says. */
		for (i = 0; root && i < st.data.count; i++) {
			xnode *x = &st.data.nodes[i];
			char path[256], val[512], dn[24];
			tfield *tf;
			xnode *a;
			int inside = 0;
			for (a = x->parent; a; a = a->parent)
				if (a == root)
					inside = 1;
			if (!inside || has_children(x))
				continue;
			if (xatt(&st.data, x, "dataNode", dn, sizeof dn) && !strcmp(dn, "dataGroup"))
				continue;
			data_path(x, root, path, sizeof path);
			tf = match_template(&st.t, path);
			if (!tf && st.t.n)
				continue;       /* data the form doesn't show */
			xtext(&st.data, x, val, sizeof val);
			if (n < max) {
				if (tf)
					copy_tf(&out[n], tf, path, val);
				else {
					memset(&out[n], 0, sizeof out[n]);
					snprintf(out[n].path, sizeof out[n].path, "%s", path);
					snprintf(out[n].label, sizeof out[n].label, "%.150s", leaf_name(path));
					snprintf(out[n].value, sizeof out[n].value, "%s", val);
					out[n].type = FOLIO_XFA_TEXT;
				}
				choose(&out[n]);
			}
			if (tf && used_t)
				used_t[tf - st.t.v] = 1;
			n++;
		}
		/* Fields with no data yet (given their default). */
		for (i = 0; i < st.t.n; i++) {
			if (used_t && used_t[i])
				continue;
			if (n < max) {
				copy_tf(&out[n], &st.t.v[i], st.t.v[i].path, NULL);
				choose(&out[n]);
			}
			n++;
		}
		free(used_t);
	}
	fz_always(ctx)
		drop_state(ctx, &st);
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return n;
}

/* ---- Writing a value ------------------------------------------------------------- */

static void put_escaped(fz_context *ctx, fz_buffer *b, const char *v)
{
	for (; *v; v++)
		switch (*v) {
		case '&': fz_append_string(ctx, b, "&amp;"); break;
		case '<': fz_append_string(ctx, b, "&lt;"); break;
		case '>': fz_append_string(ctx, b, "&gt;"); break;
		case '\r': break;
		default: fz_append_byte(ctx, b, *v);
		}
}

/* Elements to make: the rest of the path (names only), the innermost holding
 * the value. */
static void put_chain(fz_context *ctx, fz_buffer *b, const char *rest, const char *value)
{
	char seg[64];
	size_t l = strcspn(rest, ".[");
	const char *after = rest + l;
	if (l >= sizeof seg)
		l = sizeof seg - 1;
	memcpy(seg, rest, l);
	seg[l] = 0;
	if (*after == '[') {
		after = strchr(after, ']');
		after = after ? after + 1 : rest + strlen(rest);
	}
	if (*after == '.')
		after++;
	fz_append_printf(ctx, b, "<%s>", seg);
	if (*after)
		put_chain(ctx, b, after, value);
	else
		put_escaped(ctx, b, value);
	fz_append_printf(ctx, b, "</%s>", seg);
}

/* The prefix ("xfa:") an element was written with, for its end tag. */
static void raw_name(const xdoc *d, const xnode *x, char *out, size_t size)
{
	size_t l = x->att - x->start - 1;
	if (l >= size)
		l = size - 1;
	memcpy(out, d->s + x->start + 1, l);
	out[l] = 0;
}

/* The datasets packet with path set to value (NULL if it can't be). */
static fz_buffer *edited(fz_context *ctx, xfa_state *st, const char *path, const char *value)
{
	xdoc *d = &st->data;
	xnode *root = d->count ? data_root(d) : NULL, *x, *deep;
	size_t used, cut_from, cut_to;
	fz_buffer *mid, *out = NULL;
	if (!root)
		fz_throw(ctx, FZ_ERROR_FORMAT, "the form has nowhere to keep its values");
	x = data_find(root, path, &deep, &used);
	mid = fz_new_buffer(ctx, 256);
	fz_var(out);
	fz_try(ctx) {
		if (x && has_children(x))
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "that's a group, not a field");
		if (x && !x->empty) {
			cut_from = x->open_end;
			cut_to = x->close_start;
			put_escaped(ctx, mid, value);
		} else if (x) {
			/* <Name/>: <Name>value</Name>. */
			char nm[80];
			raw_name(d, x, nm, sizeof nm);
			cut_from = x->start;
			cut_to = x->close_end;
			fz_append_data(ctx, mid, d->s + x->start, x->att + x->att_len - x->start);
			fz_append_byte(ctx, mid, '>');
			put_escaped(ctx, mid, value);
			fz_append_printf(ctx, mid, "</%s>", nm);
		} else {
			/* Missing: made inside the deepest group there is. */
			const char *rest = path + used;
			if (deep->empty) {
				char nm[80];
				raw_name(d, deep, nm, sizeof nm);
				cut_from = deep->start;
				cut_to = deep->close_end;
				fz_append_data(ctx, mid, d->s + deep->start, deep->att + deep->att_len - deep->start);
				fz_append_byte(ctx, mid, '>');
				put_chain(ctx, mid, rest, value);
				fz_append_printf(ctx, mid, "</%s>", nm);
			} else {
				cut_from = cut_to = deep->close_start;
				put_chain(ctx, mid, rest, value);
			}
		}
		out = fz_new_buffer(ctx, d->n + mid->len + 16);
		fz_append_data(ctx, out, d->s, cut_from);
		fz_append_buffer(ctx, out, mid);
		fz_append_data(ctx, out, d->s + cut_to, d->n - cut_to);
	}
	fz_always(ctx)
		fz_drop_buffer(ctx, mid);
	fz_catch(ctx)
		fz_rethrow(ctx);
	return out;
}

/* AcroForm field full names to XFA paths: form1[0].#subform[0].Name[0]
 * -> form1.Name (indices above 0 kept). */
static void acro_to_path(const char *acro, char *out, size_t size)
{
	size_t o = 0;
	const char *s = acro;
	while (*s && o + 1 < size) {
		size_t l = strcspn(s, ".");
		if (*s == '#') {
			s += l;
		} else {
			const char *br = memchr(s, '[', l);
			size_t nl = br ? (size_t)(br - s) : l;
			if (o && o + 1 < size)
				out[o++] = '.';
			if (nl > size - 1 - o)
				nl = size - 1 - o;
			memcpy(out + o, s, nl);
			o += nl;
			if (br && atoi(br + 1) > 0)
				o += (size_t)snprintf(out + o, size - o, "[%d]", atoi(br + 1));
			s += l;
		}
		if (*s == '.')
			s++;
	}
	out[o < size ? o : size - 1] = 0;
}

/* Writes the edited datasets (inside an operation). */
static void store(fz_context *ctx, pdf_document *pdf, xfa_state *st, fz_buffer *ds)
{
	if (st->p.ds_from == 0 && st->p.ds_to == st->p.buf->len) {
		pdf_update_stream(ctx, pdf, st->p.stream, ds, 0);
	} else {
		fz_buffer *all = fz_new_buffer(ctx, st->p.buf->len + ds->len);
		fz_try(ctx) {
			fz_append_data(ctx, all, st->p.buf->data, st->p.ds_from);
			fz_append_buffer(ctx, all, ds);
			fz_append_data(ctx, all, st->p.buf->data + st->p.ds_to, st->p.buf->len - st->p.ds_to);
			pdf_update_stream(ctx, pdf, st->p.stream, all, 0);
		}
		fz_always(ctx)
			fz_drop_buffer(ctx, all);
		fz_catch(ctx)
			fz_rethrow(ctx);
	}
	pdf_dict_del(ctx, st->p.stream, PDF_NAME(Filter));
	pdf_dict_del(ctx, st->p.stream, PDF_NAME(DecodeParms));
	pdf_invalidate_xfa(ctx, pdf);
}

/* The ordinary field matching path, on a hybrid form. */
static pdf_obj *acro_field(fz_context *ctx, pdf_obj *fields, const char *path)
{
	int i, n = pdf_array_len(ctx, fields);
	for (i = 0; i < n; i++) {
		pdf_obj *f = pdf_array_get(ctx, fields, i), *kids = pdf_dict_get(ctx, f, PDF_NAME(Kids)), *hit;
		char *name = pdf_load_field_name(ctx, f), p[256];
		int same;
		acro_to_path(name, p, sizeof p);
		fz_free(ctx, name);
		same = !strcmp(p, path) || same_path(path, p);
		if (same && pdf_dict_get(ctx, f, PDF_NAME(FT)))
			return f;
		if (kids && (hit = acro_field(ctx, kids, path)) != NULL)
			return hit;
	}
	return NULL;
}

static void set_acro(folio_doc *doc, pdf_document *pdf, const char *path, const tfield *tf, const char *v)
{
	fz_context *ctx = doc->ctx;
	pdf_obj *f = acro_field(ctx, pdf_dict_getp(ctx, pdf_trailer(ctx, pdf), "Root/AcroForm/Fields"), path);
	if (!f)
		return;
	{
		char *name = pdf_load_field_name(ctx, f);
		fz_try(ctx)
			folio_allow(doc, FOLIO_CHANGE_FORM, name);
		fz_always(ctx)
			fz_free(ctx, name);
		fz_catch(ctx)
			fz_rethrow(ctx);
	}
	fz_var(v);
	if (tf && tf->f.type == FOLIO_XFA_CHECK && tf->f.nopts > 1 && !strcmp(v, tf->f.save[1]))
		v = "Off";
	doc->xfa_syncing = 1;
	fz_try(ctx)
		pdf_set_field_value(ctx, pdf, f, v, 1);
	fz_always(ctx)
		doc->xfa_syncing = 0;
	fz_catch(ctx)
		fz_rethrow(ctx);
}

int folio_xfa_set(folio_doc *doc, const char *path, const char *value)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	xfa_state st;
	fz_buffer *ds = NULL;
	int op = 0;
	if (!pdf || folio_xfa_kind(doc) == FOLIO_XFA_NONE) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "this isn't an XFA form");
		return -1;
	}
	memset(&st, 0, sizeof st);
	fz_var(ds);
	fz_var(op);
	folio_begin(doc);
	fz_try(ctx) {
		tfield *tf;
		if (load_state(ctx, pdf, &st) || !st.p.stream)
			fz_throw(ctx, FZ_ERROR_FORMAT, "the form has nowhere to keep its values");
		tf = match_template(&st.t, path);
		if (tf && tf->f.read_only)
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "the field can't be changed");
		ds = edited(ctx, &st, path, value ? value : "");
		folio_allow(doc, FOLIO_CHANGE_FORM, NULL);
		pdf_begin_operation(ctx, pdf, "Fill in");
		op = 1;
		store(ctx, pdf, &st, ds);
		/* The matching ordinary field, on a static form. */
		if (folio_xfa_kind(doc) == FOLIO_XFA_HYBRID)
			set_acro(doc, pdf, path, tf, value ? value : "");
		pdf_end_operation(ctx, pdf);
		op = 0;
	}
	fz_always(ctx) {
		fz_drop_buffer(ctx, ds);
		drop_state(ctx, &st);
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

/* From form.c, inside its operation: an ordinary field (full name) now has
 * value; the datasets follow, on an XFA form. Errors are ignored (the
 * field itself has changed). */
void folio_xfa_sync(folio_doc *doc, const char *acro_name, const char *value)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	xfa_state st;
	fz_buffer *ds = NULL;
	char path[256];
	if (!pdf || doc->xfa_syncing || folio_xfa_kind(doc) == FOLIO_XFA_NONE)
		return;
	memset(&st, 0, sizeof st);
	fz_var(ds);
	acro_to_path(acro_name, path, sizeof path);
	fz_try(ctx) {
		tfield *tf;
		const char *v = value ? value : "";
		if (load_state(ctx, pdf, &st) || !st.p.stream)
			break;
		tf = match_template(&st.t, path);
		if (st.t.n && !tf)
			break;
		if (tf && tf->f.type == FOLIO_XFA_CHECK && tf->f.nopts > 1 && !strcmp(v, "Off"))
			v = tf->f.save[1];
		ds = edited(ctx, &st, path, v);
		store(ctx, pdf, &st, ds);
	}
	fz_always(ctx) {
		fz_drop_buffer(ctx, ds);
		drop_state(ctx, &st);
	}
	fz_catch(ctx)
		fz_report_error(ctx);
}
