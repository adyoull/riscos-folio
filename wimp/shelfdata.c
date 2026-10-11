/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The Bookshelf's data (see shelfdata.h). File format, one record a line,
 * fields separated by tabs (tabs and newlines in titles become spaces):
 *   folio-shelf 1
 *   sort <n>
 *   folder <path>
 *   book <path> <title> <author> <added> <read> <page> <pages>        */
#include "shelfdata.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void shelf_init(shelf *s)
{
	memset(s, 0, sizeof *s);
}

void shelf_free(shelf *s)
{
	int i;
	for (i = 0; i < s->n; i++)
		free(s->books[i].cover_area);
	free(s->books);
	free(s->folders);
	memset(s, 0, sizeof *s);
}

static void copy_field(char *dst, size_t size, const char *src)
{
	size_t i;
	for (i = 0; src[i] && i + 1 < size; i++)
		dst[i] = (src[i] == '\t' || src[i] == '\n' || src[i] == '\r') ? ' ' : src[i];
	dst[i] = 0;
}

static int casecmp(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
		if (d)
			return d;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int shelf_find(const shelf *s, const char *path)
{
	int i;
	for (i = 0; i < s->n; i++)
		if (!casecmp(s->books[i].path, path))
			return i;
	return -1;
}

void shelf_leaf_title(const char *path, char *out, size_t size)
{
	const char *dot = strrchr(path, '.'), *colon = strrchr(path, ':');
	const char *leaf = dot > colon ? dot + 1 : colon ? colon + 1 : path;
	const char *ext = strrchr(leaf, '/');
	size_t n = ext ? (size_t)(ext - leaf) : strlen(leaf);
	if (n >= size)
		n = size - 1;
	memcpy(out, leaf, n);
	out[n] = 0;
	/* Hard spaces in RISC OS file names are spaces in a title. */
	for (n = 0; out[n]; n++)
		if ((unsigned char)out[n] == 0xa0)
			out[n] = ' ';
}

int shelf_add(shelf *s, const char *path, const char *title, long now)
{
	shelf_book *b;
	int i = shelf_find(s, path);
	if (i >= 0)
		return i;
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 32;
		shelf_book *nb = realloc(s->books, (size_t)cap * sizeof *nb);
		if (!nb)
			return -1;
		s->books = nb;
		s->cap = cap;
	}
	b = &s->books[s->n];
	memset(b, 0, sizeof *b);
	copy_field(b->path, sizeof b->path, path);
	if (title && *title)
		copy_field(b->title, sizeof b->title, title);
	else
		shelf_leaf_title(path, b->title, sizeof b->title);
	b->added = now;
	return s->n++;
}

void shelf_remove(shelf *s, int i)
{
	if (i < 0 || i >= s->n)
		return;
	free(s->books[i].cover_area);
	memmove(&s->books[i], &s->books[i + 1], (size_t)(s->n - i - 1) * sizeof s->books[0]);
	s->n--;
}

static int by_recent(const void *a, const void *b)
{
	const shelf_book *x = a, *y = b;
	long kx = x->read ? x->read : x->added, ky = y->read ? y->read : y->added;
	if (kx != ky)
		return kx > ky ? -1 : 1;
	return casecmp(x->title, y->title);
}

static int by_title(const void *a, const void *b)
{
	const shelf_book *x = a, *y = b;
	int d = casecmp(x->title, y->title);
	return d ? d : casecmp(x->path, y->path);
}

static int by_author(const void *a, const void *b)
{
	const shelf_book *x = a, *y = b;
	int d;
	/* Books without an author go last. */
	if (!x->author[0] != !y->author[0])
		return x->author[0] ? -1 : 1;
	d = casecmp(x->author, y->author);
	return d ? d : by_title(a, b);
}

void shelf_sort(shelf *s, int sort)
{
	s->sort = sort;
	if (s->n > 1)
		qsort(s->books, (size_t)s->n, sizeof s->books[0],
			sort == SHELF_SORT_TITLE ? by_title : sort == SHELF_SORT_AUTHOR ? by_author : by_recent);
}

int shelf_find_folder(const shelf *s, const char *path)
{
	int i;
	for (i = 0; i < s->nfolders; i++)
		if (!casecmp(s->folders[i], path))
			return i;
	return -1;
}

int shelf_add_folder(shelf *s, const char *path)
{
	int i = shelf_find_folder(s, path);
	if (i >= 0)
		return i;
	if (s->nfolders == s->folder_cap) {
		int cap = s->folder_cap ? s->folder_cap * 2 : 8;
		char (*nf)[256] = realloc(s->folders, (size_t)cap * sizeof *nf);
		if (!nf)
			return -1;
		s->folders = nf;
		s->folder_cap = cap;
	}
	copy_field(s->folders[s->nfolders], sizeof s->folders[0], path);
	return s->nfolders++;
}

void shelf_remove_folder(shelf *s, int i)
{
	if (i < 0 || i >= s->nfolders)
		return;
	memmove(&s->folders[i], &s->folders[i + 1], (size_t)(s->nfolders - i - 1) * sizeof s->folders[0]);
	s->nfolders--;
}

int shelf_in_folder(const char *path, const char *folder)
{
	size_t n = strlen(folder);
	size_t i;
	for (i = 0; i < n; i++)
		if (tolower((unsigned char)path[i]) != tolower((unsigned char)folder[i]))
			return 0;
	return path[n] == 0 || path[n] == '.';
}

int shelf_is_document(const char *leaf, int filetype, int epub_type)
{
	static const char *const exts[] = { "pdf", "epub", "fb2", "mobi", "cbz", "xps", "oxps" };
	const char *slash;
	size_t i;
	if (filetype == 0xadf || (epub_type >= 0 && filetype == epub_type))
		return 1;
	slash = strrchr(leaf, '/');
	if (!slash)
		return 0;
	for (i = 0; i < sizeof exts / sizeof exts[0]; i++)
		if (!casecmp(slash + 1, exts[i]))
			return 1;
	return 0;
}

/* ---- File ---------------------------------------------------------------- */

/* Splits line at tabs into at most max fields; returns how many. */
static int split(char *line, char **f, int max)
{
	int n = 0;
	char *p = line;
	line[strcspn(line, "\r\n")] = 0;
	while (n < max) {
		f[n++] = p;
		p = strchr(p, '\t');
		if (!p)
			break;
		*p++ = 0;
	}
	return n;
}

int shelf_load(shelf *s, const char *file)
{
	FILE *fp = fopen(file, "r");
	char line[1024];
	if (!fp)
		return -1;
	if (!fgets(line, sizeof line, fp) || strncmp(line, "folio-shelf 1", 13)) {
		fclose(fp);
		return -1;
	}
	while (fgets(line, sizeof line, fp)) {
		char *f[9];
		int n = split(line, f, 9);
		if (n == 2 && !strcmp(f[0], "sort")) {
			s->sort = atoi(f[1]);
		} else if (n == 2 && !strcmp(f[0], "folder")) {
			shelf_add_folder(s, f[1]);
		} else if (n == 8 && !strcmp(f[0], "book")) {
			int i = shelf_add(s, f[1], f[2], atol(f[4]));
			if (i >= 0) {
				shelf_book *b = &s->books[i];
				copy_field(b->author, sizeof b->author, f[3]);
				b->read = atol(f[5]);
				b->page = atoi(f[6]);
				b->pages = atoi(f[7]);
			}
		}
	}
	fclose(fp);
	return 0;
}

int shelf_save(const shelf *s, const char *file)
{
	FILE *fp = fopen(file, "w");
	int i, bad;
	if (!fp)
		return -1;
	fprintf(fp, "folio-shelf 1\nsort\t%d\n", s->sort);
	for (i = 0; i < s->nfolders; i++)
		fprintf(fp, "folder\t%s\n", s->folders[i]);
	for (i = 0; i < s->n; i++) {
		const shelf_book *b = &s->books[i];
		fprintf(fp, "book\t%s\t%s\t%s\t%ld\t%ld\t%d\t%d\n", b->path, b->title, b->author,
			b->added, b->read, b->page, b->pages);
	}
	bad = ferror(fp);
	if (fclose(fp))
		bad = 1;
	return bad ? -1 : 0;
}
