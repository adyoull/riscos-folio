/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Signatures on the desktop (core/sign.c checks them):
 *  - the roots Folio trusts: Adobe's (built in), CaCertificates' bundle if
 *    it's installed (the one PackMan uses: <CaCertificates$Dir>.ca-
 *    certificates/crt), and any certificates in Choices:Folio.Trusted (a PEM
 *    file); read by their RISC OS names, once, when first needed;
 *  - what a signature says, in words (Document info, and a click on it);
 *  - a warning on opening a PDF whose signature has been broken;
 *  - signing (test64): the Sign box takes a .p12 / .pfx certificate dragged
 *    into it, its password, and a reason and place; it signs an empty
 *    signature field that was clicked, or (from the menu) a new one at the
 *    foot of the page; then the Save box opens, as the signature is made as
 *    the PDF is saved. The password is never kept. */
#include "app.h"
#include "worker.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

static int roots_loaded;

/* A file's bytes by its RISC OS name (UnixLib names with <...$Dir> don't
 * open here: test48's fonts). */
static unsigned char *read_native(const char *ro, int *len)
{
	int type = 0, n = 0;
	unsigned char *b;
	if (_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUT(4), 17, ro, &type, &n) || type != 1 || n <= 0)
		return NULL;
	b = malloc((size_t)n);
	if (b && _swix(OS_File, _INR(0, 3), 16, ro, b, 0)) {
		free(b);
		b = NULL;
	}
	*len = n;
	return b;
}

void signui_roots(void)
{
	static const char *const files[] = {
		"<CaCertificates$Dir>.ca-certificates/crt",
		"Choices:Folio.Trusted",
	};
	int i;
	if (roots_loaded)
		return;
	roots_loaded = 1;
	for (i = 0; i < (int)(sizeof files / sizeof files[0]); i++) {
		int len = 0, n;
		unsigned char *b = read_native(files[i], &len);
		if (!b)
			continue;
		n = folio_set_trust_roots(app.fc, b, (size_t)len);
		free(b);
		app_log("signatures: %s: %d certificate%s trusted", files[i], n, n == 1 ? "" : "s");
	}
	app_log("signatures: %d trusted roots in all", folio_trust_root_count());
}

/* What a signature says, in a sentence or three (UTF-8). */
void signui_describe(const folio_signature *s, char *out, size_t size)
{
	char when[64], who[300];
	const char *verdict, *cert, *after = "";
	if (!s->is_signed) {
		snprintf(out, size, "'%s' is a place for a signature; it hasn't been signed.", s->field);
		return;
	}
	when[0] = 0;
	if (s->date[0])
		folio_pdf_date(s->date, when, sizeof when);
	if (s->organisation[0])
		snprintf(who, sizeof who, "%s (%s)", s->signer[0] ? s->signer : "someone", s->organisation);
	else
		snprintf(who, sizeof who, "%s", s->signer[0] ? s->signer : "someone unnamed");
	verdict = s->digest == FOLIO_SIG_VALID ? "The signature is good" :
		s->digest == FOLIO_SIG_CHANGED ? "The signature is BROKEN: the document has been changed since it was signed" :
		"Folio can't check the signature";
	cert = s->digest != FOLIO_SIG_VALID ? "" :
		s->cert == FOLIO_CERT_TRUSTED ? ", and the signer's certificate comes from a body Folio trusts" :
		s->cert == FOLIO_CERT_SELF_SIGNED ? ", but the signer vouches for themselves: nobody Folio trusts says who they are" :
		s->cert == FOLIO_CERT_UNTRUSTED ? ", but the signer's certificate doesn't come from a body Folio trusts, so who signed it can't be confirmed" :
		s->cert == FOLIO_CERT_NONE ? ", but there's no certificate to say who signed it" :
		", but there's something wrong with the signer's certificate";
	if (s->digest == FOLIO_SIG_VALID && s->changed_after)
		after = " The document has had changes saved since it was signed (forms filled in or notes added, perhaps); what was signed is as it was.";
	snprintf(out, size, "Signed by %s%s%s. %s%s.%s%s%s%s%s%s%s", who, when[0] ? " on " : "", when,
		verdict, cert, after,
		s->expired && s->digest == FOLIO_SIG_VALID ? " The certificate is out of date now." : "",
		s->digest == FOLIO_SIG_UNREADABLE && s->why[0] ? " (" : "",
		s->digest == FOLIO_SIG_UNREADABLE ? s->why : "",
		s->digest == FOLIO_SIG_UNREADABLE && s->why[0] ? ")" : "",
		s->reason[0] ? " Reason: " : "", s->reason);
}

/* A short form for Document info's rows. */
void signui_brief(const folio_signature *s, char *out, size_t size)
{
	char when[64];
	when[0] = 0;
	if (!s->is_signed) {
		snprintf(out, size, "%s: not signed", s->field);
		return;
	}
	if (s->date[0])
		folio_pdf_date(s->date, when, sizeof when);
	snprintf(out, size, "%s%s%s: %s%s", s->signer[0] ? s->signer : "(no name)", when[0] ? ", " : "", when,
		s->digest == FOLIO_SIG_VALID ? (s->cert == FOLIO_CERT_TRUSTED ? "good, trusted" :
			s->cert == FOLIO_CERT_SELF_SIGNED ? "good, self-signed" : "good, signer not confirmed") :
		s->digest == FOLIO_SIG_CHANGED ? "BROKEN (changed since)" : "can't be checked",
		s->digest == FOLIO_SIG_VALID && s->changed_after ? "; changed since" : "");
}

/* A signature field clicked: say what it is. */
void signui_click(docwin *w, int page, const char *field)
{
	folio_signature s[16];
	char text[700], *d;
	int n, i;
	signui_roots();
	_swix(Hourglass_On, 0);
	core_now();
	n = folio_signatures(docwin_doc(w), s, 16);
	core_unlock();
	_swix(Hourglass_Off, 0);
	for (i = 0; i < n; i++)
		if (s[i].page == page && !strcmp(s[i].field, field))
			break;
	if (i >= n) {
		tbx_report("Folio couldn't find that signature.");
		return;
	}
	if (!s[i].is_signed) {
		signbox_open(w, page, field);
		return;
	}
	signui_describe(&s[i], text, sizeof text);
	app_log("signature '%s': digest %d cert %d changed %d (%s)", field, s[i].digest, s[i].cert,
		s[i].changed_after, s[i].why);
	d = app_desktop_text(text);
	ask_show("Signature", d ? d : text, "OK", NULL, NULL, NULL);
	free(d);
}

/* On opening: a broken signature is worth saying at once. */
void signui_on_open(docwin *w)
{
	folio_signature s[16];
	char text[800], line[300], *d;
	int n, i, broken = 0;
	core_now();
	n = folio_has_signatures(docwin_doc(w));
	core_unlock();
	if (n <= 0)
		return;
	signui_roots();
	core_now();
	n = folio_signatures(docwin_doc(w), s, 16);
	core_unlock();
	snprintf(text, sizeof text, "This PDF's signature has been broken: it has been changed since it was signed.");
	for (i = 0; i < n; i++) {
		app_log("signature '%s': signed %d digest %d cert %d changed %d", s[i].field, s[i].is_signed,
			s[i].digest, s[i].cert, s[i].changed_after);
		if (s[i].is_signed && s[i].digest == FOLIO_SIG_CHANGED) {
			broken++;
			if (broken == 1 && s[i].signer[0]) {
				snprintf(line, sizeof line, " (signed by %s)", s[i].signer);
				strncat(text, line, sizeof text - strlen(text) - 1);
			}
		}
	}
	if (!broken)
		return;
	d = app_desktop_text(text);
	ask_show("Signature broken", d ? d : text, "OK", NULL, NULL, NULL);
	free(d);
}

/* ---- The Sign box ---------------------------------------------------------- */

typedef struct {
	int flags, type;
	int x0, y0, x1, y1;
	ComponentId id;
	const char *help;
	int max_help;
} gadget_hdr;

typedef struct { gadget_hdr h; const char *label; } label_gadget;
typedef struct {
	gadget_hdr h;
	char *text;
	int max_text;
	const char *allowable;
	int max_allowable;
	ComponentId before, after;
} writable_gadget;
typedef struct { gadget_hdr h; char *text; int max_text; } display_gadget;
typedef struct {
	gadget_hdr h;
	const char *text;
	int max_text;
	const char *click_show;
	int event;
} button_gadget;

enum { G_CERT_L = 0, G_CERT, G_PW_L, G_PW, G_WHY_L, G_WHY, G_WHERE_L, G_WHERE, G_CANCEL, G_SIGN };

typedef struct {
	label_gadget cert_l;
	display_gadget cert;
	label_gadget pw_l;
	writable_gadget pw;
	label_gadget why_l;
	writable_gadget why;
	label_gadget where_l;
	writable_gadget where;
	button_gadget cancel, sign;
} sign_gadgets;

static struct {
	ObjectId box;
	sign_gadgets g;
	char cert_buf[200], pw_buf[128], why_buf[128], where_buf[128];
	char path[256];               /* the certificate file, kept for the session */
	docwin *w;
	int page;
	char field[128];              /* "" for a new field */
} sb;

#define SB_W 760
#define SB_H 376

static void hdr(gadget_hdr *h, int type, int size, int id, int x0, int y0, int x1, int y1,
		const char *help)
{
	h->flags = 0;
	h->type = (size << 16) | type;
	h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1;
	h->id = id;
	h->help = help;
	h->max_help = help ? (int)strlen(help) + 1 : 0;
}

static void row_label(label_gadget *l, int id, int row, const char *text)
{
	hdr(&l->h, 320, sizeof *l, id, 16, -84 - 64 * row, 200, -36 - 64 * row, NULL);
	l->label = text;
}

static void row_field(writable_gadget *f, int id, int row, char *buf, int size, int before, int after,
		const char *help)
{
	hdr(&f->h, 512, sizeof *f, id, 200, -88 - 64 * row, SB_W - 16, -32 - 64 * row, help);
	f->text = buf;
	f->max_text = size;
	f->before = before;
	f->after = after;
}

static ObjectId create_signbox(void)
{
	static tbx_window t;
	static char title[] = "Sign";
	sign_gadgets *g = &sb.g;
	int by = -SB_H + 16;
	ObjectId id = 0;
	_kernel_oserror *e;

	memset(g, 0, sizeof *g);
	row_label(&g->cert_l, G_CERT_L, 0, "Certificate");
	hdr(&g->cert.h, 448, sizeof g->cert, G_CERT, 200, -84, SB_W - 16, -36,
		"Drag your certificate file (.p12 or .pfx: your key and certificate) into this box.");
	g->cert.text = sb.cert_buf;
	g->cert.max_text = sizeof sb.cert_buf;
	row_label(&g->pw_l, G_PW_L, 1, "Password");
	row_field(&g->pw, G_PW, 1, sb.pw_buf, sizeof sb.pw_buf, G_WHERE, G_WHY,
		"Type the certificate file's password.");
	g->pw.h.flags = 1 << 4;         /* shows '-' for each character */
	row_label(&g->why_l, G_WHY_L, 2, "Reason");
	row_field(&g->why, G_WHY, 2, sb.why_buf, sizeof sb.why_buf, G_PW, G_WHERE,
		"Type why you're signing, such as 'I approve this document' (or leave it empty).");
	row_label(&g->where_l, G_WHERE_L, 3, "Location");
	row_field(&g->where, G_WHERE, 3, sb.where_buf, sizeof sb.where_buf, G_WHY, G_PW,
		"Type where you're signing, such as a town (or leave it empty).");
	hdr(&g->cancel.h, 128, sizeof g->cancel, G_CANCEL, SB_W - 360, by, SB_W - 200, by + 68,
		"Click to close this box without signing.");
	g->cancel.h.flags = 2;
	g->cancel.text = "Cancel";
	g->cancel.max_text = 7;
	g->cancel.event = EV_SG_CANCEL;
	hdr(&g->sign.h, 128, sizeof g->sign, G_SIGN, SB_W - 184, by, SB_W - 24, by + 68,
		"Click (or press Return) to sign. Then save the PDF: the signature is made as it's saved.");
	g->sign.h.flags = 1 | (1 << 2);   /* default; local, so a mistake can be put right */
	g->sign.text = "Sign";
	g->sign.max_text = 5;
	g->sign.event = EV_SG_SIGN;

	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);
	t.help_message = "Use this box to sign the PDF with your certificate: drag the .p12 or .pfx file here.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = 10;
	t.gadgets = g;
	t.default_focus = G_PW;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = SB_W;
	t.window.vis_y1 = SB_H;
	t.window.behind = -1;
	t.window.window_flags = 0x80000002u | (1u << 4) | (1u << 24) | (1u << 25) | (1u << 26);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 1;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -SB_H;
	t.window.work_x1 = SB_W;
	t.window.title_flags = 0x00000119;
	t.window.sprite_area = (void *)1;
	t.window.title_text = title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof title;
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	return id;
}

static void show_cert(void)
{
	const char *leaf = strrchr(sb.path, '.');
	tbx_method(sb.box, 448, G_CERT, (int)(sb.path[0] ? (leaf ? leaf + 1 : sb.path) :
		"Drag your .p12 or .pfx file here"), 0);
}

void signbox_open(docwin *w, int page, const char *field)
{
	int dst[9], pos[2];
	if (!docwin_can_sign(w))
		return;
	if (!sb.box && !(sb.box = create_signbox()))
		return;
	sb.w = w;
	sb.page = page;
	snprintf(sb.field, sizeof sb.field, "%s", field ? field : "");
	show_cert();
	tbx_method(sb.box, 512, G_PW, (int)"", 0);
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	pos[0] = dst[1] + 64;
	pos[1] = dst[4] - 64;
	tbx_show(sb.box, 0, 2, pos, 0, -1);
	app_log("sign: %s on page %d", sb.field[0] ? sb.field : "a new field", page + 1);
}

void signbox_forget(docwin *w)
{
	if (sb.w != w)
		return;
	sb.w = NULL;
	if (sb.box)
		tbx_hide(sb.box);
}

static void get_text(int id, char *out, int size)
{
	out[0] = 0;
	_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, sb.box, 513, id, out, size);
	out[size - 1] = 0;
}

static void wipe_pw(char *pw, size_t n)
{
	volatile char *p = pw;
	while (n--)
		*p++ = 0;
}

void signbox_event(int code)
{
	char pw[128], why[128], where[128], *u_pw, *u_why, *u_where;
	unsigned char *p12;
	int len = 0, rc;
	folio_signer *sg;
	if (code == EV_SG_CANCEL || !sb.w) {
		if (sb.box)
			tbx_hide(sb.box);
		return;
	}
	if (!sb.path[0]) {
		tbx_report("Drag your certificate file (.p12 or .pfx) into the Sign box first.");
		return;
	}
	p12 = read_native(sb.path, &len);
	if (!p12) {
		tbx_report("Folio couldn't read the certificate file.");
		return;
	}
	get_text(G_PW, pw, sizeof pw);
	get_text(G_WHY, why, sizeof why);
	get_text(G_WHERE, where, sizeof where);
	/* Typed in the desktop's alphabet; MuPDF wants UTF-8. */
	u_pw = app_utf8_text(pw);
	u_why = app_utf8_text(why);
	u_where = app_utf8_text(where);
	_swix(Hourglass_On, 0);
	core_now();
	sg = folio_signer_open(app.fc, p12, (size_t)len, u_pw ? u_pw : pw);
	core_unlock();
	_swix(Hourglass_Off, 0);
	wipe_pw(pw, sizeof pw);
	if (u_pw)
		wipe_pw(u_pw, strlen(u_pw));
	tbx_method(sb.box, 512, G_PW, (int)"", 0);
	wipe_pw((char *)p12, (size_t)len);
	free(p12);
	free(u_pw);
	if (!sg) {
		char m[300];
		snprintf(m, sizeof m, "Folio can't sign with that certificate: %s.", folio_error(app.fc));
		tbx_report(m);
		free(u_why);
		free(u_where);
		return;
	}
	rc = docwin_sign(sb.w, sb.page, sb.field[0] ? sb.field : NULL, sg, u_why ? u_why : why,
		u_where ? u_where : where);
	core_now();
	folio_signer_drop(app.fc, sg);
	core_unlock();
	free(u_why);
	free(u_where);
	if (rc == 0)
		tbx_hide(sb.box);
}

/* A certificate dropped in the Sign box (Message_DataLoad). 1 if ours. */
int signbox_data_load(int *b)
{
	int wimp = 0;
	if (!sb.box || !sb.w)
		return 0;
	window_get_wimp_handle(sb.box, &wimp);
	if (b[5] != wimp)
		return 0;
	if (b[10] == 0x1000 || b[10] == 0x2000) {
		tbx_report("Drag your certificate file here, not a directory.");
		return 1;
	}
	snprintf(sb.path, sizeof sb.path, "%s", (const char *)(b + 11));
	show_cert();
	app_log("sign: certificate %s", sb.path);
	return 1;
}
